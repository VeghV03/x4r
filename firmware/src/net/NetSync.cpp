#include "util/HeapLows.h"
#include "NetSync.h"

#include <Arduino.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <TrustedTime.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_netif.h>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <lwip/sockets.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <new>
#include <optional>
#include <string>
#include <vector>

#include "Issue.h"
#include "Upload.h"
#include "WifiCredentialStore.h"
#include "learn/Learn.h"
#include "usb/UsbHooks.h"
#include "usb/UsbProtocol.h"

namespace x4::net {

namespace {

constexpr size_t kCalendars = 4;
constexpr size_t kUrl = 400;
constexpr size_t kError = 72;
constexpr size_t kNetworks = 4;
constexpr uint32_t kTaskStack = 10 * 1024;
constexpr uint32_t kConnectMs = 8000;   // per network
constexpr uint32_t kTimeMs = 10000;     // NTP
constexpr int kHttpTimeoutMs = 10000;   // per socket operation
constexpr int kMacTimeoutMs = 3000;     // the Mac on the LAN; away from home it does not answer
constexpr uint32_t kStopWaitMs = 4000;  // leaving Today
constexpr uint32_t kSleepBudgetMs = 25000;
constexpr uint32_t kFreshMs = 60UL * 60 * 1000;  // after a good fetch
constexpr uint32_t kRetryMs = 10UL * 60 * 1000;  // after a failed one
constexpr int64_t kSleepStale = 30 * 60;         // seconds
// Wi-Fi and one TLS session need about this much; below it the fetch waits.
constexpr uint32_t kMinFreeHeap = 100 * 1024;
constexpr uint32_t kMinBlock = 40 * 1024;
constexpr int64_t kDay = 86400;

// What goes up to the data repo's inbox (docs/DATA.md, "inbox/"). A log is sent from
// <log>.pulling, where a cable pull would look for it too.
constexpr const char* kLogs[] = {"highlights", "reviews", "seen", "play", "read-log", "lookups", "kept", "visits"};
constexpr char STATS_PATH[] = "/x4/stats.json";
constexpr char STATS_UP[] = "/x4/state/stats.up";
constexpr char TAKEN_UP[] = "/x4/state/taken.up";
constexpr char UP_STATE[] = "/x4/state/up.json";
constexpr uint32_t kUploadMax = 256 * 1024;  // a larger log waits for the cable

struct Config {
  bool hasZone = false;
  TzRule zone;
  uint8_t calendarCount = 0;
  char calendars[kCalendars][kUrl] = {};
  bool hasWeather = false;
  float latitude = 0;
  float longitude = 0;
  bool hasMac = false;
  char macUrl[96] = {};
  char macToken[72] = {};  // "Bearer <token>"
  bool hasRepo = false;
  char repoUrl[128] = {};    // https://api.github.com/repos/<owner>/<repo>/contents
  char repoToken[272] = {};  // "Bearer <token>"; GitHub asks for room for 255
};

struct Job {
  Config config;
  WifiCredential networks[kNetworks];
  uint8_t networkCount = 0;
  uint32_t deadline = 0;  // millis, 0 for none

  std::atomic<bool> cancel{false};
  std::atomic<bool> done{false};

