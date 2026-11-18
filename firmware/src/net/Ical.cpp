#include "Ical.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace x4::net {

namespace {

constexpr int64_t kDay = 86400;
constexpr int kMaxPeriods = 3000;  // periods walked per recurring event, after skipping ahead

int64_t floorDiv(const int64_t a, const int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

uint32_t hashOf(const char* s) {
  uint32_t h = 2166136261u;
  for (; *s; ++s) {
    h ^= static_cast<uint8_t>(*s);
    h *= 16777619u;
  }
  return h;
}

char upper(const char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; }

bool same(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b) {
    if (upper(*a) != upper(*b)) return false;
  }
  return *a == *b;
}

bool startsWith(const char* s, const char* prefix) {
  for (; *prefix; ++s, ++prefix) {
    if (upper(*s) != upper(*prefix)) return false;
  }
  return true;
}

// The value of `name` in ";A=1;NAME=value;B=2", quotes stripped, into buf.
bool param(const char* params, const char* name, char* buf, const size_t size) {
  const size_t nameLength = strlen(name);
  for (const char* p = params; p && *p;) {
    if (*p == ';') ++p;
    const char* eq = strchr(p, '=');
    if (!eq) return false;
    const bool match = static_cast<size_t>(eq - p) == nameLength && startsWith(p, name);
    const char* v = eq + 1;
    const bool quoted = *v == '"';
    if (quoted) ++v;
    const char* stop = v;
    while (*stop && (quoted ? *stop != '"' : *stop != ';')) ++stop;
    if (match) {
      const size_t n = std::min(static_cast<size_t>(stop - v), size - 1);
      memcpy(buf, v, n);
      buf[n] = '\0';
      return true;
    }
    p = quoted && *stop ? stop + 1 : stop;
  }
  return false;
}

int digits(const char* s, const int n) {
  int v = 0;
  for (int i = 0; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return -1;
    v = v * 10 + (s[i] - '0');
  }
  return v;
}

int weekdayCode(const char* s) {
  static const char* const kCodes[7] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
  for (int i = 0; i < 7; ++i) {
    if (upper(s[0]) == kCodes[i][0] && upper(s[1]) == kCodes[i][1]) return i;
  }
  return -1;
}

// The `ordinal`th `weekday` of the month (negative counts from its end), or -1 past it.
int32_t ordinalWeekday(const int32_t year, const int32_t month, const int ordinal, const int weekday) {
  const int dim = daysInMonth(year, month);
  if (ordinal > 0) {
    const int32_t first = daysFromCivil(year, month, 1);
    const int32_t day = first + (weekday - weekdayOf(first) + 7) % 7 + (ordinal - 1) * 7;
    return day < first + dim ? day : -1;
  }
  const int32_t last = daysFromCivil(year, month, dim);
  const int32_t day = last - (weekdayOf(last) - weekday + 7) % 7 + (ordinal + 1) * 7;
  return day > last - dim ? day : -1;
}

int32_t changeDay(const int32_t year, const TzRule::Change& c) {
  return ordinalWeekday(year, c.month, c.week >= 5 ? -1 : c.week, c.weekday);
}

// [+|-]hh[:mm[:ss]] in seconds; advances p.
bool clockValue(const char*& p, int32_t& out) {
  int sign = 1;
  if (*p == '+' || *p == '-') sign = *p++ == '-' ? -1 : 1;
  if (*p < '0' || *p > '9') return false;
  int32_t parts[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    int32_t v = 0;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    parts[i] = v;
    if (*p != ':') break;
    ++p;
  }
  out = sign * (parts[0] * 3600 + parts[1] * 60 + parts[2]);
  return true;
}

bool zoneName(const char*& p) {
  if (*p == '<') {
    const char* close = strchr(p, '>');
    if (!close) return false;
    p = close + 1;
    return true;
  }
  const char* begin = p;
  while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) ++p;
  return p - begin >= 3;
}

bool posixChange(const char*& p, TzRule::Change& c) {
  if (*p != 'M') return false;
  ++p;
  char* end = nullptr;
  const long month = strtol(p, &end, 10);
  if (*end != '.') return false;
  const long week = strtol(end + 1, &end, 10);
  if (*end != '.') return false;
  const long weekday = strtol(end + 1, &end, 10);
  if (month < 1 || month > 12 || week < 1 || week > 5 || weekday < 0 || weekday > 6) return false;
  c.month = static_cast<uint8_t>(month);
  c.week = static_cast<uint8_t>(week);
  c.weekday = static_cast<uint8_t>(weekday);
  c.seconds = 2 * 3600;
  p = end;
  if (*p == '/') {
    ++p;
    if (!clockValue(p, c.seconds)) return false;
  }
  return true;
}

