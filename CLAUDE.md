# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Firmware for an ESP32 that controls a 433MHz gate/door remote access system. The device runs a web dashboard (HTTP, port 80) protected by session-based login, receives 433MHz RF codes via a **CC1101** transceiver (decoded by RCSwitch), activates a relay when an authorized code is received, and persists all data to LittleFS flash storage.

## Build and upload

This is a **PlatformIO** project (`platformio.ini`, board `esp32dev`, framework `arduino`). The source lives in `src/main.cpp`.

Helper script (`./flash.sh`) from the project root:

```bash
./flash.sh            # compile, upload, and open serial monitor
./flash.sh build      # compile only
./flash.sh monitor    # serial monitor (115200)
./flash.sh clean      # wipe the .pio build dir
./flash.sh -p /dev/ttyUSB0 [action]   # force a specific port
```

Or PlatformIO directly:

```bash
pio run                       # compile
pio run -t upload             # compile and flash
pio device monitor -b 115200  # serial monitor
```

Libraries are pinned in `platformio.ini` and fetched automatically on first build:
- **SmartRC-CC1101-Driver-Lib** (LSatan)
- **rc-switch** (sui77)
- **ArduinoJson** (Benoit Blanchon)
- **RTClib** (Adafruit) — only needed if using the DS3231 module

`WiFi`, `WebServer`, `LittleFS`, `WiFiClientSecure`, `Wire`, `time.h`, and `MD5Builder` are bundled with the ESP32 core. Password hashing uses `MD5Builder` (core) — the old `mbedtls/md5.h` calls were removed because those symbols changed in the newer ESP-IDF and no longer link.

## Architecture

All logic lives in a single file: `src/main.cpp`.

### Hardware — CC1101 pinout

| CC1101 pin | ESP32 GPIO |
|---|---|
| MOSI | 23 (SPI default) |
| MISO | 19 (SPI default) |
| SCK | 18 (SPI default) |
| CSN / SS | 5 (`CC1101_CS_PIN`) |
| GDO0 (RX interrupt) | 27 (`RF_RX_PIN`) |
| GDO2 (TX) | 32 (`RF_TX_PIN`) |

### RX sensitivity / range

The radio setup mirrors the last known-good commit (`f210a0f`) — `setMHZ(433.92)`, `setModulation(2)` (OOK/ASK), `setPA()`, and **nothing else**. Applied from one place, `applyRfRadioConfig()`, called by `setup()` and `reinitRfReceiver()` so the two cannot drift apart.

**Tuning is runtime-configurable** (`rfMhzCfg`, `rfRxBwCfg`, persisted in `/rf.json`, edited from the **🍽 Sintonia** card in the Controles tab via `POST /api/rf/tune`, applied live through `pendingRfReset` → `reinitRfReceiver()`). This exists precisely because the right value depends on the remotes at a given site and can only be found by testing there — recompiling and reflashing for each candidate is what made the earlier SEG regression so expensive to find. Frequency is clamped to 430-436 MHz and the bandwidth must be one the CC1101 can actually synthesize (`RF_BW_TABLE`), since the library silently rounds anything else and the panel would then be lying. The `RF_MHZ` / `RF_NARROW_RXBW` / `RF_RXBW_KHZ` defines now only supply the **boot default** when `/rf.json` is absent — currently **433.80 MHz / 325 kHz**, chosen on site. A device that already has `/rf.json` keeps whatever the panel last applied, so changing these defines does nothing to a unit in service until it is reset or retuned from the panel. `RF_RX_TOLERANCE` (RCSwitch, default 60) stays compile-time.

**Do not trust the "remotes are at 434.1-434.3" claim.** Early edge-count sweeps suggested it; retuning to 434.20 / 325 kHz then received **nothing at all**, which refutes it. The remotes' true centre is still unmeasured — redo it with the RSSI sweep. The known-good fallback is 433.92 / 812 kHz, where both remote types decode (at poor range); it is a row in the tuning card's stage-2 list, since the card's reset button now goes to the 433.80 / 325 default instead.