  // Results, written by the task, read once done.
  char connected[33] = {};
  bool timeSynced = false;
  bool eventsOk = false;
  bool weatherOk = false;
  EventList events;
  Weather weather;
  bool movedHighlights = false;  // the highlights log was set aside to go up
  bool issueTaken = false;  // files from the Mac are in place
  bool issueWroteX4 = false;
  char issueBook[64] = {};  // a Read article it wrote or removed, for the Library
  char error[kError] = {};
};

Job* job = nullptr;
std::optional<HalPowerManager::Lock> powerLock;
uint32_t lastAttemptMs = 0;
bool attempted = false;
bool lastOk = false;
int64_t fetchedAt = 0;
char lastError[kError] = {};
uint32_t generationCount = 0;
bool storeLoaded = false;
// What a fetch that ended away from Today changed, for Today to take in.
bool reloadPending = false;
bool reloadStream = false;
char reloadBook[sizeof(Job::issueBook)] = {};

void ensureStore() {
  if (storeLoaded) return;
  WIFI_STORE.loadFromFile();
  storeLoaded = true;
}

bool readFile(const char* path, std::string& out, const size_t cap = 16 * 1024) {
  return Storage.exists(path) && Storage.readFileToString("NET", path, cap, out);
}

bool writeFileAtomically(const char* path, const std::string& body) {
  char tmp[64];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  HalFile f = Storage.open(tmp, O_RDWR | O_CREAT | O_TRUNC);
  if (!f) return false;
  const bool wrote = f.write(reinterpret_cast<const uint8_t*>(body.data()), body.size()) == body.size();
  if (!f.close() || !wrote) {
    Storage.remove(tmp);
    return false;
  }
  return Storage.replaceFile(tmp, path);
}

bool readConfig(Config& out) {
  out = Config{};
  std::string body;
  if (!readFile(CONFIG_PATH, body)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    LOG_ERR("NET", "%s does not parse", CONFIG_PATH);
    return false;
  }
  if (const char* tz = doc["tz"]) out.hasZone = parsePosixTz(tz, out.zone);
  for (JsonVariantConst c : doc["calendars"].as<JsonArrayConst>()) {
    const char* url = c.is<const char*>() ? c.as<const char*>() : c["url"].as<const char*>();
    if (!url || strncmp(url, "https://", 8) != 0 || strlen(url) >= kUrl || out.calendarCount >= kCalendars) continue;
    strcpy(out.calendars[out.calendarCount++], url);
  }
  JsonVariantConst w = doc["weather"];
  if (w["lat"].is<float>() && w["lon"].is<float>()) {
    out.hasWeather = true;
    out.latitude = w["lat"].as<float>();
    out.longitude = w["lon"].as<float>();
  }
  JsonVariantConst mac = doc["mac"];
  const char* macUrl = mac["url"];
  const char* token = mac["token"];
  if (macUrl && token && strncmp(macUrl, "http://", 7) == 0 && strlen(macUrl) < sizeof(out.macUrl) - 24 &&
      strlen(token) < sizeof(out.macToken) - 8) {
    out.hasMac = true;
    snprintf(out.macUrl, sizeof(out.macUrl), "%s", macUrl);
    const size_t n = strlen(out.macUrl);
    if (n && out.macUrl[n - 1] == '/') out.macUrl[n - 1] = 0;
    snprintf(out.macToken, sizeof(out.macToken), "Bearer %s", token);
  }
  JsonVariantConst repo = doc["repo"];
  const char* repoName = repo["name"];
  const char* repoToken = repo["token"];
  if (repoNameAllowed(repoName) && repoToken && repoToken[0] && strlen(repoToken) < sizeof(out.repoToken) - 8) {
    out.hasRepo = true;
    snprintf(out.repoUrl, sizeof(out.repoUrl), "https://api.github.com/repos/%s/contents", repoName);
    snprintf(out.repoToken, sizeof(out.repoToken), "Bearer %s", repoToken);
  }
  return true;
}

TzRule hostZone() {
  TzRule r;
  r.standard = learn::state().tzMinutes * 60;
  return r;
}

// --- The task's side: radio, clock, HTTP. Touches no screen, and no file but the issue's. ---

bool stopped(const Job& j) {
  return j.cancel.load() || (j.deadline && static_cast<int32_t>(millis() - j.deadline) >= 0);
}

void fail(Job& j, const char* what) {
  if (!j.error[0]) snprintf(j.error, sizeof(j.error), "%s", what);
  LOG_ERR("NET", "%s", what);
}

EventGroupHandle_t wifiEvents = nullptr;
constexpr EventBits_t GOT_IP = 1;
constexpr EventBits_t DROPPED = 2;
volatile uint8_t dropReason = 0;  // wifi_err_reason_t of the last disconnect

void onWifiEvent(void*, const esp_event_base_t base, const int32_t id, void* data) {
  if (!wifiEvents) return;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    dropReason = data ? static_cast<const wifi_event_sta_disconnected_t*>(data)->reason : 0;
    xEventGroupSetBits(wifiEvents, DROPPED);
  }
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(wifiEvents, GOT_IP);
}

// The stack and event loop, once: they stay, so they are made before the heap fills.
bool stackReady = false;
bool readyStack() {
  if (stackReady) return true;
  if (esp_netif_init() != ESP_OK) return false;
  const esp_err_t e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
  if (!wifiEvents) wifiEvents = xEventGroupCreate();
  stackReady = wifiEvents != nullptr;
  return stackReady;
}

// Why a network did not take us, in words for `x4 wifi`.
const char* dropWhy(const uint8_t reason) {
  // x4r joins with WPA2 (WPA3 is off in sdkconfig), so a wrong password fails the 4-way handshake.
  switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
      return "not found, 2.4 GHz?";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
      return "wrong password?";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_ASSOC_FAIL:
      return "refused the X4";
    case 0:
      return "no answer";
    default:
      return nullptr;
  }
}

struct Radio {
  esp_netif_t* sta = nullptr;
  esp_event_handler_instance_t wifiHandler = nullptr;
  esp_event_handler_instance_t ipHandler = nullptr;
  bool started = false;

  // How the X4 sees `ssid`: "WPA2/WPA3 ch 6 -61 dBm", or "not seen".
  static void describe(const char* ssid, char* out, const size_t size) {
    snprintf(out, size, "not seen");
    wifi_scan_config_t scan = {};
    scan.ssid = reinterpret_cast<uint8_t*>(const_cast<char*>(ssid));
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) return;
    uint16_t count = 4;
    wifi_ap_record_t aps[4];
    if (esp_wifi_scan_get_ap_records(&count, aps) != ESP_OK || count == 0) return;
    const wifi_ap_record_t& ap = aps[0];  // strongest first
    const char* mode = ap.authmode == WIFI_AUTH_WPA2_PSK        ? "WPA2"
                       : ap.authmode == WIFI_AUTH_WPA3_PSK      ? "WPA3 only"
                       : ap.authmode == WIFI_AUTH_WPA2_WPA3_PSK ? "WPA2/WPA3"
                       : ap.authmode == WIFI_AUTH_WPA_WPA2_PSK  ? "WPA/WPA2"
                       : ap.authmode == WIFI_AUTH_OPEN          ? "open"
                                                                : "other security";
    snprintf(out, size, "%s ch %u %d dBm", mode, static_cast<unsigned>(ap.primary), static_cast<int>(ap.rssi));
  }

  bool join(Job& j, const WifiCredential& n, uint8_t& reason) {
    wifi_config_t wc = {};
    strncpy(reinterpret_cast<char*>(wc.sta.ssid), n.ssid.c_str(), sizeof(wc.sta.ssid) - 1);
    strncpy(reinterpret_cast<char*>(wc.sta.password), n.password.c_str(), sizeof(wc.sta.password) - 1);
    if (!n.password.empty()) wc.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    xEventGroupClearBits(wifiEvents, GOT_IP | DROPPED);
    dropReason = 0;
    LOG_INF("NET", "Joining %s", n.ssid.c_str());
    if (esp_wifi_connect() != ESP_OK) return false;
    const uint32_t until = millis() + kConnectMs;
    while (!stopped(j) && static_cast<int32_t>(millis() - until) < 0) {
      const EventBits_t bits = xEventGroupWaitBits(wifiEvents, GOT_IP | DROPPED, pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
      if (bits & GOT_IP) {
        snprintf(j.connected, sizeof(j.connected), "%s", n.ssid.c_str());
        LOG_INF("NET", "On %s", j.connected);
        return true;
      }
      if (bits & DROPPED) break;
    }
    reason = dropReason;
    esp_wifi_disconnect();
    return false;
  }

  bool up(Job& j) {
    if (!readyStack()) {
      fail(j, "wifi: no network stack");
      return false;
    }
    sta = esp_netif_create_default_wifi_sta();
    if (!sta) return false;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;  // networks live in WifiCredentialStore; NVS would only leave allocations behind
    if (const esp_err_t e = esp_wifi_init(&cfg); e != ESP_OK) {
      char what[40];
      snprintf(what, sizeof(what), "wifi: init %d", e);
      fail(j, what);
      return false;
    }
    started = true;
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &onWifiEvent, nullptr, &wifiHandler);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onWifiEvent, nullptr, &ipHandler);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_start() != ESP_OK) {
      fail(j, "wifi: cannot start");
      return false;
    }

    for (uint8_t i = 0; i < j.networkCount && !stopped(j); ++i) {
      const WifiCredential& n = j.networks[i];
      uint8_t reason = 0;
      if (join(j, n, reason)) return true;
      const char* why = dropWhy(reason);
      char seen[32];
      describe(n.ssid.c_str(), seen, sizeof(seen));
      char what[kError];
      snprintf(what, sizeof(what), "wifi: %.12s %s (%u), %s", n.ssid.c_str(), why ? why : "refused",
               static_cast<unsigned>(reason), seen);
      fail(j, what);
    }
    return false;
  }

  ~Radio() {
    if (started) {
      esp_wifi_disconnect();
      esp_wifi_stop();
      if (wifiHandler) esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifiHandler);
      if (ipHandler) esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ipHandler);
      esp_wifi_deinit();
    }
    if (sta) esp_netif_destroy_default_wifi(sta);
  }
};

