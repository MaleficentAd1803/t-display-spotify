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
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <SpotifyEsp32.h>
#include <OneButton.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_https_server.h>
#include <time.h>
#include "certs.h"

// ── Spotify Credentials ──────────────────────────────────
const char* SPOTIFY_CLIENT_ID     = "YOUR_CLIENT_ID";
const char* SPOTIFY_CLIENT_SECRET = "YOUR_CLIENT_SECRET";

#define SPOTIFY_SCOPES "user-read-playback-state%20user-modify-playback-state%20user-read-currently-playing"

// ── Hardware Pins ────────────────────────────────────────
#define BTN_TOP     0     // GPIO 0  — single=Next, double=Prev
#define BTN_BOTTOM  14    // GPIO 14 — Play / Pause
#define BL_PIN      38    // Backlight
#define PWR_EN      15    // Peripheral power enable

// ── Screen Geometry (landscape) ──────────────────────────
#define SCR_W       320
#define SCR_H       170

// Album art: 300x300 JPEG scaled 2x -> 150x150 px
#define ART_X       5
#define ART_Y       8
#define ART_SZ      150

// Vertical separator (2 px wide, shorter — matching art area)
#define SEP_X       159
#define SEP_W       2
#define SEP_TOP     20
#define SEP_BOT     150

// Right-panel text area
#define TXT_X       166
#define TXT_W       (SCR_W - TXT_X - 4)
#define TITLE_Y     6
#define TITLE_H     22   // FreeSansBold9pt7b height
#define TITLE_BL    16   // Baseline offset from top of sprite
#define ARTIST_Y    30
#define ALBUM_Y     50
#define DEVICE_Y    72
#define ICON_W      12    // Icon content width
#define ICON_H      16    // Icon content height
#define ICON_MARGIN 14    // Equal margin from screen right and screen bottom
#define ICON_X      (SCR_W - ICON_W - ICON_MARGIN)   // 294
#define ICON_Y      (SCR_H - ICON_H - ICON_MARGIN)   // 140

// Progress bar (full width, bottom edge)
#define BAR_Y       168
#define BAR_H       2

// Clock (right panel, below play icon)
#define CLOCK_X     TXT_X
#define CLOCK_Y     95
#define CLOCK_MS    1000

// Title scroll
#define SCROLL_MS       30
#define SCROLL_PAUSE_MS 2000
#define SCROLL_GAP      40   // px gap before title repeats

// ── Timing (ms) ─────────────────────────────────────────
#define POLL_MS     5000
#define BAR_MS      500
#define WIFI_MS     30000

// ── Global Objects ───────────────────────────────────────
TFT_eSPI    tft;
Spotify*    sp          = nullptr;
OneButton   topBtn(BTN_TOP,    true, true);
OneButton   botBtn(BTN_BOTTOM, true, true);
Preferences prefs;
WiFiManager wm;

bool          spotifyReady   = false;
uint8_t       screenRotation = 1;
unsigned long lastPoll       = 0;
unsigned long lastBar        = 0;
unsigned long lastWifi       = 0;

// Title scroll state
TFT_eSprite   titleSpr(&tft);
int           scrollX        = 0;
int           titlePixelW    = 0;
unsigned long lastScroll     = 0;
unsigned long scrollPauseAt  = 0;
bool          scrollPaused   = true;

// Screen on/off
bool          screenOn       = true;

// Clock
unsigned long lastClock      = 0;
String        lastTimeStr    = "";

// ── Playback State ───────────────────────────────────────
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
} now;

// ── OAuth state (used during HTTPS auth flow) ────────────
static String oauthCode       = "";
static bool   oauthComplete   = false;
static String oauthRedirectUri = "";

// ============================================================
//  TJpg_Decoder block-render callback
// ============================================================
bool onJpgBlock(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (y >= SCR_H) return false;
  tft.pushImage(x, y, w, h, bmp);
  return true;
}

