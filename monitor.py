#!/usr/bin/env python3
"""
Monitor serie para el Portón 433MHz (ESP32).

Ventaja sobre el monitor normal: NO togglea las lineas DTR/RTS, que en los
DevKit ESP32 estan conectadas (via transistores) a EN y GPIO0. Muchos monitores
las mueven al abrir/cerrar y eso reinicia la placa o simula "BOOT presionado".
Este script las deja quietas.

Uso:
    python3 monitor.py                     # /dev/ttyUSB0 a 115200
    python3 monitor.py -p /dev/ttyUSB1     # otro puerto
    python3 monitor.py -b 115200           # otra velocidad
    python3 monitor.py -f RF               # solo lineas que contengan "RF"
    python3 monitor.py -f "RF,SNIFF,STA"   # varias palabras (cualquiera)
    python3 monitor.py --no-ts             # sin marca de tiempo
    python3 monitor.py --reset             # dar un reset al abrir (pulso RTS)

Ctrl-C para salir.
"""
import argparse, sys, time

try:
    import serial
except ImportError:
    sys.exit("Falta pyserial. Instala con:  pip install pyserial   (o: pio pkg exec -- pip install pyserial)")


def open_port(port, baud, do_reset):
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = 1
    # Evitar que la apertura mueva DTR/RTS (reinicia/mete ruido en GPIO0)
    s.dtr = False
    s.rts = False
    s.open()
    s.dtr = False
    s.rts = False
    if do_reset:
        # pulso de reset controlado (solo si el usuario lo pide)
        s.setRTS(True); time.sleep(0.1); s.setRTS(False)
    return s


def main():
    ap = argparse.ArgumentParser(description="Monitor serie ESP32 (sin togglear DTR/RTS)")
    ap.add_argument("-p", "--port", default="/dev/ttyUSB0", help="puerto serie (def: /dev/ttyUSB0)")
    ap.add_argument("-b", "--baud", type=int, default=115200, help="velocidad (def: 115200)")
    ap.add_argument("-f", "--filter", default="", help="mostrar solo lineas con estas palabras (coma = varias)")
    ap.add_argument("--no-ts", action="store_true", help="no anteponer marca de tiempo")
    ap.add_argument("--reset", action="store_true", help="dar un reset (pulso RTS) al abrir")
    args = ap.parse_args()

    keys = [k.strip() for k in args.filter.split(",") if k.strip()]
    print(f"[monitor] {args.port} @ {args.baud}  filtro={keys or 'ninguno'}  (Ctrl-C para salir)", flush=True)

    while True:
        try:
            s = open_port(args.port, args.baud, args.reset)
        except Exception as e:
            print(f"[monitor] no se pudo abrir {args.port}: {e} — reintentando en 2s...", flush=True)
            time.sleep(2)
            continue

        print(f"[monitor] conectado a {args.port}", flush=True)
        try:
            while True:
                raw = s.readline()
                if not raw:
                    continue
                line = raw.decode(errors="replace").rstrip("\r\n")
                if not line:
                    continue
                if keys and not any(k in line for k in keys):
                    continue
                if args.no_ts:
                    print(line, flush=True)
                else:
                    print(f"{time.strftime('%H:%M:%S')}  {line}", flush=True)
        except KeyboardInterrupt:
            print("\n[monitor] saliendo.", flush=True)
            s.close()
            return
        except Exception as e:
            print(f"[monitor] desconectado ({e}) — reconectando...", flush=True)
            try:
                s.close()
            except Exception:
                pass
            time.sleep(2)


if __name__ == "__main__":
    main()