// "+0200" or "-0530" in seconds.
bool utcOffset(const char* s, int32_t& out) {
  if ((s[0] != '+' && s[0] != '-') || strlen(s) < 5) return false;
  const int h = digits(s + 1, 2);
  const int m = digits(s + 3, 2);
  if (h < 0 || m < 0) return false;
  out = (s[0] == '-' ? -1 : 1) * (h * 3600 + m * 60);
  return true;
}

// Copies whole UTF-8 sequences only, so a cut never splits a letter.
void copyUtf8(char* dst, const size_t size, const char* src) {
  size_t n = 0;
  while (*src) {
    const auto lead = static_cast<uint8_t>(*src);
    const size_t len = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 1;
    if (n + len >= size) break;
    size_t i = 0;
    while (i < len && src[i]) ++i;
    if (i < len) break;
    memcpy(dst + n, src, len);
    n += len;
    src += len;
  }
  dst[n] = '\0';
}

}  // namespace

int32_t daysFromCivil(int32_t year, const int32_t month, const int32_t day) {
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const int32_t yoe = year - era * 400;
  const int32_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void civilFromDays(int32_t days, int32_t& year, int32_t& month, int32_t& day) {
  days += 719468;
  const int32_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int32_t doe = days - era * 146097;
  const int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int32_t mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp < 10 ? mp + 3 : mp - 9;
  year = yoe + era * 400 + (month <= 2);
}

int weekdayOf(const int32_t days) { return static_cast<int>(((days % 7) + 11) % 7); }

int daysInMonth(const int32_t year, const int32_t month) {
  static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2) return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 29 : 28;
  return kDays[(month - 1) % 12];
}

int32_t TzRule::offsetAt(const int64_t utc) const {
  if (!hasDaylight) return standard;
  int32_t year, month, day;
  civilFromDays(static_cast<int32_t>(floorDiv(utc + standard, kDay)), year, month, day);
  const int64_t on = changeDay(year, toDaylight) * kDay + toDaylight.seconds - standard;
  const int64_t off = changeDay(year, toStandard) * kDay + toStandard.seconds - daylight;
  const bool inside = on < off ? utc >= on && utc < off : utc >= on || utc < off;
  return inside ? daylight : standard;
}

int64_t TzRule::toUtc(const int64_t local) const {
  const int64_t asStandard = local - standard;
  if (!hasDaylight || offsetAt(asStandard) == standard) return asStandard;
  return local - daylight;
}

bool parsePosixTz(const char* posix, TzRule& out) {
  out = TzRule{};
  if (!posix) return false;
  const char* p = posix;
  int32_t west = 0;
  if (!zoneName(p) || !clockValue(p, west)) return false;
  TzRule rule;
  rule.standard = -west;
  if (*p == '\0') {
    out = rule;
    return true;
  }
  if (!zoneName(p)) return false;
  rule.daylight = rule.standard + 3600;
  if (*p && *p != ',') {
    if (!clockValue(p, west)) return false;
    rule.daylight = -west;
  }
  if (*p != ',') {
    out = rule;  // no rule given: keep standard time all year
    return true;
  }
  ++p;
  if (!posixChange(p, rule.toDaylight) || *p != ',') {
    out = rule;
    return true;
  }
  ++p;
  if (!posixChange(p, rule.toStandard)) {
    out = rule;
    return true;
  }
  rule.hasDaylight = true;
  out = rule;
  return true;
}

void EventList::add(const Event& e) {
  for (uint8_t i = 0; i < count; ++i) {
    // One event shared into two calendars shows once. Within a calendar, an edited
    // instance and the occurrence it replaces both stay until finish() sorts them out.
    if (items[i].calendar != e.calendar && items[i].uid == e.uid && items[i].start == e.start) return;
  }
  const auto before = [](const Event& a, const Event& b) {
    if (a.start != b.start) return a.start < b.start;
    return a.allDay && !b.allDay;
  };
  size_t at = count;
  while (at > 0 && before(e, items[at - 1])) --at;
  if (count == kMax) {
    overflowed = true;
    if (at == kMax) return;
    --count;
  }
  for (size_t i = count; i > at; --i) items[i] = items[i - 1];
  items[at] = e;
  ++count;
}

