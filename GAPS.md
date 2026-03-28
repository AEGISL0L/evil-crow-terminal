# Evil Crow RF — Auditoría de Gaps para Uso por Terminal Serie

Archivo auditado: `EvilCrow-RF/firmware/firmware.ino`
Fecha de auditoría: 2026-03-28
Fecha de implementación: 2026-03-28 — firmware v2.0 → v2.4

---

## Estado de implementación

Todos los gaps han sido corregidos en `firmware.ino` v2.0.

| # | Estado | Descripción breve | Línea original | Severidad |
|---|---|---|---|---|
| GAP-01 | ✅ DONE | `appendFile`: fallo silencioso de apertura | 64 | Alta |
| GAP-02 | ✅ DONE | `deleteFile`/`clearlog`: retorno ignorado, siempre `[OK]` | 70-72, 473 | Alta |
| GAP-03 | ✅ DONE | `setup`: LittleFS falla sin mensaje Serial antes de reiniciar | 510-514 | Alta |
| GAP-04 | ✅ DONE | `rx`: `CC1101.Init()` sin verificar, reporta `[OK]` aunque falle | 363 | Alta |
| GAP-05 | ✅ DONE | `tx`: `CC1101.Init()` sin verificar, reporta `[OK]` aunque falle | 408 | Alta |
| GAP-06 | ✅ DONE | `jammer`: `CC1101.Init()` sin verificar, activa jammer con HW roto | 441 | Alta |
| GAP-07 | ✅ DONE | `rx`/`tx`/`jammer`: índice de módulo sin validar (rango 1-2) | 355, 405, 438 | Media |
| GAP-08 | ✅ DONE | `tx`: rawdata con espacios → tokens perdidos silenciosamente | 395 | Media |
| GAP-09 | ✅ DONE | `tx`: buffer `data_to_send[2000]` sin límite → overflow silencioso | 397-403 | Alta |
| GAP-10 | ✅ DONE | `rx`: naming `mod` vs `mode` ambiguo en help y código | 35, 278, 355, 359 | Baja |
| GAP-11 | ✅ DONE | `rx`: modo de modulación sin validar (rango 0-4) | 359, 370 | Media |
| GAP-12 | ✅ DONE | `printReceived`: CSV con coma final no estándar, sin delimitadores | 96-101 | Media |
| GAP-13 | ✅ DONE | `signalanalyse`: texto libre mezclado con datos, no parseable | 213-248 | Media |
| GAP-14 | ✅ DONE | `log`: vuelco sin header/footer, fin de stream indetectable | 462-467 | Media |
| GAP-15 | ✅ DONE | `printStatus`: fallo de FS info sin mensaje al operador | 304-308 | Baja |
| GAP-16 | ✅ DONE | Loop jammer sin heartbeat periódico; RX sin timeout watchdog | 541-549 | Media |
| GAP-17 | ✅ DONE | `setup`: `enableReceive()` llamada sin parámetros RF configurados | 519 | Media |
| GAP-18 | ✅ DONE | `handleSerial`: sin eco ni manejo de backspace | 491-501 | Alta |
| GAP-19 | ✅ DONE | `appendFile`: función nunca emite nada al Serial (cubierto por GAP-01) | 62-68 | Media |
| GAP-20 | ✅ DONE | `enableReceive`: función nunca emite nada al Serial | 260-268 | Baja |

---

## Cambios implementados en firmware v2.0

### GAP-01 / GAP-19 — `appendFile()` con aviso de fallo ✅ DONE
`appendFile()` ahora emite JSON warn si `fs.open()` falla:
```json
{"event":"warn","msg":"FS open failed - datos no guardados en flash"}
```

### GAP-02 — `deleteFile()` retorna bool; `clearlog` verifica resultado ✅ DONE
`deleteFile()` retorna `bool` (retorno de `fs.remove()`).
`clearlog` emite `{"status":"ok",...}` solo si el borrado tuvo éxito, o `{"status":"error",...}` si falló.

