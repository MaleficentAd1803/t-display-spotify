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
  drawCpuTemp(CPU_TEMP_PLAY_X, CPU_TEMP_PLAY_Y, cpuTempC, COLOR_DIM_GREY);

  lastTimeStr = "";
  drawClock();
}

// ── Lyrics page ─────────────────────────────────────────
// Layout (320x170):
//   Top 42px:  track title (bold) + artist (dim)
//   y=43:      1px separator
//   y=45..170: lyrics area (125px) — previous / current / next lines, centered
#define LYR_HDR_H    42
#define LYR_AREA_Y   45
#define LYR_AREA_H   (SCR_H - LYR_AREA_Y)

static int  lastLyricIdx = -2;   // -2 forces redraw on first call
static bool lastHadLyrics = false;

// Split s into two lines, each fitting maxPx in the CURRENT TFT font.
// Returns:
//   WRAP_NONE       — whole string fits on one line (b is empty).
//   WRAP_OK         — split cleanly into two lines, both fit.
//   WRAP_TRUNCATED  — second line had to be truncated with ".." to fit.
// Prefers word boundaries; falls back to mid-word split.
enum WrapResult { WRAP_NONE, WRAP_OK, WRAP_TRUNCATED };
static WrapResult wrapTwoLines(const String& s, int maxPx, String& a, String& b) {
  if (tft.textWidth(s) <= maxPx) { a = s; b = ""; return WRAP_NONE; }

  int len = (int)s.length();
  int best = -1;  // byte index of the space we'll break on
  for (int i = 1; i < len; i++) {
    if (s[i] != ' ') continue;
    if (tft.textWidth(s.substring(0, i)) <= maxPx) best = i;
    else break;  // prefix widths are monotonic — further splits are worse
  }

  if (best > 0) {
    a = s.substring(0, best);
    b = s.substring(best + 1);  // skip the space
  } else {
    // No usable word boundary — hard-split character-wise
    int cut = len;
    while (cut > 1 && tft.textWidth(s.substring(0, cut)) > maxPx) cut--;
    a = s.substring(0, cut);
    b = s.substring(cut);
  }
  if (tft.textWidth(b) > maxPx) { b = fitText(b, maxPx); return WRAP_TRUNCATED; }
  return WRAP_OK;
}

static int findLyricIdx() {
  if (numLyrics == 0) return -1;
  int curMs = now.progress + LYRIC_LEAD_MS;
  if (now.playing) curMs += (int)(millis() - now.pollTime);
  int idx = 0;
  for (int i = 0; i < numLyrics; i++) {
    if (lyrics[i].timeMs <= curMs) idx = i;
    else break;
  }
  // If we're before the first timestamp, show a blank lead-in (idx = -1 style)
  if (lyrics[0].timeMs > curMs) return -1;
  return idx;
}

static void drawLyricsHeader() {
  tft.fillRect(0, 0, SCR_W, LYR_HDR_H + 1, TFT_BLACK);
  tft.setFreeFont(&FreeSansBold9pt8b);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(6, 17);
  tft.print(fitText(now.track, SCR_W - 12));
  tft.setFreeFont(&FreeSans8pt8b);
  tft.setTextColor(COLOR_DIM_GREY, TFT_BLACK);
  tft.setCursor(6, 37);
  tft.print(fitText(now.artist, SCR_W - 12));
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawFastHLine(0, LYR_HDR_H + 1, SCR_W, COLOR_DARK_GREY);
}

// Row bounding boxes (centered on each y). Clearing per-row instead of the
// whole body cuts the black-flash area by ~6x, making line-changes much less
// flashy than a full fillRect.
#define LYR_ROW_HALF_THIN    12   // prev / next lines (8pt)
#define LYR_ROW_HALF_THICK   26   // current line (may be 2 rows, covers descenders)
#define LYR_WRAP_OFFSET      12   // vertical offset from yCur for each wrapped row

