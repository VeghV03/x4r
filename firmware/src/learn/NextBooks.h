#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Books Claude picked at sync to follow a book, for its end-of-book menu: /x4/next.jsonl.
namespace x4::nextbooks {

constexpr size_t MAX = 3;

struct Pick {
  std::string path;  // with the leading '/'
  std::string title;
};

// The picks for `bookPath` that are on the card, in order; false when there are none.
bool load(const char* bookPath, std::vector<Pick>& out);

}  // namespace x4::nextbooks
