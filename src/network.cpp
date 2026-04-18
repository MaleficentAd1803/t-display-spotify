// ============================================================
//  Network: OAuth flow, config web server, WiFi, serial input
// ============================================================

#include "config.h"
#include <esp_https_server.h>
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
<label>Playing Brightness: <span id="brpv"></span>/16</label>
<input type="range" id="brp" min="1" max="16" oninput="updBr()">
<label>Idle Brightness: <span id="briv"></span>/16</label>
<input type="range" id="bri" min="1" max="16" oninput="updBr()">
<button class="sv" onclick="save()">Save Changes</button>
<div class="msg" id="msg"></div><a href="/now" style="display:block;text-align:center;color:#888;font-size:.85em;text-decoration:none;margin-top:16px">Now Playing &#8594;</a><script>
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
function save(){fetch('/api/tickers',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({tickers:T.join(','),key:document.getElementById('apikey').value.trim(),gmt:parseFloat(document.getElementById('gmt').value)||0,dst:document.getElementById('dst').checked?1:0,bri:parseInt(document.getElementById('bri').value)||8,brp:parseInt(document.getElementById('brp').value)||16})})
.then(r=>r.json()).then(d=>{msg(d.ok?'Saved! Settings applied.':'Error.')}).catch(()=>msg('Connection error.'));}
function msg(s){let m=document.getElementById('msg');m.textContent=s;setTimeout(()=>m.textContent='',4000);}
fetch('/api/tickers').then(r=>r.json()).then(d=>{T=d.tickers?d.tickers.split(',').filter(s=>s):[];document.getElementById('apikey').value=d.key||'';document.getElementById('gmt').value=d.gmt||0;document.getElementById('dst').checked=!!d.dst;document.getElementById('brp').value=d.brp||16;document.getElementById('bri').value=d.bri||8;updBr();render();});
</script></body></html>)rawliteral";

