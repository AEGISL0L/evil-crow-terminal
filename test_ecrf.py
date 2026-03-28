#!/usr/bin/env python3
"""
test_ecrf.py — Evil Crow RF CLI Protocol Tester
================================================
Connects to the ESP32 via serial, sends each firmware CLI command, and
verifies the response is a valid protocol reply (OK:/ERR: line or parseable
JSON).  No active RF hardware required — the tests only exercise the serial
protocol layer.

Usage
-----
    python3 test_ecrf.py
    python3 test_ecrf.py --port /dev/ttyUSB0
    python3 test_ecrf.py --port /dev/ttyUSB0 --baud 115200 --timeout 5 -v

Exit codes
----------
    0  — all tests passed
    1  — one or more tests failed (or connection error)

Requirements
------------
    pip install pyserial
"""

import sys
import json
import time
import argparse

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("[FATAL] pyserial not installed.  Run:  pip install pyserial")
    sys.exit(1)

# ─── Constants ────────────────────────────────────────────────────────────────

PROMPT        = b"ECRF> "
DEFAULT_BAUD  = 115200
DEFAULT_TIMEOUT = 5.0

# ANSI colour codes (auto-disabled if stdout is not a tty)
_TTY = sys.stdout.isatty()
GREEN  = "\033[32m" if _TTY else ""
RED    = "\033[31m" if _TTY else ""
YELLOW = "\033[33m" if _TTY else ""
RESET  = "\033[0m"  if _TTY else ""
BOLD   = "\033[1m"  if _TTY else ""

# ─── Test definitions ─────────────────────────────────────────────────────────
# Each entry: (display_name, command_string, validator_key)
#
# Validator keys:
#   "ok_or_json"  — any line starts with OK: OR is valid JSON
#   "ok_or_err"   — any line starts with OK: or ERR: (hardware may be absent)
#   "json_only"   — at least one line is parseable JSON
#   "help_block"  — response contains the help banner header
#   "log_or_json" — [LOG-BEGIN...] block OR JSON/OK: response
#   "any_response"— any non-empty response is a PASS (tests echo + prompt)

TESTS = [
    ("help",      "help",      "help_block"),
    ("status",    "status",    "ok_or_json"),
    ("meminfo",   "meminfo",   "ok_or_json"),
    ("config",    "config",    "ok_or_json"),
    ("freqw 1",   "freqw 1",   "ok_or_err"),
    ("freqw 2",   "freqw 2",   "ok_or_err"),
    ("version",   "version",   "json_only"),   # unknown cmd → JSON error response
    ("log",       "log",       "log_or_json"),
    ("clearlog",  "clearlog",  "json_only"),
]

# ─── Validators ───────────────────────────────────────────────────────────────

def _has_ok(lines):
    return any(l.startswith("OK:") for l in lines)

def _has_err(lines):
    return any(l.startswith("ERR:") for l in lines)

def _has_json(lines):
    """Return True if any line can be parsed as JSON object or array."""
    for raw in lines:
        s = raw.strip()
        if s and s[0] in ('{', '['):
            try:
                json.loads(s)
                return True
            except json.JSONDecodeError:
                pass
    return False

def _has_log_block(lines):
    return any("[LOG-BEGIN" in l for l in lines)

VALIDATORS = {
    "ok_or_json":   lambda lines: (_has_ok(lines) or _has_json(lines),
                                   "OK: or JSON found" if _has_ok(lines) or _has_json(lines)
                                   else "no OK: or JSON line"),
    "ok_or_err":    lambda lines: (_has_ok(lines) or _has_err(lines) or _has_json(lines),
                                   "OK:/ERR:/JSON found" if _has_ok(lines) or _has_err(lines) or _has_json(lines)
                                   else "no OK:/ERR:/JSON line"),
    "json_only":    lambda lines: (_has_json(lines),
                                   "JSON response found" if _has_json(lines)
                                   else "no JSON line in response"),
    "help_block":   lambda lines: (any("Evil Crow RF" in l for l in lines),
                                   "help banner found" if any("Evil Crow RF" in l for l in lines)
                                   else "help banner not found"),
    "log_or_json":  lambda lines: (_has_log_block(lines) or _has_json(lines) or _has_ok(lines),
                                   "[LOG-BEGIN] or JSON/OK: found" if _has_log_block(lines) or _has_json(lines) or _has_ok(lines)
                                   else "no log block, JSON, or OK: line"),
    "any_response": lambda lines: (len(lines) > 0, "response received" if lines else "empty response"),
}

def validate(validator_key, lines):
    """Return (passed: bool, reason: str)."""
    fn = VALIDATORS.get(validator_key)
    if fn is None:
        return False, f"unknown validator '{validator_key}'"
    passed, reason = fn(lines)
    return bool(passed), reason

# ─── Serial helpers ───────────────────────────────────────────────────────────

def autodetect_port():
    """Return the first USB-serial port that looks like a CP210x/CH340/FTDI."""
    keywords = ("cp210", "ch340", "ftdi", "uart", "usb serial", "esp")
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        if any(k in desc or k in hwid for k in keywords):
            return p.device
    # Fallback: first available port
    ports = list(serial.tools.list_ports.comports())
    return ports[0].device if ports else None


