// ============================================================
//  Ticker: price fetching (core 0) and rendering (core 1)
// ============================================================

#include "config.h"

// ── Lookup CoinGecko ID for a crypto symbol ─────────────
const char* getCoinGeckoId(const char* sym) {
  for (int i = 0; i < (int)CRYPTO_MAP_SIZE; i++) {
    if (strcasecmp(sym, CRYPTO_MAP[i].sym) == 0) return CRYPTO_MAP[i].id;
  }
  return nullptr;
}

// ── Lookup Finnhub symbol for a commodity ────────────────
const char* getCommodityFinnhubSymbol(const char* sym) {
  for (int i = 0; i < (int)COMMODITY_MAP_SIZE; i++) {
    if (strcasecmp(sym, COMMODITY_MAP[i].sym) == 0) return COMMODITY_MAP[i].finnhubSym;
  }
  return nullptr;
}

// ── Load ticker list from NVS ───────────────────────────
void loadTickers() {
  String list = prefs.getString("tickers", "NVDA,LMT,PLTR,BTC,XMR,ETH");
  Serial.printf("[Ticker] List: %s\n", list.c_str());
  numTickers = 0;
  int start = 0;
  while (start < (int)list.length() && numTickers < MAX_TICKERS) {
    int comma = list.indexOf(',', start);
    if (comma < 0) comma = list.length();
    String sym = list.substring(start, comma);
    sym.trim();
    sym.toUpperCase();
    if (sym.length() > 0 && sym.length() < 8) {
      strncpy(tickerItems[numTickers].symbol, sym.c_str(), 7);
      tickerItems[numTickers].symbol[7] = 0;
      tickerItems[numTickers].price = 0;
      tickerItems[numTickers].change = 0;
      tickerItems[numTickers].valid = false;
      tickerItems[numTickers].isCrypto = (getCoinGeckoId(sym.c_str()) != nullptr);
      tickerItems[numTickers].isCommodity = (getCommodityFinnhubSymbol(sym.c_str()) != nullptr);
      numTickers++;
    }
    start = comma + 1;
  }
  Serial.printf("[Ticker] Loaded %d tickers\n", numTickers);
}

// ── Fetch crypto prices from CoinGecko (batch) ─────────
void fetchCryptoPrices() {
  String ids = "";
  for (int i = 0; i < numTickers; i++) {
    if (!tickerItems[i].isCrypto) continue;
    const char* cgId = getCoinGeckoId(tickerItems[i].symbol);
    if (!cgId) continue;
    if (ids.length() > 0) ids += ",";
    ids += cgId;
  }
  if (ids.length() == 0) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.coingecko.com/api/v3/simple/price?ids=" + ids +
               "&vs_currencies=usd&include_24hr_change=true";
  http.setTimeout(10000);
  http.begin(client, url);
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "ESP32");
  int code = http.GET();
  Serial.printf("[Ticker] CoinGecko HTTP %d\n", code);
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    deserializeJson(doc, payload);
    for (int i = 0; i < numTickers; i++) {
      if (!tickerItems[i].isCrypto) continue;
      const char* cgId = getCoinGeckoId(tickerItems[i].symbol);
      if (!cgId) continue;
      float p = doc[cgId]["usd"] | 0.0f;
      float c = doc[cgId]["usd_24h_change"] | 0.0f;
      if (p > 0) {
        tickerItems[i].price = p;
        tickerItems[i].change = c;
        tickerItems[i].valid = true;
        Serial.printf("[Ticker] %s = $%.2f (%.1f%%)\n", tickerItems[i].symbol, p, c);
      }
    }
    doc.clear();
  } else {
    Serial.printf("[Ticker] CoinGecko error: %s\n", http.getString().c_str());
  }
  http.end();
}