void EventList::removeOccurrence(const uint8_t calendar, const uint32_t uid, const int64_t occurrence) {
  uint8_t kept = 0;
  for (uint8_t i = 0; i < count; ++i) {
    const Event& e = items[i];
    const bool replaced = e.calendar == calendar && e.uid == uid && e.occurrence == occurrence && e.occurrence >= 0;
    if (!replaced) items[kept++] = e;
  }
  count = kept;
}

IcalParser::IcalParser(const TzRule& local, const int64_t from, const int64_t to, const uint8_t calendar,
                       EventList& out)
    : local(local), from(from), to(to), calendar(calendar), out(out) {}

void IcalParser::feed(const char* data, const size_t len) {
  for (size_t i = 0; i < len; ++i) {
    const char c = data[i];
    if (lineEnded) {
      lineEnded = false;
      if (c == ' ' || c == '\t') continue;  // a folded line goes on
      endLine();
    }
    if (c == '\r') continue;
    if (c == '\n') {
      lineEnded = true;
      continue;
    }
    if (lineLength < kLine) line[lineLength++] = c;
  }
}

void IcalParser::finish() {
  if (lineEnded || lineLength > 0) endLine();
  lineEnded = false;
  for (uint8_t i = 0; i < overrideCount; ++i) out.removeOccurrence(calendar, overrides[i].uid, overrides[i].occurrence);
}

void IcalParser::endLine() {
  line[lineLength] = '\0';
  lineLength = 0;
  processLine(line);
}

