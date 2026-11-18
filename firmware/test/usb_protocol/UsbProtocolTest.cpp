#include "usb/UsbProtocol.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace usb;

namespace {

uint32_t crcOf(const std::string& s) { return crc32(0, reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

// Parses `json` into a document that lives as long as the fixture. A 64-bit
// host takes 4 KB per ArduinoJson slot pool, four times the device's 1 KB.
class Args {
 public:
  explicit Args(const char* json) : arena(buffer, sizeof(buffer)), doc(&arena) {
    EXPECT_EQ(deserializeJson(doc, json), DeserializationError::Ok);
  }
  JsonObjectConst get() const { return doc.as<JsonObjectConst>(); }

 private:
  uint8_t buffer[8192];
  ArenaAllocator arena;
  JsonDocument doc;
};

struct CollectSink final : ReplySink {
  std::vector<std::string> lines;
  void send(const char* line, size_t len) override { lines.emplace_back(line, len); }
};

struct FakeTarget final : UploadTarget {
  std::string bytes;
  size_t failAfter = SIZE_MAX;  // write() fails once this many bytes are stored
  Status commitStatus;
  const char* okJson = "{}";
  int commits = 0;
  int aborts = 0;

  bool write(const uint8_t* data, size_t len) override {
    if (bytes.size() + len > failAfter) return false;
    bytes.append(reinterpret_cast<const char*>(data), len);
    return true;
  }
  Status commit(char* json, size_t cap) override {
    ++commits;
    if (commitStatus.ok()) snprintf(json, cap, "%s", okJson);
    return commitStatus;
  }
  void abort() override { ++aborts; }
};

std::string pattern(size_t n) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>(i * 31 + 7);
  return s;
}

void feedString(Upload& upload, const std::string& data, size_t piece, uint32_t now = 0) {
  for (size_t at = 0; at < data.size(); at += piece) {
    const size_t n = std::min(piece, data.size() - at);
    upload.feed(reinterpret_cast<const uint8_t*>(data.data() + at), n, now);
  }
}

}  // namespace

TEST(Crc32, MatchesZlib) {
  EXPECT_EQ(crcOf(""), 0u);
  EXPECT_EQ(crcOf("123456789"), 0xCBF43926u);
  const std::string a = "hello ", b = "world";
  const uint32_t part = crcOf(a);
  EXPECT_EQ(crc32(part, reinterpret_cast<const uint8_t*>(b.data()), b.size()), crcOf(a + b));
}

TEST(LineReader, SplitsLinesAndStripsCr) {
  LineReader r;
  const char* input = "X4 1 hello {}\r\nX4 2 bye {}\n";
  std::vector<std::string> lines;
  for (const char* p = input; *p; ++p) {
    if (r.push(*p) == LineReader::Result::Line) lines.emplace_back(r.line());
  }
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], "X4 1 hello {}");
  EXPECT_EQ(lines[1], "X4 2 bye {}");
}

TEST(LineReader, ReportsOverflowOnceAndRecovers) {
  LineReader r;
  std::string longLine = "X4 7 put {\"path\":\"/" + std::string(kMaxLine, 'a') + "\"}";
  LineReader::Result last = LineReader::Result::Partial;
  for (char c : longLine) EXPECT_EQ(r.push(c), LineReader::Result::Partial);
  last = r.push('\n');
  EXPECT_EQ(last, LineReader::Result::Overflow);
  EXPECT_EQ(std::string(r.line()).substr(0, 9), "X4 7 put ");
  EXPECT_EQ(strlen(r.line()), kMaxLine);

  for (char c : std::string("X4 8 bye {}")) r.push(c);
  EXPECT_EQ(r.push('\n'), LineReader::Result::Line);
  EXPECT_STREQ(r.line(), "X4 8 bye {}");
}

TEST(LineReader, AcceptsExactlyMaxLength) {
  LineReader r;
  for (size_t i = 0; i < kMaxLine; ++i) r.push('x');
  EXPECT_EQ(r.push('\n'), LineReader::Result::Line);
}