### GAP-03 — `setup()` con mensajes antes de format/restart ✅ DONE
Antes de `LittleFS.format()` y `ESP.restart()` se emiten:
```json
{"event":"error","msg":"LittleFS fallo al montar. Iniciando formato..."}
{"event":"error","msg":"Formato completado. Reiniciando dispositivo..."}
```

### GAP-04 / GAP-05 / GAP-06 — Verificación de CC1101 antes de `Init()` ✅ DONE
Nueva función `checkCC1101Module(moduleIdx)` que llama `ELECHOUSE_cc1101.getCC1101()` antes de `Init()`. Si el módulo no responde emite:
```json
{"status":"error","msg":"CC1101 no responde (SPI). Modulo 1 sin conexion o fallo de hardware"}
```
Aplicado en comandos `rx`, `tx` y `jammer`.

### GAP-07 — `parseModuleIndex()` con validación de rango ✅ DONE
Nueva función `parseModuleIndex(token)` valida que el módulo sea 1 o 2. Si no, emite:
```json
{"status":"error","msg":"module debe ser 1 o 2, recibido: 3"}
```
Aplicado en `rx`, `tx`, `jammer`.

### GAP-08 — Rawdata reconstruido de todos los `tokens[5+]` ✅ DONE
En el comando `tx`, el rawdata se reconstruye concatenando todos los tokens desde `tokens[5]` en adelante, permitiendo rawdata con espacios entre valores.

### GAP-09 — Límite de buffer con error explícito ✅ DONE
Durante el parsing de rawdata, si `counter >= 2000` se emite:
```json
{"status":"error","cmd":"tx","msg":"rawdata supera limite de 2000 valores"}
```
y se aborta el comando.

### GAP-10 — Variable `mod` renombrada a `modulationMode`; help clarificado ✅ DONE
La variable global `mod` fue renombrada a `modulationMode` en todo el código para evitar la colisión de nombres con el parámetro `module`. El help ahora muestra claramente:
```
module:     1 o 2 (modulo fisico CC1101)
modulation: 0=2-FSK 1=GFSK 2=ASK/OOK 3=4-FSK 4=MSK
```

### GAP-11 — Validación de modo de modulación en `rx` y `tx` ✅ DONE
Si `modulation` no está en [0,4] se emite:
```json
{"status":"error","cmd":"rx","msg":"modulation fuera de rango: 0=2-FSK,1=GFSK,2=ASK/OOK,3=4-FSK,4=MSK"}
```

### GAP-12 — `printReceived()` con formato estructurado ✅ DONE
La salida ahora incluye:
1. Bloque etiquetado para parsers de texto:
```
[RX-RAW-BEGIN]
Count=45
300,900,300,900
[RX-RAW-END]
```
2. JSON compacto:
```json
{"event":"rx_raw","count":45,"data":[300,900,300,900]}
```
Sin coma trailing en los datos.

### GAP-13 — `signalanalyse()` con salida estructurada ✅ DONE
La salida ahora incluye:
1. Bloque etiquetado:
```
[ANALYSIS-BEGIN]
BITS=01010101...
SAMPLES_PER_SYMBOL=300
PAUSES=[{"pos":15,"us":4800}]
[CORRECTED-BEGIN]
Count=44
300,900,300,900
[CORRECTED-END]
[ANALYSIS-END]
```
2. JSON compacto:
```json
{"event":"rx_analysis","samples_per_symbol":300,"bits":"01010101...","pauses":[...],"corrected_count":44,"corrected":[300,900,...]}
```

### GAP-14 — Comando `log` con delimitadores ✅ DONE
```
[LOG-BEGIN size=1234]
...contenido del log...
[LOG-END]
```

### GAP-15 — `printStatus()` informa si FS no disponible ✅ DONE
Si `LittleFS.begin(false)` falla, se imprime:
```
Flash libre:    [FS no disponible]
```
El status también emite JSON al final:
```json
{"event":"status","rx":false,"jammer":false,"heap":180000,"uptime_s":42}
```