// ============================================================
//  Download a JPEG from `url` and draw it at (ART_X, ART_Y)
// ============================================================
void showAlbumArt(const String& url) {
  if (url.isEmpty()) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
  http.begin(client, url);

  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return; }

  int len = http.getSize();
  if (len <= 0 || len > 80000) { http.end(); return; }

  uint8_t* buf = (uint8_t*)malloc(len);
  if (!buf) { http.end(); return; }

  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  unsigned long deadline = millis() + 10000;

  while (got < (size_t)len && millis() < deadline) {
    size_t avail = stream->available();
    if (avail) {
      got += stream->readBytes(buf + got, min(avail, (size_t)(len - got)));
    } else {
      delay(1);
    }
  }
  http.end();

  if (got == (size_t)len) {
    TJpgDec.drawJpg(ART_X, ART_Y, buf, len);
  }
  free(buf);
}

// ============================================================
//  Truncate a string so it fits within `maxPx` pixels
// ============================================================
String fitText(const String& s, int maxPx) {
  if (tft.textWidth(s) <= maxPx) return s;
  String t = s;
  while (t.length() > 1 && tft.textWidth(t + "..") > maxPx) {
    t.remove(t.length() - 1);
  }
  return t + "..";
}

// ============================================================
//  Draw play / pause icon
// ============================================================
void drawIcon(bool playing) {
  tft.fillRect(ICON_X, ICON_Y, ICON_W, ICON_H, TFT_BLACK);
  if (playing) {
    tft.fillTriangle(ICON_X, ICON_Y,
                     ICON_X, ICON_Y + ICON_H - 1,
                     ICON_X + ICON_W - 1, ICON_Y + ICON_H / 2, TFT_WHITE);
  } else {
    tft.fillRect(ICON_X,            ICON_Y, 4, ICON_H, TFT_WHITE);
    tft.fillRect(ICON_X + ICON_W - 4, ICON_Y, 4, ICON_H, TFT_WHITE);
  }
}

// ============================================================
//  Draw the 2 px progress bar at the bottom
// ============================================================
void drawBar(int progress, int duration) {
  tft.fillRect(0, BAR_Y, SCR_W, BAR_H, TFT_BLACK);
  if (duration > 0) {
    int w = constrain((int)((long)progress * SCR_W / duration), 0, SCR_W);
    if (w > 0) tft.fillRect(0, BAR_Y, w, BAR_H, TFT_WHITE);
  }
}

// ============================================================
//  Draw clock on left panel (below album art area)
// ============================================================
void drawClock() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return;

  char buf[6];
  strftime(buf, sizeof(buf), "%H:%M", &t);
  String timeStr = String(buf);

  if (timeStr == lastTimeStr) return;
  lastTimeStr = timeStr;

  tft.fillRect(CLOCK_X, CLOCK_Y, TXT_W, 16, TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextColor(0x7BEF, TFT_BLACK);
  tft.setCursor(CLOCK_X, CLOCK_Y);
  tft.print(timeStr);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
}

// ============================================================
//  Draw scrolling title into its sprite area
// ============================================================
void drawTitle() {
  titleSpr.fillSprite(TFT_BLACK);
  titleSpr.setFreeFont(&FreeSansBold9pt7b);
  titleSpr.setTextColor(TFT_WHITE, TFT_BLACK);

  if (titlePixelW <= TXT_W) {
    titleSpr.setCursor(0, TITLE_BL);
    titleSpr.print(now.track);
  } else {
    titleSpr.setCursor(-scrollX, TITLE_BL);
    titleSpr.print(now.track);
    titleSpr.setCursor(-scrollX + titlePixelW + SCROLL_GAP, TITLE_BL);
    titleSpr.print(now.track);
  }
  titleSpr.pushSprite(TXT_X, TITLE_Y);
}

// ============================================================
//  Draw idle clock (big HH:MM:SS, centered, low brightness)
// ============================================================
void drawIdleClock() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return;

  char buf[9];
  strftime(buf, sizeof(buf), "%H:%M:%S", &t);
  String timeStr = String(buf);

  if (timeStr == lastTimeStr) return;
  lastTimeStr = timeStr;

  // Clear center area and draw big time
  tft.setTextFont(4);
  int tw = tft.textWidth(timeStr);
  int th = 26;  // Font 4 height
  int cx = (SCR_W - tw) / 2;
  int cy = (SCR_H - th) / 2;
  tft.fillRect(cx - 4, cy - 2, tw + 8, th + 4, TFT_BLACK);
  tft.setTextColor(0x4208, TFT_BLACK);  // dim gray
  tft.setTextDatum(MC_DATUM);
  tft.drawString(timeStr, SCR_W / 2, SCR_H / 2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
}

