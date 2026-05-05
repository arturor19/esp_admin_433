# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Firmware for an ESP32 that controls a 433MHz gate/door remote access system. The device runs a web dashboard (HTTP, port 80) protected by session-based login, receives 433MHz RF codes via a **CC1101** transceiver (decoded by RCSwitch), activates a relay when an authorized code is received, and persists all data to LittleFS flash storage.

## Build and upload

This is an Arduino IDE project. There is no CLI build system. To compile and flash:

1. Open `porton_433.ino` in **Arduino IDE**.
2. Select board: **Herramientas → Placa → ESP32 Dev Module**.
3. Select the correct COM port.
4. Upload with the **Subir** button (or `Ctrl+U`).

Required libraries (install via **Sketch → Incluir librería → Gestionar librerías**):
- **ELECHOUSE_CC1101_SRC_DRV** (LSatan / SmartRC-CC1101-Driver)
- **RCSwitch** (sui77)
- **ArduinoJson** (Benoit Blanchon)
- **RTClib** (Adafruit) — only needed if using the DS3231 module

`WiFi`, `WebServer`, `LittleFS`, `WiFiClientSecure`, `Wire`, `time.h`, and `mbedtls/md5.h` are bundled with the ESP32 core. IntelliSense may flag `mbedtls/md5.h` and `WiFiClientSecure.h` as missing — this is a false positive; the code compiles correctly.

## Architecture

All logic lives in a single file: `porton_433.ino`.

### Hardware — CC1101 pinout

| CC1101 pin | ESP32 GPIO |
|---|---|
| MOSI | 23 (SPI default) |
| MISO | 19 (SPI default) |
| SCK | 18 (SPI default) |
| CSN / SS | 5 (`CC1101_CS_PIN`) |
| GDO0 (RX interrupt) | 27 (`RF_RX_PIN`) |
| GDO2 (TX) | 32 (`RF_TX_PIN`) |

### Data flow

1. **RF reception (loop):** RCSwitch fires on the GDO0 interrupt when a 433MHz code arrives. The loop checks:
   - If **learn mode** is active (`learnName != ""`): save the code under that name.
   - Else: look up the code in `users.json`. If found and not blocked → activate relay for `relayPulseMs` ms and send Telegram notification. Log the event only if the code is unknown and `logUnknown == true`, or always if it is a known code.

2. **RF transmission / provision mode (loop):** When `provisionName != ""`, every 1 second the loop switches the CC1101 to TX, transmits `provisionCode` (24-bit, `esp_random()`), then resets the CC1101 back to RX via `rfResetToRx()`. This runs for `PROVISION_DURATION_MS` (15 s). The code is saved to `users.json` immediately when provision starts. A **Retransmitir 15s** button is shown in the UI if the first attempt failed.

3. **Web server (loop):** `WebServer` handles HTTP requests on port 80. Every route calls `requireAuth()` first, which validates the `session` cookie against the in-RAM sessions array.

4. **Relay timing (loop):** Non-blocking — `relayEndMs` is set when the relay activates; the loop turns it off when `millis() >= relayEndMs`. Pulse duration is runtime-configurable (`relayPulseMs`, persisted to `/relay.json`).

### Storage (LittleFS)

All persistence is in JSON files on flash:

| File | Contents |
|---|---|
| `/users.json` | Array of `{code, name, blocked}` — registered remotes |
| `/logs.json` | Array of `{ts, name, code, blocked?}` — access history (max 500) |
| `/admins.json` | Array of `{username, hash}` — MD5-hashed admin passwords |
| `/wifi.json` | `{ssid, pass}` — home WiFi credentials |
| `/ap.json` | `{ssid, pass}` — AP configuration |
| `/tz.json` | `{posix}` — POSIX timezone string (default `<-06>6`, Mexico UTC-6) |
| `/telegram.json` | `{token, chatId, enabled}` — Telegram bot config |
| `/relay.json` | `{pulseMs, logUnknown}` — relay pulse duration and log-unknown flag |

### Authentication

Session tokens are stored in a fixed-size RAM array (`sessions[MAX_SESSIONS]`, max 5). Sessions expire after 1 hour (`SESSION_TTL_MS`). Passwords are stored as MD5 hex hashes. There is no HTTPS — intended for trusted local networks only.

### Time synchronization priority

1. DS3231 RTC (if connected, I2C on default Wire pins) → loaded at boot via `syncSystemFromRtc()`
2. NTP (`pool.ntp.org`) when STA WiFi connects → `configTzTime(posix, NTP_SERVER)` applies TZ and syncs in one call; also updates DS3231 via `syncRtcFromSystem()`
3. Manual NTP re-sync via web UI (`POST /api/ntp/sync`)
4. Manual time entry via web UI (`POST /api/rtc/set`) — user inputs local time, firmware converts to UTC using current TZ
5. Fallback: `uptime+Xs` if no time source is available

### Key constants (top of `porton_433.ino`)

```cpp
#define CC1101_CS_PIN         5    // SPI CSN / SS
#define RF_RX_PIN            27    // GDO0 — RX interrupt
#define RF_TX_PIN            32    // GDO2 — TX
#define RELAY_PIN            26    // Relay signal pin
#define RELAY_PULSE_MS     1000    // Default relay ON time (ms) — overridden by /relay.json
#define RESET_BUTTON_PIN      0    // BOOT button for factory reset
#define LED_PIN               2    // Built-in blue LED
#define RESET_HOLD_MS      5000    // Hold time for factory reset
#define COOLDOWN_MS        3000    // Ignore same code within this window
#define MAX_LOGS            500    // Rolling log limit
#define NTP_SERVER   "pool.ntp.org"
#define MAX_SESSIONS          5
#define SESSION_TTL_MS  3600000UL  // 1 hour
#define PROVISION_DURATION_MS 15000 // How long to broadcast a new provisioned code
```

### Runtime-configurable state

| Variable | Default | Persisted in |
|---|---|---|
| `relayPulseMs` | `RELAY_PULSE_MS` (1000 ms) | `/relay.json` |
| `logUnknown` | `true` | `/relay.json` |

Both are loaded at boot via `loadRelayConfig()` and updated via `POST /api/relay/config`.

### Factory reset

Hold the BOOT button (GPIO 0) for 5 seconds → deletes `/ap.json` and restarts. Only AP config resets; users, logs, and admins are preserved.
