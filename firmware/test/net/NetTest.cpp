#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "net/Agenda.h"
#include "net/Ical.h"
#include "net/Football.h"
#include "net/Issue.h"
#include "net/Upload.h"

using namespace x4::net;

namespace {

constexpr char kBudapest[] = "CET-1CEST,M3.5.0,M10.5.0/3";
// Saturday 2026-11-07 00:00 to Thursday 2026-11-12 00:00, Budapest.
constexpr int64_t kFrom = 1794006000;
constexpr int64_t kTo = 1794438000;

// As Google Calendar exports a calendar: zones first, then events, CRLF and folded lines.
const char* const kFeed =
    "BEGIN:VCALENDAR\r\n"
    "PRODID:-//Google Inc//Google Calendar 70.9054//EN\r\n"
    "VERSION:2.0\r\n"
    "X-WR-TIMEZONE:Europe/Budapest\r\n"
    "BEGIN:VTIMEZONE\r\n"
    "TZID:Europe/Budapest\r\n"
    "X-LIC-LOCATION:Europe/Budapest\r\n"
    "BEGIN:DAYLIGHT\r\n"
    "TZOFFSETFROM:+0100\r\n"
    "TZOFFSETTO:+0200\r\n"
    "TZNAME:GMT+2\r\n"
    "DTSTART:19700329T020000\r\n"
    "RRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=-1SU\r\n"
    "END:DAYLIGHT\r\n"
    "BEGIN:STANDARD\r\n"
    "TZOFFSETFROM:+0200\r\n"
    "TZOFFSETTO:+0100\r\n"
    "TZNAME:GMT+1\r\n"
    "DTSTART:19701025T030000\r\n"
    "RRULE:FREQ=YEARLY;BYMONTH=10;BYDAY=-1SU\r\n"
    "END:STANDARD\r\n"
    "END:VTIMEZONE\r\n"
    "BEGIN:VTIMEZONE\r\n"
    "TZID:America/New_York\r\n"
    "BEGIN:DAYLIGHT\r\n"
    "TZOFFSETFROM:-0500\r\n"
    "TZOFFSETTO:-0400\r\n"
    "DTSTART:19700308T020000\r\n"
    "RRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=2SU\r\n"
    "END:DAYLIGHT\r\n"
    "BEGIN:STANDARD\r\n"
    "TZOFFSETFROM:-0400\r\n"
    "TZOFFSETTO:-0500\r\n"
    "DTSTART:19701101T020000\r\n"
    "RRULE:FREQ=YEARLY;BYMONTH=11;BYDAY=1SU\r\n"
    "END:STANDARD\r\n"
    "END:VTIMEZONE\r\n"
    // A: timed, folded, escaped, with an alarm inside.
    "BEGIN:VEVENT\r\n"
    "DTSTART;TZID=Europe/Budapest:20261107T100000\r\n"
    "DTEND;TZID=Europe/Budapest:20261107T110000\r\n"
    "UID:a@google.com\r\n"
    "DESCRIPTION:A long description that Google folds onto a second line becaus\r\n"
    " e it runs past seventy-five octets.\r\n"
    "SUMMARY:Coffee\\, with An\r\n"
    " na\r\n"
    "BEGIN:VALARM\r\n"
    "ACTION:DISPLAY\r\n"
    "TRIGGER:-P0DT0H10M0S\r\n"
    "SUMMARY:Not the title\r\n"
    "END:VALARM\r\n"
    "END:VEVENT\r\n"
    // B: all day.
    "BEGIN:VEVENT\r\n"
    "DTSTART;VALUE=DATE:20261107\r\n"
    "DTEND;VALUE=DATE:20261108\r\n"
    "UID:b@google.com\r\n"
    "SUMMARY:Market day\r\n"
    "END:VEVENT\r\n"
    // C: weekly since January 2025; Monday the 9th excluded, Wednesday the 11th moved below.
    "BEGIN:VEVENT\r\n"
    "DTSTART;TZID=Europe/Budapest:20250106T093000\r\n"
    "DTEND;TZID=Europe/Budapest:20250106T094500\r\n"
    "RRULE:FREQ=WEEKLY;BYDAY=MO,WE,FR\r\n"
    "EXDATE;TZID=Europe/Budapest:20261109T093000\r\n"
    "UID:c@google.com\r\n"
    "SUMMARY:Standup\r\n"
    "END:VEVENT\r\n"
    // D: every other Monday from July (summer time); still 08:30 local in November.
    "BEGIN:VEVENT\r\n"
    "DTSTART;TZID=Europe/Budapest:20260706T083000\r\n"
    "DTEND;TZID=Europe/Budapest:20260706T090000\r\n"
    "RRULE:FREQ=WEEKLY;WKST=SU;INTERVAL=2;BYDAY=MO\r\n"
    "UID:d@google.com\r\n"
    "SUMMARY:One to one\r\n"
    "END:VEVENT\r\n"
    // E: New York time.
    "BEGIN:VEVENT\r\n"
    "DTSTART;TZID=America/New_York:20261109T090000\r\n"
    "DTEND;TZID=America/New_York:20261109T100000\r\n"
    "UID:e@google.com\r\n"
    "SUMMARY:Call with New York\r\n"
    "END:VEVENT\r\n"
    // F: UTC.
    "BEGIN:VEVENT\r\n"
    "DTSTART:20261108T170000Z\r\n"
    "DTEND:20261108T180000Z\r\n"
    "UID:f@google.com\r\n"
    "SUMMARY:Concert\r\n"
    "END:VEVENT\r\n"
    // G: cancelled.
    "BEGIN:VEVENT\r\n"
    "DTSTART:20261108T090000Z\r\n"
    "DTEND:20261108T100000Z\r\n"
    "UID:g@google.com\r\n"
    "STATUS:CANCELLED\r\n"
    "SUMMARY:Cancelled\r\n"
    "END:VEVENT\r\n"
    // H: five days in 2025.
    "BEGIN:VEVENT\r\n"
    "DTSTART;TZID=Europe/Budapest:20250101T080000\r\n"
    "DTEND;TZID=Europe/Budapest:20250101T083000\r\n"
    "RRULE:FREQ=DAILY;COUNT=5\r\n"
    "UID:h@google.com\r\n"
    "SUMMARY:Finished series\r\n"
    "END:VEVENT\r\n"
    // I: the first Saturday of every month.
    "BEGIN:VEVENT\r\n"
    "DTSTART;TZID=Europe/Budapest:20260103T180000\r\n"
    "DTEND;TZID=Europe/Budapest:20260103T200000\r\n"
    "RRULE:FREQ=MONTHLY;BYDAY=1SA\r\n"
    "UID:i@google.com\r\n"
    "SUMMARY:Book club\r\n"
    "END:VEVENT\r\n"
    // J: a birthday.
    "BEGIN:VEVENT\r\n"
    "DTSTART;VALUE=DATE:19901110\r\n"
    "DTEND;VALUE=DATE:19901111\r\n"
    "RRULE:FREQ=YEARLY\r\n"
    "UID:j@google.com\r\n"
    "SUMMARY:Birthday\r\n"
    "END:VEVENT\r\n"
    // K: ended before the window.
    "BEGIN:VEVENT\r\n"
    "DTSTART;TZID=Europe/Budapest:20261001T120000\r\n"
    "DTEND;TZID=Europe/Budapest:20261001T130000\r\n"
    "RRULE:FREQ=DAILY;UNTIL=20261101T000000Z\r\n"
    "UID:k@google.com\r\n"
    "SUMMARY:Over\r\n"
    "END:VEVENT\r\n"
    // L: every third day since 2015.
    "BEGIN:VEVENT\r\n"
    "DTSTART;TZID=Europe/Budapest:20150101T070000\r\n"
    "DURATION:PT30M\r\n"
    "RRULE:FREQ=DAILY;INTERVAL=3\r\n"
    "UID:l@google.com\r\n"
    "SUMMARY:Run\r\n"
    "END:VEVENT\r\n"
    // C's Wednesday, moved to noon. It comes after the series, as Google writes it.
    "BEGIN:VEVENT\r\n"
    "DTSTART;TZID=Europe/Budapest:20261111T120000\r\n"
    "DTEND;TZID=Europe/Budapest:20261111T121500\r\n"
    "RECURRENCE-ID;TZID=Europe/Budapest:20261111T093000\r\n"
    "UID:c@google.com\r\n"
    "SUMMARY:Standup (moved)\r\n"
    "END:VEVENT\r\n"
    "END:VCALENDAR\r\n";

TzRule budapest() {
  TzRule r;
  EXPECT_TRUE(parsePosixTz(kBudapest, r));
  return r;
}

const Event* find(const EventList& list, const char* title) {
  for (uint8_t i = 0; i < list.count; ++i) {
    if (strcmp(list.items[i].title, title) == 0) return &list.items[i];
  }
  return nullptr;
}

int countTitled(const EventList& list, const char* title) {
  int n = 0;
  for (uint8_t i = 0; i < list.count; ++i) n += strcmp(list.items[i].title, title) == 0;
  return n;
}

EventList parse(const char* feed, const size_t chunk) {
  const TzRule zone = budapest();
  EventList list;
  IcalParser parser(zone, kFrom, kTo, 0, list);
  const size_t length = strlen(feed);
  for (size_t at = 0; at < length; at += chunk) parser.feed(feed + at, std::min(chunk, length - at));
  parser.finish();
  EXPECT_TRUE(parser.sawCalendar());
  return list;
}

}  // namespace

