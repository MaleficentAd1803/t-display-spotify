// ============================================================
//  Lyrics fetch + LRC parser (runs on core 0)
//  Source: LRCLIB (https://lrclib.net) — free, no auth, synced LRC.
// ============================================================

#include "config.h"

// ── Global lyric state ──────────────────────────────────
LyricLine lyrics[MAX_LYRIC_LINES];
int       numLyrics = 0;
String    lyricsTrackId;
bool      lyricsTriedCurrent = false;

// TLS client is created fresh each fetch (runs at most once per track change)
// and torn down at the end so ~30KB of mbedTLS state doesn't sit in the
// internal heap between tracks.

// ── percent-encode for URL query ────────────────────────
static String urlEncode(const String& s) {
  String out;
  out.reserve(s.length() * 2);
  char buf[4];
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = (unsigned char)s[i];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      out += (char)c;
    } else {
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

// Parse "[MM:SS.xx]" or "[MM:SS.xxx]" prefix. Returns ms or -1 on miss.
// On success, *textOut points at first char after the "]".
static int parseTimestamp(const char* p, const char** textOut) {
  if (*p != '[') return -1;
  p++;
  int mm = 0, ss = 0, frac = 0, fracDigits = 0;
  while (*p >= '0' && *p <= '9') { mm = mm * 10 + (*p - '0'); p++; }
  if (*p != ':') return -1;
  p++;
  while (*p >= '0' && *p <= '9') { ss = ss * 10 + (*p - '0'); p++; }
  if (*p == '.' || *p == ':') {
    p++;
    while (*p >= '0' && *p <= '9' && fracDigits < 3) {
      frac = frac * 10 + (*p - '0');
      fracDigits++;
      p++;
    }
    while (*p >= '0' && *p <= '9') p++;  // skip any extras
  }
  if (*p != ']') return -1;
  p++;
  if (fracDigits == 2) frac *= 10;       // xx -> xxx
  else if (fracDigits == 1) frac *= 100;
  else if (fracDigits == 0) frac = 0;
  *textOut = p;
  return mm * 60000 + ss * 1000 + frac;
}

// Parse LRC blob into lyrics[] array, overwriting previous content.
// Lines with no timestamp are skipped. Sorted by timeMs.
static void parseLrc(const String& lrc) {
  numLyrics = 0;
  int len = lrc.length();
  int i = 0;
  while (i < len && numLyrics < MAX_LYRIC_LINES) {
    // skip whitespace at line start
    while (i < len && (lrc[i] == '\r' || lrc[i] == '\n')) i++;
    if (i >= len) break;

    int lineEnd = i;
    while (lineEnd < len && lrc[lineEnd] != '\n' && lrc[lineEnd] != '\r') lineEnd++;

    // Extract this line (may have multiple [timestamp] prefixes, rare)
    String line = lrc.substring(i, lineEnd);
    i = lineEnd;

    const char* p = line.c_str();
    // Handle chained timestamps: "[00:10.00][00:20.00]text"
    int timestamps[8];
    int tsCount = 0;
    const char* textStart = p;
    while (tsCount < 8) {
      const char* next;
      int t = parseTimestamp(textStart, &next);
      if (t < 0) break;
      timestamps[tsCount++] = t;
      textStart = next;
    }
    if (tsCount == 0) continue;

    // trim leading whitespace of text
    while (*textStart == ' ' || *textStart == '\t') textStart++;

    // Skip empty metadata-only lines ("[ar:...]" etc still have text after, but
    // those don't match MM:SS — parseTimestamp rejects them since mm is bounded)
    for (int k = 0; k < tsCount && numLyrics < MAX_LYRIC_LINES; k++) {
      LyricLine& L = lyrics[numLyrics++];
      L.timeMs = timestamps[k];
      // copy text with truncation
      size_t tl = strnlen(textStart, LYRIC_TEXT_LEN - 1);
      memcpy(L.text, textStart, tl);
      L.text[tl] = 0;
    }
  }

  // Sort by timeMs (insertion sort — small N)
  for (int a = 1; a < numLyrics; a++) {
    LyricLine tmp = lyrics[a];
    int b = a - 1;
    while (b >= 0 && lyrics[b].timeMs > tmp.timeMs) {
      lyrics[b + 1] = lyrics[b];
      b--;
    }
    lyrics[b + 1] = tmp;
  }
}

// ── Fetch lyrics for the currently playing track ────────
void fetchLyrics() {
  String track, artist, album, trackId;
  int    durationMs;
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  track      = now.track;
  artist     = now.artist;
  album      = now.album;
  trackId    = now.trackId;
  durationMs = now.duration;
  xSemaphoreGive(dataMutex);

  if (trackId.length() == 0 || track.length() == 0) {
    numLyrics = 0;
    lyricsTrackId = "";
    lyricsTriedCurrent = true;
    redrawFlags |= RFLAG_LYRICS_READY;
    return;
  }

  WiFiClientSecure lrcClient;
  HTTPClient       lrcHttp;
  lrcClient.setInsecure();
  lrcHttp.setTimeout(8000);
  lrcHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  String url = "https://lrclib.net/api/get?track_name=" + urlEncode(track) +
               "&artist_name=" + urlEncode(artist);
  if (album.length() > 0) url += "&album_name=" + urlEncode(album);
  if (durationMs > 0)     url += "&duration=" + String(durationMs / 1000);

  unsigned long t0 = millis();
  lrcHttp.begin(lrcClient, url);
  lrcHttp.addHeader("User-Agent", "t-display-spotify/1.0");
  int code = lrcHttp.GET();

  if (code == HTTP_CODE_OK) {
    // Filter — we only want syncedLyrics (and plainLyrics as fallback flag)
    JsonDocument filter;
    filter["syncedLyrics"] = true;
    filter["plainLyrics"]  = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, lrcHttp.getStream(),
                                               DeserializationOption::Filter(filter));
    lrcHttp.end();
    if (err) {
      Serial.printf("[Lyrics] JSON err: %s (%lums)\n", err.c_str(), millis() - t0);
      numLyrics = 0;
    } else {
      const char* synced = doc["syncedLyrics"] | (const char*)nullptr;
      if (synced && strlen(synced) > 0) {
        parseLrc(String(synced));
        Serial.printf("[Lyrics] %d lines for %s (%lums)\n",
                      numLyrics, track.c_str(), millis() - t0);
      } else {
        numLyrics = 0;
        Serial.printf("[Lyrics] No synced lyrics for %s (%lums)\n",
                      track.c_str(), millis() - t0);
      }
    }
  } else if (code == 404) {
    lrcHttp.end();
    numLyrics = 0;
    Serial.printf("[Lyrics] 404 (not indexed) for %s (%lums)\n",
                  track.c_str(), millis() - t0);
  } else {
    lrcHttp.end();
    numLyrics = 0;
    Serial.printf("[Lyrics] HTTP %d (%lums)\n", code, millis() - t0);
  }

  lyricsTrackId = trackId;
  lyricsTriedCurrent = true;
  redrawFlags |= RFLAG_LYRICS_READY;
}