bool syncClock(Job& j) {
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_setservername(1, "time.google.com");
  sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  esp_sntp_init();
  const uint32_t until = millis() + kTimeMs;
  // Reading COMPLETED resets the status, so it is read once per turn.
  bool ok = false;
  while (!(ok = sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) && !stopped(j) &&
         static_cast<int32_t>(millis() - until) < 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  esp_sntp_stop();
  return ok;
}

// A connection for one request, or for a run of them to the same host: opening a TLS session
// takes the X4 about 3 s, so an issue's files share one. `auth` is an Authorization header and
// `accept` an Accept header, sent with every request.
class Link {
 public:
  Link(const char* auth, const char* accept, const int timeoutMs)
      : auth(auth), accept(accept), timeoutMs(timeoutMs) {}
  ~Link() {
    if (client) esp_http_client_cleanup(client);
  }
  Link(const Link&) = delete;
  Link& operator=(const Link&) = delete;

  // What a request sends besides its address.
  struct Sends {
    esp_http_client_method_t method = HTTP_METHOD_GET;
    int length = 0;  // of the body
    // Writes the body. May be called twice: a dropped connection is tried once more.
    std::function<bool(esp_http_client_handle_t)> body;
    const char* accept = nullptr;  // instead of the link's
  };

  // The body of a GET, chunk by chunk; the HTTP status, or a negative number when there was
  // none. With `post` the request is a POST of that JSON body.
  int get(Job& j, const char* url, const std::function<bool(const char*, size_t)>& onData,
          const std::string* post = nullptr) {
    Sends sends;
    if (post) {
      sends.method = HTTP_METHOD_POST;
      sends.length = static_cast<int>(post->size());
      sends.body = [post](esp_http_client_handle_t c) {
        return esp_http_client_write(c, post->data(), static_cast<int>(post->size())) >= 0;
      };
    }
    return request(j, url, sends, onData);
  }

  // Any request. `onData` gets the body of a 200; any other answer is read and dropped, so the
  // connection can carry the next request.
  int request(Job& j, const char* url, const Sends& sends, const std::function<bool(const char*, size_t)>& onData) {
    const bool kept = client != nullptr;
    if (!ready(url)) return -1;
    esp_http_client_set_method(client, sends.method);
    if (sends.body) esp_http_client_set_header(client, "Content-Type", "application/json");
    if (sends.accept || accept) esp_http_client_set_header(client, "Accept", sends.accept ? sends.accept : accept);
    int status = exchange(j, sends);
    if (status < 0 && kept && !stopped(j)) {
      // The server may have dropped the connection it kept: once more, on a new one.
      esp_http_client_close(client);
      status = exchange(j, sends);
    }
    bool whole = false;
    if (status > 0) {
      char buf[1024];
      const int answered = status;
      for (;;) {
        if (stopped(j)) {
          status = -3;
          break;
        }
        const int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n < 0) {
          status = -2;
          break;
        }
        if (n == 0) break;
        if (answered == 200 && !onData(buf, static_cast<size_t>(n))) {
          status = -3;
          break;
        }
      }
      whole = status > 0 && esp_http_client_is_complete_data_received(client);
      // An answer that is not a 200 stands, however its body ended.
      if (answered != 200) status = answered;
    }
    // What is left unread would be taken for the next answer, so the connection goes.
    if (!whole) esp_http_client_close(client);
    return status;
  }

 private:
  bool ready(const char* url) {
    if (client) return esp_http_client_set_url(client, url) == ESP_OK;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = timeoutMs;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    cfg.user_agent = "x4r";
    cfg.keep_alive_enable = false;
    client = esp_http_client_init(&cfg);
    if (!client) return false;
    if (auth) esp_http_client_set_header(client, "Authorization", auth);
    return true;
  }

  // Sends the request and reads the answer's headers, following redirects.
  int exchange(Job& j, const Sends& sends) {
    int status = -1;
    for (int hop = 0; hop < 5 && !stopped(j); ++hop) {
      if (esp_http_client_open(client, sends.length) != ESP_OK) {
        status = -1;
        break;
      }
      if (sends.body && !sends.body(client)) {
        status = -2;
        break;
      }
      esp_http_client_fetch_headers(client);
      status = esp_http_client_get_status_code(client);
      if (status < 300 || status >= 400 || status == 304) break;
      esp_http_client_set_redirection(client);
      esp_http_client_close(client);
    }
    return status;
  }

  const char* auth;
  const char* accept;
  int timeoutMs;
  esp_http_client_handle_t client = nullptr;
};

