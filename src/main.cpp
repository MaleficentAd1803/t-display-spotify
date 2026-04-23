// ============================================================
//  Spotify Dashboard — LilyGO T-Display S3 (ST7789, 320 x 170)
// ============================================================
//  SETUP (one-time):
//   1. Create an app at https://developer.spotify.com/dashboard
//   2. After WiFi connects the display shows the redirect URI —
//      add it (https://...) to your Spotify app settings.
//   3. Open the auth URL shown on the display in a browser.
//      Accept the self-signed cert warning, then authorize.
//   4. The refresh token is saved automatically to NVS.
//
//  BOOT COMBO:
//   Hold BOT button at boot → reset WiFi + Spotify token
//
//  CONTROLS:
//   TOP single-click  → Play / Pause
//   TOP double-click  → Next track
//   TOP triple-click  → Previous track
//   TOP long-press    → Flip screen orientation
//   BOT single-click  → Screen on/off
//
//  CONFIG:
//   Open http://<device-ip> in browser to manage tickers
//
//  ARCHITECTURE:
//   Core 0 — background: Spotify API, ticker fetches, WiFi
//   Core 1 — foreground: all rendering, button handling, scrolling
// ============================================================

#include "config.h"
#include <WiFiManager.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <mbedtls/base64.h>

// ── Credentials ─────────────────────────────────────────
const char* SPOTIFY_CLIENT_ID     = "ca1f92d52a1945c684308ec209cb2d89";
const char* SPOTIFY_CLIENT_SECRET = "dc1c281c25634d3d9ed2218cbbc613e2";

// ── CoinGecko crypto ID mapping ─────────────────────────
const CryptoMap CRYPTO_MAP[] = {
  {"BTC","bitcoin"},{"ETH","ethereum"},{"SOL","solana"},
  {"ADA","cardano"},{"XRP","ripple"},{"DOGE","dogecoin"},
  {"DOT","polkadot"},{"AVAX","avalanche-2"},{"BNB","binancecoin"},
  {"LTC","litecoin"},{"LINK","chainlink"},{"SHIB","shiba-inu"},
  {"MATIC","matic-network"},{"UNI","uniswap"},{"ATOM","cosmos"},
  {"PEPE","pepe"},{"ARB","arbitrum"},{"OP","optimism"},
  {"SUI","sui"},{"APT","aptos"},{"XMR","monero"},
};

const CommodityMap COMMODITY_MAP[] = {
  {"GOLD",   "GLD"},     // SPDR Gold Shares ETF
  {"SILVER", "SLV"},     // iShares Silver Trust
  {"OIL",    "USO"},     // United States Oil Fund
  {"NATGAS", "UNG"},     // United States Natural Gas Fund
  {"COPPER", "CPER"},    // United States Copper Index Fund
  {"PLAT",   "PPLT"},    // abrdn Physical Platinum Shares
  {"PALLAD", "PALL"},    // abrdn Physical Palladium Shares
};

// ── Global object definitions ───────────────────────────
TFT_eSPI    tft;
Spotify*    sp          = nullptr;
OneButton   topBtn(BTN_TOP,    true, true);
OneButton   botBtn(BTN_BOTTOM, true, true);
Preferences prefs;
WiFiManager wm;

SemaphoreHandle_t dataMutex;

bool          spotifyReady   = false;
uint8_t       screenRotation = 1;
bool          screenOn       = true;
uint8_t       brightPlay     = 16;     // 1-16 backlight levels
uint8_t       brightIdle     = 8;
uint8_t       brightCurrent  = 16;
unsigned long brightSettingsAt = 0;
static uint8_t blLevel       = 0;     // Backlight chip state: 0=off, 1-16
Playback      now;
float         cpuTempC       = 0;

// T-Display S3 backlight uses a one-wire pulse protocol (NOT PWM).
// The chip has 16 brightness levels. Each LOW→HIGH pulse decrements
// by one step (wrapping from 1 back to 16). Pulling LOW for >3ms resets to off.
// Pulling HIGH from off starts at max (16).
static void setBrightness(uint8_t val, const char* src = "") {
  if (val > BL_STEPS) val = BL_STEPS;
  brightCurrent = val;
  if (val == 0) {
    digitalWrite(BL_PIN, LOW);
    delay(3);
    blLevel = 0;
#ifdef VERBOSE_BL
    LOG("[BL] %s level=0 (off)\n", src);
#else
    (void)src;
#endif
    return;
  }
  if (blLevel == 0) {
    digitalWrite(BL_PIN, HIGH);
    blLevel = BL_STEPS;
    delayMicroseconds(30);
  }
  int from = BL_STEPS - blLevel;
  int to   = BL_STEPS - val;
  int pulses = (BL_STEPS + to - from) % BL_STEPS;
  for (int i = 0; i < pulses; i++) {
    digitalWrite(BL_PIN, LOW);
    digitalWrite(BL_PIN, HIGH);
  }
  blLevel = val;
#ifdef VERBOSE_BL
  LOG("[BL] %s level=%d (pulses=%d)\n", src, val, pulses);
#else
  (void)src; (void)pulses;
#endif
}

// Title scroll
TFT_eSprite   titleSpr(&tft);
int           scrollX        = 0;
int           titlePixelW    = 0;
unsigned long lastScroll     = 0;
unsigned long scrollPauseAt  = 0;
bool          scrollPaused   = true;

