#pragma once
// ============================================================
//  Spotify Dashboard — Shared configuration & declarations
// ============================================================

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <SpotifyEsp32.h>
#include <OneButton.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <freertos/semphr.h>

// ── Spotify Credentials ──────────────────────────────────
extern const char* SPOTIFY_CLIENT_ID;
extern const char* SPOTIFY_CLIENT_SECRET;
#define SPOTIFY_SCOPES "user-read-playback-state%20user-modify-playback-state%20user-read-currently-playing"

// ── Hardware Pins ────────────────────────────────────────
#define BTN_TOP     0
#define BTN_BOTTOM  14
#define BL_PIN      38
#define PWR_EN      15

// ── Screen Geometry (landscape) ──────────────────────────
#define SCR_W       320
#define SCR_H       170

#define ART_X       5
#define ART_Y       8
#define ART_SZ      150

#define SEP_X       159
#define SEP_W       2
#define SEP_TOP     20
#define SEP_BOT     150

#define TXT_X       166
#define TXT_W       (SCR_W - TXT_X - 4)
#define TITLE_Y     6
#define TITLE_H     22
#define TITLE_BL    16
#define ARTIST_Y    30
#define ALBUM_Y     50
#define DEVICE_Y    72
#define ICON_W      12
#define ICON_H      16
#define ICON_MARGIN 14
#define ICON_X      (SCR_W - ICON_W - ICON_MARGIN)
#define ICON_Y      (SCR_H - ICON_H - ICON_MARGIN)

#define BAR_Y       168
#define BAR_H       2

#define CLOCK_X     TXT_X
#define CLOCK_Y     95
#define CLOCK_MS    1000

// ── Scroll ───────────────────────────────────────────────
#define SCROLL_MS       30
#define SCROLL_PAUSE_MS 2000
#define SCROLL_GAP      40

// ── Ticker ───────────────────────────────────────────────
#define MAX_TICKERS      8
#define TICKER_FETCH_MS  60000
#define TICKER_SCROLL_MS 30
#define TICKER_Y         148
#define TICKER_H         16
#define TICKER_GAP       30

// ── Timing ───────────────────────────────────────────────
#define POLL_MS     5000
#define BAR_MS      500
#define WIFI_MS     30000

// ── Redraw flags (set by core 0, consumed by core 1) ────
#define RFLAG_TRACK_CHANGED  (1 << 0)
#define RFLAG_DEVICE_CHANGED (1 << 1)
#define RFLAG_PLAY_CHANGED   (1 << 2)
#define RFLAG_GONE_IDLE      (1 << 3)
#define RFLAG_GONE_ACTIVE    (1 << 4)
#define RFLAG_TICKER_READY   (1 << 5)

// ── Button actions (set by core 1, consumed by core 0) ──
enum PendingAction { ACTION_NONE, ACTION_SKIP, ACTION_PREV, ACTION_PLAY, ACTION_PAUSE };

// ── CoinGecko crypto mapping ────────────────────────────
struct CryptoMap { const char* sym; const char* id; };
extern const CryptoMap CRYPTO_MAP[];
#define CRYPTO_MAP_SIZE 21

// ── Ticker item ─────────────────────────────────────────
struct TickerItem {
  char  symbol[8];
  float price;
  float change;
  bool  valid;
  bool  isCrypto;
};

// ── Playback state ──────────────────────────────────────
struct Playback {
  String track;
  String artist;
  String album;
  String device;
  String imgUrl;
  bool   playing  = false;
  bool   active   = false;
  int    progress = 0;
  int    duration = 0;
  unsigned long pollTime = 0;
};

// ── Global objects ──────────────────────────────────────
extern TFT_eSPI    tft;
extern Spotify*    sp;
extern OneButton   topBtn;
extern OneButton   botBtn;
extern Preferences prefs;

extern bool          spotifyReady;
extern uint8_t       screenRotation;
extern bool          screenOn;
extern Playback      now;
extern SemaphoreHandle_t dataMutex;

// Title scroll
extern TFT_eSprite   titleSpr;
extern int           scrollX;
extern int           titlePixelW;
extern unsigned long lastScroll;
extern unsigned long scrollPauseAt;
extern bool          scrollPaused;

// Clock
extern unsigned long lastClock;
extern String        lastTimeStr;

// Ticker
extern TFT_eSprite   tickerSpr;
extern TickerItem    tickerItems[MAX_TICKERS];
extern int           numTickers;
extern int           tickerTextW;
extern int           tickerScrollX;
extern unsigned long lastTickerScroll;
extern bool          tickerReady;
extern String        stockApiKey;

// Threading
extern volatile uint32_t      redrawFlags;
extern volatile PendingAction  pendingAction;
extern volatile bool           tickerListChanged;

// ── Function declarations ───────────────────────────────
// display.cpp
bool onJpgBlock(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp);
void showAlbumArt(const String& url);
String fitText(const String& s, int maxPx);
void drawIcon(bool playing);
void drawBar(int progress, int duration);
void drawClock();
void drawIdleClock();
void drawTitle();
void drawInfo();
void showStatus(const char* line1, const char* line2 = nullptr);

// ticker.cpp
const char* getCoinGeckoId(const char* sym);
void loadTickers();
void fetchCryptoPrices();
void fetchStockPrices();
void recalcTickerWidth();
int  drawTickerItemsAt(int startX);
void drawTicker();

// network.cpp
String runOAuthFlow();
void startConfigServer();
void ensureWiFi();
void checkSerialInput();
