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
 *     - WiFi de casa: borrado (vuelve a conectarse solo al AP)
 *   (Los logs, controles y admins NO se borran.)
 *
 * CAMBIAR PASSWORD DEL AP:
 *   1. Conectate a la web del porton.
 *   2. Pestana "Red" -> seccion "AP del porton".
 *   3. Poner SSID y password nuevos -> Guardar.
 *   4. El ESP32 reinicia. Te reconectas con la nueva red.
 * ============================================================
 */

#include <Arduino.h>
#include "soc/soc.h"           // registros SoC — para desactivar el brownout detector
#include "soc/rtc_cntl_reg.h"  // RTC_CNTL_BROWN_OUT_REG
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>   // acceso por nombre: porton-XXXX.local (sin necesitar la IP)
#include <DNSServer.h> // portal cautivo: resuelve cualquier dominio al AP (192.168.4.1)
#include <LittleFS.h>
#include <RCSwitch.h>
#include <SPI.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>  // SmartRC-CC1101-Driver (LSatan) — instalar desde gestor de librerias
#include <ArduinoJson.h>
#include <time.h>
#include <MD5Builder.h>   // core ESP32 — MD5 para hash de passwords admin
#include <Update.h>       // core ESP32 — actualizacion OTA por HTTP desde el panel
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <RTClib.h>       // Adafruit RTClib — instalar desde gestor de librerias

// =================== CONFIG ===================
// --- CC1101 SPI (SmartRC-CC1101-Driver) ---
// MOSI=23, MISO=19, SCK=18 (SPI por defecto del ESP32)
#define CC1101_CS_PIN      5                // SPI CSN / SS
#define RF_RX_PIN          27               // GDO0 — pin de interrupcion RX
#define RF_TX_PIN          32               // GDO2 — pin de transmision TX
// ------------------------------------------
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
#define RELAY_PULSE_MS     1000             // valor por defecto (ms) — se sobreescribe con /relay.json
#define PROVISION_DURATION_MS 15000         // 15 seg transmitiendo codigo nuevo
// Potencia de TX. Baja a proposito: el LNA SPF5189Z esta cableado RFIN->antena,
// RFOUT->CC1101, y el CC1101 usa UN SOLO puerto RF. Al transmitir (aprovisionamiento)
// la potencia entra invertida por la SALIDA del LNA, que no esta diseñada para
// recibirla: a +12 dBm se degrada o se quema, y un LNA degradado no deja de conducir,
// solo pierde ganancia -> se cae el alcance de RECEPCION, que es lo que importa.
// Valores validos del CC1101 en 433 MHz: -30 -20 -15 -10 0 5 7 10 12.
// A 0 dBm son 16x menos potencia inversa; el clonador debe estar cerca del equipo.
// La solucion definitiva es un conmutador TX/RX (PE4259) o sacar el LNA de la ruta TX.
#define CC1101_PA_DBM       0               // dBm en TX (aprovisionamiento)
// Los controles de esta instalacion (SEG y genericos por igual) se capturaron como
// protocolo 6 = HT6P20B, 28 bits, pulso 487-500us. El aprovisionamiento debe emitir
// EN ESE MISMO FORMATO: antes mandaba protocolo 1 / 24 bits / 339us, que no
// corresponde a ningun control del sitio y los clonadores HT6P20B no lo reproducen.
#define RF_TX_PROTOCOL      6               // HT6P20B (450us nominal, sync 23:1 invertido)
#define RF_TX_PULSE_US    490               // medido de los controles reales
#define RF_TX_BITS         28               // HT6P20B
// Estructura de la trama: [ ID de 20 bits ][ boton 4 bits ][ 4 bits de cola ]
//   245589013 -> 11101010001101100100 0001 0101
//     9585445 -> 00001001001001000011 0010 0101
// Contrastado contra los 30 controles registrados en la instalacion:
//   - los ultimos DOS bits son "01" en los 30 (invariante duro)
//   - la cola completa es 0101 en 27 de 30; las 3 excepciones (0001, 1001) son
//     clones hechos con otro clonador, no controles de fabrica
//   - el nibble de boton mas frecuente es 0010 (19 de 30)
// Por eso se genera con cola 0101 y boton 0010: el patron real mayoritario. Generar
// los 28 bits al azar producia colas que ningun control del sitio emite.
#define RF_TX_TAIL_NIBBLE 0x5               // 0101 — 27 de 30 controles del sitio
#define RF_TX_DATA_NIBBLE 0x2               // 0010 — nibble de boton mas frecuente
// ==============================================

RCSwitch     mySwitch = RCSwitch();
WebServer    server(80);
DNSServer    dnsServer;              // portal cautivo (solo tiene sentido para clientes del AP)
#define DNS_PORT 53
RTC_DS3231   rtc;
bool         rtcOk = false;

String        learnName = "";
String        learnCloneOf = "";   // nombre previo si el codigo ya existia (posible clon)
unsigned long provisionCode     = 0;
String        provisionName     = "";
unsigned int  provisionBits     = 28;  // bits del codigo a transmitir (HT6P20B)
unsigned long provisionEndMs    = 0;
unsigned long provisionNextTxMs = 0;
bool          provisionTxReady  = false; // true cuando CC1101 ya esta en TX mode
bool          pendingRfReset    = false; // reinit solicitado desde handler, ejecutar en loop()
unsigned long lastCode = 0;
unsigned long lastDetectionMs = 0;
bool          timeOk = false;

// ---- diagnostico RF (sniffer de pulsos crudos en GDO0) ----
#define SNIFF_BUF 200
volatile uint16_t sniffDur[SNIFF_BUF];   // duraciones de pulso en µs
volatile uint16_t sniffIdx    = 0;       // cuantos pulsos capturados
volatile uint32_t sniffLastUs = 0;
bool          rfSniffing   = false;      // sniffer activo
bool          pendingSniff = false;      // solicitud desde handler
volatile uint32_t sniffPulses = 0;       // contador de pulsos SIN tope (sniffDur si se llena)

// ---- barrido de frecuencia ----
// El sniffer normal solo escucha en RF_MHZ: si el control transmite corrido, mide
// "no llega señal" y no dice hacia donde. El barrido recorre la banda contando
// pulsos en cada paso y devuelve el pico, o sea la frecuencia REAL del control.
// Se corre con BW angosta a proposito: con los 812 kHz de operacion (+-406 kHz)
// los pasos se solapan tanto que el pico no se puede localizar.
#define SCAN_MHZ_START   433.00
#define SCAN_MHZ_STEP      0.05
#define SCAN_STEPS           37   // 433.00 -> 434.80
#define SCAN_STEP_MS        250
// VARIAS PASADAS, acumulando. Con una sola pasada lo que se mide es en gran parte
// el azar de si la ventana de ese paso cayo sobre una rafaga del control o sobre un
// silencio: el perfil salia con huecos imposibles (una frecuencia alta, la de al
// lado casi cero, la siguiente alta otra vez) y dos barridos seguidos daban picos
// distintos. Repartir el tiempo en pasadas promedia ese azar; la respuesta en
// frecuencia se mantiene pasada a pasada, la suerte de la sincronia no.
#define SCAN_PASSES           3   // 3 x 37 x 250ms = ~28 s
#define SCAN_RXBW_KHZ       101   // +-50 kHz: resolucion suficiente para separar pasos
bool          pendingScan  = false;
bool          rfScanning   = false;
uint8_t       scanIdx      = 0;
uint8_t       scanPass     = 0;
unsigned long scanEndMs    = 0;
uint16_t      scanCount[SCAN_STEPS];
int8_t        scanRssi[SCAN_STEPS];    // RSSI pico por frecuencia: medida de POTENCIA,
                                       // no de flancos, asi que no depende de si la
                                       // ventana cayo sobre una rafaga o un silencio
bool          scanResReady = false;
float         scanBestMhz  = 0;
uint16_t      scanBestN    = 0;
int8_t        scanBestRssi = -128;
int8_t        scanFloorRssi = 0;       // mediana del RSSI = ruido de fondo
unsigned long rfSniffEndMs = 0;          // fin de la ventana de captura
// El diagnostico corre en 2 fases sobre el mismo pin GDO0 (no se pueden usar
// el sniffer crudo y RCSwitch a la vez): fase 1 mide si LLEGA señal (pulsos
// crudos), fase 2 mide si RCSwitch la DECODIFICA (=> codigo fijo, clonable).
#define SNIFF_PHASE_MS 4000              // duracion de cada fase
uint8_t       sniffPhase = 0;            // 0=inactivo, 1=crudo, 2=decodifica
// resultado acumulado de la fase de decodificacion
bool          sniffDecoded = false;      // RCSwitch decodifico un codigo
uint32_t      sniffDecCode = 0;          // codigo decodificado
uint16_t      sniffDecBits = 0;          // bits del codigo decodificado
uint16_t      sniffDecProto = 0;         // numero de protocolo RCSwitch
uint16_t      sniffDecPulse = 0;         // duracion del pulso base (µs)
// resultado del ultimo diagnostico (para mostrar en la GUI)
bool          sniffResReady = false;     // hay un resultado nuevo sin leer
int8_t        sniffRssiPeak = -128;    // RSSI maximo durante la captura (dBm)
int8_t        sniffRssiFloor = 0;      // ruido de fondo medido antes de la captura
uint16_t      sniffResN = 0, sniffResMin = 0, sniffResMax = 0;
uint32_t      sniffResAvg = 0;

// ---- sintonia del receptor CC1101 ----
// OJO con el ancho de banda: la libreria deja m4RxBw=0 y escribe MDMCFG4 = 7+0,
// o sea el CC1101 arranca en 812 kHz, su ajuste MAS ANCHO y mas ruidoso. Por eso
// llamar setRxBW(325) NO es "volver al default": es angostar de 812 a 325 y ganar
// sensibilidad. 325 kHz es el punto probado: suficientemente angosto para mejorar
// el alcance y suficientemente ancho (+-162 kHz) para seguir oyendo controles con
// el cristal corrido. Bajar a 102 kHz (+-51 kHz) los deja fuera y no se oye nada.
// RF_NARROW_RXBW=0 => se deja la BW ancha (812 kHz) del build f210a0f, donde los
// SEG y los genericos funcionaban. Angostar a 325 kHz (+-162 kHz) gana ~4 dB pero
// deja fuera al control cuyo cristal esta corrido: en esta instalacion los SEG
// dejaron de oirse. No activarlo sin probar TODOS los controles en sitio.
// Sintonia ajustable en caliente desde el panel (persistida en /rf.json). El valor
// correcto depende de los controles del SITIO y solo se puede hallar probando ahi:
// angostar la BW gana sensibilidad (~4 dB de 812 a 325 = ~1.6x distancia) pero cierra
// la ventana de frecuencia y deja fuera a los controles con el cristal corrido.
// Poder probarlo sin recompilar es lo que evita repetir el ciclo de romper los SEG.
float    rfMhzCfg  = 433.92;   // frecuencia central
uint16_t rfRxBwCfg = 812;      // ancho de banda RX en kHz
// Predeterminados de fabrica de la sintonia (solo si NO existe /rf.json: lo guardado
// desde el panel siempre manda). 433.80/325 es el punto de partida elegido en sitio.
#define RF_NARROW_RXBW      1
#define RF_RXBW_KHZ       325
// Tolerancia de RCSwitch. El default (60) es el del build donde ambos tipos
// andaban. Subirla a 80 hace que un protocolo generico "reclame" primero la
// trama del SEG y salga un codigo distinto al registrado => "Desconocido".
#define RF_RX_TOLERANCE     60
#define CC1101_PROBE_MS   300            // espera maxima a que MISO baje antes de darlo por ausente
#define CC1101_SCK_PIN     18
#define CC1101_MISO_PIN    19
#define CC1101_MOSI_PIN    23
#define CC1101_PROBE_TRIES  3            // lecturas independientes: un chip vivo da SIEMPRE el mismo ID
// Un control 433MHz real produce CIENTOS de pulsos de 300-1000µs en la ventana de
// 4s. Menos de esto, o pulsos demasiado cortos, es ruido electrico en el pin: no
// se debe concluir "codigo rodante" a partir de dos transiciones sueltas.
#define SNIFF_MIN_REAL_PULSES 20         // por debajo = no llego señal util
#define SNIFF_MIN_REAL_PULSE_US 200      // pulso promedio minimo para considerarlo RF real
#define RF_MHZ            433.80

// estado del boton de reset
unsigned long btnPressStart = 0;
bool          btnWasPressed = false;
bool          btnSeenReleased = false;  // true cuando GPIO0 se leyo HIGH al menos una vez
unsigned long lastBlinkMs = 0;
bool          ledState = false;

unsigned long lastSessionCleanMs = 0;
// ---- reconexion automatica del WiFi de casa (STA) ----
String        staSsidSaved;                   // credenciales para reconectar sin releer flash
String        staPassSaved;
String        mdnsHost;                        // nombre mDNS (porton-XXXX) -> porton-XXXX.local
bool          staWasConnected  = false;       // para detectar transicion desconectado->conectado
unsigned long lastWifiCheckMs  = 0;
#define WIFI_CHECK_INTERVAL_MS  2000UL        // solo mirar el estado: barato, no reintenta
// Reintento con espera creciente. Cada WiFi.begin() obliga a la radio (una sola) a barrer
// todos los canales, y mientras eso ocurre el AP Porton_Config se vuelve lento o no aparece.
// Si la red de casa no esta, reintentar cada 20 s degradaba el AP de forma permanente.
#define WIFI_RETRY_MIN_MS      30000UL        // primer reintento (deja terminar el intento previo)
#define WIFI_RETRY_MAX_MS     300000UL        // tope: 5 min entre reintentos
unsigned long wifiRetryDelayMs = WIFI_RETRY_MIN_MS;
unsigned long lastWifiTryMs    = 0;
bool          wifiPendingBegin  = false;   // disconnect() hecho, begin() en el siguiente tick
unsigned long relayEndMs    = 0;
uint32_t      relayPulseMs  = RELAY_PULSE_MS;  // configurable desde UI
bool          logUnknown    = true;              // registrar codigos desconocidos

struct Session {
  char token[33];
  char username[32];
  unsigned long createdMs;
  bool active;
};
Session sessions[MAX_SESSIONS];

// ---- cache de usuarios en RAM (evita leer flash en cada deteccion RF) ----
struct UserEntry {
  unsigned long code;
  char          name[21];
  bool          blocked;
  unsigned int  bits;   // bits capturados al aprender (0 = desconocido, usar 24)
};
#define MAX_USERS_CACHE 50
UserEntry userCache[MAX_USERS_CACHE];
int       userCacheCount = 0;

// ---- cola FreeRTOS para envio de Telegram sin bloquear el loop ----
struct TgMsg {
  char event[24];
  char message[220];
};
static QueueHandle_t tgQueue = nullptr;

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
void   rebuildUserCache();
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
void   handleNtpSync();
void   applyRfRadioConfig();
bool   probeCc1101();
// false = la radio no respondio al arrancar. Todo el codigo que habla con el
// CC1101 debe respetarlo: la libreria ELECHOUSE espera a MISO con bucles
// while(digitalRead(MISO_PIN)) SIN timeout, asi que con el modulo muerto o un
// cable flojo el setup() se cuelga, loop() nunca corre y el panel web queda
// inaccesible aunque el AP se vea. Este flag evita ese cuelgue.
bool          rfHardwareOk = false;
bool   loadRelayConfig();
bool   loadRfConfig();
void   saveRfConfig();
void   saveRelayConfig(uint32_t ms, bool logUnk);
void   handleRelayConfigGet();
void   handleRelayConfigSet();

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

// Reconstruye el cache RAM de usuarios. Llamar tras cualquier saveUsers().
void rebuildUserCache() {
  userCacheCount = 0;
  JsonDocument doc; loadUsers(doc);
  for (JsonObject u : doc.as<JsonArray>()) {
    if (userCacheCount >= MAX_USERS_CACHE) break;
    userCache[userCacheCount].code    = u["code"].as<unsigned long>();
    userCache[userCacheCount].blocked = u["blocked"] | false;
    userCache[userCacheCount].bits    = u["bits"] | RF_TX_BITS;  // sin dato: HT6P20B
    strncpy(userCache[userCacheCount].name, u["name"].as<String>().c_str(), 20);
    userCache[userCacheCount].name[20] = '\0';
    userCacheCount++;
  }
  Serial.printf("[CACHE] %d usuarios en RAM\n", userCacheCount);
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

void addLog(const String &name, unsigned long code, bool blocked = false, unsigned int pulse = 0) {
  JsonDocument doc; loadLogs(doc);
  JsonArray arr = doc.as<JsonArray>();
  JsonObject entry = arr.add<JsonObject>();
  entry["ts"]      = currentTimestamp();
  entry["name"]    = name;
  entry["code"]    = code;
  if (blocked) entry["blocked"] = true;
  if (pulse > 0) entry["pulse"] = pulse;
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

// Copia en RAM de /telegram.json. Evita leer LittleFS en cada evento RF (el camino
// critico de apertura) y permite descartar el mensaje antes de encolarlo.
struct TgCache {
  bool   enabled = false;
  String token, chatId;
  bool   notifyAccess = true, notifyBlocked = true, notifyUnknown = false;
};
static TgCache tgCache;

void refreshTelegramCache() {
  JsonDocument cfg; loadTelegramConfig(cfg);
  tgCache.enabled       = cfg["enabled"]       | false;
  tgCache.token         = cfg["token"].as<String>();
  tgCache.chatId        = cfg["chatId"].as<String>();
  tgCache.notifyAccess  = cfg["notifyAccess"]  | true;
  tgCache.notifyBlocked = cfg["notifyBlocked"] | true;
  tgCache.notifyUnknown = cfg["notifyUnknown"] | false;
}

// true si Telegram esta realmente utilizable: activado, con token y chatId.
bool telegramConfigured() {
  return tgCache.enabled && tgCache.token.length() > 0 && tgCache.chatId.length() > 0;
}

// Envia un mensaje Telegram (bloqueo real de red). Solo llamar desde telegramTask.
static void _sendTelegramBlocking(const char *eventType, const char *message) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!telegramConfigured()) return;
  String token  = tgCache.token;
  String chatId = tgCache.chatId;

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
  Serial.println("[TG] mensaje enviado: " + String(message));
}

