#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

#include "LearnTypes.h"

namespace x4 {

// Scratch for reading and parsing one JSONL line. Heap-owned by the caller
// (about 10.7 KB) and reused for every line; the text area doubles as the
// arena that a parsed item's strings are copied into.
struct LineBuffer {
  char text[ARENA_MAX];
  uint8_t chunk[READ_CHUNK];
};

// Bounded line reader. Never holds more than one line: lines longer than
// JSONL_LINE_MAX - 1 bytes are skipped and reported as TooLong. Strips "\r\n" and a
// leading UTF-8 BOM, and skips blank lines.
class JsonlReader {
 public:
  enum class Line : uint8_t { Ok, TooLong, End };

  JsonlReader(HalFile& file, LineBuffer& buffer) : file(file), buffer(buffer) {}
  // Line text is buffer.text[0..length), NUL-terminated; offset is where it starts.
  Line next(size_t& length, uint32_t& offset);
  bool seek(uint32_t offset);

 private:
  bool fill();

  HalFile& file;
  LineBuffer& buffer;
  size_t chunkLength = 0;
  size_t chunkPos = 0;
  uint32_t filePos = 0;  // file offset of chunk[chunkPos]
};

// Reads the single line starting at `offset` of `path`.
bool readLineAt(const char* path, uint32_t offset, LineBuffer& buffer, size_t& length);

}  // namespace x4