def read_response(ser, timeout):
    """
    Read bytes from *ser* until the ECRF> prompt is received or *timeout* expires.
    Returns a list of stripped, non-empty text lines (prompt stripped).
    """
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            buf += chunk
            if PROMPT in buf:
                break
        else:
            time.sleep(0.02)

    # Remove prompt and decode
    text = buf.replace(PROMPT, b"").decode("utf-8", errors="replace")
    lines = [l.strip() for l in text.splitlines() if l.strip()]
    return lines


def flush_prompt(ser, timeout=3.0):
    """
    Send an empty line and wait for ECRF> to ensure a clean slate before
    running the test suite.
    """
    ser.reset_input_buffer()
    ser.write(b"\r\n")
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            buf += chunk
            if PROMPT in buf:
                return True
        else:
            time.sleep(0.05)
    return False  # prompt never arrived (firmware may not be running)

# ─── Test runner ──────────────────────────────────────────────────────────────

def run_tests(port, baud, timeout, verbose):
    print(f"\n{BOLD}=== Evil Crow RF CLI Protocol Tester ==={RESET}")
    print(f"Port    : {port}")
    print(f"Baud    : {baud}")
    print(f"Timeout : {timeout} s")
    print(f"Tests   : {len(TESTS)}\n")

    # ── Open port ──────────────────────────────────────────────────────────
    try:
        ser = serial.Serial(port, baud, timeout=0.1)
    except serial.SerialException as exc:
        print(f"{RED}[FATAL]{RESET} Cannot open {port}: {exc}")
        sys.exit(1)

    time.sleep(0.5)

    # ── Synchronise with firmware ──────────────────────────────────────────
    print("Synchronising with firmware ...", end=" ", flush=True)
    if flush_prompt(ser, timeout=5.0):
        print(f"{GREEN}OK{RESET}")
    else:
        print(f"{YELLOW}WARN — ECRF> prompt not received; continuing anyway{RESET}")

    print()

    # ── Run each test ──────────────────────────────────────────────────────
    results = []

    for display_name, cmd, validator_key in TESTS:
        ser.reset_input_buffer()
        ser.write((cmd + "\r\n").encode())

        lines = read_response(ser, timeout)

        passed, reason = validate(validator_key, lines)
        results.append((display_name, cmd, passed, reason, lines))

        badge = f"{GREEN}PASS{RESET}" if passed else f"{RED}FAIL{RESET}"
        print(f"  [{badge}]  {display_name:<12}  {reason}")

        # Show response lines in verbose mode or on failure
        if verbose or not passed:
            preview = lines[:6]
            for l in preview:
                print(f"             > {l}")
            if len(lines) > 6:
                print(f"             > ... ({len(lines) - 6} more lines)")

        time.sleep(0.3)   # small gap between commands

    ser.close()

    # ── Summary ────────────────────────────────────────────────────────────
    total        = len(results)
    passed_count = sum(1 for *_, p, _, _ in results if p)
    pct          = (100 * passed_count // total) if total else 0

    bar_fill  = "█" * (passed_count * 20 // total) if total else ""
    bar_empty = "░" * (20 - len(bar_fill))
    colour    = GREEN if pct == 100 else (YELLOW if pct >= 50 else RED)

    print(f"\n{'─' * 50}")
    print(f"  {BOLD}Resultado:{RESET}  {colour}{passed_count}/{total}  ({pct}%){RESET}")
    print(f"  {colour}[{bar_fill}{bar_empty}]{RESET}")

    failed = [n for n, _, p, *_ in results if not p]
    if failed:
        print(f"  {RED}Fallidos : {', '.join(failed)}{RESET}")
    else:
        print(f"  {GREEN}Todos los comandos respondieron con protocolo correcto.{RESET}")

    print(f"{'─' * 50}\n")

    return passed_count == total

# ─── Entry point ──────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Evil Crow RF CLI protocol tester — no RF hardware required",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--port", "-p",
        help="Serial port (default: autodetect CP210x/CH340/FTDI)",
    )
    parser.add_argument(
        "--baud", "-b",
        type=int, default=DEFAULT_BAUD,
        help=f"Baud rate (default: {DEFAULT_BAUD})",
    )
    parser.add_argument(
        "--timeout", "-t",
        type=float, default=DEFAULT_TIMEOUT,
        help=f"Response timeout per command in seconds (default: {DEFAULT_TIMEOUT})",
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Show response lines for every command, not only on failure",
    )
    args = parser.parse_args()

    port = args.port or autodetect_port()
    if not port:
        print(f"{RED}[ERROR]{RESET} No serial port found.")
        print("  Connect the Evil Crow RF and retry, or specify --port /dev/ttyUSB0")
        sys.exit(1)

    all_passed = run_tests(port, args.baud, args.timeout, args.verbose)
    sys.exit(0 if all_passed else 1)


if __name__ == "__main__":
    main()