// Tarea FreeRTOS (core 0): procesa la cola de mensajes Telegram sin bloquear el loop.
static void telegramTask(void *pvParameters) {
  TgMsg msg;
  for (;;) {
    if (xQueueReceive(tgQueue, &msg, portMAX_DELAY) == pdTRUE) {
      _sendTelegramBlocking(msg.event, msg.message);
    }
  }
}

// Encola un mensaje Telegram (no bloquea). Si la cola esta llena se descarta.
void sendTelegram(const String &eventType, const String &message) {
  if (tgQueue == nullptr) return;
  // Si no esta configurado o no hay WiFi de casa, no encolar nada: el mensaje no
  // podria salir y solo ocuparia la cola (y antes gastaba una lectura de LittleFS).
  if (!telegramConfigured() || WiFi.status() != WL_CONNECTED) return;
  if (eventType == "notifyAccess"  && !tgCache.notifyAccess)  return;
  if (eventType == "notifyBlocked" && !tgCache.notifyBlocked) return;
  if (eventType == "notifyUnknown" && !tgCache.notifyUnknown) return;
  TgMsg msg;
  strncpy(msg.event,   eventType.c_str(), sizeof(msg.event)   - 1); msg.event[sizeof(msg.event)-1]     = '\0';
  strncpy(msg.message, message.c_str(),   sizeof(msg.message) - 1); msg.message[sizeof(msg.message)-1] = '\0';
  xQueueSend(tgQueue, &msg, 0);   // no bloquear si la cola esta llena
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

// ================= RF RADIO CONFIG =================
// Sondea el CC1101 ANTES de tocar la libreria: baja CSN y espera a que MISO se
// ponga en bajo (el chip indica asi que esta listo). Con timeout, a diferencia
// de la libreria. Si no responde, no se llama a Init() y el arranque continua.
// Espera a CHIP_RDYn: el CC1101 pone MISO en bajo cuando la alimentacion y el
// cristal se estabilizaron. CON timeout, a diferencia de la libreria ELECHOUSE.
// Devuelve false si nunca bajo (modulo ausente, sin 3.3V, o MISO desconectado).
static bool cc1101WaitReady() {
  uint32_t t0 = millis();
  while (digitalRead(CC1101_MISO_PIN) == HIGH) {
    if (millis() - t0 >= CC1101_PROBE_MS) return false;
  }
  return true;
}

// Reset manual de encendido, seccion 19.1.2 del datasheet. Obligatorio cuando el
// chip no puede asumirse en un estado conocido — que es exactamente nuestro caso:
// la sonda corre ANTES de ELECHOUSE_cc1101.Init(), asi que el CC1101 esta recien
// alimentado y jamas fue reseteado. Sin esto, una lectura de VERSION en 0x00 o
// erratica no prueba que el modulo este muerto.
static bool cc1101ManualReset() {
  // SCLK=1, MOSI=0 antes de tocar CSn, para no caer en el modo pin-control.
  pinMode(CC1101_SCK_PIN, OUTPUT);  digitalWrite(CC1101_SCK_PIN, HIGH);
  pinMode(CC1101_MOSI_PIN, OUTPUT); digitalWrite(CC1101_MOSI_PIN, LOW);
  digitalWrite(CC1101_CS_PIN, LOW);
  delayMicroseconds(5);
  digitalWrite(CC1101_CS_PIN, HIGH);
  delayMicroseconds(45);            // >=40us con CSn en alto
  SPI.begin(CC1101_SCK_PIN, CC1101_MISO_PIN, CC1101_MOSI_PIN, CC1101_CS_PIN);
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(CC1101_CS_PIN, LOW);
  bool ok = cc1101WaitReady();      // CHIP_RDYn antes del strobe
  if (ok) {
    SPI.transfer(0x30);             // SRES
    ok = cc1101WaitReady();         // MISO vuelve a bajar: reset completo, chip en IDLE
  }
  digitalWrite(CC1101_CS_PIN, HIGH);
  SPI.endTransaction();
  return ok;
}

// Lee un registro de estado. El bit BURST es OBLIGATORIO aqui: sin el, el header
// se interpreta como un command strobe en vez de una lectura.
static uint8_t cc1101ReadStatus(uint8_t reg) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(CC1101_CS_PIN, LOW);
  uint8_t val = 0xFF;
  if (cc1101WaitReady()) {
    SPI.transfer(reg | 0xC0);       // READ | BURST
    val = SPI.transfer(0x00);
  }
  digitalWrite(CC1101_CS_PIN, HIGH);
  SPI.endTransaction();
  return val;
}

bool probeCc1101() {
  // Sondeo REAL por SPI, sin usar la libreria: reset manual y luego PARTNUM (0x30)
  // + VERSION (0x31). Leer solo el pin MISO NO sirve: flotando puede leerse como
  // bajo y hacer creer que el chip respondio (fue el fallo original).
  //
  // Se repite CC1101_PROBE_TRIES veces porque el sintoma decisivo no es el valor
  // sino su ESTABILIDAD: un chip vivo devuelve siempre el mismo ID, mientras que
  // un bus con ruido (masa deficiente, cables largos, MOSI/SCK cruzados) devolvio
  // aqui 0x7D, 0xA2, 0x01, 0x08 y 0x00 en arranques sucesivos.
  pinMode(CC1101_CS_PIN, OUTPUT);
  digitalWrite(CC1101_CS_PIN, HIGH);

  if (!cc1101ManualReset()) {
    Serial.println("[RF] CC1101 no responde al reset manual (MISO nunca bajo):");
    Serial.println("[RF]   sin 3.3V en VDD, GND sin continuidad, o MISO/GPIO19 desconectado");
    return false;
  }

  uint8_t partnum = 0, version = 0;
  bool stable = true;
  for (int i = 0; i < CC1101_PROBE_TRIES; i++) {
    uint8_t p = cc1101ReadStatus(0x30);   // PARTNUM: 0x00 en el CC1101 autentico
    uint8_t v = cc1101ReadStatus(0x31);   // VERSION: 0x04 / 0x14 / 0x17
    if (i == 0) { partnum = p; version = v; }
    else if (p != partnum || v != version) stable = false;
    Serial.printf("[RF] sonda %d/%d: PARTNUM=0x%02X VERSION=0x%02X\n",
                  i + 1, CC1101_PROBE_TRIES, p, v);
  }

  // Valores validos documentados por TI. Aceptar "!=0x00 && !=0xFF" no basta: un
  // modulo quemado devolvio 0x7D, paso esa validacion y colgo el arranque despues.
  bool idOk = (version == 0x04 || version == 0x14 || version == 0x17) && partnum == 0x00;
  bool ok   = idOk && stable;

  if (ok) {
    Serial.printf("[RF] CC1101 VERSION=0x%02X (valido, %d lecturas identicas)\n",
                  version, CC1101_PROBE_TRIES);
  } else if (!stable) {
    Serial.println("[RF] CC1101 INVALIDO: las lecturas CAMBIAN entre si.");
    Serial.println("[RF]   eso es ruido en el bus, no un chip: revisa GND comun, cables");
    Serial.println("[RF]   cortos, y que SCK18/MISO19/MOSI23/CSN5 no esten cruzados.");
  } else {
    Serial.printf("[RF] CC1101 INVALIDO: PARTNUM=0x%02X VERSION=0x%02X "
                  "(se esperaba PARTNUM=0x00 y VERSION=0x04/0x14/0x17)\n", partnum, version);
    if (version == 0x00)      Serial.println("[RF]   0x00 fijo = MISO en bajo permanente: modulo sin alimentar");
    else if (version == 0xFF) Serial.println("[RF]   0xFF fijo = MISO flotando en alto: modulo ausente");
  }
  return ok;
}

// Un solo punto que programa la sintonia del CC1101. Lo llaman setup(),
// reinitRfReceiver() y el guardado desde la GUI, para que no se puedan
// desincronizar entre si (era la causa de que un reinit dejara otra BW).
void applyRfRadioConfig() {
  ELECHOUSE_cc1101.setMHZ(rfMhzCfg);
  ELECHOUSE_cc1101.setModulation(2);      // OOK/ASK — obligatorio para controles 433MHz
  ELECHOUSE_cc1101.setRxBW(rfRxBwCfg);    // 812 = el ancho por defecto del chip
  ELECHOUSE_cc1101.setPA(CC1101_PA_DBM);  // solo afecta a TX; ver la nota del define
}

// ================= RELAY CONFIG =================
bool loadRelayConfig() {
  relayPulseMs = RELAY_PULSE_MS;
  logUnknown   = true;
  if (!LittleFS.exists("/relay.json")) return true;
  File f = LittleFS.open("/relay.json", "r");
  if (!f) return true;
  JsonDocument doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    uint32_t v = doc["pulseMs"] | RELAY_PULSE_MS;
    if (v >= 100 && v <= 30000) relayPulseMs = v;
    logUnknown = doc["logUnknown"] | true;
  }
  f.close();
  return true;
}

// Solo se aceptan los anchos que el CC1101 puede sintetizar de verdad; cualquier
// otro valor la libreria lo redondea en silencio y el panel mentiria.
const uint16_t RF_BW_TABLE[] = {58, 68, 81, 102, 116, 135, 162, 203, 232, 270, 325, 406, 464, 541, 650, 812};
bool rfBwValid(uint16_t bw) {
  for (uint8_t i = 0; i < sizeof(RF_BW_TABLE)/sizeof(RF_BW_TABLE[0]); i++) if (RF_BW_TABLE[i] == bw) return true;
  return false;
}

bool loadRfConfig() {
  rfMhzCfg  = RF_MHZ;
  rfRxBwCfg = RF_NARROW_RXBW ? RF_RXBW_KHZ : 812;
  if (!LittleFS.exists("/rf.json")) return true;
  File f = LittleFS.open("/rf.json", "r");
  if (!f) return true;
  JsonDocument doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    float m = doc["mhz"] | (float)RF_MHZ;
    if (m >= 430.0 && m <= 436.0) rfMhzCfg = m;   // fuera de la banda no se acepta
    uint16_t bw = doc["rxbw"] | (uint16_t)812;
    if (rfBwValid(bw)) rfRxBwCfg = bw;
  }
  f.close();
  Serial.printf("[RF] sintonia guardada: %.2f MHz  BW %u kHz\n", rfMhzCfg, rfRxBwCfg);
  return true;
}

