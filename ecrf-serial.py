#!/usr/bin/env python3
"""
ecrf-serial.py — Cliente terminal para Evil Crow RF via puerto serie.

Modo interactivo:
  python3 ecrf-serial.py [PORT] [BAUD]
  python3 ecrf-serial.py --port /dev/ttyUSB0 --baud 115200

Modo batch (script):
  python3 ecrf-serial.py --script PATH.ecrf [opciones]
  python3 ecrf-serial.py --script ~/evilcrow/scripts/example.ecrf --delay 1000

Opciones batch:
  --delay MS     Delay entre comandos en ms (defecto: 500)
  --timeout S    Timeout por respuesta en segundos (defecto: 5)
  --log PATH     Archivo de log (defecto: <script>.log junto al script)
  --no-log       No guardar log en modo batch

Formato .ecrf:
  # comentario           linea ignorada
  # delay <ms>           cambia el delay entre comandos
  # wait <s>             pausa sin enviar comando (ej: esperar captura RX)
  # timeout <s>          cambia el timeout del siguiente comando
  <comando>              enviado al dispositivo (texto despues de  # se ignora)

Requiere: pip install pyserial
"""

import sys
import os
import argparse
import serial
import serial.tools.list_ports
import threading
import time
import datetime

try:
    import readline
    HAS_READLINE = True
except ImportError:
    HAS_READLINE = False

import re

BAUD_DEFAULT      = 115200
DELAY_DEFAULT_MS  = 500.0
TIMEOUT_DEFAULT_S = 5.0

# ---- Prompt dinamico (v2.9) ----
# El firmware emite ECRF[IDLE]> / ECRF[RX:1@433.92]> / ECRF[JAM:2]>
# El cliente v<2.9 emitia ECRF> (mantenido para retrocompatibilidad)
PROMPT_LEGACY  = "ECRF> "
PROMPT_PREFIX  = "ECRF["    # prefijo comun de todos los prompts dinamicos
PROMPT_RE      = re.compile(r'^ECRF\[([^\]]+)\]> ?$')

# ANSI colors
_ANSI_RESET = "\033[0m"
_ANSI_BLUE  = "\033[94m"   # IDLE
_ANSI_GREEN = "\033[92m"   # RX activo
_ANSI_RED   = "\033[91m"   # JAM activo

# Estado actual — actualizado por el hilo lector, leido por el hilo principal
_device_state = "IDLE"   # str assignment es atomica en CPython (GIL)

def _color_prompt(state: str) -> str:
    """Devuelve el string de prompt ANSI-coloreado segun estado."""
    if state.startswith("RX:"):
        color = _ANSI_GREEN
    elif state.startswith("JAM:"):
        color = _ANSI_RED
    else:
        color = _ANSI_BLUE
    return f"{color}ECRF[{state}]> {_ANSI_RESET}"

def _live_prompt() -> str:
    """Prompt coloreado con el ultimo estado conocido del dispositivo."""
    return _color_prompt(_device_state)

# Directorio donde se guardan las capturas exportadas
CAPTURES_DIR = os.path.expanduser("~/evilcrow/captures")

# Extension por formato de exportacion
EXPORT_EXT = {"urh": ".urh.txt", "rtl": ".json"}

# Etiquetas de bloque de exportacion emitidas por el firmware
EXPORT_BEGIN_PREFIX = "[EXPORT-"
EXPORT_END_SUFFIX   = "-END]"


# ============================================================
# Utilidades de exportacion
# ============================================================

def _ensure_captures_dir():
    """Crea el directorio de capturas si no existe."""
    os.makedirs(CAPTURES_DIR, exist_ok=True)


def save_export_block(fmt, content_lines):
    """
    Guarda las lineas de un bloque de exportacion en
    ~/evilcrow/captures/<timestamp>.<ext>.
    Retorna la ruta del archivo guardado.
    """
    _ensure_captures_dir()
    ts  = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    ext = EXPORT_EXT.get(fmt.lower(), f".{fmt.lower()}.txt")
    path = os.path.join(CAPTURES_DIR, f"{ts}{ext}")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(content_lines) + "\n")
    return path