TEST(Civil, DaysAndWeekdays) {
  EXPECT_EQ(daysFromCivil(1970, 1, 1), 0);
  EXPECT_EQ(weekdayOf(0), 4);  // Thursday
  EXPECT_EQ(daysFromCivil(2026, 11, 7), 20764);
  EXPECT_EQ(weekdayOf(20764), 6);  // Saturday
  int32_t y, m, d;
  civilFromDays(20764, y, m, d);
  EXPECT_EQ(y, 2026);
  EXPECT_EQ(m, 11);
  EXPECT_EQ(d, 7);
  EXPECT_EQ(daysInMonth(2028, 2), 29);
  EXPECT_EQ(daysInMonth(2100, 2), 28);
}

TEST(Tz, PosixBudapest) {
  const TzRule r = budapest();
  EXPECT_TRUE(r.hasDaylight);
  EXPECT_EQ(r.standard, 3600);
  EXPECT_EQ(r.daylight, 7200);
  // Summer time starts 2026-03-29 01:00 UTC and ends 2026-10-25 01:00 UTC.
  EXPECT_EQ(r.offsetAt(1774746000 - 1), 3600);
  EXPECT_EQ(r.offsetAt(1774746000), 7200);
  EXPECT_EQ(r.offsetAt(1792890000 - 1), 7200);
  EXPECT_EQ(r.offsetAt(1792890000), 3600);
  EXPECT_EQ(r.toUtc(1794042000 + 3600), 1794042000);
}

