// ============================================================
//  Display drawing functions (all run on core 1)
// ============================================================

#include "config.h"

// ── TJpg_Decoder block-render callback ──────────────────
bool onJpgBlock(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (y >= SCR_H) return false;
  tft.pushImage(x, y, w, h, bmp);
  return true;
}

// ── Download & draw album art JPEG ──────────────────────
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

// ── Truncate string to fit pixel width ──────────────────
String fitText(const String& s, int maxPx) {
  if (tft.textWidth(s) <= maxPx) return s;
  String t = s;
  while (t.length() > 1 && tft.textWidth(t + "..") > maxPx) {
    t.remove(t.length() - 1);
  }
  return t + "..";
}

// ── Play / pause icon ───────────────────────────────────
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

// ── Progress bar ────────────────────────────────────────
void drawBar(int progress, int duration) {
  tft.fillRect(0, BAR_Y, SCR_W, BAR_H, TFT_BLACK);
  if (duration > 0) {
    int w = constrain((int)((long)progress * SCR_W / duration), 0, SCR_W);
    if (w > 0) tft.fillRect(0, BAR_Y, w, BAR_H, TFT_WHITE);
  }
}

// ── Small clock (right panel, active playback) ──────────
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

// ── Scrolling title sprite ──────────────────────────────
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

// ── Idle clock (big centered HH:MM:SS) ──────────────────
void drawIdleClock() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return;

  char buf[9];
  strftime(buf, sizeof(buf), "%H:%M:%S", &t);
  String timeStr = String(buf);

  if (timeStr == lastTimeStr) return;
  lastTimeStr = timeStr;

  tft.setTextFont(4);
  int tw = tft.textWidth(timeStr);
  int th = 26;
  int cx = (SCR_W - tw) / 2;
  int cy = (SCR_H - th) / 2;
  tft.fillRect(cx - 4, cy - 2, tw + 8, th + 4, TFT_BLACK);
  tft.setTextColor(0x4208, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(timeStr, SCR_W / 2, SCR_H / 2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
}

// ── Right-side info panel ───────────────────────────────
void drawInfo() {
  tft.fillRect(SEP_X, 0, SCR_W - SEP_X, BAR_Y, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  if (!now.active) {
    tft.fillScreen(TFT_BLACK);
    analogWrite(BL_PIN, 128);
    drawIdleClock();
    // Show config URL
    tft.setTextFont(1);
    tft.setTextColor(0x3186, TFT_BLACK);
    tft.setCursor(4, 4);
    tft.print("http://" + WiFi.localIP().toString());
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tickerScrollX = 0;
    return;
  }

  tft.fillRect(SEP_X, SEP_TOP, SEP_W, SEP_BOT - SEP_TOP, TFT_WHITE);

  tft.setFreeFont(&FreeSansBold9pt7b);
  titlePixelW = tft.textWidth(now.track);
  tft.setTextFont(2);
  scrollX = 0;
  scrollPaused = true;
  scrollPauseAt = millis() + SCROLL_PAUSE_MS;
  drawTitle();

  tft.setTextFont(2);
  tft.setCursor(TXT_X, ARTIST_Y);
  tft.print(fitText(now.artist, TXT_W));

  tft.setTextFont(2);
  tft.setCursor(TXT_X, ALBUM_Y);
  tft.print(fitText(now.album, TXT_W));

  if (now.device.length() > 0) {
    tft.setTextFont(1);
    tft.setTextColor(0x7BEF, TFT_BLACK);
    tft.setCursor(TXT_X, DEVICE_Y);
    tft.print(fitText(now.device, TXT_W));
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
  }

  drawIcon(now.playing);
  drawBar(now.progress, now.duration);

  lastTimeStr = "";
  drawClock();
}

// ── Centered status message ─────────────────────────────
void showStatus(const char* line1, const char* line2) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(line1, SCR_W / 2, 65);
  if (line2) tft.drawString(line2, SCR_W / 2, 90);
  tft.setTextDatum(TL_DATUM);
}
