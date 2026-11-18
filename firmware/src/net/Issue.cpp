#include "Issue.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

namespace x4::net {

namespace {

bool readIdChar(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
         c == '-';
}

bool endsWith(const char* s, const char* tail) {
  const size_t n = strlen(s);
  const size_t t = strlen(tail);
  return n >= t && strcmp(s + n - t, tail) == 0;
}

// <dir>/<name><ext>, the name 1 to `max` of [A-Za-z0-9._-] and not starting with a dot.
bool namedIn(const char* path, const char* dir, const char* ext, const size_t max) {
  const size_t dirLength = strlen(dir);
  if (strncmp(path, dir, dirLength) != 0 || path[dirLength] != '/' || !endsWith(path, ext)) return false;
  const char* name = path + dirLength + 1;
  const size_t total = strlen(name);
  const size_t extLength = strlen(ext);
  if (total <= extLength || total - extLength > max || name[0] == '.') return false;
  for (size_t i = 0; i < total - extLength; ++i) {
    if (!readIdChar(name[i])) return false;
  }
  return true;
}

}  // namespace

bool issuePathAllowed(const char* path) {
  if (!path) return false;
  for (const char* fixed : {"/x4/stream.jsonl", "/x4/read.jsonl", "/x4/football.json", "/x4/threads.jsonl",
                            "/x4/previously.jsonl"}) {
    if (strcmp(path, fixed) == 0) return true;
  }
  return namedIn(path, READ_DIR, ".epub", 32) || namedIn(path, DECKS_DIR, ".jsonl", 32) ||
         namedIn(path, CROSSWORDS_DIR, ".jsonl", 2);
}

bool repoNameAllowed(const char* name) {
  if (!name || strlen(name) >= 80) return false;
  const char* slash = strchr(name, '/');
  if (!slash || slash == name || !slash[1] || strchr(slash + 1, '/')) return false;
  for (const char* c = name; *c; ++c) {
    if (c != slash && !readIdChar(*c)) return false;
  }
  return name[0] != '.' && slash[1] != '.';
}

const char* parseIssue(const char* json, const size_t length, Issue& out) {
  out = Issue{};
  JsonDocument doc;
  if (deserializeJson(doc, json, length) != DeserializationError::Ok) return "unreadable";
  const char* id = doc["issue"];
  if (!id || !*id || strlen(id) >= sizeof(out.id)) return "no issue id";
  strcpy(out.id, id);
  JsonArrayConst files = doc["files"].as<JsonArrayConst>();
  if (files.isNull()) return "no files";
  if (files.size() > Issue::kFiles) return "too many files";
  uint32_t total = 0;
  for (JsonVariantConst f : files) {
    const char* path = f["path"];
    if (!issuePathAllowed(path) || strlen(path) >= sizeof(IssueFile::path)) return "path not allowed";
    if (!f["size"].is<uint32_t>() || !f["crc32"].is<uint32_t>()) return "size or crc32 missing";
    const uint32_t size = f["size"].as<uint32_t>();
    if (size > Issue::kFileMax || total + size > Issue::kTotalMax) return "too large";
    for (uint8_t i = 0; i < out.count; ++i) {
      if (strcmp(out.files[i].path, path) == 0) return "a path twice";
    }
    IssueFile& file = out.files[out.count++];
    strcpy(file.path, path);
    file.size = size;
    file.crc = f["crc32"].as<uint32_t>();
    total += size;
  }
  for (JsonVariantConst p : doc["prune"].as<JsonArrayConst>()) {
    const char* dir = p.as<const char*>();
    if (dir && strcmp(dir, READ_DIR) == 0) {
      out.pruneRead = true;
    } else if (dir && strcmp(dir, DECKS_DIR) == 0) {
      out.pruneDecks = true;
    } else {
      return "prune not allowed";
    }
  }
  return nullptr;
}

uint8_t markChanged(Issue& issue, const Taken& taken) {
  uint8_t changed = 0;
  for (uint8_t i = 0; i < issue.count; ++i) {
    IssueFile& file = issue.files[i];
    file.take = true;
    for (uint8_t k = 0; k < taken.count; ++k) {
      if (strcmp(taken.files[k].path, file.path) == 0) {
        file.take = taken.files[k].crc != file.crc;
        break;
      }
    }
    changed += file.take;
  }
  return changed;
}

bool issueLists(const Issue& issue, const char* dir, const char* name) {
  char path[sizeof(IssueFile::path) + 8];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  for (uint8_t i = 0; i < issue.count; ++i) {
    if (strcmp(issue.files[i].path, path) == 0) return true;
  }
  return false;
}

bool takenFromJson(const char* json, const size_t length, Taken& out) {
  out = Taken{};
  JsonDocument doc;
  if (deserializeJson(doc, json, length) != DeserializationError::Ok) return false;
  snprintf(out.id, sizeof(out.id), "%s", doc["issue"] | "");
  for (JsonPairConst kv : doc["files"].as<JsonObjectConst>()) {
    if (out.count >= Issue::kFiles || strlen(kv.key().c_str()) >= sizeof(Taken::File::path)) continue;
    Taken::File& f = out.files[out.count++];
    strcpy(f.path, kv.key().c_str());
    f.crc = kv.value().as<uint32_t>();
  }
  return true;
}

bool takenToJson(const Issue& issue, std::string& out) {
  JsonDocument doc;
  doc["issue"] = issue.id;
  JsonObject files = doc["files"].to<JsonObject>();
  for (uint8_t i = 0; i < issue.count; ++i) files[issue.files[i].path] = issue.files[i].crc;
  out.clear();
  serializeJson(doc, out);
  return !doc.overflowed();
}

bool takenRequest(const Issue& issue, std::string& out) {
  JsonDocument doc;
  doc["issue"] = issue.id;
  JsonArray files = doc["files"].to<JsonArray>();
  for (uint8_t i = 0; i < issue.count; ++i) files.add(issue.files[i].path);
  out.clear();
  serializeJson(doc, out);
  return !doc.overflowed();
}

}  // namespace x4::net
