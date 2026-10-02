#pragma once
#include <esp_netif.h>
// UI-owned configuration and published snapshots. Worker never reads these Strings.
static String g_ssid, g_pass;
static char g_net_state[64] = "no wifi configured";
static std::atomic<bool> g_disp_on{true}, g_sntp_done{false};
static constexpr int MAX_SCAN = 24;
static constexpr int64_t NTP_RESYNC_MS = 6LL * 60 * 60 * 1000;
static ScanEntry g_scan[MAX_SCAN];
static int g_scan_n = 0;
static bool g_scan_ready = false;
static uint32_t g_net_generation = 0;
struct NetCommand { int command; uint32_t generation; char ssid[33], pass[65]; };
struct NtpResult { int64_t utc_ms, mono_ms; uint32_t generation; };
struct ScanResult { int count; ScanEntry entries[MAX_SCAN]; };
struct NetStatus { char text[64]; };
static QueueHandle_t g_command_queue, g_ntp_queue, g_scan_queue, g_status_queue;

static bool net_request(int command) {
  if (!g_command_queue) { Serial.println("ERR network unavailable"); return false; }
  if (g_ssid.length() > 32 || g_pass.length() > 64) {
    Serial.println("ERR SSID or password too long"); return false;
  }
  NetCommand request = {};
  request.command = command; request.generation = g_net_generation;
  strlcpy(request.ssid, g_ssid.c_str(), sizeof(request.ssid));
  strlcpy(request.pass, g_pass.c_str(), sizeof(request.pass));
  if (xQueueSend(g_command_queue, &request, 0) == pdTRUE) return true;
  strlcpy(g_net_state, "network busy; retry", sizeof(g_net_state));
  Serial.println("ERR network busy; retry");
  return false;
}
static void sntp_cb(struct timeval*) { g_sntp_done.store(true); }
static void net_state(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void net_state(const char* fmt, ...) {
  NetStatus status = {};
  va_list ap; va_start(ap, fmt);
  vsnprintf(status.text, sizeof(status.text), fmt, ap);
  va_end(ap);
  xQueueOverwrite(g_status_queue, &status);
}

// Between syncs: disconnect but leave the WiFi stack (and the ESP-Hosted link to the C6) initialised.
// A disconnected station idles the radio, so this costs very little.  Fully switching WiFi off and on again
// looked like a better power saver but is NOT reliable on the Tab5 - all of these were tried:
//  * WiFi.mode(WIFI_OFF) + WiFi.mode(WIFI_STA) on the next sync, with or without hostedDeinitWiFi(): every
//    second re-init dies with "HS_MP: mempool create failed: no mem" (sdio_mempool_create assert -> reboot),
//    because the ESP-Hosted SDIO buffer pool must be rebuilt in *internal* RAM, which the first session leaves
//    fragmented.
//  * cutting power to the C6 via WLAN_PWR_EN (IO expander 0x44 bit 0) saves ~6 mA of battery current, but needs
//    the link torn down first, so it hits the same problem.
static void net_radio_off() {
  WiFi.disconnect(false, false);
}

static void net_scan() {
  net_state("scanning...");
  WiFi.mode(WIFI_STA);
  const int n = WiFi.scanNetworks();
  if (n < 0) { WiFi.scanDelete(); net_radio_off(); net_state("scan failed (%d)", n); return; }
  int cnt = 0;
  static ScanEntry tmp[MAX_SCAN];
  for (int i = 0; i < n; i++) {         // de-duplicate by SSID (keep the strongest), skip hidden networks
    const String ss = WiFi.SSID(i);
    if (ss.isEmpty()) continue;
    int j = 0;
    while (j < cnt && strcmp(tmp[j].ssid, ss.c_str()) != 0) j++;
    const int8_t rssi = (int8_t)WiFi.RSSI(i);
    if (j == cnt) {
      if (cnt >= MAX_SCAN) continue;
      strlcpy(tmp[cnt].ssid, ss.c_str(), sizeof(tmp[cnt].ssid));
      tmp[cnt].rssi = rssi;
      tmp[cnt].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      cnt++;
    } else if (rssi > tmp[j].rssi) {
      tmp[j].rssi = rssi;
      tmp[j].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
  }
  for (int a = 1; a < cnt; a++)         // strongest first
    for (int b = a; b > 0 && tmp[b].rssi > tmp[b - 1].rssi; b--) { ScanEntry x = tmp[b]; tmp[b] = tmp[b - 1]; tmp[b - 1] = x; }
  ScanResult result = {};
  result.count = cnt;
  memcpy(result.entries, tmp, sizeof(ScanEntry) * cnt);
  xQueueOverwrite(g_scan_queue, &result);
  WiFi.scanDelete();
  net_radio_off();
  net_state("scan done: %d networks", cnt);
}

static bool net_sync(const NetCommand& config) {
  if (!config.ssid[0]) { net_state("no wifi configured"); return false; }
  net_state("connecting to %s", config.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid, config.pass);
  for (int i = 0; i < 100 && WiFi.status() != WL_CONNECTED; i++) vTaskDelay(pdMS_TO_TICKS(200));
  if (WiFi.status() != WL_CONNECTED) {
    net_state("wifi connect failed (status %d)", (int)WiFi.status());
    net_radio_off();
    return false;
  }
  net_state("connected %s rssi %d, syncing time", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());

  if (esp_sntp_enabled()) esp_sntp_stop();
  g_sntp_done = false;
  sntp_set_time_sync_notification_cb(sntp_cb);
  // Configure SNTP without touching the process-wide timezone from this task.
  esp_netif_init();
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, (char*)"time.cloudflare.com");
  esp_sntp_setservername(1, (char*)"pool.ntp.org");
  esp_sntp_setservername(2, (char*)"time.google.com");
  esp_sntp_init();
  for (int i = 0; i < 150 && !g_sntp_done; i++) vTaskDelay(pdMS_TO_TICKS(100));

  bool ok = false;
  if (g_sntp_done) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const NtpResult result = {(int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000, mono_ms(), config.generation};
    xQueueOverwrite(g_ntp_queue, &result);
    ok = true;
    net_state("time synced");
  } else {
    net_state("NTP timeout");
  }
  esp_sntp_stop();
  net_radio_off();
  return ok;
}


static void net_task(void*) {
  NetCommand config = {};
  int64_t next = 0;
  unsigned fails = 0;
  static const int64_t backoff[] = {60000, 120000, 300000, 600000, 1800000, 3600000};
  for (;;) {
    NetCommand request;
    const bool received = xQueueReceive(g_command_queue, &request, pdMS_TO_TICKS(200)) == pdTRUE;
    int cmd = 0;
    if (received) { config = request; cmd = request.command; }
    if (cmd == 2) { net_scan(); continue; }
    if (cmd == 1 || (g_disp_on.load() && config.ssid[0] && mono_ms() >= next)) {
      if (net_sync(config)) { fails = 0; next = mono_ms() + NTP_RESYNC_MS; }
      else { next = mono_ms() + backoff[fails]; if (fails < 5) ++fails; }
    }
  }
}

static void net_setup() {
  g_command_queue = xQueueCreate(8, sizeof(NetCommand));
  g_ntp_queue = xQueueCreate(1, sizeof(NtpResult));
  g_scan_queue = xQueueCreate(1, sizeof(ScanResult));
  g_status_queue = xQueueCreate(1, sizeof(NetStatus));
  if (!g_command_queue || !g_ntp_queue || !g_scan_queue || !g_status_queue ||
      xTaskCreatePinnedToCore(net_task, "net", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
    if (g_command_queue) vQueueDelete(g_command_queue);
    if (g_ntp_queue) vQueueDelete(g_ntp_queue);
    if (g_scan_queue) vQueueDelete(g_scan_queue);
    if (g_status_queue) vQueueDelete(g_status_queue);
    g_command_queue = g_ntp_queue = g_scan_queue = g_status_queue = nullptr;
    strlcpy(g_net_state, "network unavailable: no memory", sizeof(g_net_state));
    Serial.println(g_net_state);
    return;
  }
  net_request(0); // copy saved credentials; worker starts automatic sync when awake
}

static void net_apply() {
  if (!g_command_queue) return;
  NetStatus status;
  if (xQueueReceive(g_status_queue, &status, 0) == pdTRUE) {
    strlcpy(g_net_state, status.text, sizeof(g_net_state));
    Serial.printf("net: %s\n", g_net_state);
  }
  ScanResult scan;
  if (xQueueReceive(g_scan_queue, &scan, 0) == pdTRUE) {
    g_scan_n = scan.count;
    memcpy(g_scan, scan.entries, sizeof(g_scan));
    g_scan_ready = true;
    for (int i = 0; i < g_scan_n; ++i)
      Serial.printf("  %-32s %ddBm %s\n", g_scan[i].ssid, (int)g_scan[i].rssi, g_scan[i].secure ? "secured" : "open");
  }
  NtpResult fix;
  if (xQueueReceive(g_ntp_queue, &fix, 0) == pdTRUE && fix.generation == g_net_generation)
    adopt_ntp(fix.utc_ms, fix.mono_ms);
}