void saveRfConfig() {
  JsonDocument doc;
  doc["mhz"]  = rfMhzCfg;
  doc["rxbw"] = rfRxBwCfg;
  File f = LittleFS.open("/rf.json", "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

void saveRelayConfig(uint32_t ms, bool logUnk) {
  JsonDocument doc;
  doc["pulseMs"]    = ms;
  doc["logUnknown"] = logUnk;
  File f = LittleFS.open("/relay.json", "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

// ================= AUTH =================
String md5Hex(const String &input) {
  MD5Builder md5;
  md5.begin();
  md5.add(input);
  md5.calculate();
  return md5.toString();  // hex minusculas de 32 chars — mismo formato que antes
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

void startMdns();  // forward declaration

int apStartedChannel = 0;  // canal en el que quedo el AP (para no reiniciarlo sin necesidad)

// Levanta (o reinicia) el AP. Se limita el canal a 1-11: los canales 12-13 estan
// restringidos en muchos telefonos y el AP simplemente no aparece en el escaneo.
void startSoftAp(const String& ssid, const String& pass, int channel) {
  if (channel < 1 || channel > 11) channel = 1;
  bool ok = (pass.length() == 0)
              ? WiFi.softAP(ssid.c_str(), nullptr, channel)          // red abierta
              : WiFi.softAP(ssid.c_str(), pass.c_str(), channel);
  apStartedChannel = channel;
  Serial.printf("[AP] SSID=%s  IP=%s  canal=%d  %s\n", ssid.c_str(),
                WiFi.softAPIP().toString().c_str(), channel, ok ? "OK" : "FALLO");
}

void setupWiFi() {
  String apSsid, apPass;
  loadApConfig(apSsid, apPass);

  String staSsid, staPass;
  loadWiFiConfig(staSsid, staPass);
  staSsidSaved = staSsid;  // guardar para la reconexion automatica en loop()
  staPassSaved = staPass;

  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);  // el supplicant reintenta solo; ademas reforzamos desde loop()
  // Hostname "porton-XXXX" (XXXX = ultimos 2 bytes de la MAC) para identificarlo facil
  // en el router (reserva DHCP). Debe fijarse antes de WiFi.begin() para que el DHCP lo anuncie.
  uint8_t mac[6]; WiFi.macAddress(mac);
  char host[20]; snprintf(host, sizeof(host), "porton-%02X%02X", mac[4], mac[5]);
  WiFi.setHostname(host);
  WiFi.softAPsetHostname(host);
  mdnsHost = String(host);
  mdnsHost.toLowerCase();  // mDNS usa minusculas: porton-xxxx.local
  Serial.printf("[NET] hostname: %s\n", host);
  // Desactiva el modem power-save: por defecto el ESP32 en STA duerme la radio
  // entre balizas y agrega 100-300 ms de latencia a cada peticion HTTP (panel lento).
  WiFi.setSleep(false);
  // Potencia TX WiFi al maximo (+19.5 dBm) para maximo alcance.
  // OJO: sube el pico de corriente; requiere fuente/cable USB solido para evitar brownout.
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  // El AP se levanta YA, en el canal 1, y sin esperar nada: Porton_Config existe
  // desde el primer segundo. Si el STA conecta despues en otro canal, el loop()
  // reinicia el AP ahi (una sola radio no puede sostener dos canales).
  startSoftAp(apSsid, apPass, 1);

  if (staSsid.length() > 0) {
    // NO se espera aqui. La espera bloqueante de 15 s (mas 5 s de NTP) dejaba al
    // equipo sordo y sin panel durante todo el arranque cuando el WiFi de casa no
    // estaba: el porton no abria hasta que terminaba. El loop() ya detecta la
    // conexion (rama !staWasConnected) y ahi corre NTP, mDNS y el ajuste de canal.
    Serial.printf("[STA] conectando a %s en segundo plano...\n", staSsid.c_str());
    WiFi.begin(staSsid.c_str(), staPass.c_str());
  }


  // Portal cautivo: resuelve CUALQUIER dominio a la IP del AP. Sin esto el celular
  // no puede resolver connectivitycheck.gstatic.com, la sonda falla en seco y Android
  // muestra "la red no tiene internet". Con el DNS + el 302 de handleNotFound(), el
  // sistema detecta un portal y abre el panel solo al conectarse.
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  if (dnsServer.start(DNS_PORT, "*", WiFi.softAPIP()))
    Serial.printf("[DNS] portal cautivo activo -> %s\n", WiFi.softAPIP().toString().c_str());
  else
    Serial.println("[DNS] no se pudo iniciar el servidor DNS");

  // mDNS responde en ambas interfaces (AP y STA): porton-XXXX.local funciona
  // tanto conectado a Porton_Config (192.168.4.1) como al WiFi de casa.
  startMdns();
}

// Inicia (o reinicia) mDNS para poder entrar por http://porton-XXXX.local sin la IP.
void startMdns() {
  if (mdnsHost.length() == 0) return;
  MDNS.end();  // por si ya estaba activo (reconexion)
  if (MDNS.begin(mdnsHost.c_str())) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mDNS] activo -> http://%s.local\n", mdnsHost.c_str());
  } else {
    Serial.println("[mDNS] no se pudo iniciar");
  }
}

// ================= FACTORY RESET (boton BOOT) =================
void factoryResetAp() {
  Serial.println("\n[RESET] borrando config AP y WiFi... volviendo a defaults");
  LittleFS.remove("/ap.json");
  LittleFS.remove("/wifi.json");
  // feedback visual: 3 parpadeos rapidos
  for (int i = 0; i < 6; i++) {
    digitalWrite(LED_PIN, i % 2); delay(100);
  }
  delay(300);
  ESP.restart();
}

void checkResetButton() {
  // Antirrebote: el estado solo cambia si la lectura cruda se mantiene estable 60ms.
  // Filtra el ruido electrico en GPIO0 (evita falsos "presionado" y falsos reset).
  static bool          rawLast   = false;   // ultima lectura cruda
  static unsigned long rawSince  = 0;        // desde cuando es estable
  static bool          pressed   = false;    // estado debounced
  bool raw = (digitalRead(RESET_BUTTON_PIN) == LOW);
  if (raw != rawLast) { rawLast = raw; rawSince = millis(); }
  if (millis() - rawSince >= 60) pressed = raw;   // estable 60ms -> aceptar

  // Anti-bucle: solo armar el reset si el boton estuvo suelto (HIGH) desde el arranque.
  // Si GPIO0 esta en LOW al bootear (atascado, puente de soldadura, o algo cableado ahi)
  // NUNCA se dispara el factory reset -> evita el reinicio en bucle.
  if (!pressed) btnSeenReleased = true;
  if (!btnSeenReleased) return;

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
/* toasts (notificaciones elegantes en vez de alert) */
#toasts{position:fixed;left:50%;bottom:20px;transform:translateX(-50%);display:flex;flex-direction:column;gap:8px;z-index:1000;width:calc(100% - 32px);max-width:420px;pointer-events:none}
.toast{background:var(--card);border:1px solid #2a2e38;border-left:4px solid var(--acc);color:var(--txt);padding:12px 14px;border-radius:10px;font-size:13px;line-height:1.4;box-shadow:0 8px 24px rgba(0,0,0,.4);opacity:0;transform:translateY(10px);transition:opacity .25s,transform .25s;pointer-events:auto}
.toast.show{opacity:1;transform:translateY(0)}
.toast.ok{border-left-color:var(--acc)}
.toast.err{border-left-color:var(--err)}
.toast.warn{border-left-color:var(--warn)}
.toast.info{border-left-color:#60a5fa}
/* modal de confirmacion elegante */
#modal{position:fixed;inset:0;background:rgba(0,0,0,.6);display:none;align-items:center;justify-content:center;z-index:1100;padding:16px}
#modal.show{display:flex}
.modal-card{background:var(--card);border-radius:14px;padding:20px;max-width:360px;width:100%;box-shadow:0 12px 40px rgba(0,0,0,.5)}
.modal-card .mtxt{font-size:14px;line-height:1.5;margin-bottom:16px}
.modal-card .mbtns{display:flex;gap:8px}
/* animacion de captura RF */
.pulse-dot{display:inline-block;width:10px;height:10px;border-radius:50%;background:#1a1200;margin-right:8px;animation:pulse 1s infinite}
@keyframes pulse{0%,100%{opacity:.3}50%{opacity:1}}
/* overlay de reinicio a pantalla completa */
#reboot{position:fixed;inset:0;background:rgba(10,12,16,.96);display:none;flex-direction:column;align-items:center;justify-content:center;z-index:2000;padding:24px;text-align:center}
#reboot.show{display:flex}
#reboot .spin{width:52px;height:52px;border:5px solid #2a2e38;border-top-color:var(--acc);border-radius:50%;animation:spin 1s linear infinite;margin-bottom:22px}
@keyframes spin{to{transform:rotate(360deg)}}
#reboot .rt{font-size:19px;font-weight:700;margin-bottom:10px}
#reboot .rn{font-size:14px;color:var(--mut);line-height:1.5;max-width:360px;margin-bottom:18px}
#reboot .rc{font-size:13px;color:var(--acc);font-weight:600}
</style></head><body>
<div id="reboot"><div class="spin"></div><div class="rt" id="rebootTitle">Reiniciando&hellip;</div><div class="rn" id="rebootNote"></div><div class="rc" id="rebootCount"></div></div>
<div id="modal"><div class="modal-card"><div class="mtxt" id="modalTxt"></div><div class="mbtns"><button class="sec" id="modalNo" style="flex:1">Cancelar</button><button id="modalYes" style="flex:1">Confirmar</button></div></div></div>
<div class="hdr"><h1>&#128682; Porton</h1><a href="/logout" style="text-decoration:none"><button class="sec" style="width:auto;padding:6px 14px;font-size:12px">Salir</button></a></div>
<div class="sub" id="status">cargando...</div>
<div class="nav">
  <a href="#" class="active" data-tab="logs">Registros</a>
  <a href="#" data-tab="users">Controles</a>
  <a href="#" data-tab="net">Red</a>
  <a href="#" data-tab="admins">Admins</a>
</div>

<div id="tab-logs">
  <div class="card" style="padding:10px 14px;margin-bottom:8px">
    <div class="row" style="padding:4px 0;border-bottom:none">
      <span style="font-size:13px;font-weight:600">Registrar codigos desconocidos</span>
      <input type="checkbox" id="relayLogUnknown" style="width:auto;height:18px;width:18px;cursor:pointer" onchange="saveLogUnknown(this)">
    </div>
  </div>
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
  <div id="provBox" style="display:none;background:var(--warn);color:#1a1200;padding:12px;border-radius:8px;margin-bottom:10px;font-weight:600"></div>
  <div id="cloneBox" style="display:none;background:var(--err);color:#fff;padding:12px;border-radius:8px;margin-bottom:10px;font-weight:600"></div>
  <div id="sniffBox" style="display:none;padding:14px;border-radius:8px;margin-bottom:10px"></div>
  <div id="tuneBox" style="display:none;background:#1e293b;color:#e2e8f0;padding:14px;border-radius:8px;margin-bottom:10px">
    <b>&#127859; Sintonia del receptor</b>
    <div style="font-size:12px;opacity:.85;margin:6px 0 10px">
      Angostar el ancho de banda gana sensibilidad (de 812 a 325 kHz son ~4 dB, casi <b>1.6x distancia</b>)
      pero cierra la ventana de frecuencia: un control con el cristal corrido deja de oirse por completo.
      <b>Prueba TODOS los controles</b> despues de cada cambio.
    </div>
    <div style="display:flex;gap:8px;flex-wrap:wrap;align-items:flex-end">
      <label style="font-size:12px">Frecuencia (MHz)<br>
        <input id="tuneMhz" type="number" step="0.01" min="430" max="436" style="width:110px"></label>
      <label style="font-size:12px">Ancho de banda RX<br>
        <select id="tuneBw" style="width:150px">
          <option value="812">812 kHz (&plusmn;406) mas tolerante</option>
          <option value="650">650 kHz (&plusmn;325)</option>
          <option value="541">541 kHz (&plusmn;270)</option>
          <option value="464">464 kHz (&plusmn;232)</option>
          <option value="406">406 kHz (&plusmn;203)</option>
          <option value="325">325 kHz (&plusmn;162) mas alcance</option>
          <option value="270">270 kHz (&plusmn;135)</option>
          <option value="232">232 kHz (&plusmn;116)</option>
        </select></label>
      <button class="sec" style="width:auto;padding:6px 14px" onclick="tuneSave()">Aplicar</button>
      <button class="sec" style="width:auto;padding:6px 14px" onclick="tuneReset()">Volver al predeterminado (433.80 / 325)</button>
    </div>
    <div style="margin-top:8px;font-size:12px">
      <span style="opacity:.75">Frecuencias rapidas:</span>
      <span id="tunePresets"></span>
    </div>
    <div style="margin-top:12px;padding-top:10px;border-top:1px solid rgba(255,255,255,.2)">
      <b style="font-size:13px">Sintonias a probar</b>
      <div style="font-size:11px;opacity:.8;margin:4px 0 8px">
        Aplica una, corre <b>&#128269; Diagnostico RF</b> apretando el control desde el
        <b>mismo sitio</b> siempre, y el margen se anota solo. Gana el margen mas alto.
        <b>Etapa 1</b> busca el centro con ventana angosta; <b>etapa 2</b> abre o cierra
        la ventana ya centrado.
      </div>
      <div id="tuneList" style="font-family:monospace;font-size:11px"></div>
      <button class="sec" style="width:auto;padding:4px 10px;font-size:11px;margin-top:6px"
              onclick="tuneClear()">Borrar mediciones</button>
    </div>
    <div id="tuneWin" style="font-size:12px;font-family:monospace;margin-top:8px;opacity:.9"></div>
    <div id="tuneMsg" style="font-size:12px;margin-top:8px"></div>
  </div>
  <div id="rfDeadBox" style="display:none;background:var(--err);color:#fff;padding:12px;border-radius:8px;margin-bottom:10px;font-weight:600"></div>
  <div style="text-align:right;margin-bottom:6px;display:flex;gap:6px;justify-content:flex-end;flex-wrap:wrap">
    <button class="sec" style="width:auto;padding:6px 12px;font-size:12px" onclick="rfSniff()">&#128269; Diagnostico RF</button>
    <button class="sec" style="width:auto;padding:6px 12px;font-size:12px" onclick="rfScan()">&#128225; Buscar frecuencia</button>
    <button class="sec" style="width:auto;padding:6px 12px;font-size:12px" onclick="tuneToggle()">&#127859; Sintonia</button>
    <button class="sec" style="width:auto;padding:6px 12px;font-size:12px" onclick="rebootDevice()">&#128260; Reiniciar dispositivo</button>
    <a href="/users.csv" download="controles.csv" style="text-decoration:none"><button class="sec" style="width:auto;padding:6px 12px;font-size:12px">&#128229; Exportar Excel</button></a>
    <button class="sec" style="width:auto;padding:6px 12px;font-size:12px" onclick="document.getElementById('csvImportInput').click()">&#128228; Importar CSV</button>
    <input type="file" id="csvImportInput" accept=".csv,text/csv" style="display:none" onchange="importCsv(this)">
  </div>
  <h2>Apertura remota</h2>
  <div class="card">
    <button onclick="activateRelay()" style="font-size:16px;padding:14px">&#128275; Abrir ahora</button>
    <div id="relayBox" style="display:none;padding:10px;border-radius:8px;margin-top:8px;font-weight:600"></div>
    <div class="hint">Activa el relevador por el mismo tiempo configurado para los controles.</div>
  </div>
  <h2>Bloqueo por grupo</h2>
  <div class="card">
    <input type="text" id="prefixName" placeholder="Prefijo del nombre (ej. Casa 13)" maxlength="20">
    <div class="grid" style="margin-top:8px">
      <button class="blk" onclick="togglePrefix(1)">&#128274; Bloquear grupo</button>
      <button class="sec" onclick="togglePrefix(0)">&#128275; Desbloquear grupo</button>
    </div>
    <div class="hint">Afecta a todos los controles cuyo nombre empieza con ese texto. Ej. "Casa 13" bloquea "Casa 13 # 1", "Casa 13 # 2", etc.</div>
    <div id="prefixBox" style="display:none;padding:10px;border-radius:8px;margin-top:8px;font-weight:600"></div>
  </div>
  <h2>Controles guardados</h2>
  <div class="card" style="margin-bottom:10px">
    <input type="text" id="userSearch" placeholder="&#128269; Buscar por nombre o codigo" oninput="renderUsers()">
    <div class="hint" id="userCount"></div>
  </div>
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
  <h2>Generar control unico</h2>
  <div class="card">
    <input type="text" id="provName" placeholder="Nombre (ej. Visitante)" maxlength="20">
    <div class="grid" style="margin-top:8px">
      <button onclick="startProvision()">Generar y transmitir</button>
      <button class="sec" onclick="cancelProvision()">Cancelar</button>
    </div>
    <div class="hint">El porton genera un codigo unico y lo transmite 15 seg por el CC1101. Pon el control en modo aprendizaje y apuntalo al porton. Si no grabo, usa el boton <b>Retransmitir 15s</b> que aparece en el aviso amarillo.</div>
  </div>
</div>

<div id="tab-net" style="display:none">
  <h2>Relevador</h2>
  <div class="card">
    <div class="row" style="padding:6px 0"><span>Pulso actual</span><span id="relayPulseLabel" class="tag" style="font-family:monospace">-</span></div>
    <label style="font-size:12px;color:var(--mut);display:block;margin-top:10px;margin-bottom:4px">Duracion del pulso (segundos)</label>
    <input type="number" id="relayPulseSec" min="0.1" max="30" step="0.1" placeholder="1" style="margin-bottom:10px">
    <button onclick="saveRelayPulse()">Guardar</button>
    <div class="hint">Tiempo que el relevador permanece activado al recibir un codigo valido. Rango: 0.1&nbsp;&ndash;&nbsp;30 segundos.</div>
  </div>
  <h2>AP del porton</h2>
  <div class="card">
    <div style="margin-bottom:8px">Actual: <span id="apInfo" class="tag">-</span></div>
    <input type="text" id="apSsid" placeholder="SSID (ej. Porton_Config)" maxlength="32">
    <input type="password" id="apPass" placeholder="Password (min. 8 caracteres)" style="margin-top:8px" maxlength="63">
    <button style="margin-top:10px" onclick="saveAp()">Guardar y reiniciar</button>
    <button class="warn" style="margin-top:8px" onclick="factoryReset()">&#9888;&#65039; Restablecer red de fábrica</button>
    <div class="hint">Restablecer deja el AP en Porton_Config / porton1234 y borra el WiFi de casa (los controles, registros y admins se conservan). Equivale a mantener apretado el botón BOOT 5 segundos.</div>
  </div>
  <h2>Actualizar firmware (OTA)</h2>
  <div class="card">
    <input type="file" id="otaFile" accept=".bin,application/octet-stream" style="display:none" onchange="otaUpload(this)">
    <button class="sec" onclick="document.getElementById('otaFile').click()">&#128228; Seleccionar firmware.bin</button>
    <div id="otaBar" style="display:none;height:10px;border-radius:6px;background:#00000022;margin-top:10px;overflow:hidden">
      <div id="otaFill" style="height:100%;width:0%;background:var(--ok,#3fb950);transition:width .2s"></div>
    </div>
    <div id="otaMsg" style="display:none;padding:10px;border-radius:8px;margin-top:8px;font-weight:600"></div>
    <div class="hint">Sube el archivo <b>firmware.bin</b> generado con PlatformIO (<code>.pio/build/esp32dev/firmware.bin</code>). El equipo se reinicia solo al terminar. Si la subida falla o se corta, sigue arrancando la versión anterior. No se puede actualizar con un aprendizaje, provisión o diagnóstico en curso.</div>
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
    <div id="tgState" style="display:none;padding:8px 10px;border-radius:8px;margin-bottom:10px;font-size:13px;font-weight:600"></div>
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
    <div class="grid" style="margin-top:8px">
      <button class="sec" onclick="exportTelegram()">&#128229; Exportar config</button>
      <button class="sec" onclick="document.getElementById('tgImportInput').click()">&#128228; Importar config</button>
    </div>
    <input type="file" id="tgImportInput" accept=".json,application/json" style="display:none" onchange="importTelegram(this)">
    <div id="tgIoBox" style="display:none;padding:10px;border-radius:8px;margin-top:8px;font-weight:600"></div>
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
  <h2>Reloj / Hora del sistema</h2>
  <div class="card">
    <div class="row" style="padding:6px 0"><span>Modulo DS3231</span><span id="rtcStatus" class="tag">-</span></div>
    <div class="row" style="padding:6px 0"><span>Hora actual (local)</span><span id="rtcNow" class="tag" style="font-family:monospace">-</span></div>
    <button style="margin-top:10px" onclick="syncNtp(this)">&#128257; Sincronizar NTP ahora</button>
    <div class="hint" style="margin-bottom:0">Requiere WiFi de casa. Aplica la zona horaria configurada arriba.</div>
    <h2 style="margin:14px 0 6px">Establecer hora manual</h2>
    <div class="hint" style="margin-bottom:8px">Funciona sin internet y sin modulo DS3231. Ingresar en hora local segun la zona configurada arriba.</div>
    <input type="datetime-local" id="rtcDatetime" step="1" style="margin-bottom:10px">
    <button onclick="setRtcTime()">Establecer hora</button>
    <div class="hint">Si hay DS3231 conectado, tambien actualiza el chip de reloj.</div>
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
var curTab='logs';
function tab(id){
  curTab=id;
  document.querySelectorAll('.nav a').forEach(a=>a.classList.toggle('active',a.dataset.tab===id));
  ['logs','users','net','admins'].forEach(t=>document.getElementById('tab-'+t).style.display=t===id?'':'none');
  if(id==='logs'){loadLogs();loadRelayPulse();}if(id==='users')loadUsers();if(id==='net'){loadStatus();loadTelegram();loadTz();loadRtc();loadRelayPulse();}if(id==='admins')loadAdmins();
}
document.querySelectorAll('.nav a').forEach(a=>a.addEventListener('click',e=>{e.preventDefault();tab(a.dataset.tab);}));
async function loadRelayPulse(){
  const r=await fetch('/api/relay/config');if(!chk(r))return;const j=await r.json();
  const ms=j.pulseMs||1000;
  const el=document.getElementById('relayPulseLabel');
  if(el)el.textContent=(ms/1000).toFixed(1)+' s';
  const sec=document.getElementById('relayPulseSec');
  if(sec)sec.value=(ms/1000).toFixed(1);
  const cb=document.getElementById('relayLogUnknown');
  if(cb)cb.checked=j.logUnknown!==false;
}
async function saveLogUnknown(cb){
  const sec=parseFloat(document.getElementById('relayPulseSec')?.value||'1');
  const ms=isNaN(sec)?relayPulseMs:(Math.round(sec*1000)||1000);
  await fetch('/api/relay/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({pulseMs:ms,logUnknown:cb.checked})});
}
async function saveRelayPulse(){
  const sec=parseFloat(document.getElementById('relayPulseSec').value);
  if(isNaN(sec)||sec<0.1||sec>30){alert('Ingresa un valor entre 0.1 y 30 segundos');return;}
  const ms=Math.round(sec*1000);
  const cb=document.getElementById('relayLogUnknown');
  const logUnk=cb?cb.checked:true;
  const r=await fetch('/api/relay/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({pulseMs:ms,logUnknown:logUnk})});
  const j=await r.json();
  if(j.ok){const el=document.getElementById('relayPulseLabel');if(el)el.textContent=sec.toFixed(1)+' s';alert('Guardado.');}
  else alert(j.error||'Error al guardar');
}
function mdnsTag(m){
  if(!m)return '';
  return '<br><span style="font-size:12px">Acceso por nombre: <a href="http://'+m+'" style="color:#4ade80;font-weight:600">http://'+m+'</a> <span style="color:#888">(sin necesitar la IP)</span></span>';
}
function rssiTag(rssi,ch){
  if(rssi===undefined)return '';
  let label,color;
  if(rssi>=-60){label='excelente';color='#14532d';}
  else if(rssi>=-70){label='buena';color='#14532d';}
  else if(rssi>=-80){label='regular';color='#78350f';}
  else{label='débil';color='#7f1d1d';}
  return '<br><span style="display:inline-block;margin-top:4px;padding:2px 8px;border-radius:6px;font-size:11px;background:'+color+';color:#fff">señal '+label+' '+rssi+' dBm'+(ch?' · canal '+ch:'')+'</span>';
}
async function loadStatus(){
  const r=await fetch('/api/status');if(!chk(r))return;const j=await r.json();
  document.getElementById('status').textContent=j.time+' - '+(j.staIp||'sin WiFi casa')+' - AP: '+j.apIp;
  document.getElementById('wifiStatus').innerHTML=j.staIp?('Conectado a '+j.ssid+' ('+j.staIp+')'+mdnsTag(j.mdns)+rssiTag(j.rssi,j.channel)):'No conectado';
  document.getElementById('apInfo').innerHTML=j.apSsid+(j.apOpen?' (abierta)':' (protegida)')+(!j.staIp?mdnsTag(j.mdns):'');
  document.getElementById('apSsid').value=j.apSsid;
}
async function loadLogs(){
  const r=await fetch('/api/logs');if(!chk(r))return;const j=await r.json();
  const el=document.getElementById('logs');
  if(!j.length){el.innerHTML='<div class="empty">Sin registros todavia</div>';return;}
  el.innerHTML=j.slice().reverse().map(e=>`<div class="row"><div><div class="name">${e.name||'Desconocido'}${e.blocked?' <span class="tag red">BLOQUEADO</span>':''}</div><div class="meta">codigo ${e.code}${e.pulse?' &bull; pulse <b>'+e.pulse+'µs</b>':''}</div></div><div class="meta">${e.ts}</div></div>`).join('');
}
let allUsers=[];
function renderUsers(){
  const el=document.getElementById('users');
  const q=(document.getElementById('userSearch').value||'').trim().toLowerCase();
  let list=allUsers.slice().sort((a,b)=>a.name.localeCompare(b.name,'es',{numeric:true,sensitivity:'base'}));
  if(q)list=list.filter(u=>u.name.toLowerCase().includes(q)||String(u.code).includes(q));
  document.getElementById('userCount').textContent=q?(list.length+' de '+allUsers.length+' controles'):(allUsers.length+' controles');
  if(!allUsers.length){el.innerHTML='<div class="empty">Todavia no hay controles aprendidos</div>';return;}
  if(!list.length){el.innerHTML='<div class="empty">Ningun control coincide con la busqueda</div>';return;}
  el.innerHTML=list.map(u=>`<div class="row"><div><div class="name">${u.name}${u.blocked?' <span class="tag red">BLOQUEADO</span>':''}</div><div class="meta">codigo ${u.code}${u.bits?' &bull; '+u.bits+'bits':''}</div></div><div style="display:flex;gap:6px;flex-wrap:wrap"><button class="sec" style="width:auto;padding:6px 10px" onclick="transmitUser(${u.code},'${u.name.replace(/'/g,"\\'")}')">&#128225; Transmitir</button><button class="sec" style="width:auto;padding:6px 10px" onclick="renameUser(${u.code},'${u.name.replace(/'/g,"\\'")}')">Renombrar</button><button class="${u.blocked?'':'blk'}" style="width:auto;padding:6px 10px" onclick="toggleUser(${u.code})">${u.blocked?'Habilitar':'Bloquear'}</button><button class="warn" style="width:auto;padding:6px 10px" onclick="delUser(${u.code})">Borrar</button></div></div>`).join('');
}
async function loadUsers(){
  const r=await fetch('/api/users',{cache:'no-store'});if(!chk(r))return;const j=await r.json();
  allUsers=j.users;
  renderUsers();
  rfDeadCheck(j);
  const lb=document.getElementById('learnBox');
  lb.innerHTML=j.learning?`<div class="learning">Esperando boton del control para "${j.learning}"...</div>`:'';
  const pb=document.getElementById('provBox');
  if(j.provisioning){pb.innerHTML='&#128225; Transmitiendo codigo <b>'+j.provisionCode+'</b> para "'+j.provisioning+'"&hellip; Pon el control en modo aprendizaje.<br><div style="display:flex;gap:8px;margin-top:8px;flex-wrap:wrap"><button onclick="retransmitProvision()" style="padding:4px 10px;font-size:12px;background:#1a1200;color:#fbbf24;border:1px solid #fbbf24;border-radius:6px;cursor:pointer;font-weight:600">&#128257; Retransmitir 15s</button><button onclick="stopTransmit()" style="padding:4px 10px;font-size:12px;background:#1a1200;color:#f87171;border:1px solid #f87171;border-radius:6px;cursor:pointer;font-weight:600">&#9209; Detener</button></div>';pb.style.display='';}
  else pb.style.display='none';
  if(j.learning||j.provisioning)setTimeout(loadUsers,1500);
  const cb=document.getElementById('cloneBox');
  if(j.cloneWarning){cb.textContent='No se grabo: este control ya esta registrado como "'+j.cloneWarning+'". Si es un control distinto con el mismo codigo, puede ser un clon.';cb.style.display='';}
  else cb.style.display='none';
}
async function renameUser(code,current){
  const n=prompt('Nuevo nombre para "'+current+'":', current);
  if(!n||!n.trim()||n.trim()===current)return;
  const r=await fetch('/api/users/rename',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({code,name:n.trim()})});
  const j=await r.json();
  if(r.ok)loadUsers();else alert(j.error||'Error');
}
async function toggleUser(code){await fetch('/api/users/toggle?code='+code,{cache:'no-store'});loadUsers();}
async function activateRelay(){
  const box=document.getElementById('relayBox');
  const r=await fetch('/api/relay/activate');if(!chk(r))return;const j=await r.json();
  box.style.display='';box.style.color='#fff';
  if(r.ok){
    box.style.background='#14532d';
    const seg=(j.pulseMs/1000).toFixed(1);
    box.textContent='\u{1F513} Abriendo… rele activo '+seg+' s.';
    setTimeout(()=>{box.style.display='none';},Math.max(j.pulseMs,1500)+500);
  }else{box.style.background='#7f1d1d';box.textContent=j.error||'Error';}
}
function prefixMatch(name,prefix){
  const nl=name.toLowerCase(),pl=prefix.toLowerCase();
  if(nl===pl)return true;
  if(nl.startsWith(pl)&&nl.length>pl.length){
    const c=nl.charAt(pl.length);
    return !((c>='a'&&c<='z')||(c>='0'&&c<='9'));
  }
  return false;
}
async function togglePrefix(block){
  const p=document.getElementById('prefixName').value.trim();
  const box=document.getElementById('prefixBox');
  if(!p){box.style.display='';box.style.background='#78350f';box.style.color='#fff';box.textContent='Escribe un prefijo (ej. Casa 13).';return;}
  // previsualizar los afectados con la misma regla que el servidor
  const ru=await fetch('/api/users',{cache:'no-store'});if(!chk(ru))return;const ju=await ru.json();
  const hits=ju.users.filter(u=>prefixMatch(u.name,p)).map(u=>u.name);
  box.style.display='';box.style.color='#fff';
  if(hits.length===0){box.style.background='#78350f';box.textContent='Ningun control coincide con "'+p+'".';return;}
  const accion=block?'Bloquear':'Desbloquear';
  const lista=hits.map(n=>'<div style="padding:2px 0">&bull; '+n+'</div>').join('');
  const html='<b>'+accion+' '+hits.length+' control(es)</b> que coinciden con "'+p+'":'
    +'<div style="max-height:180px;overflow:auto;margin-top:10px;font-size:13px;color:var(--mut)">'+lista+'</div>';
  if(!await confirmModal(html,accion,block))return;
  const r=await fetch('/api/users/toggle-prefix?block='+block+'&prefix='+encodeURIComponent(p),{cache:'no-store'});
  const j=await r.json();
  if(!r.ok){box.style.display='';box.style.background='#7f1d1d';box.style.color='#fff';box.textContent=j.error||'Error';return;}
  box.style.display='';
  if(j.affected===0){box.style.background='#78350f';box.style.color='#fff';box.textContent='Ningun control empieza con "'+p+'".';}
  else{box.style.background='#14532d';box.style.color='#fff';box.textContent=(block?'Bloqueados':'Desbloqueados')+' '+j.affected+' control(es) de "'+p+'".';}
  loadUsers();
}
async function startLearn(){
  const name=document.getElementById('newName').value.trim();
  if(!name){alert('Ingresa un nombre');return;}
  await fetch('/api/learn/start?name='+encodeURIComponent(name));
  document.getElementById('newName').value='';loadUsers();
}
async function cancelLearn(){await fetch('/api/learn/cancel');loadUsers();}
async function startProvision(){
  const name=document.getElementById('provName').value.trim();
  if(!name){alert('Ingresa un nombre');return;}
  await fetch('/api/provision/start?name='+encodeURIComponent(name));
  document.getElementById('provName').value='';loadUsers();
}
async function cancelProvision(){await fetch('/api/provision/cancel');loadUsers();}
async function stopTransmit(){
  await fetch('/api/provision/cancel');
  loadUsers();
  sniffFlash('&#9209; Transmision detenida. Receptor RF restaurado.','#14532d');
}
function sniffFlash(html,color){
  const b=document.getElementById('sniffBox');
  b.style.display='';b.style.background=color;b.style.color='#fff';b.style.fontWeight='600';b.innerHTML=html;
  setTimeout(()=>{b.style.display='none';},4000);
}
async function retransmitProvision(){await fetch('/api/provision/retransmit');loadUsers();}
async function rfReset(){await fetch('/api/rf/reset');loadUsers();sniffFlash('&#128260; Receptor RF reiniciado.','#14532d');}
// Pie con la sintonia activa: hace que una captura del recuadro sea autosuficiente.
function rfCfgLine(j){
  const c=j&&j.rfCfg; if(!c)return '';
  return '<div style="margin-top:8px;padding-top:6px;border-top:1px solid rgba(255,255,255,.25);'
    +'font-size:11px;font-family:monospace;font-weight:400;opacity:.85">'
    +'RX '+c.mhz.toFixed(2)+' MHz &bull; BW '+c.rxbw+' kHz'+(c.narrow?'':' (ancha)')
    +' &bull; tol '+c.tol+'% &bull; PA '+(c.pa>=0?'+':'')+c.pa+' dBm<br>'
    +'TX proto '+c.txProto+' &bull; '+c.txBits+' bits &bull; '+c.txPulse+'µs</div>';
}
// Traduce RSSI + margen sobre el ruido a algo accionable. Es la medida que evita
// tener que caminar 30 m para comparar dos configuraciones.
function rssiLine(rssi,floor){
  if(rssi===undefined||rssi<=-128)return '';
  const m=rssi-floor;
  let txt,col;
  if(m>=30){txt='señal muy fuerte — sobra margen';col='#86efac';}
  else if(m>=20){txt='señal buena — alcance holgado';col='#86efac';}
  else if(m>=12){txt='señal aceptable — alcanza, sin mucho margen';col='#fcd34d';}
  else if(m>=6){txt='señal DEBIL — al limite, aqui es donde falla a distancia';col='#fdba74';}
  else{txt='casi indistinguible del ruido';col='#fca5a5';}
  return '<div style="margin-top:6px;font-weight:400">&#128246; <b>'+rssi+' dBm</b> '
    +'<span style="opacity:.8">(ruido '+floor+' dBm &rarr; <b>+'+m+' dB</b> de margen)</span><br>'
    +'<span style="color:'+col+'">'+txt+'</span></div>';
}
function sniffShow(html,color){
  const b=document.getElementById('sniffBox');
  b.style.display='';b.style.background=color;b.style.color='#fff';b.innerHTML=html;
}
function rfDeadCheck(j){
  const b=document.getElementById('rfDeadBox'); if(!b)return;
  if(j&&j.rfOk===false){
    b.style.display='';
    b.innerHTML='&#9888;&#65039; <b>La radio CC1101 no responde.</b><br>'
      +'<span style="font-weight:400;opacity:.9">El equipo arranco sin radio: el panel funciona, pero <b>no hay recepcion de controles ni clonado</b>. '
      +'Revisa VDD=3.3V (nunca 5V), GND y el cableado SPI: SCK&rarr;18, MISO&rarr;19, MOSI&rarr;23, CSN&rarr;5.</span>';
  } else b.style.display='none';
}
// Ventana real que cubre la sintonia: es lo que decide si un control entra o no,
// asi que se muestra en vivo en vez de obligar a calcular mhz +- bw/2 a mano.
function tuneWin(){
  const m=parseFloat(document.getElementById('tuneMhz').value);
  const b=parseInt(document.getElementById('tuneBw').value,10);
  const e=document.getElementById('tuneWin');
  if(isNaN(m)||isNaN(b)){e.innerHTML='';return;}
  const h=b/2000;
  e.innerHTML='Ventana: <b>'+(m-h).toFixed(3)+' &ndash; '+(m+h).toFixed(3)+' MHz</b> (&plusmn;'+(b/2)+' kHz)';
}
function tunePreset(v){document.getElementById('tuneMhz').value=v.toFixed(2);tuneWin();}
// Etapa 1: mismo ancho angosto, distintas frecuencias -> localiza el centro real.
// Etapa 2: ya centrado, distintos anchos -> elige el mas angosto que aun oiga a todos.
const TUNE_CANDS=[
 ['1',433.70,325],['1',433.80,325],['1',433.90,325],['1',434.00,325],['1',434.10,325],['1',434.20,325],
 ['2',433.92,812],['2',433.92,650],['2',433.92,541],['2',433.92,464],['2',433.92,406],['2',433.92,232]
];
function tuneKey(m,b){return 'tm_'+m.toFixed(2)+'_'+b;}
function tuneGet(m,b){try{return localStorage.getItem(tuneKey(m,b));}catch(e){return null;}}
// Guarda el margen medido para la sintonia que estaba aplicada durante el diagnostico.
function tuneRecord(cfg,margin){
  if(!cfg)return;
  try{localStorage.setItem(tuneKey(cfg.mhz,cfg.rxbw),String(margin));}catch(e){}
  tuneRender();
}
function tuneClear(){
  try{TUNE_CANDS.forEach(c=>localStorage.removeItem(tuneKey(c[1],c[2])));}catch(e){}
  tuneRender();
}
function tuneRender(){
  const el=document.getElementById('tuneList'); if(!el)return;
  let best=null;
  TUNE_CANDS.forEach(c=>{const v=tuneGet(c[1],c[2]);if(v!==null&&(best===null||+v>best))best=+v;});
  let h='',stage='';
  TUNE_CANDS.forEach(c=>{
    const [et,m,b]=c, v=tuneGet(m,b), half=b/2000;
    if(et!==stage){stage=et;h+='<div style="opacity:.7;margin:6px 0 2px">Etapa '+et
      +(et==='1'?' &mdash; buscar el centro (BW 325)':' &mdash; elegir el ancho (433.92)')+'</div>';}
    const win=(m-half).toFixed(2)+'&ndash;'+(m+half).toFixed(2);
    const win_=' <span style="opacity:.55">'+win+'</span>';
    let res='<span style="opacity:.5">sin medir</span>';
    if(v!==null){
      const top=(best!==null&&+v===best);
      res='<b style="color:'+(top?'#86efac':'#e2e8f0')+'">+'+v+' dB</b>'+(top?' &#11088;':'');
    }
    h+='<div style="display:flex;gap:6px;align-items:center;margin:2px 0">'
      +'<button class="sec" style="width:auto;padding:2px 8px;font-size:11px" '
      +'onclick="tuneApply('+m+','+b+')">Aplicar</button>'
      +'<span style="min-width:130px">'+m.toFixed(2)+' / '+b+'kHz</span>'+win_
      +'<span style="margin-left:auto">'+res+'</span></div>';
  });
  el.innerHTML=h;
}
async function tuneToggle(){
  const b=document.getElementById('tuneBox');
  if(b.style.display!=='none'){b.style.display='none';return;}
  const j=await (await fetch('/api/users',{cache:'no-store'})).json();
  if(j.rfCfg){document.getElementById('tuneMhz').value=j.rfCfg.mhz.toFixed(2);
              document.getElementById('tuneBw').value=j.rfCfg.rxbw;}
  const p=document.getElementById('tunePresets');
  if(!p.innerHTML){
    let h='';
    for(let f=433.30;f<=434.51;f+=0.15)
      h+='<button class="sec" style="width:auto;padding:3px 8px;font-size:11px;margin:2px" '
        +'onclick="tunePreset('+f.toFixed(2)+')">'+f.toFixed(2)+'</button>';
    p.innerHTML=h;
  }
  document.getElementById('tuneMhz').oninput=tuneWin;
  document.getElementById('tuneBw').onchange=tuneWin;
  tuneWin();
  tuneRender();
  document.getElementById('tuneMsg').innerHTML='';
  b.style.display='';
}
async function tuneApply(mhz,rxbw){
  const m=document.getElementById('tuneMsg');
  m.innerHTML='<span style="opacity:.8">Aplicando&hellip;</span>';
  const r=await fetch('/api/rf/tune',{method:'POST',headers:{'Content-Type':'application/json'},
                                      body:JSON.stringify({mhz:mhz,rxbw:rxbw})});
  const j=await r.json();
  if(j.error){m.innerHTML='<span style="color:#fca5a5">&#9888;&#65039; '+j.error+'</span>';return;}
  m.innerHTML='&#9989; Aplicado: <b>'+mhz.toFixed(2)+' MHz</b>, BW <b>'+rxbw+' kHz</b>. '
    +'Guardado, sobrevive al reinicio.<br><span style="color:#fcd34d">Prueba AHORA todos los controles. '
    +'Si alguno deja de responder, vuelve a 812 kHz.</span>';
  document.getElementById('tuneMhz').value=mhz.toFixed(2);
  document.getElementById('tuneBw').value=rxbw;
}
function tuneSave(){
  tuneApply(parseFloat(document.getElementById('tuneMhz').value),
            parseInt(document.getElementById('tuneBw').value,10));
}
function tuneReset(){tuneApply(433.80,325);}

// Barrido de frecuencia: encuentra en que MHz transmite realmente un control.
async function rfScan(){
  const r0=await fetch('/api/rf/scan');
  const j0=await r0.json();
  if(j0.error){sniffShow('&#9888;&#65039; '+j0.error,'#7f1d1d');return;}
  let left=Math.ceil((j0.ms||15000)/1000);
  const tick=()=>sniffShow('&#128225; <b>Buscando frecuencia&hellip;</b><br>'
    +'<span style="opacity:.9;font-weight:400">MANTEN APRETADO el control todo el barrido, sin soltarlo. <b>'+left+'s</b></span>','#1e3a5f');
  tick();
  const iv=setInterval(()=>{left--;if(left>0)tick();},1000);
  setTimeout(async()=>{
    clearInterval(iv);
    sniffShow('&#8987; Procesando barrido&hellip;','#1e3a5f');
    let r=await fetch('/api/users',{cache:'no-store'});let j=await r.json();
    for(let i=0;i<8&&!j.scanResult;i++){await new Promise(s=>setTimeout(s,500));r=await fetch('/api/users',{cache:'no-store'});j=await r.json();}
    const s=j.scanResult;
    if(!s){sniffShow('&#9888;&#65039; No se pudo leer el resultado del barrido. Reintenta.','#7f1d1d');return;}
    // perfil en texto: una barra por paso, proporcional al pico
    // Perfil por RSSI (potencia), que es la medida fiable; el conteo de pulsos queda
    // como dato secundario. Barra proporcional a los dB sobre el fondo.
    const fl=s.floor, top=Math.max(1,s.bestRssi-fl);
    const row=p=>{
      const db=Math.max(0,p.rssi-fl);
      const bar='&#9608;'.repeat(Math.round(db*18/top));
      const hit=(s.bestMhz&&Math.abs(p.mhz-s.bestMhz)<0.001);
      return '<span style="opacity:'+(hit?1:.65)+'">'+p.mhz.toFixed(2)+' '+bar+' '+p.rssi+'dBm'+(hit?' &larr;':'')+'</span>';
    };
    const prof=s.points.map(row).join('<br>');
    // Perfil visible por defecto (solo los pasos con señal apreciable): asi una
    // captura de pantalla del recuadro ya contiene la medicion completa.
    const visProf=s.points.filter(p=>p.rssi-fl>=3).map(row).join('<br>');
    const det='<div style="font-family:monospace;font-size:11px;line-height:1.5;margin-top:8px;'
      +'padding-top:6px;border-top:1px solid rgba(255,255,255,.25);font-weight:400">'
      +(visProf||'<i>sin pasos por encima del ruido</i>')+'</div>'
      +'<details style="margin-top:6px"><summary style="cursor:pointer;font-weight:400;opacity:.8;font-size:11px">Ver los '+s.points.length+' pasos</summary>'
      +'<div style="font-family:monospace;font-size:11px;line-height:1.5;margin-top:6px">'+prof+'</div></details>'
      +rfCfgLine(j);
    if(!s.bestMhz){
      sniffShow('&#10060; <b>Sin pico claro.</b><br><span style="opacity:.85;font-weight:400">'
        +'O no se apreto el control durante todo el barrido, o transmite fuera de 433.0-434.8 MHz, o no llega señal a la antena.</span>'+det,'#7f1d1d');
      return;
    }
    const dif=Math.abs(s.bestMhz-s.rfMhz);
    const nota=dif<0.06
      ? '&#9989; Coincide con la sintonia actual ('+s.rfMhz.toFixed(2)+' MHz): la frecuencia no es el problema.'
      : '&#9888;&#65039; <b>Esta corrido '+(dif*1000).toFixed(0)+' kHz</b> respecto a la sintonia actual ('+s.rfMhz.toFixed(2)+' MHz).<br>'
        +'Ponlo en <b>&#127859; Sintonia</b> a '+s.bestMhz.toFixed(2)+' MHz (se aplica al momento, sin recompilar).';
    sniffShow('&#128225; <b>Transmite en ~'+s.bestMhz.toFixed(2)+' MHz</b> <span style="opacity:.8;font-weight:400">('
      +s.bestRssi+' dBm, ruido '+s.floor+' dBm, +'+(s.bestRssi-s.floor)+' dB)</span><br>'
      +'<span style="font-weight:400">'+nota+'</span>'+det, dif<0.06?'#14532d':'#78350f');
  },(j0.ms||15000)+1200);
}

async function rfSniff(){
  await fetch('/api/rf/sniff');
  // 2 fases de 4s: primero mide si llega señal, luego si es decodificable (codigo fijo)
  let left=8;
  const tick=()=>{
    const fase=left>4?'1/2 &mdash; midiendo señal':'2/2 &mdash; intentando decodificar';
    sniffShow('&#128269; <b>Diagnostico fase '+fase+'</b><br><span style="opacity:.9;font-weight:400">MANTEN APRETADO el control apuntando al receptor. <b>'+left+'s</b></span>','#1e3a5f');
  };
  tick();
  const iv=setInterval(()=>{left--;if(left>0)tick();},1000);
  setTimeout(async()=>{
    clearInterval(iv);
    sniffShow('&#8987; Procesando captura&hellip;','#1e3a5f');
    let r=await fetch('/api/users',{cache:'no-store'});let j=await r.json();
    for(let i=0;i<6&&!j.sniffResult;i++){await new Promise(s=>setTimeout(s,500));r=await fetch('/api/users',{cache:'no-store'});j=await r.json();}
    const s=j.sniffResult;
    if(!s){sniffShow('&#9888;&#65039; No se pudo leer el resultado. Reintenta el diagnostico.','#7f1d1d');return;}
    const stats='min '+s.min+'µs &bull; max '+s.max+'µs &bull; prom '+s.avg+'µs &bull; '+s.n+' pulsos';
    // anotar el margen para la sintonia con la que se acaba de medir
    if(s.rssi!==undefined&&s.rssi>-128) tuneRecord(j.rfCfg,s.rssi-s.floor);
    if(s.decoded){
      let ident;
      if(s.knownName!==undefined){
        const est=s.knownBlocked?' <span style="background:#7f1d1d;padding:1px 6px;border-radius:4px">BLOQUEADO</span>':' <span style="background:#14532d;padding:1px 6px;border-radius:4px">habilitado</span>';
        ident='&#128100; <b>Control ya registrado:</b> "'+s.knownName+'"'+est;
      }else{
        ident='&#10024; <b>Control NUEVO</b> (no registrado) &mdash; se puede aprender/clonar.';
      }
      const tec='Codigo <b>'+s.code+'</b> · '+s.bits+' bits · protocolo <b>'+(s.proto||'?')+'</b> · pulso '+(s.pulse||0)+'µs';
      sniffShow('&#9989; <b>CODIGO FIJO detectado</b><br>'+ident+'<br><span style="opacity:.85;font-weight:400">'+tec+'<br>'+stats+'</span>'+rssiLine(s.rssi,s.floor)+rfCfgLine(j),'#14532d');
    }else if(s.n===0){
      sniffShow('&#10060; <b>No llego ninguna señal RF.</b><br><span style="opacity:.85;font-weight:400">Revisa: antena del CC1101, o el control no transmite en la frecuencia sintonizada. Usa &#128225; Buscar frecuencia.</span>'+rfCfgLine(j),'#7f1d1d');
    }else if(s.n<20||s.avg<200){
      sniffShow('&#10060; <b>No llego señal util</b> &mdash; solo '+s.n+' pulsos de '+s.avg+'µs promedio.<br>'
        +'<span style="opacity:.85;font-weight:400">Un control real da <b>cientos</b> de pulsos de 300-1000µs. Esto es ruido electrico, no una transmision: '
        +'revisa que el CC1101 responda por SPI, su alimentacion 3.3V y la antena. '+stats+'</span>'+rssiLine(s.rssi,s.floor)+rfCfgLine(j),'#7f1d1d');
    }else{
      sniffShow('&#128260; <b>Llega señal pero NO se decodifica</b> &mdash; probablemente CODIGO RODANTE (no clonable) o protocolo no soportado.<br><span style="opacity:.85;font-weight:400">'+stats+'</span>'+rssiLine(s.rssi,s.floor)+rfCfgLine(j),'#78350f');
    }
  },9000);
}
// Modal de confirmacion elegante (reemplaza confirm()). Retorna Promise<bool>.
function confirmModal(html,yesText,danger){
  return new Promise(res=>{
    document.getElementById('modalTxt').innerHTML=html;
    const yes=document.getElementById('modalYes'),no=document.getElementById('modalNo'),m=document.getElementById('modal');
    yes.textContent=yesText||'Confirmar';
    yes.className=danger?'warn':'';
    const close=v=>{m.classList.remove('show');yes.onclick=null;no.onclick=null;m.onclick=null;res(v);};
    yes.onclick=()=>close(true);
    no.onclick=()=>close(false);
    m.onclick=e=>{if(e.target===m)close(false);};
    m.classList.add('show');
  });
}
// Muestra el overlay de reinicio a pantalla completa con cuenta regresiva.
// Si reconnect=true, intenta recargar la pagina cuando el equipo vuelve (mismo IP,
// util cuando solo cambia el WiFi de casa y el AP sigue arriba).
function showReboot(title,note,reconnect){
  document.getElementById('rebootTitle').innerHTML=title;
  document.getElementById('rebootNote').innerHTML=note;
  document.getElementById('reboot').classList.add('show');
  let left=15;const cEl=document.getElementById('rebootCount');
  const upd=()=>{cEl.textContent=reconnect?('Reconectando en '+left+' s…'):('Reiniciando… espera ~'+left+' s');};
  upd();
  const iv=setInterval(async()=>{
    left--;
    if(left<=0){
      clearInterval(iv);
      if(reconnect){
        cEl.textContent='Intentando reconectar…';
        // sondear hasta que responda y recargar
        const tryReload=async()=>{
          try{const r=await fetch('/api/status',{cache:'no-store'});if(r.ok){location.reload();return;}}catch(e){}
          setTimeout(tryReload,2000);
        };
        tryReload();
      }else{
        cEl.textContent='Si no cargó solo, recarga la página.';
      }
      return;
    }
    upd();
  },1000);
}
async function rebootDevice(){
  if(!confirm('¿Reiniciar el dispositivo?'))return;
  await fetch('/api/reboot');
  showReboot('&#128260; Reiniciando el dispositivo',
    'El Portón se está reiniciando. Esta página se volverá a cargar sola cuando vuelva a estar disponible.',true);
}
async function transmitUser(code,name){
  if(!confirm('Transmitir codigo de "'+name+'" por 15 segundos?'))return;
  await fetch('/api/users/transmit?code='+code);
  loadUsers();
}
async function importCsv(input){
  const file=input.files[0]; if(!file)return;
  const text=await file.text();
  const r=await fetch('/api/users/import',{method:'POST',headers:{'Content-Type':'text/plain'},body:text});
  const j=await r.json();
  input.value='';
  alert('Importacion lista:\n+'+j.added+' nuevos\n'+j.updated+' actualizados\n'+j.skipped+' omitidos');
  loadUsers();
}
async function delUser(code){if(!confirm('Borrar este control?'))return;await fetch('/api/users/delete?code='+code);loadUsers();}
async function clearLogs(){if(!confirm('Borrar TODOS los registros?'))return;await fetch('/api/logs/clear');loadLogs();}
async function saveWifi(){
  const ssid=document.getElementById('ssid').value.trim();
  const pass=document.getElementById('pass').value;
  if(!ssid){alert('Ingresa el SSID');return;}
  const r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,pass})});
  if(!r.ok){alert('Error al guardar');return;}
  // el AP sigue arriba tras el reinicio, asi que se puede reconectar solo
  showReboot('&#128246; WiFi guardado &mdash; reiniciando',
    'Se guardó la red <b>'+ssid+'</b>. El Portón se reinicia para conectarse. Sigues en la red del Portón (AP), así que esta página se recargará sola.',true);
}
async function saveAp(){
  const ssid=document.getElementById('apSsid').value.trim();
  const pass=document.getElementById('apPass').value;
  if(!ssid){alert('Ingresa el SSID del AP');return;}
  if(pass.length>0&&pass.length<8){alert('El password debe tener al menos 8 caracteres (o dejarlo vacio para red abierta)');return;}
  if(!confirm('Se cambiara la red del Porton (AP) y el equipo se reiniciara. ¿Continuar?'))return;
  const r=await fetch('/api/ap',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,pass})});
  if(!r.ok){alert('Error al guardar');return;}
  // cambia el SSID/pass del propio AP -> el navegador se desconecta, no se puede recargar solo
  showReboot('&#128225; Red del Portón cambiada &mdash; reiniciando',
    'Se guardó el nuevo AP <b>'+ssid+'</b> y el equipo se está reiniciando. <b>Reconéctate al WiFi «'+ssid+'»</b> y vuelve a abrir la página.',false);
}
async function factoryReset(){
  if(!confirm('Restablecer la red a valores de fabrica?\n\nEl AP volvera a «Porton_Config» / «porton1234» y se borrara el WiFi de casa.\nLos controles, registros y admins NO se borran.\n\n¿Continuar?'))return;
  const r=await fetch('/api/factory-reset');
  if(!r.ok){alert('Error al restablecer');return;}
  const j=await r.json();
  showReboot('&#9888;&#65039; Restableciendo de fábrica &mdash; reiniciando',
    'La red del Portón se restableció. <b>Reconéctate al WiFi «'+(j.apSsid||'Porton_Config')+'»</b> (contraseña «'+(j.apPass||'porton1234')+'») y vuelve a abrir la página.',false);
}
function otaMsgShow(t){
  const m=document.getElementById('otaMsg');
  m.innerHTML=t;m.style.display='block';
  m.style.background='#f8514922';m.style.color='#f85149';
  document.getElementById('otaBar').style.display='none';
}
function otaUpload(input){
  const file=input.files[0]; if(!file)return;
  input.value='';
  if(!file.name.endsWith('.bin')){otaMsgShow('&#10060; El archivo debe ser un .bin');return;}
  if(!confirm('Actualizar el firmware con «'+file.name+'» ('+Math.round(file.size/1024)+' KB)?\n\nEl Portón se reiniciará al terminar.\nNo cierres esta página ni te desconectes durante la subida.'))return;
  const bar=document.getElementById('otaBar'),fill=document.getElementById('otaFill'),msg=document.getElementById('otaMsg');
  bar.style.display='block';fill.style.width='0%';msg.style.display='none';
  const fd=new FormData();fd.append('firmware',file,file.name);
  const xhr=new XMLHttpRequest();
  xhr.upload.onprogress=e=>{if(e.lengthComputable)fill.style.width=Math.round(e.loaded/e.total*100)+'%';};
  xhr.onload=()=>{
    if(xhr.status===200){
      fill.style.width='100%';
      showReboot('&#128228; Firmware actualizado &mdash; reiniciando',
        'La imagen se escribió correctamente. El Portón está reiniciando con la versión nueva. Esta página se recargará sola (tendrás que iniciar sesión otra vez).',true);
    }else{
      let e='Error '+xhr.status;
      try{e=JSON.parse(xhr.responseText).error||e;}catch(_){}
      otaMsgShow('&#10060; '+e);
    }
  };
  xhr.onerror=()=>otaMsgShow('&#10060; Se perdió la conexión durante la subida. El equipo sigue con la versión anterior.');
  xhr.open('POST','/api/ota');
  xhr.send(fd);
}
async function loadTelegram(){
  const r=await fetch('/api/telegram');if(!chk(r))return;const j=await r.json();
  document.getElementById('tgEnabled').checked=j.enabled||false;
  document.getElementById('tgToken').value=j.token||'';
  document.getElementById('tgChatId').value=j.chatId||'';
  document.getElementById('tgAccess').checked=j.notifyAccess!==false;
  document.getElementById('tgBlocked').checked=j.notifyBlocked!==false;
  document.getElementById('tgUnknown').checked=j.notifyUnknown||false;
  // Estado: sin WiFi de casa Telegram no puede salir, aunque este bien configurado.
  const st=document.getElementById('tgState');
  if(!j.configured){st.textContent='Sin configurar: no se enviara ninguna notificacion.';st.style.background='#1f2937';st.style.color='#9ca3af';}
  else if(!j.staOk){st.textContent='Configurado, pero sin WiFi de casa: las notificaciones no saldran hasta reconectar.';st.style.background='#78350f';st.style.color='#fff';}
  else{st.textContent='Activo: conectado al WiFi de casa, las notificaciones se envian.';st.style.background='#14532d';st.style.color='#fff';}
  st.style.display='';
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
  if(r.ok){alert('Guardado.');loadTelegram();}else alert('Error al guardar');
}
function tgIo(msg,color){const b=document.getElementById('tgIoBox');b.style.display='';b.style.background=color;b.style.color='#fff';b.textContent=msg;}
async function exportTelegram(){
  const r=await fetch('/api/telegram');if(!chk(r))return;const j=await r.json();
  const cfg={enabled:!!j.enabled,token:j.token||'',chatId:j.chatId||'',
    notifyAccess:j.notifyAccess!==false,notifyBlocked:j.notifyBlocked!==false,notifyUnknown:!!j.notifyUnknown};
  const blob=new Blob([JSON.stringify(cfg,null,2)],{type:'application/json'});
  const a=document.createElement('a');a.href=URL.createObjectURL(blob);
  a.download='telegram-config.json';a.click();URL.revokeObjectURL(a.href);
  tgIo('Configuracion exportada.','#14532d');
}
async function importTelegram(input){
  const file=input.files[0];input.value='';
  if(!file)return;
  let cfg;
  try{cfg=JSON.parse(await file.text());}
  catch(e){tgIo('Archivo invalido: no es JSON.','#7f1d1d');return;}
  if(typeof cfg!=='object'||cfg===null||(!('token'in cfg)&&!('chatId'in cfg))){
    tgIo('El archivo no parece una config de Telegram.','#7f1d1d');return;}
  // volcar a los campos (sin guardar todavia)
  document.getElementById('tgEnabled').checked=!!cfg.enabled;
  document.getElementById('tgToken').value=cfg.token||'';
  document.getElementById('tgChatId').value=cfg.chatId||'';
  document.getElementById('tgAccess').checked=cfg.notifyAccess!==false;
  document.getElementById('tgBlocked').checked=cfg.notifyBlocked!==false;
  document.getElementById('tgUnknown').checked=!!cfg.notifyUnknown;
  if(!confirm('Se cargo la configuracion del archivo. ¿Guardarla ahora en el dispositivo?')){
    tgIo('Config cargada en el formulario. Revisa y toca Guardar cuando quieras.','#78350f');return;}
  await saveTelegram();
  tgIo('Configuracion importada y guardada.','#14532d');
}
async function syncNtp(btn){
  btn.disabled=true;const orig=btn.textContent;btn.textContent='Sincronizando...';
  const r=await fetch('/api/ntp/sync',{method:'POST'});
  const j=await r.json();
  btn.disabled=false;btn.textContent=orig;
  if(j.ok){alert('Hora sincronizada con NTP correctamente.');loadRtc();}
  else alert('Error: '+(j.msg||'sin respuesta'));
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
loadStatus();loadLogs();loadRelayPulse();
// Auto-refresco de registros: solo cuando estas viendo la pestaña Registros y la
// ventana esta visible, cada 15s. Evita saturar el enlace WiFi con peticiones de fondo.
setInterval(()=>{if(curTab==='logs'&&!document.hidden)loadLogs();},15000);
</script></body></html>
)rawliteral";