static void drawLyricsBody() {
  tft.setTextDatum(MC_DATUM);
  int cx = SCR_W / 2;

  if (numLyrics == 0) {
    tft.fillRect(0, LYR_AREA_Y, SCR_W, LYR_AREA_H, TFT_BLACK);
    tft.setFreeFont(&FreeSans8pt8b);
    tft.setTextColor(COLOR_DIM_GREY, TFT_BLACK);
    const char* msg = lyricsTriedCurrent ? "No lyrics available" : "Loading lyrics...";
    tft.drawString(msg, cx, LYR_AREA_Y + LYR_AREA_H / 2);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    return;
  }

  int idx = findLyricIdx();

  // y-positions inside the 125px area: prev=20, current=62, next=104
  int yPrev = LYR_AREA_Y + 20;
  int yCur  = LYR_AREA_Y + 62;
  int yNext = LYR_AREA_Y + 104;

  // previous line (dim) — clear just its row
  tft.fillRect(0, yPrev - LYR_ROW_HALF_THIN, SCR_W, LYR_ROW_HALF_THIN * 2, TFT_BLACK);
  if (idx > 0) {
    tft.setFreeFont(&FreeSans8pt8b);
    tft.setTextColor(COLOR_DIM_GREY, TFT_BLACK);
    tft.drawString(fitText(String(lyrics[idx - 1].text), SCR_W - 8), cx, yPrev);
  }
  // current line (bold white) — wraps into up to two rows; if still too long,
  // fall back to a smaller font before truncating. Clear only its row band.
  tft.fillRect(0, yCur - LYR_ROW_HALF_THICK, SCR_W, LYR_ROW_HALF_THICK * 2, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  const char* t = (idx < 0) ? "..." : lyrics[idx].text;
  if (t[0] == 0) t = "...";
  String a, b;
  tft.setFreeFont(&FreeSansBold9pt8b);
  WrapResult r = wrapTwoLines(String(t), SCR_W - 8, a, b);
  if (r == WRAP_TRUNCATED) {
    // Try again with the smaller 8pt font (fits ~25% more characters).
    tft.setFreeFont(&FreeSans8pt8b);
    r = wrapTwoLines(String(t), SCR_W - 8, a, b);
  }
  if (r == WRAP_NONE) {
    tft.drawString(a, cx, yCur);
  } else {
    tft.drawString(a, cx, yCur - 10);
    tft.drawString(b, cx, yCur + 10);
  }
  // next line (dim) — clear just its row
  tft.fillRect(0, yNext - LYR_ROW_HALF_THIN, SCR_W, LYR_ROW_HALF_THIN * 2, TFT_BLACK);
  int nextI = (idx < 0) ? 0 : idx + 1;
  if (nextI < numLyrics) {
    tft.setFreeFont(&FreeSans8pt8b);
    tft.setTextColor(COLOR_DIM_GREY, TFT_BLACK);
    tft.drawString(fitText(String(lyrics[nextI].text), SCR_W - 8), cx, yNext);
  }

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
}

void drawLyricsPage() {
  tft.fillScreen(TFT_BLACK);
  drawLyricsHeader();
  drawLyricsBody();
  lastLyricIdx  = findLyricIdx();
  lastHadLyrics = (numLyrics > 0);
}

// Called frequently from loop — only redraws body if the active line changed.
void drawLyricsUpdate() {
  bool hasLyrics = (numLyrics > 0);
  int  idx = findLyricIdx();
  if (hasLyrics == lastHadLyrics && idx == lastLyricIdx) return;
  lastHadLyrics = hasLyrics;
  lastLyricIdx  = idx;
  drawLyricsBody();
}

// Dispatch based on currentPage and playback state. Lyrics page falls back to
// the now-playing/idle view when nothing is playing.
void drawPage() {
  if (currentPage == PAGE_LYRICS && now.active) {
    drawLyricsPage();
  } else {
    drawInfo();
  }
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
