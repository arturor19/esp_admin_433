/*
 * ============================================================
 *  CONTROL DE PORTON 433MHz con ESP32  (v2)
 *  - Modo "aprender" para registrar controles nuevos
 *  - Log automatico (fecha + hora + nombre) de cada apertura
 *  - Dashboard web (AP propio + WiFi de casa simultaneos)
 *  - Persistencia en LittleFS
 *  - NUEVO: cambiar password del AP desde la web
 *  - NUEVO: factory reset manteniendo BOOT por 5 segundos
 * ============================================================
 *
 * FACTORY RESET:
 *   Mantene apretado el boton "BOOT" de la placa por 5 segundos.
 *   El LED azul (GPIO 2) parpadea mientras contas. Al soltar (o
 *   al completar los 5 seg) el ESP32 reinicia con:
 *     - SSID AP:  Porton_Config
 *     - Pass AP:  porton1234
 *   (Los logs y controles aprendidos NO se borran.)
 *
 * CAMBIAR PASSWORD DEL AP:
 *   1. Conectate a la web del porton.
 *   2. Pestana "Red" -> seccion "AP del porton".
 *   3. Poner SSID y password nuevos -> Guardar.
 *   4. El ESP32 reinicia. Te reconectas con la nueva red.
 * ============================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <RCSwitch.h>
#include <ArduinoJson.h>
#include <time.h>
#include "mbedtls/md5.h"  // ESP32 SDK — IntelliSense no lo ve, pero compila correctamente
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <RTClib.h>       // Adafruit RTClib — instalar desde gestor de librerias

// =================== CONFIG ===================
#define RF_RX_PIN          27               // pin DATA del receptor 433MHz
#define RESET_BUTTON_PIN   0                // boton BOOT del ESP32 DevKit
#define LED_PIN            2                // LED azul integrado
#define RESET_HOLD_MS      5000             // 5 seg para factory reset
#define DEFAULT_AP_SSID    "Porton_Config"
#define DEFAULT_AP_PASS    "porton1234"     // minimo 8 caracteres
#define COOLDOWN_MS        3000
#define MAX_LOGS           500
#define NTP_SERVER         "pool.ntp.org"
// TZ se gestiona dinamicamente via /tz.json — ver applyTimezone()
#define MAX_SESSIONS       5
#define SESSION_TTL_MS     3600000UL        // 1 hora
#define MAX_ADMINS         10
#define RELAY_PIN          26               // pin del relevador (cambiar segun tu cableado)
#define RELAY_PULSE_MS     2000             // cuanto tiempo permanece activo (ms)
// ==============================================

RCSwitch     mySwitch = RCSwitch();
WebServer    server(80);
RTC_DS3231   rtc;
bool         rtcOk = false;

String        learnName = "";
String        learnCloneOf = "";   // nombre previo si el codigo ya existia (posible clon)
unsigned long lastCode = 0;
unsigned long lastDetectionMs = 0;
bool          timeOk = false;

// estado del boton de reset
unsigned long btnPressStart = 0;
bool          btnWasPressed = false;
unsigned long lastBlinkMs = 0;
bool          ledState = false;

unsigned long lastSessionCleanMs = 0;
unsigned long relayEndMs = 0;

struct Session {
  char token[33];
  char username[32];
  unsigned long createdMs;
  bool active;
};
Session sessions[MAX_SESSIONS];

// ---------- forward decls ----------
String currentTimestamp();
bool   loadUsers(JsonDocument &doc);
bool   saveUsers(JsonDocument &doc);
bool   loadLogs(JsonDocument &doc);
bool   saveLogs(JsonDocument &doc);
void   loadApConfig(String &ssid, String &pass);
void   saveApConfig(const String &ssid, const String &pass);
bool   loadAdmins(JsonDocument &doc);
bool   saveAdmins(JsonDocument &doc);
void   bootstrapFirstAdmin();
String md5Hex(const String &input);
String generateToken();
String extractCookie(const String &cookieHeader, const String &name);
bool   isValidSession(const String &token);
String createSession(const String &username);
void   invalidateSession(const String &token);
void   cleanExpiredSessions();
bool   requireAuth();
String sessionUsername();
bool   loadTelegramConfig(JsonDocument &doc);
bool   saveTelegramConfig(JsonDocument &doc);
void   sendTelegram(const String &eventType, const String &message);
bool   loadTzConfig(String &posix);
void   saveTzConfig(const String &posix);
void   applyTimezone();
void   syncSystemFromRtc();
void   syncRtcFromSystem();

// ================= STORAGE =================
// /users.json : [{"code":123456,"name":"Usuario"}]
// /logs.json  : [{"ts":"...","name":"Usuario","code":123456}]
// /wifi.json  : {"ssid":"...","pass":"..."}   (red de casa)
// /ap.json    : {"ssid":"...","pass":"..."}   (AP del porton)

bool loadUsers(JsonDocument &doc) {
  doc.to<JsonArray>();
  if (!LittleFS.exists("/users.json")) return true;
  File f = LittleFS.open("/users.json", "r");
  if (!f) return false;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { doc.clear(); doc.to<JsonArray>(); }
  return true;
}

bool saveUsers(JsonDocument &doc) {
  File f = LittleFS.open("/users.json", "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

bool loadAdmins(JsonDocument &doc) {
  doc.to<JsonArray>();
  if (!LittleFS.exists("/admins.json")) return true;
  File f = LittleFS.open("/admins.json", "r");
  if (!f) return false;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { doc.clear(); doc.to<JsonArray>(); }
  return true;
}

bool saveAdmins(JsonDocument &doc) {
  File f = LittleFS.open("/admins.json", "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

void bootstrapFirstAdmin() {
  if (LittleFS.exists("/admins.json")) return;
  JsonDocument doc;
  doc.to<JsonArray>();
  JsonObject a = doc.as<JsonArray>().add<JsonObject>();
  a["username"] = "admin";
  a["hash"]     = md5Hex("admin1234");
  saveAdmins(doc);
  Serial.println("[AUTH] primer admin creado: admin / admin1234");
  Serial.println("[AUTH] *** CAMBIA EL PASSWORD DESDE LA PESTANA ADMINS ***");
}

bool loadLogs(JsonDocument &doc) {
  doc.to<JsonArray>();
  if (!LittleFS.exists("/logs.json")) return true;
  File f = LittleFS.open("/logs.json", "r");
  if (!f) return false;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { doc.clear(); doc.to<JsonArray>(); }
  return true;
}

bool saveLogs(JsonDocument &doc) {
  File f = LittleFS.open("/logs.json", "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

String findUserName(unsigned long code) {
  JsonDocument doc; loadUsers(doc);
  for (JsonObject u : doc.as<JsonArray>()) {
    if (u["code"].as<unsigned long>() == code) return u["name"].as<String>();
  }
  return "";
}

void addLog(const String &name, unsigned long code, bool blocked = false) {
  JsonDocument doc; loadLogs(doc);
  JsonArray arr = doc.as<JsonArray>();
  JsonObject entry = arr.add<JsonObject>();
  entry["ts"]      = currentTimestamp();
  entry["name"]    = name;
  entry["code"]    = code;
  if (blocked) entry["blocked"] = true;
  while (arr.size() > MAX_LOGS) arr.remove(0);
  saveLogs(doc);
}

String currentTimestamp() {
  if (!timeOk) {
    unsigned long s = millis() / 1000;
    char buf[32]; snprintf(buf, sizeof(buf), "uptime+%lus", s);
    return String(buf);
  }
  time_t now; time(&now);
  struct tm tm; localtime_r(&now, &tm);
  char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return String(buf);
}

// ================= TELEGRAM =================
bool loadTelegramConfig(JsonDocument &doc) {
  doc["enabled"]       = false;
  doc["token"]         = "";
  doc["chatId"]        = "";
  doc["notifyAccess"]  = true;
  doc["notifyBlocked"] = true;
  doc["notifyUnknown"] = false;
  if (!LittleFS.exists("/telegram.json")) return true;
  File f = LittleFS.open("/telegram.json", "r");
  if (!f) return false;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { doc["enabled"] = false; }
  return true;
}

bool saveTelegramConfig(JsonDocument &doc) {
  File f = LittleFS.open("/telegram.json", "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

void sendTelegram(const String &eventType, const String &message) {
  if (WiFi.status() != WL_CONNECTED) return;
  JsonDocument cfg; loadTelegramConfig(cfg);
  if (!(cfg["enabled"] | false)) return;
  if (!(cfg[eventType] | false)) return;
  String token  = cfg["token"].as<String>();
  String chatId = cfg["chatId"].as<String>();
  if (token.length() == 0 || chatId.length() == 0) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);
  if (!client.connect("api.telegram.org", 443)) {
    Serial.println("[TG] error conectando");
    return;
  }
  String body = "{\"chat_id\":\"" + chatId + "\",\"text\":\"" + message + "\"}";
  String path = "/bot" + token + "/sendMessage";
  client.print("POST " + path + " HTTP/1.1\r\n");
  client.print("Host: api.telegram.org\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Content-Length: " + String(body.length()) + "\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(body);
  unsigned long t = millis();
  while (client.available() == 0 && millis() - t < 4000) delay(10);
  client.stop();
  Serial.println("[TG] mensaje enviado: " + message);
}

// ================= RTC DS3231 =================
void syncSystemFromRtc() {
  if (!rtcOk) return;
  DateTime t = rtc.now();
  if (t.year() < 2020) { Serial.println("[RTC] hora no valida, ignorando"); return; }
  struct timeval tv;
  tv.tv_sec  = (time_t)t.unixtime();
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  timeOk = true;
  Serial.printf("[RTC] hora cargada: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second());
}

void syncRtcFromSystem() {
  if (!rtcOk) return;
  time_t now; time(&now);
  rtc.adjust(DateTime((uint32_t)now));
  Serial.println("[RTC] DS3231 actualizado desde NTP");
}

// ================= TIMEZONE =================
bool loadTzConfig(String &posix) {
  posix = "<-06>6";  // default: Mexico Centro UTC-6
  if (!LittleFS.exists("/tz.json")) return true;
  File f = LittleFS.open("/tz.json", "r");
  if (!f) return true;
  JsonDocument doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    String p = doc["posix"].as<String>();
    if (p.length() > 0) posix = p;
  }
  f.close();
  return true;
}

void saveTzConfig(const String &posix) {
  JsonDocument doc;
  doc["posix"] = posix;
  File f = LittleFS.open("/tz.json", "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

void applyTimezone() {
  String posix;
  loadTzConfig(posix);
  setenv("TZ", posix.c_str(), 1);
  tzset();
  Serial.printf("[TZ] zona aplicada: %s\n", posix.c_str());
}

// ================= AUTH =================
String md5Hex(const String &input) {
  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  mbedtls_md5_starts(&ctx);
  mbedtls_md5_update(&ctx, (const uint8_t*)input.c_str(), input.length());
  uint8_t digest[16];
  mbedtls_md5_finish(&ctx, digest);
  mbedtls_md5_free(&ctx);
  char hex[33];
  for (int i = 0; i < 16; i++) sprintf(hex + i * 2, "%02x", digest[i]);
  hex[32] = '\0';
  return String(hex);
}

String generateToken() {
  char token[33];
  for (int i = 0; i < 4; i++) sprintf(token + i * 8, "%08x", esp_random());
  token[32] = '\0';
  return String(token);
}

String extractCookie(const String &cookieHeader, const String &name) {
  String search = name + "=";
  int idx = cookieHeader.indexOf(search);
  if (idx < 0) return "";
  int start = idx + search.length();
  int end = cookieHeader.indexOf(';', start);
  return end < 0 ? cookieHeader.substring(start) : cookieHeader.substring(start, end);
}

bool isValidSession(const String &token) {
  unsigned long now = millis();
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].active &&
        token == sessions[i].token &&
        (now - sessions[i].createdMs) < SESSION_TTL_MS) return true;
  }
  return false;
}

String createSession(const String &username) {
  cleanExpiredSessions();
  int slot = -1;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions[i].active) { slot = i; break; }
  }
  if (slot < 0) {
    // evict oldest
    unsigned long oldest = millis();
    slot = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i].createdMs <= oldest) { oldest = sessions[i].createdMs; slot = i; }
    }
  }
  String token = generateToken();
  strncpy(sessions[slot].token, token.c_str(), 32); sessions[slot].token[32] = '\0';
  strncpy(sessions[slot].username, username.c_str(), 31); sessions[slot].username[31] = '\0';
  sessions[slot].createdMs = millis();
  sessions[slot].active = true;
  return token;
}

void invalidateSession(const String &token) {
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].active && token == sessions[i].token) { sessions[i].active = false; break; }
  }
}

void cleanExpiredSessions() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].active && (now - sessions[i].createdMs) > SESSION_TTL_MS)
      sessions[i].active = false;
  }
}

bool requireAuth() {
  String token = extractCookie(server.header("Cookie"), "session");
  if (token.length() > 0 && isValidSession(token)) return true;
  server.sendHeader("Location", "/login");
  server.send(302, "text/plain", "");
  return false;
}

String sessionUsername() {
  String token = extractCookie(server.header("Cookie"), "session");
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].active && token == sessions[i].token) return String(sessions[i].username);
  }
  return "";
}

// ================= AP CONFIG =================
void loadApConfig(String &ssid, String &pass) {
  ssid = DEFAULT_AP_SSID;
  pass = DEFAULT_AP_PASS;
  if (!LittleFS.exists("/ap.json")) return;
  File f = LittleFS.open("/ap.json", "r");
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    String s = doc["ssid"].as<String>();
    String p = doc["pass"].as<String>();
    if (s.length() > 0) ssid = s;
    pass = p;  // puede ser vacio (red abierta)
  }
  f.close();
}

void saveApConfig(const String &ssid, const String &pass) {
  JsonDocument doc;
  doc["ssid"] = ssid;
  doc["pass"] = pass;
  File f = LittleFS.open("/ap.json", "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

// ================= WIFI CLIENTE =================
void loadWiFiConfig(String &ssid, String &pass) {
  ssid = ""; pass = "";
  if (!LittleFS.exists("/wifi.json")) return;
  File f = LittleFS.open("/wifi.json", "r");
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    ssid = doc["ssid"].as<String>();
    pass = doc["pass"].as<String>();
  }
  f.close();
}

void saveWiFiConfig(const String &ssid, const String &pass) {
  JsonDocument doc;
  doc["ssid"] = ssid;
  doc["pass"] = pass;
  File f = LittleFS.open("/wifi.json", "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

void setupWiFi() {
  String apSsid, apPass;
  loadApConfig(apSsid, apPass);

  String staSsid, staPass;
  loadWiFiConfig(staSsid, staPass);

  WiFi.mode(WIFI_AP_STA);
  if (apPass.length() == 0) WiFi.softAP(apSsid.c_str());            // red abierta
  else                      WiFi.softAP(apSsid.c_str(), apPass.c_str());

  Serial.printf("[AP] SSID=%s  IP=%s\n",
                apSsid.c_str(), WiFi.softAPIP().toString().c_str());

  if (staSsid.length() > 0) {
    Serial.printf("[STA] Conectando a %s ...\n", staSsid.c_str());
    WiFi.begin(staSsid.c_str(), staPass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(500); Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[STA] OK  IP=%s\n", WiFi.localIP().toString().c_str());
      configTime(0, 0, NTP_SERVER);
      struct tm tm;
      if (getLocalTime(&tm, 5000)) {
        timeOk = true;
        Serial.println("[NTP] hora sincronizada");
        syncRtcFromSystem();
      }
    } else {
      Serial.println("[STA] fallo. Usa el AP para configurar.");
    }
  }
}

// ================= FACTORY RESET (boton BOOT) =================
void factoryResetAp() {
  Serial.println("\n[RESET] borrando config AP... volviendo a defaults");
  LittleFS.remove("/ap.json");
  // feedback visual: 3 parpadeos rapidos
  for (int i = 0; i < 6; i++) {
    digitalWrite(LED_PIN, i % 2); delay(100);
  }
  delay(300);
  ESP.restart();
}

void checkResetButton() {
  bool pressed = (digitalRead(RESET_BUTTON_PIN) == LOW);

  if (pressed && !btnWasPressed) {
    btnPressStart = millis();
    btnWasPressed = true;
    Serial.println("[BTN] presionado, mantene 5s para reset");
  } else if (!pressed && btnWasPressed) {
    btnWasPressed = false;
    digitalWrite(LED_PIN, LOW);
    ledState = false;
  } else if (pressed && btnWasPressed) {
    unsigned long held = millis() - btnPressStart;
    // parpadeo con frecuencia creciente
    unsigned long blinkInterval = held < 2000 ? 400 : (held < 4000 ? 200 : 80);
    if (millis() - lastBlinkMs >= blinkInterval) {
      lastBlinkMs = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
    if (held >= RESET_HOLD_MS) {
      digitalWrite(LED_PIN, HIGH);
      factoryResetAp();
    }
  }
}

// ================= HTML =================
const char LOGIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Login - Porton</title>
<style>
:root{--bg:#0f1115;--card:#1a1d24;--txt:#e6e6e6;--acc:#4ade80;--err:#f87171}
*{box-sizing:border-box}
body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--txt);margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:16px}
.card{background:var(--card);border-radius:12px;padding:28px 24px;width:100%;max-width:340px}
h1{font-size:20px;margin:0 0 6px;text-align:center}
.sub{color:#888;font-size:13px;text-align:center;margin-bottom:20px}
input{display:block;width:100%;background:#262a33;border:1px solid #3a3e48;color:var(--txt);padding:10px 12px;border-radius:8px;font-size:14px;font-family:inherit;margin-bottom:10px}
input:focus{outline:none;border-color:var(--acc)}
button{width:100%;background:var(--acc);color:#051a0d;border:none;padding:12px;border-radius:8px;font-size:15px;font-weight:600;cursor:pointer;font-family:inherit;margin-top:4px}
.err{color:var(--err);font-size:13px;text-align:center;margin-top:12px;display:none}
</style></head><body>
<div class="card">
  <h1>&#128682; Porton</h1>
  <div class="sub">Ingresa tus credenciales</div>
  <input type="text" id="u" placeholder="Usuario" autocomplete="username">
  <input type="password" id="p" placeholder="Contrasena" autocomplete="current-password">
  <button onclick="doLogin()">Ingresar</button>
  <div class="err" id="err">Usuario o contrasena incorrectos</div>
</div>
<script>
async function doLogin(){
  const u=document.getElementById('u').value.trim();
  const p=document.getElementById('p').value;
  if(!u||!p)return;
  const r=await fetch('/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'username='+encodeURIComponent(u)+'&password='+encodeURIComponent(p)});
  if(r.ok)window.location='/';
  else document.getElementById('err').style.display='block';
}
document.addEventListener('keydown',e=>{if(e.key==='Enter')doLogin();});
</script></body></html>
)rawliteral";

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Porton</title>
<style>
:root{--bg:#0f1115;--card:#1a1d24;--txt:#e6e6e6;--mut:#888;--acc:#4ade80;--warn:#fbbf24;--err:#f87171}
*{box-sizing:border-box}
body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--txt);margin:0;padding:16px;max-width:680px;margin:0 auto}
h1{font-size:22px;margin:0}
h2{font-size:13px;margin:20px 0 8px;color:var(--mut);text-transform:uppercase;letter-spacing:.5px}
.sub{color:var(--mut);font-size:13px;margin-bottom:16px}
.card{background:var(--card);border-radius:12px;padding:14px;margin-bottom:12px}
.row{display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-bottom:1px solid #2a2e38;gap:10px}
.row:last-child{border-bottom:none}
.row .name{font-weight:600}
.row .meta{color:var(--mut);font-size:12px}
button,input,select{background:#262a33;border:1px solid #3a3e48;color:var(--txt);padding:10px 12px;border-radius:8px;font-size:14px;width:100%;font-family:inherit}
button{cursor:pointer;background:var(--acc);color:#051a0d;border:none;font-weight:600}
button.sec{background:#262a33;color:var(--txt)}
button.warn{background:var(--err);color:#fff}
button.blk{background:var(--warn);color:#1a1200}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:10px}
.empty{color:var(--mut);text-align:center;padding:24px 0}
.tag{display:inline-block;padding:2px 8px;border-radius:6px;font-size:11px;background:#262a33;color:var(--mut)}
.tag.red{background:var(--err);color:#fff}
.learning{background:var(--warn);color:#1a1200;padding:12px;border-radius:8px;text-align:center;margin-bottom:10px;font-weight:600}
.nav{display:flex;gap:6px;margin-bottom:16px}
.nav a{flex:1;text-align:center;padding:10px;text-decoration:none;color:var(--txt);background:var(--card);border-radius:8px;font-size:13px}
.nav a.active{background:var(--acc);color:#051a0d;font-weight:600}
.hint{font-size:11px;color:var(--mut);margin-top:6px;line-height:1.4}
.hdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:4px}
</style></head><body>
<div class="hdr"><h1>&#128682; Porton</h1><a href="/logout" style="text-decoration:none"><button class="sec" style="width:auto;padding:6px 14px;font-size:12px">Salir</button></a></div>
<div class="sub" id="status">cargando...</div>
<div class="nav">
  <a href="#" class="active" data-tab="logs">Registros</a>
  <a href="#" data-tab="users">Controles</a>
  <a href="#" data-tab="net">Red</a>
  <a href="#" data-tab="admins">Admins</a>
</div>

<div id="tab-logs">
  <h2>Ultimos accesos</h2>
  <div class="card" id="logs"></div>
  <div class="grid">
    <button class="sec" onclick="loadLogs()">Actualizar</button>
    <a href="/logs.csv" style="text-decoration:none"><button class="sec" style="width:100%">Descargar CSV</button></a>
  </div>
  <button class="warn" style="margin-top:8px" onclick="clearLogs()">Borrar registros</button>
</div>

<div id="tab-users" style="display:none">
  <div id="learnBox"></div>
  <div id="cloneBox" style="display:none;background:var(--err);color:#fff;padding:12px;border-radius:8px;margin-bottom:10px;font-weight:600"></div>
  <h2>Controles guardados</h2>
  <div class="card" id="users"></div>
  <h2>Aprender control nuevo</h2>
  <div class="card">
    <input type="text" id="newName" placeholder="Nombre (ej. Usuario)" maxlength="20">
    <div class="grid" style="margin-top:8px">
      <button onclick="startLearn()">Iniciar aprendizaje</button>
      <button class="sec" onclick="cancelLearn()">Cancelar</button>
    </div>
    <div class="hint">Toca iniciar y despues apreta una vez el boton del control.</div>
  </div>
</div>

<div id="tab-net" style="display:none">
  <h2>AP del porton</h2>
  <div class="card">
    <div style="margin-bottom:8px">Actual: <span id="apInfo" class="tag">-</span></div>
    <input type="text" id="apSsid" placeholder="SSID (ej. Porton_Config)" maxlength="32">
    <input type="password" id="apPass" placeholder="Password (min. 8 caracteres)" style="margin-top:8px" maxlength="63">
    <button style="margin-top:10px" onclick="saveAp()">Guardar y reiniciar</button>
    <div class="hint">Si olvidas el password: manten apretado el boton BOOT 5 segundos. Vuelve a Porton_Config / porton1234.</div>
  </div>
  <h2>Conexion a WiFi de casa</h2>
  <div class="card">
    <div style="margin-bottom:8px">Estado: <span id="wifiStatus" class="tag">-</span></div>
    <input type="text" id="ssid" placeholder="SSID de tu casa">
    <input type="password" id="pass" placeholder="Contrasena" style="margin-top:8px">
    <button style="margin-top:10px" onclick="saveWifi()">Guardar y reiniciar</button>
    <div class="hint">Si falla la conexion siempre podes volver al AP del porton.</div>
  </div>
  <h2>Notificaciones Telegram</h2>
  <div class="card">
    <div class="row" style="padding:8px 0">
      <span style="font-weight:600">Activar notificaciones</span>
      <input type="checkbox" id="tgEnabled" style="width:auto;height:18px;width:18px;cursor:pointer">
    </div>
    <input type="text" id="tgToken" placeholder="Bot Token (ej. 123456789:ABC-DEF...)" style="margin-top:4px" maxlength="128">
    <input type="text" id="tgChatId" placeholder="Chat ID (ej. -1001234567890)" style="margin-top:8px" maxlength="32">
    <div style="margin-top:12px;font-size:12px;color:var(--mut);margin-bottom:4px">Notificar cuando:</div>
    <div class="row" style="padding:6px 0"><span style="font-size:13px">Acceso autorizado</span><input type="checkbox" id="tgAccess" style="width:auto;height:18px;width:18px;cursor:pointer"></div>
    <div class="row" style="padding:6px 0"><span style="font-size:13px">Control bloqueado</span><input type="checkbox" id="tgBlocked" style="width:auto;height:18px;width:18px;cursor:pointer"></div>
    <div class="row" style="padding:6px 0"><span style="font-size:13px">Codigo desconocido</span><input type="checkbox" id="tgUnknown" style="width:auto;height:18px;width:18px;cursor:pointer"></div>
    <button style="margin-top:10px" onclick="saveTelegram()">Guardar</button>
    <div class="hint">Requiere WiFi de casa activo. Obtene el token con @BotFather en Telegram y el Chat ID con @userinfobot.</div>
  </div>
  <h2>Zona horaria</h2>
  <div class="card">
    <select id="tzPosix" style="margin-bottom:10px">
      <option value="&lt;-06&gt;6">Mexico Centro (UTC-6)</option>
      <option value="&lt;-07&gt;7">Mexico Sonora (UTC-7)</option>
      <option value="&lt;-08&gt;8&lt;-07&gt;,M3.2.0,M11.1.0">Mexico Noroeste/Tijuana (UTC-8/-7)</option>
      <option value="&lt;-05&gt;5">Colombia / Ecuador / Peru (UTC-5)</option>
      <option value="&lt;-04&gt;4">Venezuela / Bolivia (UTC-4)</option>
      <option value="&lt;-03&gt;3">Argentina / Uruguay (UTC-3)</option>
      <option value="&lt;-04&gt;4&lt;-03&gt;,M9.1.6/24,M4.1.6/24">Chile (UTC-4/-3)</option>
      <option value="&lt;-04&gt;4&lt;-03&gt;,M9.1.0/0,M3.4.0/0">Paraguay (UTC-4/-3)</option>
      <option value="CET-1CEST,M3.5.0,M10.5.0/3">Espana (UTC+1/+2)</option>
      <option value="UTC0">UTC</option>
      <option value="EST5EDT,M3.2.0,M11.1.0">USA Este (UTC-5/-4)</option>
      <option value="CST6CDT,M3.2.0,M11.1.0">USA Centro (UTC-6/-5)</option>
      <option value="MST7MDT,M3.2.0,M11.1.0">USA Montana (UTC-7/-6)</option>
      <option value="PST8PDT,M3.2.0,M11.1.0">USA Pacifico (UTC-8/-7)</option>
    </select>
    <button onclick="saveTz()">Guardar</button>
    <div class="hint">Se aplica de inmediato, sin reiniciar. Afecta la hora en los registros (requiere WiFi de casa para NTP).</div>
  </div>
  <h2>Reloj RTC (DS3231)</h2>
  <div class="card">
    <div class="row" style="padding:6px 0"><span>Modulo DS3231</span><span id="rtcStatus" class="tag">-</span></div>
    <div class="row" style="padding:6px 0"><span>Hora actual (local)</span><span id="rtcNow" class="tag" style="font-family:monospace">-</span></div>
    <h2 style="margin:14px 0 6px">Establecer hora manual</h2>
    <div class="hint" style="margin-bottom:8px">Ingresar en hora local segun la zona configurada arriba. Util cuando no hay internet.</div>
    <input type="datetime-local" id="rtcDatetime" step="1" style="margin-bottom:10px">
    <button onclick="setRtcTime()">Establecer hora</button>
    <div class="hint">Con WiFi de casa la hora se sincroniza automaticamente desde NTP al conectar.</div>
  </div>
</div>

<div id="tab-admins" style="display:none">
  <h2>Usuarios admin</h2>
  <div class="card" id="admins"></div>
  <h2>Agregar admin</h2>
  <div class="card">
    <input type="text" id="newAdminUser" placeholder="Nombre de usuario" maxlength="31">
    <input type="password" id="newAdminPass" placeholder="Contrasena (min. 6 caracteres)" style="margin-top:8px" maxlength="63">
    <button style="margin-top:10px" onclick="addAdmin()">Agregar</button>
    <div class="hint">No se puede borrar el ultimo admin.</div>
  </div>
  <h2>Cambiar mi password</h2>
  <div class="card">
    <input type="password" id="cpOld" placeholder="Password actual" maxlength="63">
    <input type="password" id="cpNew" placeholder="Nuevo password (min. 6 caracteres)" style="margin-top:8px" maxlength="63">
    <input type="password" id="cpNew2" placeholder="Repetir nuevo password" style="margin-top:8px" maxlength="63">
    <button style="margin-top:10px" onclick="changePass()">Cambiar password</button>
  </div>
</div>

<script>
function chk(r){if(r&&r.status===401){window.location='/login';return false;}return true;}
function tab(id){
  document.querySelectorAll('.nav a').forEach(a=>a.classList.toggle('active',a.dataset.tab===id));
  ['logs','users','net','admins'].forEach(t=>document.getElementById('tab-'+t).style.display=t===id?'':'none');
  if(id==='logs')loadLogs();if(id==='users')loadUsers();if(id==='net'){loadStatus();loadTelegram();loadTz();loadRtc();}if(id==='admins')loadAdmins();
}
document.querySelectorAll('.nav a').forEach(a=>a.addEventListener('click',e=>{e.preventDefault();tab(a.dataset.tab);}));
async function loadStatus(){
  const r=await fetch('/api/status');if(!chk(r))return;const j=await r.json();
  document.getElementById('status').textContent=j.time+' - '+(j.staIp||'sin WiFi casa')+' - AP: '+j.apIp;
  document.getElementById('wifiStatus').textContent=j.staIp?('Conectado a '+j.ssid+' ('+j.staIp+')'):'No conectado';
  document.getElementById('apInfo').textContent=j.apSsid+(j.apOpen?' (abierta)':' (protegida)');
  document.getElementById('apSsid').value=j.apSsid;
}
async function loadLogs(){
  const r=await fetch('/api/logs');if(!chk(r))return;const j=await r.json();
  const el=document.getElementById('logs');
  if(!j.length){el.innerHTML='<div class="empty">Sin registros todavia</div>';return;}
  el.innerHTML=j.slice().reverse().map(e=>`<div class="row"><div><div class="name">${e.name||'Desconocido'}${e.blocked?' <span class="tag red">BLOQUEADO</span>':''}</div><div class="meta">codigo ${e.code}</div></div><div class="meta">${e.ts}</div></div>`).join('');
}
async function loadUsers(){
  const r=await fetch('/api/users');if(!chk(r))return;const j=await r.json();
  const el=document.getElementById('users');
  if(!j.users.length)el.innerHTML='<div class="empty">Todavia no hay controles aprendidos</div>';
  else el.innerHTML=j.users.map(u=>`<div class="row"><div><div class="name">${u.name}${u.blocked?' <span class="tag red">BLOQUEADO</span>':''}</div><div class="meta">codigo ${u.code}</div></div><div style="display:flex;gap:6px"><button class="sec" style="width:auto;padding:6px 10px" onclick="renameUser(${u.code},'${u.name.replace(/'/g,"\\'")}')">Renombrar</button><button class="${u.blocked?'':'blk'}" style="width:auto;padding:6px 10px" onclick="toggleUser(${u.code})">${u.blocked?'Habilitar':'Bloquear'}</button><button class="warn" style="width:auto;padding:6px 10px" onclick="delUser(${u.code})">Borrar</button></div></div>`).join('');
  const lb=document.getElementById('learnBox');
  lb.innerHTML=j.learning?`<div class="learning">Esperando boton del control para "${j.learning}"...</div>`:'';
  if(j.learning)setTimeout(loadUsers,1500);
  const cb=document.getElementById('cloneBox');
  if(j.cloneWarning){cb.textContent='Aviso: el codigo recibido ya estaba registrado como "'+j.cloneWarning+'". Puede ser un control clonado.';cb.style.display='';}
  else cb.style.display='none';
}
async function renameUser(code,current){
  const n=prompt('Nuevo nombre para "'+current+'":', current);
  if(!n||!n.trim()||n.trim()===current)return;
  const r=await fetch('/api/users/rename',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({code,name:n.trim()})});
  const j=await r.json();
  if(r.ok)loadUsers();else alert(j.error||'Error');
}
async function toggleUser(code){await fetch('/api/users/toggle?code='+code);loadUsers();}
async function startLearn(){
  const name=document.getElementById('newName').value.trim();
  if(!name){alert('Ingresa un nombre');return;}
  await fetch('/api/learn/start?name='+encodeURIComponent(name));
  document.getElementById('newName').value='';loadUsers();
}
async function cancelLearn(){await fetch('/api/learn/cancel');loadUsers();}
async function delUser(code){if(!confirm('Borrar este control?'))return;await fetch('/api/users/delete?code='+code);loadUsers();}
async function clearLogs(){if(!confirm('Borrar TODOS los registros?'))return;await fetch('/api/logs/clear');loadLogs();}
async function saveWifi(){
  const ssid=document.getElementById('ssid').value.trim();
  const pass=document.getElementById('pass').value;
  if(!ssid){alert('Ingresa el SSID');return;}
  await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,pass})});
  alert('Guardado. El Porton Admin se va a reiniciar.');
}
async function saveAp(){
  const ssid=document.getElementById('apSsid').value.trim();
  const pass=document.getElementById('apPass').value;
  if(!ssid){alert('Ingresa el SSID del AP');return;}
  if(pass.length>0&&pass.length<8){alert('El password debe tener al menos 8 caracteres (o dejarlo vacio para red abierta)');return;}
  if(!confirm('El Porton Admin se va a reiniciar. Continuar?'))return;
  const r=await fetch('/api/ap',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,pass})});
  if(r.ok)alert('Guardado. Reiniciando...');else alert('Error al guardar');
}
async function loadTelegram(){
  const r=await fetch('/api/telegram');if(!chk(r))return;const j=await r.json();
  document.getElementById('tgEnabled').checked=j.enabled||false;
  document.getElementById('tgToken').value=j.token||'';
  document.getElementById('tgChatId').value=j.chatId||'';
  document.getElementById('tgAccess').checked=j.notifyAccess!==false;
  document.getElementById('tgBlocked').checked=j.notifyBlocked!==false;
  document.getElementById('tgUnknown').checked=j.notifyUnknown||false;
}
async function saveTelegram(){
  const r=await fetch('/api/telegram',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({
    enabled:document.getElementById('tgEnabled').checked,
    token:document.getElementById('tgToken').value.trim(),
    chatId:document.getElementById('tgChatId').value.trim(),
    notifyAccess:document.getElementById('tgAccess').checked,
    notifyBlocked:document.getElementById('tgBlocked').checked,
    notifyUnknown:document.getElementById('tgUnknown').checked
  })});
  if(r.ok)alert('Guardado.');else alert('Error al guardar');
}
async function loadTz(){
  const r=await fetch('/api/timezone');if(!chk(r))return;const j=await r.json();
  const sel=document.getElementById('tzPosix');
  const v=j.posix||'<-06>6';
  for(let i=0;i<sel.options.length;i++){if(sel.options[i].value===v){sel.selectedIndex=i;return;}}
}
async function loadRtc(){
  const r=await fetch('/api/rtc');if(!chk(r))return;const j=await r.json();
  const st=document.getElementById('rtcStatus');
  st.textContent=j.rtcOk?'Conectado':'No detectado';
  st.className='tag'+(j.rtcOk?'':' red');
  const hn=document.getElementById('rtcNow');
  if(j.local){hn.textContent=j.local.replace('T',' ');document.getElementById('rtcDatetime').value=j.local;}
  else hn.textContent='sin hora';
}
async function setRtcTime(){
  const v=document.getElementById('rtcDatetime').value;
  if(!v){alert('Ingresa fecha y hora');return;}
  const [date,time]=v.split('T');
  const [year,month,day]=date.split('-').map(Number);
  const parts=(time||'00:00:00').split(':').map(Number);
  const r=await fetch('/api/rtc/set',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({year,month,day,hour:parts[0],minute:parts[1],second:parts[2]||0})});
  if(r.ok){alert('Hora establecida.');loadRtc();}else alert('Error al establecer hora');
}
async function saveTz(){
  const v=document.getElementById('tzPosix').value;
  const r=await fetch('/api/timezone',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({posix:v})});
  if(r.ok)alert('Zona horaria guardada.');else alert('Error al guardar');
}
async function loadAdmins(){
  const r=await fetch('/api/admin/users');if(!chk(r))return;const j=await r.json();
  const el=document.getElementById('admins');
  if(!j.length){el.innerHTML='<div class="empty">No hay admins</div>';return;}
  el.innerHTML=j.map(a=>`<div class="row"><div class="name">${a.username}</div><button class="warn" style="width:auto;padding:6px 10px" onclick="delAdmin('${a.username}')">Borrar</button></div>`).join('');
}
async function addAdmin(){
  const u=document.getElementById('newAdminUser').value.trim();
  const p=document.getElementById('newAdminPass').value;
  if(!u||!p){alert('Completa usuario y contrasena');return;}
  if(p.length<6){alert('El password debe tener al menos 6 caracteres');return;}
  const r=await fetch('/api/admin/users/add',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:u,password:p})});
  const j=await r.json();
  if(r.ok){document.getElementById('newAdminUser').value='';document.getElementById('newAdminPass').value='';loadAdmins();}
  else alert(j.error||'Error');
}
async function changePass(){
  const o=document.getElementById('cpOld').value;
  const n=document.getElementById('cpNew').value;
  const n2=document.getElementById('cpNew2').value;
  if(!o||!n||!n2){alert('Completa todos los campos');return;}
  if(n.length<6){alert('El nuevo password debe tener al menos 6 caracteres');return;}
  if(n!==n2){alert('Los passwords nuevos no coinciden');return;}
  const r=await fetch('/api/admin/changepass',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({oldPassword:o,newPassword:n})});
  const j=await r.json();
  if(r.ok){alert('Password cambiado correctamente');document.getElementById('cpOld').value='';document.getElementById('cpNew').value='';document.getElementById('cpNew2').value='';}
  else alert(j.error||'Error');
}
async function delAdmin(u){
  if(!confirm('Borrar admin '+u+'?'))return;
  const r=await fetch('/api/admin/users/delete?username='+encodeURIComponent(u));
  const j=await r.json();
  if(r.ok)loadAdmins();else alert(j.error||'Error');
}
loadStatus();loadLogs();setInterval(loadLogs,5000);
</script></body></html>
)rawliteral";

// ================= HANDLERS =================
void handleRoot() {
  if (!requireAuth()) return;
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  if (!requireAuth()) return;
  String apSsid, apPass; loadApConfig(apSsid, apPass);
  JsonDocument doc;
  doc["time"]    = currentTimestamp();
  doc["apIp"]    = WiFi.softAPIP().toString();
  doc["apSsid"]  = apSsid;
  doc["apOpen"]  = (apPass.length() == 0);
  if (WiFi.status() == WL_CONNECTED) {
    doc["staIp"] = WiFi.localIP().toString();
    doc["ssid"]  = WiFi.SSID();
  } else {
    doc["staIp"] = "";
    doc["ssid"]  = "";
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleLogs() {
  if (!requireAuth()) return;
  if (!LittleFS.exists("/logs.json")) { server.send(200, "application/json", "[]"); return; }
  File f = LittleFS.open("/logs.json", "r");
  server.streamFile(f, "application/json");
  f.close();
}

void handleLogsCsv() {
  if (!requireAuth()) return;
  JsonDocument doc; loadLogs(doc);
  String out = "fecha,nombre,codigo\n";
  for (JsonObject e : doc.as<JsonArray>()) {
    out += String((const char*)e["ts"]) + "," +
           String((const char*)e["name"]) + "," +
           String(e["code"].as<unsigned long>()) + "\n";
  }
  server.sendHeader("Content-Disposition", "attachment; filename=logs.csv");
  server.send(200, "text/csv", out);
}

void handleLogsClear() {
  if (!requireAuth()) return;
  LittleFS.remove("/logs.json");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleUsers() {
  if (!requireAuth()) return;
  JsonDocument users; loadUsers(users);
  JsonDocument out;
  out["users"]       = users.as<JsonArray>();
  out["learning"]    = learnName;
  out["cloneWarning"] = learnCloneOf;
  learnCloneOf = "";
  String s; serializeJson(out, s);
  server.send(200, "application/json", s);
}

void handleLearnStart() {
  if (!requireAuth()) return;
  if (!server.hasArg("name")) { server.send(400, "text/plain", "name requerido"); return; }
  learnName = server.arg("name");
  Serial.printf("[LEARN] esperando codigo para: %s\n", learnName.c_str());
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleLearnCancel() {
  if (!requireAuth()) return;
  learnName = "";
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleUserDelete() {
  if (!requireAuth()) return;
  if (!server.hasArg("code")) { server.send(400, "text/plain", "code requerido"); return; }
  unsigned long code = strtoul(server.arg("code").c_str(), nullptr, 10);
  JsonDocument doc; loadUsers(doc);
  JsonArray arr = doc.as<JsonArray>();
  for (int i = arr.size() - 1; i >= 0; i--) {
    if (arr[i]["code"].as<unsigned long>() == code) arr.remove(i);
  }
  saveUsers(doc);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleWifiSet() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "body requerido"); return; }
  JsonDocument doc; deserializeJson(doc, server.arg("plain"));
  saveWiFiConfig(doc["ssid"].as<String>(), doc["pass"].as<String>());
  server.send(200, "application/json", "{\"ok\":true}");
  delay(500); ESP.restart();
}

void handleApSet() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "body requerido"); return; }
  JsonDocument doc; deserializeJson(doc, server.arg("plain"));
  String ssid = doc["ssid"].as<String>();
  String pass = doc["pass"].as<String>();
  if (ssid.length() == 0) { server.send(400, "text/plain", "SSID vacio"); return; }
  if (pass.length() > 0 && pass.length() < 8) {
    server.send(400, "text/plain", "password debe tener >= 8 caracteres"); return;
  }
  saveApConfig(ssid, pass);
  Serial.printf("[AP] config nueva: %s (%s)\n", ssid.c_str(), pass.length() ? "protegido" : "abierto");
  server.send(200, "application/json", "{\"ok\":true}");
  delay(500); ESP.restart();
}

void handleTelegramGet() {
  if (!requireAuth()) return;
  JsonDocument doc; loadTelegramConfig(doc);
  String s; serializeJson(doc, s);
  server.send(200, "application/json", s);
}

void handleTelegramSet() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "body requerido"); return; }
  JsonDocument doc; deserializeJson(doc, server.arg("plain"));
  saveTelegramConfig(doc);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleTzGet() {
  if (!requireAuth()) return;
  String posix; loadTzConfig(posix);
  server.send(200, "application/json", "{\"posix\":\"" + posix + "\"}");
}

void handleTzSet() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "body requerido"); return; }
  JsonDocument doc; deserializeJson(doc, server.arg("plain"));
  String posix = doc["posix"].as<String>();
  if (posix.length() == 0) { server.send(400, "text/plain", "posix requerido"); return; }
  saveTzConfig(posix);
  setenv("TZ", posix.c_str(), 1);
  tzset();
  Serial.printf("[TZ] zona cambiada: %s\n", posix.c_str());
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleUserToggle() {
  if (!requireAuth()) return;
  if (!server.hasArg("code")) { server.send(400, "text/plain", "code requerido"); return; }
  unsigned long code = strtoul(server.arg("code").c_str(), nullptr, 10);
  JsonDocument doc; loadUsers(doc);
  for (JsonObject u : doc.as<JsonArray>()) {
    if (u["code"].as<unsigned long>() == code) {
      u["blocked"] = !(u["blocked"] | false);
      break;
    }
  }
  saveUsers(doc);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRtcGet() {
  if (!requireAuth()) return;
  JsonDocument doc;
  doc["rtcOk"]  = rtcOk;
  doc["timeOk"] = timeOk;
  // hora local actual (desde sistema, que puede venir del RTC o NTP)
  time_t now; time(&now);
  struct tm lt; localtime_r(&now, &lt);
  char local[20]; strftime(local, sizeof(local), "%Y-%m-%dT%H:%M:%S", &lt);
  if (timeOk) doc["local"] = local;
  // hora raw del chip DS3231
  if (rtcOk) {
    DateTime t = rtc.now();
    char raw[20]; snprintf(raw, sizeof(raw), "%04d-%02d-%02dT%02d:%02d:%02d",
      t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second());
    doc["rtcUtc"] = raw;
  }
  String s; serializeJson(doc, s);
  server.send(200, "application/json", s);
}

void handleRtcSet() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "body requerido"); return; }
  JsonDocument req; deserializeJson(req, server.arg("plain"));
  int yr = req["year"]   | 0;
  int mo = req["month"]  | 0;
  int dy = req["day"]    | 0;
  int hr = req["hour"]   | 0;
  int mn = req["minute"] | 0;
  int sc = req["second"] | 0;
  if (yr < 2020 || mo < 1 || mo > 12 || dy < 1 || dy > 31 || hr > 23 || mn > 59 || sc > 59) {
    server.send(400, "application/json", "{\"error\":\"fecha invalida\"}"); return;
  }
  // el usuario ingresa hora local; mktime() la convierte a UTC usando TZ
  struct tm t = {};
  t.tm_year = yr - 1900; t.tm_mon = mo - 1; t.tm_mday = dy;
  t.tm_hour = hr;        t.tm_min = mn;     t.tm_sec  = sc;
  t.tm_isdst = -1;
  time_t utc = mktime(&t);
  if (rtcOk) rtc.adjust(DateTime((uint32_t)utc));
  struct timeval tv; tv.tv_sec = utc; tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  timeOk = true;
  Serial.printf("[RTC] hora manual: %04d-%02d-%02d %02d:%02d:%02d local\n", yr, mo, dy, hr, mn, sc);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleUserRename() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"error\":\"body requerido\"}"); return; }
  JsonDocument req; deserializeJson(req, server.arg("plain"));
  unsigned long code = req["code"].as<unsigned long>();
  String name = req["name"].as<String>();
  name.trim();
  if (name.length() == 0 || name.length() > 20) {
    server.send(400, "application/json", "{\"error\":\"nombre invalido\"}"); return;
  }
  JsonDocument doc; loadUsers(doc);
  bool found = false;
  for (JsonObject u : doc.as<JsonArray>()) {
    if (u["code"].as<unsigned long>() == code) { u["name"] = name; found = true; break; }
  }
  if (!found) { server.send(404, "application/json", "{\"error\":\"control no encontrado\"}"); return; }
  saveUsers(doc);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleLoginGet() {
  String token = extractCookie(server.header("Cookie"), "session");
  if (token.length() > 0 && isValidSession(token)) {
    server.sendHeader("Location", "/"); server.send(302, "text/plain", ""); return;
  }
  server.send_P(200, "text/html", LOGIN_HTML);
}

void handleLoginPost() {
  String username = server.arg("username");
  String password = server.arg("password");
  if (username.length() == 0 || password.length() == 0) {
    server.send(400, "text/plain", "Datos incompletos"); return;
  }
  String hash = md5Hex(password);
  JsonDocument doc; loadAdmins(doc);
  for (JsonObject a : doc.as<JsonArray>()) {
    if (a["username"].as<String>() == username && a["hash"].as<String>() == hash) {
      String token = createSession(username);
      server.sendHeader("Set-Cookie", "session=" + token + "; Path=/; HttpOnly");
      server.send(200, "text/plain", "OK");
      Serial.printf("[AUTH] login: %s\n", username.c_str());
      return;
    }
  }
  server.send(401, "text/plain", "Credenciales incorrectas");
}

void handleLogout() {
  String token = extractCookie(server.header("Cookie"), "session");
  if (token.length() > 0) invalidateSession(token);
  server.sendHeader("Set-Cookie", "session=; Path=/; Max-Age=0; HttpOnly");
  server.sendHeader("Location", "/login");
  server.send(302, "text/plain", "");
}

void handleAdminUsers() {
  if (!requireAuth()) return;
  JsonDocument doc; loadAdmins(doc);
  JsonDocument out;
  JsonArray arr = out.to<JsonArray>();
  for (JsonObject a : doc.as<JsonArray>()) {
    JsonObject item = arr.add<JsonObject>();
    item["username"] = a["username"].as<String>();
  }
  String s; serializeJson(out, s);
  server.send(200, "application/json", s);
}

void handleAdminAdd() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"error\":\"body requerido\"}"); return; }
  JsonDocument req; deserializeJson(req, server.arg("plain"));
  String username = req["username"].as<String>();
  String password = req["password"].as<String>();
  if (username.length() == 0 || password.length() < 6) {
    server.send(400, "application/json", "{\"error\":\"usuario invalido o password muy corto\"}"); return;
  }
  JsonDocument doc; loadAdmins(doc);
  JsonArray arr = doc.as<JsonArray>();
  if ((int)arr.size() >= MAX_ADMINS) {
    server.send(400, "application/json", "{\"error\":\"limite de admins alcanzado\"}"); return;
  }
  for (JsonObject a : arr) {
    if (a["username"].as<String>() == username) {
      server.send(400, "application/json", "{\"error\":\"usuario ya existe\"}"); return;
    }
  }
  JsonObject nu = arr.add<JsonObject>();
  nu["username"] = username;
  nu["hash"]     = md5Hex(password);
  saveAdmins(doc);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleAdminChangePass() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"error\":\"body requerido\"}"); return; }
  JsonDocument req; deserializeJson(req, server.arg("plain"));
  String oldPass = req["oldPassword"].as<String>();
  String newPass = req["newPassword"].as<String>();
  if (newPass.length() < 6) {
    server.send(400, "application/json", "{\"error\":\"el nuevo password debe tener al menos 6 caracteres\"}"); return;
  }
  String username = sessionUsername();
  JsonDocument doc; loadAdmins(doc);
  for (JsonObject a : doc.as<JsonArray>()) {
    if (a["username"].as<String>() == username) {
      if (a["hash"].as<String>() != md5Hex(oldPass)) {
        server.send(401, "application/json", "{\"error\":\"password actual incorrecto\"}"); return;
      }
      a["hash"] = md5Hex(newPass);
      saveAdmins(doc);
      server.send(200, "application/json", "{\"ok\":true}");
      Serial.printf("[AUTH] password cambiado: %s\n", username.c_str());
      return;
    }
  }
  server.send(404, "application/json", "{\"error\":\"usuario no encontrado\"}");
}

void handleAdminDelete() {
  if (!requireAuth()) return;
  if (!server.hasArg("username")) { server.send(400, "application/json", "{\"error\":\"username requerido\"}"); return; }
  String username = server.arg("username");
  JsonDocument doc; loadAdmins(doc);
  JsonArray arr = doc.as<JsonArray>();
  if ((int)arr.size() <= 1) {
    server.send(400, "application/json", "{\"error\":\"no se puede borrar el ultimo admin\"}"); return;
  }
  for (int i = arr.size() - 1; i >= 0; i--) {
    if (arr[i]["username"].as<String>() == username) arr.remove(i);
  }
  saveAdmins(doc);
  server.send(200, "application/json", "{\"ok\":true}");
}

void setupWebServer() {
  const char* hdrs[] = {"Cookie"};
  server.collectHeaders(hdrs, 1);

  server.on("/",                       handleRoot);
  server.on("/login",      HTTP_GET,   handleLoginGet);
  server.on("/login",      HTTP_POST,  handleLoginPost);
  server.on("/logout",                 handleLogout);
  server.on("/api/status",             handleStatus);
  server.on("/api/logs",               handleLogs);
  server.on("/api/logs/clear",         handleLogsClear);
  server.on("/logs.csv",               handleLogsCsv);
  server.on("/api/users",              handleUsers);
  server.on("/api/users/delete",       handleUserDelete);
  server.on("/api/users/toggle",       handleUserToggle);
  server.on("/api/users/rename",  HTTP_POST, handleUserRename);
  server.on("/api/learn/start",        handleLearnStart);
  server.on("/api/learn/cancel",       handleLearnCancel);
  server.on("/api/wifi",   HTTP_POST,  handleWifiSet);
  server.on("/api/ap",     HTTP_POST,  handleApSet);
  server.on("/api/admin/users",                      handleAdminUsers);
  server.on("/api/admin/users/add",       HTTP_POST, handleAdminAdd);
  server.on("/api/admin/users/delete",               handleAdminDelete);
  server.on("/api/admin/changepass",      HTTP_POST, handleAdminChangePass);
  server.on("/api/telegram",              HTTP_GET,  handleTelegramGet);
  server.on("/api/telegram",              HTTP_POST, handleTelegramSet);
  server.on("/api/timezone",              HTTP_GET,  handleTzGet);
  server.on("/api/timezone",              HTTP_POST, handleTzSet);
  server.on("/api/rtc",                   HTTP_GET,  handleRtcGet);
  server.on("/api/rtc/set",               HTTP_POST, handleRtcSet);
  server.begin();
  Serial.println("[WEB] servidor iniciado");
}

// ================= SETUP / LOOP =================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Porton 433MHz v2 ===");

  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  if (!LittleFS.begin(true)) Serial.println("[FS] error montando LittleFS");

  bootstrapFirstAdmin();
  applyTimezone();

  Wire.begin();
  if (rtc.begin()) {
    rtcOk = true;
    Serial.println("[RTC] DS3231 listo");
    syncSystemFromRtc();
  } else {
    Serial.println("[RTC] DS3231 no encontrado (opcional)");
  }

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  setupWiFi();
  setupWebServer();

  mySwitch.enableReceive(digitalPinToInterrupt(RF_RX_PIN));
  Serial.println("[RF] escuchando 433MHz en GPIO " + String(RF_RX_PIN));
  Serial.println("[BTN] mantene BOOT por 5s para factory reset del AP");
}

void loop() {
  server.handleClient();
  checkResetButton();

  // relay no-bloqueante: apagar cuando vence el tiempo
  if (relayEndMs > 0 && millis() >= relayEndMs) {
    digitalWrite(RELAY_PIN, LOW);
    relayEndMs = 0;
  }

  // limpiar sesiones vencidas cada 60 segundos
  if (millis() - lastSessionCleanMs >= 60000UL) {
    lastSessionCleanMs = millis();
    cleanExpiredSessions();
  }

  if (mySwitch.available()) {
    unsigned long code = mySwitch.getReceivedValue();
    unsigned int  bits = mySwitch.getReceivedBitlength();
    mySwitch.resetAvailable();

    if (code == 0) return;
    if (code == lastCode && (millis() - lastDetectionMs) < COOLDOWN_MS) return;
    lastCode = code; lastDetectionMs = millis();

    Serial.printf("[RF] codigo=%lu bits=%u\n", code, bits);

    if (learnName.length() > 0) {
      JsonDocument doc; loadUsers(doc);
      JsonArray arr = doc.as<JsonArray>();
      bool existed = false;
      String oldName = "";
      for (JsonObject u : arr) {
        if (u["code"].as<unsigned long>() == code) {
          oldName = u["name"].as<String>();
          u["name"] = learnName;
          existed = true;
          break;
        }
      }
      if (!existed) {
        JsonObject nu = arr.add<JsonObject>();
        nu["code"] = code; nu["name"] = learnName; nu["blocked"] = false;
      }
      learnCloneOf = (existed && oldName != learnName) ? oldName : "";
      if (learnCloneOf.length() > 0)
        Serial.printf("[CLON] codigo %lu ya existia como '%s'\n", code, oldName.c_str());
      saveUsers(doc);
      Serial.printf("[LEARN] %s -> %lu\n", learnName.c_str(), code);
      learnName = "";
      return;
    }

    // buscar usuario y verificar si esta bloqueado
    String name = "";
    bool blocked = false;
    bool known = false;
    JsonDocument usersDoc; loadUsers(usersDoc);
    for (JsonObject u : usersDoc.as<JsonArray>()) {
      if (u["code"].as<unsigned long>() == code) {
        name    = u["name"].as<String>();
        blocked = u["blocked"] | false;
        known   = true;
        break;
      }
    }
    if (!known) name = "Desconocido";

    if (known && !blocked) {
      digitalWrite(RELAY_PIN, HIGH);
      relayEndMs = millis() + RELAY_PULSE_MS;
      Serial.printf("[RELAY] activado para %s\n", name.c_str());
      sendTelegram("notifyAccess", "Acceso: " + name + " - " + currentTimestamp());
    } else if (blocked) {
      Serial.printf("[BLOQUEADO] %s (codigo %lu)\n", name.c_str(), code);
      sendTelegram("notifyBlocked", "BLOQUEADO: " + name + " - " + currentTimestamp());
    } else {
      sendTelegram("notifyUnknown", "Desconocido: codigo " + String(code) + " - " + currentTimestamp());
    }

    addLog(name, code, blocked && known);
    Serial.printf("[LOG] %s (codigo %lu)%s\n", name.c_str(), code, blocked ? " [BLOQUEADO]" : "");
  }
}
