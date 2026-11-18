#include "DictionaryRegistry.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "StringUtils.h"
#include "learn/LearnTypes.h"

namespace DictionaryRegistry {
namespace {

// Dictionaries are looked up in both roots, in order. The hidden variant
// lets users keep the folder out of the file browser (hidden by default,
// see FileBrowserActivity's showHiddenFiles check).
constexpr const char* DICT_ROOTS[] = {"/dictionaries", "/.dictionaries"};
// Per-book glossaries written by the host (docs/DATA.md).
constexpr char GLOSSARY_ROOT[] = "/x4/glossary";

// Find the single .idx stem inside one dictionary folder. Returns false when
// the folder holds no .idx or more than one distinct stem (ambiguous).
bool findStem(const char* folderPath, std::string& stemOut) {
  auto dir = Storage.open(folderPath);
  if (!dir || !dir.isDirectory()) return false;

  dir.rewindDirectory();
  char name[128];
  char foundStem[128];
  foundStem[0] = '\0';
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    entry.getName(name, sizeof(name));
    // Skip macOS metadata files (AppleDouble resource forks)
    if (entry.isDirectory() || strncmp(name, "._", 2) == 0) continue;

    const size_t len = strlen(name);
    if (len <= 4 || strcmp(name + len - 4, ".idx") != 0) continue;

    name[len - 4] = '\0';
    if (foundStem[0] != '\0' && strcmp(foundStem, name) != 0) {
      LOG_DBG("DREG", "Skipping %s: multiple index stems found", folderPath);
      return false;
    }
    strncpy(foundStem, name, sizeof(foundStem) - 1);
    foundStem[sizeof(foundStem) - 1] = '\0';
  }

  if (foundStem[0] == '\0') return false;

  // Require dictionary data next to the index, so folders holding only an
  // .idx never surface as selectable dictionaries that fail at lookup time.
  const std::string base = std::string(folderPath) + "/" + foundStem;
  if (!Storage.exists((base + ".dict").c_str()) && !Storage.exists((base + ".dict.dz").c_str())) {
    LOG_DBG("DREG", "Skipping %s: no .dict or .dict.dz", folderPath);
    return false;
  }

  stemOut = foundStem;
  return true;
}

}  // namespace

void discover(std::vector<DictionaryEntry>& out) {
  out.clear();
  out.reserve(8);

  for (const char* dictRoot : DICT_ROOTS) {
    auto rootDir = Storage.open(dictRoot);
    if (!rootDir || !rootDir.isDirectory()) {
      LOG_DBG("DREG", "No %s directory on SD card", dictRoot);
      continue;
    }

    rootDir.rewindDirectory();
    char name[128];
    for (auto entry = rootDir.openNextFile(); entry; entry = rootDir.openNextFile()) {
      entry.getName(name, sizeof(name));
      if (!entry.isDirectory() || name[0] == '.') continue;

      std::string folderPath = std::string(dictRoot) + "/" + name;
      std::string stem;
      if (!findStem(folderPath.c_str(), stem)) continue;

      DictionaryEntry e;
      e.name = name;
      e.stem = std::move(stem);
      out.push_back(std::move(e));
      LOG_DBG("DREG", "Found dictionary: %s", name);
    }
  }

  // Case-insensitive sort by folder name (matches FileBrowserActivity ordering).
  std::sort(out.begin(), out.end(), [](const DictionaryEntry& a, const DictionaryEntry& b) {
    return StringUtils::asciiCaseCmp(a.name.c_str(), b.name.c_str()) < 0;
  });
}

bool resolveBasePath(const char* folderName, std::string& basePathOut) {
  if (!folderName || folderName[0] == '\0') return false;
  // folderName is persisted in the settings JSON: reject separators and dot
  // prefixes so a crafted value cannot escape the dictionary roots.
  if (folderName[0] == '.' || strpbrk(folderName, "/\\") != nullptr) return false;

  for (const char* dictRoot : DICT_ROOTS) {
    std::string folderPath = std::string(dictRoot) + "/" + folderName;
    std::string stem;
    if (!findStem(folderPath.c_str(), stem)) continue;
    basePathOut = folderPath + "/" + stem;
    return true;
  }
  return false;
}

void lookupChain(const char* bookPath, std::vector<std::string>& basePathsOut) {
  basePathsOut.clear();
  std::string base;
  if (bookPath && *bookPath) {
    // The folder is a hash of the book key, so a long book name never overflows the path limit.
    const char* key = *bookPath == '/' ? bookPath + 1 : bookPath;
    char folder[sizeof(GLOSSARY_ROOT) + 17];
    snprintf(folder, sizeof(folder), "%s/%016llx", GLOSSARY_ROOT,
             static_cast<unsigned long long>(x4::fnv1a64(key, strlen(key))));
    std::string stem;
    if (findStem(folder, stem)) basePathsOut.push_back(std::string(folder) + "/" + stem);
  }
#if X4R
  std::vector<DictionaryEntry> dictionaries;
  discover(dictionaries);
  for (const auto& dictionary : dictionaries) {
    if (resolveBasePath(dictionary.name.c_str(), base)) basePathsOut.push_back(std::move(base));
  }
#else
  if (resolveBasePath(SETTINGS.dictionaryName, base)) basePathsOut.push_back(std::move(base));
#endif
}

bool hasLookupSources() {
#if X4R
  if (Storage.exists(GLOSSARY_ROOT)) return true;
  for (const char* dictRoot : DICT_ROOTS) {
    if (Storage.exists(dictRoot)) return true;
  }
  return false;
#else
  return SETTINGS.dictionaryName[0] != '\0';
#endif
}

}  // namespace DictionaryRegistry
