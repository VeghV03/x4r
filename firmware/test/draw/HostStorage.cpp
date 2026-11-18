#include <HalStorage.h>
#include <dirent.h>
#include <sys/stat.h>

#include <map>
#include <utility>

namespace {

std::map<std::string, std::string>& mounts() {
  static std::map<std::string, std::string> table;
  return table;
}

bool isDirectoryPath(const std::string& hostPath) {
  struct stat info {};
  return stat(hostPath.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

}  // namespace

// ---- HalFile -------------------------------------------------------------------

HalFile::HalFile(HalFile&& other) noexcept
    : file_(other.file_), dir_(other.dir_), hostPath_(std::move(other.hostPath_)) {
  other.file_ = nullptr;
  other.dir_ = nullptr;
}

HalFile& HalFile::operator=(HalFile&& other) noexcept {
  if (this == &other) return *this;
  close();
  file_ = other.file_;
  dir_ = other.dir_;
  hostPath_ = std::move(other.hostPath_);
  other.file_ = nullptr;
  other.dir_ = nullptr;
  return *this;
}

bool HalFile::openHostPath(const std::string& hostPath) {
  close();
  if (isDirectoryPath(hostPath)) {
    dir_ = opendir(hostPath.c_str());
  } else {
    file_ = fopen(hostPath.c_str(), "rb");
  }
  hostPath_ = hostPath;
  return isOpen();
}

bool HalFile::close() {
  if (file_) fclose(file_);
  if (dir_) closedir(static_cast<DIR*>(dir_));
  file_ = nullptr;
  dir_ = nullptr;
  return true;
}

size_t HalFile::getName(char* name, const size_t len) {
  const size_t slash = hostPath_.find_last_of('/');
  const std::string base = slash == std::string::npos ? hostPath_ : hostPath_.substr(slash + 1);
  if (len > 0) snprintf(name, len, "%s", base.c_str());
  return base.size();
}

size_t HalFile::fileSize() {
  if (!file_) return 0;
  struct stat info {};
  return fstat(fileno(file_), &info) == 0 ? static_cast<size_t>(info.st_size) : 0;
}

bool HalFile::seekSet(const size_t offset) {
  return file_ && offset <= fileSize() && fseek(file_, static_cast<long>(offset), SEEK_SET) == 0;
}

bool HalFile::seekCur(const int64_t offset) { return file_ && fseek(file_, static_cast<long>(offset), SEEK_CUR) == 0; }

size_t HalFile::position() const { return file_ ? static_cast<size_t>(ftell(file_)) : 0; }

int HalFile::available() const {
  if (!file_) return 0;
  const size_t size = const_cast<HalFile*>(this)->fileSize();
  const size_t at = position();
  return at < size ? static_cast<int>(size - at) : 0;
}

int HalFile::read(void* buf, const size_t count) {
  if (!file_) return -1;
  return static_cast<int>(fread(buf, 1, count, file_));
}

int HalFile::read() {
  uint8_t byte = 0;
  return read(&byte, 1) == 1 ? byte : -1;
}

void HalFile::rewindDirectory() {
  if (dir_) rewinddir(static_cast<DIR*>(dir_));
}

HalFile HalFile::openNextFile() {
  HalFile next;
  if (!dir_) return next;
  while (const dirent* entry = readdir(static_cast<DIR*>(dir_))) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    if (next.openHostPath(hostPath_ + "/" + name)) break;
  }
  return next;
}

// ---- HalStorage ----------------------------------------------------------------

HalStorage& HalStorage::getInstance() {
  static HalStorage instance;
  return instance;
}

void HalStorage::mount(const std::string& cardPrefix, const std::string& hostFolder) {
  mounts()[cardPrefix] = hostFolder;
}

std::string HalStorage::hostPath(const char* cardPath) const {
  const std::string path = cardPath ? cardPath : "";
  const std::string* bestPrefix = nullptr;
  const std::string* bestFolder = nullptr;
  for (const auto& [prefix, folder] : mounts()) {
    if (path.compare(0, prefix.size(), prefix) != 0) continue;
    // "/fonts" covers "/fonts/x", not "/fontsize".
    if (path.size() != prefix.size() && prefix != "/" && path[prefix.size()] != '/') continue;
    if (!bestPrefix || prefix.size() > bestPrefix->size()) {
      bestPrefix = &prefix;
      bestFolder = &folder;
    }
  }
  if (!bestPrefix) return "";
  std::string rest = path.substr(bestPrefix->size());
  if (!rest.empty() && rest.front() != '/') rest.insert(rest.begin(), '/');
  return *bestFolder + rest;
}

HalFile HalStorage::open(const char* path, oflag_t) {
  HalFile file;
  const std::string host = hostPath(path);
  if (!host.empty()) file.openHostPath(host);
  return file;
}

bool HalStorage::exists(const char* path) {
  const std::string host = hostPath(path);
  struct stat info {};
  return !host.empty() && stat(host.c_str(), &info) == 0;
}

bool HalStorage::openFileForRead(const char*, const char* path, HalFile& file) {
  const std::string host = hostPath(path);
  return !host.empty() && !isDirectoryPath(host) && file.openHostPath(host);
}