def extract_and_save_exports(lines):
    """
    Escanea una lista de lineas buscando bloques [EXPORT-*-BEGIN] / [EXPORT-*-END].
    Guarda cada bloque encontrado y retorna lista de (fmt, saved_path).
    """
    saved = []
    collecting = False
    fmt         = None
    buf         = []

    for line in lines:
        if line.startswith(EXPORT_BEGIN_PREFIX) and line.endswith("-BEGIN]"):
            # Extraer formato: "[EXPORT-URH-BEGIN]" -> "urh"
            fmt        = line[len(EXPORT_BEGIN_PREFIX):-7].lower()
            collecting = True
            buf        = []
        elif collecting and line == f"[EXPORT-{fmt.upper()}-END]":
            path = save_export_block(fmt, buf)
            saved.append((fmt, path))
            collecting = False
            fmt        = None
            buf        = []
        elif collecting:
            buf.append(line)

    return saved


# ============================================================
# Autodeteccion de puerto
# ============================================================

def find_port():
    """Autodetectar puerto ESP32."""
    candidates = []
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "").lower()
        hwid  = (p.hwid or "").lower()
        if any(k in desc or k in hwid
               for k in ["cp210", "ch340", "ftdi", "esp", "uart", "usb serial"]):
            candidates.append(p.device)
    if candidates:
        return candidates[0]
    for p in serial.tools.list_ports.comports():
        if "USB" in p.device or "ACM" in p.device:
            return p.device
    return None


# ============================================================
# Modo interactivo
# ============================================================

def _reader_thread(ser):
    """
    Hilo que imprime lo que llega del dispositivo (modo interactivo).
    Detecta bloques [EXPORT-*-BEGIN/END] y los guarda automaticamente.
    """
    export_collecting = False
    export_fmt        = None
    export_buf        = []

    while True:
        try:
            line = ser.readline()
            if not line:
                continue
            text = line.decode("utf-8", errors="replace").rstrip()

            # Detectar inicio de bloque de exportacion
            if text.startswith(EXPORT_BEGIN_PREFIX) and text.endswith("-BEGIN]"):
                export_fmt        = text[len(EXPORT_BEGIN_PREFIX):-7].lower()
                export_collecting = True
                export_buf        = []
                sys.stdout.write(f"\r\033[K{text}\n")
                sys.stdout.write(f"\r\033[K[*] Recibiendo exportacion formato={export_fmt}...\n")
                sys.stdout.flush()
                continue

            # Detectar fin de bloque de exportacion
            if export_collecting and text == f"[EXPORT-{export_fmt.upper()}-END]":
                try:
                    path = save_export_block(export_fmt, export_buf)
                    sys.stdout.write(f"\r\033[K{text}\n")
                    sys.stdout.write(f"\r\033[K[+] Captura guardada: {path}\n")
                    sys.stdout.flush()
                except Exception as exc:
                    sys.stdout.write(f"\r\033[K[!] Error al guardar exportacion: {exc}\n")
                    sys.stdout.flush()
                export_collecting = False
                export_fmt        = None
                export_buf        = []
                continue

            # Acumular lineas de contenido (sin imprimir datos crudos)
            if export_collecting:
                export_buf.append(text)
                continue

            # Prompt dinamico: colorizar y actualizar estado
            m = PROMPT_RE.match(text)
            if m:
                global _device_state
                _device_state = m.group(1)
                sys.stdout.write(f"\r\033[K{_color_prompt(_device_state)}\n")
                sys.stdout.flush()
                continue

            # Prompt legacy (firmware < v2.9): imprimir sin color
            if text == PROMPT_LEGACY.rstrip():
                sys.stdout.write(f"\r\033[K{text}\n")
                sys.stdout.flush()
                continue

            # Linea normal: imprimir
            sys.stdout.write(f"\r\033[K{text}\n")
            sys.stdout.flush()

        except serial.SerialException:
            print("\n[!] Conexión serie perdida.")
            break
        except Exception:
            break


def run_interactive(ser):
    """Bucle interactivo de comandos (comportamiento original v1)."""
    if HAS_READLINE:
        readline.set_history_length(200)
        try:
            readline.read_history_file(".ecrf_history")
        except FileNotFoundError:
            pass

    t = threading.Thread(target=_reader_thread, args=(ser,), daemon=True)
    t.start()

    try:
        while True:
            try:
                cmd = input(_live_prompt()).strip()
            except EOFError:
                break
            if not cmd:
                continue
            if HAS_READLINE:
                readline.write_history_file(".ecrf_history")
            if cmd.lower() in ("exit", "quit", "q"):
                print("[*] Saliendo.")
                break
            ser.write((cmd + "\n").encode("utf-8"))
            time.sleep(0.05)
    except KeyboardInterrupt:
        print("\n[*] Interrupción. Saliendo.")