// One request on a connection of its own.
int get(Job& j, const char* url, const std::function<bool(const char*, size_t)>& onData, const char* auth = nullptr,
        const int timeoutMs = kHttpTimeoutMs, const std::string* post = nullptr, const char* accept = nullptr) {
  Link link(auth, accept, timeoutMs);
  return link.get(j, url, onData, post);
}

void fetchWeather(Job& j) {
  // A week of days, and the day ahead in three-hour steps for the weather screen: asked for
  // that way, so the answer stays small while the link's own buffers are up.
  char url[352];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min"
           "&hourly=temperature_2m,precipitation_probability,weather_code"
           "&timezone=auto&forecast_days=7&forecast_hours=24&temporal_resolution=hourly_3",
           j.config.latitude, j.config.longitude);
  std::string body;
  body.reserve(1024);
  const int status = get(j, url, [&](const char* data, const size_t n) {
    if (body.size() + n > 8192) return false;
    body.append(data, n);
    return true;
  });
  if (status != 200) {
    char what[40];
    snprintf(what, sizeof(what), "weather: %d", status);
    fail(j, what);
    return;
  }
  j.weatherOk = parseOpenMeteo(body.data(), body.size(), j.weather);
  if (!j.weatherOk) fail(j, "weather: unreadable");
}

void fetchCalendars(Job& j, const TzRule& zone) {
  const int64_t now = time(nullptr);
  const int64_t from = zone.toUtc(static_cast<int64_t>(localDay(now, zone)) * kDay);
  const int64_t to = from + 3 * kDay + 3600;
  for (uint8_t i = 0; i < j.config.calendarCount && !stopped(j); ++i) {
    std::unique_ptr<IcalParser> parser(new (std::nothrow) IcalParser(zone, from, to, i, j.events));
    if (!parser) {
      fail(j, "calendar: out of memory");
      return;
    }
    const int status = get(j, j.config.calendars[i], [&](const char* data, const size_t n) {
      parser->feed(data, n);
      return true;
    });
    if (status != 200) {
      char what[40];
      snprintf(what, sizeof(what), "calendar %u: %d", static_cast<unsigned>(i + 1), status);
      fail(j, what);
      continue;
    }
    parser->finish();
    if (!parser->sawCalendar()) {
      char what[40];
      snprintf(what, sizeof(what), "calendar %u: not iCal", static_cast<unsigned>(i + 1));
      fail(j, what);
      continue;
    }
    j.eventsOk = true;
  }
}

// --- The issue, from the Mac on the LAN or from the data repo (docs/PROTOCOL.md, "LAN issue"
// and "Repo issue"). ---

// Where an issue comes from.
struct Source {
  const char* name;    // the error's prefix
  const char* base;
  const char* auth;
  const char* accept;  // asks GitHub for a file's bytes
  int timeoutMs;       // for the manifest: away from home the Mac does not answer
  int none;            // the status that says nothing is staged
  size_t manifestMax;  // a TLS session leaves less room than plain HTTP
  bool repo;
};

Source macSource(const Job& j) {
  return {"mac", j.config.macUrl, j.config.macToken, nullptr, kMacTimeoutMs, 204, 16 * 1024, false};
}

Source repoSource(const Job& j) {
  return {"repo", j.config.repoUrl, j.config.repoToken, "application/vnd.github.raw+json", kHttpTimeoutMs, 404,
          8 * 1024, true};
}

constexpr size_t kIssueUrl = sizeof(Config::repoUrl) + sizeof(IssueFile::path) + 24;

void issueFail(Job& j, const Source& from, const char* what, const int status) {
  char e[kError];
  snprintf(e, sizeof(e), "%s: %s %d", from.name, what, status);
  fail(j, e);
}

void tmpPath(const IssueFile& f, char* out, const size_t size) { snprintf(out, size, "%s.lan", f.path); }

// True when <path>.lan is already the file the issue lists: a fetch that was stopped left it.
bool arrived(const IssueFile& f) {
  char tmp[sizeof(IssueFile::path) + 8];
  tmpPath(f, tmp, sizeof(tmp));
  if (!Storage.exists(tmp)) return false;
  HalFile in = Storage.open(tmp, O_RDONLY);
  if (!in || in.size() != f.size) return false;
  uint32_t crc = 0;
  uint8_t buf[512];
  for (uint32_t left = f.size; left > 0;) {
    const size_t want = left < sizeof(buf) ? left : sizeof(buf);
    if (in.read(buf, want) != static_cast<int>(want)) return false;
    crc = usb::crc32(crc, buf, want);
    left -= want;
  }
  return crc == f.crc;
}