TEST(Tz, PosixPlain) {
  TzRule r;
  EXPECT_TRUE(parsePosixTz("UTC0", r));
  EXPECT_FALSE(r.hasDaylight);
  EXPECT_EQ(r.standard, 0);
  EXPECT_TRUE(parsePosixTz("<+0530>-5:30", r));
  EXPECT_EQ(r.standard, 5 * 3600 + 1800);
  EXPECT_TRUE(parsePosixTz("EST5EDT,M3.2.0,M11.1.0", r));
  EXPECT_EQ(r.standard, -5 * 3600);
  EXPECT_EQ(r.daylight, -4 * 3600);
  EXPECT_FALSE(parsePosixTz("", r));
}

TEST(Ical, GoogleFeed) {
  for (const size_t chunk : {strlen(kFeed), static_cast<size_t>(7), static_cast<size_t>(1)}) {
    SCOPED_TRACE(chunk);
    const EventList list = parse(kFeed, chunk);

    const Event* a = find(list, "Coffee, with Anna");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->start, 1794042000);
    EXPECT_EQ(a->end, 1794042000 + 3600);
    EXPECT_FALSE(a->allDay);

    const Event* b = find(list, "Market day");
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(b->allDay);
    EXPECT_EQ(b->start, 1794006000);
    EXPECT_EQ(b->end, 1794092400);

    // Standup: Monday excluded, Wednesday moved; Friday the 13th is past the window.
    EXPECT_EQ(countTitled(list, "Standup"), 0);
    const Event* moved = find(list, "Standup (moved)");
    ASSERT_NE(moved, nullptr);
    EXPECT_EQ(moved->start, 1794394800);

    const Event* d = find(list, "One to one");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->start, 1794209400);
    EXPECT_EQ(countTitled(list, "One to one"), 1);

    const Event* e = find(list, "Call with New York");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->start, 1794232800);

    const Event* f = find(list, "Concert");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->start, 1794157200);

    EXPECT_EQ(find(list, "Cancelled"), nullptr);
    EXPECT_EQ(find(list, "Finished series"), nullptr);
    EXPECT_EQ(find(list, "Over"), nullptr);
    EXPECT_EQ(find(list, "Not the title"), nullptr);

    const Event* i = find(list, "Book club");
    ASSERT_NE(i, nullptr);
    EXPECT_EQ(i->start, 1794070800);

    const Event* j = find(list, "Birthday");
    ASSERT_NE(j, nullptr);
    EXPECT_TRUE(j->allDay);
    EXPECT_EQ(j->start, 1794265200);

    EXPECT_EQ(countTitled(list, "Run"), 2);
    ASSERT_NE(find(list, "Run"), nullptr);
    EXPECT_EQ(find(list, "Run")->start, 1794117600);

    // Earliest first.
    for (uint8_t k = 1; k < list.count; ++k) EXPECT_LE(list.items[k - 1].start, list.items[k].start);
    EXPECT_EQ(list.count, 10);
  }
}

