// ============================================================================
// AUTO-TEST DEL ESP32 — diagnostico independiente del firmware del porton
//
// Responde una sola pregunta: ¿los GPIO y el periferico SPI que usa el CC1101
// siguen sanos, o el ESP32 se daño?
//
// Compilar y subir:   pio run -e diag -t upload
// Ver resultado:      python3 monitor.py
// Volver al firmware: pio run -e esp32dev -t upload
//
// ANTES DE CORRER:
//   1. DESCONECTA el modulo CC1101 por completo. Si sigue conectado, sus pines
//      cargan las lineas y los tests dan falsos negativos.
//   2. Para el test de SPI, puentea con un cable GPIO23 (MOSI) <-> GPIO19 (MISO).
//      Sin el puente el test se salta solo; no es obligatorio, pero es el unico
//      que prueba el periferico SPI de punta a punta.
// ============================================================================

#include <Arduino.h>
#include <SPI.h>

#define PIN_CS    5
#define PIN_SCK  18
#define PIN_MISO 19
#define PIN_MOSI 23
#define PIN_RELAY 26
#define PIN_RX   27   // GDO0
#define PIN_TX   32   // GDO2
#define PIN_LED   2

static int fallos = 0;

// Un pin de salida sano puede forzar su propio nivel y releerlo. Devuelve true
// si conmuta. Es la prueba MAS FUERTE: un driver push-pull vence a cualquier
// resistencia externa, asi que no da falsos negativos por el cableado de la placa.
static bool pinConmuta(int pin) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH); delay(5);
  int alto = digitalRead(pin);
  digitalWrite(pin, LOW);  delay(5);
  int bajo = digitalRead(pin);
  pinMode(pin, INPUT);
  return (alto == HIGH && bajo == LOW);
}

// Un GPIO sano sigue a su resistencia interna: con pull-up lee HIGH, con
// pull-down lee LOW. Un pin quemado se queda pegado y no responde a ninguna.
//
// PERO el pull interno del ESP32 son ~45k, y varias placas montan un pull-up
// EXTERNO de 10k en los pines de strapping (GPIO5, GPIO0, GPIO2, GPIO12, GPIO15)
// para fijar el modo de arranque. Esos 10k ganan la pelea y el pin lee HIGH en
// ambos casos — en una placa perfectamente sana. Por eso, cuando el test de
// pulls falla, se cae al test de salida, que una resistencia externa no puede
// falsear. Sin este segundo paso, GPIO5 se reportaba como dañado sin estarlo.
static void testPin(const char *nombre, int pin) {
  pinMode(pin, INPUT_PULLUP);
  delay(5);
  int alto = digitalRead(pin);
  pinMode(pin, INPUT_PULLDOWN);
  delay(5);
  int bajo = digitalRead(pin);
  pinMode(pin, INPUT);

  if (alto == HIGH && bajo == LOW) {
    Serial.printf("  [OK ] GPIO%-2d %-12s pull-up=HIGH  pull-down=LOW\n", pin, nombre);
    return;
  }

  const char *pegado = (alto == bajo) ? (alto ? "ALTO" : "BAJO") : "?";
  if (pinConmuta(pin)) {
    Serial.printf("  [OK ] GPIO%-2d %-12s pegado en %s por resistencia EXTERNA,\n",
                  pin, nombre, pegado);
    Serial.printf("                            pero conmuta como salida -> pin SANO\n");
  } else {
    fallos++;
    Serial.printf("  [MAL] GPIO%-2d %-12s pegado en %s y NO conmuta como salida\n",
                  pin, nombre, pegado);
  }
}

static void testSalida(const char *nombre, int pin) {
  if (pinConmuta(pin)) {
    Serial.printf("  [OK ] GPIO%-2d %-12s conmuta alto/bajo\n", pin, nombre);
  } else {
    fallos++;
    Serial.printf("  [MAL] GPIO%-2d %-12s no conmuta\n", pin, nombre);
  }
}