// Clock
unsigned long lastClock      = 0;
unsigned long lastBar        = 0;
String        lastTimeStr    = "";

// Ticker
TFT_eSprite   tickerSpr(&tft);
TickerItem    tickerItems[MAX_TICKERS];
int           numTickers       = 0;
int           tickerTextW      = 0;
int           tickerScrollX    = 0;
unsigned long lastTickerScroll = 0;
bool          tickerReady      = false;
String        stockApiKey;

// Threading
volatile uint32_t      redrawFlags    = 0;
volatile PendingAction pendingAction  = ACTION_NONE;
volatile bool          tickerListChanged = false;
volatile bool          settingsChanged   = false;

// Data usage counters (session total). HTTPClient doesn't expose wire bytes,
// so TX is estimated from URL + headers + POST body; RX uses getSize() for
// body plus a ~250 B fudge for response headers on 200s, or ~200 B on 304.
NetStats netSpotify = {0, 0};
NetStats netArt     = {0, 0};
NetStats netTicker  = {0, 0};

// CPU usage tracking (per-core busy time measurement)
static unsigned long cpuLastReport     = 0;
static unsigned long core1BusyUs       = 0;
static unsigned long core0BusyUs       = 0;
#define CPU_REPORT_MS 5000

// Background task timing (owned by core 0)
static unsigned long bgLastPoll       = 0;
static unsigned long bgLastTickerFetch = 0;
static unsigned long bgLastWifi       = 0;
static bool          bgTickerFetchNeeded = true;  // fetch on first idle

// ── Persistent Spotify polling client (keep-alive TLS) ──
static WiFiClientSecure pollClient;
static HTTPClient       pollHttp;
static bool             pollClientInit = false;
static String           accessToken;
static String           lastETag;
static const char*      etagHeader = "ETag";

// ── JSON filter for Spotify response (built once) ───────
static JsonDocument     pollFilter;
static bool             pollFilterInit = false;

// ── Build "Basic <base64(client_id:client_secret)>" auth header value
String buildSpotifyBasicAuth() {
  String creds = String(SPOTIFY_CLIENT_ID) + ":" + String(SPOTIFY_CLIENT_SECRET);
  char b64[256];
  size_t outLen = 0;
  mbedtls_base64_encode((unsigned char*)b64, sizeof(b64), &outLen,
                        (const unsigned char*)creds.c_str(), creds.length());
  b64[outLen] = 0;
  return String("Basic ") + b64;
}

// ── Refresh the Spotify access token ────────────────────
static bool refreshAccessToken() {
  LOGLN("[Token] Refreshing access token...");
  // Free mbedtls memory held by our keep-alive TLS sockets — the token
  // handshake needs ~30KB and the ESP32 can't allocate it while we hold
  // poll + art contexts (each ~30KB) alongside the library's own client.
  stopAlbumArtClient();
  pollHttp.end();
  pollClient.stop();
  pollClientInit = false;
  lastETag = "";

  WiFiClientSecure tokenClient;
  tokenClient.setInsecure();
  HTTPClient http;
  http.begin(tokenClient, "https://accounts.spotify.com/api/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Authorization", buildSpotifyBasicAuth());

  String rtoken = prefs.getString("rtoken", "");
  String body = "grant_type=refresh_token&refresh_token=" + rtoken;
  netSpotify.txBytes += body.length() + 48 /*URL path*/ + REQ_HDR_EST + 64 /*Basic auth*/;
  int code = http.POST(body);
  int rxSz = http.getSize();
  if (rxSz > 0) netSpotify.rxBytes += (uint32_t)rxSz + RESP_HDR_EST;
  else netSpotify.rxBytes += RESP_HDR_EST;

  if (code == 200) {
    JsonDocument doc;
    deserializeJson(doc, http.getStream());
    accessToken = doc["access_token"] | "";
    // Spotify may issue a new refresh token
    const char* newRt = doc["refresh_token"] | (const char*)nullptr;
    if (newRt && strlen(newRt) > 0) {
      prefs.putString("rtoken", newRt);
      LOGLN("[Token] New refresh token saved");
    }
    http.end();
    LOG("[Token] Access token obtained (%d chars)\n", accessToken.length());
    return accessToken.length() > 0;
  }

  LOG("[Token] Refresh failed: %d\n", code);
  http.end();
  return false;
}