TEST(ParseRequest, ReadsIdCommandAndArgs) {
  Request req;
  ASSERT_EQ(parseRequest("X4 42 get {\"path\":\"/a\"}", req), Parse::Ok);
  EXPECT_EQ(req.id, 42u);
  EXPECT_STREQ(req.command, "get");
  EXPECT_STREQ(req.args, "{\"path\":\"/a\"}");
}

TEST(ParseRequest, MissingArgsReadAsEmptyObject) {
  Request req;
  ASSERT_EQ(parseRequest("X4 3 bye", req), Parse::Ok);
  EXPECT_STREQ(req.args, "{}");
}

TEST(ParseRequest, IgnoresLinesForOthers) {
  Request req;
  EXPECT_EQ(parseRequest("[123] [MEM] Free: 1", req), Parse::NotForUs);
  EXPECT_EQ(parseRequest("", req), Parse::NotForUs);
  EXPECT_EQ(parseRequest("X41 hello", req), Parse::NotForUs);
}

TEST(ParseRequest, RejectsMalformedLinesKeepingTheId) {
  Request req;
  EXPECT_EQ(parseRequest("X4 abc hello {}", req), Parse::Bad);
  EXPECT_EQ(req.id, 0u);
  EXPECT_EQ(parseRequest("X4 4294967296 hello {}", req), Parse::Bad);
  EXPECT_EQ(parseRequest("X4 5 HELLO {}", req), Parse::Bad);
  EXPECT_EQ(req.id, 5u);
  EXPECT_EQ(parseRequest("X4 6 averyverylongcommand {}", req), Parse::Bad);
  EXPECT_EQ(req.id, 6u);
  EXPECT_EQ(parseRequest("X4 7", req), Parse::Bad);
}

TEST(ValidPath, AcceptsAbsolutePaths) {
  EXPECT_TRUE(validPath("/"));
  EXPECT_TRUE(validPath("/x4/decks/stoicism.jsonl"));
  EXPECT_TRUE(validPath("/books/.hidden"));
  EXPECT_TRUE(validPath(("/" + std::string(kMaxPath - 1, 'a')).c_str()));
}

TEST(ValidPath, RejectsEscapesAndJunk) {
  EXPECT_FALSE(validPath(nullptr));
  EXPECT_FALSE(validPath(""));
  EXPECT_FALSE(validPath("x4/a"));
  EXPECT_FALSE(validPath("/x4/../etc"));
  EXPECT_FALSE(validPath("/.."));
  EXPECT_FALSE(validPath("/./a"));
  EXPECT_FALSE(validPath("/a\\b"));
  EXPECT_FALSE(validPath("/a\nb"));
  EXPECT_FALSE(validPath(("/" + std::string(kMaxPath, 'a')).c_str()));
}

TEST(ValidPath, RejectsEmptyComponents) {
  EXPECT_FALSE(validPath("//"));
  EXPECT_FALSE(validPath("//x4"));
  EXPECT_FALSE(validPath("/x4//decks"));
  EXPECT_FALSE(validPath("/x4/"));
  EXPECT_FALSE(validPath("/x4/decks/."));
  EXPECT_FALSE(validPath("/x4/decks/.."));
  EXPECT_TRUE(validPath("/x4/..a"));
}

TEST(PathCompare, SameEntryIgnoresAsciiCase) {
  EXPECT_TRUE(samePathIgnoringCase("/x4/Stream.jsonl", "/X4/stream.JSONL"));
  EXPECT_TRUE(samePathIgnoringCase("/a", "/a"));
  EXPECT_FALSE(samePathIgnoringCase("/a", "/ab"));
  EXPECT_FALSE(samePathIgnoringCase("/ab", "/a"));
  EXPECT_FALSE(samePathIgnoringCase("/a/b", "/a_b"));
}