**The range trade-off, measured on site:** with `setRxBW(325)` and only the generic remotes working, range was very good; at 812 kHz with both types working, range is short. That is the same ~4 dB (≈1.6× distance) seen from both sides — it is the bandwidth, not the frequency, which was never changed from 433.92. The resolution is not to pick a side but to narrow *centred where both remotes fall*, which is what `GET /api/rf/scan` measures. Both are currently at the `f210a0f` values because `setRxBW(325)` + `setReceiveTolerance(80)` **broke the SEG remotes in production**: 325 kHz closes the window to ±162 kHz and a SEG with a drifted crystal falls outside it, while tolerance 80 lets a generic RCSwitch protocol claim the SEG frame first and return a code that does not match the registered one (logged as "Desconocido"). Generic clones, freshly programmed and centered on 433.92, kept working throughout — hence the misleading "only the generics work" symptom. Gain range through the LNA and the antenna, not by narrowing the window.

**Do not add `setRxBW()` or `setDRate()` without field-testing every remote on site.** Narrowing the RX bandwidth to ~100 kHz is worth roughly 5 dB (≈2× distance) on paper, since demodulator noise scales with bandwidth — but it shrinks the frequency window to ±51 kHz, and a remote whose crystal is off by 100-200 kHz then falls outside it and produces **zero** pulses. This was tried here (102 kHz, plus `setDRate(4)`) and the installation stopped hearing any remote at all.

Note that `setRxBW(325)`, `setDRate`, and `mySwitch.setReceiveTolerance(80)` were **never part of a working committed build** — they only ever existed in uncommitted working-tree edits. Claims that 325 kHz is "field-proven" trace back to a code comment, not to a verified test. Range is to be gained through antenna and power supply, not by trading away tolerance to detuned remotes.

### TX power — deliberately low (LNA in the RF path)

WiFi runs at `WIFI_POWER_19_5dBm`, but the CC1101 is at **`CC1101_PA_DBM 0`** (0 dBm), *not* the +12 dBm max. This is not a mistake and must not be "restored".

The installation has an **SPF5189Z LNA** wired RFIN→antenna, RFOUT→CC1101. The CC1101 has a **single RF port**, so every transmission (provision mode: `mySwitch.send()` bursts for 15 s) is fed **backwards into the LNA's output**, which is not designed to receive it. At +12 dBm the LNA degrades or dies — and a degraded LNA does not stop conducting, it just loses gain and raises its noise figure, so the symptom is **lost receive range** with nothing visibly broken. Receive range is what matters here; cloning is occasional.

Cost of the low setting: a cloner has to be held close to the unit to capture a provisioned code. The proper fix is a TX/RX switch (e.g. PE4259) or keeping the LNA out of the transmit path — then, and only then, does raising `CC1101_PA_DBM` make sense.

The LNA needs its own **5 V** (~90 mA); running it off the ESP32's 3.3 V rail starves it and it delivers far less than its ~20 dB. Note `setup()` disables the brownout detector, so a sagging supply never announces itself — it just performs worse.

### Data flow

1. **RF reception (loop):** RCSwitch fires on the GDO0 interrupt when a 433MHz code arrives. The loop checks:
   - If **learn mode** is active (`learnName != ""`): save the code under that name.
   - Else: look up the code in `users.json`. If found and not blocked → activate relay for `relayPulseMs` ms and send Telegram notification. Log the event only if the code is unknown and `logUnknown == true`, or always if it is a known code.

**Remote format on this site (measured, both types):** every remote here — the SEG *and* the generic clones — decodes as **RCSwitch protocol 6 (HT6P20B), 28 bits, measured pulse 487-500 µs** (nominal for that protocol is 450 µs). They are the *same* protocol; there is no SEG-vs-generic protocol difference, so any tuning aimed at HT6P20B helps both equally. This also settles the earlier regression: losing the SEG remotes was **purely an RF-level effect of `setRxBW(325)`**, not a decode/tolerance issue — with a single protocol in play, no other protocol can steal the frame, so `setReceiveTolerance(80)` was not the cause. TX constants (`RF_TX_PROTOCOL`, `RF_TX_PULSE_US`, `RF_TX_BITS`) mirror these measurements; provisioning previously emitted protocol 1 / 24 bits / 339 µs, which matched nothing on site and would not reproduce on HT6P20B-specific cloners.

