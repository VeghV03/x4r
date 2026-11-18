#include "Upload.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace x4::net {

void inboxName(const int64_t unixUtc, const char* kind, const char* ext, char* out, const size_t size) {
  const time_t t = static_cast<time_t>(unixUtc);
  struct tm utc {};
  gmtime_r(&t, &utc);
  snprintf(out, size, "%04d%02d%02dT%02d%02d%02dZ-%s.%s", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
           utc.tm_min, utc.tm_sec, kind, ext);
}

size_t putBodyLength(const size_t fileSize) {
  return sizeof(PUT_HEAD) - 1 + (fileSize + 2) / 3 * 4 + sizeof(PUT_TAIL) - 1;
}

size_t base64(const uint8_t* in, const size_t n, char* out) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t a = in[i];
    const uint32_t b = i + 1 < n ? in[i + 1] : 0;
    const uint32_t c = i + 2 < n ? in[i + 2] : 0;
    const uint32_t v = (a << 16) | (b << 8) | c;
    out[o++] = kAlphabet[(v >> 18) & 63];
    out[o++] = kAlphabet[(v >> 12) & 63];
    out[o++] = i + 1 < n ? kAlphabet[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? kAlphabet[v & 63] : '=';
  }
  return o;
}

const UpState::Item* UpState::find(const char* kind) const {
  for (uint8_t i = 0; i < count; ++i) {
    if (strcmp(items[i].kind, kind) == 0) return &items[i];
  }
  return nullptr;
}

void UpState::set(const char* kind, const char* name, const uint32_t size) {
  erase(kind);
  if (count >= kItems) return;
  Item& item = items[count++];
  snprintf(item.kind, sizeof(item.kind), "%s", kind);
  snprintf(item.name, sizeof(item.name), "%s", name);
  item.size = size;
}

void UpState::erase(const char* kind) {
  for (uint8_t i = 0; i < count; ++i) {
    if (strcmp(items[i].kind, kind) != 0) continue;
    items[i] = items[--count];
    items[count] = Item{};
    return;
  }
}

bool upStateFromJson(const char* json, const size_t length, UpState& out) {
  out = UpState{};
  JsonDocument doc;
  if (deserializeJson(doc, json, length) != DeserializationError::Ok) return false;
  for (JsonPairConst kv : doc["waiting"].as<JsonObjectConst>()) {
    const char* name = kv.value()["name"];
    if (!name || strlen(kv.key().c_str()) >= sizeof(UpState::Item::kind) || strlen(name) >= sizeof(UpState::Item::name)) {
      continue;
    }
    out.set(kv.key().c_str(), name, kv.value()["size"] | 0u);
  }
  if (doc["stats_crc"].is<uint32_t>()) {
    out.hasStatsCrc = true;
    out.statsCrc = doc["stats_crc"].as<uint32_t>();
  }
  return true;
}

bool upStateToJson(const UpState& state, std::string& out) {
  JsonDocument doc;
  JsonObject waiting = doc["waiting"].to<JsonObject>();
  for (uint8_t i = 0; i < state.count; ++i) {
    JsonObject item = waiting[state.items[i].kind].to<JsonObject>();
    item["name"] = state.items[i].name;
    item["size"] = state.items[i].size;
  }
  if (state.hasStatsCrc) doc["stats_crc"] = state.statsCrc;
  out.clear();
  serializeJson(doc, out);
  return !doc.overflowed();
}

}  // namespace x4::net