TEST(PathCompare, InsideMeansBelowTheFolder) {
  EXPECT_TRUE(isInsideIgnoringCase("/books/a/b.epub", "/books"));
  EXPECT_TRUE(isInsideIgnoringCase("/Books/a", "/books"));
  EXPECT_FALSE(isInsideIgnoringCase("/books", "/books"));
  EXPECT_FALSE(isInsideIgnoringCase("/BOOKS", "/books"));
  EXPECT_FALSE(isInsideIgnoringCase("/books2/a", "/books"));
  EXPECT_FALSE(isInsideIgnoringCase("/boo", "/books"));
  EXPECT_TRUE(isInsideIgnoringCase("/x4", "/"));
  EXPECT_FALSE(isInsideIgnoringCase("/", "/"));
}

TEST(Args, Put) {
  PutArgs put;
  EXPECT_TRUE(parsePutArgs(Args(R"({"path":"/x4/a.jsonl","size":9000,"crc32":3735928559})").get(), put).ok());
  EXPECT_STREQ(put.path, "/x4/a.jsonl");
  EXPECT_EQ(put.size, 9000u);
  EXPECT_EQ(put.crc32, 3735928559u);

  EXPECT_EQ(parsePutArgs(Args(R"({"path":"/a","size":1})").get(), put).code, Error::BadRequest);
  EXPECT_EQ(parsePutArgs(Args(R"({"path":"/a","size":-1,"crc32":0})").get(), put).code, Error::BadRequest);
  EXPECT_EQ(parsePutArgs(Args(R"({"path":"/a","size":1,"crc32":4294967296})").get(), put).code, Error::BadRequest);
  EXPECT_EQ(parsePutArgs(Args(R"({"path":"../a","size":1,"crc32":0})").get(), put).code, Error::BadRequest);
  const std::string longPath = R"({"path":"/)" + std::string(kMaxPath - 3, 'a') + R"(","size":1,"crc32":0})";
  EXPECT_EQ(parsePutArgs(Args(longPath.c_str()).get(), put).code, Error::BadRequest);
}

TEST(Args, Get) {
  GetArgs get;
  ASSERT_TRUE(parseGetArgs(Args(R"({"path":"/a"})").get(), get).ok());
  EXPECT_EQ(get.offset, 0u);
  EXPECT_TRUE(get.toEnd);
  ASSERT_TRUE(parseGetArgs(Args(R"({"path":"/a","offset":10,"length":null})").get(), get).ok());
  EXPECT_EQ(get.offset, 10u);
  EXPECT_TRUE(get.toEnd);
  ASSERT_TRUE(parseGetArgs(Args(R"({"path":"/a","offset":0,"length":5})").get(), get).ok());
  EXPECT_FALSE(get.toEnd);
  EXPECT_EQ(get.length, 5u);
  EXPECT_EQ(parseGetArgs(Args(R"({"path":"/a","offset":1.5})").get(), get).code, Error::BadRequest);
  EXPECT_EQ(parseGetArgs(Args(R"({})").get(), get).code, Error::BadRequest);
}

TEST(Args, Time) {
  TimeArgs t;
  ASSERT_TRUE(parseTimeArgs(Args(R"({"time":1793714400,"tz":60})").get(), true, t).ok());
  EXPECT_TRUE(t.present);
  EXPECT_EQ(t.time, 1793714400);
  EXPECT_EQ(t.tzMinutes, 60);
  ASSERT_TRUE(parseTimeArgs(Args(R"({})").get(), false, t).ok());
  EXPECT_FALSE(t.present);
  EXPECT_EQ(parseTimeArgs(Args(R"({})").get(), true, t).code, Error::BadRequest);
  EXPECT_EQ(parseTimeArgs(Args(R"({"time":1000})").get(), true, t).code, Error::BadRequest);
  EXPECT_EQ(parseTimeArgs(Args(R"({"time":1793714400,"tz":2000})").get(), true, t).code, Error::BadRequest);
  EXPECT_EQ(parseTimeArgs(Args(R"({"time":"now"})").get(), true, t).code, Error::BadRequest);
}