// One file into <path>.lan, its size and CRC checked. With `link` the request goes down that
// connection.
bool download(Job& j, const Source& from, const IssueFile& f, Link* link) {
  char url[kIssueUrl];
  snprintf(url, sizeof(url), from.repo ? "%s/outbox%s" : "%s/file?path=%s", from.base, f.path);
  char tmp[sizeof(IssueFile::path) + 8];
  tmpPath(f, tmp, sizeof(tmp));
  HalFile out = Storage.open(tmp, O_RDWR | O_CREAT | O_TRUNC);
  if (!out) {
    issueFail(j, from, "cannot write", 0);
    return false;
  }
  uint32_t size = 0;
  uint32_t crc = 0;
  bool wrote = true;
  const auto onData = [&](const char* data, const size_t n) {
    if (size + n > f.size) return false;
    crc = usb::crc32(crc, reinterpret_cast<const uint8_t*>(data), n);
    size += n;
    wrote = out.write(reinterpret_cast<const uint8_t*>(data), n) == n;
    return wrote;
  };
  const int status =
      link ? link->get(j, url, onData) : get(j, url, onData, from.auth, kHttpTimeoutMs, nullptr, from.accept);
  const bool closed = out.close();
  if (status != 200 || !wrote || !closed || size != f.size || crc != f.crc) {
    Storage.remove(tmp);
    if (!stopped(j)) issueFail(j, from, status != 200 ? "file" : "bad file", status);
    return false;
  }
  return true;
}

// Removes the files in `dir` ending in `ext` that the issue does not list, and what a stopped
// fetch left there. Returns the first one removed, or an empty string.
std::string prune(const Issue& issue, const char* dir, const char* ext, const bool unlisted = true) {
  std::vector<std::string> gone;
  if (HalFile folder = Storage.open(dir); folder && folder.isDirectory()) {
    char name[128];
    const size_t extLength = strlen(ext);
    for (HalFile f = folder.openNextFile(); f; f = folder.openNextFile()) {
      f.getName(name, sizeof(name));
      const size_t n = strlen(name);
      if (f.isDirectory()) continue;
      const bool ofKind = n > extLength && strcmp(name + n - extLength, ext) == 0;
      const bool leftover = n > 4 && strcmp(name + n - 4, ".lan") == 0;
      if ((unlisted && ofKind && !issueLists(issue, dir, name)) || leftover) {
        gone.emplace_back(std::string(dir) + "/" + name);
      }
    }
  }
  for (const std::string& path : gone) Storage.remove(path.c_str());
  for (const std::string& path : gone) {
    if (path.size() < 4 || path.compare(path.size() - 4, 4, ".lan") != 0) return path;
  }
  return {};
}

// Every changed file first, then the swap: a failure leaves the old files. A fetch that is
// stopped keeps the files that arrived, and the next one goes on from there.
void takeIssue(Job& j, const Source& from) {
  char url[kIssueUrl];
  snprintf(url, sizeof(url), from.repo ? "%s/outbox/manifest.json" : "%s/issue", from.base);
  std::string body;
  const int status = get(
      j, url,
      [&](const char* data, const size_t n) {
        if (body.size() + n > from.manifestMax) return false;
        body.append(data, n);
        return true;
      },
      from.auth, from.timeoutMs, nullptr, from.accept);
  if (status == from.none) return;
  if (status != 200) {
    issueFail(j, from, "issue", status);
    return;
  }
  // The connection is closed by now, so the manifest is parsed with the link's buffers gone.
  std::unique_ptr<Issue> issue(new (std::nothrow) Issue);
  std::unique_ptr<Taken> taken(new (std::nothrow) Taken);
  if (!issue || !taken) {
    issueFail(j, from, "out of memory", 0);
    return;
  }
  if (const char* why = parseIssue(body.data(), body.size(), *issue)) {
    char e[kError];
    snprintf(e, sizeof(e), "%s: %s", from.name, why);
    fail(j, e);
    return;
  }
  body.clear();
  body.shrink_to_fit();
  std::string before;
  if (readFile(TAKEN_PATH, before)) takenFromJson(before.data(), before.size(), *taken);
  if (strcmp(taken->id, issue->id) == 0) return;
  markChanged(*issue, *taken);
  taken.reset();

  Storage.mkdir(READ_DIR);
  Storage.mkdir(DECKS_DIR);
  Storage.mkdir(CROSSWORDS_DIR);
  {
    // The repo's files share one connection. The manifest had its own, closed before it was
    // parsed, so the parsing never ran beside a TLS session.
    std::unique_ptr<Link> link;
    if (from.repo) link.reset(new (std::nothrow) Link(from.auth, from.accept, kHttpTimeoutMs));
    for (uint8_t i = 0; i < issue->count; ++i) {
      const IssueFile& f = issue->files[i];
      if (!f.take || arrived(f)) continue;
      if (stopped(j) || !download(j, from, f, link.get())) return;
    }
  }
  // Stopped means the user left Today: the files arrived stay as .lan, for the next fetch.
  if (stopped(j)) return;
  char tmp[sizeof(IssueFile::path) + 8];
  for (uint8_t i = 0; i < issue->count; ++i) {
    const IssueFile& f = issue->files[i];
    if (!f.take) continue;
    tmpPath(f, tmp, sizeof(tmp));
    if (!Storage.replaceFile(tmp, f.path)) {
      issueFail(j, from, "cannot replace", i);
      continue;
    }
    if (strncmp(f.path, "/x4/", 4) == 0) {
      j.issueWroteX4 = true;
    } else if (!j.issueBook[0]) {
      snprintf(j.issueBook, sizeof(j.issueBook), "%s", f.path);
    }
  }
  if (issue->pruneRead) {
    // Articles the issue no longer lists stay for now: a kept piece needs its own, and the
    // front page removes the rest when it takes the issue (paper::settle).
    const std::string gone = prune(*issue, READ_DIR, ".epub", false);
    if (!gone.empty() && !j.issueBook[0]) snprintf(j.issueBook, sizeof(j.issueBook), "%s", gone.c_str());
  }
  if (issue->pruneDecks && !prune(*issue, DECKS_DIR, ".jsonl").empty()) j.issueWroteX4 = true;
  std::string after;
  if (takenToJson(*issue, after)) {
    Storage.mkdir("/x4/state");
    writeFileAtomically(TAKEN_PATH, after);
  }
  j.issueTaken = true;
  LOG_INF("NET", "Issue %s taken from %s", issue->id, from.name);
  std::string request;
  if (from.repo) {
    // The repo is told with the records, as a file in its inbox.
    if (takenRequest(*issue, request)) writeFileAtomically(TAKEN_UP, request);
    return;
  }
  char takenUrl[sizeof(Config::macUrl) + 16];
  snprintf(takenUrl, sizeof(takenUrl), "%s/taken", from.base);
  if (takenRequest(*issue, request)) {
    const int answer = get(j, takenUrl, [](const char*, size_t) { return true; }, from.auth, kMacTimeoutMs, &request);
    if (answer != 204 && answer != 200) issueFail(j, from, "taken", answer);
  }
}