// ============================================================
//  Spotify data fetch (runs on core 0 — no drawing!)
//  Uses persistent TLS connection, ETag caching, and stream
//  parsing with a JSON filter for minimal memory usage.
// ============================================================
static void pollSpotifyData() {
  if (!spotifyReady) return;

  // One-time init: persistent TLS client + JSON filter
  if (!pollClientInit) {
    pollClient.setInsecure();
    pollHttp.setReuse(true);
    pollHttp.setTimeout(10000);
    pollHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    pollHttp.collectHeaders(&etagHeader, 1);
    pollClientInit = true;
  }
  if (!pollFilterInit) {
    pollFilter["progress_ms"] = true;
    pollFilter["is_playing"] = true;
    pollFilter["item"]["id"] = true;
    pollFilter["item"]["name"] = true;
    pollFilter["item"]["artists"][0]["name"] = true;
    pollFilter["item"]["album"]["name"] = true;
    pollFilter["item"]["album"]["images"][0]["url"] = true;
    pollFilter["item"]["duration_ms"] = true;
    pollFilter["device"]["name"] = true;
    pollFilterInit = true;
  }

  // Get access token if we don't have one yet
  if (accessToken.length() == 0 && !refreshAccessToken()) {
    LOGLN("[Poll] No access token — skipping");
    return;
  }

#ifdef VERBOSE_POLL
  LOG("[Poll] Heap: %u\n", ESP.getFreeHeap());
#endif

  pollHttp.begin(pollClient, "https://api.spotify.com/v1/me/player");
  pollHttp.addHeader("Authorization", "Bearer " + accessToken);
  pollHttp.addHeader("Connection", "keep-alive");
  if (lastETag.length() > 0) {
    pollHttp.addHeader("If-None-Match", lastETag);
  }

  // ~90 B request line + Bearer token + headers. Bearer is ~180 chars.
  netSpotify.txBytes += 90 + accessToken.length() + lastETag.length() + REQ_HDR_EST;

  unsigned long t0 = millis();
  int code = pollHttp.GET();
  unsigned long rtt = millis() - t0;

  if (code == 304) {
    // Not Modified — nothing changed, just update poll time for interpolation
    netSpotify.rxBytes += RESP_HDR_EST;  // headers only
    LOG("[Poll] 304 (%lums)\n", rtt);
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (now.active) now.pollTime = millis();
    xSemaphoreGive(dataMutex);
    pollHttp.end();
    return;
  }

  if (code == 200) {
    int sz = pollHttp.getSize();
    netSpotify.rxBytes += (sz > 0 ? (uint32_t)sz : 0) + RESP_HDR_EST;
    // Capture ETag for next request
    if (pollHttp.hasHeader("ETag")) {
      lastETag = pollHttp.header("ETag");
    }

    // Stream-parse with filter — only extracts fields we need
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, pollHttp.getStream(),
                                               DeserializationOption::Filter(pollFilter));
    pollHttp.end();

    if (err) {
      LOG("[Poll] JSON error: %s (%lums)\n", err.c_str(), rtt);
      return;
    }

    String tid  = doc["item"]["id"] | "";
    int    prog = doc["progress_ms"] | 0;
    bool   play = doc["is_playing"] | false;

    // Check if track ID changed (triggers full metadata redraw)
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool wasInactive = !now.active;
    bool idChanged   = (tid != now.trackId) || wasInactive;

    // If paused and already idle, just stay idle
    if (!play && !now.active && !idChanged) {
      xSemaphoreGive(dataMutex);
      LOG("[Poll] Idle, no change (%lums)\n", rtt);
      return;
    }

    if (idChanged) {
      // ── Full metadata update ──
      String trk = doc["item"]["name"] | "";
      String alb = doc["item"]["album"]["name"] | "";
      int    dur = doc["item"]["duration_ms"] | 0;
      String art;
      JsonArray artists = doc["item"]["artists"];
      if (artists.size() > 0) art = artists[0]["name"] | "";
      String dev = doc["device"]["name"] | "";
      String img;
      JsonArray images = doc["item"]["album"]["images"];
      if (images.size() > 1)      img = images[1]["url"] | "";
      else if (images.size() > 0) img = images[0]["url"] | "";

      now.trackId  = tid;
      now.track    = trk;
      now.artist   = art;
      now.album    = alb;
      now.device   = dev;
      now.imgUrl   = img;
      now.progress = prog;
      now.duration = dur;
      now.playing  = play;
      now.pollTime = millis();
      now.active   = true;
      xSemaphoreGive(dataMutex);

      LOG("[Poll] New track: %s | %s (%lums)\n", trk.c_str(), art.c_str(), rtt);
      if (wasInactive) redrawFlags |= RFLAG_GONE_ACTIVE;
      redrawFlags |= RFLAG_TRACK_CHANGED;

    } else {
      // ── Same track — lightweight update (progress + play state) ──
      bool playChanged   = (play != now.playing);
      String dev = doc["device"]["name"] | "";
      bool deviceChanged = (dev != now.device);
      if (deviceChanged) now.device = dev;
      now.progress = prog;
      now.playing  = play;
      now.pollTime = millis();
      xSemaphoreGive(dataMutex);

      LOG("[Poll] Update: prog=%d play=%d (%lums)\n", prog, play, rtt);
      if (deviceChanged)       redrawFlags |= RFLAG_DEVICE_CHANGED;
      else if (playChanged)    redrawFlags |= RFLAG_PLAY_CHANGED;
      // No flag = progress-only (handled by interpolation in loop)
    }

  } else if (code == 204) {
    netSpotify.rxBytes += RESP_HDR_EST;
    pollHttp.end();
    LOG("[Poll] Nothing playing (%lums)\n", rtt);
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool wasActive = now.active;
    if (wasActive) now = Playback{};
    xSemaphoreGive(dataMutex);
    if (wasActive) {
      redrawFlags |= RFLAG_GONE_IDLE;
      bgTickerFetchNeeded = true;
      lastETag = "";
      // CDN will close the keep-alive during idle — drop it now so the next
      // wake opens a fresh handshake instead of retrying on a dead socket.
      stopAlbumArtClient();
    }

  } else if (code == 401) {
    pollHttp.end();
    LOG("[Poll] 401 — refreshing token (%lums)\n", rtt);
    accessToken = "";
    lastETag = "";
    refreshAccessToken();

  } else {
    pollHttp.end();
    LOG("[Poll] HTTP %d (%lums)\n", code, rtt);
    if (code < 0) {
      // Connection lost — reset client for fresh TLS handshake
      pollClient.stop();
    }
  }
}