TEST(HostClock, TimeWithoutTzKeepsTheLastOffset) {
  HostClock clock;
  TimeArgs t;
  ASSERT_TRUE(parseTimeArgs(Args(R"({})").get(), false, t).ok());
  clock.apply(t);
  EXPECT_FALSE(clock.set);

  ASSERT_TRUE(parseTimeArgs(Args(R"({"time":1793714400})").get(), true, t).ok());
  EXPECT_FALSE(t.tzPresent);
  clock.apply(t);
  EXPECT_TRUE(clock.set);
  EXPECT_FALSE(clock.tzSet);

  ASSERT_TRUE(parseTimeArgs(Args(R"({"time":1793714400,"tz":-300})").get(), true, t).ok());
  EXPECT_TRUE(t.tzPresent);
  clock.apply(t);
  EXPECT_TRUE(clock.tzSet);
  EXPECT_EQ(clock.tzMinutes, -300);

  ASSERT_TRUE(parseTimeArgs(Args(R"({"time":1793718000})").get(), true, t).ok());
  clock.apply(t);
  EXPECT_EQ(clock.tzMinutes, -300);

  ASSERT_TRUE(parseTimeArgs(Args(R"({"time":1793718000,"tz":0})").get(), true, t).ok());
  clock.apply(t);
  EXPECT_EQ(clock.tzMinutes, 0);
}

TEST(Args, RmAndMv) {
  RmArgs rm;
  ASSERT_TRUE(parseRmArgs(Args(R"({"path":"/x4/tmp","recursive":true})").get(), rm).ok());
  EXPECT_TRUE(rm.recursive);
  ASSERT_TRUE(parseRmArgs(Args(R"({"path":"/x4/tmp"})").get(), rm).ok());
  EXPECT_FALSE(rm.recursive);
  EXPECT_EQ(parseRmArgs(Args(R"({"path":"/"})").get(), rm).code, Error::BadRequest);
  EXPECT_EQ(parseRmArgs(Args(R"({"path":"/a","recursive":1})").get(), rm).code, Error::BadRequest);

  MvArgs mv;
  ASSERT_TRUE(parseMvArgs(Args(R"({"from":"/x4/log.jsonl","to":"/x4/log.jsonl.pulling"})").get(), mv).ok());
  EXPECT_STREQ(mv.to, "/x4/log.jsonl.pulling");
  EXPECT_EQ(parseMvArgs(Args(R"({"from":"/a"})").get(), mv).code, Error::BadRequest);
  EXPECT_EQ(parseMvArgs(Args(R"({"from":"/","to":"/b"})").get(), mv).code, Error::BadRequest);
}

TEST(Args, Press) {
  PressArgs press;
  ASSERT_TRUE(parsePressArgs(Args(R"({"button":"confirm"})").get(), press).ok());
  EXPECT_EQ(press.button, 1);
  EXPECT_FALSE(press.longPress);
  ASSERT_TRUE(parsePressArgs(Args(R"({"button":"power","long":true})").get(), press).ok());
  EXPECT_EQ(press.button, 6);
  EXPECT_TRUE(press.longPress);
  EXPECT_EQ(parsePressArgs(Args(R"({"button":"home"})").get(), press).code, Error::BadRequest);
  EXPECT_EQ(parsePressArgs(Args(R"({})").get(), press).code, Error::BadRequest);
}

TEST(Args, Flash) {
  FlashArgs flash;
  const std::string hex(64, 'a');
  const std::string ok = R"({"size":1048576,"sha256":")" + hex + R"("})";
  ASSERT_TRUE(parseFlashArgs(Args(ok.c_str()).get(), flash).ok());
  EXPECT_EQ(flash.size, 1048576u);
  EXPECT_EQ(flash.sha256[0], 0xAA);
  EXPECT_EQ(flash.sha256[31], 0xAA);
  const std::string shortSha = R"({"size":10,"sha256":"abcd"})";
  EXPECT_EQ(parseFlashArgs(Args(shortSha.c_str()).get(), flash).code, Error::BadRequest);
  const std::string badHex = R"({"size":10,"sha256":")" + std::string(63, 'a') + R"(g"})";
  EXPECT_EQ(parseFlashArgs(Args(badHex.c_str()).get(), flash).code, Error::BadRequest);
  const std::string empty = R"({"size":0,"sha256":")" + hex + R"("})";
  EXPECT_EQ(parseFlashArgs(Args(empty.c_str()).get(), flash).code, Error::BadRequest);
}

