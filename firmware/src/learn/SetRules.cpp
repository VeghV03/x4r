#include "SetRules.h"

namespace x4 {

int64_t civilDay(const int64_t unixSeconds, const int32_t tzMinutes) {
  const int64_t local = unixSeconds + static_cast<int64_t>(tzMinutes) * 60;
  // Floor division so times before the epoch still land on the right day.
  return local >= 0 ? local / 86400 : -((-local + 86399) / 86400);
}

CivilTime civilTime(const int64_t unixSeconds, const int32_t tzMinutes) {
  const int64_t days = civilDay(unixSeconds, tzMinutes);
  const int64_t seconds = unixSeconds + static_cast<int64_t>(tzMinutes) * 60 - days * 86400;
  CivilTime t{};
  t.hour = static_cast<uint8_t>(seconds / 3600);
  t.minute = static_cast<uint8_t>(seconds / 60 % 60);
  t.weekday = static_cast<uint8_t>(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday
  // Days to year/month/day (Howard Hinnant's civil_from_days).
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  t.day = static_cast<uint8_t>(doy - (153 * mp + 2) / 5 + 1);
  t.month = static_cast<uint8_t>(mp < 10 ? mp + 3 : mp - 9);
  t.year = static_cast<int32_t>(yoe + era * 400 + (t.month <= 2 ? 1 : 0));
  return t;
}

bool openFirstDay(LearnState& state) {
  if (state.day != 0) return false;
  startDay(state, 1);
  return true;
}

void startDay(LearnState& state, const uint32_t day) {
  state.day = day;
  state.newToday = 0;
}

}  // namespace x4