void IcalParser::processLine(char* text) {
  // NAME;PARAM=a;PARAM="b:c":value
  char* nameEnd = text;
  while (*nameEnd && *nameEnd != ';' && *nameEnd != ':') ++nameEnd;
  char* colon = nameEnd;
  bool quoted = false;
  while (*colon && (quoted || *colon != ':')) {
    if (*colon == '"') quoted = !quoted;
    ++colon;
  }
  if (*colon != ':') return;
  *colon = '\0';
  const char* value = colon + 1;
  const char* params = nameEnd;  // ";PARAM=..." or ""
  const char saved = *nameEnd;
  *nameEnd = '\0';
  const char* name = text;
  // `params` begins at nameEnd, which now holds '\0'; point past it when there were any.
  if (saved == ';') params = nameEnd + 1;

  const bool begin = same(name, "BEGIN");
  const bool endTag = same(name, "END");

  switch (block) {
    case Block::None:
      if (!begin) return;
      if (same(value, "VCALENDAR")) {
        calendarSeen = true;
      } else if (same(value, "VEVENT")) {
        beginEvent();
        block = Block::Event;
      } else if (same(value, "VTIMEZONE")) {
        zoneId = 0;
        zoneRule = TzRule{};
        block = Block::Zone;
      } else {
        block = Block::Other;
        otherDepth = 1;
      }
      return;
    case Block::Other:
      if (begin) ++otherDepth;
      if (endTag && --otherDepth == 0) block = Block::None;
      return;
    case Block::Zone:
      if (begin && (same(value, "STANDARD") || same(value, "DAYLIGHT"))) {
        block = same(value, "STANDARD") ? Block::ZoneStandard : Block::ZoneDaylight;
        partOffset = 0;
        partChange = TzRule::Change{};
        partHasRule = false;
      } else if (endTag && same(value, "VTIMEZONE")) {
        endZone();
        block = Block::None;
      } else if (same(name, "TZID")) {
        zoneId = hashOf(value);
      }
      return;
    case Block::ZoneStandard:
    case Block::ZoneDaylight: {
      const bool daylight = block == Block::ZoneDaylight;
      if (endTag) {
        endZonePart(daylight);
        block = Block::Zone;
      } else if (same(name, "TZOFFSETTO")) {
        utcOffset(value, partOffset);
      } else if (same(name, "DTSTART")) {
        DateTime t;
        if (parseDateTime("", value, t)) partChange.seconds = t.seconds;
      } else if (same(name, "RRULE")) {
        // FREQ=YEARLY;BYMONTH=3;BYDAY=-1SU
        char buf[24];
        int month = 0;
        int ordinal = 0;
        int weekday = -1;
        if (param(value, "BYMONTH", buf, sizeof(buf))) month = atoi(buf);
        if (param(value, "BYDAY", buf, sizeof(buf))) {
          char* rest = nullptr;
          ordinal = static_cast<int>(strtol(buf, &rest, 10));
          weekday = weekdayCode(rest);
        }
        if (month >= 1 && month <= 12 && weekday >= 0 && ordinal != 0 && ordinal >= -1 && ordinal <= 4) {
          partChange.month = static_cast<uint8_t>(month);
          partChange.week = static_cast<uint8_t>(ordinal < 0 ? 5 : ordinal);
          partChange.weekday = static_cast<uint8_t>(weekday);
          partHasRule = true;
        }
      }
      return;
    }
    case Block::Event:
      break;
  }

  // Inside a VEVENT.
  if (otherDepth > 0) {
    if (begin) ++otherDepth;
    if (endTag) --otherDepth;
    return;
  }
  if (begin) {
    otherDepth = 1;  // VALARM
    return;
  }
  if (endTag) {
    if (same(value, "VEVENT")) {
      endEvent();
      block = Block::None;
    }
    return;
  }
  if (same(name, "UID")) {
    uid = hashOf(value);
  } else if (same(name, "SUMMARY")) {
    char plain[sizeof(title)];
    size_t n = 0;
    for (const char* p = value; *p && n + 1 < sizeof(plain); ++p) {
      char c = *p;
      if (c == '\\' && p[1]) {
        ++p;
        c = (*p == 'n' || *p == 'N') ? ' ' : *p;
      }
      plain[n++] = c;
    }
    plain[n] = '\0';
    copyUtf8(title, sizeof(title), plain);
  } else if (same(name, "DTSTART")) {
    parseDateTime(params, value, start);
  } else if (same(name, "DTEND")) {
    parseDateTime(params, value, end);
  } else if (same(name, "DURATION")) {
    duration = parseDuration(value);
  } else if (same(name, "RRULE")) {
    parseRule(value);
  } else if (same(name, "EXDATE")) {
    char one[24];
    for (const char* p = value; *p;) {
      const char* comma = strchr(p, ',');
      const size_t n = std::min(comma ? static_cast<size_t>(comma - p) : strlen(p), sizeof(one) - 1);
      memcpy(one, p, n);
      one[n] = '\0';
      DateTime t;
      if (exdateCount < kExdates && parseDateTime(params, one, t)) exdates[exdateCount++] = toUtc(t);
      if (!comma) break;
      p = comma + 1;
    }
  } else if (same(name, "RECURRENCE-ID")) {
    DateTime t;
    if (parseDateTime(params, value, t)) {
      hasRecurrenceId = true;
      recurrenceId = toUtc(t);
    }
  } else if (same(name, "STATUS")) {
    cancelled = same(value, "CANCELLED");
  }
}

void IcalParser::beginEvent() {
  title[0] = '\0';
  uid = 0;
  start = DateTime{};
  end = DateTime{};
  duration = -1;
  rule = Rule{};
  cancelled = false;
  hasRecurrenceId = false;
  recurrenceId = 0;
  exdateCount = 0;
  otherDepth = 0;
  wkst = 1;
}

void IcalParser::endZonePart(const bool daylight) {
  if (daylight) {
    zoneRule.daylight = partOffset;
    zoneRule.toDaylight = partChange;
    zoneRule.hasDaylight = partHasRule;
  } else {
    zoneRule.standard = partOffset;
    zoneRule.toStandard = partChange;
    if (!partHasRule) zoneRule.hasDaylight = false;
  }
}

void IcalParser::endZone() {
  if (zoneId == 0) return;
  if (zoneRule.hasDaylight && zoneRule.toStandard.month == 0) zoneRule.hasDaylight = false;
  for (uint8_t i = 0; i < zoneCount; ++i) {
    if (zones[i].id == zoneId) {
      zones[i].rule = zoneRule;
      return;
    }
  }
  if (zoneCount < kZones) zones[zoneCount++] = Zone{zoneId, zoneRule};
}

const TzRule& IcalParser::zoneFor(const DateTime& t) const {
  if (t.kind == DateTime::Kind::Zone) {
    for (uint8_t i = 0; i < zoneCount; ++i) {
      if (zones[i].id == t.zone) return zones[i].rule;
    }
  }
  return local;
}

