# Control de Portón 433MHz con ESP32

Sistema de control de acceso para portones y puertas mediante controles remotos 433MHz, con panel web protegido por login, relevador físico y registro de accesos.

---

## Características

- Aprende y registra controles remotos 433MHz existentes
- Genera y transmite códigos únicos de 24 bits (RNG de hardware) para provisionar controles baratos de aprendizaje
- Activa un relevador físico al recibir un código autorizado
- Bloquea controles individualmente sin borrarlos
- Registro automático de cada acceso (fecha, hora, nombre)
- Opción de no registrar códigos desconocidos (configurable desde el panel)
- Panel web con autenticación de usuarios admin
- Múltiples cuentas admin con gestión desde la web
- Red WiFi dual: AP propio + conexión a WiFi de casa simultáneos
- Sincronización de hora por NTP (cuando hay WiFi de casa) con zona horaria configurable desde la web
- Sincronización manual de NTP desde el panel (botón)
- Soporte para RTC DS3231 (opcional) para mantener hora sin internet
- Pulso del relevador configurable desde la web (0.1 – 30 segundos)
- Factory reset por botón físico
- Exportación e importación de controles en CSV
- Notificaciones Telegram configurables por tipo de evento

---

## Hardware requerido

| Componente | Detalle |
|---|---|
| ESP32 DevKit | Cualquier variante con botón BOOT en GPIO 0 |
| CC1101 | Transceptor 433MHz (TX + RX en un módulo) — reemplaza al MX-RM-5V + FS1000A |
| Relevador (relay) | Módulo de 1 canal, 5V o 3.3V según tu módulo |
| LED azul | Integrado en GPIO 2 (ya incluido en la mayoría de DevKits) |
| RTC DS3231 | **Opcional** — mantiene hora sin internet (incluye batería CR2032) |

### Pinout del CC1101

| Pin CC1101 | Conectar a ESP32 |
|---|---|
| VCC | 3.3V |
| GND | GND |
| MOSI | GPIO 23 |
| MISO | GPIO 19 |
| SCK | GPIO 18 |
| CSN / SS | GPIO 5 |
| GDO0 | GPIO 27 (interrupción RX) |
| GDO2 | GPIO 32 (transmisión TX) |

> El CC1101 opera a **3.3V**. No conectar a 5V.

> Para mayor alcance soldá una antena de **17.3 cm** (λ/4 a 433MHz) al pin ANT del módulo.

### Conexiones completas ESP32

| Señal | GPIO ESP32 |
|---|---|
| CC1101 MOSI | 23 |
| CC1101 MISO | 19 |
| CC1101 SCK | 18 |
| CC1101 CSN | 5 |
| CC1101 GDO0 (RX) | 27 |
| CC1101 GDO2 (TX) | 32 |
| Relevador (IN) | 26 |
| LED de estado | 2 (integrado) |
| Botón factory reset | 0 (BOOT, integrado) |
| DS3231 SDA (opcional) | 21 (I2C por defecto) |
| DS3231 SCL (opcional) | 22 (I2C por defecto) |

> El DS3231 opera a 3.3V. Conectar VCC al pin 3.3V del ESP32 (no al 5V).

> Los pines se pueden cambiar modificando las constantes al inicio de `porton_433.ino`.

### Diagrama de conexiones

![Diagrama de conexiones](diagrama_conexiones.svg)

> Si el diagrama no se ve, abrí [diagrama_conexiones.html](diagrama_conexiones.html) directamente en el navegador.

---

## Librerías necesarias (Arduino IDE)

Instalar desde el gestor de librerías (**Sketch → Incluir librería → Gestionar librerías**):

| Librería | Autor |
|---|---|
| **ELECHOUSE_CC1101_SRC_DRV** | LSatan (SmartRC-CC1101-Driver) |
| **RCSwitch** | sui77 |
| **ArduinoJson** | Benoit Blanchon |
| **RTClib** | Adafruit (opcional, solo si usás el módulo DS3231) |

Las demás (`WiFi`, `WebServer`, `LittleFS`, `WiFiClientSecure`, `time.h`, `mbedtls/md5.h`) vienen incluidas en el core de ESP32.

> **Nota:** Si el IDE muestra errores en `mbedtls/md5.h` o `WiFiClientSecure.h`, son falsos positivos del IntelliSense. El código compila correctamente con el toolchain del ESP32.

---

## Configuración en el código

Al inicio de `porton_433.ino` están las constantes que podés modificar:

```cpp
#define CC1101_CS_PIN         5    // SPI CSN / SS del CC1101
#define RF_RX_PIN            27    // GDO0 — interrupción RX
#define RF_TX_PIN            32    // GDO2 — transmisión TX
#define RELAY_PIN            26    // Pin de señal del relevador
#define RELAY_PULSE_MS     1000    // Valor por defecto del pulso (ms); configurable desde la web
#define PROVISION_DURATION_MS 15000 // Tiempo transmitiendo código nuevo (ms)
#define COOLDOWN_MS          3000  // Ignora el mismo código dentro de esta ventana
#define MAX_LOGS              500  // Límite del historial de accesos
```

El pulso del relevador también se puede cambiar en cualquier momento desde la pestaña **Red → Relevador** sin necesidad de recompilar.

---

## Primer uso

### 1. Cargar el sketch

1. Abrí `porton_433.ino` en Arduino IDE.
2. Seleccioná tu placa: **Herramientas → Placa → ESP32 Dev Module**.
3. Seleccioná el puerto COM correcto.
4. Cargá el sketch con el botón **Subir**.

### 2. Conectarse al portón

Al encender por primera vez el ESP32 crea su propia red WiFi:

- **SSID:** `Porton_Config`
- **Password:** `porton1234`

Conectate a esa red desde tu celular o PC y abrí el navegador en:

```
http://192.168.4.1
```

### 3. Login inicial

La primera vez se crea automáticamente un usuario admin:

- **Usuario:** `admin`
- **Password:** `admin1234`

> **Importante:** Cambiá estas credenciales inmediatamente desde **Admins → Cambiar mi password**.

---

## Panel web

### Pestaña — Registros

Historial de todos los accesos con fecha/hora, nombre y código del control.

- **Registrar códigos desconocidos** (checkbox al tope): si está desactivado, los códigos RF no registrados no aparecerán en el historial. Se aplica inmediatamente al cambiar, sin botón de guardar. La preferencia se persiste en flash.
- Accesos de controles **bloqueados** aparecen con badge rojo `BLOQUEADO`.
- **Actualizar** recarga el listado manualmente.
- **Descargar CSV** exporta el historial completo.
- **Borrar registros** elimina todo el historial.

### Pestaña — Controles

Lista todos los controles remotos registrados y permite gestionarlos.

**Aprender un control existente:**
1. Escribí un nombre (ej. `Usuario`, `Vecino`).
2. Presioná **Iniciar aprendizaje**.
3. Apretá una vez el botón del control remoto.
4. El código se guarda automáticamente.

**Generar control único (requiere CC1101 conectado):**
1. Escribe un nombre en la sección **Generar control unico**.
2. Presioná **Generar y transmitir**.
3. El ESP32 genera un código de 24 bits por RNG de hardware, lo guarda y lo transmite cada segundo durante 15 seg.
4. Poné tu control de aprendizaje en modo aprendizaje y apuntalo al portón durante esos 15 seg.
5. Si no grabó en el primer intento, aparece el botón **Retransmitir 15s** para volver a intentarlo.
6. El control queda grabado con un código único que no comparte con ningún otro.

**Exportar / Importar controles:**
- **Exportar Excel** descarga todos los controles como `controles.csv`.
- **Importar CSV** carga controles desde un archivo CSV (mismo formato que la exportación).

**Bloquear / habilitar / borrar un control:**
- **Bloquear** impide el acceso sin eliminar el control.
- **Habilitar** restaura el acceso.
- **Borrar** lo elimina definitivamente del sistema.

### Pestaña — Red

**Relevador:**
Muestra el pulso actual y permite cambiarlo desde la web.
- Rango: 0.1 – 30 segundos.
- El cambio se aplica al instante y persiste en flash (`/relay.json`).

**AP del portón:**
Cambia el SSID y contraseña de la red WiFi propia del dispositivo.
- Password mínimo 8 caracteres. Dejarlo vacío para red abierta.
- El dispositivo se reinicia al guardar.

**Conexión a WiFi de casa:**
Conecta el ESP32 a tu red doméstica para:
- Tener hora real en los registros (sincronización NTP automática).
- Acceder al panel desde cualquier dispositivo de tu red local.
- El AP propio sigue funcionando en simultáneo.

**Notificaciones Telegram:**
Configuración para recibir mensajes en un grupo o chat de Telegram.

| Campo | Descripción |
|---|---|
| Activar notificaciones | Toggle general |
| Bot Token | Obtenido con @BotFather |
| Chat ID | Obtenido con @userinfobot (grupos: número negativo) |
| Acceso autorizado | Notifica al abrir el portón |
| Control bloqueado | Notifica intentos de acceso bloqueados |
| Código desconocido | Notifica códigos RF no registrados |

> Requiere WiFi de casa activo.

**Zona horaria:**
Selector de zona horaria. Se aplica al instante sin reiniciar. Opciones disponibles: México (tres zonas), países de América Latina, España y UTC.