// ============================================================
//  Background task — core 0
//  Handles all blocking network operations so core 1 is free
//  to render smooth animations without interruption.
// ============================================================
static void backgroundTask(void* param) {
  // Unsubscribe IDLE0 from task watchdog — Spotify API calls can block
  // Core 0 for several seconds during TLS handshakes, starving IDLE0.
  esp_task_wdt_delete(xTaskGetIdleTaskHandleForCPU(0));
  LOGLN("[BG] Background task started on core 0");

  while (true) {
    unsigned long loopStart = micros();
    unsigned long ms = millis();

    // Process queued button actions (Spotify API calls)
    PendingAction action = pendingAction;
    if (action != ACTION_NONE) {
      pendingAction = ACTION_NONE;
      if (sp && spotifyReady) {
        unsigned long tAct = millis();
        switch (action) {
          case ACTION_SKIP:  sp->skip();                    break;
          case ACTION_PREV:  sp->previous();                break;
          case ACTION_PLAY:  sp->start_resume_playback();   break;
          case ACTION_PAUSE: sp->pause_playback();          break;
          default: break;
        }
        LOG("[BG] Spotify action %d: %lums\n", action, millis() - tAct);
        // Poll immediately after action to get new state
        delay(400);
        unsigned long tPoll = millis();
        pollSpotifyData();
        LOG("[BG] Post-action poll: %lums\n", millis() - tPoll);
        bgLastPoll = millis();
      }
    }

    // Periodic Spotify poll
    bool isActive;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    isActive = now.active;
    xSemaphoreGive(dataMutex);

    unsigned long pollInterval = isActive ? POLL_MS : POLL_IDLE_MS;
    if (screenOn && ms - bgLastPoll >= pollInterval) {
      bgLastPoll = ms;
      pollSpotifyData();
      // Re-read active state (poll may have changed it)
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      isActive = now.active;
      xSemaphoreGive(dataMutex);
    }

    // Ticker price fetching
    if (!isActive && numTickers > 0 &&
        (ms - bgLastTickerFetch >= TICKER_FETCH_MS || bgTickerFetchNeeded)) {
      bgTickerFetchNeeded = false;
      bgLastTickerFetch = ms;
      LOGLN("[BG] Fetching prices...");
      fetchCryptoPrices();
      fetchStockPrices();
      redrawFlags |= RFLAG_TICKER_READY;
      LOGLN("[BG] Price fetch done");
    }

    // WiFi reconnect
    if (ms - bgLastWifi >= WIFI_MS) {
      bgLastWifi = ms;
      ensureWiFi();
    }

    core0BusyUs += micros() - loopStart;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ============================================================
//  Button callbacks (run on core 1 — queue API calls for core 0)
// ============================================================
static void onSkip() {
  if (!sp || !spotifyReady) return;
  LOGLN("[Button] Skip");
  pendingAction = ACTION_SKIP;
}

static void onPrev() {
  if (!sp || !spotifyReady) return;
  LOGLN("[Button] Previous");
  pendingAction = ACTION_PREV;
}

static void onPlayPause() {
  if (!sp || !spotifyReady) return;
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  bool active = now.active;
  if (active) now.playing = !now.playing;
  bool isPlaying = now.playing;
  xSemaphoreGive(dataMutex);

  if (!active) return;
  LOG("[Button] %s\n", isPlaying ? "Play" : "Pause");
  drawIcon(isPlaying);
  pendingAction = isPlaying ? ACTION_PLAY : ACTION_PAUSE;
}

static void onTopMulti();   // fwd

static void onScreenToggle() {
  screenOn = !screenOn;
  LOG("[Button] Screen %s\n", screenOn ? "ON" : "OFF");
  if (screenOn) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool active = now.active;
    xSemaphoreGive(dataMutex);
    setBrightness(active ? brightPlay : brightIdle, "screen-on");
    lastTimeStr = "";
    bgLastPoll = 0;  // trigger immediate poll on core 0
  } else {
    setBrightness(0, "screen-off");
  }
}

static void onFlipScreen() {
  screenRotation = (screenRotation == 1) ? 3 : 1;
  prefs.putUChar("rotation", screenRotation);
  tft.setRotation(screenRotation);
  tft.fillScreen(TFT_BLACK);
  LOG("[Button] Rotation flipped to %d\n", screenRotation);

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  bool active = now.active;
  String imgUrl = now.imgUrl;
  xSemaphoreGive(dataMutex);

  if (active) showAlbumArt(imgUrl);
  drawInfo();
}

static void onTopMulti() {
  // OneButton fires attachMultiClick for 3+ clicks. We only care about 3.
  if (topBtn.getNumberClicks() >= 3) onPrev();
}

// ============================================================
//  setup() — runs on core 1
// ============================================================
void setup() {
  Serial.begin(115200);
  LOG("\n[Boot] Reset reason: %d | Free heap: %u | Min heap: %u | PSRAM: %u/%u\n",
                esp_reset_reason(), ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getPsramSize());

  pinMode(PWR_EN, OUTPUT);
  digitalWrite(PWR_EN, HIGH);

  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, LOW);
  delay(5);                     // Reset backlight chip (needs >3ms LOW)
  digitalWrite(BL_PIN, HIGH);   // Chip starts at max brightness (level 16)
  blLevel = BL_STEPS;

  // ── Boot-time button check ────────────────────────────
  pinMode(BTN_BOTTOM, INPUT_PULLUP);
  delay(100);
  bool resetHeld = digitalRead(BTN_BOTTOM) == LOW;

  // ── Display ────────────────────────────────────────────
  prefs.begin("spotify", false);
  screenRotation = prefs.getUChar("rotation", 1);

  tft.init();
  tft.setRotation(screenRotation);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  // ── JPEG decoder ───────────────────────────────────────
  TJpgDec.setJpgScale(2);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(onJpgBlock);

  cpuTempC = temperatureRead();

  // ── WiFi ───────────────────────────────────────────────
  WiFi.mode(WIFI_STA);
  wm.setDebugOutput(true);

  if (resetHeld) {
    showStatus("Resetting all...");
    wm.resetSettings();
    prefs.remove("rtoken");
    LOGLN("[Reset] WiFi + Spotify token cleared");
    delay(800);
  }

  showStatus("Connecting WiFi...", "Hold BOT at boot to reset");

  wm.setConfigPortalTimeout(300);
  wm.setAPCallback([](WiFiManager* mgr) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("Connect to WiFi:", SCR_W / 2, 20);
    tft.drawString("SpotifyDisplay", SCR_W / 2, 50);
    tft.drawString("Then open browser:", SCR_W / 2, 90);
    tft.drawString("http://192.168.4.1", SCR_W / 2, 120);
    tft.setTextDatum(TL_DATUM);
    LOGLN("[WiFi] Config portal started");
  });

  if (!wm.autoConnect("SpotifyDisplay")) {
    showStatus("WiFi failed", "Restarting...");
    delay(3000);
    ESP.restart();
  }

  showStatus("WiFi connected", WiFi.localIP().toString().c_str());
  delay(1500);

  // ── NTP time sync ──────────────────────────────────────
  long gmtOff = prefs.getLong("gmtoff", 3600);
  long dstOff = prefs.getLong("dstoff", 0);
  configTime(gmtOff, dstOff, "pool.ntp.org", "time.nist.gov");
  LOGLN("[NTP] Syncing time...");
  struct tm t;
  if (getLocalTime(&t, 5000)) {
    LOG("[NTP] Time: %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    LOGLN("[NTP] Sync failed — will retry in background");
  }

  // ── Spotify auth ───────────────────────────────────────
  String refreshToken = prefs.getString("rtoken", "");

  if (refreshToken.length() == 0) {
    refreshToken = runOAuthFlow();
    if (refreshToken.length() > 0) {
      prefs.putString("rtoken", refreshToken);
      showStatus("Spotify authorized!");
      delay(1000);
    } else {
      showStatus("Auth failed!", "Restarting...");
      delay(3000);
      ESP.restart();
    }
  }

  sp = new Spotify(SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET, refreshToken.c_str());
  sp->begin();

  // Get initial access token for direct API polling
  if (!refreshAccessToken()) {
    showStatus("Token failed!", "Retrying...");
    delay(2000);
    refreshAccessToken();  // One retry
  }

  spotifyReady = true;
  showStatus("Spotify ready!");
  LOGLN("[Spotify] Ready");
  delay(500);

  // ── Sprites ────────────────────────────────────────────
  titleSpr.createSprite(TXT_W + SCROLL_OVERFLOW, TITLE_H);
  titleSpr.setSwapBytes(true);

  tickerSpr.createSprite(SCR_W + SCROLL_OVERFLOW, TICKER_H);
  tickerSpr.setSwapBytes(true);
  loadTickers();
  stockApiKey = prefs.getString("stockkey", "d6m0k71r01qu3p05ktsgd6m0k71r01qu3p05ktt0");
  brightPlay = prefs.getUChar("br_play", 16);
  brightIdle = prefs.getUChar("br_idle", 8);
  // Migrate old 0-255 range values to 0-16 range
  if (brightPlay > BL_STEPS) { brightPlay = BL_STEPS; prefs.putUChar("br_play", brightPlay); }
  if (brightIdle > BL_STEPS) { brightIdle = BL_STEPS / 2; prefs.putUChar("br_idle", brightIdle); }
  LOG("[Boot] Loaded brightness: play=%d idle=%d\n", brightPlay, brightIdle);
  if (stockApiKey.length() == 0) {
    LOGLN("[Ticker] No stock API key. Get free key at https://finnhub.io/register");
    LOGLN("[Ticker] Then send: STOCKKEY:your_key_here");
  } else {
    LOGLN("[Ticker] Stock API key loaded");
  }

  // ── Buttons ────────────────────────────────────────────
  // Top: 1-click=play/pause, 2-click=skip, 3-click=prev, long=flip
  topBtn.attachClick(onPlayPause);
  topBtn.attachDoubleClick(onSkip);
  topBtn.attachMultiClick(onTopMulti);
  topBtn.attachLongPressStart(onFlipScreen);
  // Bottom: 1-click=screen on/off
  botBtn.attachClick(onScreenToggle);

  // ── Config web server ──────────────────────────────────
  startConfigServer();

  // ── Data mutex ─────────────────────────────────────────
  dataMutex = xSemaphoreCreateMutex();

  // ── First poll + draw (before background task starts) ──
  tft.fillScreen(TFT_BLACK);
  pollSpotifyData();
  bgLastPoll = millis();
  // Process initial flags immediately
  uint32_t initFlags = redrawFlags;
  redrawFlags = 0;
  if (initFlags & RFLAG_TRACK_CHANGED) {
    setBrightness(brightPlay, "boot-play");
    drawInfo();
    showAlbumArt(now.imgUrl);
  } else {
    setBrightness(brightIdle, "boot-idle");
    drawInfo();  // idle screen
  }

  // ── Start background task on core 0 ────────────────────
  // Must be after first poll so both cores don't call Spotify API simultaneously
  xTaskCreatePinnedToCore(backgroundTask, "bg", 16384, NULL, 1, NULL, 0);
}