// ============================================================
//  Draw the right-side panel
// ============================================================
void drawInfo() {
  // Clear right panel and separator area
  tft.fillRect(SEP_X, 0, SCR_W - SEP_X, BAR_Y, TFT_BLACK);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  if (!now.active) {
    // Clear screen, dim backlight, show big clock
    tft.fillScreen(TFT_BLACK);
    analogWrite(BL_PIN, 60);  // Low brightness for idle clock
    drawIdleClock();
    return;
  }

  // Draw shorter separator
  tft.fillRect(SEP_X, SEP_TOP, SEP_W, SEP_BOT - SEP_TOP, TFT_WHITE);

  // Title (scrolling sprite — FreeSansBold9pt7b)
  tft.setFreeFont(&FreeSansBold9pt7b);
  titlePixelW = tft.textWidth(now.track);
  tft.setTextFont(2); // reset back
  scrollX = 0;
  scrollPaused = true;
  scrollPauseAt = millis() + SCROLL_PAUSE_MS;
  drawTitle();

  // Artist
  tft.setTextFont(2);
  tft.setCursor(TXT_X, ARTIST_Y);
  tft.print(fitText(now.artist, TXT_W));

  // Album
  tft.setTextFont(2);
  tft.setCursor(TXT_X, ALBUM_Y);
  tft.print(fitText(now.album, TXT_W));

  // Device
  if (now.device.length() > 0) {
    tft.setTextFont(1);
    tft.setTextColor(0x7BEF, TFT_BLACK); // gray
    tft.setCursor(TXT_X, DEVICE_Y);
    tft.print(fitText(now.device, TXT_W));
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
  }

  drawIcon(now.playing);
  drawBar(now.progress, now.duration);
}

// ============================================================
//  Poll Spotify
// ============================================================
void poll() {
  if (!sp || !spotifyReady) return;

  Serial.printf("[Poll] Free heap: %u bytes\n", ESP.getFreeHeap());

  // Recreate Spotify object if heap is getting low (memory leak workaround)
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

    // If not playing and we're already idle, stay idle
    if (!play && !now.active) {
      Serial.printf("[Poll] Paused & idle — staying idle (%s)\n", trk.c_str());
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

    Serial.printf("[Poll] Track: %s | Artist: %s | Device: %s | Playing: %d\n",
                  trk.c_str(), art.c_str(), dev.c_str(), play);

    if (trackChanged || wasInactive) {
      Serial.println("[Poll] Track changed — redrawing");
      if (wasInactive) {
        analogWrite(BL_PIN, 255);  // Restore full brightness
        lastTimeStr = "";  // Reset clock state
      }
      tft.fillScreen(TFT_BLACK);
      showAlbumArt(img);
      drawInfo();
    } else if (deviceChanged) {
      Serial.printf("[Poll] Device changed → %s\n", dev.c_str());
      drawInfo();
    } else if (playChanged) {
      drawIcon(now.playing);
      drawBar(now.progress, now.duration);
    } else {
      drawBar(now.progress, now.duration);
    }

    // Free JSON memory
    res.reply.clear();

  } else if (res.status_code == 204) {
    Serial.println("[Poll] Nothing playing (204)");
    if (now.active) {
      now = Playback{};
      tft.fillScreen(TFT_BLACK);
      drawInfo();
    }
  } else {
    Serial.printf("[Poll] API error: %d\n", res.status_code);
    // On auth errors, try recreating the client
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
//  Button callbacks
// ============================================================
void onSkip() {
  if (!sp || !spotifyReady) return;
  Serial.println("[Button] Skip →");
  sp->skip();
  delay(400);
  poll();
}

void onPrev() {
  if (!sp || !spotifyReady) return;
  Serial.println("[Button] Previous ←");
  sp->previous();
  delay(400);
  poll();
}

void onPlayPause() {
  if (!sp || !spotifyReady) return;
  Serial.printf("[Button] %s\n", now.playing ? "Pause" : "Play");
  if (now.playing) sp->pause_playback();
  else             sp->start_resume_playback();
  now.playing = !now.playing;
  drawIcon(now.playing);
}

void onScreenToggle() {
  screenOn = !screenOn;
  Serial.printf("[Button] Screen %s\n", screenOn ? "ON" : "OFF");
  if (screenOn) {
    analogWrite(BL_PIN, now.active ? 255 : 60);
    lastTimeStr = "";  // Force clock redraw
    poll();
  } else {
    analogWrite(BL_PIN, 0);
  }
}

void onFlipScreen() {
  screenRotation = (screenRotation == 1) ? 3 : 1;
  prefs.putUChar("rotation", screenRotation);
  tft.setRotation(screenRotation);
  tft.fillScreen(TFT_BLACK);
  Serial.printf("[Button] Rotation flipped to %d\n", screenRotation);
  poll();
}

// ============================================================
//  WiFi auto-reconnect
// ============================================================
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.println("[WiFi] reconnecting...");
  WiFi.reconnect();
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) {
    delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] OK");
  }
}