> La zona se guarda en `/tz.json`. NTP obtiene la hora en UTC y el ESP32 la convierte localmente usando la cadena POSIX configurada.

**Reloj / Hora del sistema:**
- Muestra el estado del DS3231 (Conectado / No detectado) y la hora local actual.
- **Sincronizar NTP ahora**: fuerza una sincronización de hora por red en ese momento (requiere WiFi de casa). Si el DS3231 está conectado, también lo actualiza.
- **Establecer hora manualmente**: ingresás la hora en tu zona horaria local y el sistema la convierte a UTC internamente. Útil sin internet.

Prioridad de sincronización:
1. Al arrancar: si el DS3231 tiene hora válida (año ≥ 2020) la carga al sistema.
2. Al conectar al WiFi de casa: NTP sincroniza automáticamente con la zona horaria configurada y actualiza el DS3231.
3. Sin internet: el DS3231 mantiene la hora con su batería CR2032.
4. Sin DS3231 ni internet: la hora se muestra como `uptime+Xs`.

### Pestaña — Admins

**Usuarios admin:** lista todos los usuarios con acceso al panel.
- No se puede borrar el último admin que queda.

**Agregar admin:** crea un nuevo usuario con password propio (mínimo 6 caracteres).

**Cambiar mi password:** cambia la contraseña del usuario actualmente logueado.
- Requiere ingresar el password actual para confirmar.

---

## Relevador (relay)

El relevador se activa por el tiempo configurado (por defecto **1 segundo**) cada vez que se recibe un código registrado y no bloqueado.

| Situación | Relevador | Log |
|---|---|---|
| Control registrado y habilitado | Se activa N seg | Acceso normal |
| Control registrado y bloqueado | No se activa | `BLOQUEADO` en rojo |
| Código desconocido | No se activa | `Desconocido` (si la opción está activa) |

El tiempo de pulso se ajusta desde **Red → Relevador** (0.1 – 30 segundos) sin necesidad de recompilar.

---

## Factory Reset

Si olvidás el password del AP y no podés conectarte al panel:

1. Mantené apretado el botón **BOOT** de la placa por **5 segundos**.
2. El LED azul parpadea con frecuencia creciente mientras contás.
3. Al completar los 5 segundos el ESP32 reinicia con los valores por defecto:
   - **SSID:** `Porton_Config`
   - **Password:** `porton1234`

> Los registros, controles aprendidos y usuarios admin **NO se borran** con el factory reset. Solo se resetea la configuración del AP.

---

## Archivos en flash (LittleFS)

| Archivo | Contenido |
|---|---|
| `/users.json` | Controles remotos (código, nombre, estado bloqueado, bits) |
| `/logs.json` | Historial de accesos (máximo 500 entradas) |
| `/admins.json` | Usuarios admin con password hasheado (MD5) |
| `/wifi.json` | Credenciales del WiFi de casa |
| `/ap.json` | Configuración del AP propio (SSID + password) |
| `/tz.json` | Zona horaria (cadena POSIX, ej. `<-06>6` para México Centro UTC-6) |
| `/telegram.json` | Token, Chat ID y preferencias de notificaciones Telegram |
| `/relay.json` | Pulso del relevador en ms y flag `logUnknown` |

---

## Seguridad

- El panel requiere login con sesión por cookie (duración 1 hora).
- Los passwords se almacenan como hash MD5, nunca en texto plano.
- Las sesiones viven solo en RAM; al reiniciar el ESP32 se cierran todas.
- Máximo 5 sesiones simultáneas y 10 usuarios admin.
- No usa HTTPS (limitación de hardware). Se recomienda usar en red local de confianza.

---

## Diagrama de flujo — transmisión de código único

```
Usuario presiona "Generar y transmitir"
        │
        ▼
  ESP32 genera código 24 bits (esp_random())
  Guarda en users.json con el nombre asignado
        │
        ▼
  Transmite via CC1101 cada 1 segundo
  durante 15 seg (GDO2 / GPIO32)
        │
        ▼
  Control de aprendizaje copia el código
  └── Listo — control queda registrado con código único
```

## Diagrama de flujo — detección RF

```
Señal 433MHz recibida (GDO0 / GPIO27)
        │
        ▼
  ¿Modo aprendizaje activo?
   ├── Sí → Guarda código con el nombre ingresado. Fin.
   └── No
        │
        ▼
  ¿Código registrado?
   ├── No → ¿logUnknown activo? → Log "Desconocido". Relevador inactivo. Fin.
   └── Sí
        │
        ▼
  ¿Control bloqueado?
   ├── Sí → Log "BLOQUEADO", relevador inactivo. Fin.
   └── No → Activa relevador N segundos + Log acceso + Telegram (si configurado). Fin.
```
