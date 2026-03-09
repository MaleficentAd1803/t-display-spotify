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
//   TOP single-click  → Next track
//   TOP double-click  → Previous track
//   TOP long-press    → Flip screen orientation
//   BOT single-click  → Play / Pause
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

// ── Credentials ─────────────────────────────────────────
const char* SPOTIFY_CLIENT_ID     = "YOUR_CLIENT_ID";
const char* SPOTIFY_CLIENT_SECRET = "YOUR_CLIENT_SECRET";

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
Playback      now;

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

// ============================================================
//  Spotify data fetch (runs on core 0 — no drawing!)
// ============================================================
static void pollSpotifyData() {
  if (!sp || !spotifyReady) return;

  Serial.printf("[Poll] Free heap: %u bytes\n", ESP.getFreeHeap());

  if (ESP.getFreeHeap() < 50000) {
    Serial.println("[Poll] Low memory — recreating Spotify client");
    String rtoken = prefs.getString("rtoken", "");
    delete sp;
    sp = new Spotify(SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET, rtoken.c_str());
    sp->begin();
    Serial.printf("[Poll] Heap after recreate: %u bytes\n", ESP.getFreeHeap());
  }

  response res = sp->current_playback_state();
  Serial.printf("[Poll] Status: %d\n", res.status_code);

  if (res.status_code == 200) {
    String trk  = res.reply["item"]["name"]              | "";
    String alb  = res.reply["item"]["album"]["name"]     | "";
    int    prog = res.reply["progress_ms"]               | 0;
    int    dur  = res.reply["item"]["duration_ms"]       | 0;
    bool   play = res.reply["is_playing"]                | false;

    String art;
    JsonArray artists = res.reply["item"]["artists"];
    if (artists.size() > 0) art = artists[0]["name"] | "";

    String dev  = res.reply["device"]["name"]              | "";

    String img;
    JsonArray images = res.reply["item"]["album"]["images"];
    if (images.size() > 1)      img = images[1]["url"] | "";
    else if (images.size() > 0) img = images[0]["url"] | "";

    xSemaphoreTake(dataMutex, portMAX_DELAY);

    // If not playing and we're already idle, stay idle
    if (!play && !now.active) {
      Serial.printf("[Poll] Paused & idle — staying idle (%s)\n", trk.c_str());
      xSemaphoreGive(dataMutex);
      res.reply.clear();
      return;
    }

    bool wasInactive   = !now.active;
    bool trackChanged  = (trk != now.track || img != now.imgUrl);
    bool deviceChanged = (dev != now.device);
    bool playChanged   = (play != now.playing);

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

    Serial.printf("[Poll] Track: %s | Artist: %s | Device: %s | Playing: %d\n",
                  trk.c_str(), art.c_str(), dev.c_str(), play);

    // Set redraw flags (read by core 1)
    if (trackChanged || wasInactive) {
      if (wasInactive) redrawFlags |= RFLAG_GONE_ACTIVE;
      redrawFlags |= RFLAG_TRACK_CHANGED;
    } else if (deviceChanged) {
      redrawFlags |= RFLAG_DEVICE_CHANGED;
    } else if (playChanged) {
      redrawFlags |= RFLAG_PLAY_CHANGED;
    }

    res.reply.clear();

  } else if (res.status_code == 204) {
    Serial.println("[Poll] Nothing playing (204)");
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool wasActive = now.active;
    if (wasActive) {
      now = Playback{};
    }
    xSemaphoreGive(dataMutex);
    if (wasActive) {
      redrawFlags |= RFLAG_GONE_IDLE;
      bgTickerFetchNeeded = true;
    }
  } else {
    Serial.printf("[Poll] API error: %d\n", res.status_code);
    if (res.status_code == 401) {
      Serial.println("[Poll] 401 Unauthorized — recreating Spotify client");
      String rtoken = prefs.getString("rtoken", "");
      delete sp;
      sp = new Spotify(SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET, rtoken.c_str());
      sp->begin();
    }
  }
}