int64_t IcalParser::toUtc(const int32_t days, const int32_t seconds, const DateTime& like) const {
  const int64_t wall = static_cast<int64_t>(days) * kDay + seconds;
  switch (like.kind) {
    case DateTime::Kind::Utc:
      return wall;
    case DateTime::Kind::Date:
      return local.toUtc(static_cast<int64_t>(days) * kDay);
    default:
      return zoneFor(like).toUtc(wall);
  }
}

int64_t IcalParser::toUtc(const DateTime& t) const { return toUtc(t.days, t.seconds, t); }

bool IcalParser::excluded(const int64_t utc) const {
  for (uint8_t i = 0; i < exdateCount; ++i) {
    if (exdates[i] == utc) return true;
  }
  return false;
}

void IcalParser::emit(const int64_t startUtc, const int64_t endUtc, const int64_t occurrence) {
  const bool overlaps = endUtc > startUtc ? startUtc < to && endUtc > from : startUtc >= from && startUtc < to;
  if (!overlaps) return;
  Event e;
  e.start = startUtc;
  e.end = endUtc;
  e.allDay = start.kind == DateTime::Kind::Date;
  e.calendar = calendar;
  e.uid = uid;
  e.occurrence = occurrence;
  memcpy(e.title, title, sizeof(e.title));
  out.add(e);
}

void IcalParser::endEvent() {
  if (start.kind == DateTime::Kind::None) return;
  const int64_t startUtc = toUtc(start);
  int64_t length = 0;
  if (end.kind != DateTime::Kind::None) {
    length = std::max<int64_t>(0, toUtc(end) - startUtc);
  } else if (duration >= 0) {
    length = duration;
  } else if (start.kind == DateTime::Kind::Date) {
    length = kDay;
  }

  if (hasRecurrenceId) {
    // An edited (or cancelled) instance replaces its occurrence of the series.
    if (overrideCount < kOverrides) overrides[overrideCount++] = Override{uid, recurrenceId};
    // -1 marks it as the replacement, which finish() keeps.
    if (!cancelled) emit(startUtc, startUtc + length, -1);
    return;
  }
  if (cancelled) return;
  if (rule.freq == Rule::Freq::None || rule.unsupported) {
    emit(startUtc, startUtc + length, startUtc);
    return;
  }
  expand(length);
}

bool IcalParser::candidate(const int32_t days, const int64_t length, uint32_t& counted, bool& stop) {
  const int64_t s = toUtc(days, start.seconds, start);
  if ((rule.hasUntil && s > rule.until) || (rule.count && counted >= rule.count) || s >= to) {
    stop = true;
    return false;
  }
  ++counted;
  if (!excluded(s)) emit(s, s + length, s);
  return true;
}