TEST(Ical, OverflowKeepsTheEarliest) {
  std::string feed = "BEGIN:VCALENDAR\r\n";
  for (int i = 0; i < 40; ++i) {
    char ev[256];
    snprintf(ev, sizeof(ev),
             "BEGIN:VEVENT\r\nDTSTART:20261108T%02d%02d00Z\r\nDTEND:20261108T%02d%02d00Z\r\nUID:%d\r\nSUMMARY:E%d\r\n"
             "END:VEVENT\r\n",
             20 - i / 2, (i % 2) * 30, 20 - i / 2, (i % 2) * 30 + 10, i, i);
    feed += ev;
  }
  feed += "END:VCALENDAR\r\n";
  const EventList list = parse(feed.c_str(), 64);
  EXPECT_EQ(list.count, EventList::kMax);
  EXPECT_TRUE(list.overflowed);
  EXPECT_EQ(list.items[0].start, 1794099600);  // 01:00 UTC on the 8th is the earliest
  EXPECT_NE(find(list, "E39"), nullptr);
  EXPECT_EQ(find(list, "E0"), nullptr);
}

TEST(Ical, NotACalendar) {
  const TzRule zone = budapest();
  EventList list;
  IcalParser parser(zone, kFrom, kTo, 0, list);
  const char html[] = "<html><body>Sign in</body></html>";
  parser.feed(html, sizeof(html) - 1);
  parser.finish();
  EXPECT_FALSE(parser.sawCalendar());
  EXPECT_EQ(list.count, 0);
}

TEST(Weather, OpenMeteo) {
  const char json[] =
      R"({"latitude":47.5,"longitude":19.04,"timezone":"Europe/Budapest","daily_units":{"time":"iso8601"},)"
      R"("daily":{"time":["2026-11-07","2026-11-08","2026-11-09"],"weather_code":[3,61,0],)"
      R"("temperature_2m_max":[12.4,9.6,-0.4],"temperature_2m_min":[4.1,3.5,-6.6]}})";
  Weather w;
  ASSERT_TRUE(parseOpenMeteo(json, sizeof(json) - 1, w));
  ASSERT_EQ(w.count, 3);
  EXPECT_EQ(w.days[0].day, 20764);
  EXPECT_EQ(w.days[0].code, 3);
  EXPECT_EQ(w.days[0].high, 12);
  EXPECT_EQ(w.days[0].low, 4);
  EXPECT_EQ(w.days[2].high, 0);
  EXPECT_EQ(w.days[2].low, -7);
  EXPECT_STREQ(conditionName(61), "Light rain");
  char t[24];
  formatTemperatures(w.days[2], t, sizeof(t));
  EXPECT_STREQ(t, "0\xC2\xB0 / -7\xC2\xB0");
  EXPECT_FALSE(parseOpenMeteo("{\"error\":true}", 14, w));
}

