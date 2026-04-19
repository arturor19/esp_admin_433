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
#define TZ_OFFSET_SEC      -10800           // UTC-3 (Argentina)
#define DST_OFFSET_SEC     0
// ==============================================

RCSwitch   mySwitch = RCSwitch();
WebServer  server(80);

String        learnName = "";
unsigned long lastCode = 0;
unsigned long lastDetectionMs = 0;
bool          timeOk = false;

// estado del boton de reset
unsigned long btnPressStart = 0;
bool          btnWasPressed = false;
unsigned long lastBlinkMs = 0;
bool          ledState = false;

// ---------- forward decls ----------
String currentTimestamp();
bool   loadUsers(JsonDocument &doc);
bool   saveUsers(JsonDocument &doc);
bool   loadLogs(JsonDocument &doc);
bool   saveLogs(JsonDocument &doc);
void   loadApConfig(String &ssid, String &pass);
void   saveApConfig(const String &ssid, const String &pass);

// ================= STORAGE =================
// /users.json : [{"code":123456,"name":"Papa"}]
// /logs.json  : [{"ts":"...","name":"Papa","code":123456}]
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

void addLog(const String &name, unsigned long code) {
  JsonDocument doc; loadLogs(doc);
  JsonArray arr = doc.as<JsonArray>();
  JsonObject entry = arr.add<JsonObject>();
  entry["ts"]   = currentTimestamp();
  entry["name"] = name;
  entry["code"] = code;
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
      configTime(TZ_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
      struct tm tm;
      if (getLocalTime(&tm, 5000)) { timeOk = true; Serial.println("[NTP] hora sincronizada"); }
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
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Porton</title>
<style>
:root{--bg:#0f1115;--card:#1a1d24;--txt:#e6e6e6;--mut:#888;--acc:#4ade80;--warn:#fbbf24;--err:#f87171}
*{box-sizing:border-box}
body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--txt);margin:0;padding:16px;max-width:680px;margin:0 auto}
h1{font-size:22px;margin:0 0 4px}
h2{font-size:13px;margin:20px 0 8px;color:var(--mut);text-transform:uppercase;letter-spacing:.5px}
.sub{color:var(--mut);font-size:13px;margin-bottom:16px}
.card{background:var(--card);border-radius:12px;padding:14px;margin-bottom:12px}
.row{display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-bottom:1px solid #2a2e38;gap:10px}
.row:last-child{border-bottom:none}
.row .name{font-weight:600}
.row .meta{color:var(--mut);font-size:12px}
button,input{background:#262a33;border:1px solid #3a3e48;color:var(--txt);padding:10px 12px;border-radius:8px;font-size:14px;width:100%;font-family:inherit}
button{cursor:pointer;background:var(--acc);color:#051a0d;border:none;font-weight:600}
button.sec{background:#262a33;color:var(--txt)}
button.warn{background:var(--err);color:#fff}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:10px}
.empty{color:var(--mut);text-align:center;padding:24px 0}
.tag{display:inline-block;padding:2px 8px;border-radius:6px;font-size:11px;background:#262a33;color:var(--mut)}
.learning{background:var(--warn);color:#1a1200;padding:12px;border-radius:8px;text-align:center;margin-bottom:10px;font-weight:600}
.nav{display:flex;gap:6px;margin-bottom:16px}
.nav a{flex:1;text-align:center;padding:10px;text-decoration:none;color:var(--txt);background:var(--card);border-radius:8px;font-size:13px}
.nav a.active{background:var(--acc);color:#051a0d;font-weight:600}
.hint{font-size:11px;color:var(--mut);margin-top:6px;line-height:1.4}
</style></head><body>
<h1>&#128682; Porton</h1>
<div class="sub" id="status">cargando...</div>
<div class="nav">
  <a href="#" class="active" data-tab="logs">Registros</a>
  <a href="#" data-tab="users">Controles</a>
  <a href="#" data-tab="net">Red</a>
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
  <h2>Controles guardados</h2>
  <div class="card" id="users"></div>
  <h2>Aprender control nuevo</h2>
  <div class="card">
    <input type="text" id="newName" placeholder="Nombre (ej. Papa)" maxlength="20">
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
    <div class="hint">Si olvidas el password: manten apretado el boton BOOT del Porton Admin por 5 segundos. El LED azul parpadea y vuelve a Porton_Config / porton1234.</div>
  </div>

  <h2>Conexion a WiFi de casa</h2>
  <div class="card">
    <div style="margin-bottom:8px">Estado: <span id="wifiStatus" class="tag">-</span></div>
    <input type="text" id="ssid" placeholder="SSID de tu casa">
    <input type="password" id="pass" placeholder="Contrasena" style="margin-top:8px">
    <button style="margin-top:10px" onclick="saveWifi()">Guardar y reiniciar</button>
    <div class="hint">Si falla la conexion siempre podes volver al AP del porton.</div>
  </div>
</div>

<script>
function tab(id){
  document.querySelectorAll('.nav a').forEach(a=>a.classList.toggle('active',a.dataset.tab===id));
  ['logs','users','net'].forEach(t=>document.getElementById('tab-'+t).style.display=t===id?'':'none');
  if(id==='logs')loadLogs();if(id==='users')loadUsers();if(id==='net')loadStatus();
}
document.querySelectorAll('.nav a').forEach(a=>a.addEventListener('click',e=>{e.preventDefault();tab(a.dataset.tab);}));

async function loadStatus(){
  const r=await fetch('/api/status');const j=await r.json();
  document.getElementById('status').textContent=j.time+' - '+(j.staIp||'sin WiFi casa')+' - AP: '+j.apIp;
  document.getElementById('wifiStatus').textContent=j.staIp?('Conectado a '+j.ssid+' ('+j.staIp+')'):'No conectado';
  document.getElementById('apInfo').textContent=j.apSsid+(j.apOpen?' (abierta)':' (protegida)');
  document.getElementById('apSsid').value=j.apSsid;
}
async function loadLogs(){
  const r=await fetch('/api/logs');const j=await r.json();
  const el=document.getElementById('logs');
  if(!j.length){el.innerHTML='<div class="empty">Sin registros todavia</div>';return;}
  el.innerHTML=j.slice().reverse().map(e=>`<div class="row"><div><div class="name">${e.name||'Desconocido'}</div><div class="meta">codigo ${e.code}</div></div><div class="meta">${e.ts}</div></div>`).join('');
}
async function loadUsers(){
  const r=await fetch('/api/users');const j=await r.json();
  const el=document.getElementById('users');
  if(!j.users.length)el.innerHTML='<div class="empty">Todavia no hay controles aprendidos</div>';
  else el.innerHTML=j.users.map(u=>`<div class="row"><div><div class="name">${u.name}</div><div class="meta">codigo ${u.code}</div></div><button class="warn" style="width:auto;padding:6px 10px" onclick="delUser(${u.code})">Borrar</button></div>`).join('');
  const lb=document.getElementById('learnBox');
  lb.innerHTML=j.learning?`<div class="learning">Esperando boton del control para "${j.learning}"...</div>`:'';
  if(j.learning)setTimeout(loadUsers,1500);
}
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
  if(pass.length>0 && pass.length<8){alert('El password debe tener al menos 8 caracteres (o dejarlo vacio para red abierta)');return;}
  if(!confirm('El Porton Admin se va a reiniciar. Te vas a tener que reconectar con el nuevo password. Continuar?'))return;
  const r=await fetch('/api/ap',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,pass})});
  if(r.ok)alert('Guardado. Reiniciando...');
  else alert('Error al guardar');
}
loadStatus();loadLogs();setInterval(loadLogs,5000);
</script></body></html>
)rawliteral";

// ================= HANDLERS =================
void handleRoot() { server.send_P(200, "text/html", INDEX_HTML); }

void handleStatus() {
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
  if (!LittleFS.exists("/logs.json")) { server.send(200, "application/json", "[]"); return; }
  File f = LittleFS.open("/logs.json", "r");
  server.streamFile(f, "application/json");
  f.close();
}

void handleLogsCsv() {
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
  LittleFS.remove("/logs.json");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleUsers() {
  JsonDocument users; loadUsers(users);
  JsonDocument out;
  out["users"]    = users.as<JsonArray>();
  out["learning"] = learnName;
  String s; serializeJson(out, s);
  server.send(200, "application/json", s);
}

void handleLearnStart() {
  if (!server.hasArg("name")) { server.send(400, "text/plain", "name requerido"); return; }
  learnName = server.arg("name");
  Serial.printf("[LEARN] esperando codigo para: %s\n", learnName.c_str());
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleLearnCancel() {
  learnName = "";
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleUserDelete() {
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
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "body requerido"); return; }
  JsonDocument doc; deserializeJson(doc, server.arg("plain"));
  saveWiFiConfig(doc["ssid"].as<String>(), doc["pass"].as<String>());
  server.send(200, "application/json", "{\"ok\":true}");
  delay(500); ESP.restart();
}

void handleApSet() {
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

void setupWebServer() {
  server.on("/",                  handleRoot);
  server.on("/api/status",        handleStatus);
  server.on("/api/logs",          handleLogs);
  server.on("/api/logs/clear",    handleLogsClear);
  server.on("/logs.csv",          handleLogsCsv);
  server.on("/api/users",         handleUsers);
  server.on("/api/users/delete",  handleUserDelete);
  server.on("/api/learn/start",   handleLearnStart);
  server.on("/api/learn/cancel",  handleLearnCancel);
  server.on("/api/wifi",          HTTP_POST, handleWifiSet);
  server.on("/api/ap",            HTTP_POST, handleApSet);
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

  setupWiFi();
  setupWebServer();

  mySwitch.enableReceive(digitalPinToInterrupt(RF_RX_PIN));
  Serial.println("[RF] escuchando 433MHz en GPIO " + String(RF_RX_PIN));
  Serial.println("[BTN] mantene BOOT por 5s para factory reset del AP");
}

void loop() {
  server.handleClient();
  checkResetButton();

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
      for (JsonObject u : arr) {
        if (u["code"].as<unsigned long>() == code) { u["name"] = learnName; existed = true; break; }
      }
      if (!existed) {
        JsonObject nu = arr.add<JsonObject>();
        nu["code"] = code; nu["name"] = learnName;
      }
      saveUsers(doc);
      Serial.printf("[LEARN] %s -> %lu\n", learnName.c_str(), code);
      learnName = "";
      return;
    }

    String name = findUserName(code);
    if (name.length() == 0) name = "Desconocido";
    addLog(name, code);
    Serial.printf("[LOG] %s (codigo %lu)\n", name.c_str(), code);
  }
}