// --- Records up to the data repo's inbox (docs/PROTOCOL.md, "Repo issue"). ---

void saveUpState(const UpState& state) {
  std::string body;
  if (upStateToJson(state, body)) writeFileAtomically(UP_STATE, body);
}

bool fileCrc(const char* path, uint32_t& size, uint32_t& crc) {
  HalFile in = Storage.open(path, O_RDONLY);
  if (!in) return false;
  size = static_cast<uint32_t>(in.size());
  crc = 0;
  uint8_t buf[384];
  for (uint32_t left = size; left > 0;) {
    const size_t want = left < sizeof(buf) ? left : sizeof(buf);
    if (in.read(buf, want) != static_cast<int>(want)) return false;
    crc = usb::crc32(crc, buf, want);
    left -= want;
  }
  return true;
}

// Sends `local` up as a new file in the inbox and removes it once GitHub has it. False when
// the upload failed, which ends this fetch's uploads.
bool sendFile(Job& j, Link& link, UpState& state, const char* kind, const char* ext, const char* local) {
  if (!Storage.exists(local)) return true;
  uint32_t size = 0;
  uint32_t crc = 0;
  if (!fileCrc(local, size, crc)) return true;
  if (size == 0) {
    Storage.remove(local);
    return true;
  }
  if (size > kUploadMax) {
    LOG_INF("NET", "%s is %lu bytes: it waits for the cable", local, static_cast<unsigned long>(size));
    return true;
  }
  // The name it first went under, unless the file is another one by now.
  const UpState::Item* waiting = state.find(kind);
  char name[sizeof(UpState::Item::name)];
  if (waiting && waiting->size == size) {
    snprintf(name, sizeof(name), "%s", waiting->name);
  } else {
    inboxName(time(nullptr), kind, ext, name, sizeof(name));
    state.set(kind, name, size);
    saveUpState(state);
  }
  char url[sizeof(Config::repoUrl) + sizeof(name) + 16];
  snprintf(url, sizeof(url), "%s/inbox/%s", j.config.repoUrl, name);
  Link::Sends sends;
  sends.method = HTTP_METHOD_PUT;
  sends.length = static_cast<int>(putBodyLength(size));
  sends.accept = "application/vnd.github+json";
  sends.body = [&](esp_http_client_handle_t c) {
    HalFile in = Storage.open(local, O_RDONLY);
    if (!in || esp_http_client_write(c, PUT_HEAD, sizeof(PUT_HEAD) - 1) < 0) return false;
    uint8_t raw[384];  // a multiple of 3, so the pieces read as one
    char text[516];
    for (uint32_t left = size; left > 0;) {
      const size_t want = left < sizeof(raw) ? left : sizeof(raw);
      if (in.read(raw, want) != static_cast<int>(want)) return false;
      const size_t n = base64(raw, want, text);
      if (esp_http_client_write(c, text, static_cast<int>(n)) < 0) return false;
      left -= want;
    }
    return esp_http_client_write(c, PUT_TAIL, sizeof(PUT_TAIL) - 1) >= 0;
  };
  int status = link.request(j, url, sends, [](const char*, size_t) { return true; });
  if (status == 422) {
    // The file may be there already: an earlier answer was lost. Only then does it count.
    uint32_t there = 0;
    const int found = link.get(j, url, [&](const char*, const size_t n) {
      there += n;
      return true;
    });
    if (found == 200 && there == size) status = 201;
  }
  if (status != 200 && status != 201) {
    if (!stopped(j)) {
      char e[kError];
      snprintf(e, sizeof(e), "up: %s %d", kind, status);
      fail(j, e);
    }
    return false;
  }
  Storage.remove(local);
  state.erase(kind);
  if (strcmp(kind, "stats") == 0) {
    state.hasStatsCrc = true;
    state.statsCrc = crc;
  }
  saveUpState(state);
  LOG_INF("NET", "Sent %s (%lu bytes)", name, static_cast<unsigned long>(size));
  return true;
}

// Every waiting log, the stats when they changed, and what was taken. Names carry the time,
// so nothing goes up in a fetch that did not set the clock.
void sendUp(Job& j) {
  if (!j.timeSynced) return;
  UpState state;
  std::string before;
  if (readFile(UP_STATE, before)) upStateFromJson(before.data(), before.size(), state);
  before.clear();
  before.shrink_to_fit();
  if (Storage.exists(STATS_UP)) {
    uint32_t size = 0;
    uint32_t crc = 0;
    if (fileCrc(STATS_UP, size, crc) && state.hasStatsCrc && state.statsCrc == crc) Storage.remove(STATS_UP);
  }
  Link link(j.config.repoToken, "application/vnd.github.raw+json", kHttpTimeoutMs);
  char local[40];
  for (const char* log : kLogs) {
    snprintf(local, sizeof(local), "/x4/%s.pulling", log);
    if (stopped(j) || !sendFile(j, link, state, log, "jsonl", local)) return;
  }
  if (stopped(j) || !sendFile(j, link, state, "stats", "json", STATS_UP)) return;
  if (!stopped(j)) sendFile(j, link, state, "taken", "json", TAKEN_UP);
}

