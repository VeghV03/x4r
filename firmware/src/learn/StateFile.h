#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <initializer_list>

// State files rewritten whole: written to a tmp file, closed, then moved over the old file. FAT
// cannot rename over a file, so the move removes the old one first, and a power cut between the
// two leaves only the tmp, complete. recover() puts it back before the file is read.
namespace x4::statefile {

struct Part {
  const void* data;
  size_t length;
};

// Writes `parts` in order to `tmp` and moves it over `path`. A failed write removes `tmp` and
// leaves `path` as it was.
bool replace(const char* tmp, const char* path, std::initializer_list<Part> parts);

// For writers that stream records: `file` was opened on `tmp` and written; `ok` says the writes
// succeeded. Closes it and moves it over `path`, or removes it.
bool commit(HalFile& file, bool ok, const char* tmp, const char* path);

// Before reading `path`: only `tmp` left means a replace lost power after the remove, so `tmp`
// becomes `path`. A tmp beside `path` is a write cut short and is removed.
void recover(const char* tmp, const char* path);

}  // namespace x4::statefile
