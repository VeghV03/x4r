#pragma once

#include <string>
#include <vector>

// One StarDict dictionary found under /dictionaries or /.dictionaries: a
// subfolder holding <stem>.idx plus <stem>.dict or <stem>.dict.dz.
struct DictionaryEntry {
  std::string name;  // subfolder name (shown to the user, stored in settings)
  std::string stem;  // index basename without .idx
};

namespace DictionaryRegistry {

// Scan /dictionaries/*/ and /.dictionaries/*/ for dictionaries. Folders with
// multiple index stems are ambiguous and skipped. Result is sorted
// case-insensitively by name.
void discover(std::vector<DictionaryEntry>& out);

// Resolve a folder name to its extensionless base path
// ("/dictionaries/<folder>/<stem>" or "/.dictionaries/<folder>/<stem>").
// Returns false if the folder holds no usable dictionary in either root.
bool resolveBasePath(const char* folderName, std::string& basePathOut);

// The base paths a lookup while reading `bookPath` tries, in order: the book's
// glossary (/x4/glossary/<FNV-1a 64 of the book key, 16 hex>/), then on x4r every
// dictionary in name order, else the dictionary chosen in Settings.
void lookupChain(const char* bookPath, std::vector<std::string>& basePathsOut);

// Cheap check for showing Look Up: on x4r a glossary or dictionary folder
// exists, else Settings chose a dictionary.
bool hasLookupSources();

}  // namespace DictionaryRegistry