TEST(Arena, FailsCleanlyWhenFull) {
  uint8_t small[64];
  ArenaAllocator arena(small, sizeof(small));
  JsonDocument doc(&arena);
  EXPECT_EQ(deserializeJson(doc, R"({"path":"/a/rather/long/path/that/will/not/fit/in/the/arena"})"),
            DeserializationError::NoMemory);
}

TEST(Arena, ResetReclaimsSpace) {
  uint8_t buffer[8192];
  ArenaAllocator arena(buffer, sizeof(buffer));
  for (int i = 0; i < 50; ++i) {
    arena.reset();
    JsonDocument doc(&arena);
    ASSERT_EQ(deserializeJson(doc, R"({"path":"/x4/decks/a.jsonl","size":12,"crc32":7})"), DeserializationError::Ok);
    EXPECT_STREQ(doc["path"].as<const char*>(), "/x4/decks/a.jsonl");
  }
}

TEST(Format, ErrorLine) {
  char buf[160];
  const size_t n = formatError(buf, sizeof(buf), 9, fail(Error::NotFound, "no such path"));
  EXPECT_EQ(std::string(buf, n), "X4 9 err {\"code\":\"not_found\",\"message\":\"no such path\"}\n");
}

TEST(Format, TruncatedReplyStillEndsTheLine) {
  char buf[16];
  const size_t n = formatReply(buf, sizeof(buf), 1, "ok", "{\"a\":\"long value\"}");
  EXPECT_EQ(n, 15u);
  EXPECT_EQ(buf[n - 1], '\n');
}

TEST(Upload, ProtocolExample) {
  CollectSink sink;
  FakeTarget target;
  Upload upload;
  const std::string data = pattern(9000);
  upload.begin(3, data.size(), true, crcOf(data), target, sink, 0);
  feedString(upload, data, 4096);
  EXPECT_FALSE(upload.active());
  EXPECT_EQ(sink.lines, (std::vector<std::string>{
                            "X4 3 ready {\"chunk\":4096}\n",
                            "X4 3 ack {\"n\":4096}\n",
                            "X4 3 ack {\"n\":8192}\n",
                            "X4 3 ack {\"n\":9000}\n",
                            "X4 3 ok {}\n",
                        }));
  EXPECT_EQ(target.bytes, data);
  EXPECT_EQ(target.commits, 1);
  EXPECT_EQ(target.aborts, 0);
  EXPECT_TRUE(upload.takeCommitted());
  EXPECT_FALSE(upload.takeCommitted());
}

TEST(Upload, AcksAtChunkBoundariesWhateverTheReadSize) {
  CollectSink sink;
  FakeTarget target;
  Upload upload;
  const std::string data = pattern(kChunk + 10);
  upload.begin(4, data.size(), true, crcOf(data), target, sink, 0);
  feedString(upload, data, 7);
  ASSERT_EQ(sink.lines.size(), 4u);
  EXPECT_EQ(sink.lines[1], "X4 4 ack {\"n\":4096}\n");
  EXPECT_EQ(sink.lines[2], "X4 4 ack {\"n\":4106}\n");
  EXPECT_EQ(sink.lines[3], "X4 4 ok {}\n");
  EXPECT_EQ(target.bytes, data);
}

TEST(Upload, EmptyFileSkipsAcks) {
  CollectSink sink;
  FakeTarget target;
  Upload upload;
  upload.begin(5, 0, true, 0, target, sink, 0);
  EXPECT_FALSE(upload.active());
  EXPECT_EQ(sink.lines, (std::vector<std::string>{"X4 5 ready {\"chunk\":4096}\n", "X4 5 ok {}\n"}));
  EXPECT_EQ(target.commits, 1);
}