// ============================================================
//  Background task — core 0
//  Handles all blocking network operations so core 1 is free
//  to render smooth animations without interruption.
// ============================================================
static void backgroundTask(void* param) {
  Serial.println("[BG] Background task started on core 0");

  while (true) {
    unsigned long loopStart = micros();
    unsigned long ms = millis();

    // Process queued button actions (Spotify API calls)
    PendingAction action = pendingAction;
    if (action != ACTION_NONE) {
      pendingAction = ACTION_NONE;
      if (sp && spotifyReady) {
        switch (action) {
          case ACTION_SKIP:  sp->skip();                    break;
          case ACTION_PREV:  sp->previous();                break;
          case ACTION_PLAY:  sp->start_resume_playback();   break;
          case ACTION_PAUSE: sp->pause_playback();          break;
          default: break;
        }
        // Poll immediately after action to get new state
        delay(400);
        pollSpotifyData();
        bgLastPoll = millis();
      }
    }

    // Periodic Spotify poll
    bool isActive;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    isActive = now.active;
    xSemaphoreGive(dataMutex);

    unsigned long pollInterval = isActive ? POLL_MS : 5000;
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
  now.playing = !now.playing;
  bool isPlaying = now.playing;
  xSemaphoreGive(dataMutex);

  Serial.printf("[Button] %s\n", isPlaying ? "Play" : "Pause");
  drawIcon(isPlaying);
  pendingAction = isPlaying ? ACTION_PLAY : ACTION_PAUSE;
}

static void onScreenToggle() {
  screenOn = !screenOn;
  Serial.printf("[Button] Screen %s\n", screenOn ? "ON" : "OFF");
  if (screenOn) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool active = now.active;
    xSemaphoreGive(dataMutex);
    analogWrite(BL_PIN, active ? 255 : 128);
    lastTimeStr = "";
    bgLastPoll = 0;  // trigger immediate poll on core 0
  } else {
    analogWrite(BL_PIN, 0);
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

  if (active) {
    showAlbumArt(imgUrl);
    drawInfo();
  } else {
    drawInfo();
  }
}

// ============================================================
//  setup() — runs on core 1
// ============================================================
void setup() {
  Serial.begin(115200);

  pinMode(PWR_EN, OUTPUT);
  digitalWrite(PWR_EN, HIGH);

  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, HIGH);

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
  configTime(3600, 3600, "pool.ntp.org", "time.nist.gov");
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

  spotifyReady = true;
  showStatus("Spotify ready!");
  Serial.println("[Spotify] Ready");
  delay(500);

  // ── Sprites ────────────────────────────────────────────
  titleSpr.createSprite(TXT_W, TITLE_H);
  titleSpr.setSwapBytes(true);

  tickerSpr.createSprite(SCR_W, TICKER_H);
  tickerSpr.setSwapBytes(true);
  loadTickers();
  stockApiKey = prefs.getString("stockkey", "");
  if (stockApiKey.length() == 0) {
    Serial.println("[Ticker] No stock API key. Get free key at https://finnhub.io/register");
    Serial.println("[Ticker] Then send: STOCKKEY:your_key_here");
  } else {
    Serial.println("[Ticker] Stock API key loaded");
  }

  // ── Buttons ────────────────────────────────────────────
  topBtn.attachClick(onSkip);
  topBtn.attachDoubleClick(onPrev);
  topBtn.attachLongPressStart(onFlipScreen);
  botBtn.attachClick(onPlayPause);
  botBtn.attachDoubleClick(onScreenToggle);

  // ── Config web server ──────────────────────────────────
  startConfigServer();

  // ── Data mutex ─────────────────────────────────────────
  dataMutex = xSemaphoreCreateMutex();

  // ── Start background task on core 0 ────────────────────
  xTaskCreatePinnedToCore(backgroundTask, "bg", 16384, NULL, 1, NULL, 0);

  // ── First poll + draw (before background task starts) ──
  tft.fillScreen(TFT_BLACK);
  pollSpotifyData();
  bgLastPoll = millis();
  // Process initial flags immediately
  uint32_t initFlags = redrawFlags;
  redrawFlags = 0;
  if (initFlags & RFLAG_TRACK_CHANGED) {
    if (initFlags & RFLAG_GONE_ACTIVE) analogWrite(BL_PIN, 255);
    showAlbumArt(now.imgUrl);
    drawInfo();
  } else {
    analogWrite(BL_PIN, 128);
    drawInfo();  // idle screen
  }
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
      analogWrite(BL_PIN, 128);
      tft.fillScreen(TFT_BLACK);
      drawInfo();
    }

    if (flags & RFLAG_TRACK_CHANGED) {
      if (flags & RFLAG_GONE_ACTIVE) {
        analogWrite(BL_PIN, 255);
        lastTimeStr = "";
      }

      String imgUrl;
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      imgUrl = now.imgUrl;
      xSemaphoreGive(dataMutex);

      tft.fillScreen(TFT_BLACK);
      showAlbumArt(imgUrl);

      xSemaphoreTake(dataMutex, portMAX_DELAY);
      drawInfo();
      xSemaphoreGive(dataMutex);
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
      drawTitle();
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
    stockApiKey = prefs.getString("stockkey", "");
    tickerReady = false;
    tickerScrollX = 0;
    bgTickerFetchNeeded = true;
    if (!now.active) tft.fillRect(0, TICKER_Y, SCR_W, TICKER_H, TFT_BLACK);
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
    Serial.printf("[CPU] Core 0: %.1f%%  Core 1: %.1f%%  Heap: %u\n", c0, c1, ESP.getFreeHeap());
    core0BusyUs = 0;
    core1BusyUs = 0;
    cpuLastReport = ms;
  }

  // ── Yield to RTOS (prevents 100% busy loop) ───────────
  delay(1);
}
