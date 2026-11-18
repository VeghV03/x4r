#pragma once

#include <Arduino.h>
#include <fcntl.h>

#include <cstdint>
#include <cstdio>
#include <string>

using oflag_t = int;

// The SD card as folders on the Mac, read-only. A card path is served from the folder mounted at
// its longest matching prefix: mount("/fonts", "build/fonts") makes /fonts/Literata/... readable.
// Paths under no mount do not exist.
class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(HalFile&& other) noexcept;
  HalFile& operator=(HalFile&& other) noexcept;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  size_t getName(char* name, size_t len);
  size_t size() { return fileSize(); }
  size_t fileSize();
  uint64_t fileSize64() { return fileSize(); }
  bool seek(size_t pos) { return seekSet(pos); }
  bool seekCur(int64_t offset);
  bool seekSet(size_t offset);
  int available() const;
  size_t position() const;
  int read(void* buf, size_t count);
  int read();
  bool isDirectory() const { return dir_ != nullptr; }
  void rewindDirectory();
  bool close();
  HalFile openNextFile();
  bool isOpen() const { return file_ != nullptr || dir_ != nullptr; }
  operator bool() const { return isOpen(); }

 private:
  friend class HalStorage;
  bool openHostPath(const std::string& hostPath);

  FILE* file_ = nullptr;
  void* dir_ = nullptr;  // DIR*
  std::string hostPath_;
};

class HalStorage {
 public:
  void mount(const std::string& cardPrefix, const std::string& hostFolder);
  // Empty when no mount covers the path.
  std::string hostPath(const char* cardPath) const;

  HalFile open(const char* path, oflag_t oflag = O_RDONLY);
  bool exists(const char* path);
  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
    return openFileForRead(moduleName, path.c_str(), file);
  }

  static HalStorage& getInstance();
};

#define Storage HalStorage::getInstance()
