#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// The issue the X4 takes over Wi-Fi, from the Mac on the LAN or from the data repo: its
// manifest and what the X4 took last time (docs/PROTOCOL.md, "LAN issue" and "Repo issue").
// No hardware here.
namespace x4::net {

inline constexpr char TAKEN_PATH[] = "/x4/state/lan.json";
inline constexpr char READ_DIR[] = "/books/read";
inline constexpr char DECKS_DIR[] = "/x4/decks";
inline constexpr char CROSSWORDS_DIR[] = "/x4/crosswords";

struct IssueFile {
  char path[64] = {};
  uint32_t size = 0;
  uint32_t crc = 0;
  bool take = false;  // differs from what the X4 took for this path
};

struct Issue {
  static constexpr size_t kFiles = 48;
  static constexpr uint32_t kFileMax = 4UL << 20;
  static constexpr uint32_t kTotalMax = 16UL << 20;

  char id[32] = {};
  IssueFile files[kFiles];
  uint8_t count = 0;
  bool pruneRead = false;   // Read articles the issue does not list go
  bool pruneDecks = false;  // and so do decks
};

// The issue id and CRC of each file the X4 took last time.
struct Taken {
  char id[32] = {};
  struct File {
    char path[64] = {};
    uint32_t crc = 0;
  } files[Issue::kFiles];
  uint8_t count = 0;
};

// Only the stream, the Read list, football, threads and recaps under /x4, a deck
// (/x4/decks/<id>.jsonl), a crossword set (/x4/crosswords/<lang>.jsonl) and a Read article
// (/books/read/<id>.epub).
bool issuePathAllowed(const char* path);
// "<owner>/<repo>" as GitHub names them: two parts of [A-Za-z0-9._-], under 80 bytes in all.
bool repoNameAllowed(const char* name);
// Parses and checks a manifest. Returns nullptr, or why it was refused.
const char* parseIssue(const char* json, size_t length, Issue& out);
// Marks the files whose CRC differs from what was taken. Returns how many.
uint8_t markChanged(Issue& issue, const Taken& taken);
// True when `name`, a file in `dir`, is listed by the issue.
bool issueLists(const Issue& issue, const char* dir, const char* name);

bool takenFromJson(const char* json, size_t length, Taken& out);
bool takenToJson(const Issue& issue, std::string& out);
// The body of POST /taken: the issue id and every path it lists.
bool takenRequest(const Issue& issue, std::string& out);

}  // namespace x4::net