// ── Fetch stock/commodity prices from Finnhub ───────────
void fetchStockPrices() {
  if (stockApiKey.length() == 0) return;

  for (int i = 0; i < numTickers; i++) {
    if (tickerItems[i].isCrypto) continue;

    // Use mapped Finnhub symbol for commodities, raw symbol for stocks
    String symbol;
    if (tickerItems[i].isCommodity) {
      const char* mapped = getCommodityFinnhubSymbol(tickerItems[i].symbol);
      if (!mapped) continue;
      symbol = mapped;
    } else {
      symbol = tickerItems[i].symbol;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(10000);
    String url = "https://finnhub.io/api/v1/quote?symbol=";
    url += symbol;
    url += "&token=";
    url += stockApiKey;
    http.begin(client, url);
    int code = http.GET();
    Serial.printf("[Ticker] Finnhub %s HTTP %d\n", tickerItems[i].symbol, code);
    if (code == 200) {
      String payload = http.getString();
      JsonDocument doc;
      deserializeJson(doc, payload);
      float price = doc["c"] | 0.0f;
      float pct   = doc["dp"] | 0.0f;
      if (price > 0) {
        tickerItems[i].price = price;
        tickerItems[i].change = pct;
        tickerItems[i].valid = true;
        Serial.printf("[Ticker] %s = $%.2f (%.1f%%)\n", tickerItems[i].symbol, price, pct);
      }
      doc.clear();
    }
    http.end();
  }
}

// ── Recalculate total scroll width (core 1 only) ────────
void recalcTickerWidth() {
  tickerTextW = 0;
  tft.setTextFont(2);
  for (int i = 0; i < numTickers; i++) {
    if (!tickerItems[i].valid) continue;
    char buf[32];
    if (tickerItems[i].price >= 1000)
      snprintf(buf, sizeof(buf), "%s $%.0f ", tickerItems[i].symbol, tickerItems[i].price);
    else if (tickerItems[i].price >= 1)
      snprintf(buf, sizeof(buf), "%s $%.2f ", tickerItems[i].symbol, tickerItems[i].price);
    else
      snprintf(buf, sizeof(buf), "%s $%.4f ", tickerItems[i].symbol, tickerItems[i].price);
    tickerTextW += tft.textWidth(buf);
    char chg[16];
    snprintf(chg, sizeof(chg), "%s%.1f%%", tickerItems[i].change >= 0 ? "+" : "", tickerItems[i].change);
    tickerTextW += tft.textWidth(chg) + TICKER_GAP;
  }
  tickerReady = (tickerTextW > 0);
  Serial.printf("[Ticker] Width=%d ready=%d\n", tickerTextW, tickerReady);
}

// ── Draw ticker items at x offset ───────────────────────
int drawTickerItemsAt(int startX) {
  int x = startX;
  for (int i = 0; i < numTickers; i++) {
    if (!tickerItems[i].valid) continue;
    char buf[32];
    if (tickerItems[i].price >= 1000)
      snprintf(buf, sizeof(buf), "%s $%.0f ", tickerItems[i].symbol, tickerItems[i].price);
    else if (tickerItems[i].price >= 1)
      snprintf(buf, sizeof(buf), "%s $%.2f ", tickerItems[i].symbol, tickerItems[i].price);
    else
      snprintf(buf, sizeof(buf), "%s $%.4f ", tickerItems[i].symbol, tickerItems[i].price);
    tickerSpr.setTextColor(0xC618, TFT_BLACK);
    tickerSpr.setCursor(x, 0);
    tickerSpr.print(buf);
    x += tickerSpr.textWidth(buf);
    char chg[16];
    snprintf(chg, sizeof(chg), "%s%.1f%%", tickerItems[i].change >= 0 ? "+" : "", tickerItems[i].change);
    tickerSpr.setTextColor(tickerItems[i].change >= 0 ? 0x07E0 : 0xF800, TFT_BLACK);
    tickerSpr.setCursor(x, 0);
    tickerSpr.print(chg);
    x += tickerSpr.textWidth(chg) + TICKER_GAP;
  }
  return x;
}

// ── Draw one frame of scrolling ticker ──────────────────
void drawTicker() {
  if (!tickerReady || numTickers == 0) return;
  tickerSpr.fillSprite(TFT_BLACK);
  tickerSpr.setTextFont(2);
  if (tickerTextW <= SCR_W) {
    drawTickerItemsAt(0);
  } else {
    // Second copy starts exactly at tickerTextW (gap already included)
    drawTickerItemsAt(-tickerScrollX);
    drawTickerItemsAt(-tickerScrollX + tickerTextW);
  }
  tickerSpr.pushSprite(0, TICKER_Y);
}
