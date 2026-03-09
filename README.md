# T-Display Spotify

A Spotify now-playing dashboard for the **LilyGO T-Display S3** (ESP32-S3, ST7789 320x170 LCD) with a stock & crypto ticker on the idle screen.

![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange)
![License](https://img.shields.io/badge/license-MIT-blue)

## Features

- **Album art** — 300x300 JPEG scaled to 150x150, decoded on-device with TJpg_Decoder
- **Scrolling title** — bold FreeSans font, auto-scrolls when the title doesn't fit
- **Artist, album & device name** — displayed on the right panel
- **Play/pause icon** and **progress bar** with interpolated updates
- **NTP clock** — small clock during playback, large centered clock when idle
- **Stock, crypto & commodity ticker** — scrolling prices on the idle screen (CoinGecko + Finnhub)
- **Web config UI** — manage ticker list, timezone, and brightness settings from any browser
- **On-device HTTPS OAuth** — self-signed cert server runs on the ESP32; no external scripts needed
- **WiFiManager captive portal** — no hardcoded SSIDs
- **Screen flip** — long-press to rotate 180°, saved to NVS
- **Screen on/off** — double-click to toggle the backlight
- **Dual-core architecture** — rendering on core 1, network on core 0 for smooth animations

## Hardware

| Component | Detail |
|---|---|
| Board | LilyGO T-Display S3 (ESP32-S3) |
| Display | ST7789 320x170, 8-bit parallel interface |
| Buttons | TOP (GPIO 0), BOTTOM (GPIO 14) |
| Backlight | GPIO 38 |
| Power enable | GPIO 15 |

## Controls

| Action | Button |
|---|---|
| Next track | TOP single-click |
| Previous track | TOP double-click |
| Flip screen | TOP long-press |
| Play / Pause | BOT single-click |
| Screen on/off | BOT double-click |
| Reset WiFi + token | Hold BOT at boot |

## Setup

### 1. Spotify App

1. Go to the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard) and create an app.
2. Copy your **Client ID** and **Client Secret**.

### 2. Configure credentials

Edit `src/main.cpp` and replace the placeholders:

```cpp
const char* SPOTIFY_CLIENT_ID     = "YOUR_CLIENT_ID";
const char* SPOTIFY_CLIENT_SECRET = "YOUR_CLIENT_SECRET";
```

### 3. Generate SSL certificate

A self-signed certificate is needed for the on-device HTTPS OAuth server. Generate one and place it in `include/certs.h`:

```bash
openssl req -x509 -newkey rsa:2048 -days 3650 -nodes \
  -keyout esp_key.pem -out esp_cert.pem \
  -subj "/CN=SpotifyDisplay"
```

Then create `include/certs.h`:

```cpp
#pragma once

static const char server_cert[] = R"(
-----BEGIN CERTIFICATE-----
<paste contents of esp_cert.pem>
-----END CERTIFICATE-----
)";

static const char server_key[] = R"(
-----BEGIN PRIVATE KEY-----
<paste contents of esp_key.pem>
-----END PRIVATE KEY-----
)";
```

### 4. Build & flash

Requires [PlatformIO](https://platformio.org/).

```bash
pio run -t upload
```

### 5. First boot

1. Connect to the **SpotifyDisplay** WiFi AP and configure your network at `192.168.4.1`.
2. After WiFi connects, the display shows an HTTPS URL — add it as a **Redirect URI** in your Spotify app settings.
3. Open the URL in a browser, accept the self-signed certificate warning, and click the login link.
4. The refresh token is saved automatically. Subsequent boots skip this step.

### 6. Stock & crypto ticker

The idle screen shows a scrolling ticker with stock, crypto, and commodity prices. Default symbols: `NVDA, LMT, PLTR, BTC, XMR, ETH`.

**Supported symbol types:**
- **Stocks** — any Finnhub-supported ticker (e.g. `AAPL`, `MSFT`, `NVDA`)
- **Crypto** — `BTC`, `ETH`, `SOL`, `ADA`, `XRP`, `DOGE`, `DOT`, `AVAX`, `BNB`, `LTC`, `LINK`, `SHIB`, `MATIC`, `UNI`, `ATOM`, `PEPE`, `ARB`, `OP`, `SUI`, `APT`, `XMR`
- **Commodities** — `GOLD`, `SILVER`, `OIL`, `NATGAS`, `COPPER`, `PLAT`, `PALLAD` (fetched via ETF proxies)

**Web config:** Open `http://<device-ip>` in a browser (the IP is shown on the idle screen) to:
- Add, remove, and reorder ticker symbols
- Set your Finnhub API key
- Configure timezone (UTC offset + DST)
- Adjust playing and idle brightness separately

The web UI only reloads what actually changed — adjusting brightness won't trigger a ticker reload.

**Stock prices** require a free [Finnhub](https://finnhub.io/register) API key — enter it in the web config page. Crypto prices use CoinGecko (no key needed).

You can also configure via serial:
```
TICKERS:AAPL,MSFT,BTC,ETH,GOLD
STOCKKEY:your_finnhub_api_key
```

## Project Structure

```
src/
  main.cpp       — setup, loop, button callbacks, background task
  display.cpp    — all TFT drawing functions
  ticker.cpp     — price fetching (CoinGecko/Finnhub) and ticker rendering
  network.cpp    — OAuth flow, config web server, WiFi, serial input
include/
  config.h       — shared constants, structs, and declarations
  certs.h        — SSL certificate (not in repo)
  User_Setup.h   — TFT_eSPI display configuration
```

## Architecture

The ESP32-S3's dual cores are used to keep animations smooth:

- **Core 0** (background) — Spotify API polling, ticker price fetching, WiFi reconnect, button API calls
- **Core 1** (foreground) — all rendering, scroll animations, button detection, clock updates

A FreeRTOS mutex protects shared playback state. Volatile flags signal redraw requests from core 0 to core 1.

## Libraries

- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — display driver
- [TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder) — JPEG decoding
- [SpotifyEsp32](https://github.com/FinianLandes/SpotifyEsp32) — Spotify Web API
- [WiFiManager](https://github.com/tzapu/WiFiManager) — captive portal
- [OneButton](https://github.com/mathertel/OneButton) — button handling
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) — JSON parsing

## License

MIT
