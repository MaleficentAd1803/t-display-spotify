// ============================================================
//  Network: OAuth flow, config web server, WiFi, serial input
// ============================================================

#include "config.h"
#include <esp_https_server.h>
#include <mbedtls/base64.h>
#include "certs.h"

// ── OAuth state ─────────────────────────────────────────
static String oauthCode       = "";
static bool   oauthComplete   = false;
static String oauthRedirectUri = "";

// ── Config web server handle ────────────────────────────
static httpd_handle_t configServer = NULL;

// ── Config Web UI HTML (stored in flash) ────────────────
static const char CONFIG_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Ticker Config</title><style>
*{box-sizing:border-box}body{background:#121212;color:#fff;font-family:-apple-system,sans-serif;margin:0 auto;padding:16px;max-width:420px}
h1{color:#1DB954;font-size:1.3em}
.item{display:flex;align-items:center;background:#282828;border-radius:8px;padding:10px;margin-bottom:6px}
.sym{flex:1;font-weight:bold}.tag{font-size:.7em;padding:2px 6px;border-radius:4px;margin-right:8px}
.c{background:#2d4a3e;color:#1DB954}.s{background:#3a3a2d;color:#e8b93c}.o{background:#3a2d2d;color:#e8933c}
input[type=range]{accent-color:#1DB954;width:100%}input[type=checkbox]{accent-color:#1DB954}
button{background:#333;color:#fff;border:none;border-radius:6px;padding:8px 10px;cursor:pointer;margin-left:3px}
button:active{background:#555}.del{color:#f44}
.sv{background:#1DB954;color:#000;font-weight:bold;width:100%;padding:14px;font-size:1.1em;border-radius:8px;margin-top:16px}
.add{display:flex;gap:8px;margin-top:14px}
.add input{flex:1;background:#282828;color:#fff;border:1px solid #444;border-radius:8px;padding:10px;font-size:1em;outline:none}
.add button{background:#1DB954;color:#000;font-weight:bold;padding:10px 16px;border-radius:8px;margin:0}
label{color:#888;font-size:.85em;display:block;margin:16px 0 4px}
.key{width:100%;background:#282828;color:#fff;border:1px solid #444;border-radius:8px;padding:10px;font-size:.9em;outline:none}
.msg{text-align:center;color:#1DB954;margin-top:10px;min-height:20px}
.hint{color:#666;font-size:.75em;margin-top:4px}
</style></head><body>
<h1>Ticker Config</h1><div id="list"></div>
<div class="add"><input id="sym" placeholder="Add symbol (e.g. AAPL)" maxlength="7" onkeydown="if(event.key==='Enter')addT()"><button onclick="addT()">Add</button></div>
<label>Finnhub Stock API Key</label>
<input class="key" id="apikey" placeholder="Get free key at finnhub.io">
<p class="hint">Required for stock prices. Crypto uses CoinGecko (no key needed).</p>
<h1 style="margin-top:24px">Settings</h1>
<label>UTC Offset (hours)</label>
<div style="display:flex;gap:8px;align-items:center"><input type="number" class="key" id="gmt" step="0.5" min="-12" max="14" style="width:90px"><label style="margin:0;color:#fff;font-size:.9em"><input type="checkbox" id="dst"> DST (+1h)</label></div>
<label>Playing Brightness: <span id="brpv"></span></label>
<input type="range" id="brp" min="10" max="255" oninput="updBr()">
<label>Idle Brightness: <span id="briv"></span></label>
<input type="range" id="bri" min="10" max="255" oninput="updBr()">
<button class="sv" onclick="save()">Save Changes</button>
<div class="msg" id="msg"></div><script>
const C=['BTC','ETH','SOL','ADA','XRP','DOGE','DOT','AVAX','BNB','LTC','LINK','SHIB','MATIC','UNI','ATOM','PEPE','ARB','OP','SUI','APT','XMR'];
const CM=['GOLD','SILVER','OIL','NATGAS','COPPER','PLAT','PALLAD'];
let T=[];function render(){let h='';T.forEach((t,i)=>{let cr=C.includes(t),co=CM.includes(t);
h+='<div class="item"><span class="tag '+(cr?'c':co?'o':'s')+'">'+(cr?'CRYPTO':co?'COMMODITY':'STOCK')+'</span><span class="sym">'+t+'</span>'
+'<button onclick="mv('+i+',-1)">&#9650;</button><button onclick="mv('+i+',1)">&#9660;</button>'
+'<button class="del" onclick="del('+i+')">&#10005;</button></div>';});
document.getElementById('list').innerHTML=h;}
function mv(i,d){let j=i+d;if(j<0||j>=T.length)return;[T[i],T[j]]=[T[j],T[i]];render();}
function del(i){T.splice(i,1);render();}
function addT(){let s=document.getElementById('sym'),v=s.value.trim().toUpperCase();if(v&&v.length<=7&&!T.includes(v)&&T.length<8){T.push(v);render();}s.value='';s.focus();}
function updBr(){document.getElementById('briv').textContent=document.getElementById('bri').value;document.getElementById('brpv').textContent=document.getElementById('brp').value;}
function save(){fetch('/api/tickers',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({tickers:T.join(','),key:document.getElementById('apikey').value.trim(),gmt:parseFloat(document.getElementById('gmt').value)||0,dst:document.getElementById('dst').checked?1:0,bri:parseInt(document.getElementById('bri').value)||128,brp:parseInt(document.getElementById('brp').value)||255})})
.then(r=>r.json()).then(d=>{msg(d.ok?'Saved! Settings applied.':'Error.')}).catch(()=>msg('Connection error.'));}
function msg(s){let m=document.getElementById('msg');m.textContent=s;setTimeout(()=>m.textContent='',4000);}
fetch('/api/tickers').then(r=>r.json()).then(d=>{T=d.tickers?d.tickers.split(',').filter(s=>s):[];document.getElementById('apikey').value=d.key||'';document.getElementById('gmt').value=d.gmt||0;document.getElementById('dst').checked=!!d.dst;document.getElementById('brp').value=d.brp||255;document.getElementById('bri').value=d.bri||128;updBr();render();});
</script></body></html>)rawliteral";

// ── Config web handlers ─────────────────────────────────
static esp_err_t config_page_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, CONFIG_PAGE, strlen(CONFIG_PAGE));
  return ESP_OK;
}

static esp_err_t api_get_tickers_handler(httpd_req_t* req) {
  String list = prefs.getString("tickers", "NVDA,LMT,PLTR,BTC,XMR,ETH");
  String key = prefs.getString("stockkey", "");
  long gmt = prefs.getLong("gmtoff", 3600);
  long dst = prefs.getLong("dstoff", 0);
  int bri = prefs.getUChar("br_idle", 128);
  int brp = prefs.getUChar("br_play", 255);
  char json[384];
  snprintf(json, sizeof(json),
    "{\"tickers\":\"%s\",\"key\":\"%s\",\"gmt\":%.1f,\"dst\":%d,\"bri\":%d,\"brp\":%d}",
    list.c_str(), key.c_str(), gmt / 3600.0f, dst > 0 ? 1 : 0, bri, brp);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, strlen(json));
  return ESP_OK;
}

static esp_err_t api_post_tickers_handler(httpd_req_t* req) {
  char buf[512];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    return ESP_FAIL;
  }
  buf[ret] = '\0';
  JsonDocument doc;
  deserializeJson(doc, buf);
  const char* t = doc["tickers"] | "";
  const char* k = doc["key"] | "";

  // Read old values to compare
  String oldTickers = prefs.getString("tickers", "NVDA,LMT,PLTR,BTC,XMR,ETH");
  String oldKey = prefs.getString("stockkey", "");
  bool tickersChanged = false;
  bool setChanged = false;

  // Tickers & API key
  if (strlen(t) > 0 && String(t) != oldTickers) {
    prefs.putString("tickers", t);
    tickersChanged = true;
  }
  if (String(k) != oldKey) {
    prefs.putString("stockkey", k);
    tickersChanged = true;
  }

  // Timezone
  if (!doc["gmt"].isNull()) {
    long newGmt = (long)(((float)(doc["gmt"] | 0.0f)) * 3600);
    if (newGmt != prefs.getLong("gmtoff", 3600)) {
      prefs.putLong("gmtoff", newGmt);
      setChanged = true;
    }
  }
  if (!doc["dst"].isNull()) {
    long newDst = (doc["dst"] | 0) ? 3600L : 0L;
    if (newDst != prefs.getLong("dstoff", 0)) {
      prefs.putLong("dstoff", newDst);
      setChanged = true;
    }
  }

  // Brightness
  if (!doc["bri"].isNull()) {
    uint8_t newBri = constrain(doc["bri"] | 128, 10, 255);
    if (newBri != prefs.getUChar("br_idle", 128)) {
      prefs.putUChar("br_idle", newBri);
      setChanged = true;
    }
  }
  if (!doc["brp"].isNull()) {
    uint8_t newBrp = constrain(doc["brp"] | 255, 10, 255);
    if (newBrp != prefs.getUChar("br_play", 255)) {
      prefs.putUChar("br_play", newBrp);
      setChanged = true;
    }
  }

  doc.clear();
  if (tickersChanged) tickerListChanged = true;
  if (setChanged)     settingsChanged = true;
  const char* resp = "{\"ok\":true}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

void startConfigServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;
  config.max_uri_handlers = 4;
  if (httpd_start(&configServer, &config) == ESP_OK) {
    httpd_uri_t p1 = { .uri = "/",             .method = HTTP_GET,  .handler = config_page_handler };
    httpd_uri_t p2 = { .uri = "/api/tickers",  .method = HTTP_GET,  .handler = api_get_tickers_handler };
    httpd_uri_t p3 = { .uri = "/api/tickers",  .method = HTTP_POST, .handler = api_post_tickers_handler };
    httpd_register_uri_handler(configServer, &p1);
    httpd_register_uri_handler(configServer, &p2);
    httpd_register_uri_handler(configServer, &p3);
    Serial.printf("[Config] Web UI: http://%s\n", WiFi.localIP().toString().c_str());
  }
}

// ── OAuth callback handler ──────────────────────────────
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

// ── OAuth root handler ──────────────────────────────────
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

// ── Exchange auth code for refresh token ────────────────
static String exchangeCodeForToken(const String& code) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, "https://accounts.spotify.com/api/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

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

// ── Run full HTTPS OAuth flow ───────────────────────────
String runOAuthFlow() {
  String ip = WiFi.localIP().toString();
  oauthRedirectUri = "https://" + ip;

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

  httpd_uri_t root_uri     = { .uri = "/",         .method = HTTP_GET, .handler = oauth_root_handler };
  httpd_uri_t callback_uri = { .uri = "/callback",  .method = HTTP_GET, .handler = oauth_callback_handler };
  httpd_register_uri_handler(server, &root_uri);
  httpd_register_uri_handler(server, &callback_uri);

  Serial.printf("[OAuth] HTTPS server running on https://%s\n", ip.c_str());

  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 10);
  tft.print("1. Add redirect URI to Spotify app:");
  tft.setTextColor(0x07E0, TFT_BLACK);
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
  tft.setTextColor(0x7BEF, TFT_BLACK);
  tft.print("Waiting for authorization...");

  oauthComplete = false;
  oauthCode = "";
  while (!oauthComplete) {
    delay(100);
  }

  showStatus("Exchanging token...");
  String refreshToken = exchangeCodeForToken(oauthCode);

  httpd_ssl_stop(server);

  return refreshToken;
}

// ── WiFi auto-reconnect ─────────────────────────────────
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

// ── Serial input for ticker/key changes ─────────────────
void checkSerialInput() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.startsWith("TICKERS:")) {
    String list = line.substring(8);
    list.trim();
    if (list.length() > 0) {
      prefs.putString("tickers", list);
      Serial.printf("[Ticker] Saved: %s\n", list.c_str());
      tickerListChanged = true;
    }
  } else if (line.startsWith("STOCKKEY:")) {
    String key = line.substring(9);
    key.trim();
    prefs.putString("stockkey", key);
    Serial.printf("[Ticker] Stock API key saved (%d chars)\n", key.length());
    tickerListChanged = true;
  }
}