TEST(Weather, TheDayAheadInThreeHourSteps) {
  const char json[] =
      R"({"hourly":{"time":["2026-11-07T22:00","2026-11-08T01:00","2026-11-08T04:00"],)"
      R"("temperature_2m":[6.4,-0.6,4.0],"precipitation_probability":[40,null,0],"weather_code":[61,71,0]},)"
      R"("daily":{"time":["2026-11-07"],"weather_code":[61],"temperature_2m_max":[9.0],"temperature_2m_min":[4.0]}})";
  Agenda agenda;
  agenda.hasWeather = true;
  ASSERT_TRUE(parseOpenMeteo(json, sizeof(json) - 1, agenda.weather));
  ASSERT_EQ(agenda.weather.hourCount, 3);
  EXPECT_EQ(agenda.weather.hours[0].day, 20764);
  EXPECT_EQ(agenda.weather.hours[0].hour, 22);
  EXPECT_EQ(agenda.weather.hours[0].temp, 6);
  EXPECT_EQ(agenda.weather.hours[0].rain, 40);
  EXPECT_EQ(agenda.weather.hours[0].code, 61);
  EXPECT_EQ(agenda.weather.hours[1].day, 20765);
  EXPECT_EQ(agenda.weather.hours[1].hour, 1);
  EXPECT_EQ(agenda.weather.hours[1].temp, -1);
  EXPECT_EQ(agenda.weather.hours[1].rain, 0);  // no chance given

  std::string stored;
  ASSERT_TRUE(agendaToJson(agenda, stored));
  Agenda back;
  ASSERT_TRUE(agendaFromJson(stored.data(), stored.size(), back));
  ASSERT_EQ(back.weather.hourCount, 3);
  EXPECT_EQ(back.weather.hours[1].code, 71);
  EXPECT_EQ(back.weather.hours[2].hour, 4);

  // An hour stands until the next one starts; hours over are left out.
  const WeatherHour* hours[8];
  EXPECT_EQ(hoursFrom(back, 20764, 22, hours, 8), 3);
  ASSERT_EQ(hoursFrom(back, 20765, 0, hours, 8), 3);
  ASSERT_EQ(hoursFrom(back, 20765, 1, hours, 8), 2);
  EXPECT_EQ(hours[0]->hour, 1);
  EXPECT_EQ(hoursFrom(back, 20765, 7, hours, 8), 0);
  EXPECT_EQ(hoursFrom(back, 20765, 1, hours, 1), 1);
}

TEST(Agenda, RoundTripAndDays) {
  const TzRule zone = budapest();
  Agenda agenda;
  agenda.fetched = 1794040000;
  agenda.hasEvents = true;
  agenda.events = parse(kFeed, 512);
  agenda.hasWeather = true;
  agenda.weather.count = 1;
  agenda.weather.days[0] = WeatherDay{20764, 3, 12, 4};

  std::string json;
  ASSERT_TRUE(agendaToJson(agenda, json));
  Agenda back;
  ASSERT_TRUE(agendaFromJson(json.data(), json.size(), back));
  EXPECT_EQ(back.fetched, agenda.fetched);
  EXPECT_TRUE(back.hasEvents);
  EXPECT_EQ(back.events.count, agenda.events.count);
  EXPECT_STREQ(back.events.items[0].title, agenda.events.items[0].title);
  ASSERT_NE(weatherOfDay(back, 20764), nullptr);
  EXPECT_EQ(weatherOfDay(back, 20764)->high, 12);
  EXPECT_EQ(weatherOfDay(back, 20765), nullptr);

  // Saturday: Market day (all day) first, then Coffee at 10, Book club at 18.
  const Event* today[4];
  ASSERT_EQ(localDay(1794042000, zone), 20764);
  uint8_t n = eventsOfDay(back, zone, 20764, 0, today, 4);
  ASSERT_EQ(n, 3);
  EXPECT_STREQ(today[0]->title, "Market day");
  EXPECT_STREQ(today[1]->title, "Coffee, with Anna");
  EXPECT_STREQ(today[2]->title, "Book club");
  // At 11:30 the coffee is over.
  n = eventsOfDay(back, zone, 20764, 1794042000 + 9000, today, 4);
  ASSERT_EQ(n, 2);
  EXPECT_STREQ(today[1]->title, "Book club");

  char clock[8];
  formatClock(today[1]->start, zone, clock, sizeof(clock));
  EXPECT_STREQ(clock, "18:00");
}

