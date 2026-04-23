# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Firmware for an ESP32 that controls a 433MHz gate/door remote access system. The device runs a web dashboard (HTTP, port 80) protected by session-based login, listens for 433MHz RF codes via RCSwitch, activates a relay when an authorized code is received, and persists all data to LittleFS flash storage.

## Build and upload

This is an Arduino IDE project. There is no CLI build system. To compile and flash:

1. Open `porton_433.ino` in **Arduino IDE**.
2. Select board: **Herramientas → Placa → ESP32 Dev Module**.
3. Select the correct COM port.
4. Upload with the **Subir** button (or `Ctrl+U`).

Required libraries (install via **Sketch → Incluir librería → Gestionar librerías**):
- **RCSwitch** (sui77)
- **ArduinoJson** (Benoit Blanchon)
- **RTClib** (Adafruit) — only needed if using the DS3231 module

`WiFi`, `WebServer`, `LittleFS`, `WiFiClientSecure`, `time.h`, and `mbedtls/md5.h` are bundled with the ESP32 core. IntelliSense may flag `mbedtls/md5.h` and `WiFiClientSecure.h` as missing — this is a false positive; the code compiles correctly.

## Architecture

All logic lives in a single file: `porton_433.ino`.

### Data flow

1. **RF reception (loop):** `RCSwitch` fires when a 433MHz code arrives. The loop checks:
   - If **learn mode** is active (`learnName != ""`): save the code under that name.
   - Else: look up the code in `users.json`. If found and not blocked → activate relay for `RELAY_PULSE_MS` ms and send Telegram notification. Log the event either way.

2. **Web server (loop):** `WebServer` handles HTTP requests. Every route calls `requireAuth()` first, which validates the `session` cookie against the in-RAM sessions array.

3. **Relay timing (loop):** Non-blocking — `relayEndMs` is set when the relay activates; the loop turns it off when `millis() >= relayEndMs`.

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
| `/telegram.json` | Telegram bot config and notification preferences |

### Authentication

Session tokens are stored in a fixed-size RAM array (`sessions[MAX_SESSIONS]`, max 5). Sessions expire after 1 hour (`SESSION_TTL_MS`). Passwords are stored as MD5 hex hashes. There is no HTTPS — intended for trusted local networks only.

### Time synchronization priority

1. DS3231 RTC (if connected) → loaded at boot via `syncSystemFromRtc()`
2. NTP (`pool.ntp.org`) when STA WiFi connects → also updates DS3231 via `syncRtcFromSystem()`
3. Manual entry via web UI (`/api/rtc/set`) — user inputs local time, firmware converts to UTC using current TZ
4. Fallback: `uptime+Xs` if no time source is available

### Key constants (top of `porton_433.ino`)

```cpp
#define RF_RX_PIN       27    // 433MHz DATA pin
#define RELAY_PIN       26    // Relay signal pin
#define RELAY_PULSE_MS  2000  // How long relay stays ON
#define RESET_BUTTON_PIN 0    // BOOT button for factory reset
#define LED_PIN          2    // Built-in blue LED
#define COOLDOWN_MS     3000  // Ignore same code within this window
#define MAX_LOGS         500  // Rolling log limit
```

### Factory reset

Hold the BOOT button (GPIO 0) for 5 seconds → deletes `/ap.json` and restarts. Only AP config resets; users, logs, and admins are preserved.