TEST(Upload, CrcMismatchAbortsAfterTheLastAck) {
  CollectSink sink;
  FakeTarget target;
  Upload upload;
  const std::string data = pattern(100);
  upload.begin(6, data.size(), true, crcOf(data) ^ 1, target, sink, 0);
  feedString(upload, data, 100);
  ASSERT_EQ(sink.lines.size(), 3u);
  EXPECT_EQ(sink.lines[1], "X4 6 ack {\"n\":100}\n");
  EXPECT_EQ(sink.lines[2], "X4 6 err {\"code\":\"crc\",\"message\":\"crc mismatch\"}\n");
  EXPECT_EQ(target.commits, 0);
  EXPECT_EQ(target.aborts, 1);
  EXPECT_FALSE(upload.takeCommitted());
}

TEST(Upload, WriteFailureDrainsTheChunkThenReplacesItsAck) {
  CollectSink sink;
  FakeTarget target;
  target.failAfter = 5000;
  Upload upload;
  const std::string data = pattern(9000);
  upload.begin(7, data.size(), true, crcOf(data), target, sink, 0);
  feedString(upload, data.substr(0, 4096), 512);
  EXPECT_EQ(sink.lines.back(), "X4 7 ack {\"n\":4096}\n");
  feedString(upload, data.substr(4096, 4096), 512);
  EXPECT_FALSE(upload.active());
  EXPECT_EQ(sink.lines.back(), "X4 7 err {\"code\":\"io\",\"message\":\"write failed\"}\n");
  EXPECT_EQ(sink.lines.size(), 3u);
  EXPECT_EQ(target.aborts, 1);
}

TEST(Upload, StallAbortsWithIo) {
  CollectSink sink;
  FakeTarget target;
  Upload upload;
  const std::string data = pattern(5000);
  upload.begin(8, data.size(), true, crcOf(data), target, sink, 1000);
  feedString(upload, data.substr(0, 4096), 4096, 2000);
  upload.tick(2000 + kStallMs - 1);
  EXPECT_TRUE(upload.active());
  upload.tick(2000 + kStallMs);
  EXPECT_FALSE(upload.active());
  EXPECT_EQ(sink.lines.back(), "X4 8 err {\"code\":\"io\",\"message\":\"upload stalled\"}\n");
  EXPECT_EQ(target.aborts, 1);
}

TEST(Upload, CommitFailureIsReportedWithoutAbort) {
  CollectSink sink;
  FakeTarget target;
  target.commitStatus = fail(Error::Crc, "sha256 mismatch");
  Upload upload;
  const std::string data = pattern(10);
  upload.begin(9, data.size(), false, 0, target, sink, 0);
  feedString(upload, data, 10);
  EXPECT_EQ(sink.lines.back(), "X4 9 err {\"code\":\"crc\",\"message\":\"sha256 mismatch\"}\n");
  EXPECT_EQ(target.aborts, 0);
}

TEST(Upload, FlashReplyCarriesTheSlot) {
  CollectSink sink;
  FakeTarget target;
  target.okJson = "{\"slot\":\"ota_1\"}";
  Upload upload;
  const std::string data = pattern(10);
  upload.begin(10, data.size(), false, 0, target, sink, 0);
  feedString(upload, data, 10);
  EXPECT_EQ(sink.lines.back(), "X4 10 ok {\"slot\":\"ota_1\"}\n");
}

TEST(Upload, IgnoresBytesPastTheAnnouncedSize) {
  CollectSink sink;
  FakeTarget target;
  Upload upload;
  const std::string data = pattern(10);
  upload.begin(11, 10, true, crcOf(data), target, sink, 0);
  feedString(upload, data + "X4 12 bye {}\n", 64);
  EXPECT_EQ(target.bytes, data);
  EXPECT_EQ(sink.lines.back(), "X4 11 ok {}\n");
}

TEST(Upload, CancelAbortsSilently) {
  CollectSink sink;
  FakeTarget target;
  Upload upload;
  upload.begin(12, 100, true, 0, target, sink, 0);
  upload.cancel();
  EXPECT_FALSE(upload.active());
  EXPECT_EQ(sink.lines.size(), 1u);
  EXPECT_EQ(target.aborts, 1);
}