TEST(Issue, ParsesAndMarksWhatChanged) {
  const std::string manifest =
      R"({"issue":"2026-11-09T06:41:12","files":[)"
      R"({"path":"/x4/stream.jsonl","size":4210,"crc32":305419896},)"
      R"({"path":"/x4/football.json","size":900,"crc32":7},)"
      R"({"path":"/books/read/r-2026-11-09-1.epub","size":48211,"crc32":1203948123}],"prune":["/books/read"]})";
  Issue issue;
  ASSERT_EQ(parseIssue(manifest.data(), manifest.size(), issue), nullptr);
  EXPECT_STREQ(issue.id, "2026-11-09T06:41:12");
  EXPECT_EQ(issue.count, 3);
  EXPECT_TRUE(issue.pruneRead);
  EXPECT_EQ(issue.files[2].crc, 1203948123u);

  const std::string before = R"({"issue":"2026-11-08T17:30:00","files":{"/x4/stream.jsonl":1,"/x4/football.json":7}})";
  Taken taken;
  ASSERT_TRUE(takenFromJson(before.data(), before.size(), taken));
  EXPECT_EQ(markChanged(issue, taken), 2);
  EXPECT_TRUE(issue.files[0].take);
  EXPECT_FALSE(issue.files[1].take);
  EXPECT_TRUE(issue.files[2].take);

  EXPECT_TRUE(issueLists(issue, READ_DIR, "r-2026-11-09-1.epub"));
  EXPECT_FALSE(issueLists(issue, READ_DIR, "r-2026-11-08-3.epub"));

  std::string after;
  ASSERT_TRUE(takenToJson(issue, after));
  Taken again;
  ASSERT_TRUE(takenFromJson(after.data(), after.size(), again));
  EXPECT_STREQ(again.id, issue.id);
  EXPECT_EQ(markChanged(issue, again), 0);

  std::string request;
  ASSERT_TRUE(takenRequest(issue, request));
  EXPECT_NE(request.find("\"/books/read/r-2026-11-09-1.epub\""), std::string::npos);
}

TEST(Issue, RefusesWhatBreaksTheRules) {
  const auto refused = [](const std::string& json) {
    Issue issue;
    return parseIssue(json.data(), json.size(), issue) != nullptr;
  };
  EXPECT_TRUE(refused(R"({"files":[]})"));
  EXPECT_TRUE(refused(R"({"issue":"a","files":[{"path":"/x4/state/learn.bin","size":1,"crc32":1}]})"));
  EXPECT_TRUE(refused(R"({"issue":"a","files":[{"path":"/books/read/../x4/net.json","size":1,"crc32":1}]})"));
  EXPECT_TRUE(refused(R"({"issue":"a","files":[{"path":"/books/read/a.epub","size":5000000,"crc32":1}]})"));
  EXPECT_TRUE(refused(R"({"issue":"a","files":[{"path":"/x4/read.jsonl","size":1}]})"));
  EXPECT_TRUE(refused(R"({"issue":"a","files":[],"prune":["/books"]})"));
  EXPECT_TRUE(refused(
      R"({"issue":"a","files":[{"path":"/x4/read.jsonl","size":1,"crc32":1},{"path":"/x4/read.jsonl","size":1,"crc32":1}]})"));
  EXPECT_FALSE(refused(R"({"issue":"a","files":[]})"));

  EXPECT_TRUE(issuePathAllowed("/books/read/r-2026-11-09-1.epub"));
  EXPECT_FALSE(issuePathAllowed("/books/read/.epub"));
  EXPECT_FALSE(issuePathAllowed("/books/read/a b.epub"));
  EXPECT_FALSE(issuePathAllowed("/books/meditations.epub"));

  // What the writer writes besides the set: threads, recaps, decks, crosswords.
  EXPECT_TRUE(issuePathAllowed("/x4/threads.jsonl"));
  EXPECT_TRUE(issuePathAllowed("/x4/previously.jsonl"));
  EXPECT_TRUE(issuePathAllowed("/x4/decks/meditations.jsonl"));
  EXPECT_TRUE(issuePathAllowed("/x4/crosswords/hu.jsonl"));
  EXPECT_FALSE(issuePathAllowed("/x4/decks/.jsonl"));
  EXPECT_FALSE(issuePathAllowed("/x4/decks/a/b.jsonl"));
  EXPECT_FALSE(issuePathAllowed("/x4/decks/0123456789012345678901234567890123.jsonl"));
  EXPECT_FALSE(issuePathAllowed("/x4/crosswords/hu.grids.jsonl"));
  EXPECT_FALSE(issuePathAllowed("/x4/decksx/a.jsonl"));
  EXPECT_FALSE(issuePathAllowed("/x4/net.json"));
  EXPECT_FALSE(issuePathAllowed("/x4/next.jsonl"));

  Issue decks;
  const std::string pruned =
      R"({"issue":"a","files":[{"path":"/x4/decks/meditations.jsonl","size":1,"crc32":1}],"prune":["/x4/decks"]})";
  ASSERT_EQ(parseIssue(pruned.data(), pruned.size(), decks), nullptr);
  EXPECT_TRUE(decks.pruneDecks);
  EXPECT_FALSE(decks.pruneRead);
  EXPECT_TRUE(issueLists(decks, DECKS_DIR, "meditations.jsonl"));
  EXPECT_FALSE(issueLists(decks, DECKS_DIR, "gone.jsonl"));

  EXPECT_TRUE(repoNameAllowed("VeghV03/x4-data"));
  EXPECT_FALSE(repoNameAllowed("x4-data"));
  EXPECT_FALSE(repoNameAllowed("a/b/c"));
  EXPECT_FALSE(repoNameAllowed("/x4-data"));
  EXPECT_FALSE(repoNameAllowed("a/"));
  EXPECT_FALSE(repoNameAllowed("a/b?ref=x"));
  EXPECT_FALSE(repoNameAllowed("a/.."));
  EXPECT_FALSE(repoNameAllowed(nullptr));
}