void run(Job& j) {
  const uint32_t started = millis();
  // Each stage is marked, so a new low in free memory can be laid at its door (util/HeapLows).
  heap::mark("net:start");
  Radio radio;
  if (!radio.up(j)) {
    fail(j, j.networkCount ? "wifi: no network" : "wifi: none saved");  // unless a network said why
    return;
  }
  heap::mark("net:wifi");
  j.timeSynced = syncClock(j);
  if (!j.timeSynced) fail(j, "time: no answer");
  heap::mark("net:time");
  // Plain HTTP on the LAN needs no date. With a repo the Mac is not asked.
  if (j.config.hasMac && !j.config.hasRepo && !stopped(j)) takeIssue(j, macSource(j));
  heap::mark("net:issue");
  // Certificates need the date; without NTP the trusted floor may still do.
  if (trustedtime::trustedNow() == 0 && !j.timeSynced) return;
  const TzRule zone = j.config.hasZone ? j.config.zone : TzRule{};
  if (j.config.hasWeather && !stopped(j)) fetchWeather(j);
  heap::mark("net:weather");
  if (j.config.calendarCount && !stopped(j)) fetchCalendars(j, zone);
  heap::mark("net:calendar");
  // Last, so the agenda never waits behind an issue's files.
  if (j.config.hasRepo && !stopped(j)) takeIssue(j, repoSource(j));
  heap::mark("net:repo");
  if (j.config.hasRepo && !stopped(j)) sendUp(j);
  heap::mark("net:up");
  LOG_INF("NET", "Fetched in %lu ms: time %d, issue %d, events %d (%u), weather %d",
          static_cast<unsigned long>(millis() - started), j.timeSynced, j.issueTaken, j.eventsOk,
          static_cast<unsigned>(j.events.count), j.weatherOk);
}

void taskMain(void* arg) {
  auto* j = static_cast<Job*>(arg);
  run(*j);
  j->done.store(true);
  vTaskDelete(nullptr);
}

// --- The main loop's side. ---

bool heapAllows() {
  const HalMemory::HeapStats heap = HalMemory::getInternalHeap();
  return heap.freeBytes >= kMinFreeHeap && heap.largestBlockBytes >= kMinBlock;
}

// A network is saved and the last fetch is old enough.
bool wanted() {
  ensureStore();
  if (WIFI_STORE.getCredentialCount() == 0) return false;
  return !(attempted && millis() - lastAttemptMs < (lastOk ? kFreshMs : kRetryMs));
}

bool due() { return wanted() && heapAllows(); }

// On the main loop, before the task: each log is set aside as a cable pull does it, so
// records written during the fetch start a new file, and the stats are copied whole.
void setAsideRecords(Job& j) {
  char from[40];
  char to[40];
  for (const char* log : kLogs) {
    snprintf(from, sizeof(from), "/x4/%s.jsonl", log);
    snprintf(to, sizeof(to), "/x4/%s.pulling", log);
    if (!Storage.exists(from) || Storage.exists(to) || !Storage.rename(from, to)) continue;
    if (strcmp(log, "highlights") == 0) {
      // The one log the X4 reads back: the learning layer drops what pointed into it.
      x4OnHostWrote(from);
      j.movedHighlights = true;
    }
  }
  if (Storage.exists(STATS_UP)) return;
  x4BeforeHostRead();  // the totals are written lazily
  std::string stats;
  if (readFile(STATS_PATH, stats)) {
    Storage.mkdir("/x4/state");
    writeFileAtomically(STATS_UP, stats);
  }
}

bool start(const uint32_t budgetMs) {
  if (job) return true;
  ensureStore();
  auto* j = new (std::nothrow) Job;
  if (!j) return false;
  readConfig(j->config);
  if (j->config.hasRepo) setAsideRecords(*j);
  if (!j->config.hasZone) j->config.zone = hostZone();
  j->config.hasZone = true;
  // The network that answered last time first.
  const std::string last = WIFI_STORE.getLastConnectedSsid();
  if (auto c = WIFI_STORE.findCredential(last)) j->networks[j->networkCount++] = *c;
  for (size_t i = 0; i < WIFI_STORE.getCredentialCount() && j->networkCount < kNetworks; ++i) {
    auto c = WIFI_STORE.getCredentialAt(i);
    if (c && c->ssid != last) j->networks[j->networkCount++] = *c;
  }
  j->deadline = budgetMs ? millis() + budgetMs : 0;
  attempted = true;
  lastAttemptMs = millis();
  powerLock.emplace();
  if (xTaskCreate(&taskMain, "net", kTaskStack, j, 1, nullptr) != pdPASS) {
    powerLock.reset();
    delete j;
    snprintf(lastError, sizeof(lastError), "no task");
    return false;
  }
  job = j;
  LOG_INF("NET", "Fetch started");
  return true;
}

// Takes in what a fetch changed: Today rebuilds, the Library re-indexes.
void reload() {
  if (reloadStream) x4OnHostWrote("/x4/stream.jsonl");
  if (reloadBook[0]) x4OnHostWrote(reloadBook);
  x4OnHostDataChanged();
  ++generationCount;
  reloadPending = false;
  reloadStream = false;
  reloadBook[0] = 0;
}