void IcalParser::expand(const int64_t length) {
  const int32_t d0 = start.days;
  // Days to walk: a little either side of the window, in any zone.
  const auto firstDay = static_cast<int32_t>(floorDiv(from - length, kDay) - 2);
  const auto lastDay = static_cast<int32_t>(floorDiv(to, kDay) + 2);
  const int32_t interval = std::max<int32_t>(1, rule.interval);
  const bool skipAhead = rule.count == 0;  // COUNT counts from the first occurrence
  uint32_t counted = 0;
  bool stop = false;

  uint8_t weekdays = 0;  // bit per weekday, 0 = Sunday
  bool ordinals = false;
  for (uint8_t i = 0; i < rule.byDayCount; ++i) {
    weekdays |= static_cast<uint8_t>(1u << rule.byDayWeekday[i]);
    ordinals = ordinals || rule.byDayOrdinal[i] != 0;
  }
  int32_t y0, m0, dd0;
  civilFromDays(d0, y0, m0, dd0);

  switch (rule.freq) {
    case Rule::Freq::Daily: {
      if (ordinals) break;
      int64_t k = skipAhead && firstDay > d0 ? (firstDay - d0) / interval : 0;
      for (int n = 0; n < kMaxPeriods && !stop; ++n, ++k) {
        const auto day = static_cast<int32_t>(d0 + k * interval);
        if (day > lastDay) break;
        if (weekdays && !(weekdays & (1u << weekdayOf(day)))) continue;
        candidate(day, length, counted, stop);
      }
      return;
    }
    case Rule::Freq::Weekly: {
      if (ordinals) break;
      if (!weekdays) weekdays = static_cast<uint8_t>(1u << weekdayOf(d0));
      const int32_t weekStart = d0 - (weekdayOf(d0) - wkst + 7) % 7;
      int64_t k = skipAhead && firstDay > weekStart ? (firstDay - weekStart) / (7 * interval) : 0;
      for (int n = 0; n < kMaxPeriods && !stop; ++n, ++k) {
        const auto ws = static_cast<int32_t>(weekStart + k * 7 * interval);
        if (ws > lastDay) break;
        for (int i = 0; i < 7 && !stop; ++i) {
          const int32_t day = ws + i;
          if (day < d0 || !(weekdays & (1u << weekdayOf(day)))) continue;
          candidate(day, length, counted, stop);
        }
      }
      return;
    }
    case Rule::Freq::Monthly:
    case Rule::Freq::Yearly: {
      const bool monthly = rule.freq == Rule::Freq::Monthly;
      const int32_t month0 = y0 * 12 + (m0 - 1);
      int32_t fy, fm, fd;
      civilFromDays(firstDay, fy, fm, fd);
      int64_t k = 0;
      if (skipAhead) {
        const int32_t gap = monthly ? fy * 12 + (fm - 1) - month0 : fy - y0;
        if (gap > 0) k = gap / interval;
      }
      for (int n = 0; n < kMaxPeriods && !stop; ++n, ++k) {
        int32_t year, month;
        if (monthly) {
          const auto index = static_cast<int32_t>(month0 + k * interval);
          year = index / 12;
          month = index % 12 + 1;
        } else {
          year = static_cast<int32_t>(y0 + k * interval);
          month = rule.byMonth ? rule.byMonth : m0;
        }
        if (daysFromCivil(year, month, 1) > lastDay) break;
        int32_t days[31];
        int found = 0;
        const int dim = daysInMonth(year, month);
        if (rule.byDayCount) {
          for (uint8_t i = 0; i < rule.byDayCount; ++i) {
            if (rule.byDayOrdinal[i] != 0) {
              const int32_t day = ordinalWeekday(year, month, rule.byDayOrdinal[i], rule.byDayWeekday[i]);
              if (day >= 0 && found < 31) days[found++] = day;
            } else {
              for (int32_t day = ordinalWeekday(year, month, 1, rule.byDayWeekday[i]);
                   day >= 0 && day < daysFromCivil(year, month, 1) + dim && found < 31; day += 7) {
                days[found++] = day;
              }
            }
          }
        } else {
          const int dom = rule.byMonthDay > 0 ? rule.byMonthDay : rule.byMonthDay < 0 ? dim + rule.byMonthDay + 1 : dd0;
          if (dom >= 1 && dom <= dim) days[found++] = daysFromCivil(year, month, dom);
        }
        std::sort(days, days + found);
        for (int i = 0; i < found && !stop; ++i) {
          if (days[i] >= d0) candidate(days[i], length, counted, stop);
        }
      }
      return;
    }
    case Rule::Freq::None:
      break;
  }
  // A rule this parser does not walk: its first occurrence only.
  emit(toUtc(start), toUtc(start) + length, toUtc(start));
}

bool IcalParser::parseDateTime(const char* params, const char* value, DateTime& out) {
  const int year = digits(value, 4);
  const int month = digits(value + 4, 2);
  const int day = digits(value + 6, 2);
  if (year < 0 || month < 1 || month > 12 || day < 1 || day > 31) return false;
  DateTime t;
  t.days = daysFromCivil(year, month, day);
  char buf[64];
  const bool dateOnly = (param(params, "VALUE", buf, sizeof(buf)) && same(buf, "DATE")) || value[8] != 'T';
  if (dateOnly) {
    t.kind = DateTime::Kind::Date;
    out = t;
    return true;
  }
  const int h = digits(value + 9, 2);
  const int m = digits(value + 11, 2);
  const int s = digits(value + 13, 2);
  if (h < 0 || m < 0 || s < 0) return false;
  t.seconds = h * 3600 + m * 60 + s;
  if (value[15] == 'Z' || value[15] == 'z') {
    t.kind = DateTime::Kind::Utc;
  } else if (param(params, "TZID", buf, sizeof(buf))) {
    t.kind = DateTime::Kind::Zone;
    t.zone = hashOf(buf);
  } else {
    t.kind = DateTime::Kind::Floating;
  }
  out = t;
  return true;
}