**Frame structure:** aligned to 28 bits, a frame is `[20-bit ID][4-bit button][4-bit tail]`. Checked against all 30 remotes registered on site: the **last two bits are `01` in 30/30** (hard invariant), the full tail is `0101` in **27/30** (the three exceptions, `0001`/`1001`, are clones made with a different cloner rather than factory remotes), and the most common button nibble is `0010` (19/30). Provisioning therefore randomizes **only the 20-bit ID** and holds the rest at the majority pattern (`RF_TX_TAIL_NIBBLE` = 0x5, `RF_TX_DATA_NIBBLE` = 0x2); a full 28-bit `esp_random()` produced tails no remote on site emits, which a cloner can reject as malformed. Note an earlier version of this note claimed `0101` was a universal HT6P20B constant — that was inferred from two samples and is wrong; it is a strong majority, not a rule.

2. **RF transmission / provision mode (loop):** When `provisionName != ""`, every 1 second the loop switches the CC1101 to TX, transmits `provisionCode` (28-bit, `esp_random()`, protocol 6 / 490 µs), then resets the CC1101 back to RX via `rfResetToRx()`. This runs for `PROVISION_DURATION_MS` (15 s). The code is saved to `users.json` immediately when provision starts. A **Retransmitir 15s** button is shown in the UI if the first attempt failed.

3. **Web server (loop):** `WebServer` handles HTTP requests on port 80. Every route calls `requireAuth()` first, which validates the `session` cookie against the in-RAM sessions array.

4. **Relay timing (loop):** Non-blocking — `relayEndMs` is set when the relay activates; the loop turns it off when `millis() >= relayEndMs`. Pulse duration is runtime-configurable (`relayPulseMs`, persisted to `/relay.json`). The relay can also be fired manually from the UI via `GET /api/relay/activate` (**Abrir ahora** button), which respects `relayPulseMs` and sends a Telegram notification.

5. **RF diagnostic (loop, 2 phases):** `GET /api/rf/sniff` starts an 8 s diagnostic split into two `SNIFF_PHASE_MS` (4 s) phases over the same GDO0 pin (the raw sniffer and RCSwitch cannot share the pin simultaneously). Phase 1 (`sniffPhase==1`) attaches `sniffIsr` and captures raw pulse durations to measure whether **any signal arrives**. Phase 2 (`sniffPhase==2`) re-enables RCSwitch and polls `mySwitch.available()` to see whether the code **decodes** (→ fixed code, clonable). The result (`sniffResult`: pulse stats + `decoded`/`code`/`bits`, plus `knownName`/`knownBlocked` if the code matches a registered remote) is exposed via `GET /api/users` and stays available until the next diagnostic. After the window, RCSwitch is restored cleanly so normal reception/relay is unaffected.

6. **RF frequency sweep (loop):** `GET /api/rf/scan` (**📡 Buscar frecuencia** button, Controles tab) answers the question the 2-phase sniffer cannot: *at what frequency does this remote actually transmit?* It walks 433.00→434.80 MHz in 50 kHz steps (`SCAN_STEPS` × `SCAN_STEP_MS` ≈ 15 s), counting raw GDO0 pulses per step, and reports the peak. The sweep deliberately narrows the radio to `SCAN_RXBW_KHZ` (101 kHz, ±50 kHz) for the duration — at the operating 812 kHz (±406 kHz) the steps overlap so heavily the peak cannot be localized. **Both the sniffer and the sweep report RSSI**, read from the CC1101's RSSI register via `getRssi()` (dBm). The sniffer measures a noise floor before phase 1 and the peak during it; the sweep records a peak per frequency step. This is the instrument that makes configurations comparable *without walking the range* — margin over the noise floor is what predicts distance (≥20 dB comfortable, 12-20 dB workable, <12 dB is where it fails at range), and it replaced pacing out metres for every candidate setting.