// ================= HANDLERS =================
void reinitRfReceiver(); // forward declaration
void handleRoot() {
  if (!requireAuth()) return;
  // Sin esto el navegador puede seguir mostrando el panel viejo tras actualizar el firmware.
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
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
    doc["staIp"]   = WiFi.localIP().toString();
    doc["ssid"]    = WiFi.SSID();
    doc["rssi"]    = WiFi.RSSI();       // dBm (mas cerca de 0 = mejor)
    doc["channel"] = WiFi.channel();
  } else {
    doc["staIp"] = "";
    doc["ssid"]  = "";
  }
  // mDNS disponible en ambas interfaces (AP y STA)
  if (mdnsHost.length() > 0) doc["mdns"] = mdnsHost + ".local";
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
  out["users"]        = users.as<JsonArray>();
  out["learning"]     = learnName;
  out["cloneWarning"] = learnCloneOf;
  out["provisioning"] = provisionName;
  out["provisionCode"] = provisionCode;
  out["sniffing"]     = rfSniffing;
  out["scanning"]     = rfScanning;
  if (scanResReady) {
    JsonObject sc = out["scanResult"].to<JsonObject>();
    sc["bestMhz"] = scanBestMhz;   // 0 = sin pico claro
    sc["bestN"]    = scanBestN;
    sc["bestRssi"] = scanBestRssi;
    sc["floor"]    = scanFloorRssi;
    sc["rfMhz"]   = rfMhzCfg;
    JsonArray pts = sc["points"].to<JsonArray>();
    for (uint8_t i = 0; i < SCAN_STEPS; i++) {
      JsonObject pt = pts.add<JsonObject>();
      pt["mhz"] = SCAN_MHZ_START + i * SCAN_MHZ_STEP;
      pt["n"]    = scanCount[i];
      pt["rssi"] = scanRssi[i];
    }
  }
  out["rfOk"]         = rfHardwareOk;
  // sintonia actual: se muestra en el recuadro de diagnostico para que una captura
  // de pantalla lleve toda la informacion necesaria sin pedir el log serial
  JsonObject rf = out["rfCfg"].to<JsonObject>();
  rf["mhz"]    = rfMhzCfg;
  rf["rxbw"]   = rfRxBwCfg;
  rf["narrow"] = (rfRxBwCfg < 812);
  rf["tol"]    = RF_RX_TOLERANCE;
  rf["pa"]     = CC1101_PA_DBM;
  rf["txProto"]= RF_TX_PROTOCOL;
  rf["txPulse"]= RF_TX_PULSE_US;
  rf["txBits"] = RF_TX_BITS;
  if (sniffResReady) {
    JsonObject sr = out["sniffResult"].to<JsonObject>();
    sr["n"]       = sniffResN;
    sr["min"]     = sniffResMin;
    sr["max"]     = sniffResMax;
    sr["avg"]     = sniffResAvg;
    sr["decoded"] = sniffDecoded;
    sr["code"]    = sniffDecCode;
    sr["bits"]    = sniffDecBits;
    sr["proto"]   = sniffDecProto;
    sr["pulse"]   = sniffDecPulse;
    sr["rssi"]    = sniffRssiPeak;    // dBm: potencia de la señal recibida
    sr["floor"]   = sniffRssiFloor;   // dBm: ruido de fondo antes de la captura
    // si el codigo decodificado ya esta registrado, adjuntar su nombre y si esta bloqueado
    if (sniffDecoded) {
      for (JsonObject u : users.as<JsonArray>()) {
        if (u["code"].as<unsigned long>() == sniffDecCode) {
          sr["knownName"]    = u["name"].as<String>();
          sr["knownBlocked"] = (u["blocked"] | false);
          break;
        }
      }
    }
    // se mantiene disponible (no se auto-consume) hasta el proximo diagnostico,
    // para que el poll del panel siempre lo alcance aunque otro fetch ocurra antes
  }
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

