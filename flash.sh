#!/usr/bin/env bash
#
# flash.sh — compila y sube el firmware del porton 433MHz con PlatformIO.
#
# Uso:
#   ./flash.sh                compila y sube (auto-detecta el puerto)
#   ./flash.sh build          solo compila (no sube)
#   ./flash.sh monitor        abre el monitor serie (115200)
#   ./flash.sh upload         sube y abre el monitor serie
#   ./flash.sh clean          borra la carpeta de build (.pio)
#   ./flash.sh diag           sube el AUTO-TEST del ESP32 (no el firmware del porton)
#   ./flash.sh restore        vuelve al firmware del porton tras un diag
#   ./flash.sh -p /dev/ttyUSB0 [accion]   fuerza un puerto especifico
#
set -euo pipefail
cd "$(dirname "$0")"

# localizar el ejecutable de PlatformIO
PIO="$(command -v pio || true)"
[ -z "$PIO" ] && [ -x "$HOME/.platformio/penv/bin/pio" ] && PIO="$HOME/.platformio/penv/bin/pio"
if [ -z "$PIO" ]; then
  echo "ERROR: no se encontro 'pio'. Instala PlatformIO Core." >&2
  exit 1
fi

# parsear puerto opcional (-p /dev/ttyXXX)
PORT_ARGS=()
if [ "${1:-}" = "-p" ]; then
  PORT_ARGS=(--upload-port "$2" --monitor-port "$2")
  shift 2
fi

ACTION="${1:-upload}"

case "$ACTION" in
  build)
    "$PIO" run
    ;;
  monitor)
    "$PIO" device monitor -b 115200 "${PORT_ARGS[@]}"
    ;;
  clean)
    "$PIO" run -t clean
    ;;
  upload)
    # compila, sube y deja el monitor serie abierto (baud del monitor = monitor_speed en platformio.ini)
    "$PIO" run -t upload -t monitor "${PORT_ARGS[@]}"
    ;;
  diag)
    # auto-test de GPIO/SPI. Requiere el CC1101 DESCONECTADO, y un puente
    # GPIO23<->GPIO19 para que corra ademas el loopback SPI.
    echo ">>> AUTO-TEST: desconecta el CC1101 antes de continuar."
    echo ">>> (opcional) puentea GPIO23 <-> GPIO19 para probar el SPI."
    "$PIO" run -e diag -t upload "${PORT_ARGS[@]}"
    echo ">>> subido. Abriendo monitor (Ctrl-C para salir)..."
    python3 monitor.py
    ;;
  restore)
    "$PIO" run -e esp32dev -t upload "${PORT_ARGS[@]}"
    ;;
  *)
    echo "Accion desconocida: $ACTION" >&2
    echo "Usa: build | upload | monitor | clean | diag | restore" >&2
    exit 1
    ;;
esac
