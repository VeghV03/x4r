#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// What the X4 sends up to the data repo's inbox: names, the body of a request, and which
// uploads are under way (docs/PROTOCOL.md, "Repo issue"; docs/DATA.md, "inbox/"). No hardware
// here.
namespace x4::net {

// "20261111T093501Z-highlights.jsonl": the time in UTC, then what it is.
void inboxName(int64_t unixUtc, const char* kind, const char* ext, char* out, size_t size);

// The body of a PUT that creates a file: {"message":"x4","content":"<the file in base64>"}.
inline constexpr char PUT_HEAD[] = "{\"message\":\"x4\",\"content\":\"";
inline constexpr char PUT_TAIL[] = "\"}";
size_t putBodyLength(size_t fileSize);
// Base64 of `n` bytes into `out`, which holds 4 * ceil(n / 3) characters; returns how many.
// Every chunk but the last must be a multiple of 3 bytes.
size_t base64(const uint8_t* in, size_t n, char* out);

// The name each waiting upload first went under, so a retry sends the same name and never a
// second copy. The size tells a file that was taken by cable and has grown again from the one
// the name was made for.
struct UpState {
  static constexpr size_t kItems = 10;
  struct Item {
    char kind[12] = {};
    char name[40] = {};
    uint32_t size = 0;
  } items[kItems];
  uint8_t count = 0;
  bool hasStatsCrc = false;
  uint32_t statsCrc = 0;  // of the stats.json that went up last

  const Item* find(const char* kind) const;
  void set(const char* kind, const char* name, uint32_t size);
  void erase(const char* kind);
};

bool upStateFromJson(const char* json, size_t length, UpState& out);
bool upStateToJson(const UpState& state, std::string& out);

}  // namespace x4::net
