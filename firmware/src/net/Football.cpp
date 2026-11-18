#include "Football.h"

#include <ArduinoJson.h>

#include <cstdio>

#include "Agenda.h"

namespace x4::net {

namespace {

void copy(char* out, const size_t size, const char* text) { snprintf(out, size, "%s", text ? text : ""); }

int8_t goals(JsonVariantConst v) {
  if (!v.is<int>()) return -1;
  const int n = v.as<int>();
  return static_cast<int8_t>(n < 0 ? -1 : n > 99 ? 99 : n);
}

}  // namespace

bool parseFootball(const char* json, const size_t length, Football& out, const bool tables) {
  out = Football{};
  JsonDocument filter;
  filter["fetched"] = true;
  filter["matches"] = true;
  if (tables) filter["tables"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json, length, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
    return false;
  }
  out.fetched = doc["fetched"] | static_cast<int64_t>(0);
  for (JsonVariantConst m : doc["matches"].as<JsonArrayConst>()) {
    if (out.matchCount >= Football::kMatches) break;
    const char* home = m["home"];
    const char* away = m["away"];
    if (!home || !away || !m["at"].is<int64_t>()) continue;
    Match& match = out.matches[out.matchCount++];
    match.at = m["at"].as<int64_t>();
    copy(match.comp, sizeof(match.comp), m["comp"]);
    copy(match.home, sizeof(match.home), home);
    copy(match.away, sizeof(match.away), away);
    JsonArrayConst score = m["score"].as<JsonArrayConst>();
    if (score.size() == 2) {
      match.homeGoals = goals(score[0]);
      match.awayGoals = goals(score[1]);
    }
    match.live = match.hasScore() && (m["live"] | false);
  }
  for (JsonVariantConst t : doc["tables"].as<JsonArrayConst>()) {
    if (out.tableCount >= Football::kTables) break;
    Table& table = out.tables[out.tableCount++];
    copy(table.comp, sizeof(table.comp), t["comp"]);
    for (JsonVariantConst r : t["rows"].as<JsonArrayConst>()) {
      if (table.count >= Table::kRows) break;
      TableRow& row = table.rows[table.count++];
      row.pos = r["pos"] | static_cast<uint8_t>(table.count);
      copy(row.team, sizeof(row.team), r["team"]);
      row.points = r["points"] | static_cast<uint16_t>(0);
      row.mine = r["mine"] | false;
    }
  }
  return true;
}

const Match* headline(const Football& football, int64_t now, const TzRule& zone) {
  if (football.matchCount == 0) return nullptr;
  if (now <= 0) now = football.fetched;
  const Match* lastOver = nullptr;
  const Match* next = nullptr;
  for (uint8_t i = 0; i < football.matchCount; ++i) {
    const Match& m = football.matches[i];
    if (m.live) return &m;
    if (m.over() && (!lastOver || m.at >= lastOver->at)) lastOver = &m;
    if (!m.over() && (!next || m.at < next->at)) next = &m;
  }
  if (now <= 0) return next ? next : lastOver;
  const int32_t today = localDay(now, zone);
  const Match* todayNext = nullptr;
  const Match* todayOver = nullptr;
  const Match* tomorrow = nullptr;
  for (uint8_t i = 0; i < football.matchCount; ++i) {
    const Match& m = football.matches[i];
    const int32_t day = localDay(m.at, zone);
    if (day == today && !m.over() && (!todayNext || m.at < todayNext->at)) todayNext = &m;
    if (day == today && m.over() && (!todayOver || m.at > todayOver->at)) todayOver = &m;
    if (day == today + 1 && (!tomorrow || m.at < tomorrow->at)) tomorrow = &m;
  }
  if (todayNext) return todayNext;
  if (todayOver) return todayOver;
  if (tomorrow) return tomorrow;
  if (lastOver && localDay(lastOver->at, zone) >= today - 2) return lastOver;
  return next ? next : lastOver;
}

void formatMatch(const Match& match, char* out, const size_t size) {
  if (match.hasScore()) {
    snprintf(out, size, "%s %d\xE2\x80\x93%d %s", match.home, match.homeGoals, match.awayGoals, match.away);
  } else {
    snprintf(out, size, "%s \xE2\x80\x93 %s", match.home, match.away);
  }
}

}  // namespace x4::net