### GAP-16 — Heartbeat de jammer y watchdog de RX ✅ DONE
**Jammer heartbeat**: cada 5 s emite:
```json
{"event":"jammer_heartbeat","freq":433.92,"power_dbm":10,"uptime_ms":12345}
```
**RX watchdog**: si no se captura señal en 60 s emite:
```json
{"event":"rx_timeout","timeout_ms":60000,"msg":"Sin senal en el periodo. RX sigue activo."}
```

### GAP-17 — `enableReceive()` no se llama en `setup()` sin parámetros ✅ DONE
Eliminada la llamada a `enableReceive()` en `setup()`. El receptor solo se arma cuando el usuario ejecuta el comando `rx`. El mensaje de bienvenida es:
```json
{"event":"ready","fw":"2.0","msg":"Evil Crow RF listo","hint":"Escribe help"}
```

### GAP-18 — `handleSerial()` con eco y backspace ✅ DONE
- Cada carácter recibido se ecoa al Serial: `Serial.print(c)`
- Backspace (`\b` / `0x7F`) borra el último carácter del buffer y emite `\b \b` al terminal
- El `\r\n` doble no genera doble procesamiento gracias al buffer vacío

### GAP-20 — `enableReceive()` emite JSON al armarse ✅ DONE
```json
{"event":"rx_armed","msg":"Receptor rearmado - esperando senal"}
```

---

## MEMORIA — Optimizaciones de heap v2.4

Archivo auditado: `EvilCrow-RF/firmware/firmware.ino`
Fecha de implementación: 2026-03-28 — firmware v2.4

### Resumen de hallazgos y correcciones

| # | Estado | Descripción | Función | Severidad |
|---|---|---|---|---|
| MEM-01 | ✅ DONE | Globals muertos `String OutputLog`, `String transmit`, `String lastSampleSmooth` eliminados | globales | Media |
| MEM-02 | ✅ DONE | `printReceived()`: loop `OutputLog += String(sample[i])` → `File::print()` directo | `printReceived()` | Alta |
| MEM-03 | ✅ DONE | `signalanalyse()`: `String bitsStr` y `String pausesJson` con `+=` en loop → `static char bitsArr[4096]` + `char pausesBuf[512]` + `snprintf` | `signalanalyse()` | Alta |
| MEM-04 | ✅ DONE | `signalanalyse()`: loop `OutputLog += String(samplesmooth[i])` → `File::print()` directo | `signalanalyse()` | Alta |
| MEM-05 | ✅ DONE | Nuevo comando `meminfo` reporta métricas de memoria en tiempo real | nuevo comando | N/A |

---

### MEM-01 — Globals muertos eliminados ✅ DONE

Tres variables globales `String` declaradas pero no leídas:

- `String OutputLog` — acumulaba datos para `appendFile()` pero se puede reemplazar con `File::print()` directo
- `String transmit` — declarada, nunca asignada ni leída
- `String lastSampleSmooth` — asignada una vez (`String(samplesmooth[lastIndex])`) pero nunca leída

**Acción**: eliminadas las tres declaraciones y la asignación de `lastSampleSmooth`.

---

### MEM-02 — `printReceived()`: loop de String en heap eliminado ✅ DONE

**Problema original**: hasta 2000 iteraciones de `OutputLog += String(sample[i])` generando alloc/free en heap.

```cpp
// ANTES — hasta 2000 allocaciones de String en heap:
for (int i = 0; i < samplecount; i++) {
    OutputLog += String(sample[i]);          // heap alloc por iteracion
    if (i < samplecount - 1) OutputLog += ",";
}
appendFile(LittleFS, "/logs.txt", nullptr, OutputLog.c_str());
```

**Corrección**: abrir el `File` una sola vez y escribir directamente, sin String intermedia.

