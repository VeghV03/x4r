#include "JsonlReader.h"

#include <Logging.h>

#include <cstring>

namespace x4 {

bool JsonlReader::fill() {
  const int got = file.read(buffer.chunk, READ_CHUNK);
  chunkPos = 0;
  chunkLength = got > 0 ? static_cast<size_t>(got) : 0;
  return chunkLength > 0;
}

bool JsonlReader::seek(const uint32_t offset) {
  chunkLength = 0;
  chunkPos = 0;
  filePos = offset;
  return file.seekSet(offset);
}

JsonlReader::Line JsonlReader::next(size_t& length, uint32_t& offset) {
  while (true) {
    size_t len = 0;
    bool overlong = false;
    bool sawByte = false;
    offset = filePos;
    while (true) {
      if (chunkPos >= chunkLength && !fill()) break;
      const uint8_t c = buffer.chunk[chunkPos++];
      ++filePos;
      sawByte = true;
      if (c == '\n') break;
      if (len + 1 < JSONL_LINE_MAX) {
        buffer.text[len++] = static_cast<char>(c);
      } else {
        overlong = true;
      }
    }
    if (!sawByte) {
      length = 0;
      buffer.text[0] = '\0';
      return Line::End;
    }
    if (overlong) {
      LOG_ERR("JSONL", "Skipped line over %u bytes at %u", static_cast<unsigned>(JSONL_LINE_MAX),
              static_cast<unsigned>(offset));
      length = 0;
      buffer.text[0] = '\0';
      return Line::TooLong;
    }
    if (len > 0 && buffer.text[len - 1] == '\r') --len;
    size_t start = 0;
    if (offset == 0 && len >= 3 && std::memcmp(buffer.text, "\xEF\xBB\xBF", 3) == 0) start = 3;
    if (start > 0) {
      std::memmove(buffer.text, buffer.text + start, len - start);
      len -= start;
      offset += start;
    }
    buffer.text[len] = '\0';
    size_t i = 0;
    while (i < len && (buffer.text[i] == ' ' || buffer.text[i] == '\t')) ++i;
    if (i == len) continue;  // blank line
    length = len;
    return Line::Ok;
  }
}

bool readLineAt(const char* path, const uint32_t offset, LineBuffer& buffer, size_t& length) {
  HalFile file;
  if (!Storage.openFileForRead("JSONL", path, file)) return false;
  JsonlReader reader(file, buffer);
  uint32_t at = 0;
  const bool ok = reader.seek(offset) && reader.next(length, at) == JsonlReader::Line::Ok && at == offset;
  file.close();
  return ok;
}

}  // namespace x4
