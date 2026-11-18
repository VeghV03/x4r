#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "Ical.h"

// What the X4 fetched online: the next days of calendar events and the
// forecast. Kept in /x4/agenda.json between wakes. No hardware here.
namespace x4::net {

struct WeatherDay {
  int32_t day = 0;  // local days since 1970-01-01
  uint8_t code = 0;  // WMO weather code
  int8_t high = 0;   // °C
  int8_t low = 0;
};

// One hour of the forecast, in local time like the days.
struct WeatherHour {
  int32_t day = 0;   // local days since 1970-01-01
  uint8_t hour = 0;  // 0..23
  uint8_t code = 0;  // WMO weather code
  int8_t temp = 0;   // °C
  uint8_t rain = 0;  // chance of rain or snow, %
};

struct Weather {
  static constexpr size_t kDays = 7;
  // The day ahead from the fetch, in steps of three hours (asked for so in NetSync).
  static constexpr size_t kHours = 8;
  static constexpr uint8_t kHourStep = 3;
  WeatherDay days[kDays];
  uint8_t count = 0;
  WeatherHour hours[kHours];
  uint8_t hourCount = 0;
};

struct Agenda {
  int64_t fetched = 0;  // UTC of the last good fetch, 0 for none
  bool hasEvents = false;   // a calendar answered
  bool hasWeather = false;  // the forecast answered
  EventList events;
  Weather weather;
};

// Open-Meteo's forecast answer: daily weather_code and temperature_2m_max/min, and, when asked
// for, hourly temperature_2m, precipitation_probability and weather_code from the current hour
// (the first kHours entries are kept).
bool parseOpenMeteo(const char* json, size_t length, Weather& out);
// "Cloudy", "Light rain"; the WMO code in a word or two.
const char* conditionName(uint8_t code);

bool agendaToJson(const Agenda& agenda, std::string& out);
bool agendaFromJson(const char* json, size_t length, Agenda& out);

int32_t localDay(int64_t utc, const TzRule& zone);
// Up to `max` events of local day `day`, earliest first, all-day ones first;
// with `after` set, only those still to come or under way at it.
uint8_t eventsOfDay(const Agenda& agenda, const TzRule& zone, int32_t day, int64_t after, const Event** out,
                    uint8_t max);
const WeatherDay* weatherOfDay(const Agenda& agenda, int32_t day);
// Up to `max` forecast hours not yet over at `hour` of local day `day`, earliest first.
uint8_t hoursFrom(const Agenda& agenda, int32_t day, uint8_t hour, const WeatherHour** out, uint8_t max);

// "09:30" in `zone`.
void formatClock(int64_t utc, const TzRule& zone, char* out, size_t size);
// "12° / 4°"
void formatTemperatures(const WeatherDay& day, char* out, size_t size);

}  // namespace x4::net