// ============================================================
//  Serial telemetry TUI (runs on core 1 inside loop)
// ============================================================
#if TUI_ENABLED
static float lastCore0Pct = 0;
static float lastCore1Pct = 0;
static unsigned long lastTuiDraw = 0;
static bool          tuiInit     = false;

static void fmtBytes(char* buf, size_t n, uint32_t bytes) {
  if (bytes < 1024UL)             snprintf(buf, n, "%4lu B ", (unsigned long)bytes);
  else if (bytes < 1024UL * 1024) snprintf(buf, n, "%6.1f KB", bytes / 1024.0f);
  else                            snprintf(buf, n, "%6.2f MB", bytes / (1024.0f * 1024));
}

static void truncPad(char* dst, size_t n, const char* src, size_t width) {
  size_t sl = strlen(src);
  if (sl > width) {
    if (width < 2) { dst[0] = 0; return; }
    memcpy(dst, src, width - 2);
    dst[width - 2] = '.'; dst[width - 1] = '.'; dst[width] = 0;
  } else {
    memcpy(dst, src, sl);
    for (size_t i = sl; i < width; i++) dst[i] = ' ';
    dst[width] = 0;
  }
  (void)n;
}

// ANSI color palette (256-color / bold)
#define CBRD    "\x1b[38;5;244m"   // border: grey
#define CTTL    "\x1b[1;96m"       // title: bold bright cyan
#define CSEC    "\x1b[1;95m"       // section: bold magenta
#define CLBL    "\x1b[1;97m"       // label: bold bright white
#define CVAL    "\x1b[0m"          // value: default
#define CGOOD   "\x1b[92m"         // green
#define CWARN   "\x1b[93m"         // yellow
#define CINFO   "\x1b[96m"         // cyan
#define CBAD    "\x1b[91m"         // red
#define CRST    "\x1b[0m"
// Box = 66 cols: '|' at col 1, content 2..65, '|' at col 66.
#define TUI_R   "\x1b[66G" CBRD "|" CRST "\r\n"   // jump to col 66, right border
#define TUI_L   CBRD "|" CRST "  "                 // left border + 2-space indent

