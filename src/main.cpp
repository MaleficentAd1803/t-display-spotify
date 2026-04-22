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
//   BOT single-click  → Switch page (Now Playing ↔ Lyrics)
//   BOT double-click  → Screen on / off
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
    Serial.printf("[BL] %s level=0 (off)\n", src);
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
  Serial.printf("[BL] %s level=%d (pulses=%d)\n", src, val, pulses);
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
volatile Page          currentPage       = PAGE_NOWPLAYING;
volatile bool          lyricsFetchNeeded = false;
static unsigned long   lastLyricDraw     = 0;
#define LYRIC_DRAW_MS 250

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
  Serial.println("[Token] Refreshing access token...");
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
  int code = http.POST(body);

  if (code == 200) {
    JsonDocument doc;
    deserializeJson(doc, http.getStream());
    accessToken = doc["access_token"] | "";
    // Spotify may issue a new refresh token
    const char* newRt = doc["refresh_token"] | (const char*)nullptr;
    if (newRt && strlen(newRt) > 0) {
      prefs.putString("rtoken", newRt);
      Serial.println("[Token] New refresh token saved");
    }
    http.end();
    Serial.printf("[Token] Access token obtained (%d chars)\n", accessToken.length());
    return accessToken.length() > 0;
  }

  Serial.printf("[Token] Refresh failed: %d\n", code);
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
    Serial.println("[Poll] No access token — skipping");
    return;
  }

#ifdef VERBOSE_POLL
  Serial.printf("[Poll] Heap: %u\n", ESP.getFreeHeap());
