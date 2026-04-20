// ============================================================
//  Display drawing functions (all run on core 1)
// ============================================================

#include "config.h"
#include <esp_task_wdt.h>

// ── TJpg_Decoder block-render callback ──────────────────
bool onJpgBlock(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (y >= SCR_H) return false;
  tft.pushImage(x, y, w, h, bmp);
  return true;
}

// ── Persistent TLS client for album art (avoids repeated handshakes) ──
static WiFiClientSecure artClient;
static HTTPClient       artHttp;
static bool             artClientInit = false;

// Tear down the keep-alive TLS socket. Callers use this to free mbedtls
// memory (~30KB) before opening a new TLS connection elsewhere.
void stopAlbumArtClient() {
  if (!artClientInit) return;
  artHttp.end();
  artClient.stop();
}

// ── Download & draw album art JPEG ──────────────────────
void showAlbumArt(const String& url) {
  if (url.isEmpty()) return;

  if (!artClientInit) {
    artClient.setInsecure();
    artHttp.setReuse(true);  // Keep TCP+TLS connection alive
    artHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    artHttp.setTimeout(10000);
    artClientInit = true;
  }

  unsigned long t0 = millis();
  artHttp.begin(artClient, url);
  int code = artHttp.GET();
  // A kept-alive TLS socket can be closed by the CDN during long idle periods.
  // Retry once on a fresh connection before giving up.
  if (code != HTTP_CODE_OK) {
    Serial.printf("[Art] HTTP %d — retrying fresh (%lums)\n", code, millis() - t0);
    artHttp.end();
    artClient.stop();
    artHttp.begin(artClient, url);
    code = artHttp.GET();
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("[Art] HTTP %d (%lums)\n", code, millis() - t0);
    artHttp.end();
    return;
  }

  int len = artHttp.getSize();
  if (len <= 0 || len > 300000) { artHttp.end(); return; }

  // Prefer PSRAM (8MB on T-Display S3) so we don't fight the ~300KB internal
  // heap — leaves room for TJpgDec work buffers and TLS. Falls back to
  // internal heap if PSRAM is absent or exhausted.
  uint8_t* buf = (uint8_t*)ps_malloc(len);
  if (!buf) {
    unsigned heap = ESP.getFreeHeap();
    if ((unsigned)len + 16000 > heap) {
      Serial.printf("[Art] Skipping — need %d+16000, heap %u, psram %u\n",
                    len, heap, (unsigned)ESP.getFreePsram());
      artHttp.end();
      return;
    }
    buf = (uint8_t*)malloc(len);
    if (!buf) {
      Serial.printf("[Art] alloc(%d) failed, heap=%u psram=%u\n",
                    len, heap, (unsigned)ESP.getFreePsram());
      artHttp.end();
      return;
    }
  }

  WiFiClient* stream = artHttp.getStreamPtr();
  size_t got = 0;
  unsigned long deadline = millis() + 10000;

  while (got < (size_t)len && millis() < deadline) {
    size_t avail = stream->available();
    if (avail) {
      got += stream->readBytes(buf + got, min(avail, (size_t)(len - got)));
    } else {
      delay(10);
    }
    esp_task_wdt_reset();
  }

  Serial.printf("[Art] %u bytes in %lums\n", got, millis() - t0);

  if (got == (size_t)len) {
    TJpgDec.drawJpg(ART_X, ART_Y, buf, len);
  }
  free(buf);
  // Don't call artHttp.end() — keep connection alive for next image
}

// ── Truncate string to fit pixel width ──────────────────
String fitText(const String& s, int maxPx) {
  if (tft.textWidth(s) <= maxPx) return s;
  int dotsW = tft.textWidth("..");
  int budget = maxPx - dotsW;
  if (budget <= 0) return "..";
  String t = s;
  while (t.length() > 1 && tft.textWidth(t) > budget) {
    t.remove(t.length() - 1);
  }
  return t + "..";
}