The sweep's peak is chosen by **RSSI, not by edge count**, and validated as ≥6 dB over the median RSSI. Edge counting was unreliable: each step listens for a fixed window while the remote emits in bursts, so a count largely records whether that window happened to land on a burst — producing physically impossible profiles (a strong step, its 50 kHz neighbour near zero, the next strong again) and different peaks on consecutive sweeps of the same remote. RSSI measures power and does not depend on that timing. The sweep also runs `SCAN_PASSES` (3) accumulating passes for the same reason. **A conclusion was once drawn from a single-pass edge-count sweep — that the remotes sit at 434.1-434.3 rather than 433.92 — and retuning to it produced zero received signal, disproving it.** Treat any sweep result as a hypothesis until a retune to it actually improves RSSI. The median matters: an earlier version compared against the *mean*, and these remotes emit over a broad span (~433.9-434.5, many steps), which lifted the mean so far that the real peak failed the 3× test and the sweep reported "no clear peak" with the transmission plainly visible in the profile. A peak drags the mean, not the median. `reinitRfReceiver()` restores the operating tuning afterward. Exposed via `GET /api/users` as `scanResult` (`bestMhz`, `bestN`, `rfMhz`, plus the full `points[]` profile); the UI prints a bar profile and, if the peak is more than 60 kHz off `RF_MHZ`, tells you to change that define. Motivation: a stock SEG **RX2C** receiver hears both the SEG and the generic remotes on this site while the CC1101 only hears the generics — so the remotes and the antenna placement are fine and the loss is in our receive chain. A crystal offset on the SEG remotes is the leading hypothesis, and this measures it instead of guessing.

7. **Bulk block/unblock by name prefix:** `GET /api/users/toggle-prefix?prefix=...&block=0|1` blocks/unblocks every remote whose name matches the prefix at a **word boundary** (exact match, or prefix followed by a non-alphanumeric char) — so "Casa 13" affects "Casa 13 # 1" but not "Casa 130". The UI previews and confirms the affected names before applying.

### Storage (LittleFS)

All persistence is in JSON files on flash:

| File | Contents |
|---|---|
| `/users.json` | Array of `{code, name, blocked, bits}` — registered remotes. CSV round-trip via `GET /users.csv` / `POST /api/users/import`: the importer auto-detects `,` vs `;` (Excel in es-* locales writes semicolons — undetected, every row lands in field 0 and is silently skipped), strips the UTF-8 BOM Excel prepends, honours `""` as an escaped quote, and only drops a first line that actually starts with `nombre`/`name`. Import is a **merge**: it adds or updates by `code` and never deletes, so importing a subset cannot wipe the rest. |
| `/logs.json` | Array of `{ts, name, code, blocked?}` — access history (max 500) |
| `/admins.json` | Array of `{username, hash}` — MD5-hashed admin passwords |
| `/wifi.json` | `{ssid, pass}` — home WiFi credentials |
| `/ap.json` | `{ssid, pass}` — AP configuration |
| `/tz.json` | `{posix}` — POSIX timezone string (default `<-06>6`, Mexico UTC-6) |
| `/telegram.json` | `{token, chatId, enabled, notifyAccess, notifyBlocked, notifyUnknown}` — Telegram bot config (import/export from the UI as `telegram-config.json`). Mirrored in RAM (`tgCache`, refreshed at boot and on save) so the RF path never reads LittleFS; `sendTelegram()` drops the message before queueing when `telegramConfigured()` is false or STA is down, and `GET /api/telegram` returns `configured`/`staOk` so the UI card states plainly whether notifications can go out. |
| `/relay.json` | `{pulseMs, logUnknown}` — relay pulse duration and log-unknown flag |
| `/rf.json` | `{mhz, rxbw}` — receiver tuning set from the panel; absent = the `RF_MHZ` / `RF_NARROW_RXBW` compile-time defaults |

### Networking (AP + STA)

Runs in `WIFI_AP_STA` (own AP `Porton_Config` + optional connection to home WiFi), both always on. Key behaviors set up in `setupWiFi()`:

- **Hostname / mDNS:** hostname is `porton-XXXX` (XXXX = last 2 bytes of the MAC), set before `WiFi.begin()` so DHCP announces it. `startMdns()` advertises `porton-XXXX.local` (+ `_http._tcp` service) on **both** interfaces, so the device is reachable by name whether the client is on the AP (→192.168.4.1) or on home WiFi (→DHCP IP), without needing the IP. Exposed in `/api/status` as `mdns`.
- **Home WiFi is optional and never blocks boot:** `setupWiFi()` brings the AP up on channel 1 and then calls `WiFi.begin()` **without waiting**. The old blocking 15 s STA attempt (plus 5 s of NTP) left the device deaf and panel-less for that whole window whenever the home WiFi was off or absent — the "it doesn't start until you configure it" symptom. The `loop()` already detects the connection (the `!staWasConnected` branch) and runs NTP, mDNS, and the AP channel realignment there. Likewise, `setup()` initializes the **CC1101 and the relay before `setupWiFi()`** — nothing in the RF path depends on the network, so the gate opens from the first second of boot. If the STA later connects on a different channel, the AP is restarted on the router's channel — the single radio must share a channel or the ESP forces a migration (latency/instability). `startSoftAp()` clamps the channel to **1-11**; channels 12-13 are region-restricted on many phones and the AP would not show up in a scan at all.
- **No modem sleep / max TX:** `WiFi.setSleep(false)` + `WIFI_POWER_19_5dBm` for responsiveness/range (higher current draw — needs a solid 5V supply).
- **Auto-reconnect with backoff:** the `loop()` polls STA status every `WIFI_CHECK_INTERVAL_MS` (2 s, cheap) but only re-`begin()`s on a growing delay — `WIFI_RETRY_MIN_MS` (30 s) doubling up to `WIFI_RETRY_MAX_MS` (5 min), reset on reconnect. Retrying every 20 s was wrong: each `begin()` makes the single radio sweep all channels, so with the home WiFi absent the `Porton_Config` AP became permanently slow/hard to find, and calling `begin()` while the supplicant was still connecting logged `sta is connecting, return error` and restarted the sweep. Reconnect re-runs `startMdns()` + NTP. Still fully non-blocking (never stalls the loop / gate opening).
- **Captive portal:** a `DNSServer` (core, port 53) resolves *every* domain to `WiFi.softAPIP()`, and `server.onNotFound(handleNotFound)` answers any request whose `Host:` isn't ours with a `302` to the dashboard. Without this, a phone on `Porton_Config` can't even resolve `connectivitycheck.gstatic.com`, the OS probe fails silently and Android shows "this network has no internet". With it, the OS detects a portal and opens the panel automatically on connect. `handleNotFound()` uses `hostIsSelf()` (AP IP / STA IP / `porton-XXXX[.local]`) so legitimate 404s from the panel are **not** redirected, and redirects to `server.client().localIP()` so an AP client lands on 192.168.4.1 and a home-WiFi client on the DHCP IP. `dnsServer.processNextRequest()` is the first line of `loop()` and is non-blocking. Note the `DNSServer` binds `0.0.0.0:53`, so it also answers on the STA interface — harmless unless something on the home LAN is pointed at the ESP32 as its DNS. Deliberately **not** faking an HTTP 204 to make the warning vanish: Android would then treat the AP as internet-capable and stop falling back to mobile data.

### Boot button (GPIO0) robustness

