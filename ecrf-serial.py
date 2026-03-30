#!/usr/bin/env python3
"""
ecrf-serial.py — Cliente terminal para Evil Crow RF via puerto serie.

Modo interactivo:
  python3 ecrf-serial.py [PORT] [BAUD]
  python3 ecrf-serial.py --port /dev/ttyUSB0 --baud 115200

Modo batch (script):
  python3 ecrf-serial.py --script PATH.ecrf [opciones]
  python3 ecrf-serial.py --script ~/evilcrow/scripts/example.ecrf --delay 1000

Modo monitor (--monitor):
  python3 ecrf-serial.py --monitor [PORT]
  python3 ecrf-serial.py --monitor /dev/ttyUSB0
  Arma RX automáticamente (rx 1 433.92 812 0 47.6 4).
  Muestra solo líneas [RX* | OK: RELAY | FOUND: con timestamp.
  Guarda cada señal capturada en ~/evilcrow/captures/<ISO_timestamp>.txt.
  Ctrl+C envía 'stoprx' antes de cerrar.

Modo chat (--chat):
  python3 ecrf-serial.py --chat FREQ MI_ADDR
  python3 ecrf-serial.py --chat /dev/ttyUSB0 FREQ MI_ADDR
  Envía 'chat-start 1 FREQ MI_ADDR' automáticamente.
  Pide dest_addr al inicio del modo.
  Mensajes recibidos [CHAT:X] en azul con timestamp [HH:MM] addr: texto.
  Escribe texto + Enter para enviar al dest_addr configurado.
  Ctrl+C envía 'chat-stop 1' antes de cerrar.

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
_ANSI_RESET    = "\033[0m"
_ANSI_BLUE     = "\033[94m"   # IDLE / mensajes chat recibidos
_ANSI_GREEN    = "\033[92m"   # RX activo / OK chat
_ANSI_RED      = "\033[91m"   # JAM activo / ERR chat
_ANSI_CYAN     = "\033[96m"   # cabeceras info chat

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


# ---- Monitor mode constants ----
# Prefijos de linea que se muestran en modo monitor
MONITOR_PREFIXES  = ("[RX", "OK: RELAY", "FOUND:")
# Marcadores de bloque de captura emitidos por el firmware
_CAP_START        = "[RX-RAW-BEGIN]"
_CAP_RAWEND       = "[RX-RAW-END]"
_ANALYSIS_END_TAG = "[ANALYSIS-END]"
# Comando RX fijo que lanza el monitor
MONITOR_RX_CMD    = "rx 1 433.92 812 0 47.6 4"


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
# Modo monitor
# ============================================================

def run_monitor(ser, port_hint):
    """
    Modo monitor: arma RX automáticamente y muestra sólo las líneas clave.
    Filtra [RX*, OK: RELAY, FOUND: con timestamp HH:MM:SS.mmm.
    Guarda cada señal capturada en ~/evilcrow/captures/<ISO_timestamp>.txt.
    Ctrl+C envía 'stoprx' y cierra.
    """
    _ensure_captures_dir()

    print(f"[*] Monitor activo en {port_hint}")
    print(f"[*] Capturas en:   {CAPTURES_DIR}")
    print(f"[*] Comando:       {MONITOR_RX_CMD}")
    print(f"[*] Filtro:        [RX* | OK: RELAY | FOUND:")
    print(f"[*] Ctrl+C para detener\n")

    # Armar receptor
    ser.write((MONITOR_RX_CMD + "\n").encode("utf-8"))
    time.sleep(0.1)

    # Estado del bloque de captura en curso
    cap_buf      = []    # líneas acumuladas de la captura actual
    in_cap       = False # ¿estamos dentro de un bloque de captura?
    cap_has_raw  = False # ¿ya vimos [RX-RAW-END]?
    cap_start_dt = None  # datetime del inicio de la captura

    def _ts():
        return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]

    def _flush_capture(dt):
        """Guarda el buffer de captura actual en ~/evilcrow/captures/<ISO>.txt."""
        if not cap_buf:
            return
        iso  = dt.strftime("%Y%m%dT%H%M%S")
        path = os.path.join(CAPTURES_DIR, f"{iso}.txt")
        try:
            with open(path, "w", encoding="utf-8") as fout:
                fout.write("\n".join(cap_buf) + "\n")
            print(f"[{_ts()}] [SAVED] {path}")
        except Exception as exc:
            print(f"[{_ts()}] [!] Error guardando captura: {exc}")

    try:
        while True:
            raw = ser.readline()
            if not raw:
                continue
            text = raw.decode("utf-8", errors="replace").rstrip()
            if not text:
                continue

            ts = _ts()

            # --- Seguimiento del bloque de captura ---
            if text == _CAP_START:
                in_cap       = True
                cap_has_raw  = False
                cap_buf      = [text]
                cap_start_dt = datetime.datetime.now()

            elif in_cap:
                cap_buf.append(text)
                if text == _CAP_RAWEND:
                    cap_has_raw = True
                elif text == _ANALYSIS_END_TAG and cap_has_raw:
                    # Captura completa con análisis: guardar
                    _flush_capture(cap_start_dt)
                    in_cap  = False
                    cap_buf = []
                elif PROMPT_RE.match(text) or text == PROMPT_LEGACY.rstrip():
                    # Llegó el prompt antes del análisis: guardar lo que tenemos
                    if cap_has_raw:
                        _flush_capture(cap_start_dt)
                    in_cap  = False
                    cap_buf = []

            # --- Mostrar solo líneas filtradas ---
            if any(text.startswith(p) for p in MONITOR_PREFIXES):
                print(f"[{ts}] {text}")

    except KeyboardInterrupt:
        print(f"\n[{_ts()}] [*] Ctrl+C — enviando stoprx...")
        try:
            ser.write(b"stoprx\n")
            time.sleep(0.5)
            # Vaciar buffer de entrada
            while ser.in_waiting:
                ser.read(ser.in_waiting)
        except Exception:
            pass
        # Guardar captura incompleta si había datos raw
        if in_cap and cap_has_raw and cap_buf:
            _flush_capture(cap_start_dt)


# ============================================================
# Modo chat
# ============================================================

def _chat_reader_thread(ser):
    """
    Hilo lector del modo chat.
    - [CHAT:X] texto  → imprime en azul con timestamp [HH:MM] addr: texto
    - OK: msg ...     → imprime en verde (confirmación de envío)
    - ERR: msg / ERR: chat → imprime en rojo
    - OK: chat-start  → imprime en cyan (confirmación de inicio)
    - OK: chat-stop   → imprime en cyan
    - El resto de líneas (prompts, JSON, etc.) se ignoran.
    """
    while True:
        try:
            raw = ser.readline()
            if not raw:
                continue
            text = raw.decode("utf-8", errors="replace").rstrip()
            if not text:
                continue

            now = datetime.datetime.now().strftime("%H:%M")

            if text.startswith("[CHAT:"):
                # Formato firmware: "[CHAT:42] hola mundo"
                try:
                    bracket_end = text.index("]")
                    from_addr   = text[6:bracket_end]
                    msg_text    = text[bracket_end + 2:]   # skip '] '
                except ValueError:
                    from_addr = "?"
                    msg_text  = text
                sys.stdout.write(
                    f"\r\033[K{_ANSI_BLUE}[{now}] {from_addr}: {msg_text}{_ANSI_RESET}\n"
                )
                sys.stdout.flush()

            elif text.startswith("OK: msg"):
                sys.stdout.write(f"\r\033[K{_ANSI_GREEN}  ✓ {text}{_ANSI_RESET}\n")
                sys.stdout.flush()

            elif text.startswith("ERR: msg") or text.startswith("ERR: chat"):
                sys.stdout.write(f"\r\033[K{_ANSI_RED}  ✗ {text}{_ANSI_RESET}\n")
                sys.stdout.flush()

            elif text.startswith("OK: chat-start") or text.startswith("OK: chat-stop"):
                sys.stdout.write(f"\r\033[K{_ANSI_CYAN}  {text}{_ANSI_RESET}\n")
                sys.stdout.flush()

            # Ignorar prompts, JSON de estado, etc.

        except serial.SerialException:
            sys.stdout.write("\n[!] Conexión serie perdida.\n")
            sys.stdout.flush()
            break
        except Exception:
            break


def run_chat(ser, freq: float, my_addr: int, dest_addr: int):
    """
    Modo chat interactivo sobre CC1101 en modo paquete.
    Envía 'chat-start 1 <freq> <my_addr>' automáticamente.
    El usuario escribe texto y pulsa Enter para enviarlo a dest_addr.
    Ctrl+C envía 'chat-stop 1' y cierra.
    """
    print(f"{_ANSI_CYAN}[*] Iniciando chat — módulo 1, freq={freq} MHz, "
          f"mi addr={my_addr}, dest={dest_addr}{_ANSI_RESET}")
    print(f"{_ANSI_CYAN}[*] Escribe un mensaje y pulsa Enter para enviarlo. "
          f"Ctrl+C para salir.{_ANSI_RESET}")
    print()

    # Lanzar hilo lector antes de enviar chat-start para capturar la respuesta
    t = threading.Thread(target=_chat_reader_thread, args=(ser,), daemon=True)
    t.start()

    # Enviar chat-start
    ser.write(f"chat-start 1 {freq} {my_addr}\n".encode("utf-8"))
    time.sleep(0.4)   # dejar que el hilo imprima la confirmación

    prompt = f"[a {dest_addr}]> "

    try:
        while True:
            try:
                text = input(prompt).strip()
            except EOFError:
                break
            if not text:
                continue
            if text.lower() in ("exit", "quit", "q"):
                break
            # Si el usuario escribe "@<addr> texto", cambiar dest_addr al vuelo
            if text.startswith("@"):
                parts = text.split(None, 1)
                try:
                    new_dest = int(parts[0][1:])
                    dest_addr = new_dest
                    prompt = f"[a {dest_addr}]> "
                    text = parts[1] if len(parts) > 1 else ""
                    if not text:
                        sys.stdout.write(
                            f"\r\033[K{_ANSI_CYAN}  [dest cambiado a {dest_addr}]{_ANSI_RESET}\n"
                        )
                        sys.stdout.flush()
                        continue
                except (ValueError, IndexError):
                    pass
            ser.write(f"msg 1 {dest_addr} {text}\n".encode("utf-8"))
            time.sleep(0.05)
    except KeyboardInterrupt:
        pass
    finally:
        print(f"\n{_ANSI_CYAN}[*] Ctrl+C — enviando chat-stop...{_ANSI_RESET}")
        try:
            ser.write(b"chat-stop 1\n")
            time.sleep(0.3)
            while ser.in_waiting:
                ser.read(ser.in_waiting)
        except Exception:
            pass


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
    # Modo monitor
    p.add_argument("--monitor", nargs="?", const="__auto__", metavar="PORT",
                   help="Modo monitor: arma RX, filtra y guarda capturas automáticamente")
    # Modo chat
    p.add_argument("--chat", nargs="*", metavar="ARG",
                   help="Modo chat: [PORT] FREQ MI_ADDR — chat interactivo CC1101 paquete GFSK")
    return p


# ============================================================
# Main
# ============================================================

def main():
    parser = build_parser()
    args   = parser.parse_args()

    # ---- Resolver modo y argumentos de chat ----
    chat_mode   = args.chat is not None
    chat_freq   = None
    chat_addr   = None
    chat_dest   = None

    if chat_mode:
        chat_args = args.chat or []
        # Separar puerto opcional (empieza por /dev/ o COM) de FREQ y ADDR
        remaining = []
        chat_port_arg = None
        for a in chat_args:
            if a.startswith("/dev/") or a.upper().startswith("COM"):
                chat_port_arg = a
            else:
                remaining.append(a)
        if chat_port_arg:
            args.port = chat_port_arg   # sobreescribe puerto para la conexión
        if len(remaining) >= 2:
            try:
                chat_freq = float(remaining[0])
                chat_addr = int(remaining[1])
            except ValueError as exc:
                print(f"[!] --chat: frecuencia o addr inválidos: {exc}")
                sys.exit(1)
        elif len(remaining) == 1:
            try:
                chat_freq = float(remaining[0])
            except ValueError as exc:
                print(f"[!] --chat: frecuencia inválida: {exc}")
                sys.exit(1)

    # ---- Resolver puerto ----
    # --monitor PORT sobreescribe el puerto si se dio
    monitor_mode = args.monitor is not None
    if monitor_mode and args.monitor != "__auto__":
        # Puerto dado directamente a --monitor
        port = args.monitor
    else:
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

    if chat_mode:
        # ---- Modo chat ----
        # Solicitar frecuencia y/o addr si no se dieron en la línea de comandos
        if chat_freq is None:
            try:
                chat_freq = float(input("[*] Frecuencia MHz (ej: 433.92): ").strip())
            except (ValueError, EOFError):
                print("[!] Frecuencia inválida.")
                ser.close()
                sys.exit(1)
        if chat_addr is None:
            try:
                chat_addr = int(input("[*] Mi dirección (1-254): ").strip())
            except (ValueError, EOFError):
                print("[!] Dirección inválida.")
                ser.close()
                sys.exit(1)
        # Pedir dest_addr al inicio del modo (según especificación)
        try:
            chat_dest = int(input(f"[*] Dirección destino (0-255, 0=broadcast): ").strip())
        except (ValueError, EOFError):
            print("[!] Dirección destino inválida.")
            ser.close()
            sys.exit(1)
        try:
            run_chat(ser, chat_freq, chat_addr, chat_dest)
        finally:
            ser.close()

    elif monitor_mode:
        # ---- Modo monitor ----
        try:
            run_monitor(ser, port)
        finally:
            ser.close()

    elif args.script:
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
