# Control de Portón 433MHz con ESP32

Sistema de control de acceso para portones y puertas mediante controles remotos 433MHz, con panel web protegido por login, relevador físico y registro de accesos.

---

## Características

- Aprende y registra controles remotos 433MHz
- Activa un relevador físico al recibir un código autorizado
- Bloquea controles individualmente sin borrarlos
- Registro automático de cada acceso (fecha, hora, nombre)
- Panel web con autenticación de usuarios admin
- Múltiples cuentas admin con gestión desde la web
- Red WiFi dual: AP propio + conexión a WiFi de casa simultáneos
- Sincronización de hora por NTP (cuando hay WiFi de casa)
- Factory reset por botón físico

---

## Hardware requerido

| Componente | Detalle |
|---|---|
| ESP32 DevKit | Cualquier variante con botón BOOT en GPIO 0 |
| Receptor 433MHz | **MX-RM-5V** (superheterodino, 5V) |
| Relevador (relay) | Módulo de 1 canal, 5V o 3.3V según tu módulo |
| LED azul | Integrado en GPIO 2 (ya incluido en la mayoría de DevKits) |

### Pinout del MX-RM-5V

El módulo tiene 4 pines (de izquierda a derecha mirando la cara con componentes):

```
┌─────────────────────┐
│  ANT  VCC  GND  DATA│
└─────────────────────┘
```

| Pin MX-RM-5V | Conectar a |
|---|---|
| ANT | Antena (hilo de ~17.3 cm o resorte) |
| VCC | Pin **5V** del ESP32 DevKit |
| GND | GND del ESP32 |
| DATA | GPIO 27 del ESP32 (ver nota abajo) |

> **Importante — niveles de voltaje:** El MX-RM-5V opera a 5V y su pin DATA puede entregar hasta 5V. El ESP32 acepta en sus entradas un máximo de 3.3V en condiciones normales. Para proteger el ESP32 se recomienda un divisor de tensión entre DATA y GPIO 27:
>
> ```
> DATA (5V) ──[ 10kΩ ]──┬── GPIO 27
>                        │
>                      [ 20kΩ ]
>                        │
>                       GND
> ```
>
> En la práctica muchas instalaciones funcionan sin el divisor porque los pines del ESP32 toleran 5V en modo entrada, pero no está garantizado por el fabricante.

> **Antena:** para mayor alcance soldá un hilo rígido de **17.3 cm** al pin ANT (λ/4 a 433MHz).

### Conexiones completas ESP32

| Señal | GPIO ESP32 |
|---|---|
| MX-RM-5V DATA (con divisor) | 27 |
| Relevador (IN) | 26 |
| LED de estado | 2 (integrado) |
| Botón factory reset | 0 (BOOT, integrado) |

> Los pines se pueden cambiar modificando las constantes al inicio de `porton_433.ino`.

---

## Librerías necesarias (Arduino IDE)

Instalar desde el gestor de librerías (**Sketch → Incluir librería → Gestionar librerías**):

| Librería | Autor |
|---|---|
| **RCSwitch** | sui77 |
| **ArduinoJson** | Benoit Blanchon |

Las demás (`WiFi`, `WebServer`, `LittleFS`, `time.h`, `mbedtls/md5.h`) vienen incluidas en el core de ESP32.

> **Nota:** Si el IDE muestra error en `mbedtls/md5.h`, es un falso positivo del IntelliSense. El código compila correctamente con el toolchain del ESP32.

---

## Configuración en el código

Al inicio de `porton_433.ino` están las constantes que podés modificar:

```cpp
#define RF_RX_PIN       27      // Pin DATA del receptor 433MHz
#define RELAY_PIN       26      // Pin de señal del relevador
#define RELAY_PULSE_MS  2000    // Tiempo que permanece activo el relevador (ms)
#define TZ_OFFSET_SEC   -10800  // Zona horaria: UTC-3 Argentina
```

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

- Accesos de controles **bloqueados** aparecen con badge rojo `BLOQUEADO`.
- **Actualizar** recarga el listado manualmente.
- **Descargar CSV** exporta el historial completo.
- **Borrar registros** elimina todo el historial.

### Pestaña — Controles

Lista todos los controles remotos registrados y permite gestionarlos.

**Aprender un control nuevo:**
1. Escribí un nombre (ej. `Papa`, `Vecino`).
2. Presioná **Iniciar aprendizaje**.
3. Apretá una vez el botón del control remoto.
4. El código se guarda automáticamente.

**Bloquear / habilitar un control:**
- **Bloquear** impide el acceso sin eliminar el control. El relevador no se activará.
- **Habilitar** restaura el acceso.
- Los controles bloqueados se muestran con badge rojo.

**Borrar un control:**
- **Borrar** lo elimina definitivamente del sistema.

### Pestaña — Red

**AP del portón:**
Cambia el SSID y contraseña de la red WiFi propia del dispositivo.
- Password mínimo 8 caracteres. Dejarlo vacío para red abierta.
- El dispositivo se reinicia al guardar.

**WiFi de casa:**
Conecta el ESP32 a tu red doméstica para:
- Tener hora real en los registros (sincronización NTP automática).
- Acceder al panel desde cualquier dispositivo de tu red local.
- El AP propio sigue funcionando en simultáneo.

### Pestaña — Admins

**Usuarios admin:** lista todos los usuarios con acceso al panel.
- No se puede borrar el último admin que queda.

**Agregar admin:** crea un nuevo usuario con password propio (mínimo 6 caracteres).

**Cambiar mi password:** cambia la contraseña del usuario actualmente logueado.
- Requiere ingresar el password actual para confirmar.

---

## Relevador (relay)

El relevador se activa por **2 segundos** cada vez que se recibe un código registrado y no bloqueado.

| Situación | Relevador | Log |
|---|---|---|
| Control registrado y habilitado | Se activa 2 seg | Acceso normal |
| Control registrado y bloqueado | No se activa | `BLOQUEADO` en rojo |
| Código desconocido | No se activa | `Desconocido` |

El tiempo de pulso se ajusta con `RELAY_PULSE_MS` (milisegundos) en el código.

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
| `/users.json` | Controles remotos (código, nombre, estado bloqueado) |
| `/logs.json` | Historial de accesos (máximo 500 entradas) |
| `/admins.json` | Usuarios admin con password hasheado |
| `/wifi.json` | Credenciales del WiFi de casa |
| `/ap.json` | Configuración del AP propio |

---

## Seguridad

- El panel requiere login con sesión por cookie (duración 1 hora).
- Los passwords se almacenan como hash MD5, nunca en texto plano.
- Las sesiones viven solo en RAM; al reiniciar el ESP32 se cierran todas.
- Máximo 5 sesiones simultáneas y 10 usuarios admin.
- No usa HTTPS (limitación de hardware). Se recomienda usar en red local de confianza.

---

## Diagrama de flujo — detección RF

```
Señal 433MHz recibida
        │
        ▼
  ¿Modo aprendizaje activo?
   ├── Sí → Guarda código con el nombre ingresado. Fin.
   └── No
        │
        ▼
  ¿Código registrado?
   ├── No → Log "Desconocido", relevador inactivo. Fin.
   └── Sí
        │
        ▼
  ¿Control bloqueado?
   ├── Sí → Log "BLOQUEADO", relevador inactivo. Fin.
   └── No → Activa relevador 2 segundos + Log acceso. Fin.
```
