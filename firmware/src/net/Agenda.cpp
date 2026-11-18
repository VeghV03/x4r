#include "Agenda.h"

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace x4::net {

namespace {

constexpr int64_t kDay = 86400;

int64_t floorDiv(const int64_t a, const int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

// "2026-11-07" to local days.
bool isoDay(const char* s, int32_t& out) {
  if (!s || strlen(s) < 10 || s[4] != '-' || s[7] != '-') return false;
  const int y = atoi(s);
  const int m = atoi(s + 5);
  const int d = atoi(s + 8);
  if (m < 1 || m > 12 || d < 1 || d > 31) return false;
  out = daysFromCivil(y, m, d);
  return true;
}

void isoDate(const int32_t day, char* out, const size_t size) {
  int32_t y, m, d;
  civilFromDays(day, y, m, d);
  snprintf(out, size, "%04d-%02d-%02d", static_cast<int>(y), static_cast<int>(m), static_cast<int>(d));
}

int8_t wholeDegrees(const float v) {
  const float r = std::round(v);
  return static_cast<int8_t>(r < -99 ? -99 : r > 99 ? 99 : r);
}

}  // namespace

bool parseOpenMeteo(const char* json, const size_t length, Weather& out) {
  out = Weather{};
  JsonDocument filter;
  filter["daily"]["time"] = true;
  filter["daily"]["weather_code"] = true;
  filter["daily"]["temperature_2m_max"] = true;
  filter["daily"]["temperature_2m_min"] = true;
  filter["hourly"]["time"] = true;
  filter["hourly"]["temperature_2m"] = true;
  filter["hourly"]["precipitation_probability"] = true;
  filter["hourly"]["weather_code"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json, length, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
    return false;
  }
  const JsonArrayConst days = doc["daily"]["time"];
  const JsonArrayConst codes = doc["daily"]["weather_code"];
  const JsonArrayConst highs = doc["daily"]["temperature_2m_max"];
  const JsonArrayConst lows = doc["daily"]["temperature_2m_min"];
  if (days.isNull() || codes.isNull() || highs.isNull() || lows.isNull()) return false;
  for (size_t i = 0; i < days.size() && out.count < Weather::kDays; ++i) {
    WeatherDay d;
    if (!isoDay(days[i] | "", d.day) || !codes[i].is<int>() || highs[i].isNull() || lows[i].isNull()) continue;
    d.code = static_cast<uint8_t>(codes[i].as<int>());
    d.high = wholeDegrees(highs[i].as<float>());
    d.low = wholeDegrees(lows[i].as<float>());
    out.days[out.count++] = d;
  }
  // "2026-11-07T09:00", local like the days. A forecast without hours is still a forecast.
  const JsonArrayConst times = doc["hourly"]["time"];
  const JsonArrayConst temps = doc["hourly"]["temperature_2m"];
  const JsonArrayConst rains = doc["hourly"]["precipitation_probability"];
  const JsonArrayConst skies = doc["hourly"]["weather_code"];
  for (size_t i = 0; i < times.size() && out.hourCount < Weather::kHours; ++i) {
    const char* at = times[i] | "";
    WeatherHour h;
    if (!isoDay(at, h.day) || strlen(at) < 13 || at[10] != 'T' || temps[i].isNull()) continue;
    h.hour = static_cast<uint8_t>(atoi(at + 11) % 24);
    h.temp = wholeDegrees(temps[i].as<float>());
    h.rain = static_cast<uint8_t>(rains[i] | 0);
    h.code = static_cast<uint8_t>(skies[i] | 0);
    out.hours[out.hourCount++] = h;
  }
  return out.count > 0;
}

const char* conditionName(const uint8_t code) {
  switch (code) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Cloudy";
    case 45:
    case 48: return "Fog";
    case 51:
    case 53:
    case 55: return "Drizzle";
    case 56:
    case 57:
    case 66:
    case 67: return "Freezing rain";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80:
    case 81:
    case 82: return "Showers";
    case 85:
    case 86: return "Snow showers";
    case 95:
    case 96:
    case 99: return "Thunderstorm";
    default: return "";
  }
}

bool agendaToJson(const Agenda& agenda, std::string& out) {
  JsonDocument doc;
  doc["fetched"] = agenda.fetched;
  if (agenda.hasEvents) {
    JsonArray events = doc["events"].to<JsonArray>();
    for (uint8_t i = 0; i < agenda.events.count; ++i) {
      const Event& e = agenda.events.items[i];
      JsonObject o = events.add<JsonObject>();
      o["start"] = e.start;
      o["end"] = e.end;
      if (e.allDay) o["all_day"] = true;
      o["title"] = e.title;
      o["cal"] = e.calendar;
      o["uid"] = e.uid;
    }
  }
  if (agenda.hasWeather) {
    JsonArray days = doc["weather"].to<JsonArray>();
    for (uint8_t i = 0; i < agenda.weather.count; ++i) {
      const WeatherDay& d = agenda.weather.days[i];
      char date[12];
      isoDate(d.day, date, sizeof(date));
      JsonObject o = days.add<JsonObject>();
      o["date"] = date;
      o["code"] = d.code;
      o["high"] = d.high;
      o["low"] = d.low;
    }
    if (agenda.weather.hourCount > 0) {
      JsonArray hours = doc["hours"].to<JsonArray>();
      for (uint8_t i = 0; i < agenda.weather.hourCount; ++i) {
        const WeatherHour& h = agenda.weather.hours[i];
        JsonObject o = hours.add<JsonObject>();
        char date[12];
        isoDate(h.day, date, sizeof(date));
        o["date"] = date;
        o["hour"] = h.hour;
        o["code"] = h.code;
        o["temp"] = h.temp;
        o["rain"] = h.rain;
      }
    }
  }
  out.clear();
  serializeJson(doc, out);
  return !out.empty();
}

bool agendaFromJson(const char* json, const size_t length, Agenda& out) {
  out = Agenda{};
  JsonDocument doc;
  if (deserializeJson(doc, json, length) != DeserializationError::Ok || !doc.is<JsonObject>()) return false;
  out.fetched = doc["fetched"] | static_cast<int64_t>(0);
  const JsonArrayConst events = doc["events"];
  out.hasEvents = !events.isNull();
  for (JsonObjectConst o : events) {
    Event e;
    e.start = o["start"] | static_cast<int64_t>(0);
    e.end = o["end"] | e.start;
    e.allDay = o["all_day"] | false;
    e.calendar = o["cal"] | 0;
    e.uid = o["uid"] | 0u;
    e.occurrence = e.start;
    strncpy(e.title, o["title"] | "", sizeof(e.title) - 1);
    out.events.add(e);
  }
  const JsonArrayConst days = doc["weather"];
  out.hasWeather = !days.isNull();
  for (JsonObjectConst o : days) {
    WeatherDay d;
    if (!isoDay(o["date"] | "", d.day) || out.weather.count >= Weather::kDays) continue;
    d.code = o["code"] | 0;
    d.high = o["high"] | 0;
    d.low = o["low"] | 0;
    out.weather.days[out.weather.count++] = d;
  }
  for (JsonObjectConst o : doc["hours"].as<JsonArrayConst>()) {
    WeatherHour h;
    if (!isoDay(o["date"] | "", h.day) || out.weather.hourCount >= Weather::kHours) continue;
    h.hour = o["hour"] | 0;
    h.code = o["code"] | 0;
    h.temp = o["temp"] | 0;
    h.rain = o["rain"] | 0;
    out.weather.hours[out.weather.hourCount++] = h;
  }
  return true;
}

int32_t localDay(const int64_t utc, const TzRule& zone) {
  return static_cast<int32_t>(floorDiv(zone.toLocal(utc), kDay));
}

uint8_t eventsOfDay(const Agenda& agenda, const TzRule& zone, const int32_t day, const int64_t after,
                    const Event** out, const uint8_t max) {
  const int64_t dayStart = zone.toUtc(static_cast<int64_t>(day) * kDay);
  const int64_t dayEnd = zone.toUtc(static_cast<int64_t>(day + 1) * kDay);
  uint8_t n = 0;
  // All-day events first, then the rest in time order.
  for (int pass = 0; pass < 2 && n < max; ++pass) {
    for (uint8_t i = 0; i < agenda.events.count && n < max; ++i) {
      const Event& e = agenda.events.items[i];
      if (e.allDay != (pass == 0)) continue;
      const int64_t end = e.end > e.start ? e.end : e.start + 1;
      if (e.start >= dayEnd || end <= dayStart) continue;
      if (after > 0 && end <= after) continue;
      out[n++] = &e;
    }
  }
  return n;
}

const WeatherDay* weatherOfDay(const Agenda& agenda, const int32_t day) {
  for (uint8_t i = 0; i < agenda.weather.count; ++i) {
    if (agenda.weather.days[i].day == day) return &agenda.weather.days[i];
  }
  return nullptr;
}

uint8_t hoursFrom(const Agenda& agenda, const int32_t day, const uint8_t hour, const WeatherHour** out,
                  const uint8_t max) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < agenda.weather.hourCount && n < max; ++i) {
    const WeatherHour& h = agenda.weather.hours[i];
    // Each stands for the hours up to the next one.
    const int32_t ends = h.day * 24 + h.hour + Weather::kHourStep;
    if (ends > day * 24 + hour) out[n++] = &h;
  }
  return n;
}

void formatClock(const int64_t utc, const TzRule& zone, char* out, const size_t size) {
  const int64_t local = zone.toLocal(utc);
  const int64_t secs = local - floorDiv(local, kDay) * kDay;
  snprintf(out, size, "%02d:%02d", static_cast<int>(secs / 3600), static_cast<int>(secs / 60 % 60));
}

void formatTemperatures(const WeatherDay& day, char* out, const size_t size) {
  snprintf(out, size, "%d\xC2\xB0 / %d\xC2\xB0", day.high, day.low);
}

}  // namespace x4::net
