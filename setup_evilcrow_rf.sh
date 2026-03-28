#!/usr/bin/env bash
# =============================================================================
# Evil Crow RF 433 MHz - Setup & Flash Script (100% Terminal)
# Target: ESP32 Dev Module | Firmware: joelsernamoreno/EvilCrow-RF
# =============================================================================
set -euo pipefail

# --- Colores ---
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; NC='\033[0m'; BOLD='\033[1m'

info()    { echo -e "${CYAN}[INFO]${NC} $*"; }
ok()      { echo -e "${GREEN}[OK]${NC}   $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $*"; }
die()     { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }
section() { echo -e "\n${BOLD}${YELLOW}===== $* =====${NC}\n"; }

# --- Rutas base ---
BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARDUINO_CLI="$HOME/bin/arduino-cli"
ARDUINO_DATA="$HOME/.arduino15"
LIBRARIES_DIR="$HOME/Arduino/libraries"
FIRMWARE_DIR="$BASE_DIR/EvilCrow-RF"

# --- Configuración de la placa ---
FQBN="esp32:esp32:esp32:FlashSize=4M,CPUFreq=80,FlashMode=dio,FlashFreq=40"
ESP32_PKG_URL="https://espressif.github.io/arduino-esp32/package_esp32_index.json"
ESP32_CORE_VERSION="3.3.2"

# =============================================================================
section "PASO 1: Dependencias del sistema"
# =============================================================================

info "Instalando dependencias Python y herramientas serie..."
pip3 install --user --break-system-packages esptool pyserial 2>/dev/null || \
pip3 install --user esptool pyserial 2>/dev/null || \
warn "No se pudo instalar esptool via pip, intentando sistema..."

# Asegurar que el usuario tenga acceso al puerto serie
if ! groups | grep -q "dialout\|uucp\|tty"; then
    warn "Tu usuario no está en el grupo 'dialout'. Ejecuta:"
    warn "  sudo usermod -aG dialout $USER"
    warn "  (requiere cerrar sesión y volver a entrar)"
fi

# =============================================================================
section "PASO 2: Instalar arduino-cli"
# =============================================================================

if command -v arduino-cli &>/dev/null; then
    ok "arduino-cli ya instalado: $(arduino-cli version | head -1)"
    ARDUINO_CLI="$(command -v arduino-cli)"
elif [[ -f "$HOME/bin/arduino-cli" ]]; then
    ok "arduino-cli encontrado en $HOME/bin"
else
    info "Descargando arduino-cli..."
    mkdir -p "$HOME/bin"
    curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh \
        | BINDIR="$HOME/bin" sh
    export PATH="$HOME/bin:$PATH"
    ok "arduino-cli instalado en $HOME/bin/arduino-cli"
fi

# Añadir al PATH si no está
if ! command -v arduino-cli &>/dev/null; then
    export PATH="$HOME/bin:$PATH"
    info "Añadido $HOME/bin al PATH para esta sesión"
    info "Añade 'export PATH=\"\$HOME/bin:\$PATH\"' a tu ~/.bashrc para persistir"
fi

# =============================================================================
section "PASO 3: Configurar arduino-cli para ESP32"
# =============================================================================

info "Inicializando configuración..."
arduino-cli config init --overwrite 2>/dev/null || true

info "Añadiendo URL de paquetes ESP32..."
arduino-cli config set board_manager.additional_urls "$ESP32_PKG_URL"

info "Actualizando índice de paquetes..."
arduino-cli core update-index

info "Instalando ESP32 core v${ESP32_CORE_VERSION}..."
arduino-cli core install "esp32:esp32@${ESP32_CORE_VERSION}"
ok "ESP32 core instalado"

# Verificar que la placa aparece
arduino-cli board listall | grep -i "esp32 dev module" && \
    ok "Placa 'ESP32 Dev Module' disponible" || \
    warn "No se encontró 'ESP32 Dev Module' en la lista, pero el FQBN esp32:esp32:esp32 debería funcionar"

# =============================================================================
section "PASO 4: Clonar firmware Evil Crow RF"
# =============================================================================

if [[ -d "$FIRMWARE_DIR/.git" ]]; then
    info "Repositorio ya existe, actualizando..."
    git -C "$FIRMWARE_DIR" pull
else
    info "Clonando EvilCrow-RF..."
    git clone https://github.com/joelsernamoreno/EvilCrow-RF.git "$FIRMWARE_DIR"
fi
ok "Firmware disponible en: $FIRMWARE_DIR"

