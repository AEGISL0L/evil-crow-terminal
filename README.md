# Evil Crow RF — Firmware Serial CLI (sin WiFi)

Firmware modificado para controlar el **Evil Crow RF V2** (y V1) íntegramente
desde terminal USB/serie, sin necesidad de WiFi ni panel web.

Basado en el firmware original de
[joelsernamoreno/EvilCrowRF-V2](https://github.com/joelsernamoreno/EvilCrowRF-V2).

**Versión del firmware:** v2.9 | **Cliente Python:** ecrf-serial.py v2.1

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
{"event":"ready","fw":"2.9","msg":"Evil Crow RF listo","hint":"Escribe help"}
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

autodetect <module> <freq>          Detectar modulación y BW automáticamente (v2.7)
  module: 1 | 2
  freq:   MHz  ej: 433.92
  Prueba 4 modulaciones × 3 BW = 12 combinaciones:
    mod: OOK(2) → 2-FSK(0) → 4-FSK(3) → GFSK(1)
    bw:  812 → 406 → 203 kHz
  Emite TRY: mod=X bw=Y por cada combinación probada
  Emite FOUND: mod=X bw=Y rate=Z y JSON autodetect_found al capturar
  Timeout 3 s por combinación (máx ~36 s en total)
  Si encuentra señal: actualiza globals (modulationMode, setrxbw, frequency,
    datarate, tmp_module) — replay/analyze/export quedan listos sin configurar
  Si no encuentra: ERR: autodetect no_signal_found freq=…
  Emite AUTODETECT-LISTO al finalizar con éxito

brute <module> <freq> <bits> <delay_ms>   Transmitir todos los códigos posibles (v2.8)
  module:   1 | 2
  freq:     MHz  ej: 433.92
  bits:     1–24  (máximo 24 = 16 777 216 códigos)   ERR: si bits > 24
  delay_ms: pausa en ms entre transmisiones  (0 = sin pausa)
  Encoding OOK MSB primero:
    bit 1 → 600 µs HIGH + 200 µs LOW
    bit 0 → 200 µs HIGH + 600 µs LOW
  Usa la modulación activa (modulationMode); configura antes con 'load' o 'config'
  Emite cada 1000 códigos: BRUTE: <n>/<total> ultimo=0xHHHHHH
  Emite JSON {"event":"brute_started"} al comenzar
  Emite BRUTE-LISTO al finalizar (completo o detenido con 'stopbrute')

stopbrute                           Detiene un brute en curso
  Escribe 'stopbrute' mientras brute está ejecutándose
  También funciona como comando normal si brute ya terminó (no-op)
```

### Prompt dinámico (v2.9)

El firmware emite un prompt contextual según el estado activo:

| Estado | Prompt | Color en cliente |
|--------|--------|-----------------|
| Reposo | `ECRF[IDLE]> ` | Azul |
| RX activo en módulo 1 a 433.92 MHz | `ECRF[RX:1@433.92]> ` | Verde |
| Jammer activo en módulo 2 | `ECRF[JAM:2]> ` | Rojo |

El cliente `ecrf-serial.py` parsea el prompt dinámico y lo coloriza con ANSI.
El prompt de `input()` refleja el último estado recibido del dispositivo.

---

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

# Autodetectar modulación y BW en 433 MHz (pulsa el mando durante el scan)
ECRF> autodetect 1 433.92

# Autodetectar en 868 MHz con módulo 2
ECRF> autodetect 2 868.35

# Tras FOUND: replay y analyze usan los globals actualizados
ECRF> replay 1
ECRF> analyze 2

# Brute force 12 bits en 433 MHz con 50 ms entre códigos (4096 códigos)
ECRF> brute 1 433.92 12 50

# Brute force 16 bits sin pausa (65536 códigos, ~52 s con 800 µs/código)
ECRF> brute 1 433.92 16 0

# Detener brute mientras está ejecutándose (escribir durante la ejecución)
ECRF> stopbrute

# El prompt cambia según el estado (lo muestra el cliente coloreado)
ECRF[IDLE]> rx 1 433.92 812 2 47.6 4     # tras rx → prompt cambia a RX
ECRF[RX:1@433.92]> stoprx                # tras stoprx → vuelve a IDLE
ECRF[IDLE]> jammer 1 433.92 10           # tras jammer → prompt cambia a JAM
ECRF[JAM:1]> stopjammer                  # tras stopjammer → vuelve a IDLE
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

### Prompt dinámico (`ECRF[...]> `)

El firmware v2.9 emite un prompt con estado embebido en lugar del `ECRF> ` estático.
El cliente lo parsea con `PROMPT_RE = re.compile(r'^ECRF\[([^\]]+)\]> ?$')` y
coloriza según el estado extraído:

```
ECRF[IDLE]>           ← azul   (sin actividad)
ECRF[RX:1@433.92]>   ← verde  (módulo 1 escuchando a 433.92 MHz)
ECRF[JAM:2]>          ← rojo   (módulo 2 en modo jammer)
```

Retrocompatibilidad: si el dispositivo emite el prompt legacy `ECRF> ` (firmware
< v2.9), el cliente lo detecta igualmente y lo muestra sin colorizar.

### Brute force (`brute`, `stopbrute`)

Inicio y progreso:

```
ECRF> brute 1 433.92 12 50
OK: brute module=1 freq=433.92000 bits=12 total=4096 delay_ms=50
{"event":"brute_started"}
BRUTE: 1/4096 ultimo=0x000000
BRUTE: 1000/4096 ultimo=0x0003E7
BRUTE: 2000/4096 ultimo=0x0007CF
BRUTE: 3000/4096 ultimo=0x000BB7
BRUTE: 4000/4096 ultimo=0x000F9F
OK: brute done total=4096
BRUTE-LISTO
```

Detenido con `stopbrute` durante la ejecución:

```
BRUTE: 1000/65536 ultimo=0x0003E7
BRUTE: 2000/65536 ultimo=0x0007CF
stopbrute
OK: brute stopped at=2347/65536
BRUTE-LISTO
```

Error por bits > 24:

```
ECRF> brute 1 433.92 25 10
ERR: brute bits>24 max=24 requested=25
```

**Tiempo estimado** por número de bits (delay_ms=0, 800 µs por código):

| bits | códigos | tiempo aprox. |
|------|---------|---------------|
| 8 | 256 | ~0.2 s |
| 12 | 4 096 | ~3.3 s |
| 16 | 65 536 | ~52 s |
| 20 | 1 048 576 | ~14 min |
| 24 | 16 777 216 | ~3.7 h |

### Autodetect (`autodetect`)

Flujo cuando encuentra señal:

```
ECRF> autodetect 1 433.92
TRY: mod=2 bw=812
TRY: mod=2 bw=406
FOUND: mod=2 bw=406 rate=4
{"event":"autodetect_found","mod":2,"bw":406,"rate":4,"freq":433.92000,"samples":128}
AUTODETECT-LISTO
```

Flujo cuando no hay señal (se agotan las 12 combinaciones):

```
TRY: mod=2 bw=812
TRY: mod=2 bw=406
TRY: mod=2 bw=203
TRY: mod=0 bw=812
TRY: mod=0 bw=406
TRY: mod=0 bw=203
TRY: mod=3 bw=812
TRY: mod=3 bw=406
TRY: mod=3 bw=203
TRY: mod=1 bw=812
TRY: mod=1 bw=406
TRY: mod=1 bw=203
ERR: autodetect no_signal_found freq=433.92000
```

| Campo JSON | Descripción |
|-----------|-------------|
| `mod` | Modulación detectada: 0=2-FSK, 1=GFSK, 2=OOK, 3=4-FSK |
| `bw` | Ancho de banda en kHz: 203, 406 o 812 |
| `rate` | Data rate fijo de prueba: 4 kbps |
| `freq` | Frecuencia configurada (MHz) |
| `samples` | Número de muestras capturadas por el ISR |

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
├── EvilCrow-RF/                     Firmware v2.9 (modificado)
│   └── firmware/
│       ├── firmware.ino             ← Sketch principal v2.9
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
| v2.7 | Comando `autodetect <module> <freq>`: descubrimiento automático de modulación y BW — prueba 4 modulaciones (OOK → 2-FSK → 4-FSK → GFSK) × 3 anchos de banda (812 → 406 → 203 kHz) = 12 combinaciones, timeout 3 s por combo; emite `TRY: mod=X bw=Y` en cada intento, `FOUND: mod=X bw=Y rate=Z` + JSON `autodetect_found` al capturar señal, `AUTODETECT-LISTO` como marcador de finalización; actualiza globals automáticamente para que `replay`/`analyze`/`export` funcionen sin configuración adicional; corrección: `detachInterrupt(rx_pin)` incondicional para evitar interrupt colgado en módulo 2 al capturar con `relayActive=true` |
| v2.8 | Comandos `brute <module> <freq> <bits> <delay_ms>` y `stopbrute`: transmisor de fuerza bruta — genera y transmite secuencialmente todos los 2^bits códigos posibles (máx. 24 bits = 16 777 216 códigos) con encoding OOK MSB primero (bit1=600µs/200µs, bit0=200µs/600µs); progreso cada 1000 códigos con `BRUTE: n/total ultimo=0xHHH`; `stopbrute` detectable durante el loop vía polling Serial en cada pausa inter-código; `yield()` por iteración para compatibilidad con WDT de FreeRTOS; `ERR:` si bits > 24; emite `BRUTE-LISTO` al finalizar (completo o detenido) |
| v2.9 | Prompt contextual dinámico: `printPrompt()` reemplaza el `ECRF> ` estático con `ECRF[IDLE]> `, `ECRF[RX:<mod>@<freq>]> ` o `ECRF[JAM:<mod>]> ` según `raw_rx`/`jammer_tx`/`tmp_module`/`frequency`; 17 puntos de impresión actualizados; en `ecrf-serial.py`: `PROMPT_RE` + `_color_prompt()` parsean y colorean el prompt (azul=IDLE, verde=RX, rojo=JAM), `_device_state` actualizado atómicamente por el hilo lector, `_live_prompt()` para `input()` en modo interactivo; retrocompatibilidad con firmware < v2.9 mediante `PROMPT_LEGACY` |

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
  apenas ocupan el 1 % del espacio total (~1.4 MB en el ESP32 con partición típica)

**`autodetect` devuelve `ERR: autodetect no_signal_found`**
- Asegúrate de pulsar/activar el dispositivo emisor durante el scan (hay hasta ~36 s en total)
- Prueba primero con `rx 1 <freq> 812 2 47.6 4` manualmente para confirmar que hay señal
- Verifica que la frecuencia es correcta; una diferencia de 0.1 MHz puede ser suficiente
  para no capturar en señales de BW estrecho

**`autodetect` se detiene antes de probar todas las combinaciones**
- Comportamiento correcto: para en cuanto captura señal para no sobreescribir la muestra
- Si quieres forzar un BW o modulación específico, usa `rx` directamente

**Tras `autodetect FOUND` el `replay` no funciona**
- Comprueba con `config` que `last_smooth_count` > 0; `replay` usa la señal suavizada,
  no la raw — si `autodetect` capturó menos de `minsample` (30) pulsos, `signalanalyse`
  puede no haber generado el smooth; intenta con señal más fuerte o más cerca del receptor

**`autodetect` con módulo 2 no captura aunque haya señal**
- Verifica que el módulo 2 responde con `config` (campo `cc1101_module2=present`)
- `autodetect` usa `relayActive=true` internamente para suprimir el dual-pin check;
  si el módulo 2 no inicializa (`getCC1101()` falla), el comando aborta con `ERR: cc1101_no_response`

**`brute` no transmite / el receptor no detecta nada**
- Confirma que la modulación activa es OOK (mod=2): usa `load <perfil>` o configura con `rx`
  antes de ejecutar brute; brute usa `modulationMode` global tal como está
- Verifica la frecuencia con un SDR antes de bruteforcear; ±0.1 MHz puede ser suficiente
  para quedar fuera del ancho de banda del receptor objetivo
- El encoding (600/200 µs) es genérico; algunos receptores esperan timings distintos —
  en ese caso usa `tx` con los timings exactos en lugar de `brute`

**`brute 1 433.92 24 0` bloquea la CLI durante ~3.7 horas**
- Comportamiento esperado: brute es síncrono y bloquea `loop()` hasta completar
- Para detenerlo escribe `stopbrute` + Enter en el terminal serie durante la ejecución;
  el firmware lo detecta en el polling Serial del inter-código
- Con `delay_ms=0` el polling se ejecuta igualmente (el bucle de espera corre al menos
  una vez antes de salir), por lo que `stopbrute` funciona incluso sin pausa

**`stopbrute` no detiene el brute inmediatamente**
- El firmware solo comprueba Serial al final de cada código transmitido (en la pausa)
- Con `delay_ms=0` la reacción es prácticamente inmediata (<1 ms de latencia)
- Con `delay_ms=5000` el brute termina el código en curso, espera hasta 5 s leyendo
  Serial, y se detiene en cuanto detecta el `stopbrute`; la respuesta puede tardar
  hasta `delay_ms` ms

**`ERR: brute bits_out_of_range` con bits=24**
- bits=24 es válido (2^24 = 16 777 216 códigos); el error solo salta con bits > 24 o bits < 1
- Comprueba que no hay espacios extra en el comando: `brute 1 433.92 24 0`

**El prompt del cliente siempre muestra `ECRF[IDLE]>` aunque RX esté activo**
- El cliente actualiza el estado al recibir el prompt del firmware; si el terminal
  muestra el prompt de `input()` ANTES de que el firmware envíe el suyo, el estado
  se verá en la SIGUIENTE llamada a `input()` (lag de un comando)
- Si el estado nunca cambia, verifica que el firmware es v2.9+ con `status` →
  `"fw":"2.9"` en la respuesta; firmware anterior emite `ECRF> ` sin estado

**El prompt aparece con texto roto o sin color**
- El terminal no soporta secuencias ANSI; prueba `TERM=xterm-256color python3 ecrf-serial.py`
- En Windows PowerShell ejecuta `[Console]::OutputEncoding = [Text.Encoding]::UTF8`
  y activa ANSI con `Set-ItemProperty HKCU:\Console VirtualTerminalLevel 1`

**El prompt muestra `ECRF[RX:1@433.92]>` pero hay `stoprx` activo**
- El firmware actualiza `raw_rx = "0"` dentro de `processSerialCommand()`, y el prompt
  se emite al FINAL del mismo; si el comando `stoprx` actualiza el estado correctamente
  el prompt que sigue a `stoprx` debe mostrar `ECRF[IDLE]>`; si no es así, verifica
  que no hay otro comando activo rearmando el receptor (relay/bridge)