TEST(Football, ParsesAndPicksTheHeadline) {
  // Saturday 2026-11-07 15:00 Budapest is 1794060000.
  const std::string json =
      R"({"fetched":1794060000,"matches":[)"
      R"({"at":1793800800,"comp":"PL","home":"Brighton","away":"Brentford","score":[2,1]},)"
      R"({"at":1794081600,"comp":"La Liga","home":"Real Betis","away":"Barcelona"},)"
      R"({"at":1794250800,"comp":"CL","home":"Barcelona","away":"Inter"}],)"
      R"("tables":[{"comp":"PL","name":"Premier League","rows":[)"
      R"({"pos":1,"team":"Arsenal","played":11,"gd":15,"points":26},)"
      R"({"pos":6,"team":"Brighton","played":11,"gd":4,"points":18,"mine":true}]}]})";
  TzRule zone;
  ASSERT_TRUE(parsePosixTz(kBudapest, zone));
  Football f;
  ASSERT_TRUE(parseFootball(json.data(), json.size(), f));
  EXPECT_EQ(f.matchCount, 3);
  EXPECT_TRUE(f.matches[0].over());
  EXPECT_FALSE(f.matches[1].hasScore());
  ASSERT_EQ(f.tableCount, 1);
  EXPECT_EQ(f.tables[0].count, 2);
  EXPECT_TRUE(f.tables[0].rows[1].mine);
  EXPECT_EQ(f.tables[0].rows[1].points, 18);

  // Today's match first, though Wednesday's result is newer than nothing.
  const Match* m = headline(f, 1794060000, zone);
  ASSERT_NE(m, nullptr);
  EXPECT_STREQ(m->home, "Real Betis");
  char line[64];
  formatMatch(*m, line, sizeof(line));
  EXPECT_STREQ(line, "Real Betis \xE2\x80\x93 Barcelona");
  formatMatch(f.matches[0], line, sizeof(line));
  EXPECT_STREQ(line, "Brighton 2\xE2\x80\x93" "1 Brentford");

  // Without tables Today reads less.
  Football light;
  ASSERT_TRUE(parseFootball(json.data(), json.size(), light, false));
  EXPECT_EQ(light.matchCount, 3);
  EXPECT_EQ(light.tableCount, 0);

  // A day later, with nothing today or tomorrow and the last result older than two days: the next fixture.
  Football later;
  const std::string two = R"({"matches":[{"at":1793800800,"home":"A","away":"B","score":[0,0]},{"at":1794600000,"home":"C","away":"D"}]})";
  ASSERT_TRUE(parseFootball(two.data(), two.size(), later));
  m = headline(later, 1794146400, zone);
  ASSERT_NE(m, nullptr);
  EXPECT_STREQ(m->home, "C");
  // Within two days, the result.
  m = headline(later, 1793887200, zone);
  ASSERT_NE(m, nullptr);
  EXPECT_STREQ(m->home, "A");
}