static void drawTui() {
  if (!tuiInit) {
    Serial.print("\x1b[?25l");   // hide cursor
    tuiInit = true;
  }
  Serial.print("\x1b[H\x1b[J");  // home + clear

  unsigned long upS = millis() / 1000;
  unsigned int uh = upS / 3600, um = (upS / 60) % 60, us = upS % 60;

  String track, artist, album, device;
  int prog, dur;
  bool playing, active;
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  track   = now.track;   artist = now.artist;  album = now.album;
  device  = now.device;  prog   = now.progress; dur  = now.duration;
  playing = now.playing; active = now.active;
  if (active && playing) prog += (int)(millis() - now.pollTime);
  xSemaphoreGive(dataMutex);

  // Health-based colors
  int rssi = (int)WiFi.RSSI();
  const char* rssiC = (rssi < -75) ? CBAD : (rssi < -65) ? CWARN : CGOOD;
  const char* tempC_ = (cpuTempC > 70) ? CBAD : (cpuTempC > 55) ? CWARN : CGOOD;
  unsigned freeHeap = ESP.getFreeHeap();
  const char* heapC = (freeHeap < 30000) ? CBAD : (freeHeap < 60000) ? CWARN : CGOOD;

  char line[256], fld[80];

  // ── Top border ──
  Serial.print(CBRD "+================================================================+" CRST "\r\n");

  // ── Title + uptime ──
  snprintf(line, sizeof(line),
    TUI_L CTTL "T-Display Spotify" CRST "                          "
    CLBL "up " CVAL "%02u:%02u:%02u" TUI_R,
    uh, um, us);
  Serial.print(line);

  // ── Divider ──
  Serial.print(CBRD "+================================================================+" CRST "\r\n");

  // ── Heap / PSRAM / Temp ──
  snprintf(line, sizeof(line),
    TUI_L CLBL "Heap : " "%s%6u B" CRST "   "
    CLBL "PSRAM: " CVAL "%7u B" CRST "   "
    CLBL "Temp: " "%s%5.1fC" CRST TUI_R,
    heapC, freeHeap, (unsigned)ESP.getFreePsram(), tempC_, cpuTempC);
  Serial.print(line);

  // ── Core0 / Core1 / WiFi ──
  snprintf(line, sizeof(line),
    TUI_L CLBL "Core0: " CVAL "%5.1f%%" "   "
    CLBL "Core1: " CVAL "%5.1f%%" "         "
    CLBL "WiFi: " "%s%4d dBm" CRST TUI_R,
    lastCore0Pct, lastCore1Pct, rssiC, rssi);
  Serial.print(line);

  // ── IP ──
  snprintf(line, sizeof(line),
    TUI_L CLBL "IP   : " CINFO "%s" CRST TUI_R,
    WiFi.localIP().toString().c_str());
  Serial.print(line);

  // ── Now playing section header ──
  Serial.print(CBRD "+-- " CSEC "Now playing" CBRD " -------------------------------------------------+" CRST "\r\n");

  if (active) {
    truncPad(fld, sizeof(fld), track.c_str(), 53);
    snprintf(line, sizeof(line), TUI_L CLBL "Track  : " CVAL "%s" CRST TUI_R, fld);
    Serial.print(line);
    truncPad(fld, sizeof(fld), artist.c_str(), 53);
    snprintf(line, sizeof(line), TUI_L CLBL "Artist : " CVAL "%s" CRST TUI_R, fld);
    Serial.print(line);
    truncPad(fld, sizeof(fld), album.c_str(), 53);
    snprintf(line, sizeof(line), TUI_L CLBL "Album  : " CVAL "%s" CRST TUI_R, fld);
    Serial.print(line);
    truncPad(fld, sizeof(fld), device.c_str(), 53);
    snprintf(line, sizeof(line), TUI_L CLBL "Device : " CINFO "%s" CRST TUI_R, fld);
    Serial.print(line);

    int pm = prog / 60000, ps = (prog / 1000) % 60;
    int dm = dur  / 60000, ds = (dur  / 1000) % 60;
    const char* stC = playing ? CGOOD : CWARN;
    const char* stT = playing ? "Playing" : "Paused ";
    snprintf(line, sizeof(line),
      TUI_L CLBL "Status : " "%s%s" CRST "  " CVAL "%d:%02d / %d:%02d" CRST TUI_R,
      stC, stT, pm, ps, dm, ds);
    Serial.print(line);
  } else {
    snprintf(line, sizeof(line),
      TUI_L CLBL "Status : " CWARN "Idle" CRST TUI_R);
    Serial.print(line);
  }

  // ── Data usage section header ──
  Serial.print(CBRD "+-- " CSEC "Data usage (session)" CBRD " ----------------------------------------+" CRST "\r\n");

  snprintf(line, sizeof(line),
    TUI_L "             " CLBL "      TX            RX" CRST TUI_R);
  Serial.print(line);

  char tx[16], rx[16];
  fmtBytes(tx, sizeof(tx), netSpotify.txBytes);
  fmtBytes(rx, sizeof(rx), netSpotify.rxBytes);
  snprintf(line, sizeof(line),
    TUI_L CLBL "%-12s" CWARN "%10s" CRST "    " CINFO "%10s" CRST TUI_R,
    "Spotify", tx, rx);
  Serial.print(line);

  fmtBytes(tx, sizeof(tx), netArt.txBytes);
  fmtBytes(rx, sizeof(rx), netArt.rxBytes);
  snprintf(line, sizeof(line),
    TUI_L CLBL "%-12s" CWARN "%10s" CRST "    " CINFO "%10s" CRST TUI_R,
    "Album art", tx, rx);
  Serial.print(line);

  fmtBytes(tx, sizeof(tx), netTicker.txBytes);
  fmtBytes(rx, sizeof(rx), netTicker.rxBytes);
  snprintf(line, sizeof(line),
    TUI_L CLBL "%-12s" CWARN "%10s" CRST "    " CINFO "%10s" CRST TUI_R,
    "Ticker", tx, rx);
  Serial.print(line);

  // Separator inside section
  snprintf(line, sizeof(line),
    TUI_L CBRD "--------------------------------------------" CRST TUI_R);
  Serial.print(line);

  uint32_t totTx = netSpotify.txBytes + netArt.txBytes + netTicker.txBytes;
  uint32_t totRx = netSpotify.rxBytes + netArt.rxBytes + netTicker.rxBytes;
  fmtBytes(tx, sizeof(tx), totTx);
  fmtBytes(rx, sizeof(rx), totRx);
  char tot[16]; fmtBytes(tot, sizeof(tot), totTx + totRx);
  snprintf(line, sizeof(line),
    TUI_L CLBL "%-12s" CWARN "%10s" CRST "    " CINFO "%10s" CRST
    "   " CVAL "(total " CLBL "%s" CVAL ")" CRST TUI_R,
    "Total", tx, rx, tot);
  Serial.print(line);

  // ── Bottom border ──
  Serial.print(CBRD "+================================================================+" CRST "\r\n");
}
#endif  // TUI_ENABLED