```cpp
// DESPUES — cero allocaciones adicionales en heap:
File f = LittleFS.open("/logs.txt", FILE_APPEND);
if (f) {
    f.print(F("\nCount=")); f.print(samplecount); f.print('\n');
    for (int i = 0; i < samplecount; i++) {
        f.print(sample[i]);
        if (i < samplecount - 1) f.print(',');
    }
    f.print('\n');
    f.close();
}
```

---

### MEM-03 — `signalanalyse()`: `String bitsStr` y `pausesJson` en loops ✅ DONE

**Problema original**: tres sitios de concatenación de String dentro del loop principal de análisis.

```cpp
// ANTES — String heap en loop de hasta 2000 iteraciones:
bitsStr   += String((int)lastbin);   // inner loop: hasta 8 bits x sample
pausesJson += "{\"pos\":";
pausesJson += String(bitPos);        // por cada pausa detectada
pausesJson += String(sample[i]);
```

**Corrección**: `static char bitsArr[4096]` (BSS, cero heap) + `char pausesBuf[512]` (stack) con `snprintf`:

```cpp
// DESPUES — BSS + stack, cero heap:
static char bitsArr[4096];   // BSS: cero heap, persiste entre llamadas
int  bitsLen = 0;
char pausesBuf[512];         // stack: liberado al retornar la funcion
int  pbLen   = 0;

// En el loop:
bitsArr[bitsLen++] = lastbin ? '1' : '0';   // sin alloc
pbLen += snprintf(pausesBuf + pbLen, 32,
                  "{\"pos\":%d,\"us\":%lu}", bitPos, sample[i]);
```

`bitsArr` es `static` para moverlo del stack al segmento BSS (el ESP32 tiene stack por tarea de ~8 KB; 4096 bytes en stack sería riesgoso).

---

### MEM-04 — `signalanalyse()`: loop OutputLog de samplesmooth ✅ DONE

**Problema original**: loop `OutputLog += String(samplesmooth[i])` sobre `smoothcount` iteraciones (hasta 2000).

**Corrección**: mismo patrón que MEM-02 — `File::print()` directo en bloque `{ File f = ...; }`.

---

### MEM-05 — Comando `meminfo` ✅ DONE

Nuevo comando que reporta en tiempo real:

| Campo | Fuente | Descripción |
|---|---|---|
| `heap_free` | `ESP.getFreeHeap()` | Heap libre actual |
| `heap_min_ever` | `ESP.getMinFreeHeap()` | Mínimo histórico desde boot |
| `heap_largest_block` | `ESP.getMaxAllocHeap()` | Bloque contiguo más grande |
| `heap_frag_pct` | `100*(free-largest)/free` | % de fragmentación |
| `stack_hwm_bytes` | `uxTaskGetStackHighWaterMark(NULL)*4` | Mínimo stack libre del task principal |
| `fs_total` | `LittleFS.totalBytes()` | Tamaño total del FS |
| `fs_used` | `LittleFS.usedBytes()` | Bytes usados |
| `fs_files` | `openNextFile()` loop | Número de archivos en LittleFS |

Salida de ejemplo:
```
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
{"event":"meminfo","heap_free":180432,"heap_min_ever":162048,"heap_largest_block":163840,"heap_frag_pct":9,"stack_hwm_bytes":5248,"fs_total":1441792,"fs_used":2048,"fs_files":1}
```

---

## Formato de salida v2.0 (resumen para parsers)

Todas las respuestas de comandos siguen el patrón JSON:
```json
{"status":"ok"|"error","cmd":"<comando>",...campos adicionales...}
```

Eventos asíncronos (RX, heartbeat, timeout):
```json
{"event":"<tipo>",...campos...}
```

Bloques de datos raw (parseable por línea con prefijo):
```
[RX-RAW-BEGIN]  / [RX-RAW-END]
[ANALYSIS-BEGIN] / [ANALYSIS-END]
[CORRECTED-BEGIN] / [CORRECTED-END]
[LOG-BEGIN size=N] / [LOG-END]
```

El prompt `ECRF> ` aparece siempre al final de cada respuesta de comando.