// Loopback SPI: con MOSI puenteado a MISO, todo byte que sale debe volver
// identico. Es la prueba mas concluyente que existe sin el modulo: valida los
// dos pines Y el periferico SPI del ESP32 de punta a punta. Si esto pasa, el
// ESP32 esta sano y la culpa del "MISO nunca bajo" es del modulo o del cableado.
static void testSpiLoopback() {
  Serial.println("\n[3] Loopback SPI (requiere puente GPIO23 <-> GPIO19)");

  // Detectar el puente antes de nada: si MOSI no arrastra a MISO, no hay cable.
  pinMode(PIN_MOSI, OUTPUT);
  pinMode(PIN_MISO, INPUT_PULLUP);
  digitalWrite(PIN_MOSI, LOW);  delay(5);
  bool arrastraBajo = (digitalRead(PIN_MISO) == LOW);
  digitalWrite(PIN_MOSI, HIGH); delay(5);
  bool arrastraAlto = (digitalRead(PIN_MISO) == HIGH);
  pinMode(PIN_MOSI, INPUT);

  if (!arrastraBajo || !arrastraAlto) {
    Serial.println("  [--] sin puente detectado: test omitido");
    Serial.println("       pon un cable entre GPIO23 y GPIO19 y reinicia para correrlo");
    return;
  }

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  const uint8_t patron[] = {0x00, 0xFF, 0xAA, 0x55, 0x14, 0x31, 0x5A, 0xA5};
  int malos = 0;
  for (uint8_t b : patron) {
    uint8_t r = SPI.transfer(b);
    if (r != b) {
      malos++;
      Serial.printf("  [MAL] envie 0x%02X y volvio 0x%02X\n", b, r);
    }
  }
  SPI.endTransaction();
  SPI.end();

  if (malos == 0) {
    Serial.printf("  [OK ] %d bytes ida y vuelta sin un solo error\n", (int)sizeof(patron));
    Serial.println("       -> el SPI y los pines MOSI/MISO del ESP32 estan SANOS");
  } else {
    fallos++;
    Serial.printf("  [MAL] %d de %d bytes corruptos -> SPI o pines dañados\n",
                  malos, (int)sizeof(patron));
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n\n=== AUTO-TEST ESP32 (porton 433MHz) ===");
  Serial.println("IMPORTANTE: el modulo CC1101 debe estar DESCONECTADO.\n");

  Serial.println("[1] Pines del bus CC1101 (pull-up / pull-down interno)");
  testPin("CSN",  PIN_CS);
  testPin("SCK",  PIN_SCK);
  testPin("MISO", PIN_MISO);
  testPin("MOSI", PIN_MOSI);
  testPin("GDO0", PIN_RX);
  testPin("GDO2", PIN_TX);

  Serial.println("\n[2] Pines de salida");
  testSalida("RELAY", PIN_RELAY);
  testSalida("LED",   PIN_LED);

  testSpiLoopback();

  Serial.println("\n=== RESULTADO ===");
  if (fallos == 0) {
    Serial.println("  ESP32 SANO: ningun pin ni el SPI presentan fallo.");
    Serial.println("  Si el CC1101 sigue sin responder, la causa es el modulo,");
    Serial.println("  su alimentacion (debe ser 3.3V, NUNCA 5V) o el cableado.");
  } else {
    Serial.printf("  %d FALLO(S): este ESP32 esta dañado, reemplazalo.\n", fallos);
    Serial.println("  Conectar transceptores nuevos a esta placa no va a resolver nada.");
  }
  Serial.println("\n(el test no se repite; reinicia la placa para volver a correrlo)");
}

void loop() {
  // Parpadeo lento: señal visual de que el test termino y la placa vive.
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH); delay(800);
  digitalWrite(PIN_LED, LOW);  delay(800);
}