// ── 12×12 CPU chip icon (body outline, center die, 3 pins per side) ──
void drawCpuIcon(int x, int y, uint16_t color) {
  tft.drawRect(x + 1, y + 1, 10, 10, color);
  tft.drawRect(x + 4, y + 4, 4, 4, color);
  for (int i = 0; i < 3; i++) {
    int off = 2 + i * 3;
    tft.drawPixel(x + off, y,      color);
    tft.drawPixel(x + off, y + 11, color);
    tft.drawPixel(x,       y + off, color);
    tft.drawPixel(x + 11,  y + off, color);
  }
}

// ── CPU icon + temperature (e.g. "45.3C") ───────────────
void drawCpuTemp(int x, int y, float tempC, uint16_t color) {
  tft.fillRect(x, y, CPU_TEMP_W, CPU_ICON_SZ, TFT_BLACK);
  drawCpuIcon(x, y, color);
  char buf[12];
  snprintf(buf, sizeof(buf), "%.1fC", tempC);
  tft.setFreeFont(&FreeSans5pt8b);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(x + CPU_ICON_SZ + 3, y + 9);
  tft.print(buf);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
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
  tft.setTextColor(COLOR_DIM_GREY, TFT_BLACK);
  tft.setCursor(CLOCK_X, CLOCK_Y);
  tft.print(timeStr);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
}

// ── Scrolling title sprite ──────────────────────────────
void drawTitle() {
  titleSpr.fillSprite(TFT_BLACK);
  titleSpr.setFreeFont(&FreeSansBold9pt8b);
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
  int th = 26;
  // Use fixed clear width to avoid residue from proportional font width changes
  int clearW = tft.textWidth("00:00:00") + 12;
  int cy = (SCR_H - th) / 2;
  tft.fillRect((SCR_W - clearW) / 2, cy - 2, clearW, th + 4, TFT_BLACK);
  tft.setTextColor(COLOR_DARK_GREY, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(timeStr, SCR_W / 2, SCR_H / 2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
}

// ── Right-side info panel ───────────────────────────────
void drawInfo() {
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  if (!now.active) {
    tft.fillScreen(TFT_BLACK);
    drawIdleClock();
    // Show config URL
    tft.setTextFont(1);
    tft.setTextColor(COLOR_VERY_DARK, TFT_BLACK);
    tft.setCursor(4, 4);
    tft.print("http://" + WiFi.localIP().toString());
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    drawCpuTemp(CPU_TEMP_IDLE_X, CPU_TEMP_IDLE_Y, cpuTempC, COLOR_DIM_GREY);
    tickerScrollX = 0;
    return;
  }

  tft.fillRect(SEP_X, 0, SCR_W - SEP_X, BAR_Y, TFT_BLACK);
  tft.fillRect(SEP_X, SEP_TOP, SEP_W, SEP_BOT - SEP_TOP, TFT_WHITE);

  tft.setFreeFont(&FreeSansBold9pt8b);
  titlePixelW = tft.textWidth(now.track);
  tft.setTextFont(2);
  scrollX = 0;
  scrollPaused = true;
  scrollPauseAt = millis() + SCROLL_PAUSE_MS;
  drawTitle();

  tft.setFreeFont(&FreeSans8pt8b);
  tft.setCursor(TXT_X, ARTIST_Y + 9);
  tft.print(fitText(now.artist, TXT_W));

  tft.setFreeFont(&FreeSans8pt8b);
  tft.setCursor(TXT_X, ALBUM_Y + 9);
  tft.print(fitText(now.album, TXT_W));

  if (now.device.length() > 0) {
    tft.setFreeFont(&FreeSans5pt8b);
    tft.setTextColor(COLOR_DIM_GREY, TFT_BLACK);
    tft.setCursor(TXT_X, DEVICE_Y + 7);
    tft.print(fitText(now.device, TXT_W));
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
  }

  drawIcon(now.playing);
  drawBar(now.progress, now.duration);
  drawCpuTemp(CPU_TEMP_PLAY_X, CPU_TEMP_PLAY_Y, cpuTempC, TFT_WHITE);

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