#endif

  pollHttp.begin(pollClient, "https://api.spotify.com/v1/me/player");
  pollHttp.addHeader("Authorization", "Bearer " + accessToken);
  pollHttp.addHeader("Connection", "keep-alive");
  if (lastETag.length() > 0) {
    pollHttp.addHeader("If-None-Match", lastETag);
  }

  unsigned long t0 = millis();
  int code = pollHttp.GET();
  unsigned long rtt = millis() - t0;

  if (code == 304) {
    // Not Modified — nothing changed, just update poll time for interpolation
    Serial.printf("[Poll] 304 (%lums)\n", rtt);
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (now.active) now.pollTime = millis();
    xSemaphoreGive(dataMutex);
    pollHttp.end();
    return;
  }

  if (code == 200) {
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
      Serial.printf("[Poll] JSON error: %s (%lums)\n", err.c_str(), rtt);
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
      Serial.printf("[Poll] Idle, no change (%lums)\n", rtt);
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

      Serial.printf("[Poll] New track: %s | %s (%lums)\n", trk.c_str(), art.c_str(), rtt);
      if (wasInactive) redrawFlags |= RFLAG_GONE_ACTIVE;
      redrawFlags |= RFLAG_TRACK_CHANGED;
      // Invalidate previous track's lyrics immediately so the lyrics page
      // shows "Loading..." instead of mis-syncing the old lines.
      numLyrics = 0;
      lyricsTrackId = "";
      lyricsTriedCurrent = false;
      lyricsFetchNeeded = true;

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

      Serial.printf("[Poll] Update: prog=%d play=%d (%lums)\n", prog, play, rtt);
      if (deviceChanged)       redrawFlags |= RFLAG_DEVICE_CHANGED;
      else if (playChanged)    redrawFlags |= RFLAG_PLAY_CHANGED;
      // No flag = progress-only (handled by interpolation in loop)
    }

  } else if (code == 204) {
    pollHttp.end();
    Serial.printf("[Poll] Nothing playing (%lums)\n", rtt);
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool wasActive = now.active;
    if (wasActive) now = Playback{};
    xSemaphoreGive(dataMutex);
    if (wasActive) {
      redrawFlags |= RFLAG_GONE_IDLE;
      bgTickerFetchNeeded = true;
      lastETag = "";
    }

  } else if (code == 401) {
    pollHttp.end();
    Serial.printf("[Poll] 401 — refreshing token (%lums)\n", rtt);
    accessToken = "";
    lastETag = "";
    refreshAccessToken();

  } else {
    pollHttp.end();
    Serial.printf("[Poll] HTTP %d (%lums)\n", code, rtt);
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
  Serial.println("[BG] Background task started on core 0");

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
        Serial.printf("[BG] Spotify action %d: %lums\n", action, millis() - tAct);
        // Poll immediately after action to get new state
        delay(400);
        unsigned long tPoll = millis();
        pollSpotifyData();
        Serial.printf("[BG] Post-action poll: %lums\n", millis() - tPoll);
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

    // Lyrics fetch (on track change)
    if (lyricsFetchNeeded && isActive) {
      lyricsFetchNeeded = false;
      unsigned long tLyr = millis();
      fetchLyrics();
      Serial.printf("[BG] Lyrics fetch total: %lums\n", millis() - tLyr);
    }

    // Ticker price fetching
    if (!isActive && numTickers > 0 &&
        (ms - bgLastTickerFetch >= TICKER_FETCH_MS || bgTickerFetchNeeded)) {
      bgTickerFetchNeeded = false;
      bgLastTickerFetch = ms;
      Serial.println("[BG] Fetching prices...");
      fetchCryptoPrices();
      fetchStockPrices();
      redrawFlags |= RFLAG_TICKER_READY;
      Serial.println("[BG] Price fetch done");
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
  Serial.println("[Button] Skip");
  pendingAction = ACTION_SKIP;
}

static void onPrev() {
  if (!sp || !spotifyReady) return;
  Serial.println("[Button] Previous");
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
  Serial.printf("[Button] %s\n", isPlaying ? "Play" : "Pause");
  // Only update icon when the Now Playing page is visible
  if (currentPage == PAGE_NOWPLAYING) drawIcon(isPlaying);
  pendingAction = isPlaying ? ACTION_PLAY : ACTION_PAUSE;
}

static void onTopMulti();   // fwd

static void onPageToggle() {
  currentPage = (currentPage == PAGE_NOWPLAYING) ? PAGE_LYRICS : PAGE_NOWPLAYING;
  Serial.printf("[Button] Page -> %s\n",
                currentPage == PAGE_LYRICS ? "LYRICS" : "NOW-PLAYING");
  redrawFlags |= RFLAG_PAGE_CHANGED;
}

static void onScreenToggle() {
  screenOn = !screenOn;
  Serial.printf("[Button] Screen %s\n", screenOn ? "ON" : "OFF");
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
  Serial.printf("[Button] Rotation flipped to %d\n", screenRotation);

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  bool active = now.active;
  String imgUrl = now.imgUrl;
  xSemaphoreGive(dataMutex);

  if (currentPage == PAGE_LYRICS && active) {
    drawLyricsPage();
  } else {
    if (active) showAlbumArt(imgUrl);
    drawInfo();
  }
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
  Serial.printf("\n[Boot] Reset reason: %d | Free heap: %u | Min heap: %u | PSRAM: %u/%u\n",
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
    Serial.println("[Reset] WiFi + Spotify token cleared");
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
    Serial.println("[WiFi] Config portal started");
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
  Serial.println("[NTP] Syncing time...");
  struct tm t;
  if (getLocalTime(&t, 5000)) {
    Serial.printf("[NTP] Time: %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    Serial.println("[NTP] Sync failed — will retry in background");
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
  Serial.println("[Spotify] Ready");
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
  Serial.printf("[Boot] Loaded brightness: play=%d idle=%d\n", brightPlay, brightIdle);
  if (stockApiKey.length() == 0) {
    Serial.println("[Ticker] No stock API key. Get free key at https://finnhub.io/register");
    Serial.println("[Ticker] Then send: STOCKKEY:your_key_here");
  } else {
    Serial.println("[Ticker] Stock API key loaded");
  }

  // ── Buttons ────────────────────────────────────────────
  // Top: 1-click=play/pause, 2-click=skip, 3-click=prev, long=flip
  topBtn.attachClick(onPlayPause);
  topBtn.attachDoubleClick(onSkip);
  topBtn.attachMultiClick(onTopMulti);
  topBtn.attachLongPressStart(onFlipScreen);
  // Bottom: 1-click=page toggle, 2-click=screen on/off
  botBtn.attachClick(onPageToggle);
  botBtn.attachDoubleClick(onScreenToggle);

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
      drawInfo();  // idle view regardless of currentPage (lyrics needs active track)
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
      if (currentPage == PAGE_LYRICS) {
        drawLyricsPage();  // shows "Loading lyrics..." until fetch completes
      } else {
        drawInfo();
      }
      xSemaphoreGive(dataMutex);

      if (currentPage == PAGE_NOWPLAYING) showAlbumArt(imgUrl);
    } else if (flags & RFLAG_DEVICE_CHANGED) {
      if (currentPage == PAGE_NOWPLAYING) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        drawInfo();
        xSemaphoreGive(dataMutex);
      }
    } else if (flags & RFLAG_PLAY_CHANGED) {
      if (currentPage == PAGE_NOWPLAYING) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        drawIcon(now.playing);
        drawBar(now.progress, now.duration);
        xSemaphoreGive(dataMutex);
      }
    }

    if (flags & RFLAG_PAGE_CHANGED) {
      String imgUrl;
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      bool active = now.active;
      imgUrl = now.imgUrl;
      xSemaphoreGive(dataMutex);
      tft.fillScreen(TFT_BLACK);
      lastTimeStr = "";
      if (currentPage == PAGE_LYRICS && active) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        drawLyricsPage();
        xSemaphoreGive(dataMutex);
      } else {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        drawInfo();
        xSemaphoreGive(dataMutex);
        if (active) showAlbumArt(imgUrl);
      }
    }

    if (flags & RFLAG_LYRICS_READY) {
      if (currentPage == PAGE_LYRICS && now.active) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        drawLyricsPage();
        xSemaphoreGive(dataMutex);
      }
    }

    if (flags & RFLAG_TICKER_READY) {
      recalcTickerWidth();
    }
  }

  // ── Progress bar (interpolated) ───────────────────────
  if (screenOn && currentPage == PAGE_NOWPLAYING &&
      now.active && now.playing && ms - lastBar >= BAR_MS) {
    lastBar = ms;
    int elapsed = ms - now.pollTime;
    int cur = min(now.progress + (int)elapsed, now.duration);
    drawBar(cur, now.duration);
  }

  // ── Lyrics page update (track current line) ───────────
  if (screenOn && currentPage == PAGE_LYRICS && now.active &&
      ms - lastLyricDraw >= LYRIC_DRAW_MS) {
    lastLyricDraw = ms;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    drawLyricsUpdate();
    xSemaphoreGive(dataMutex);
  }

  // ── Title scrolling ───────────────────────────────────
  if (screenOn && currentPage == PAGE_NOWPLAYING && now.active && titlePixelW > TXT_W) {
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
  if (screenOn && currentPage == PAGE_NOWPLAYING && ms - lastClock >= CLOCK_MS) {
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
    Serial.printf("[Settings] Applied brightness: play=%d idle=%d\n", brightPlay, brightIdle);
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

  // ── CPU usage report ──────────────────────────────────
  core1BusyUs += micros() - loopStart;
  if (ms - cpuLastReport >= CPU_REPORT_MS) {
    unsigned long elapsed = (ms - cpuLastReport) * 1000UL;  // to microseconds
    float c1 = elapsed > 0 ? 100.0f * core1BusyUs / elapsed : 0;
    float c0 = elapsed > 0 ? 100.0f * core0BusyUs / elapsed : 0;
    cpuTempC = temperatureRead();
    Serial.printf("[CPU] Core 0: %.1f%%  Core 1: %.1f%%  Heap: %u  Temp: %.1fC\n",
                  c0, c1, ESP.getFreeHeap(), cpuTempC);
    if (screenOn && currentPage == PAGE_NOWPLAYING) {
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
