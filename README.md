# Evil Crow RF — Firmware Serial CLI (sin WiFi)

Firmware modificado para controlar el **Evil Crow RF V2** (y V1) íntegramente
desde terminal USB/serie, sin necesidad de WiFi ni panel web.

Basado en el firmware original de
[joelsernamoreno/EvilCrowRF-V2](https://github.com/joelsernamoreno/EvilCrowRF-V2).

**Versión del firmware:** v2.6 | **Cliente Python:** ecrf-serial.py v2.1

---

## Diferencias respecto al firmware original

El firmware original abre un AP WiFi y sirve un panel web (HTML/CSS/JS).
Este firmware elimina todo eso y añade un CLI serie completo:

| | Original | Este firmware |
|---|---|---|
| Interfaz | WiFi + web | Puerto serie USB |
| Dependencias externas | AsyncTCP, ESPAsyncWebServer, ElegantOTA | Ninguna |
| Arranque | Bloquea 15 s buscando WiFi | Listo en <1 s |
| Salida RX | Solo a fichero (LittleFS) | Terminal + fichero + export |
| SD card | Necesaria (HTML) | No necesaria |
| Flash ocupada | ~580 KB | ~360 KB |
| RAM dinámica | ~120 KB | ~54 KB |

---

## Hardware soportado

| Modelo | Estado |
|--------|--------|
| Evil Crow RF **V2** (con ranura SD) | **Configuración por defecto** |
| Evil Crow RF **V1** (sin SD) | Requiere cambio de pines — ver sección V1 |

---

## Requisitos

### Software

- `arduino-cli` ≥ 1.4
- ESP32 core `esp32:esp32` v3.3.2
- `esptool` ≥ 5.0 (`pip install esptool`)
- Python 3 + `pyserial` (`pip install pyserial`) — para el cliente serie

### Instalación de herramientas

```bash
# arduino-cli
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh \
    | BINDIR=~/bin sh

# ESP32 core
~/bin/arduino-cli config init
~/bin/arduino-cli config set board_manager.additional_urls \
    "https://espressif.github.io/arduino-esp32/package_esp32_index.json"
~/bin/arduino-cli core update-index
~/bin/arduino-cli core install esp32:esp32@3.3.2

# Python
pip install --user esptool pyserial
```

> **Nota Linux:** si `/tmp` está montado con `noexec` (común en sistemas
> endurecidos), añade `TMPDIR=~/tmp` a los comandos de compilación y flash.
> Compruébalo con: `mount | grep "on /tmp"`

---

## Pinout

### Evil Crow RF V2 — configuración por defecto

```
CC1101 (bus HSPI)
  SCK   → GPIO 14
  MISO  → GPIO 12
  MOSI  → GPIO 13

CC1101 módulo 1
  CS    → GPIO  5
  GDO0  → GPIO  2   (TX)
  GDO2  → GPIO  4   (RX)

CC1101 módulo 2
  CS    → GPIO 27
  GDO0  → GPIO 25   (TX)
  GDO2  → GPIO 26   (RX)

SD card (bus VSPI — no usada por este firmware)
  SCK   → GPIO 18
  MISO  → GPIO 19
  MOSI  → GPIO 23
  CS    → GPIO 22
```

### Evil Crow RF V1 — cambio de pines necesario

En el V1 los CC1101 comparten el bus VSPI con el resto del ESP32 Dev Module.
Hay que editar **3 líneas** en `firmware/firmware.ino`:

**Cambiar esto (V2):**

```cpp
// SPI Pins — CC1101 usa HSPI en V2 (pines 14/12/13)
const int sck_pin  = 14;
const int miso_pin = 12;
const int mosi_pin = 13;
```

**Por esto (V1):**

```cpp
// SPI Pins — CC1101 usa VSPI en V1
const int sck_pin  = 18;
const int miso_pin = 19;
const int mosi_pin = 23;
```

El resto del firmware (pines CC1101, lógica, CLI) es idéntico en ambas versiones.

---

## Compilar

```bash
TMPDIR=~/tmp ~/bin/arduino-cli compile \
  --fqbn "esp32:esp32:esp32:FlashSize=4M,CPUFreq=80,FlashMode=dio,FlashFreq=40" \
  --build-path ~/evilcrow/build \
  firmware/
```

Salida esperada:
```
El Sketch usa 360147 bytes (27%) del espacio de almacenamiento de programa.
Las variables Globales usan 53880 bytes (16%) de la memoria dinámica.
```

---

## Flashear

### 1. Conectar el dispositivo

Conecta el Evil Crow RF por USB. Comprueba el puerto:

```bash
ls /dev/ttyUSB* /dev/ttyACM*
```

Si el ESP32 no entra en modo flash automáticamente, mantén pulsado el botón
**BOOT** mientras conectas el cable USB y suéltalo cuando aparezca el puerto.

### 2. Permisos de puerto serie (solo la primera vez)

```bash
sudo usermod -aG dialout $USER
# Cierra sesión y vuelve a entrar para que surta efecto
```

### 3. Flash con arduino-cli

```bash
TMPDIR=~/tmp ~/bin/arduino-cli upload \
  --fqbn "esp32:esp32:esp32:FlashSize=4M,CPUFreq=80,FlashMode=dio,FlashFreq=40" \
  --port /dev/ttyUSB0 \
  --input-dir ~/evilcrow/build \
  firmware/
```

### 4. Flash alternativo con esptool (más rápido)

```bash
python3 -m esptool \
  --chip esp32 \
  --port /dev/ttyUSB0 \
  --baud 921600 \
  write_flash -z 0x1000 \
  ~/evilcrow/build/firmware.ino.bin
```

---

## Uso — cliente serie

### Modo interactivo (recomendado)

```bash
# Autodetecta el puerto
python3 ecrf-serial.py

# Especificando puerto
python3 ecrf-serial.py --port /dev/ttyUSB0

# Compatible con uso posicional anterior
python3 ecrf-serial.py /dev/ttyUSB0 115200
```

Incluye historial de comandos con las flechas arriba/abajo y eco de caracteres.

### Modo batch — scripts `.ecrf`

Ejecuta secuencias de comandos desatendidas con log automático:

```bash
# Ejecutar script con delay de 1 s entre comandos
python3 ecrf-serial.py --script ~/evilcrow/scripts/example.ecrf

# Con opciones
python3 ecrf-serial.py --script mi_script.ecrf \
    --port /dev/ttyUSB0 \
    --delay 1500 \
    --timeout 10 \
    --log ~/resultados/captura.log
```

**Opciones batch:**

| Opción | Defecto | Descripción |
|--------|---------|-------------|
| `--delay MS` | 500 | Delay entre comandos (ms) |
| `--timeout S` | 5 | Timeout de respuesta por comando (s) |
| `--log PATH` | `<script>.log` | Archivo de log |
| `--no-log` | — | No guardar log |

**Formato de archivo `.ecrf`:**

```bash
# comentario (ignorado)
# delay 1000        fija delay entre comandos (ms)
# wait 5.0          pausa sin enviar comando (para captura RF)
# timeout 10        cambia timeout para el siguiente comando
rx 1 433.92 812 0 47.6 4   # comentario inline ignorado
stoprx
log
```

El log se guarda con timestamps relativos `[MM:SS.mmm]` en cada entrada:

```
[00:01.000] SEND: rx 1 433.92 812 0 47.6 4
[00:01.045] RECV: {"event":"rx_armed","msg":"Receptor rearmado - esperando senal"}
[00:01.045] RECV: {"status":"ok","cmd":"rx","module":1,"freq":433.92,...}
[00:06.100] SEND: stoprx
[00:06.145] RECV: {"status":"ok","cmd":"stoprx","msg":"RX detenido"}
```

### Con cualquier terminal serie

```bash
picocom -b 115200 /dev/ttyUSB0
screen /dev/ttyUSB0 115200
minicom -b 115200 -D /dev/ttyUSB0
```

Al conectar verás:

```
{"event":"ready","fw":"2.6","msg":"Evil Crow RF listo","hint":"Escribe help"}
ECRF>
```

---

## Comandos CLI

### Comandos principales

```
help
status

rx <module> <freq> <bw> <modulation> <deviation> <datarate>
  module:     1 | 2
  freq:       MHz         ej: 433.92
  bw:         kHz         ej: 812
  modulation: 0=2-FSK  1=GFSK  2=ASK/OOK  3=4-FSK  4=MSK
  deviation:  kHz         ej: 47.6
  datarate:   kbps        ej: 4

stoprx

tx <module> <freq> <modulation> <deviation> <rawdata>
  rawdata: timings separados por coma  ej: 300,900,300,900
  máximo 2000 valores

jammer <module> <freq> <power_dBm>
stopjammer

log        — Vuelca señales capturadas desde flash
clearlog   — Borra el log
reboot
```

### Comandos extendidos (prefijo OK: / ERR:)

```
freqw <module>                      Frecuencia y config activa del módulo
raw <module>                        Último raw capturado (bloque [RAW-BEGIN/END])
config                              Dump completo: variables + presencia CC1101
scan <module> <start> <end> <step>  Escaneo RSSI frecuencia a frecuencia
  frecuencias en MHz, máximo 500 pasos
  ej: scan 1 433.0 435.0 0.1
replay <module>                     Retransmitir última señal capturada
save <name>                         Guardar configuración en LittleFS (/cfg_<name>.cfg)
load <name>                         Cargar configuración desde LittleFS
  name: alfanumérico, guiones y guiones bajos, máximo 20 caracteres

export <name> <format>              Exportar última señal capturada
  name:   last  (última captura en RAM)
  format: urh   → pulsos +300 -900... en texto  [EXPORT-URH-BEGIN/END]
          rtl   → JSON compatible rtl_433        [EXPORT-RTL-BEGIN/END]

analyze <modulation>                Re-analizar última señal sin nueva captura
  modulation: 0=2-FSK  1=GFSK  2=ASK/OOK  3=4-FSK  4=MSK
  Emite todos los bloques de análisis más el JSON extendido bajo clave "analysis"
  Útil para re-interpretar con otra modulación o volver a obtener el informe

debug <on|off>                      Traza SPI de operaciones CC1101
  Cada operación emite líneas DBG: con la función llamada y el registro leído
  Útil para diagnosticar módulos defectuosos o comportamiento inesperado del CC1101
  Nota: scan con debug activo emite una línea por paso — puede ser muy verbose

registers <module>                  Vuelca los 47 registros de configuración CC1101
  Formato: REG[0xNN]=0xVV NOMBRE   (registros 0x00–0x2E)
  Compara con los valores esperados para detectar módulos mal inicializados

meminfo                             Reporte de uso de memoria en tiempo real
  heap_free, heap_min_ever          Heap libre actual y mínimo histórico desde boot
  heap_largest_block, heap_frag%    Mayor bloque contiguo y % de fragmentación
  stack_hwm                         Mínimo stack libre del task principal (FreeRTOS)
  fs_total, fs_used, fs_free        Uso de LittleFS en bytes
  fs_files                          Número de archivos en LittleFS

relay <freq> <bw> <modulation>      Relay dual-radio: Módulo 1=RX, Módulo 2=TX
  Todo lo capturado por mod1 se retransmite inmediatamente por mod2
  Misma frecuencia en ambos módulos
  Emite: OK: RELAY <pulsos>  por cada paquete relayado
  Se detiene con 'stoprx'

bridge <rx_freq> <tx_freq>          Relay entre dos frecuencias distintas
  Módulo 1 escucha a rx_freq, Módulo 2 retransmite a tx_freq
  Usa mod/bw/dev/rate actuales (configura primero con 'load' o 'config')
  Se detiene con 'stoprx'

list                                Lista todos los archivos en LittleFS
  Formato: <name> size=N date=ISO  encerrado en [LIST-BEGIN] … [LIST-END count=N]
  La fecha es UTC; sin RTC/NTP puede mostrar 1970-01-01

show <name>                         Vuelca el contenido raw de un archivo al Serial
  Encerrado en [FILE-BEGIN path=X size=N] … [FILE-END]
  name sin / inicial se completa automáticamente con /

delete <name>                       Elimina un archivo de LittleFS
  ERR si el archivo no existe

rename <old> <new>                  Renombra un archivo en LittleFS
  ERR si src no existe o dst ya existe

info                                Espacio total, usado y libre de LittleFS
  Emite OK: info total=… used=… free=… used_pct=… files=N
  y JSON {"event":"fs_info",…}

save <name>                         (mejorado en v2.6)
  Avisa con OK: save overwriting existing path=… antes de sobreescribir
```

El cliente Python detecta automáticamente los bloques `[EXPORT-*-BEGIN/END]`
y guarda el archivo en `~/evilcrow/captures/<timestamp>.<ext>`.

### Ejemplos rápidos

```
# Escuchar mandos de garaje a 433 MHz (ASK/OOK)
ECRF> rx 1 433.92 812 2 47.6 4

# Ver última señal capturada raw
ECRF> raw 1

# Retransmitir lo capturado
ECRF> replay 1

# Exportar señal en formato URH (guardado automático en captures/)
ECRF> export last urh

# Exportar señal en formato rtl_433 JSON
ECRF> export last rtl

# Re-analizar última señal asumiendo ASK/OOK (mod=2)
ECRF> analyze 2

# Re-analizar la misma señal asumiendo 2-FSK (mod=0)
ECRF> analyze 0

# Activar traza SPI para diagnosticar módulo
ECRF> debug on
ECRF> rx 1 433.92 812 2 47.6 4    # verás DBG: por cada operación CC1101
ECRF> debug off

# Ver todos los registros del módulo 1
ECRF> registers 1

# Escanear de 433 a 435 MHz en pasos de 0.1
ECRF> scan 1 433.0 435.0 0.1

# Guardar configuración actual
ECRF> save garaje433

# Cargar en otra sesión
ECRF> load garaje433

# Jammer en 433 MHz con potencia máxima
ECRF> jammer 1 433.92 10
ECRF> stopjammer

# Ver uso de memoria y estado del sistema de archivos
ECRF> meminfo

# Relay: captura en mod1 y retransmite por mod2 (433 MHz OOK)
ECRF> relay 433.92 812 2

# Bridge: escucha en 433 MHz y retransmite en 868 MHz
ECRF> bridge 433.92 868.35

# Detener relay o bridge
ECRF> stoprx

# Listar archivos guardados en LittleFS
ECRF> list

# Ver contenido de un archivo de configuración
ECRF> show cfg_garaje433.cfg

# Borrar un archivo
ECRF> delete cfg_garaje433.cfg

# Renombrar un archivo
ECRF> rename cfg_garaje433.cfg cfg_garaje_nuevo.cfg

# Ver espacio libre en flash
ECRF> info

# Guardar config — avisa si ya existe
ECRF> save garaje433
```

---

## Formato de salida serie

El firmware emite respuestas en tres formatos complementarios:

### JSON por línea (comandos principales)

```json
{"status":"ok","cmd":"rx","module":1,"freq":433.92,"bw":812.0,"modulation":0,"timeout_ms":60000}
{"status":"error","cmd":"rx","msg":"module debe ser 1 o 2, recibido: 3"}
{"event":"rx_captured","msg":"Senal capturada - analizando"}
{"event":"jammer_heartbeat","freq":433.92,"power_dbm":10,"uptime_ms":42000}
{"event":"rx_timeout","timeout_ms":60000,"msg":"Sin senal. RX sigue activo."}
```

### JSON extendido de análisis (emitido por `signalanalyse` y `analyze`)

Tras cada captura y en cada llamada a `analyze`, el firmware emite un JSON bajo
la clave `"analysis"` con métricas calculadas directamente sobre los pulsos crudos:

```json
{"event":"analysis","analysis":{
  "freq": 433.92000,
  "modulation_inferred": "OOK",
  "frame_duration_us": 45600,
  "pulse_count": 128,
  "pulse_min_us": 290,
  "pulse_max_us": 950,
  "pattern": "fixed"
}}
```

| Campo | Descripción |
|-------|-------------|
| `freq` | Frecuencia configurada en el momento de la captura (MHz) |
| `modulation_inferred` | `"OOK"` si `pulse_max > 3 × pulse_min`; `"FSK"` en caso contrario |
| `frame_duration_us` | Suma de todos los intervalos crudos (µs) |
| `pulse_count` | Número de intervalos capturados |
| `pulse_min_us` | Intervalo más corto capturado (µs) |
| `pulse_max_us` | Intervalo más largo capturado (µs) |
| `pattern` | `"fixed"` si primera y segunda mitad de bits coinciden ≥75%; `"rolling"` si no |

### OK: / ERR: prefijado (comandos extendidos)

```
OK: freqw module=1 freq=433.92000 bw=812.000 mod=0 dev=47.600 rate=4
OK: scan freq=433.100 rssi=-88
OK: export format=urh samples=450 freq=433.92000
OK: analyze samples=128 mod=2 freq=433.92000
OK: debug on
OK: registers module=1 count=47
OK: relay freq=433.92000 bw=812.000 mod=2 rx=mod1 tx=mod2
{"event":"relay_armed","freq":433.92000,"bw":812.000,"mod":2,"rx":"mod1","tx":"mod2"}
OK: RELAY 128
OK: bridge rx_freq=433.92000 tx_freq=868.35000 mod=2
{"event":"bridge_armed","rx_freq":433.92000,"tx_freq":868.35000,"mod":2,"rx":"mod1","tx":"mod2"}
OK: meminfo
OK: heap_free=180432 bytes
OK: heap_min_ever=162048 bytes
OK: heap_largest_block=163840 bytes
OK: heap_frag=9%
OK: stack_hwm=5248 bytes
OK: fs_total=1441792 bytes
OK: fs_used=2048 bytes
OK: fs_free=1439744 bytes
OK: fs_files=1
ERR: export no_signal_in_memory - capture a signal with 'rx' first
ERR: analyze no_captured_signal - capture a signal with 'rx' first
```

### Eventos relay y bridge

```
OK: relay freq=433.92000 bw=812.000 mod=2 rx=mod1 tx=mod2
{"event":"relay_armed","freq":433.92000,"bw":812.000,"mod":2,"rx":"mod1","tx":"mod2"}

# Por cada paquete retransmitido:
OK: RELAY 128
```

```
OK: bridge rx_freq=433.92000 tx_freq=868.35000 mod=2
{"event":"bridge_armed","rx_freq":433.92000,"tx_freq":868.35000,"mod":2,"rx":"mod1","tx":"mod2"}
```

El campo `relay` aparece también en el evento `status`:
```json
{"event":"status","rx":true,"relay":true,"jammer":false,"heap":178000,"uptime_s":42}
```

### Evento `meminfo` JSON (también emitido por `meminfo`)

```json
{"event":"meminfo","heap_free":180432,"heap_min_ever":162048,"heap_largest_block":163840,"heap_frag_pct":9,"stack_hwm_bytes":5248,"fs_total":1441792,"fs_used":2048,"fs_files":1}
```

| Campo | Fuente | Descripción |
|-------|--------|-------------|
| `heap_free` | `ESP.getFreeHeap()` | Heap libre en el momento de la consulta |
| `heap_min_ever` | `ESP.getMinFreeHeap()` | Mínimo histórico desde el boot |
| `heap_largest_block` | `ESP.getMaxAllocHeap()` | Bloque contiguo más grande disponible |
| `heap_frag_pct` | `100*(free-largest)/free` | Porcentaje de fragmentación del heap |
| `stack_hwm_bytes` | `uxTaskGetStackHighWaterMark()*4` | Mínimo stack libre del task principal |
| `fs_total` / `fs_used` | `LittleFS.*Bytes()` | Tamaño y uso del sistema de archivos |
| `fs_files` | `openNextFile()` loop | Número de archivos en LittleFS |

### Traza DBG: (con `debug on`)

Cuando el modo debug está activo, cada operación CC1101 emite antes y después de ejecutarse:

```
DBG: >> Init()
DBG:   REG[0x0D]=0x10 (FREQ2)
DBG:   REG[0x0E]=0xA7 (FREQ1)
DBG:   REG[0x0F]=0xAE (FREQ0)
DBG:   REG[0x10]=0xC6 (MDMCFG4)
DBG:   REG[0x12]=0x06 (MDMCFG2)
DBG:   REG[0x08]=0x00 (PKTCTRL0)
DBG: >> setMHZ(433.92000)
DBG:   REG[0x0D]=0x10 (FREQ2)
DBG:   REG[0x0E]=0xA7 (FREQ1)
DBG:   REG[0x0F]=0xAE (FREQ0)
DBG: >> SetRx() [strobe SRX=0x34]
DBG:   STATUS[0x35]=0x0D (MARCSTATE)
```

Operaciones trazadas: `Init`, `setMHZ`, `setModulation`, `setRxBW`, `setDRate`,
`setDeviation`, `setSyncMode`, `setPktFormat`, `setDcFilterOff`, `setPA`,
`SetRx`, `SetTx`, `setSidle`.

### Storage management (`list`, `show`, `delete`, `rename`, `info`)

```
[LIST-BEGIN]
/logs.txt size=4096 date=2026-03-30T12:00:00
/cfg_garaje433.cfg size=82 date=2026-03-30T11:45:00
[LIST-END count=2]
OK: list files=2
```

```
[FILE-BEGIN path=/cfg_garaje433.cfg size=82]
freq=433.920000
bw=812.000
mod=2
dev=47.600
rate=4
power=10
module=1

[FILE-END]
OK: show path=/cfg_garaje433.cfg size=82
```

```
OK: deleted path=/cfg_garaje433.cfg
OK: renamed /cfg_garaje433.cfg -> /cfg_garaje_nuevo.cfg
OK: save overwriting existing path=/cfg_garaje433.cfg
OK: info total=1441792 used=4096 free=1437696 used_pct=0 files=2
{"event":"fs_info","total":1441792,"used":4096,"free":1437696,"used_pct":0,"files":2}
```

### Volcado de registros (`registers`)

```
OK: registers module=1 count=47
REG[0x00]=0x29 IOCFG2
REG[0x01]=0x2E IOCFG1
REG[0x02]=0x3F IOCFG0
REG[0x0D]=0x10 FREQ2
REG[0x0E]=0xA7 FREQ1
REG[0x0F]=0xAE FREQ0
REG[0x10]=0xC6 MDMCFG4
REG[0x11]=0x83 MDMCFG3
REG[0x12]=0x06 MDMCFG2
...
REG[0x2E]=0x09 TEST0
OK: registers done
```

### Bloques etiquetados (datos RX y exportación)

```
[RX-RAW-BEGIN]
Count=450
300,900,300,900,...
[RX-RAW-END]

[ANALYSIS-BEGIN]
BITS=010101...
SAMPLES_PER_SYMBOL=300
PAUSES=[{"pos":128,"us":9600}]
[CORRECTED-BEGIN]
Count=44
300,900,...
[CORRECTED-END]
[ANALYSIS-END]
{"event":"rx_analysis","samples_per_symbol":300,"bits":"010101...","pauses":[...],...}
{"event":"analysis","analysis":{"freq":433.92000,"modulation_inferred":"OOK",...}}

[EXPORT-URH-BEGIN]
# Evil-Crow-RF export format=URH
# freq_mhz=433.92000
+300 -900 +300 -900 ...
[EXPORT-URH-END]

[LOG-BEGIN size=4096]
...contenido del log...
[LOG-END]
```

---

## Exportación de señales

El comando `export last <format>` emite la última señal capturada (guardada en RAM).
El cliente Python la detecta y guarda automáticamente:

| Formato | Extensión | Uso |
|---------|-----------|-----|
| `urh` | `.urh.txt` | Importar en Universal Radio Hacker |
| `rtl` | `.json` | Compatible con rtl_433 analyze output |

Los archivos se guardan en `~/evilcrow/captures/` con nombre `YYYYMMDD_HHMMSS.<ext>`.

```bash
# Capturar y exportar (flujo completo)
ECRF> rx 1 433.92 812 0 47.6 4
# (esperar señal)
ECRF> stoprx
ECRF> export last urh
# [+] Captura guardada: /home/user/evilcrow/captures/20260328_153000.urh.txt
ECRF> export last rtl
# [+] Captura guardada: /home/user/evilcrow/captures/20260328_153001.json
```

---

## Scripts de automatización

Los archivos `.ecrf` permiten secuencias repetibles:

```bash
scripts/
└── example.ecrf     # rx + stoprx + log (secuencia de ejemplo)
```

Crear un script personalizado:

```bash
cat > ~/evilcrow/scripts/captura433.ecrf << 'EOF'
# delay 1000
status
rx 1 433.92 812 0 47.6 4
# wait 10
stoprx
analyze 0
export last urh
export last rtl
log
EOF

python3 ecrf-serial.py --script ~/evilcrow/scripts/captura433.ecrf
```

---

## Frecuencias soportadas (CC1101)

| Banda | Rango |
|-------|-------|
| Baja | 300 – 348 MHz |
| Media | 387 – 464 MHz |
| Alta | 779 – 928 MHz |

Frecuencias más habituales: `315.00`, `433.92`, `868.35`, `915.00`

---

## Estructura de ficheros

```
evilcrow/
├── README.md                        Este fichero
├── GAPS.md                          Auditoría de 20 gaps serie (todos resueltos)
├── ecrf-serial.py                   Cliente Python: interactivo + batch + export
├── setup_evilcrow_rf.sh             Script de instalación y build
├── scripts/
│   └── example.ecrf                 Script de ejemplo: rx + stoprx + log
├── captures/                        Señales exportadas (export last urh/rtl)
│   └── YYYYMMDD_HHMMSS.urh.txt
│   └── YYYYMMDD_HHMMSS.json
├── build/
│   ├── firmware.ino.bin             Binario listo para flashear
│   ├── firmware.ino.bootloader.bin
│   └── firmware.ino.partitions.bin
├── EvilCrow-RF/                     Firmware v2.6 (modificado)
│   └── firmware/
│       ├── firmware.ino             ← Sketch principal v2.6
│       ├── ELECHOUSE_CC1101_SRC_DRV.h
│       └── ELECHOUSE_CC1101_SRC_DRV.cpp
└── EvilCrowRF-V2/                   Repo V2 original (referencia)
    └── firmware/
        ├── firmware/firmware.ino    Firmware V2 original (WiFi/web)
        └── SD/HTML/                 Ficheros web para SD (no necesarios)
```

---

## Historial de versiones del firmware

| Versión | Cambios |
|---------|---------|
| v1.0 | CLI serie básico, sin WiFi |
| v2.0 | 20 gaps serie resueltos: respuestas JSON, feedback de errores, watchdog RX/jammer, eco de caracteres, validación de parámetros |
| v2.1 | Nuevos comandos: `freqw`, `raw`, `config`, `scan`, `replay`, `save`, `load` |
| v2.2 | Comando `export` (URH/rtl_433 con bloques etiquetados); `signalanalyse()` extendido con JSON `analysis` (freq, modulación inferida, duración, pulsos min/max, patrón rolling/fixed); comando `analyze <mod>` para re-análisis sin nueva captura |
| v2.3 | Comando `debug on/off`: traza SPI de todas las operaciones CC1101 con registro enviado y valor leído de vuelta, prefijo `DBG:`; comando `registers <mod>`: volcado de los 47 registros de configuración CC1101 en formato `REG[0xNN]=0xVV NOMBRE` |
| v2.4 | Comando `meminfo`: reporte en tiempo real de heap libre, mínimo histórico, fragmentación, stack HWM y uso de LittleFS; optimizaciones internas de heap: `String` con `+=` en loops reemplazadas por `static char[]`/`snprintf`/`File::print()` directo, eliminando hasta 2000 allocaciones de heap por captura |
| v2.5 | Comandos `relay` y `bridge`: modo dual-radio — Módulo 1 como receptor, Módulo 2 como transmisor simultáneo; `relay` opera en la misma frecuencia, `bridge` entre dos frecuencias distintas; cada paquete retransmitido emite `OK: RELAY <pulsos>`; ambos modos se detienen con `stoprx`; campo `relay` añadido al evento `status` |
| v2.6 | Gestión completa de archivos LittleFS: `list` (nombre + tamaño + fecha ISO-8601 en bloque `[LIST-BEGIN/END]`), `show <name>` (volcado raw entre `[FILE-BEGIN/END]`), `delete <name>`, `rename <old> <new>` (ERR si dst existe), `info` (espacio total/used/free/pct + JSON `fs_info`); `save <name>` mejorado: avisa con `OK: save overwriting` antes de sobreescribir; `#include <time.h>` para formato de fecha en `list` |

El detalle de los 20 gaps resueltos y las optimizaciones de memoria están en [`GAPS.md`](GAPS.md).

---

## Solución de problemas

**`libz.so.1: failed to map segment`** al compilar o flashear
```bash
mkdir -p ~/tmp
TMPDIR=~/tmp ~/bin/arduino-cli compile ...
```

**Puerto serie no aparece**
```bash
lsusb | grep -i "cp210\|ch340\|ftdi"
dmesg | tail -10
```

**Permiso denegado en /dev/ttyUSB0**
```bash
sudo usermod -aG dialout $USER
# Cerrar sesión y volver a entrar
```

**ESP32 no entra en modo flash**
- Mantener pulsado **BOOT**, conectar USB, soltar **BOOT**
- Algunos clones de ESP32 requieren además pulsar **RESET** una vez conectado

**No aparece `ECRF>` al conectar**
- Verificar baud rate: **115200**
- El firmware original usa 38400 — este usa 115200

**`export last urh` devuelve `ERR: no_signal_in_memory`**
- Captura una señal primero con `rx`, espera a que llegue, luego `stoprx`
- La señal se mantiene en RAM hasta el próximo `rx` o reinicio

**El scan bloquea la CLI durante su ejecución**
- Es normal: el scan es síncrono (máx. 500 pasos × ~12 ms = ~6 s)
- El RX activo se pausa y se reactiva automáticamente al terminar el scan

**`analyze <mod>` devuelve `ERR: no_captured_signal`**
- Se necesita al menos una captura previa con `rx` (la señal persiste en RAM hasta reinicio)
- Verifica con `config` que `last_raw_count` es mayor que 0

**`modulation_inferred` siempre muestra `FSK` aunque sea OOK**
- El umbral `pulse_max > 3 × pulse_min` asume señales PWM/ASK clásicas
- En señales con sincronización larga, el pulso de sincronía puede elevar `pulse_max`
  y hacer que el ratio no se cumpla; usa `analyze 2` para forzar ASK/OOK en el análisis

**`registers <mod>` devuelve todos los registros a 0x00 o 0xFF**
- El módulo CC1101 no responde al SPI — verifica soldadura del CS, SCK, MISO, MOSI
- Confirma presencia con `config` (mostrará `cc1101_module1=absent`)
- Con `debug on` + `rx 1 ...` verás si `Init()` lee valores coherentes

**El terminal se llena de líneas `DBG:` inesperadamente**
- Desactiva el modo debug con `debug off`
- Si lo activaste desde un script `.ecrf`, añade `debug off` al final del script

**`debug on` durante `scan` genera salida muy voluminosa**
- Cada paso del scan emite 4 líneas DBG (setMHZ + 3 FREQ regs) + 2 líneas SetRx
- Para un scan de 100 pasos: ~600 líneas; reduce el rango o aumenta el step

**`meminfo` muestra `heap_frag` alto (>30%) tras muchas capturas**
- Es normal si el firmware versión < v2.4 estuvo acumulando `String` en heap durante RX
- En v2.4 los loops de `printReceived()` y `signalanalyse()` ya no usan `String`,
  por lo que la fragmentación post-captura se reduce significativamente
- Usa `reboot` para resetear el mínimo histórico (`heap_min_ever`) si quieres una
  nueva línea base limpia

**`meminfo` muestra `fs=unavailable`**
- LittleFS no pudo montarse — prueba `reboot` para que `setup()` intente formatearlo
- Si persiste, el flash puede estar dañado; reflashea el firmware completo

**`list` muestra fechas `1970-01-01T00:00:00`**
- El ESP32 no tiene RTC ni NTP en este firmware, por lo que `getLastWrite()` devuelve
  epoch (0) al crear archivos; la fecha no es fiable sin fuente de tiempo externa

**`rename` devuelve `ERR: rename dst_exists`**
- El archivo destino ya existe — bórralo primero con `delete <new>` o elige otro nombre

**`show` o `delete` devuelven `ERR: file_not_found`**
- Usa `list` para ver los nombres exactos (con ruta completa como `/cfg_name.cfg`)
- Los nombres sin `/` inicial se completan automáticamente; los nombres con extensión
  incorrecta no coincidirán

**`info` muestra `used_pct=0` aunque haya archivos**
- Es correcto: LittleFS reserva bloques en potencias de 2; unos pocos archivos pequeños
  apenas ocupan el 1 % del espacio total (~1.4 MB en el ESP32 con partición típica