`checkResetButton()` debounces GPIO0 (stable 60 ms) and only arms the factory reset once the pin has been read HIGH at least since boot (`btnSeenReleased`). This prevents a factory-reset boot loop / false triggers when GPIO0 reads LOW at startup (electrical noise, a wire on the strapping pin, or the serial monitor's DTR/RTS). Use `monitor.py` (which does not toggle DTR/RTS) to avoid inducing that noise while watching the log.

### CC1101 absence must never block boot

`probeCc1101()` runs **before** `ELECHOUSE_cc1101.Init()` and does a raw SPI read of the CC1101 `VERSION` register (0x31, read+burst), bypassing the library entirely. It accepts only TI's documented values — **0x04, 0x14, 0x17** — and sets `rfHardwareOk` accordingly. If the probe fails, the library is never touched.

Two traps, both hit in production here:

- **Never probe by reading the MISO pin.** A first attempt just waited for MISO to go low with a timeout. A dead or absent module leaves the pin floating, it reads low, the probe reports OK, and `Init()` then hangs anyway. Only a real SPI transaction proves a chip is there.
- **Never accept "anything that isn't 0x00 or 0xFF".** A damaged module returned `0x7D` on one boot and `0xA2` on the next — plausible-looking garbage that passed that check. Differing VERSION values across boots is itself the signature of a dead bus: a live chip always returns the same ID.

This matters because the ELECHOUSE driver's `Reset()` contains eight `while(digitalRead(MISO_PIN));` spins **with no timeout**. When it hangs, `setup()` never returns, so `loop()` never runs, so `server.handleClient()` and `dnsServer.processNextRequest()` never run. The symptom is deeply misleading: the AP `Porton_Config` is visible and a phone associates fine (the WiFi stack is independent), but nothing answers DNS or HTTP and the panel looks dead at 192.168.4.1. The watchdog then resets the chip, and the boot log fills with repeated `[WEB] servidor iniciado` — which reads like a web-server loop but is actually a reset loop dying at the same point.

With `rfHardwareOk == false`: boot completes, `reinitRfReceiver()` no-ops, `GET /api/rf/sniff` returns 409 with a wiring message instead of measuring a floating GPIO27 (ambient noise reads as "a few pulses arrived", which the old thresholds reported as "rolling code" and blamed on the antenna), and `GET /api/users` exposes `rfOk` so the Controles tab shows a red banner.

### CC1101 presence check

At boot, `ELECHOUSE_cc1101.getCC1101()` verifies the chip answers over SPI and logs `[RF] CC1101 detectado por SPI (OK)` or a `NO detectado` warning — quick way to tell a wiring/power fault from an antenna/frequency issue (the raw sniffer's phase-1 pulse count then tells whether RF actually reaches GDO0/GPIO27).

### Authentication

Session tokens are stored in a fixed-size RAM array (`sessions[MAX_SESSIONS]`, max 5). Sessions expire after 1 hour (`SESSION_TTL_MS`). Passwords are stored as MD5 hex hashes. There is no HTTPS — intended for trusted local networks only.

### Time synchronization priority

1. DS3231 RTC (if connected, I2C on default Wire pins) → loaded at boot via `syncSystemFromRtc()`
2. NTP (`pool.ntp.org`) when STA WiFi connects → `configTzTime(posix, NTP_SERVER)` applies TZ and syncs in one call; also updates DS3231 via `syncRtcFromSystem()`
3. Manual NTP re-sync via web UI (`POST /api/ntp/sync`)
4. Manual time entry via web UI (`POST /api/rtc/set`) — user inputs local time, firmware converts to UTC using current TZ
5. Fallback: `uptime+Xs` if no time source is available

### Key constants (top of `src/main.cpp`)

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

### OTA firmware update

`POST /api/ota` (multipart, field `firmware`) accepts a PlatformIO `firmware.bin` uploaded from the dashboard (**Actualizar firmware (OTA)** card in the Red tab) and reboots into it. Useful from a phone: Chrome on Android has no Web Serial API, so browser-based serial flashing is impossible — OTA is the only cable-free path. The phone can't *compile*; the `.bin` still comes from a PC running `pio run`.

The stock `esp32dev` partition table already supports this — `app0`/`app1` at 1280K each plus `otadata` — so `Update` writes to the inactive slot and a truncated upload just leaves the previous image booting. LittleFS lives on a separate `spiffs` partition, so users/logs/admins survive the update.

Implementation notes (`handleOtaUpload` / `handleOtaFinish`):
- **Silent auth:** the upload handler can't call `requireAuth()` (its 302 would land mid-multipart), so it validates the session cookie via `isValidSession()` and defers the status code to `handleOtaFinish` (401 unauthorized / 409 busy / 400 otherwise).
- **Busy guard:** rejects while `learnName`/`provisionName` is set, `rfSniffing`, or the relay is on — a reboot mid-operation would strand the relay or lose a learn/provision.
- **RF off during write:** `mySwitch.disableReceive()` before `Update.begin()`. Flash writes stall non-IRAM code, so a RCSwitch ISR firing mid-write can hang the chip. On a failed upload `reinitRfReceiver()` restores reception; on success the reboot does.

### Factory reset

Two ways, both via `factoryResetAp()` (deletes `/ap.json` **and** `/wifi.json`, then restarts — users, logs, and admins are preserved; AP returns to `DEFAULT_AP_SSID`/`DEFAULT_AP_PASS`):

- **Physical:** hold the BOOT button (GPIO 0) for 5 seconds. Recovery path when the web UI is unreachable.
- **Web UI:** `GET /api/factory-reset` (**⚠️ Restablecer red de fábrica** button in the AP config card), which returns the default AP credentials so the UI can tell the user which network to reconnect to.

### Reboot UX (client-side)

Every save-and-reboot flow in the dashboard uses a reusable full-screen overlay (`showReboot(title, note, reconnect)`) instead of `alert()`/`confirm()` pop-ups, so the user always knows the device restarted:

- `reconnect=true` (Save home WiFi, Reboot device): the AP stays up, so the overlay polls `/api/status` after a countdown and auto-reloads when the device returns.
- `reconnect=false` (Save AP, Factory reset): the AP SSID/pass changes and the browser disconnects, so the overlay tells the user exactly which network/password to reconnect to.