// ============================================================
//  loop() — core 1: rendering & animations only
//  Never blocks on network — all HTTP done on core 0.
// ============================================================
void loop() {
  unsigned long loopStart = micros();
  topBtn.tick();
  botBtn.tick();

  unsigned long ms = millis();

  // ── Process redraw flags from core 0 ──────────────────
  uint32_t flags = redrawFlags;
  if (flags) {
    redrawFlags = 0;

    if (flags & RFLAG_GONE_IDLE) {
      // Don't override brightness if user just changed settings (3s cooldown)
      if (ms - brightSettingsAt > 3000) setBrightness(brightIdle, "idle");
      tft.fillScreen(TFT_BLACK);
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      drawInfo();
      xSemaphoreGive(dataMutex);
    }

    if (flags & RFLAG_TRACK_CHANGED) {
      if (flags & RFLAG_GONE_ACTIVE) {
        lastTimeStr = "";
      }

      String imgUrl;
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      imgUrl = now.imgUrl;
      xSemaphoreGive(dataMutex);

      tft.fillScreen(TFT_BLACK);
      if (ms - brightSettingsAt > 3000) setBrightness(brightPlay, "track");

      xSemaphoreTake(dataMutex, portMAX_DELAY);
      drawInfo();
      xSemaphoreGive(dataMutex);

      showAlbumArt(imgUrl);
    } else if (flags & RFLAG_DEVICE_CHANGED) {
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      drawInfo();
      xSemaphoreGive(dataMutex);
    } else if (flags & RFLAG_PLAY_CHANGED) {
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      drawIcon(now.playing);
      drawBar(now.progress, now.duration);
      xSemaphoreGive(dataMutex);
    }

    if (flags & RFLAG_TICKER_READY) {
      recalcTickerWidth();
    }
  }

  // ── Progress bar (interpolated) ───────────────────────
  if (screenOn && now.active && now.playing && ms - lastBar >= BAR_MS) {
    lastBar = ms;
    int elapsed = ms - now.pollTime;
    int cur = min(now.progress + (int)elapsed, now.duration);
    drawBar(cur, now.duration);
  }

  // ── Title scrolling ───────────────────────────────────
  if (screenOn && now.active && titlePixelW > TXT_W) {
    if (scrollPaused) {
      if (ms >= scrollPauseAt) {
        scrollPaused = false;
        lastScroll = ms;
      }
    } else if (ms - lastScroll >= SCROLL_MS) {
      lastScroll = ms;
      scrollX++;
      if (scrollX >= titlePixelW + SCROLL_GAP) {
        scrollX = 0;
        scrollPaused = true;
        scrollPauseAt = ms + SCROLL_PAUSE_MS;
      }
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      drawTitle();
      xSemaphoreGive(dataMutex);
    }
  }

  // ── Clock ─────────────────────────────────────────────
  if (screenOn && ms - lastClock >= CLOCK_MS) {
    lastClock = ms;
    if (now.active) {
      drawClock();
    } else {
      drawIdleClock();
    }
  }

  // ── Ticker list changed via web UI or serial ──────────
  if (tickerListChanged) {
    tickerListChanged = false;
    loadTickers();
    stockApiKey = prefs.getString("stockkey", "d6m0k71r01qu3p05ktsgd6m0k71r01qu3p05ktt0");
    tickerReady = false;
    tickerScrollX = 0;
    bgTickerFetchNeeded = true;
    if (!now.active) tft.fillRect(0, TICKER_Y, SCR_W, TICKER_H, TFT_BLACK);
  }

  // ── Settings changed (brightness/timezone) via web UI ──
  if (settingsChanged) {
    settingsChanged = false;
    brightPlay = prefs.getUChar("br_play", 16);
    brightIdle = prefs.getUChar("br_idle", 8);
    LOG("[Settings] Applied brightness: play=%d idle=%d\n", brightPlay, brightIdle);
    long gmt = prefs.getLong("gmtoff", 3600);
    long dst = prefs.getLong("dstoff", 0);
    configTime(gmt, dst, "pool.ntp.org", "time.nist.gov");
    lastTimeStr = "";

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool active = now.active;
    xSemaphoreGive(dataMutex);
    brightSettingsAt = millis();  // Prevent polls from overriding for 3s
    uint8_t target = active ? brightPlay : brightIdle;
    setBrightness(target, active ? "settings-play" : "settings-idle");
  }

  // ── Ticker scrolling (never interrupted by network) ───
  if (screenOn && !now.active && tickerReady &&
      ms - lastTickerScroll >= TICKER_SCROLL_MS) {
    lastTickerScroll = ms;
    tickerScrollX++;
    if (tickerTextW > SCR_W && tickerScrollX >= tickerTextW)
      tickerScrollX = 0;
    drawTicker();
  }

  // ── Serial input ──────────────────────────────────────
  checkSerialInput();

#if TUI_ENABLED
  // ── Telemetry TUI repaint ────────────────────────────
  if (ms - lastTuiDraw >= TUI_REFRESH_MS) {
    lastTuiDraw = ms;
    drawTui();
  }
#endif

  // ── CPU usage report ──────────────────────────────────
  core1BusyUs += micros() - loopStart;
  if (ms - cpuLastReport >= CPU_REPORT_MS) {
    unsigned long elapsed = (ms - cpuLastReport) * 1000UL;  // to microseconds
    float c1 = elapsed > 0 ? 100.0f * core1BusyUs / elapsed : 0;
    float c0 = elapsed > 0 ? 100.0f * core0BusyUs / elapsed : 0;
    cpuTempC = temperatureRead();
#if TUI_ENABLED
    lastCore0Pct = c0;
    lastCore1Pct = c1;
#endif
    LOG("[CPU] Core 0: %.1f%%  Core 1: %.1f%%  Heap: %u  Temp: %.1fC\n",
                  c0, c1, ESP.getFreeHeap(), cpuTempC);
    if (screenOn) {
      if (now.active) {
        drawCpuTemp(CPU_TEMP_PLAY_X, CPU_TEMP_PLAY_Y, cpuTempC, COLOR_DIM_GREY);
      } else {
        drawCpuTemp(CPU_TEMP_IDLE_X, CPU_TEMP_IDLE_Y, cpuTempC, COLOR_DIM_GREY);
      }
    }
    core0BusyUs = 0;
    core1BusyUs = 0;
    cpuLastReport = ms;
  }

  // ── Yield to RTOS (prevents 100% busy loop) ───────────
  delay(1);
}
