#pragma once

#include <cstddef>
#include <cstdint>

// Calendar feeds (RFC 5545 iCalendar), read as they stream in: the events of a
// window of days, recurring ones expanded, in a fixed amount of RAM. No
// hardware here, so host tests run it as is.
namespace x4::net {

// Days since 1970-01-01 for a proleptic Gregorian date, and back.
int32_t daysFromCivil(int32_t year, int32_t month, int32_t day);
void civilFromDays(int32_t days, int32_t& year, int32_t& month, int32_t& day);
int weekdayOf(int32_t days);  // 0 = Sunday
int daysInMonth(int32_t year, int32_t month);

// A time zone as a fixed standard offset and, optionally, a yearly daylight
// window, as POSIX TZ strings and iCalendar VTIMEZONE blocks give them.
struct TzRule {
  struct Change {
    uint8_t month = 0;    // 1-12
    uint8_t week = 0;     // 1-4, 5 = the last
    uint8_t weekday = 0;  // 0 = Sunday
    int32_t seconds = 0;  // local time of day of the change, before it
  };
  int32_t standard = 0;  // seconds east of UTC
  int32_t daylight = 0;
  bool hasDaylight = false;
  Change toDaylight;
  Change toStandard;

  int32_t offsetAt(int64_t utc) const;
  // Local wall seconds (as if the wall clock were UTC) to UTC.
  int64_t toUtc(int64_t local) const;
  int64_t toLocal(int64_t utc) const { return utc + offsetAt(utc); }
};

// "CET-1CEST,M3.5.0,M10.5.0/3", "UTC0", "<+03>-3". False leaves `out` as UTC.
bool parsePosixTz(const char* posix, TzRule& out);

struct Event {
  int64_t start = 0;  // UTC; an all-day event starts at local midnight
  int64_t end = 0;
  bool allDay = false;
  uint8_t calendar = 0;
  char title[72] = {};
  // Which occurrence of which event this is, so a moved or cancelled one replaces it.
  uint32_t uid = 0;
  int64_t occurrence = 0;
};

// The window's events, earliest first, at most kMax of them.
struct EventList {
  static constexpr size_t kMax = 32;
  Event items[kMax];
  uint8_t count = 0;
  bool overflowed = false;

  void add(const Event& e);
  void removeOccurrence(uint8_t calendar, uint32_t uid, int64_t occurrence);
  void clear() {
    count = 0;
    overflowed = false;
  }
};

class IcalParser {
 public:
  // Keeps the events that overlap [from, to) (UTC); floating times and dates
  // are read in `local`.
  IcalParser(const TzRule& local, int64_t from, int64_t to, uint8_t calendar, EventList& out);
  void feed(const char* data, size_t len);
  // Ends the feed: the last line, then occurrences that an edited instance replaced.
  void finish();

  bool sawCalendar() const { return calendarSeen; }

 private:
  static constexpr size_t kLine = 320;
  static constexpr size_t kZones = 6;
  static constexpr size_t kExdates = 24;
  static constexpr size_t kOverrides = 24;
  static constexpr size_t kByDays = 8;

  struct DateTime {
    int32_t days = 0;     // local civil date
    int32_t seconds = 0;  // local time of day
    enum class Kind : uint8_t { None, Date, Utc, Zone, Floating } kind = Kind::None;
    uint32_t zone = 0;  // TZID hash
  };
  struct Rule {
    enum class Freq : uint8_t { None, Daily, Weekly, Monthly, Yearly } freq = Freq::None;
    uint16_t interval = 1;
    uint32_t count = 0;
    bool hasUntil = false;
    int64_t until = 0;  // UTC
    uint8_t byDayCount = 0;
    int8_t byDayOrdinal[kByDays] = {};  // 0 = every such weekday
    uint8_t byDayWeekday[kByDays] = {};
    int8_t byMonthDay = 0;
    uint8_t byMonth = 0;
    bool unsupported = false;
  };
  struct Zone {
    uint32_t id = 0;
    TzRule rule;
  };
  struct Override {
    uint32_t uid = 0;
    int64_t occurrence = 0;
  };

  const TzRule& local;
  const int64_t from;
  const int64_t to;
  const uint8_t calendar;
  EventList& out;

  char line[kLine + 1] = {};
  size_t lineLength = 0;
  bool lineEnded = false;  // waiting for the next byte: a space or tab folds it on
  bool calendarSeen = false;

  enum class Block : uint8_t { None, Event, Zone, ZoneStandard, ZoneDaylight, Other } block = Block::None;
  uint8_t otherDepth = 0;  // nested blocks we skip (VALARM)

  Zone zones[kZones];
  uint8_t zoneCount = 0;
  // The VTIMEZONE being read.
  uint32_t zoneId = 0;
  TzRule zoneRule;
  int32_t partOffset = 0;
  TzRule::Change partChange;
  bool partHasRule = false;

  // The VEVENT being read.
  char title[sizeof(Event::title)] = {};
  uint32_t uid = 0;
  DateTime start;
  DateTime end;
  int64_t duration = -1;
  Rule rule;
  uint8_t wkst = 1;  // the week starts on Monday unless the rule says otherwise
  bool cancelled = false;
  bool hasRecurrenceId = false;
  int64_t recurrenceId = 0;
  int64_t exdates[kExdates] = {};
  uint8_t exdateCount = 0;

  Override overrides[kOverrides];
  uint8_t overrideCount = 0;

  void endLine();
  void processLine(char* text);
  void beginEvent();
  void endEvent();
  void endZonePart(bool daylight);
  void endZone();

  const TzRule& zoneFor(const DateTime& t) const;
  int64_t toUtc(const DateTime& t) const;
  int64_t toUtc(int32_t days, int32_t seconds, const DateTime& like) const;
  bool excluded(int64_t utc) const;
  void emit(int64_t startUtc, int64_t endUtc, int64_t occurrence);
  void expand(int64_t length);
  bool candidate(int32_t days, int64_t length, uint32_t& counted, bool& stop);

  static bool parseDateTime(const char* params, const char* value, DateTime& out);
  static int64_t parseDuration(const char* value);
  void parseRule(const char* value);
};

}  // namespace x4::net