// `here`: on Today or on the way to sleep, where a reload interrupts nothing. Elsewhere it waits
// for Today.
void finish(const bool here) {
  Job& j = *job;
  const int64_t now = time(nullptr);
  if (j.timeSynced && now > 0) {
    trustedtime::note();
    learn::onHostTime(now, j.config.zone.offsetAt(now) / 60);
  }
  if (j.connected[0] && WIFI_STORE.getLastConnectedSsid() != j.connected) {
    WIFI_STORE.setLastConnectedSsid(j.connected);
    WIFI_STORE.saveToFile();
  }
  if (j.eventsOk || j.weatherOk) {
    // What did not answer this time keeps what it had.
    std::unique_ptr<Agenda> agenda = loadAgenda();
    if (!agenda) agenda.reset(new (std::nothrow) Agenda);
    if (agenda) {
      if (j.eventsOk) {
        agenda->events = j.events;
        agenda->hasEvents = true;
      }
      if (j.weatherOk) {
        agenda->weather = j.weather;
        agenda->hasWeather = true;
      }
      agenda->fetched = now;
      std::string body;
      if (agendaToJson(*agenda, body) && writeFileAtomically(AGENDA_PATH, body)) {
        fetchedAt = now;
        ++generationCount;
      } else {
        snprintf(j.error, sizeof(j.error), "agenda: cannot write");
      }
    }
  } else if (j.timeSynced) {
    ++generationCount;  // the clock alone changes Today
  }
  // Reload as after a USB session that wrote files.
  if (j.issueTaken || j.movedHighlights) {
    reloadPending = true;
    reloadStream = reloadStream || j.issueWroteX4;
    if (j.issueBook[0]) snprintf(reloadBook, sizeof(reloadBook), "%s", j.issueBook);
  }
  lastOk = !j.error[0];
  snprintf(lastError, sizeof(lastError), "%s", j.error);
  delete job;
  job = nullptr;
  powerLock.reset();
  if (here && reloadPending) reload();
}

// Closes every socket, so a request blocked on the network returns now. The fetch is the
// only thing on the X4 that opens one.
void abortSockets() {
  for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; ++fd) shutdown(fd, SHUT_RDWR);
}

void waitFor(const uint32_t ms) {
  const uint32_t until = millis() + ms;
  while (job && !job->done.load() && static_cast<int32_t>(millis() - until) < 0) delay(20);
  if (job && job->done.load()) finish(true);
}

}  // namespace

void begin() {
  if (!Storage.exists(WifiCredentialStore::getFilePath())) return;
  if (!readyStack()) LOG_ERR("NET", "No network stack");
}

void poll(const bool resting, void (*makeRoom)(void* ctx), void* ctx) {
  if (reloadPending && !job) reload();
  if (job) {
    if (job->done.load()) finish(true);
    return;
  }
  if (!resting || !wanted()) return;
  if (makeRoom) makeRoom(ctx);
  if (heapAllows()) start(0);
}

void pollAway() {
  if (job && job->done.load()) finish(false);
}

void stop() {
  if (!job) return;
  job->cancel.store(true);
  abortSockets();
  waitFor(kStopWaitMs);
}

void refreshBeforeSleep() {
  if (!job) {
    ensureStore();
    const int64_t now = trustedtime::trustedNow();
    const bool stale = fetchedAt == 0 || now == 0 || now - fetchedAt > kSleepStale;
    if (!stale || WIFI_STORE.getCredentialCount() == 0 || !heapAllows()) return;
    if (!start(kSleepBudgetMs)) return;
  }
  waitFor(kSleepBudgetMs + 2000);
}

bool fetching() { return job != nullptr; }

uint32_t generation() { return generationCount; }

TzRule zone() {
  Config c;
  return readConfig(c) && c.hasZone ? c.zone : hostZone();
}

std::unique_ptr<Agenda> loadAgenda() {
  std::string body;
  if (!readFile(AGENDA_PATH, body)) return nullptr;
  std::unique_ptr<Agenda> agenda(new (std::nothrow) Agenda);
  if (!agenda || !agendaFromJson(body.data(), body.size(), *agenda)) return nullptr;
  if (fetchedAt == 0) fetchedAt = agenda->fetched;
  return agenda;
}

std::unique_ptr<Football> loadFootball(const bool tables) {
  std::string body;
  if (!readFile(FOOTBALL_PATH, body, 32 * 1024)) return nullptr;
  std::unique_ptr<Football> football(new (std::nothrow) Football);
  if (!football || !parseFootball(body.data(), body.size(), *football, tables)) return nullptr;
  return football;
}

const char* command(const JsonObjectConst args, JsonObject out) {
  ensureStore();
  JsonVariantConst add = args["wifi"];
  if (!add.isNull()) {
    const char* ssid = add["ssid"];
    const char* password = add["password"] | "";
    if (!ssid || !*ssid || strlen(ssid) > 32) return "wifi.ssid must be 1 to 32 bytes";
    if (strlen(password) > 63) return "wifi.password must be at most 63 bytes";
    if (!WIFI_STORE.addCredential(ssid, password)) return "no room for another network";
    WIFI_STORE.saveToFile();
    attempted = false;  // try it at the next rest on Today
    readyStack();
  }
  if (const char* forget = args["forget"]) {
    WIFI_STORE.removeCredential(forget);
    WIFI_STORE.saveToFile();
  }
  if (args["fetch"] | false) {
    if (WIFI_STORE.getCredentialCount() == 0) return "no Wi-Fi network saved";
    if (!job && !start(0)) return "cannot start a fetch now";
  }
  helloFields(out);
  return nullptr;
}

void helloFields(JsonObject out) {
  ensureStore();
  JsonObject net = out["net"].to<JsonObject>();
  JsonArray networks = net["networks"].to<JsonArray>();
  for (const auto& s : WIFI_STORE.getCredentialSummaries()) networks.add(s.ssid);
  net["fetching"] = job != nullptr;
  if (fetchedAt) net["fetched"] = fetchedAt;
  if (lastError[0]) net["error"] = lastError;
}

}  // namespace x4::net