// ============================================================
//  Centered status message helper
// ============================================================
void showStatus(const char* line1, const char* line2 = nullptr) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(line1, SCR_W / 2, 65);
  if (line2) tft.drawString(line2, SCR_W / 2, 90);
  tft.setTextDatum(TL_DATUM);
}

// ============================================================
//  HTTPS OAuth — callback handler
// ============================================================
static esp_err_t oauth_callback_handler(httpd_req_t* req) {
  char query[1024] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char code[512] = {0};
    if (httpd_query_key_value(query, "code", code, sizeof(code)) == ESP_OK) {
      oauthCode = String(code);
      oauthComplete = true;
      const char* html = "<html><body style='background:#000;color:#fff;font-family:sans-serif;text-align:center;padding-top:80px'>"
                         "<h2>Authorized!</h2><p>You can close this tab.</p></body></html>";
      httpd_resp_set_type(req, "text/html");
      httpd_resp_send(req, html, strlen(html));
      return ESP_OK;
    }
  }
  httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing code parameter");
  return ESP_FAIL;
}

// ============================================================
//  HTTPS OAuth — root handler (shows "click to login" link)
// ============================================================
static esp_err_t oauth_root_handler(httpd_req_t* req) {
  String authUrl = "https://accounts.spotify.com/authorize"
                   "?client_id=" + String(SPOTIFY_CLIENT_ID) +
                   "&response_type=code"
                   "&redirect_uri=" + oauthRedirectUri + "/callback" +
                   "&scope=" SPOTIFY_SCOPES;

  String html = "<html><body style='background:#000;color:#fff;font-family:sans-serif;text-align:center;padding-top:60px'>"
                "<h2>Spotify Display</h2>"
                "<p><a href='" + authUrl + "' style='color:#1DB954;font-size:24px'>Click here to log in with Spotify</a></p>"
                "<p style='color:#888;margin-top:30px;font-size:12px'>Redirect URI for your Spotify app settings:<br>"
                + oauthRedirectUri + "/callback</p></body></html>";

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());
  return ESP_OK;
}

// ============================================================
//  Exchange auth code for refresh token via Spotify token API
// ============================================================
String exchangeCodeForToken(const String& code) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, "https://accounts.spotify.com/api/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  // Build Basic auth header
  String creds = String(SPOTIFY_CLIENT_ID) + ":" + String(SPOTIFY_CLIENT_SECRET);
  char b64[256];
  size_t outLen = 0;
  mbedtls_base64_encode((unsigned char*)b64, sizeof(b64), &outLen,
                        (const unsigned char*)creds.c_str(), creds.length());
  b64[outLen] = 0;
  http.addHeader("Authorization", String("Basic ") + b64);

  String body = "grant_type=authorization_code&code=" + code +
                "&redirect_uri=" + oauthRedirectUri + "/callback";

  int httpCode = http.POST(body);
  String refreshToken = "";

  if (httpCode == 200) {
    JsonDocument doc;
    deserializeJson(doc, http.getString());
    refreshToken = doc["refresh_token"] | "";
    Serial.printf("[OAuth] Got refresh token: %s\n", refreshToken.c_str());
  } else {
    Serial.printf("[OAuth] Token exchange failed: %d\n", httpCode);
    Serial.println(http.getString());
  }

  http.end();
  return refreshToken;
}