int64_t IcalParser::parseDuration(const char* value) {
  const char* p = value;
  const int64_t sign = *p == '-' ? -1 : 1;
  if (*p == '+' || *p == '-') ++p;
  if (*p++ != 'P') return -1;
  int64_t total = 0;
  bool time = false;
  while (*p) {
    if (*p == 'T') {
      time = true;
      ++p;
      continue;
    }
    char* end = nullptr;
    const long n = strtol(p, &end, 10);
    if (end == p) return -1;
    switch (*end) {
      case 'W': total += n * 7 * kDay; break;
      case 'D': total += n * kDay; break;
      case 'H': total += n * 3600; break;
      case 'M': total += time ? n * 60 : 0; break;
      case 'S': total += n; break;
      default: return -1;
    }
    p = end + 1;
  }
  return sign * total;
}

void IcalParser::parseRule(const char* value) {
  rule = Rule{};
  char buf[96];
  if (!param(value, "FREQ", buf, sizeof(buf))) {
    rule.unsupported = true;
    return;
  }
  if (same(buf, "DAILY")) rule.freq = Rule::Freq::Daily;
  else if (same(buf, "WEEKLY")) rule.freq = Rule::Freq::Weekly;
  else if (same(buf, "MONTHLY")) rule.freq = Rule::Freq::Monthly;
  else if (same(buf, "YEARLY")) rule.freq = Rule::Freq::Yearly;
  else rule.unsupported = true;

  if (param(value, "INTERVAL", buf, sizeof(buf))) rule.interval = static_cast<uint16_t>(std::max(1, atoi(buf)));
  if (param(value, "COUNT", buf, sizeof(buf))) rule.count = static_cast<uint32_t>(std::max(0, atoi(buf)));
  if (param(value, "UNTIL", buf, sizeof(buf))) {
    DateTime t;
    if (parseDateTime("", buf, t)) {
      rule.hasUntil = true;
      // A date runs to its end; a floating time is local.
      rule.until = t.kind == DateTime::Kind::Date ? local.toUtc(static_cast<int64_t>(t.days) * kDay + kDay - 1)
                   : t.kind == DateTime::Kind::Utc ? static_cast<int64_t>(t.days) * kDay + t.seconds
                                                   : local.toUtc(static_cast<int64_t>(t.days) * kDay + t.seconds);
    }
  }
  if (param(value, "WKST", buf, sizeof(buf))) {
    const int w = weekdayCode(buf);
    if (w >= 0) wkst = static_cast<uint8_t>(w);
  }
  if (param(value, "BYDAY", buf, sizeof(buf))) {
    for (const char* p = buf; *p;) {
      char* end = nullptr;
      const long ordinal = strtol(p, &end, 10);
      const int weekday = weekdayCode(end);
      if (weekday < 0 || ordinal < -5 || ordinal > 5 || rule.byDayCount >= kByDays) {
        rule.unsupported = true;
        break;
      }
      rule.byDayOrdinal[rule.byDayCount] = static_cast<int8_t>(ordinal);
      rule.byDayWeekday[rule.byDayCount] = static_cast<uint8_t>(weekday);
      ++rule.byDayCount;
      p = end + 2;
      if (*p == ',') ++p;
    }
  }
  if (param(value, "BYMONTHDAY", buf, sizeof(buf))) {
    if (strchr(buf, ',')) rule.unsupported = true;
    rule.byMonthDay = static_cast<int8_t>(atoi(buf));
  }
  if (param(value, "BYMONTH", buf, sizeof(buf))) {
    if (strchr(buf, ',')) rule.unsupported = true;
    rule.byMonth = static_cast<uint8_t>(atoi(buf));
    if (rule.byMonth > 12) rule.unsupported = true;
    if (rule.freq != Rule::Freq::Yearly && rule.byMonth) rule.unsupported = true;
  }
  static const char* const kUnsupported[] = {"BYSETPOS", "BYYEARDAY", "BYWEEKNO", "BYHOUR", "BYMINUTE", "BYSECOND"};
  for (const char* key : kUnsupported) {
    if (param(value, key, buf, sizeof(buf))) rule.unsupported = true;
  }
}

}  // namespace x4::net