void handleProvisionStart() {
  if (!requireAuth()) return;
  if (!server.hasArg("name")) { server.send(400, "text/plain", "name requerido"); return; }
  learnName = "";  // cancelar aprendizaje si estaba activo
  provisionName = server.arg("name");
  // Solo se aleatorizan los 20 bits de ID: el boton y el nibble estructural se
  // mantienen para que la trama sea indistinguible de la de un control real.
  uint32_t rfId;
  do { rfId = esp_random() & 0xFFFFFUL; } while (rfId == 0);
  provisionCode = (rfId << 8) | (RF_TX_DATA_NIBBLE << 4) | RF_TX_TAIL_NIBBLE;
  provisionBits = RF_TX_BITS;  // mismo formato que los controles del sitio

  JsonDocument doc; loadUsers(doc);
  JsonObject nu = doc.as<JsonArray>().add<JsonObject>();
  // guardar tambien los bits: el aprendizaje ya lo hacia y el aprovisionamiento no,
  // asi que estos registros dependian del default global al retransmitirlos
  nu["code"] = provisionCode; nu["name"] = provisionName; nu["blocked"] = false;
  nu["bits"] = provisionBits;
  saveUsers(doc);
  rebuildUserCache();

  provisionEndMs    = millis() + PROVISION_DURATION_MS;
  provisionNextTxMs = millis();
  Serial.printf("[PROVISION] %s -> %lu\n", provisionName.c_str(), provisionCode);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleProvisionCancel() {
  if (!requireAuth()) return;
  provisionName    = "";
  provisionCode    = 0;
  pendingRfReset   = true;  // el loop() hara el reinit en su propio contexto
  server.send(200, "application/json", "{\"ok\":true}");
}

// Transmite un codigo YA registrado (para clonar a otro control fisico)
void handleTransmitUser() {
  if (!requireAuth()) return;
  if (!server.hasArg("code")) { server.send(400, "application/json", "{\"error\":\"code requerido\"}"); return; }
  unsigned long reqCode = strtoul(server.arg("code").c_str(), nullptr, 10);
  // buscar en cache
  String foundName = "";
  unsigned int foundBits = 24;
  for (int i = 0; i < userCacheCount; i++) {
    if (userCache[i].code == reqCode) {
      foundName = String(userCache[i].name);
      foundBits = userCache[i].bits > 0 ? userCache[i].bits : RF_TX_BITS;
      break;
    }
  }
  if (foundName.length() == 0) { server.send(404, "application/json", "{\"error\":\"usuario no encontrado\"}"); return; }
  learnName     = "";  // cancelar aprendizaje si estaba activo
  provisionName = foundName;
  provisionCode = reqCode;
  provisionBits = foundBits;
  provisionEndMs    = millis() + PROVISION_DURATION_MS;
  provisionNextTxMs = millis();
  Serial.printf("[TX] retransmitiendo '%s' -> %lu (%u bits)\n", foundName.c_str(), reqCode, foundBits);
  // guardar bits en variable global para el loop
  server.send(200, "application/json", "{\"ok\":true}");
}

// Exporta los controles registrados como CSV (compatible con Excel)
void handleUsersExport() {
  if (!requireAuth()) return;
  JsonDocument doc; loadUsers(doc);
  JsonArray arr = doc.as<JsonArray>();
  String csv = "nombre,codigo,bloqueado,bits\r\n";
  for (JsonObject u : arr) {
    String nombre = u["name"].as<String>();
    nombre.replace("\"", "\"\"");  // escapar comillas dobles
    csv += "\"" + nombre + "\",";
    csv += String(u["code"].as<unsigned long>()) + ",";
    csv += (u["blocked"] | false) ? "si" : "no";
    csv += "," + String(u["bits"] | RF_TX_BITS) + "\r\n";
  }
  server.sendHeader("Content-Disposition", "attachment; filename=\"controles.csv\"");
  server.send(200, "text/csv; charset=utf-8", csv);
}

// Importa controles desde un CSV subido por el usuario
// Formato esperado: nombre,codigo,bloqueado,bits (con o sin cabecera)
void handleUsersImport() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"error\":\"body vacio\"}"); return; }
  String body = server.arg("plain");
  // Excel en locales es-* guarda CSV con ';' en vez de ','. Sin detectarlo, la linea
  // entera cae en el campo 0, fi<2 y se omiten TODAS las filas sin explicar por que.
  // Se elige el separador por mayoria en el texto completo.
  int nComma = 0, nSemi = 0;
  for (unsigned i = 0; i < body.length(); i++) {
    char c = body.charAt(i);
    if (c == ',') nComma++; else if (c == ';') nSemi++;
  }
  const char SEP = (nSemi > nComma) ? ';' : ',';
  // BOM UTF-8 que Excel antepone al guardar como "CSV UTF-8"
  if (body.length() >= 3 && (uint8_t)body.charAt(0) == 0xEF
      && (uint8_t)body.charAt(1) == 0xBB && (uint8_t)body.charAt(2) == 0xBF) body.remove(0, 3);
  JsonDocument doc; loadUsers(doc);
  JsonArray arr = doc.as<JsonArray>();
  int added = 0, updated = 0, skipped = 0;
  int lineStart = 0;
  bool firstLine = true;
  while (lineStart < (int)body.length()) {
    int lineEnd = body.indexOf('\n', lineStart);
    if (lineEnd < 0) lineEnd = body.length();
    String line = body.substring(lineStart, lineEnd);
    line.trim();
    lineStart = lineEnd + 1;
    if (line.length() == 0) continue;
    // Saltar la cabecera SOLO si de verdad lo es. Antes se descartaba cualquier
    // primera linea que no empezara con digito o comilla, asi que un CSV hecho a
    // mano con el nombre sin comillas ("Casa 1,123,no,28") perdia su primera fila
    // en silencio.
    if (firstLine) {
      firstLine = false;
      String lower = line; lower.toLowerCase();
      if (lower.startsWith("nombre") || lower.startsWith("name") || lower.startsWith("\"nombre")) continue;
    }
    // parsear campos separados por coma respetando comillas
    String fields[4]; int fi = 0;
    bool inQ = false; String cur = "";
    for (int i = 0; i <= (int)line.length() && fi < 4; i++) {
      char c = i < (int)line.length() ? line.charAt(i) : SEP;
      if (c == '"') {
        // "" dentro de un campo entrecomillado = una comilla literal (CSV estandar).
        // El parser anterior alternaba en cada comilla y se las comia: un nombre
        // exportado como "Casa ""A""" volvia como Casa A.
        if (inQ && i + 1 < (int)line.length() && line.charAt(i + 1) == '"') { cur += '"'; i++; }
        else inQ = !inQ;
        continue;
      }
      if (c == SEP && !inQ) { fields[fi++] = cur; cur = ""; }
      else cur += c;
    }
    if (fi < 2) { skipped++; continue; }  // minimo nombre y codigo
    String nombre = fields[0]; nombre.trim();
    unsigned long codigo = strtoul(fields[1].c_str(), nullptr, 10);
    String bl = fields[2]; bl.trim(); bl.toLowerCase();
    bool bloqueado = (bl == "si" || bl == "sí" || bl == "1" || bl == "true" || bl == "yes");
    fields[2].trim(); fields[3].trim();
    unsigned int bits = fields[3].length() > 0 ? (unsigned int)fields[3].toInt() : RF_TX_BITS;
    if (bits < 8 || bits > 32) bits = RF_TX_BITS;   // columna corrupta: no guardar basura
    if (codigo == 0 || nombre.length() == 0 || nombre.length() > 20) { skipped++; continue; }
    // buscar si ya existe
    bool found = false;
    for (JsonObject u : arr) {
      if (u["code"].as<unsigned long>() == codigo) {
        u["name"]    = nombre;
        u["blocked"] = bloqueado;
        u["bits"]    = bits;
        found = true; updated++; break;
      }
    }
    if (!found) {
      JsonObject nu = arr.add<JsonObject>();
      nu["code"] = codigo; nu["name"] = nombre; nu["blocked"] = bloqueado; nu["bits"] = bits;
      added++;
    }
  }
  saveUsers(doc);
  rebuildUserCache();
  Serial.printf("[IMPORT] +%d nuevos, %d actualizados, %d omitidos\n", added, updated, skipped);
  String resp = "{\"ok\":true,\"added\":" + String(added) + ",\"updated\":" + String(updated) + ",\"skipped\":" + String(skipped) + "}";
  server.send(200, "application/json", resp);
}