// ============================================================
//  Run the full HTTPS OAuth flow on the ESP32
// ============================================================
String runOAuthFlow() {
  // Build redirect URI from our IP
  String ip = WiFi.localIP().toString();
  oauthRedirectUri = "https://" + ip;

  // Configure HTTPS server
  httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
  config.cacert_pem     = (const uint8_t*)server_cert;
  config.cacert_len     = strlen(server_cert) + 1;
  config.prvtkey_pem    = (const uint8_t*)server_key;
  config.prvtkey_len    = strlen(server_key) + 1;
  config.port_secure    = 443;
  config.httpd.max_uri_handlers = 4;
  config.httpd.max_resp_headers = 8;
  config.httpd.stack_size       = 10240;

  httpd_handle_t server = NULL;
  esp_err_t err = httpd_ssl_start(&server, &config);
  if (err != ESP_OK) {
    Serial.printf("[OAuth] HTTPS server failed: %s\n", esp_err_to_name(err));
    return "";
  }

  // Register URI handlers
  httpd_uri_t root_uri     = { .uri = "/",         .method = HTTP_GET, .handler = oauth_root_handler };
  httpd_uri_t callback_uri = { .uri = "/callback",  .method = HTTP_GET, .handler = oauth_callback_handler };
  httpd_register_uri_handler(server, &root_uri);
  httpd_register_uri_handler(server, &callback_uri);

  Serial.printf("[OAuth] HTTPS server running on https://%s\n", ip.c_str());

  // Show instructions on display
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 10);
  tft.print("1. Add redirect URI to Spotify app:");
  tft.setTextColor(0x07E0, TFT_BLACK); // green
  tft.setCursor(10, 32);
  tft.printf("https://%s/callback", ip.c_str());
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 62);
  tft.print("2. Open in browser & accept cert:");
  tft.setTextColor(0x07E0, TFT_BLACK);
  tft.setCursor(10, 84);
  tft.printf("https://%s", ip.c_str());
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 114);
  tft.print("3. Click the Spotify login link");
  tft.setCursor(10, 145);
  tft.setTextFont(1);
  tft.setTextColor(0x7BEF, TFT_BLACK); // gray
  tft.print("Waiting for authorization...");

  // Wait for OAuth callback
  oauthComplete = false;
  oauthCode = "";
  while (!oauthComplete) {
    delay(100);
  }

  // Exchange code for refresh token
  showStatus("Exchanging token...");
  String refreshToken = exchangeCodeForToken(oauthCode);

  // Stop HTTPS server
  httpd_ssl_stop(server);

  return refreshToken;
}

// ============================================================
//  setup()
// ============================================================
void setup() {
  Serial.begin(115200);

  // Power enable (required for T-Display S3 peripherals)
  pinMode(PWR_EN, OUTPUT);
  digitalWrite(PWR_EN, HIGH);

  // Backlight ON
  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, HIGH);

  // ── Read boot-time button state ────────────────────────
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

  // ── WiFi (captive portal) ──────────────────────────────
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

  // ── NTP time sync ────────────────────────────────────────
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
    // No saved token — run HTTPS OAuth flow on the ESP32
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

  // ── Title scroll sprite ────────────────────────────────
  titleSpr.createSprite(TXT_W, TITLE_H);
  titleSpr.setSwapBytes(true);

  // ── Buttons ────────────────────────────────────────────
  topBtn.attachClick(onSkip);
  topBtn.attachDoubleClick(onPrev);
  topBtn.attachLongPressStart(onFlipScreen);
  botBtn.attachClick(onPlayPause);
  botBtn.attachDoubleClick(onScreenToggle);

  // ── First draw ─────────────────────────────────────────
  tft.fillScreen(TFT_BLACK);
  poll();
}

// ============================================================
//  loop()
// ============================================================
void loop() {
  topBtn.tick();
  botBtn.tick();

  unsigned long ms = millis();

  // Always poll (keeps state fresh even when screen off)
  if (ms - lastPoll >= POLL_MS) {
    lastPoll = ms;
    if (screenOn) poll();
  }

  if (screenOn && now.active && now.playing && ms - lastBar >= BAR_MS) {
    lastBar = ms;
    int elapsed = ms - now.pollTime;
    int cur = min(now.progress + (int)elapsed, now.duration);
    drawBar(cur, now.duration);
  }

  // Title scrolling
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

  // Clock update
  if (screenOn && ms - lastClock >= CLOCK_MS) {
    lastClock = ms;
    if (now.active) {
      drawClock();
    } else {
      drawIdleClock();
    }
  }

  if (ms - lastWifi >= WIFI_MS) {
    lastWifi = ms;
    ensureWiFi();
  }
}