# =============================================================================
section "PASO 5: Instalar librerías requeridas"
# =============================================================================
# Firmware modo serial-only: solo necesita ELECHOUSE_CC1101_SRC_DRV (incluida
# en el repo) y las librerías estándar del ESP32 core (SPI, LittleFS).
# NO se instalan AsyncTCP, ESPAsyncWebServer ni ElegantOTA.

mkdir -p "$LIBRARIES_DIR"
ok "Sin librerías externas necesarias para el modo terminal"

# Bloque deshabilitado (modo WiFi/web):
if false; then
# --- AsyncTCP ---
if [[ -d "$LIBRARIES_DIR/AsyncTCP" ]]; then
    info "AsyncTCP ya instalada, actualizando..."
    git -C "$LIBRARIES_DIR/AsyncTCP" pull 2>/dev/null || true
else
    info "Clonando AsyncTCP..."
    git clone https://github.com/ESP32Async/AsyncTCP.git "$LIBRARIES_DIR/AsyncTCP"
fi
ok "AsyncTCP lista"

fi  # fin bloque deshabilitado modo WiFi/web

# =============================================================================
section "PASO 6: Compilar el firmware"
# =============================================================================

SKETCH="$FIRMWARE_DIR/EvilCrow-RF/EvilCrow-RF.ino"

if [[ ! -f "$SKETCH" ]]; then
    # Buscar el .ino
    SKETCH=$(find "$FIRMWARE_DIR" -name "*.ino" | head -1)
    [[ -z "$SKETCH" ]] && die "No se encontró el sketch .ino en $FIRMWARE_DIR"
fi

info "Sketch: $SKETCH"
info "FQBN: $FQBN"
info "Compilando (puede tardar varios minutos la primera vez)..."

BUILD_DIR="$BASE_DIR/build"
mkdir -p "$BUILD_DIR"

arduino-cli compile \
    --fqbn "$FQBN" \
    --libraries "$LIBRARIES_DIR" \
    --build-path "$BUILD_DIR" \
    --warnings default \
    "$(dirname "$SKETCH")"

ok "Compilación exitosa"
info "Binario generado en: $BUILD_DIR"
ls -lh "$BUILD_DIR"/*.bin 2>/dev/null || ls -lh "$BUILD_DIR/"

# =============================================================================
section "PASO 7: Detectar puerto y flashear"
# =============================================================================

detect_port() {
    # Buscar puertos USB serie comunes para ESP32
    for port in /dev/ttyUSB0 /dev/ttyUSB1 /dev/ttyACM0 /dev/ttyACM1; do
        [[ -e "$port" ]] && echo "$port" && return
    done
    echo ""
}

PORT=$(detect_port)

if [[ -z "$PORT" ]]; then
    echo ""
    warn "No se detectó ningún puerto serie USB."
    warn "Conecta el Evil Crow RF y ejecuta con el puerto como argumento:"
    warn "  $0 /dev/ttyUSB0"
    echo ""
    info "Para ver puertos disponibles: ls /dev/ttyUSB* /dev/ttyACM*"
    info "El binario compilado está en: $BUILD_DIR"
    info ""
    info "Para flashear manualmente con esptool:"
    BIN=$(find "$BUILD_DIR" -name "*.bin" | grep -v bootloader | grep -v partitions | head -1)
    info "  python3 -m esptool --chip esp32 --port /dev/ttyUSB0 \\"
    info "    --baud 921600 write_flash -z 0x1000 \"$BIN\""
    exit 0
fi

# Si se pasó un puerto como argumento, usarlo
[[ -n "${1:-}" ]] && PORT="$1"

info "Puerto detectado: $PORT"
info "Flasheando el Evil Crow RF..."

arduino-cli upload \
    --fqbn "$FQBN" \
    --port "$PORT" \
    --input-dir "$BUILD_DIR" \
    "$(dirname "$SKETCH")"

ok "Flash completado exitosamente"

# =============================================================================
section "CONEXION WIFI"
# =============================================================================

echo -e "${GREEN}"
echo "  Evil Crow RF flasheado y listo."
echo ""
echo "  Conexión al panel web:"
echo "    SSID:     Evil Crow RF"
echo "    Password: 123456789ECRFv1"
echo "    URL:      http://evilcrow-rf.local/"
echo ""
echo "  Frecuencias CC1101 configurables: 315 MHz / 433 MHz / 868 MHz / 915 MHz"
echo -e "${NC}"