// ISR del sniffer: registra la duracion de cada pulso (flanco a flanco) en GDO0.
void IRAM_ATTR sniffIsr() {
  uint32_t now = micros();
  uint32_t d   = now - sniffLastUs;
  sniffLastUs  = now;
  if (d > 40 && d < 30000) {                          // ignora ruido y silencios largos
    sniffPulses++;                                    // sin tope: lo usa el barrido
    if (sniffIdx < SNIFF_BUF) sniffDur[sniffIdx++] = (uint16_t)d;
  }
}

// Inicia el diagnostico. Fase 1 = captura cruda (mide si llega señal).
void startRfSniff() {
  mySwitch.disableReceive();
  sniffIdx     = 0;
  sniffLastUs  = micros();
  sniffDecoded = false;
  sniffDecCode = 0;
  sniffDecBits = 0;
  sniffDecProto = 0;
  sniffDecPulse = 0;
  sniffResReady = false;   // invalida el resultado anterior hasta que termine este
  // Fondo de ruido ANTES de pedir que aprieten: da la referencia contra la cual el
  // pico significa algo. Un pico 30 dB sobre el fondo es señal sobrada; 3 dB, nada.
  sniffRssiPeak  = -128;
  int32_t acc = 0;
  for (uint8_t i = 0; i < 8; i++) { acc += ELECHOUSE_cc1101.getRssi(); delay(2); }
  sniffRssiFloor = (int8_t)(acc / 8);
  attachInterrupt(digitalPinToInterrupt(RF_RX_PIN), sniffIsr, CHANGE);
  rfSniffing   = true;
  sniffPhase   = 1;
  rfSniffEndMs = millis() + SNIFF_PHASE_MS;
  Serial.println("[SNIFF] FASE 1/2 (señal): APRIETA Y MANTEN EL CONTROL AHORA...");
}

// Muestrea el RSSI durante la fase 1. Se llama en cada vuelta del loop: el pico es
// lo que importa, porque el control emite a rafagas y el promedio lo diluiria.
void pollRfSniffRssi() {
  int r = ELECHOUSE_cc1101.getRssi();
  if (r > sniffRssiPeak) sniffRssiPeak = (int8_t)r;
}

// Transicion de fase 1 (crudo) a fase 2 (decodificacion con RCSwitch).
void startRfSniffDecodePhase() {
  detachInterrupt(digitalPinToInterrupt(RF_RX_PIN));
  mySwitch.resetAvailable();
  lastCode = 0; lastDetectionMs = 0;
  mySwitch.enableReceive(digitalPinToInterrupt(RF_RX_PIN));
  sniffPhase   = 2;
  rfSniffEndMs = millis() + SNIFF_PHASE_MS;
  Serial.println("[SNIFF] FASE 2/2 (decodificar): SIGUE APRETANDO EL CONTROL...");
}

// Se llama en cada loop durante la fase 2: si RCSwitch decodifica, lo guarda.
void pollRfSniffDecode() {
  if (mySwitch.available()) {
    unsigned long v = mySwitch.getReceivedValue();
    if (v != 0 && !sniffDecoded) {
      sniffDecoded  = true;
      sniffDecCode  = v;
      sniffDecBits  = mySwitch.getReceivedBitlength();
      sniffDecProto = mySwitch.getReceivedProtocol();
      sniffDecPulse = mySwitch.getReceivedDelay();
      Serial.printf("[SNIFF] DECODIFICADO codigo=%lu bits=%u protocolo=%u pulso=%uµs -> CODIGO FIJO (clonable)\n",
                    v, sniffDecBits, sniffDecProto, sniffDecPulse);
    }
    mySwitch.resetAvailable();
  }
}

// Termina la captura, imprime el resultado por Serial y restaura RCSwitch.
void finishRfSniff() {
  rfSniffing = false;
  sniffPhase = 0;
  uint16_t n = sniffIdx;
  Serial.printf("[SNIFF] pulsos capturados (fase 1): %u\n", n);
  uint16_t mn = 0, mx = 0; uint32_t avg = 0;
  if (n > 0) {
    mn = 0xFFFF;
    uint32_t sum = 0;
    for (uint16_t i = 0; i < n; i++) {
      uint16_t d = sniffDur[i];
      if (d < mn) mn = d;
      if (d > mx) mx = d;
      sum += d;
    }
    avg = sum / n;
    Serial.printf("[SNIFF] pulso min=%uµs max=%uµs prom=%luµs\n", mn, mx, (unsigned long)avg);
  }
  // interpretacion combinando fase 1 (señal) y fase 2 (decodificacion)
  if (sniffDecoded) {
    Serial.printf("[SNIFF] RESULTADO: CODIGO FIJO clonable (codigo=%lu, %u bits)\n",
                  (unsigned long)sniffDecCode, sniffDecBits);
  } else if (n == 0) {
    Serial.println("[SNIFF] RESULTADO: NO llego señal (cero pulsos). Revisa la radio, la antena o si el control transmite en 433.92 MHz.");
  } else if (n < SNIFF_MIN_REAL_PULSES || avg < SNIFF_MIN_REAL_PULSE_US) {
    Serial.printf("[SNIFF] RESULTADO: NO llego señal util — solo %u pulsos de %luµs promedio. Un control real da\n", n, (unsigned long)avg);
    Serial.println("[SNIFF] cientos de pulsos de 300-1000µs. Esto es RUIDO electrico, no una transmision:");
    Serial.println("[SNIFF] revisa que el CC1101 responda por SPI, su alimentacion 3.3V y la antena.");
  } else {
    Serial.println("[SNIFF] RESULTADO: LLEGA señal pero no se decodifica -> codigo RODANTE (no clonable) o protocolo no soportado.");
  }
  // exponer resultado a la GUI
  sniffResN = n; sniffResMin = mn; sniffResMax = mx; sniffResAvg = avg;
  sniffResReady = true;
  // RCSwitch ya quedo habilitado en la fase 2; solo limpiar estado
  mySwitch.resetAvailable();
  lastCode = 0; lastDetectionMs = 0;
  Serial.println("[SNIFF] receptor RCSwitch restaurado");
}