# ============================================================
# Modo batch — helpers de lectura serie
# ============================================================

def wait_for_response(ser, timeout_s=TIMEOUT_DEFAULT_S):
    """
    Lee la respuesta del dispositivo hasta detectar el prompt "ECRF> "
    o hasta que expire timeout_s.
    Retorna lista de lineas de respuesta (sin el prompt final).
    """
    lines = []
    buf   = ""
    deadline = time.time() + timeout_s

    while time.time() < deadline:
        available = ser.in_waiting
        chunk = ser.read(max(1, available))
        if chunk:
            buf += chunk.decode("utf-8", errors="replace")
            # Extraer lineas completas
            while "\n" in buf:
                nl   = buf.index("\n")
                line = buf[:nl].rstrip("\r")
                buf  = buf[nl + 1:]
                if line:
                    lines.append(line)
            # Detectar prompt sin newline al final (legacy o dinamico)
            prompt_pos = -1
            for pfx in (PROMPT_PREFIX, PROMPT_LEGACY):
                idx = buf.rfind(pfx)
                if idx != -1 and idx > prompt_pos:
                    prompt_pos = idx
            if prompt_pos != -1:
                before = buf[:prompt_pos].strip("\r\n")
                if before:
                    lines.append(before)
                break
        else:
            time.sleep(0.02)

    # Datos residuales (sin el prompt)
    remainder = buf.strip()
    if remainder:
        for piece in remainder.split("\n"):
            piece = piece.rstrip("\r").strip()
            if piece and PROMPT_PREFIX not in piece and PROMPT_LEGACY not in piece:
                lines.append(piece)

    return lines


def wait_passive(ser, seconds, log_fh, start_time):
    """
    Espera 'seconds' segundos leyendo y logueando cualquier dato asíncrono
    recibido (eventos RX, heartbeat de jammer, timeouts, etc.).
    """
    buf      = ""
    deadline = time.time() + seconds
    _log(log_fh, start_time, "WAIT", f"pausando {seconds:.1f} s (datos async seran logueados)")

    while time.time() < deadline:
        available = ser.in_waiting
        if available:
            chunk = ser.read(available)
            buf  += chunk.decode("utf-8", errors="replace")
            while "\n" in buf:
                nl   = buf.index("\n")
                line = buf[:nl].rstrip("\r")
                buf  = buf[nl + 1:]
                if line and PROMPT_PREFIX not in line and PROMPT_LEGACY not in line:
                    _log(log_fh, start_time, "ASYNC", line)
        else:
            time.sleep(0.05)


# ============================================================
# Modo batch — log
# ============================================================