// ── Now Playing Web UI HTML (stored in flash) ───────────
static const char NOWPLAYING_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Now Playing</title><style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#121212;color:#fff;font-family:-apple-system,BlinkMacSystemFont,sans-serif;display:flex;justify-content:center;padding:24px}
#w{width:380px;max-width:100%}
img#art{width:100%;border-radius:8px;display:none;margin-bottom:16px}
.t{font-size:1.4em;font-weight:bold;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.ar{color:#b3b3b3;margin-top:4px}
.al{color:#727272;font-size:.9em;margin-top:2px}
.si{color:#1DB954;font-size:.85em;margin-top:12px}
.dv{color:#555;font-size:.8em;margin-top:2px}
.bw{background:#404040;border-radius:4px;height:4px;margin-top:16px}
.bf{background:#1DB954;height:100%;border-radius:4px;width:0;transition:width .5s linear}
.tm{display:flex;justify-content:space-between;font-size:.75em;color:#888;margin-top:4px}
#idle{display:none;text-align:center;padding-top:15vh}
.ck{font-size:4em;font-weight:200;color:#b3b3b3;letter-spacing:2px;font-variant-numeric:tabular-nums}
#tks{margin-top:32px;display:flex;flex-wrap:wrap;gap:10px;justify-content:center}
.tk{background:#282828;border-radius:8px;padding:10px 16px;font-size:.85em}
.tk b{color:#b3b3b3}.g{color:#1DB954}.r{color:#e74c3c}
.nv{display:block;text-align:center;color:#555;font-size:.8em;text-decoration:none;margin-top:24px}
.nv:hover{color:#1DB954}
</style></head><body><div id="w">
<div id="play"><img id="art"><div class="t" id="trk"></div><div class="ar" id="ars"></div><div class="al" id="alb"></div>
<div class="si" id="sti"></div><div class="dv" id="dev"></div>
<div class="bw"><div class="bf" id="bf"></div></div><div class="tm"><span id="ct">0:00</span><span id="dt">0:00</span></div></div>
<div id="idle"><div class="ck" id="clk">--:--:--</div><div id="tks"></div></div>
<a class="nv" href="/">Settings</a></div>
<script>
let S={},pt=0;
function f(ms){if(!ms||ms<0)ms=0;let s=Math.floor(ms/1000),m=Math.floor(s/60);s%=60;return m+':'+(s<10?'0':'')+s;}
function upd(){fetch('/api/state').then(r=>r.json()).then(d=>{S=d;pt=Date.now();
if(!d.active){document.getElementById('play').style.display='none';document.getElementById('idle').style.display='block';
let h='';(d.tickers||[]).forEach(t=>{let p=t.p>=1000?t.p.toFixed(0):t.p>=1?t.p.toFixed(2):t.p.toFixed(4);
h+='<div class="tk"><b>'+t.s+'</b> $'+p+' <span class="'+(t.c>=0?'g':'r')+'">'+(t.c>=0?'+':'')+t.c.toFixed(1)+'%</span></div>';});
document.getElementById('tks').innerHTML=h;return;}
document.getElementById('play').style.display='block';document.getElementById('idle').style.display='none';
let a=document.getElementById('art');
if(d.img){a.src=d.img.replace('ab67616d00001e02','ab67616d0000b273');a.style.display='block';}else{a.style.display='none';}
document.getElementById('trk').textContent=d.track||'';
document.getElementById('ars').textContent=d.artist||'';
document.getElementById('alb').textContent=d.album||'';
document.getElementById('dev').textContent=d.device||'';
document.getElementById('sti').textContent=d.playing?'\u25B6 Playing':'\u23F8 Paused';
document.getElementById('dt').textContent=f(d.duration);
}).catch(()=>{});}
function tk(){if(S.active){let el=S.playing?(Date.now()-pt):0;
let c=Math.min((S.progress||0)+el,S.duration||1);
document.getElementById('bf').style.width=(S.duration>0?c/S.duration*100:0)+'%';
document.getElementById('ct').textContent=f(c);}
if(!S.active){let n=new Date(),h=n.getHours(),m=n.getMinutes(),s=n.getSeconds();
document.getElementById('clk').textContent=(h<10?'0':'')+h+':'+(m<10?'0':'')+m+':'+(s<10?'0':'')+s;}}
setInterval(upd,1000);setInterval(tk,200);upd();
</script></body></html>)rawliteral";

// ── Config web handlers ─────────────────────────────────
static esp_err_t config_page_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, CONFIG_PAGE, strlen(CONFIG_PAGE));
  return ESP_OK;
}

static esp_err_t api_get_tickers_handler(httpd_req_t* req) {
  String list = prefs.getString("tickers", DEFAULT_TICKERS);
  String key = prefs.getString("stockkey", "YOUR_FINNHUB_KEY");
  long gmt = prefs.getLong("gmtoff", 3600);
  long dst = prefs.getLong("dstoff", 0);
  int bri = prefs.getUChar("br_idle", 8);
  int brp = prefs.getUChar("br_play", 16);
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
  Serial.printf("[Web] POST body (%d bytes): %s\n", ret, buf);
  JsonDocument doc;
  deserializeJson(doc, buf);
  const char* t = doc["tickers"] | "";
  const char* k = doc["key"] | "";

  // Read old values to compare
  String oldTickers = prefs.getString("tickers", DEFAULT_TICKERS);
  String oldKey = prefs.getString("stockkey", "YOUR_FINNHUB_KEY");
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

  // Brightness — always save and apply to avoid comparison/macro bugs
  if (!doc["bri"].isNull()) {
    int rawBri = doc["bri"].as<int>();
    uint8_t newBri = (rawBri < 1) ? 1 : (rawBri > 16) ? 16 : (uint8_t)rawBri;
    prefs.putUChar("br_idle", newBri);
    setChanged = true;
    Serial.printf("[Web] Idle brightness: raw=%d level=%d\n", rawBri, newBri);
  }
  if (!doc["brp"].isNull()) {
    int rawBrp = doc["brp"].as<int>();
    uint8_t newBrp = (rawBrp < 1) ? 1 : (rawBrp > 16) ? 16 : (uint8_t)rawBrp;
    prefs.putUChar("br_play", newBrp);
    setChanged = true;
    Serial.printf("[Web] Play brightness: raw=%d level=%d\n", rawBrp, newBrp);
  }

  doc.clear();
  if (tickersChanged) tickerListChanged = true;
  if (setChanged)     settingsChanged = true;
  const char* resp = "{\"ok\":true}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

// ── Now Playing page handler ────────────────────────────
static esp_err_t now_page_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, NOWPLAYING_PAGE, strlen(NOWPLAYING_PAGE));
  return ESP_OK;
}

// ── Playback state JSON endpoint ────────────────────────
static esp_err_t api_state_handler(httpd_req_t* req) {
  // Copy playback state under mutex
  bool active, playing;
  String track, artist, album, device, imgUrl;
  int progress, duration;
  unsigned long pollTime;

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  active   = now.active;
  playing  = now.playing;
  track    = now.track;
  artist   = now.artist;
  album    = now.album;
  device   = now.device;
  imgUrl   = now.imgUrl;
  progress = now.progress;
  duration = now.duration;
  pollTime = now.pollTime;
  xSemaphoreGive(dataMutex);

  // Interpolate progress to current time
  if (active && playing && pollTime > 0) {
    int interp = progress + (int)(millis() - pollTime);
    progress = (interp < duration) ? interp : duration;
  }

  JsonDocument doc;
  doc["active"]  = active;
  doc["playing"] = playing;
  if (active) {
    doc["track"]    = track;
    doc["artist"]   = artist;
    doc["album"]    = album;
    doc["device"]   = device;
    doc["img"]      = imgUrl;
    doc["progress"] = progress;
    doc["duration"] = duration;
  }

  JsonArray tArr = doc["tickers"].to<JsonArray>();
  for (int i = 0; i < numTickers; i++) {
    if (!tickerItems[i].valid) continue;
    JsonObject t = tArr.add<JsonObject>();
    t["s"] = (const char*)tickerItems[i].symbol;
    t["p"] = tickerItems[i].price;
    t["c"] = tickerItems[i].change;
  }

  String out;
  serializeJson(doc, out);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, out.c_str(), out.length());
  return ESP_OK;
}

void startConfigServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;
  config.max_uri_handlers = 6;
  if (httpd_start(&configServer, &config) == ESP_OK) {
    httpd_uri_t p1 = { .uri = "/",             .method = HTTP_GET,  .handler = config_page_handler };
    httpd_uri_t p2 = { .uri = "/api/tickers",  .method = HTTP_GET,  .handler = api_get_tickers_handler };
    httpd_uri_t p3 = { .uri = "/api/tickers",  .method = HTTP_POST, .handler = api_post_tickers_handler };
    httpd_uri_t p4 = { .uri = "/now",          .method = HTTP_GET,  .handler = now_page_handler };
    httpd_uri_t p5 = { .uri = "/api/state",    .method = HTTP_GET,  .handler = api_state_handler };
    httpd_register_uri_handler(configServer, &p1);
    httpd_register_uri_handler(configServer, &p2);
    httpd_register_uri_handler(configServer, &p3);
    httpd_register_uri_handler(configServer, &p4);
    httpd_register_uri_handler(configServer, &p5);
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
  http.addHeader("Authorization", buildSpotifyBasicAuth());

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

  // Also start a plain HTTP server on port 80 that redirects to HTTPS
  httpd_config_t httpConfig = HTTPD_DEFAULT_CONFIG();
  httpConfig.max_uri_handlers = 2;
  httpConfig.stack_size = 4096;
  httpd_handle_t httpServer = NULL;
  httpd_start(&httpServer, &httpConfig);
  if (httpServer) {
    // Redirect any HTTP request to the HTTPS URL
    static String httpsUrl;
    httpsUrl = "https://" + ip;
    httpd_uri_t redirect = {
      .uri = "/", .method = HTTP_GET,
      .handler = [](httpd_req_t* req) -> esp_err_t {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", httpsUrl.c_str());
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
      }
    };
    httpd_uri_t redirectCb = {
      .uri = "/callback", .method = HTTP_GET,
      .handler = [](httpd_req_t* req) -> esp_err_t {
        char query[1024] = {0};
        String loc = httpsUrl + "/callback";
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
          loc += "?";
          loc += query;
        }
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", loc.c_str());
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
      }
    };
    httpd_register_uri_handler(httpServer, &redirect);
    httpd_register_uri_handler(httpServer, &redirectCb);
    Serial.println("[OAuth] HTTP redirect server on port 80");
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
  tft.setTextColor(COLOR_GAIN, TFT_BLACK);
  tft.setCursor(10, 32);
  tft.printf("https://%s/callback", ip.c_str());
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 62);
  tft.print("2. Open in browser:");
  tft.setTextColor(COLOR_GAIN, TFT_BLACK);
  tft.setCursor(10, 84);
  tft.printf("http://%s", ip.c_str());
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 114);
  tft.print("3. Accept cert & click login");
  tft.setCursor(10, 145);
  tft.setTextFont(1);
  tft.setTextColor(COLOR_DIM_GREY, TFT_BLACK);
  tft.print("Waiting for authorization...");

  oauthComplete = false;
  oauthCode = "";
  while (!oauthComplete) {
    delay(100);
  }

  showStatus("Exchanging token...");
  String refreshToken = exchangeCodeForToken(oauthCode);

  if (httpServer) httpd_stop(httpServer);
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