TEST(Upload, NamesSortByTheTimeTheyWereSent) {
  char first[40];
  char later[40];
  inboxName(1794389701, "highlights", "jsonl", first, sizeof(first));
  inboxName(1794389702, "read-log", "jsonl", later, sizeof(later));
  EXPECT_STREQ(first, "20261111T093501Z-highlights.jsonl");
  EXPECT_STREQ(later, "20261111T093502Z-read-log.jsonl");
  EXPECT_LT(std::string(first), std::string(later));
  char stats[40];
  inboxName(1794389701, "stats", "json", stats, sizeof(stats));
  EXPECT_STREQ(stats, "20261111T093501Z-stats.json");
}

TEST(Upload, BodyIsTheFileInBase64) {
  const auto encoded = [](const std::string& in) {
    char out[64];
    const size_t n = base64(reinterpret_cast<const uint8_t*>(in.data()), in.size(), out);
    return std::string(out, n);
  };
  EXPECT_EQ(encoded(""), "");
  EXPECT_EQ(encoded("f"), "Zg==");
  EXPECT_EQ(encoded("fo"), "Zm8=");
  EXPECT_EQ(encoded("foo"), "Zm9v");
  EXPECT_EQ(encoded("foobar"), "Zm9vYmFy");
  EXPECT_EQ(encoded("{\"id\":1}\n"), "eyJpZCI6MX0K");
  // A file sent in chunks of a multiple of 3 reads as one.
  EXPECT_EQ(encoded("foo") + encoded("ba"), "Zm9vYmE=");
  const std::string body = std::string(PUT_HEAD) + encoded("foobar") + PUT_TAIL;
  EXPECT_EQ(body, "{\"message\":\"x4\",\"content\":\"Zm9vYmFy\"}");
  EXPECT_EQ(putBodyLength(6), body.size());
  EXPECT_EQ(putBodyLength(7), body.size() + 4);
  EXPECT_EQ(putBodyLength(0), sizeof(PUT_HEAD) - 1 + sizeof(PUT_TAIL) - 1);
}

TEST(Upload, RemembersTheNameAWaitingUploadWentUnder) {
  UpState state;
  state.set("highlights", "20261111T093501Z-highlights.jsonl", 420);
  state.set("stats", "20261111T093501Z-stats.json", 1220);
  state.hasStatsCrc = true;
  state.statsCrc = 3069196794u;
  std::string json;
  ASSERT_TRUE(upStateToJson(state, json));
  UpState again;
  ASSERT_TRUE(upStateFromJson(json.data(), json.size(), again));
  ASSERT_NE(again.find("highlights"), nullptr);
  EXPECT_STREQ(again.find("highlights")->name, "20261111T093501Z-highlights.jsonl");
  EXPECT_EQ(again.find("highlights")->size, 420u);
  EXPECT_TRUE(again.hasStatsCrc);
  EXPECT_EQ(again.statsCrc, 3069196794u);
  EXPECT_EQ(again.find("reviews"), nullptr);

  again.set("highlights", "20261111T113000Z-highlights.jsonl", 900);  // taken by cable, grown again
  EXPECT_EQ(again.count, 2);
  EXPECT_EQ(again.find("highlights")->size, 900u);
  again.erase("highlights");
  EXPECT_EQ(again.find("highlights"), nullptr);
  EXPECT_NE(again.find("stats"), nullptr);

  UpState empty;
  EXPECT_FALSE(upStateFromJson("not json", 8, empty));
  EXPECT_TRUE(upStateFromJson("{}", 2, empty));
  EXPECT_EQ(empty.count, 0);
  EXPECT_FALSE(empty.hasStatsCrc);
}