def _ts(start_time):
    """Timestamp relativo MM:SS.mmm desde start_time."""
    elapsed = time.time() - start_time
    mins    = int(elapsed // 60)
    secs    = elapsed % 60
    return f"{mins:02d}:{secs:06.3f}"


def _log(log_fh, start_time, tag, text, print_it=True):
    """Escribe entrada en log y stdout."""
    entry = f"[{_ts(start_time)}] {tag:4s}: {text}"
    if log_fh:
        log_fh.write(entry + "\n")
        log_fh.flush()
    if print_it:
        print(entry)


# ============================================================
# Modo batch — parser de script .ecrf
# ============================================================

def parse_ecrf_script(path):
    """
    Parsea un archivo .ecrf.
    Retorna lista de dicts {"type": ..., "value": ...}:
      type="cmd"     value=str (comando a enviar)
      type="wait"    value=float (segundos de pausa)
      type="delay"   value=float (nuevo delay entre cmds en ms)
      type="timeout" value=float (nuevo timeout en segundos)
      type="comment" value=str (texto del comentario, solo para log)
    """
    instructions = []
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\n\r")
            stripped = line.strip()
            if not stripped:
                continue

            if stripped.startswith("#"):
                content = stripped[1:].strip()
                lower   = content.lower()

                if lower.startswith("delay "):
                    try:
                        val = float(content.split()[1])
                        instructions.append({"type": "delay", "value": val})
                    except (IndexError, ValueError):
                        instructions.append({"type": "comment", "value": content})

                elif lower.startswith("wait "):
                    try:
                        val = float(content.split()[1])
                        instructions.append({"type": "wait", "value": val})
                    except (IndexError, ValueError):
                        instructions.append({"type": "comment", "value": content})

                elif lower.startswith("timeout "):
                    try:
                        val = float(content.split()[1])
                        instructions.append({"type": "timeout", "value": val})
                    except (IndexError, ValueError):
                        instructions.append({"type": "comment", "value": content})

                else:
                    instructions.append({"type": "comment", "value": content})

            else:
                # Comando: quitar comentario inline (dos o más espacios seguidos de #)
                cmd = stripped
                for sep in ("  #", "\t#"):
                    if sep in cmd:
                        cmd = cmd[:cmd.index(sep)]
                cmd = cmd.strip()
                if cmd:
                    instructions.append({"type": "cmd", "value": cmd})

    return instructions


# ============================================================
# Modo batch — ejecucion principal
# ============================================================

def run_batch(ser, script_path, delay_ms=DELAY_DEFAULT_MS,
              response_timeout=TIMEOUT_DEFAULT_S, log_path=None):
    """
    Ejecuta un script .ecrf linea por linea contra el dispositivo.
    Guarda todas las respuestas en log_path.
    Retorna True si se completó sin error fatal.
    """
    # Abrir archivo de log
    log_fh = None
    if log_path:
        log_dir = os.path.dirname(log_path)
        if log_dir and not os.path.exists(log_dir):
            os.makedirs(log_dir, exist_ok=True)
        log_fh = open(log_path, "w", encoding="utf-8")

    start_time = time.time()
    now_str    = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    # Cabecera del log
    header = [
        "=" * 60,
        "ecrf-serial.py — Batch Execution Log",
        f"Script:  {os.path.abspath(script_path)}",
        f"Device:  {ser.port} @ {ser.baudrate} baud",
        f"Started: {now_str}",
        f"Delay:   {delay_ms:.0f} ms entre comandos",
        f"Timeout: {response_timeout:.1f} s por respuesta",
        "=" * 60,
        "",
    ]
    for h in header:
        print(h)
        if log_fh:
            log_fh.write(h + "\n")

    # Parsear script
    try:
        instructions = parse_ecrf_script(script_path)
    except FileNotFoundError:
        _log(log_fh, start_time, "ERR ", f"Script no encontrado: {script_path}")
        if log_fh:
            log_fh.close()
        return False
    except Exception as exc:
        _log(log_fh, start_time, "ERR ", f"Error al parsear script: {exc}")
        if log_fh:
            log_fh.close()
        return False

    n_cmds = sum(1 for i in instructions if i["type"] == "cmd")
    _log(log_fh, start_time, "INFO",
         f"Script cargado: {len(instructions)} instrucciones, {n_cmds} comandos")

    # Limpiar buffer de entrada del puerto
    ser.reset_input_buffer()
    time.sleep(0.2)

    # Estado mutable del batch
    current_delay   = delay_ms
    current_timeout = response_timeout
    cmd_count       = 0
    ok_count        = 0
    err_count       = 0

    # Ejecutar instrucciones
    for instr in instructions:
        itype  = instr["type"]
        ivalue = instr["value"]

        if itype == "comment":
            _log(log_fh, start_time, "NOTE", ivalue)

        elif itype == "delay":
            current_delay = ivalue
            _log(log_fh, start_time, "SET ", f"delay={current_delay:.0f} ms")

        elif itype == "timeout":
            current_timeout = ivalue
            _log(log_fh, start_time, "SET ", f"timeout={current_timeout:.1f} s")

        elif itype == "wait":
            wait_passive(ser, ivalue, log_fh, start_time)

        elif itype == "cmd":
            cmd_count += 1
            _log(log_fh, start_time, "SEND", ivalue)

            # Enviar comando al dispositivo
            ser.write((ivalue + "\n").encode("utf-8"))

            # Leer y loguear respuesta
            resp_lines = wait_for_response(ser, timeout_s=current_timeout)
            for rline in resp_lines:
                _log(log_fh, start_time, "RECV", rline)
                if rline.startswith("OK:") or '"status":"ok"' in rline:
                    ok_count += 1
                elif rline.startswith("ERR:") or '"status":"error"' in rline:
                    err_count += 1

            # Detectar y guardar bloques de exportacion en la respuesta (batch)
            saved_exports = extract_and_save_exports(resp_lines)
            for (efmt, epath) in saved_exports:
                msg = f"Captura exportada: format={efmt} path={epath}"
                _log(log_fh, start_time, "SAVE", msg)

            # Delay antes del siguiente comando
            if current_delay > 0:
                time.sleep(current_delay / 1000.0)

    # Resumen final
    elapsed  = time.time() - start_time
    summary = [
        "",
        "=" * 60,
        f"Batch completado",
        f"  Comandos enviados: {cmd_count}",
        f"  Respuestas OK:     {ok_count}",
        f"  Respuestas ERR:    {err_count}",
        f"  Duracion total:    {elapsed:.2f} s",
        "=" * 60,
    ]
    for s in summary:
        print(s)
        if log_fh:
            log_fh.write(s + "\n")

    if log_fh:
        log_fh.close()
        print(f"\n[*] Log guardado en: {log_path}")

    return True


# ============================================================
# Argumentos CLI
# ============================================================

def build_parser():
    p = argparse.ArgumentParser(
        prog="ecrf-serial.py",
        description="Cliente serial para Evil Crow RF (interactivo o batch)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    # Posicionales para compatibilidad con uso anterior
    p.add_argument("pos_port", nargs="?", default=None,
                   metavar="PORT", help="Puerto serie (ej: /dev/ttyUSB0)")
    p.add_argument("pos_baud", nargs="?", type=int, default=None,
                   metavar="BAUD", help="Baudrate")
    # Flags nombrados
    p.add_argument("--port", "-p", dest="port",
                   help="Puerto serie (tiene prioridad sobre posicional)")
    p.add_argument("--baud", "-b", type=int, default=BAUD_DEFAULT,
                   help=f"Baudrate (defecto: {BAUD_DEFAULT})")
    # Modo batch
    p.add_argument("--script", "-s", metavar="PATH.ecrf",
                   help="Archivo .ecrf para ejecucion batch")
    p.add_argument("--delay", type=float, default=DELAY_DEFAULT_MS,
                   metavar="MS",
                   help=f"Delay entre comandos en ms (defecto: {DELAY_DEFAULT_MS:.0f})")
    p.add_argument("--timeout", type=float, default=TIMEOUT_DEFAULT_S,
                   metavar="S",
                   help=f"Timeout por respuesta en segundos (defecto: {TIMEOUT_DEFAULT_S:.0f})")
    p.add_argument("--log", "-l", metavar="PATH",
                   help="Archivo de log batch (defecto: <script>.log)")
    p.add_argument("--no-log", action="store_true",
                   help="No guardar log en modo batch")
    return p


# ============================================================
# Main
# ============================================================

def main():
    parser = build_parser()
    args   = parser.parse_args()

    # Resolver puerto y baud
    port = args.port or args.pos_port
    baud = args.baud
    if baud == BAUD_DEFAULT and args.pos_baud is not None:
        baud = args.pos_baud
    if not port:
        port = find_port()

    if not port:
        print("[!] No se encontró ningún puerto serie USB.")
        print("    Especifica el puerto: --port /dev/ttyUSB0")
        sys.exit(1)

    # Asegurar directorio de capturas
    _ensure_captures_dir()

    print(f"[*] Conectando a {port} @ {baud} baud...")
    try:
        ser = serial.Serial(port, baud, timeout=0.1)
    except serial.SerialException as exc:
        print(f"[!] Error al abrir {port}: {exc}")
        print("    Permisos: sudo usermod -aG dialout $USER")
        sys.exit(1)

    # Esperar reset/boot del ESP32
    time.sleep(1.5)

    if args.script:
        # ---- Modo batch ----
        script_path = os.path.expanduser(args.script)

        log_path = None
        if not args.no_log:
            if args.log:
                log_path = os.path.expanduser(args.log)
            else:
                # Defecto: mismo directorio que el script, extension .log
                base     = os.path.splitext(script_path)[0]
                log_path = base + ".log"

        print(f"[*] Modo batch: {script_path}")
        if log_path:
            print(f"[*] Log:        {log_path}")
        print()

        ok = run_batch(
            ser,
            script_path,
            delay_ms=args.delay,
            response_timeout=args.timeout,
            log_path=log_path,
        )
        ser.close()
        sys.exit(0 if ok else 1)

    else:
        # ---- Modo interactivo ----
        print(f"[*] Conectado. Escribe 'help' para ver comandos. Ctrl+C para salir.\n")
        try:
            run_interactive(ser)
        finally:
            ser.close()


if __name__ == "__main__":
    main()