// ---- barrido de frecuencia ----
// Programa un paso del barrido y arranca su ventana de conteo.
void scanStepBegin(uint8_t i) {
  ELECHOUSE_cc1101.SpiStrobe(0x36);   // SIDLE antes de retocar la sintonia
  ELECHOUSE_cc1101.setMHZ(SCAN_MHZ_START + i * SCAN_MHZ_STEP);
  ELECHOUSE_cc1101.SetRx();
  delayMicroseconds(800);             // asentamiento del PLL
  sniffPulses = 0;
  sniffLastUs = micros();
  sniffRssiPeak = -128;
  attachInterrupt(digitalPinToInterrupt(RF_RX_PIN), sniffIsr, CHANGE);
  scanEndMs = millis() + SCAN_STEP_MS;
}

void startRfScan() {
  mySwitch.disableReceive();
  for (uint8_t i = 0; i < SCAN_STEPS; i++) { scanCount[i] = 0; scanRssi[i] = -128; }
  scanResReady = false;
  scanBestMhz  = 0;
  scanBestN    = 0;
  scanIdx      = 0;
  scanPass     = 0;
  rfScanning   = true;
  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.setRxBW(SCAN_RXBW_KHZ);
  Serial.printf("[SCAN] barriendo %.2f-%.2f MHz, %d pasadas — MANTEN APRETADO EL CONTROL %d s...\n",
                SCAN_MHZ_START, SCAN_MHZ_START + (SCAN_STEPS - 1) * SCAN_MHZ_STEP,
                SCAN_PASSES, (SCAN_STEPS * SCAN_STEP_MS * SCAN_PASSES) / 1000);
  scanStepBegin(0);
}

// Cierra el paso actual y pasa al siguiente; al terminar imprime el perfil.
void pollRfScan() {
  pollRfSniffRssi();                       // acumula el pico de potencia de este paso
  if (millis() < scanEndMs) return;
  detachInterrupt(digitalPinToInterrupt(RF_RX_PIN));
  uint32_t n = sniffPulses;
  uint32_t acc = scanCount[scanIdx] + n;
  scanCount[scanIdx] = (acc > 65535) ? 65535 : (uint16_t)acc;
  if (sniffRssiPeak > scanRssi[scanIdx]) scanRssi[scanIdx] = sniffRssiPeak;
  scanIdx++;
  if (scanIdx < SCAN_STEPS) { scanStepBegin(scanIdx); return; }
  scanPass++;
  if (scanPass < SCAN_PASSES) {
    Serial.printf("[SCAN] pasada %u/%u completa, siguiendo...\n", scanPass, SCAN_PASSES);
    scanIdx = 0; scanStepBegin(0); return;
  }

  rfScanning = false;
  // El pico se decide por RSSI, no por conteo de flancos: el RSSI mide POTENCIA y no
  // depende de si la ventana de ese paso cayo sobre una rafaga o sobre un silencio,
  // que era lo que hacia irrepetible el perfil por conteo.
  int8_t bestR = -128;
  for (uint8_t i = 0; i < SCAN_STEPS; i++) {
    if (scanRssi[i] > bestR) { bestR = scanRssi[i]; scanBestMhz = SCAN_MHZ_START + i * SCAN_MHZ_STEP; }
  }
  uint16_t best = 0;
  for (uint8_t i = 0; i < SCAN_STEPS; i++) if (scanCount[i] > best) best = scanCount[i];
  scanBestN = best;
  scanBestRssi = bestR;
  Serial.println("[SCAN] perfil (RSSI dBm y pulsos por frecuencia):");
  for (uint8_t i = 0; i < SCAN_STEPS; i++) {
    float f = SCAN_MHZ_START + i * SCAN_MHZ_STEP;
    int bar = (best > 0) ? (scanCount[i] * 40) / best : 0;
    Serial.printf("[SCAN] %7.2f MHz  %4d dBm  %5u ", f, scanRssi[i], scanCount[i]);
    for (int b = 0; b < bar; b++) Serial.print('#');
    Serial.println(f == scanBestMhz ? "  <== PICO" : "");
  }
  // Fondo estimado con la MEDIANA, no con la media. Con la media, una señal ancha
  // (que ocupa muchos pasos, como la de estos controles: ~433.9-434.5) sube tanto el
  // promedio que su propio pico no llega a 3x y el barrido concluia "sin pico claro"
  // teniendo la transmision delante. La mediana no se deja arrastrar por el pico.
  // Fondo por MEDIANA del RSSI: un pico arrastra la media, no la mediana. El criterio
  // es en dB (potencia), no en conteo de flancos, que era lo irrepetible.
  int8_t sorted[SCAN_STEPS];
  memcpy(sorted, scanRssi, sizeof(sorted));
  for (uint8_t i = 1; i < SCAN_STEPS; i++) {          // insercion: 37 elementos
    int8_t v = sorted[i]; int8_t j = i - 1;
    while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
    sorted[j + 1] = v;
  }
  scanFloorRssi = sorted[SCAN_STEPS / 2];
  int margen = bestR - scanFloorRssi;
  if (margen < 6) {
    Serial.printf("[SCAN] RESULTADO: sin pico claro (pico %d dBm, fondo %d dBm, solo %d dB de margen).\n",
                  bestR, scanFloorRssi, margen);
    Serial.println("[SCAN] O no se apreto el control durante todo el barrido, o transmite fuera de");
    Serial.println("[SCAN] 433.0-434.8 MHz, o no llega señal a la antena.");
    scanBestMhz = 0;
  } else {
    Serial.printf("[SCAN] RESULTADO: el control transmite en ~%.2f MHz (%d dBm, fondo %d dBm, +%d dB).\n",
                  scanBestMhz, bestR, scanFloorRssi, margen);
    Serial.printf("[SCAN] Si difiere de la sintonia actual (%.2f), ajustala desde el panel.\n", rfMhzCfg);
  }
  scanResReady = true;
  reinitRfReceiver();   // restaura sintonia de operacion (RF_MHZ + BW configurada)
}

// Reinicia completamente el CC1101 y habilita el receptor RF
void reinitRfReceiver() {
  if (!rfHardwareOk) { Serial.println("[RF] reinit omitido: la radio no respondio al arrancar"); return; }
  mySwitch.disableReceive();
  mySwitch.disableTransmit();
  pinMode(RF_RX_PIN, INPUT);   // CRITICO: disableTransmit() deja el pin como OUTPUT
  provisionTxReady = false;
  // Llevar CC1101 a IDLE antes de reconfigurar (SetTx puede haber alterado PKTCTRL0/IOCFG0)
  ELECHOUSE_cc1101.SpiStrobe(0x36); // SIDLE
  delay(5);
  // Reconfigurar exactamente igual que en setup() pero sin Init()
  // (Init() tarda demasiado y reinicia el ESP; los registros base persisten)
  applyRfRadioConfig();
  ELECHOUSE_cc1101.SetRx();
  delay(5);
  mySwitch.resetAvailable();
  lastCode        = 0;
  lastDetectionMs = 0;
  mySwitch.enableReceive(digitalPinToInterrupt(RF_RX_PIN));
  mySwitch.setReceiveTolerance(RF_RX_TOLERANCE);
  Serial.println("[RF] CC1101 en RX (OOK 433.92MHz)");
}

// Inicia el diagnostico/sniffer RF (8s). Ver resultado en el Monitor Serie.
void handleRfSniff() {
  if (!requireAuth()) return;
  if (!rfHardwareOk) {
    // sin radio, GPIO27 queda flotando y el sniffer contaria ruido como si fuera
    // señal ("llegan pocos pulsos"), mandando a revisar la antena sin motivo
    server.send(409, "application/json",
      "{\"error\":\"La radio CC1101 no responde. Revisa VDD=3.3V, GND y el cableado SPI (SCK18/MISO19/MOSI23/CSN5). Sin radio no hay recepcion ni diagnostico.\"}");
    return;
  }
  learnName     = "";
  provisionName = "";
  provisionCode = 0;
  pendingSniff  = true;  // el loop() arranca la captura en su contexto
  server.send(200, "application/json", "{\"ok\":true}");
}

// Barrido de frecuencia: mide en que frecuencia transmite REALMENTE un control.
void handleRfScan() {
  if (!requireAuth()) return;
  if (!rfHardwareOk) {
    server.send(409, "application/json",
      "{\"error\":\"La radio CC1101 no responde. Revisa VDD=3.3V, GND y el cableado SPI (SCK18/MISO19/MOSI23/CSN5).\"}");
    return;
  }
  learnName     = "";
  provisionName = "";
  provisionCode = 0;
  pendingScan   = true;   // el loop() arranca el barrido en su contexto
  server.send(200, "application/json",
    "{\"ok\":true,\"ms\":" + String(SCAN_STEPS * SCAN_STEP_MS * SCAN_PASSES) + "}");
}

// Ajuste en caliente de la sintonia (frecuencia + ancho de banda RX).
void handleRfTuneSet() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"error\":\"body vacio\"}"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    server.send(400, "application/json", "{\"error\":\"json invalido\"}"); return;
  }
  float    m  = doc["mhz"]  | rfMhzCfg;
  uint16_t bw = doc["rxbw"] | rfRxBwCfg;
  if (m < 430.0 || m > 436.0) {
    server.send(400, "application/json", "{\"error\":\"frecuencia fuera de 430-436 MHz\"}"); return;
  }
  if (!rfBwValid(bw)) {
    server.send(400, "application/json", "{\"error\":\"ancho de banda no soportado por el CC1101\"}"); return;
  }
  rfMhzCfg = m; rfRxBwCfg = bw;
  saveRfConfig();
  Serial.printf("[RF] nueva sintonia: %.2f MHz  BW %u kHz\n", rfMhzCfg, rfRxBwCfg);
  pendingRfReset = true;   // el loop() reprograma la radio con los valores nuevos
  server.send(200, "application/json", "{\"ok\":true}");
}

// Detiene cualquier transmision y reinicia el receptor RF
void handleRfReset() {
  if (!requireAuth()) return;
  provisionName  = "";
  provisionCode  = 0;
  learnName      = "";
  pendingRfReset = true;  // el loop() hara el reinit
  Serial.println("[RF] reset solicitado");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleReboot() {
  if (!requireAuth()) return;
  server.send(200, "application/json", "{\"ok\":true}");
  delay(200);
  ESP.restart();
}

// Restablece la red del Porton a los valores de fabrica (borra ap.json y wifi.json)
// y reinicia. Equivale al boton fisico BOOT de 5s. Controles/logs/admins se conservan.
void handleFactoryReset() {
  if (!requireAuth()) return;
  JsonDocument res; res["ok"] = true;
  res["apSsid"] = DEFAULT_AP_SSID; res["apPass"] = DEFAULT_AP_PASS;
  String s; serializeJson(res, s);
  server.send(200, "application/json", s);
  delay(200);
  factoryResetAp();  // borra ap.json + wifi.json y reinicia
}

void handleProvisionRetransmit() {
  if (!requireAuth()) return;
  if (provisionName.length() == 0 || provisionCode == 0) {
    server.send(400, "application/json", "{\"error\":\"no hay provision activa\"}"); return;
  }
  // reinicia el timer sin cambiar el codigo — el control puede intentar grabarlo de nuevo
  provisionEndMs    = millis() + PROVISION_DURATION_MS;
  provisionNextTxMs = millis();
  Serial.printf("[PROVISION] retransmitiendo %s -> %lu\n", provisionName.c_str(), provisionCode);
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
  rebuildUserCache();
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
  // Estado real para que la UI no ofrezca notificaciones que no pueden salir.
  doc["staOk"]      = (WiFi.status() == WL_CONNECTED);
  doc["configured"] = telegramConfigured();
  String s; serializeJson(doc, s);
  server.send(200, "application/json", s);
}

void handleTelegramSet() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "body requerido"); return; }
  JsonDocument doc; deserializeJson(doc, server.arg("plain"));
  saveTelegramConfig(doc);
  refreshTelegramCache();   // la copia en RAM debe reflejar lo recien guardado
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
  rebuildUserCache();
  server.send(200, "application/json", "{\"ok\":true}");
}

// Activa el rele manualmente desde la GUI (apertura remota), respetando relayPulseMs.
void handleRelayActivate() {
  if (!requireAuth()) return;
  digitalWrite(RELAY_PIN, HIGH);
  relayEndMs = millis() + relayPulseMs;
  Serial.printf("[RELAY] activado manualmente desde GUI (%u ms)\n", relayPulseMs);
  sendTelegram("notifyAccess", "Apertura manual (GUI) - " + currentTimestamp());
  JsonDocument res; res["ok"] = true; res["pulseMs"] = relayPulseMs;
  String s; serializeJson(res, s);
  server.send(200, "application/json", s);
}

// Bloquea/desbloquea en masa todos los controles cuyo nombre EMPIEZA con un prefijo.
// GET /api/users/toggle-prefix?prefix=Casa 13&block=1  (block=1 bloquea, block=0 desbloquea)
void handleUserTogglePrefix() {
  if (!requireAuth()) return;
  if (!server.hasArg("prefix")) { server.send(400, "application/json", "{\"error\":\"prefix requerido\"}"); return; }
  String prefix = server.arg("prefix");
  prefix.trim();
  if (prefix.length() == 0) { server.send(400, "application/json", "{\"error\":\"prefix vacio\"}"); return; }
  bool block = (server.arg("block") == "1");
  String pl = prefix; pl.toLowerCase();
  int affected = 0;
  JsonDocument doc; loadUsers(doc);
  for (JsonObject u : doc.as<JsonArray>()) {
    String name = u["name"].as<String>();
    String nl = name; nl.toLowerCase();
    // Coincide si el nombre es igual al prefijo, o empieza con el prefijo
    // seguido de un separador (no un caracter alfanumerico). Asi "Casa 13"
    // afecta "Casa 13 # 1" pero NO "Casa 130".
    bool match = false;
    if (nl == pl) match = true;
    else if (nl.startsWith(pl) && nl.length() > pl.length()) {
      char next = nl.charAt(pl.length());
      bool alnum = (next >= 'a' && next <= 'z') || (next >= '0' && next <= '9');
      match = !alnum;
    }
    if (match) {
      u["blocked"] = block;
      affected++;
    }
  }
  if (affected > 0) { saveUsers(doc); rebuildUserCache(); }
  Serial.printf("[USERS] toggle-prefix '%s' block=%d -> %d afectados\n", prefix.c_str(), block, affected);
  JsonDocument res; res["ok"] = true; res["affected"] = affected;
  String s; serializeJson(res, s);
  server.send(200, "application/json", s);
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

void handleRelayConfigGet() {
  if (!requireAuth()) return;
  JsonDocument doc;
  doc["pulseMs"]    = relayPulseMs;
  doc["logUnknown"] = logUnknown;
  String s; serializeJson(doc, s);
  server.send(200, "application/json", s);
}

void handleRelayConfigSet() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "body requerido"); return; }
  JsonDocument doc; deserializeJson(doc, server.arg("plain"));
  uint32_t ms = doc["pulseMs"] | 0;
  if (ms < 100 || ms > 30000) {
    server.send(400, "application/json", "{\"error\":\"pulseMs debe estar entre 100 y 30000\"}"); return;
  }
  bool logUnk = doc["logUnknown"] | true;
  relayPulseMs = ms;
  logUnknown   = logUnk;
  saveRelayConfig(ms, logUnk);
  Serial.printf("[RELAY] pulso: %u ms  logUnknown: %s\n", ms, logUnk ? "si" : "no");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleNtpSync() {
  if (!requireAuth()) return;
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "application/json", "{\"ok\":false,\"msg\":\"sin WiFi de casa\"}");
    return;
  }
  String posix; loadTzConfig(posix);
  configTzTime(posix.c_str(), NTP_SERVER);
  struct tm tm;
  bool ok = getLocalTime(&tm, 5000);
  if (ok) {
    timeOk = true;
    syncRtcFromSystem();
    Serial.printf("[NTP] sincronizado manualmente (TZ: %s)\n", posix.c_str());
  } else {
    Serial.println("[NTP] sin respuesta del servidor");
  }
  server.send(ok ? 200 : 503, "application/json",
              ok ? "{\"ok\":true}" : "{\"ok\":false,\"msg\":\"sin respuesta del servidor NTP\"}");
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
  rebuildUserCache();
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

// ---- Portal cautivo ----
// true si el "Host:" pedido somos realmente nosotros (IP del AP, IP de casa o nombre mDNS).
// Sirve para no redirigir un 404 legitimo del propio panel.
bool hostIsSelf(const String& h) {
  if (h.length() == 0) return true;          // peticion sin Host (HTTP/1.0): tratar como propia
  String host = h;
  int c = host.indexOf(':');
  if (c >= 0) host = host.substring(0, c);   // quitar el puerto
  host.toLowerCase();
  if (host == WiFi.softAPIP().toString()) return true;
  if (WiFi.status() == WL_CONNECTED && host == WiFi.localIP().toString()) return true;
  if (mdnsHost.length() > 0 && (host == mdnsHost || host == mdnsHost + ".local")) return true;
  return false;
}

// Cualquier peticion a un dominio externo que caiga aqui es una sonda de portal cautivo:
// Android /generate_204, iOS /hotspot-detect.html, Windows /connecttest.txt. Respondemos
// 302 al dashboard -> el sistema muestra "iniciar sesion en la red" y abre el panel.
void handleNotFound() {
  // OJO: no imprimir "404" antes de decidir. Una peticion a otro host se REDIRIGE
  // (302, portal cautivo), y el log anterior la reportaba como 404: al depurar el
  // portal parecia que no estaba redirigiendo cuando si lo hacia.
  if (hostIsSelf(server.hostHeader())) {
    Serial.printf("[HTTP] 404 %s%s\n", server.hostHeader().c_str(), server.uri().c_str());
    server.send(404, "text/plain", "404: no encontrado");
    return;
  }
  Serial.printf("[HTTP] 302 portal: %s%s\n", server.hostHeader().c_str(), server.uri().c_str());
  // Redirigir a la IP de la interfaz por la que entro la peticion (AP o WiFi de casa).
  IPAddress via = server.client().localIP();
  String ip = (via == IPAddress((uint32_t)0)) ? WiFi.softAPIP().toString() : via.toString();
  server.sendHeader("Location", "http://" + ip + "/", true);
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(302, "text/plain", "");
}

// ---- Actualizacion OTA (POST /api/ota, multipart con el firmware.bin) ----
// El esquema de particiones por defecto de esp32dev ya reserva ota_0/ota_1, asi que
// la imagen nueva se escribe en la particion inactiva; si la subida falla a medias el
// bootloader sigue arrancando la anterior (no se ladrilla el equipo).
static String otaError = "";
static bool   otaStarted = false;

void handleOtaUpload() {
  HTTPUpload &up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    otaError = ""; otaStarted = false;
    // Auth silenciosa: requireAuth() responderia 302 en medio del multipart.
    String token = extractCookie(server.header("Cookie"), "session");
    if (token.length() == 0 || !isValidSession(token)) { otaError = "no autorizado"; return; }
    // No actualizar con el porton ocupado: un reinicio a media operacion deja el rele
    // colgado o pierde un aprendizaje/provision en curso.
    if (learnName.length() > 0 || provisionName.length() > 0 || rfSniffing || rfScanning || relayEndMs > 0) {
      otaError = "el porton esta ocupado (aprendizaje, provision, diagnostico o rele activo)";
      return;
    }
    // Escribir en flash detiene la ejecucion del codigo que no vive en IRAM: la ISR de
    // RCSwitch podria dispararse en ese momento y colgar el equipo. Se apaga la recepcion
    // durante la actualizacion (se restaura sola al reiniciar).
    mySwitch.disableReceive();
    digitalWrite(RELAY_PIN, LOW);
    relayEndMs = 0;

    Serial.printf("[OTA] inicio: %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      otaError = "no se pudo iniciar la actualizacion";
      Serial.printf("[OTA] Update.begin fallo: %s\n", Update.errorString());
      return;
    }
    otaStarted = true;

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!otaStarted) return;
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      otaError = String("error de escritura: ") + Update.errorString();
      Update.abort(); otaStarted = false;
    }

  } else if (up.status == UPLOAD_FILE_END) {
    if (!otaStarted) return;
    if (!Update.end(true)) {          // true = la imagen debe estar completa
      otaError = String("imagen invalida o incompleta: ") + Update.errorString();
      otaStarted = false;
      return;
    }
    Serial.printf("[OTA] OK, %u bytes escritos\n", (unsigned)up.totalSize);

  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (otaStarted) { Update.abort(); otaStarted = false; }
    otaError = "subida cancelada";
  }
}

// Se ejecuta cuando el multipart ya termino: responde y (si todo fue bien) reinicia.
void handleOtaFinish() {
  if (otaError.length() > 0) {
    Serial.printf("[OTA] fallo: %s\n", otaError.c_str());
    JsonDocument doc; doc["error"] = otaError;
    String out; serializeJson(doc, out);
    // 401 si fue por sesion, 409 si el porton estaba ocupado, 400 en el resto
    int code = otaError.startsWith("no autorizado") ? 401
             : otaError.startsWith("el porton esta ocupado") ? 409 : 400;
    otaError = "";
    if (!otaStarted) reinitRfReceiver();   // devolver el RF a su estado normal
    server.send(code, "application/json", out);
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
  delay(300);            // dar tiempo a que salga la respuesta antes de reiniciar
  ESP.restart();
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
  server.on("/api/relay/activate",     handleRelayActivate);
  server.on("/api/users/toggle-prefix", handleUserTogglePrefix);
  server.on("/api/users/rename",  HTTP_POST, handleUserRename);
  server.on("/api/learn/start",        handleLearnStart);
  server.on("/api/learn/cancel",       handleLearnCancel);
  server.on("/api/provision/start",       handleProvisionStart);
  server.on("/api/provision/cancel",      handleProvisionCancel);
  server.on("/api/provision/retransmit",  handleProvisionRetransmit);
  server.on("/api/rf/reset",              handleRfReset);
  server.on("/api/rf/sniff",              handleRfSniff);
  server.on("/api/rf/scan",               handleRfScan);
  server.on("/api/rf/tune",   HTTP_POST,  handleRfTuneSet);
  server.on("/api/reboot",                handleReboot);
  server.on("/api/factory-reset",         handleFactoryReset);
  server.on("/api/users/transmit",        handleTransmitUser);
  server.on("/users.csv",           HTTP_GET,  handleUsersExport);
  server.on("/api/users/import",    HTTP_POST, handleUsersImport);
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
  server.on("/api/ntp/sync",              HTTP_POST, handleNtpSync);
  server.on("/api/relay/config",          HTTP_GET,  handleRelayConfigGet);
  server.on("/api/relay/config",          HTTP_POST, handleRelayConfigSet);
  server.on("/api/ota", HTTP_POST, handleOtaFinish, handleOtaUpload);
  // El navegador pide /favicon.ico en cada carga; sin ruta propia cae en el 404 del core
  // y ensucia el log. Responder 204 (sin contenido) lo silencia.
  server.on("/favicon.ico", HTTP_GET, [](){ server.send(204); });
  server.onNotFound(handleNotFound);   // portal cautivo + 404 normal
  server.begin();
  Serial.println("[WEB] servidor iniciado");
}

// ================= SETUP / LOOP =================
void setup() {
  // WORKAROUND: desactiva el brownout detector para que no reinicie en bucle
  // cuando el pico de corriente del WiFi hace caer el voltaje (cable/fuente USB debil).
  // Lo ideal sigue siendo alimentar con 5V/2A + capacitor; esto solo evita el boot-loop.
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Porton 433MHz v2 ===");

  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  if (!LittleFS.begin(true)) Serial.println("[FS] error montando LittleFS");

  bootstrapFirstAdmin();
  applyTimezone();
  loadRelayConfig();
  loadRfConfig();

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

  // La radio y el rele se levantan ANTES de la red: nada en la ruta RF depende
  // del WiFi, y con esto el porton abre desde el primer segundo del arranque.
  // Antes iba despues de setupWiFi(), que bloquea hasta 15 s intentando el STA
  // (y 5 s mas en NTP): en un equipo recien flasheado, o con el WiFi de casa
  // ausente, el receptor quedaba sordo toda esa ventana.
  // Inicializar CC1101
  ELECHOUSE_cc1101.setSpiPin(18, 19, 23, CC1101_CS_PIN);  // SCK, MISO, MOSI, CSN
  rfHardwareOk = probeCc1101();
  if (!rfHardwareOk) {
    Serial.println("[RF] *** CC1101 NO RESPONDE *** revisa VDD=3.3V (NO 5V), GND, SCK18/MISO19/MOSI23/CSN5");
    Serial.println("[RF] el equipo sigue arrancando SIN radio: el panel web funciona, pero no habra");
    Serial.println("[RF] recepcion de controles ni clonado hasta resolver el modulo.");
  } else {
    ELECHOUSE_cc1101.Init();
    if (ELECHOUSE_cc1101.getCC1101()) {
      Serial.println("[RF] CC1101 detectado por SPI (OK)");
    } else {
      Serial.println("[RF] CC1101 respondio a MISO pero no se identifica — revisa MOSI/SCK");
    }
    applyRfRadioConfig();            // frecuencia + OOK + ancho de banda RX + potencia TX
    Serial.printf("[RF] sintonia: %.2f MHz  OOK  RxBW=%s kHz  tol=%d%%  PA=%+d dBm\n",
                  rfMhzCfg, String(rfRxBwCfg).c_str(),
                  RF_RX_TOLERANCE, CC1101_PA_DBM);
    ELECHOUSE_cc1101.SetRx();        // modo recepcion
    mySwitch.enableReceive(digitalPinToInterrupt(RF_RX_PIN));
    mySwitch.setReceiveTolerance(RF_RX_TOLERANCE);
    // NO llamar enableTransmit en setup: se llama por burst usando GDO0 (RF_RX_PIN)
    mySwitch.setRepeatTransmit(20);          // 20 repeticiones por burst (default=10)
    mySwitch.setProtocol(RF_TX_PROTOCOL);    // HT6P20B: el de los controles del sitio
    mySwitch.setPulseLength(RF_TX_PULSE_US); // 490us medidos (el nominal del proto 6 es 450)
    Serial.println("[RF] CC1101 listo — OOK 433.92 MHz (GDO0=GPIO" + String(RF_RX_PIN) + ", GDO2=GPIO" + String(RF_TX_PIN) + ")");
  }
  Serial.println("[BTN] mantene BOOT por 5s para factory reset del AP");

  rebuildUserCache();

  setupWiFi();
  setupWebServer();


  tgQueue = xQueueCreate(5, sizeof(TgMsg));
  xTaskCreatePinnedToCore(telegramTask, "TG", 8192, nullptr, 1, nullptr, 0);
  refreshTelegramCache();
  Serial.printf("[TG] tarea Telegram iniciada en core 0 (%s)\n",
                telegramConfigured() ? "configurado" : "sin configurar: no se enviara nada");
}

void loop() {
  dnsServer.processNextRequest();  // no bloqueante: solo atiende si hay una consulta pendiente
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

  // reconexion automatica del WiFi de casa: si se cayo, reintentar sin bloquear el loop
  if (staSsidSaved.length() > 0 && millis() - lastWifiCheckMs >= WIFI_CHECK_INTERVAL_MS) {
    lastWifiCheckMs = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (staWasConnected) {
        Serial.println("[STA] desconectado — reintentando...");
        staWasConnected = false;
        wifiPendingBegin = false;
        wifiRetryDelayMs = WIFI_RETRY_MIN_MS;  // caida nueva: empezar con la espera corta
        lastWifiTryMs = millis();
      }
      // Reintentar solo cuando toca. Llamar a begin() mientras el supplicant sigue
      // conectando devuelve "sta is connecting, return error" y reinicia el barrido.
      // Dos pasos, separados por un tick (2 s): WiFi.disconnect() es asincrono, y
      // llamar a begin() en la misma pasada lo pisa -> "sta is connecting, return error".
      if (wifiPendingBegin) {
        wifiPendingBegin = false;
        WiFi.begin(staSsidSaved.c_str(), staPassSaved.c_str());  // no bloquea; conecta en segundo plano
      } else if (millis() - lastWifiTryMs >= wifiRetryDelayMs) {
        lastWifiTryMs = millis();
        wifiRetryDelayMs = min(wifiRetryDelayMs * 2, (unsigned long)WIFI_RETRY_MAX_MS);
        Serial.printf("[STA] intento de reconexion (proximo en %lus)\n", wifiRetryDelayMs / 1000);
        WiFi.disconnect();       // limpia estado colgado del supplicant
        wifiPendingBegin = true; // el begin() va en el siguiente tick
      }
    } else if (!staWasConnected) {
      // acaba de reconectar: relanzar NTP en segundo plano. NO se llama getLocalTime()
      // con espera aqui: bloquearia el loop y retrasaria la apertura del porton.
      // El SNTP actualiza la hora solo en unos segundos via callback interno.
      staWasConnected = true;
      wifiRetryDelayMs = WIFI_RETRY_MIN_MS;   // volver a la espera corta para la proxima caida
      Serial.printf("[STA] reconectado  IP=%s\n", WiFi.localIP().toString().c_str());
      String tzPosix; loadTzConfig(tzPosix);
      configTzTime(tzPosix.c_str(), NTP_SERVER);  // no bloquea: solo configura y arranca SNTP
      startMdns();  // re-anunciar el nombre .local tras reconectar
      // Con una sola radio el AP debe compartir canal con el STA o el ESP lo fuerza
      // a migrar (latencia/inestabilidad). Antes esto se hacia en setup() tras la
      // espera bloqueante; ahora que el STA conecta en segundo plano, va aqui.
      if (WiFi.channel() != apStartedChannel) {
        String apSsid, apPass; loadApConfig(apSsid, apPass);
        Serial.printf("[AP] moviendo al canal del router (%d -> %d)\n", apStartedChannel, WiFi.channel());
        startSoftAp(apSsid, apPass, WiFi.channel());
      }
    }
  }

  // reinicio del receptor RF solicitado por handler (detener TX, reset manual)
  if (pendingRfReset) {
    pendingRfReset = false;
    reinitRfReceiver();
    return;
  }

  // barrido de frecuencia solicitado por handler
  if (pendingScan) { pendingScan = false; startRfScan(); return; }
  if (rfScanning)  { pollRfScan(); return; }

  // diagnostico RF: iniciar captura cruda solicitada por handler
  if (pendingSniff) {
    pendingSniff = false;
    startRfSniff();
    return;
  }
  // diagnostico RF en curso: manejar las dos fases
  if (rfSniffing) {
    if (sniffPhase == 1) {
      pollRfSniffRssi();
      if (millis() >= rfSniffEndMs) startRfSniffDecodePhase();
    } else if (sniffPhase == 2) {
      pollRfSniffDecode();
      if (millis() >= rfSniffEndMs) finishRfSniff();
    }
    return;
  }

  // transmision de codigo de provisionamiento
  if (provisionName.length() > 0) {
    if (millis() >= provisionEndMs) {
      Serial.printf("[PROVISION] fin para %s\n", provisionName.c_str());
      provisionName = "";
      provisionCode = 0;
      reinitRfReceiver();
      Serial.println("[RF] receptor restaurado tras provision");
    } else if (millis() >= provisionNextTxMs) {
      provisionNextTxMs = millis() + 200;
      // Primera vez: entrar en TX mode una sola vez y quedarse ahi
      if (!provisionTxReady) {
        mySwitch.disableReceive();
        ELECHOUSE_cc1101.SetTx();
        ELECHOUSE_cc1101.setPA(CC1101_PA_DBM);
        provisionTxReady = true;
      }
      // Transmitir sin ciclar TX/RX — el chip queda en TX entre bursts
      mySwitch.enableTransmit(RF_RX_PIN);
      mySwitch.send(provisionCode, provisionBits);
      mySwitch.disableTransmit(); // GPIO27 queda LOW (carrier off entre bursts)
    }
  }

  if (mySwitch.available()) {
    unsigned long code  = mySwitch.getReceivedValue();
    unsigned int  bits  = mySwitch.getReceivedBitlength();
    unsigned int  pulse = mySwitch.getReceivedDelay();
    mySwitch.resetAvailable();

    if (code == 0) return;
    if (code == lastCode && (millis() - lastDetectionMs) < COOLDOWN_MS) return;
    lastCode = code; lastDetectionMs = millis();

    Serial.printf("[RF] codigo=%lu bits=%u pulse=%uµs\n", code, bits, pulse);

    if (learnName.length() > 0) {
      JsonDocument doc; loadUsers(doc);
      JsonArray arr = doc.as<JsonArray>();
      bool existed = false;
      String oldName = "";
      for (JsonObject u : arr) {
        if (u["code"].as<unsigned long>() == code) {
          oldName = u["name"].as<String>();
          existed = true;
          break;
        }
      }
      if (existed) {
        // El codigo ya esta registrado: NO renombrar ni duplicar. Rechazar y avisar.
        learnCloneOf = oldName;
        Serial.printf("[LEARN] rechazado: codigo %lu ya existe como '%s'\n", code, oldName.c_str());
      } else {
        JsonObject nu = arr.add<JsonObject>();
        nu["code"] = code; nu["name"] = learnName; nu["blocked"] = false; nu["bits"] = bits;
        learnCloneOf = "";
        saveUsers(doc);
        rebuildUserCache();
        Serial.printf("[LEARN] %s -> %lu\n", learnName.c_str(), code);
      }
      learnName = "";
      return;
    }

    // buscar usuario en cache RAM (sin leer flash)
    String name = "";
    bool blocked = false;
    bool known = false;
    for (int i = 0; i < userCacheCount; i++) {
      if (userCache[i].code == code) {
        name    = String(userCache[i].name);
        blocked = userCache[i].blocked;
        known   = true;
        break;
      }
    }
    if (!known) name = "Desconocido";

    if (known && !blocked) {
      digitalWrite(RELAY_PIN, HIGH);
      relayEndMs = millis() + relayPulseMs;
      Serial.printf("[RELAY] activado para %s\n", name.c_str());
      sendTelegram("notifyAccess", "Acceso: " + name + " - " + currentTimestamp());
    } else if (blocked) {
      Serial.printf("[BLOQUEADO] %s (codigo %lu)\n", name.c_str(), code);
      sendTelegram("notifyBlocked", "BLOQUEADO: " + name + " - " + currentTimestamp());
    } else {
      sendTelegram("notifyUnknown", "Desconocido: codigo " + String(code) + " - " + currentTimestamp());
    }

    if (known || logUnknown) addLog(name, code, blocked && known, pulse);
    Serial.printf("[LOG] %s (codigo %lu, pulse=%uµs)%s\n", name.c_str(), code, pulse, blocked ? " [BLOQUEADO]" : "");
  }
}
