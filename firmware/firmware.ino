// Evil Crow RF - Firmware con CLI Serie (sin WiFi)
// Basado en: https://github.com/joelsernamoreno/EvilCrow-RF
// v2.10 - profiles system (JSON LittleFS, predefined profiles)
// v2.9 - dynamic contextual prompt
// v2.8 - brute force transmitter
// v2.7 - autodetect
// v2.6 - storage management completo (list/show/delete/rename/info)
// v2.5 - relay/bridge dual-radio mode
//
// v2.0: todos los gaps de terminal serie resueltos (ver GAPS.md)
// v2.1 nuevos comandos:
//   freqw <mod>                    consulta frecuencia y config activa
//   raw <mod>                      muestra ultimo raw capturado
//   config                         dump de configuracion CC1101
//   scan <mod> <start> <end> <step> escaneo RSSI por frecuencia
//   replay <mod>                   retransmite ultima senal capturada
//   save <name>                    guarda config en LittleFS
//   load <name>                    carga config desde LittleFS
//   export <name> <format>         exporta ultima senal (urh|rtl)
// v2.2 nuevos comandos:
//   analyze <mod>                  re-analiza ultima senal sin nueva captura
//   signalanalyse() ahora emite JSON "analysis" con freq, modulacion inferida,
//   duracion total, pulsos min/max y deteccion rolling/fixed code
// v2.3 nuevos comandos:
//   debug <on|off>                 traza SPI: cada op CC1101 emite DBG: reg+valor
//   registers <mod>                vuelca los 47 registros config CC1101
// v2.4 optimizaciones de memoria (ver GAPS.md sección MEMORIA) + comando:
//   meminfo                        reporte de heap, stack y LittleFS
// v2.5 nuevos comandos dual-radio:
//   relay <freq> <bw> <mod>        mod1=RX, mod2=TX, misma frecuencia
//   bridge <rx_freq> <tx_freq>     mod1=RX a rx_freq, mod2=TX a tx_freq
// v2.6 storage management:
//   list                           lista archivos LittleFS con nombre, tamaño y fecha
//   show <name>                    vuelca contenido de archivo al Serial
//   delete <name>                  elimina archivo
//   rename <old> <new>             renombra archivo
//   info                           espacio total/usado/libre de LittleFS
//   save <name> ahora avisa si sobreescribe
//
// Prefijos de respuesta: OK: / ERR: en todos los comandos nuevos

#include "ELECHOUSE_CC1101_SRC_DRV.h"
#include <SPI.h>
#include <LittleFS.h>
#include <time.h>

// SPI Pins — CC1101 usa HSPI en V2 (pines 14/12/13)
// El bus VSPI (18/19/23) lo usa la SD card del V2
const int sck_pin  = 14;
const int miso_pin = 12;
const int mosi_pin = 13;

// CC1101 Module 1
const int tx_pin1 = 2;
const int cs_pin1 = 5;
const int rx_pin1 = 4;

// CC1101 Module 2
const int tx_pin2 = 25;
const int cs_pin2 = 27;
const int rx_pin2 = 26;

// RF variables
#define RECEIVE_ATTR IRAM_ATTR
#define samplesize 2000
int error_toleranz = 200;
const int minsample = 30;
volatile unsigned long sample[samplesize];
unsigned long samplesmooth[samplesize];
int lastIndex;
volatile int samplecount = 0;
static unsigned long lastTime = 0;

// GAP-10: renombrado de 'mod' a 'modulationMode'
int   modulationMode = 0;
float deviation      = 0.0;
int   datarate       = 4;
float frequency      = 433.92;
float setrxbw        = 812.0;
int   power_jammer   = 10;

byte jammer[] = { 0xff, 0xff };
const size_t jammer_len = sizeof(jammer) / sizeof(jammer[0]);
long data_to_send[2000];

// State
String raw_rx    = "0";
String jammer_tx = "0";
String tmp_module = "1";

// GAP-16: RX timeout watchdog
unsigned long rxStartTime = 0;
const unsigned long RX_TIMEOUT_MS = 60000UL;

// GAP-16: Jammer heartbeat
unsigned long lastJammerHeartbeat = 0;
const unsigned long JAMMER_HEARTBEAT_MS = 5000UL;

// ---- v2.1: estado para nuevos comandos ----

// Para 'raw': snapshot del ultimo raw capturado
unsigned long lastRawSamples[samplesize];
int  lastRawCount  = 0;
int  lastRawModule = 1;  // numero de modulo 1-based

// Para 'replay': ultimo smooth + metadatos de captura
// samplesmooth[] ya es global; solo falta el conteo y metadatos
int   lastSmoothCount  = 0;
int   lastSmoothModule = 1;
float lastSmoothFreq   = 0.0;
int   lastSmoothMod    = 0;

// Para 'scan': limite de pasos para evitar scans infinitos
const int SCAN_MAX_STEPS = 500;

// v2.5: relay / bridge mode
volatile bool relayActive = false;  // true mientras relay o bridge esten activos
float         relayTxFreq = 0.0;    // freq TX (relay: igual que RX; bridge: distinta)
int           relayTxMod  = 0;      // modulacion TX para relay/bridge

// v2.8: brute force mode
volatile bool bruteActive = false;  // true mientras brute esta transmitiendo

// File
File logs;

// Serial CLI
String serialBuffer = "";

// ============================================================
// Helpers: salida JSON al Serial
// ============================================================

void jsonEvent(const char* evt, const char* fields) {
  Serial.print(F("{\"event\":\""));
  Serial.print(evt);
  Serial.print(F("\""));
  if (fields && fields[0] != '\0') {
    Serial.print(',');
    Serial.print(fields);
  }
  Serial.println('}');
}

// ============================================================
// Filesystem helpers
// ============================================================

void appendFile(fs::FS &fs, const char *path, const char *message, const char *extra) {
  logs = fs.open(path, FILE_APPEND);
  if (!logs) {
    Serial.println(F("{\"event\":\"warn\",\"msg\":\"FS open failed - datos no guardados en flash\"}"));
    return;
  }
  if (message) logs.print(message);
  if (extra)   logs.print(extra);
  logs.close();
}

bool deleteFile(fs::FS &fs, const char *path) {
  return fs.remove(path);
}

// ============================================================
// CC1101 check helper
// ============================================================

bool checkCC1101Module(int moduleIdx) {
  ELECHOUSE_cc1101.setModul(moduleIdx);
  if (!ELECHOUSE_cc1101.getCC1101()) {
    Serial.print(F("ERR: cc1101_no_response module="));
    Serial.println(moduleIdx + 1);
    return false;
  }
  return true;
}

// ============================================================
// v2.3: Debug mode — flag global, helpers y wrappers CC1101
// ============================================================

bool debugMode = false;  // activo con 'debug on'

// Lee un registro de configuracion CC1101 (0x00-0x2E) y lo emite al Serial
static void dbgReg(byte addr, const char* regName) {
  byte val = ELECHOUSE_cc1101.SpiReadReg(addr);
  Serial.print(F("DBG:   REG[0x"));
  if (addr < 0x10) Serial.print('0');
  Serial.print(addr, HEX);
  Serial.print(F("]=0x"));
  if (val < 0x10) Serial.print('0');
  Serial.print(val, HEX);
  Serial.print(F(" ("));
  Serial.print(regName);
  Serial.println(')');
}

// Lee un registro de estado CC1101 (0x30-0x3D) y lo emite al Serial
static void dbgStatus(byte addr, const char* regName) {
  byte val = ELECHOUSE_cc1101.SpiReadStatus(addr);
  Serial.print(F("DBG:   STATUS[0x"));
  if (addr < 0x10) Serial.print('0');
  Serial.print(addr, HEX);
  Serial.print(F("]=0x"));
  if (val < 0x10) Serial.print('0');
  Serial.print(val, HEX);
  Serial.print(F(" ("));
  Serial.print(regName);
  Serial.println(')');
}

// ---------- Wrappers de operaciones CC1101 ----------

static void cc1101Init() {
  if (debugMode) Serial.println(F("DBG: >> Init()"));
  ELECHOUSE_cc1101.Init();
  if (!debugMode) return;
  dbgReg(CC1101_FREQ2,    "FREQ2");
  dbgReg(CC1101_FREQ1,    "FREQ1");
  dbgReg(CC1101_FREQ0,    "FREQ0");
  dbgReg(CC1101_MDMCFG4,  "MDMCFG4");
  dbgReg(CC1101_MDMCFG2,  "MDMCFG2");
  dbgReg(CC1101_PKTCTRL0, "PKTCTRL0");
}

static void cc1101SetMHZ(float f) {
  if (debugMode) { Serial.print(F("DBG: >> setMHZ(")); Serial.print(f, 5); Serial.println(')'); }
  ELECHOUSE_cc1101.setMHZ(f);
  if (!debugMode) return;
  dbgReg(CC1101_FREQ2, "FREQ2");
  dbgReg(CC1101_FREQ1, "FREQ1");
  dbgReg(CC1101_FREQ0, "FREQ0");
}

static void cc1101SetModulation(byte m) {
  if (debugMode) { Serial.print(F("DBG: >> setModulation(")); Serial.print(m); Serial.println(')'); }
  ELECHOUSE_cc1101.setModulation(m);
  if (!debugMode) return;
  dbgReg(CC1101_MDMCFG2, "MDMCFG2");
}

static void cc1101SetRxBW(float bw) {
  if (debugMode) { Serial.print(F("DBG: >> setRxBW(")); Serial.print(bw); Serial.println(')'); }
  ELECHOUSE_cc1101.setRxBW(bw);
  if (!debugMode) return;
  dbgReg(CC1101_MDMCFG4, "MDMCFG4");
}

static void cc1101SetDRate(int rate) {
  if (debugMode) { Serial.print(F("DBG: >> setDRate(")); Serial.print(rate); Serial.println(')'); }
  ELECHOUSE_cc1101.setDRate(rate);
  if (!debugMode) return;
  dbgReg(CC1101_MDMCFG4, "MDMCFG4");
  dbgReg(CC1101_MDMCFG3, "MDMCFG3");
}

static void cc1101SetDeviation(float d) {
  if (debugMode) { Serial.print(F("DBG: >> setDeviation(")); Serial.print(d); Serial.println(')'); }
  ELECHOUSE_cc1101.setDeviation(d);
  if (!debugMode) return;
  dbgReg(CC1101_DEVIATN, "DEVIATN");
}

static void cc1101SetSyncMode(byte m) {
  if (debugMode) { Serial.print(F("DBG: >> setSyncMode(")); Serial.print(m); Serial.println(')'); }
  ELECHOUSE_cc1101.setSyncMode(m);
  if (!debugMode) return;
  dbgReg(CC1101_MDMCFG2, "MDMCFG2");
}

static void cc1101SetPktFormat(byte v) {
  if (debugMode) { Serial.print(F("DBG: >> setPktFormat(")); Serial.print(v); Serial.println(')'); }
  ELECHOUSE_cc1101.setPktFormat(v);
  if (!debugMode) return;
  dbgReg(CC1101_PKTCTRL0, "PKTCTRL0");
}

static void cc1101SetDcFilterOff(bool v) {
  if (debugMode) { Serial.print(F("DBG: >> setDcFilterOff(")); Serial.print(v); Serial.println(')'); }
  ELECHOUSE_cc1101.setDcFilterOff(v);
  if (!debugMode) return;
  dbgReg(CC1101_MDMCFG2, "MDMCFG2");
}

static void cc1101SetPA(int p) {
  if (debugMode) { Serial.print(F("DBG: >> setPA(")); Serial.print(p); Serial.println(')'); }
  ELECHOUSE_cc1101.setPA(p);
  if (!debugMode) return;
  dbgReg(CC1101_FREND0, "FREND0");  // FREND0 contiene el indice PATABLE activo
}

static void cc1101SetRx() {
  if (debugMode) Serial.println(F("DBG: >> SetRx() [strobe SRX=0x34]"));
  ELECHOUSE_cc1101.SetRx();
  if (!debugMode) return;
  dbgStatus(CC1101_MARCSTATE, "MARCSTATE");
}

static void cc1101SetTx() {
  if (debugMode) Serial.println(F("DBG: >> SetTx() [strobe STX=0x35]"));
  ELECHOUSE_cc1101.SetTx();
  if (!debugMode) return;
  dbgStatus(CC1101_MARCSTATE, "MARCSTATE");
}

static void cc1101SetSidle() {
  if (debugMode) Serial.println(F("DBG: >> setSidle() [strobe SIDLE=0x36]"));
  ELECHOUSE_cc1101.setSidle();
  if (!debugMode) return;
  dbgStatus(CC1101_MARCSTATE, "MARCSTATE");
}

// ============================================================
// Module index helper
// ============================================================

int parseModuleIndex(const String &token) {
  int num = token.toInt();
  if (num < 1 || num > 2) {
    Serial.print(F("ERR: module_invalid value="));
    Serial.print(token);
    Serial.println(F(" expected=1|2"));
    return -1;
  }
  return num - 1;
}

// ============================================================
// RF receive / analyse
// ============================================================

bool checkReceived(void) {
  delay(1);
  if (samplecount >= minsample && micros() - lastTime > 100000) {
    detachInterrupt(digitalPinToInterrupt(rx_pin1));
    if (!relayActive) detachInterrupt(digitalPinToInterrupt(rx_pin2));
    return true;
  }
  return false;
}

void printReceived() {
  appendFile(LittleFS, "/logs.txt", "-------------------------------------------------------\n", nullptr);

  // v2.1: snapshot para comando 'raw'
  lastRawCount  = samplecount;
  lastRawModule = tmp_module.toInt();
  for (int i = 0; i < lastRawCount; i++) lastRawSamples[i] = sample[i];

  // MEM-02: escribir raw al log directamente, sin acumular String en heap
  {
    File f = LittleFS.open("/logs.txt", FILE_APPEND);
    if (f) {
      f.print(F("\nCount="));
      f.print(samplecount);
      f.print('\n');
      for (int i = 0; i < samplecount; i++) {
        f.print(sample[i]);
        if (i < samplecount - 1) f.print(',');
      }
      f.print('\n');
      f.close();
    } else {
      Serial.println(F("{\"event\":\"warn\",\"msg\":\"FS open failed - raw data no guardado\"}"));
    }
  }

  Serial.println(F("[RX-RAW-BEGIN]"));
  Serial.print(F("Count="));
  Serial.println(samplecount);
  for (int i = 0; i < samplecount; i++) {
    Serial.print(sample[i]);
    if (i < samplecount - 1) Serial.print(',');
  }
  Serial.println();
  Serial.println(F("[RX-RAW-END]"));

  Serial.print(F("{\"event\":\"rx_raw\",\"count\":"));
  Serial.print(samplecount);
  Serial.print(F(",\"data\":["));
  for (int i = 0; i < samplecount; i++) {
    Serial.print(sample[i]);
    if (i < samplecount - 1) Serial.print(',');
  }
  Serial.println(F("]}"));
}

void RECEIVE_ATTR receiver() {
  const long t = micros();
  const unsigned int duration = t - lastTime;

  if (duration > 100000) samplecount = 0;

  if (duration >= 100 && samplecount < samplesize) {
    sample[samplecount++] = duration;
  }

  if (samplecount >= samplesize) {
    samplecount = samplesize - 1;
    // No llamar checkReceived() desde ISR — delay() en ISR corrompe FreeRTOS.
    // Forzar lastTime al pasado para que checkReceived() en loop() retorne true.
    lastTime = t - 200000;
    detachInterrupt(digitalPinToInterrupt(rx_pin1));
    if (!relayActive) detachInterrupt(digitalPinToInterrupt(rx_pin2));
  }

  // In relay/bridge mode Module 2 is TX so rx_pin2 state is undefined — skip dual-pin check
  if (modulationMode == 0 && !relayActive) {
    if (samplecount == 1 && digitalRead(rx_pin2) != HIGH) { samplecount = 0; }
    else if (samplecount == 1 && digitalRead(rx_pin1) != HIGH) { samplecount = 0; }
  }

  lastTime = t;
}

void signalanalyse() {
  #define signalstorage 10

  int signalanz = 0;
  int timingdelay[signalstorage];
  long signaltimings[signalstorage * 2];
  int  signaltimingscount[signalstorage];
  long signaltimingssum[signalstorage];

  for (int i = 0; i < signalstorage; i++) {
    signaltimings[i * 2]     = 100000;
    signaltimings[i * 2 + 1] = 0;
    signaltimingscount[i]    = 0;
    signaltimingssum[i]      = 0;
  }

  for (int p = 0; p < signalstorage; p++) {
    for (int i = 1; i < samplecount; i++) {
      if (p == 0) {
        if (sample[i] < (unsigned long)signaltimings[p * 2]) signaltimings[p * 2] = sample[i];
      } else {
        if (sample[i] < (unsigned long)signaltimings[p * 2] && sample[i] > (unsigned long)signaltimings[p * 2 - 1])
          signaltimings[p * 2] = sample[i];
      }
    }
    for (int i = 1; i < samplecount; i++) {
      if (sample[i] < (unsigned long)(signaltimings[p * 2] + error_toleranz) && sample[i] > (unsigned long)signaltimings[p * 2 + 1])
        signaltimings[p * 2 + 1] = sample[i];
    }
    for (int i = 1; i < samplecount; i++) {
      if (sample[i] >= (unsigned long)signaltimings[p * 2] && sample[i] <= (unsigned long)signaltimings[p * 2 + 1]) {
        signaltimingscount[p]++;
        signaltimingssum[p] += sample[i];
      }
    }
  }

  int firstsample = signaltimings[0];
  signalanz = signalstorage;
  for (int i = 0; i < signalstorage; i++) {
    if (signaltimingscount[i] == 0) { signalanz = i; break; }
  }

  for (int s = 1; s < signalanz; s++) {
    for (int i = 0; i < signalanz - s; i++) {
      if (signaltimingscount[i] < signaltimingscount[i + 1]) {
        long  t1 = signaltimings[i*2],     t2 = signaltimings[i*2+1];
        long  t3 = signaltimingssum[i];
        int   t4 = signaltimingscount[i];
        signaltimings[i*2]         = signaltimings[(i+1)*2];
        signaltimings[i*2+1]       = signaltimings[(i+1)*2+1];
        signaltimingssum[i]        = signaltimingssum[i+1];
        signaltimingscount[i]      = signaltimingscount[i+1];
        signaltimings[(i+1)*2]     = t1;
        signaltimings[(i+1)*2+1]   = t2;
        signaltimingssum[i+1]      = t3;
        signaltimingscount[i+1]    = t4;
      }
    }
  }

  for (int i = 0; i < signalanz; i++) {
    timingdelay[i] = signaltimingssum[i] / signaltimingscount[i];
  }

  if (firstsample == sample[1] && firstsample < timingdelay[0]) {
    sample[1] = timingdelay[0];
  }

  // MEM-03: static char en BSS en lugar de String en heap — evita fragmentacion
  static char bitsArr[4096];
  int  bitsLen       = 0;
  char pausesBuf[512];
  int  pbLen         = 0;
  bool firstPause    = true;
  bool lastbin       = 0;
  int  bitPos        = 0;
  bitsArr[0]         = '\0';
  pausesBuf[pbLen++] = '[';

  for (int i = 1; i < samplecount; i++) {
    float r = (float)sample[i] / timingdelay[0];
    int calculate = (int)r;
    r = (r - calculate) * 10;
    if (r >= 5) calculate++;
    if (calculate > 0) {
      lastbin = !lastbin;
      if (lastbin == 0 && calculate > 8) {
        // MEM-03: snprintf en lugar de String += String(val)
        if (!firstPause && pbLen < (int)sizeof(pausesBuf) - 32)
          pausesBuf[pbLen++] = ',';
        if (pbLen < (int)sizeof(pausesBuf) - 32) {
          int n = snprintf(pausesBuf + pbLen, 32,
                           "{\"pos\":%d,\"us\":%lu}", bitPos, (unsigned long)sample[i]);
          if (n > 0) pbLen += n;
        }
        firstPause = false;
      } else {
        for (int b = 0; b < calculate; b++) {
          if (bitsLen < (int)sizeof(bitsArr) - 1) bitsArr[bitsLen++] = lastbin ? '1' : '0';
          bitPos++;
        }
      }
    }
  }
  if (pbLen < (int)sizeof(pausesBuf) - 1) pausesBuf[pbLen++] = ']';
  bitsArr[bitsLen] = '\0';
  pausesBuf[pbLen] = '\0';

  int smoothcount = 0;
  for (int i = 1; i < samplecount; i++) {
    float r = (float)sample[i] / timingdelay[0];
    int calculate = (int)r;
    r = (r - calculate) * 10;
    if (r >= 5) calculate++;
    if (calculate > 0) {
      samplesmooth[smoothcount] = calculate * timingdelay[0];
      smoothcount++;
    }
  }

  lastIndex = smoothcount - 1;

  // v2.1: guardar metadatos para 'replay'
  lastSmoothCount  = smoothcount;
  lastSmoothModule = tmp_module.toInt();
  lastSmoothFreq   = frequency;
  lastSmoothMod    = modulationMode;

  // MEM-03/MEM-04: escribir analisis al log directamente sin acumular String en heap
  {
    File f = LittleFS.open("/logs.txt", FILE_APPEND);
    if (f) {
      f.print(F("\nBits: "));           f.println(bitsArr);
      f.print(F("Samples/Symbol: "));   f.println(timingdelay[0]);
      f.println(F("Rawdata corrected:"));
      f.print(F("Count="));             f.println(smoothcount);
      for (int i = 0; i < smoothcount; i++) {
        f.print(samplesmooth[i]);
        if (i < smoothcount - 1) f.print(',');
      }
      f.println(F("\n-------------------------------------------------------"));
      f.close();
    }
  }

  Serial.println(F("[ANALYSIS-BEGIN]"));
  Serial.print(F("BITS="));           Serial.println(bitsArr);
  Serial.print(F("SAMPLES_PER_SYMBOL=")); Serial.println(timingdelay[0]);
  Serial.print(F("PAUSES="));         Serial.println(pausesBuf);
  Serial.println(F("[CORRECTED-BEGIN]"));
  Serial.print(F("Count="));          Serial.println(smoothcount);
  for (int i = 0; i < smoothcount; i++) {
    Serial.print(samplesmooth[i]);
    if (i < smoothcount - 1) Serial.print(',');
  }
  Serial.println();
  Serial.println(F("[CORRECTED-END]"));
  Serial.println(F("[ANALYSIS-END]"));

  Serial.print(F("{\"event\":\"rx_analysis\",\"samples_per_symbol\":"));
  Serial.print(timingdelay[0]);
  Serial.print(F(",\"bits\":\""));     Serial.print(bitsArr);
  Serial.print(F("\",\"pauses\":"));   Serial.print(pausesBuf);
  Serial.print(F(",\"corrected_count\":"));  Serial.print(smoothcount);
  Serial.print(F(",\"corrected\":["));
  for (int i = 0; i < smoothcount; i++) {
    Serial.print(samplesmooth[i]);
    if (i < smoothcount - 1) Serial.print(',');
  }
  Serial.println(F("]}"));

  // ---- v2.2: JSON extendido de analisis ----

  // Duracion total de trama: suma de todos los intervalos crudos
  unsigned long frameDuration = 0;
  for (int i = 0; i < samplecount; i++) frameDuration += sample[i];

  // Pulso minimo y maximo
  unsigned long pulseMin = (samplecount > 0) ? sample[0] : 0UL;
  unsigned long pulseMax = pulseMin;
  for (int i = 1; i < samplecount; i++) {
    if (sample[i] < pulseMin) pulseMin = sample[i];
    if (sample[i] > pulseMax) pulseMax = sample[i];
  }

  // Modulacion inferida: OOK cuando el pulso mas largo > 3x el mas corto
  // (duty cycle dominado por silencios => tipico en ASK/OOK)
  // FSK cuando los tiempos HIGH y LOW son comparables
  const char* modInferred = (pulseMin > 0 && pulseMax > 3UL * pulseMin) ? "OOK" : "FSK";

  // Patron de codigo: compara primera mitad de bits con segunda mitad
  // Si coinciden >= 75% => "fixed" (mensaje repetido); si no => "rolling"
  const char* codePattern = "rolling";
  if (bitsLen >= 8) {
    int half    = bitsLen / 2;
    int matches = 0;
    for (int i = 0; i < half; i++) {
      if (bitsArr[i] == bitsArr[i + half]) matches++;
    }
    if (matches * 100 / half >= 75) codePattern = "fixed";
  }

  Serial.print(F("{\"event\":\"analysis\",\"analysis\":{\"freq\":"));
  Serial.print(frequency, 5);
  Serial.print(F(",\"modulation_inferred\":\""));
  Serial.print(modInferred);
  Serial.print(F("\",\"frame_duration_us\":"));
  Serial.print(frameDuration);
  Serial.print(F(",\"pulse_count\":"));
  Serial.print(samplecount);
  Serial.print(F(",\"pulse_min_us\":"));
  Serial.print(pulseMin);
  Serial.print(F(",\"pulse_max_us\":"));
  Serial.print(pulseMax);
  Serial.print(F(",\"pattern\":\""));
  Serial.print(codePattern);
  Serial.println(F("\"}}"));

  Serial.flush();
}

void enableReceive() {
  pinMode(rx_pin1, INPUT);
  pinMode(rx_pin2, INPUT);
  cc1101SetRx();
  samplecount = 0;
  attachInterrupt(digitalPinToInterrupt(rx_pin1), receiver, CHANGE);
  attachInterrupt(digitalPinToInterrupt(rx_pin2), receiver, CHANGE);
  Serial.println(F("{\"event\":\"rx_armed\",\"msg\":\"Receptor rearmado - esperando senal\"}"));
}

// v2.5: Arms only Module 1 as RX — Module 2 is reserved for TX in relay/bridge mode
void enableRelayReceive() {
  ELECHOUSE_cc1101.setModul(0);
  pinMode(rx_pin1, INPUT);
  cc1101SetRx();
  samplecount = 0;
  attachInterrupt(digitalPinToInterrupt(rx_pin1), receiver, CHANGE);
  // rx_pin2 interrupt intentionally NOT attached: Module 2 is TX
}

// v2.5: Transmit captured sample[] on Module 2 at relayTxFreq
void relayTransmit(int pulseCount) {
  ELECHOUSE_cc1101.setModul(1);
  cc1101SetMHZ(relayTxFreq);
  cc1101SetTx();
  for (int i = 0; i < pulseCount; i += 2) {
    digitalWrite(tx_pin2, HIGH);
    delayMicroseconds((unsigned int)sample[i]);
    digitalWrite(tx_pin2, LOW);
    if (i + 1 < pulseCount) delayMicroseconds((unsigned int)sample[i + 1]);
  }
  cc1101SetSidle();
}

// ============================================================
// Serial CLI — prompt dinamico (v2.9)
// ============================================================

// Emite el prompt contextual segun estado activo:
//   ECRF[IDLE]>          en reposo
//   ECRF[RX:<mod>@<freq>]>   receptor activo
//   ECRF[JAM:<mod>]>         jammer activo
void printPrompt() {
  Serial.println();
  Serial.print(F("ECRF["));
  if (jammer_tx == "1") {
    Serial.print(F("JAM:"));
    Serial.print(tmp_module);
  } else if (raw_rx == "1") {
    Serial.print(F("RX:"));
    Serial.print(tmp_module);
    Serial.print('@');
    Serial.print(frequency, 2);
  } else {
    Serial.print(F("IDLE"));
  }
  Serial.print(F("]> "));
}

// ============================================================
// Serial CLI — ayuda
// ============================================================

void printHelp() {
  Serial.println(F("\n=== Evil Crow RF - Serial CLI v2.10 ==="));
  Serial.println(F("  help"));
  Serial.println(F("  status"));
  Serial.println(F("  rx <module> <freq> <bw> <modulation> <deviation> <datarate>"));
  Serial.println(F("    module:     1|2   freq: MHz   bw: kHz"));
  Serial.println(F("    modulation: 0=2-FSK 1=GFSK 2=ASK/OOK 3=4-FSK 4=MSK"));
  Serial.println(F("    deviation:  kHz    datarate: kbps"));
  Serial.println(F("  stoprx"));
  Serial.println(F("  tx <module> <freq> <modulation> <deviation> <rawdata>"));
  Serial.println(F("    rawdata: timings por coma ej:300,900,300  max:2000"));
  Serial.println(F("  jammer <module> <freq> <power_dBm>"));
  Serial.println(F("  stopjammer"));
  Serial.println(F("  log / clearlog / reboot"));
  Serial.println(F("--- Nuevos en v2.1 (prefijo OK:/ERR:) ---"));
  Serial.println(F("  freqw <module>                  Config activa del modulo"));
  Serial.println(F("  raw <module>                    Ultimo raw capturado"));
  Serial.println(F("  config                          Dump completo de config"));
  Serial.println(F("  scan <module> <start> <end> <step>  Escaneo RSSI"));
  Serial.println(F("    max 500 pasos; frecuencias en MHz"));
  Serial.println(F("  replay <module>                 Retransmitir ultima captura"));
  Serial.println(F("  save <name>                     Guardar config en LittleFS"));
  Serial.println(F("  load <name>                     Cargar config desde LittleFS"));
  Serial.println(F("    name: alfanumerico, max 20 chars"));
  Serial.println(F("  export <name> <format>          Exportar ultima senal capturada"));
  Serial.println(F("    name: last (ultima captura en RAM)"));
  Serial.println(F("    format: urh  -> pulsos URH +300 -900...  [EXPORT-URH-BEGIN/END]"));
  Serial.println(F("    format: rtl  -> JSON rtl_433             [EXPORT-RTL-BEGIN/END]"));
  Serial.println(F("--- Nuevos en v2.2 ---"));
  Serial.println(F("  analyze <modulation>            Re-analizar ultima senal capturada"));
  Serial.println(F("    Emite JSON 'analysis': freq, modulation_inferred, frame_duration_us,"));
  Serial.println(F("    pulse_count, pulse_min_us, pulse_max_us, pattern (fixed|rolling)"));
  Serial.println(F("    modulation: 0=2-FSK 1=GFSK 2=ASK/OOK 3=4-FSK 4=MSK"));
  Serial.println(F("--- Nuevos en v2.3 ---"));
  Serial.println(F("  debug <on|off>                  Traza SPI de operaciones CC1101"));
  Serial.println(F("    Cada op emite: DBG: >> <func>(<param>)  y  DBG:   REG[0xNN]=0xVV"));
  Serial.println(F("    Util para diagnosticar modulos defectuosos o comportamiento inesperado"));
  Serial.println(F("  registers <module>              Vuelca los 47 registros config CC1101"));
  Serial.println(F("    Formato: REG[0xNN]=0xVV NOMBRE"));
  Serial.println(F("--- Nuevos en v2.4 ---"));
  Serial.println(F("  meminfo                         Reporte de uso de memoria"));
  Serial.println(F("    heap_free, heap_min_ever, heap_largest_block, heap_frag%"));
  Serial.println(F("    stack_hwm (FreeRTOS high-water mark del task principal)"));
  Serial.println(F("    fs_total, fs_used, fs_free, fs_files (LittleFS)"));
  Serial.println(F("--- Nuevos en v2.5 ---"));
  Serial.println(F("  relay <freq> <bw> <modulation>  Mod1=RX, Mod2=TX (misma frecuencia)"));
  Serial.println(F("    Todo lo que captura mod1 se retransmite por mod2 de inmediato"));
  Serial.println(F("    Cada paquete emite: OK: RELAY <pulsos>"));
  Serial.println(F("  bridge <rx_freq> <tx_freq>       Mod1=RX a rx_freq, Mod2=TX a tx_freq"));
  Serial.println(F("    Usa mod/bw/dev/rate actuales (ver config/load)"));
  Serial.println(F("    Ambos modos se detienen con 'stoprx'"));
  Serial.println(F("--- Nuevos en v2.6 ---"));
  Serial.println(F("  list                            Lista archivos en LittleFS"));
  Serial.println(F("    Formato: <name> size=N date=ISO  encerrado en [LIST-BEGIN/END count=N]"));
  Serial.println(F("  show <name>                     Vuelca contenido de archivo al Serial"));
  Serial.println(F("    Encerrado en [FILE-BEGIN path=X size=N] ... [FILE-END]"));
  Serial.println(F("  delete <name>                   Elimina archivo de LittleFS"));
  Serial.println(F("  rename <old> <new>              Renombra archivo en LittleFS"));
  Serial.println(F("    ERR si dst ya existe; rutas sin / se completan automaticamente"));
  Serial.println(F("  info                            Espacio total/used/free/pct + recuento"));
  Serial.println(F("    Emite OK: info ... y JSON {\"event\":\"fs_info\",...}"));
  Serial.println(F("  save <name> ahora avisa si sobreescribe un archivo existente"));
  Serial.println(F("--- Nuevos en v2.7 ---"));
  Serial.println(F("  autodetect <module> <freq>      Detecta modulacion y BW automaticamente"));
  Serial.println(F("    Prueba: mod=[OOK,2FSK,4FSK,GFSK] x bw=[812,406,203 kHz]"));
  Serial.println(F("    Emite TRY: mod=X bw=Y por cada combinacion"));
  Serial.println(F("    Emite FOUND: mod=X bw=Y rate=Z al capturar"));
  Serial.println(F("    Timeout 3 s por combinacion; max 12 intentos (~36 s)"));
  Serial.println(F("    Si encuentra senal actualiza globals (replay/analyze listos)"));
  Serial.println(F("    Si no encuentra: ERR: autodetect no_signal_found"));
  Serial.println(F("--- Nuevos en v2.8 ---"));
  Serial.println(F("  brute <module> <freq> <bits> <delay_ms>"));
  Serial.println(F("    Transmite todos los codigos de bits bits (max 24 = 16M codigos)"));
  Serial.println(F("    mod: 1|2  freq: MHz  bits: 1-24  delay_ms: pausa entre codigos"));
  Serial.println(F("    Encoding OOK: bit1=600us/200us  bit0=200us/600us  MSB primero"));
  Serial.println(F("    Emite cada 1000 codigos: BRUTE: <n>/<total> ultimo=0xHHH"));
  Serial.println(F("    Detener con 'stopbrute' durante la ejecucion"));
  Serial.println(F("    Emite BRUTE-LISTO al finalizar (completo o detenido)"));
  Serial.println(F("    ERR: si bits>24 para evitar ejecuciones infinitas"));
  Serial.println(F("  stopbrute                         Detiene brute en curso"));
  Serial.println(F("--- Cambios en v2.9 ---"));
  Serial.println(F("  Prompt dinamico segun estado:"));
  Serial.println(F("    ECRF[IDLE]>          en reposo"));
  Serial.println(F("    ECRF[RX:1@433.92]>   modulo 1 recibiendo a 433.92 MHz"));
  Serial.println(F("    ECRF[JAM:2]>         modulo 2 con jammer activo"));
  Serial.println(F("--- Nuevos en v2.10 ---"));
  Serial.println(F("  save <name>                     Guarda config como /profiles/<name>.json"));
  Serial.println(F("  load <name>                     Carga perfil JSON; fallback a /cfg_<name>.cfg"));
  Serial.println(F("  profiles                        Lista perfiles guardados en /profiles/"));
  Serial.println(F("  profile-del <name>              Elimina perfil /profiles/<name>.json"));
  Serial.println(F("  Perfiles predefinidos: default433, default868, fsk433"));
  Serial.println(F("==========================================\n"));
}

// ============================================================
// Serial CLI — status
// ============================================================

void printStatus() {
  Serial.println(F("\n--- Evil Crow RF Status ---"));
  Serial.print(F("RX activo:      ")); Serial.println(raw_rx);
  Serial.print(F("Jammer activo:  ")); Serial.println(jammer_tx);
  Serial.print(F("Free heap:      ")); Serial.print(ESP.getFreeHeap()); Serial.println(F(" bytes"));
  Serial.print(F("Uptime:         ")); Serial.print(millis() / 1000); Serial.println(F(" s"));
  Serial.print(F("Temperatura:    ")); Serial.print(temperatureRead()); Serial.println(F(" C"));
  if (LittleFS.begin(false)) {
    Serial.print(F("Flash libre:    "));
    Serial.print(LittleFS.totalBytes() - LittleFS.usedBytes());
    Serial.println(F(" bytes"));
  } else {
    Serial.println(F("Flash libre:    [FS no disponible]"));
  }
  Serial.println(F("---------------------------"));
  Serial.print(F("{\"event\":\"status\",\"rx\":"));
  Serial.print(raw_rx == "1" ? "true" : "false");
  Serial.print(F(",\"relay\":"));
  Serial.print(relayActive ? "true" : "false");
  Serial.print(F(",\"jammer\":"));
  Serial.print(jammer_tx == "1" ? "true" : "false");
  Serial.print(F(",\"heap\":"));   Serial.print(ESP.getFreeHeap());
  Serial.print(F(",\"uptime_s\":"));  Serial.print(millis() / 1000);
  Serial.println(F("}"));
}

// ============================================================
// v2.1 — Nuevos comandos
// ============================================================

// freqw <module>: consulta configuracion activa
void cmdFreqw(const String &modToken) {
  int modIdx = parseModuleIndex(modToken);
  if (modIdx < 0) return;

  Serial.print(F("OK: freqw module="));
  Serial.print(modToken);
  Serial.print(F(" freq="));    Serial.print(frequency, 5);
  Serial.print(F(" bw="));      Serial.print(setrxbw, 3);
  Serial.print(F(" mod="));     Serial.print(modulationMode);
  Serial.print(F(" dev="));     Serial.print(deviation, 3);
  Serial.print(F(" rate="));    Serial.print(datarate);
  Serial.print(F(" power="));   Serial.print(power_jammer);
  Serial.print(F(" last_used_module="));
  Serial.println(tmp_module);
}

// raw <module>: muestra el ultimo raw capturado
void cmdRaw(const String &modToken) {
  int modIdx = parseModuleIndex(modToken);
  if (modIdx < 0) return;

  if (lastRawCount == 0) {
    Serial.println(F("ERR: raw no_data_available"));
    return;
  }

  Serial.print(F("OK: raw count="));
  Serial.print(lastRawCount);
  Serial.print(F(" captured_on_module="));
  Serial.println(lastRawModule);
  Serial.println(F("[RAW-BEGIN]"));
  for (int i = 0; i < lastRawCount; i++) {
    Serial.print(lastRawSamples[i]);
    if (i < lastRawCount - 1) Serial.print(',');
  }
  Serial.println();
  Serial.println(F("[RAW-END]"));
}

// config: dump completo de la configuracion CC1101 activa
void cmdConfig() {
  Serial.println(F("OK: config"));
  Serial.print(F("OK: freq="));        Serial.println(frequency, 5);
  Serial.print(F("OK: bw="));          Serial.println(setrxbw, 3);
  Serial.print(F("OK: modulation="));  Serial.println(modulationMode);
  Serial.print(F("OK: deviation="));   Serial.println(deviation, 3);
  Serial.print(F("OK: datarate="));    Serial.println(datarate);
  Serial.print(F("OK: power_dbm="));   Serial.println(power_jammer);
  Serial.print(F("OK: active_module="));  Serial.println(tmp_module);
  Serial.print(F("OK: rx_active="));   Serial.println(raw_rx);
  Serial.print(F("OK: jammer_active=")); Serial.println(jammer_tx);
  Serial.print(F("OK: last_smooth_count="));  Serial.println(lastSmoothCount);
  Serial.print(F("OK: last_raw_count="));     Serial.println(lastRawCount);

  // Verificar presencia fisica de cada modulo
  for (int m = 0; m < 2; m++) {
    ELECHOUSE_cc1101.setModul(m);
    bool present = ELECHOUSE_cc1101.getCC1101();
    Serial.print(F("OK: cc1101_module"));
    Serial.print(m + 1);
    Serial.print(F("="));
    Serial.println(present ? "present" : "absent");
  }
  // Restaurar modulo activo
  ELECHOUSE_cc1101.setModul(tmp_module.toInt() - 1);
}

// scan <module> <start> <end> <step>: escaneo RSSI
void cmdScan(const String &modToken, float startFreq, float endFreq, float stepFreq) {
  int modIdx = parseModuleIndex(modToken);
  if (modIdx < 0) return;

  if (stepFreq <= 0.0) {
    Serial.println(F("ERR: scan step_must_be_positive"));
    return;
  }
  if (startFreq >= endFreq) {
    Serial.println(F("ERR: scan start_must_be_less_than_end"));
    return;
  }

  int steps = (int)((endFreq - startFreq) / stepFreq) + 1;
  if (steps > SCAN_MAX_STEPS) {
    Serial.print(F("ERR: scan too_many_steps="));
    Serial.print(steps);
    Serial.print(F(" max="));
    Serial.println(SCAN_MAX_STEPS);
    return;
  }

  if (!checkCC1101Module(modIdx)) return;

  // Detener RX si estaba activo
  bool rxWasActive = (raw_rx == "1");
  if (rxWasActive) {
    detachInterrupt(digitalPinToInterrupt(rx_pin1));
    detachInterrupt(digitalPinToInterrupt(rx_pin2));
  }

  Serial.print(F("OK: scan module="));
  Serial.print(modToken);
  Serial.print(F(" start="));  Serial.print(startFreq, 3);
  Serial.print(F(" end="));    Serial.print(endFreq, 3);
  Serial.print(F(" step="));   Serial.print(stepFreq, 3);
  Serial.print(F(" steps="));  Serial.println(steps);

  ELECHOUSE_cc1101.setModul(modIdx);
  cc1101Init();
  cc1101SetModulation(0);   // 2-FSK para scan
  cc1101SetSyncMode(0);
  cc1101SetRxBW(812);

  int doneSteps = 0;
  for (float f = startFreq; f <= endFreq + 0.0001f; f += stepFreq) {
    if (doneSteps >= SCAN_MAX_STEPS) break;
    cc1101SetMHZ(f);
    cc1101SetRx();
    delay(12);  // ~12 ms para estabilizar RSSI
    int rssi = ELECHOUSE_cc1101.getRssi();
    Serial.print(F("OK: scan freq="));
    Serial.print(f, 3);
    Serial.print(F(" rssi="));
    Serial.println(rssi);
    doneSteps++;
  }

  cc1101SetSidle();
  Serial.print(F("OK: scan done steps="));
  Serial.println(doneSteps);

  // Restaurar estado si RX estaba activo
  if (rxWasActive) {
    cc1101SetModulation(modulationMode);
    cc1101SetMHZ(frequency);
    cc1101SetRxBW(setrxbw);
    cc1101SetDRate(datarate);
    enableReceive();
  }
}

// replay <module>: retransmite el ultimo smooth capturado
void cmdReplay(const String &modToken) {
  int modIdx = parseModuleIndex(modToken);
  if (modIdx < 0) return;

  if (lastSmoothCount == 0) {
    Serial.println(F("ERR: replay no_captured_signal"));
    return;
  }

  if (!checkCC1101Module(modIdx)) return;

  int tx_pin = (modIdx == 0) ? tx_pin1 : tx_pin2;
  cc1101Init();
  cc1101SetModulation(lastSmoothMod);
  cc1101SetMHZ(lastSmoothFreq);
  cc1101SetDeviation(deviation);
  cc1101SetTx();
  pinMode(tx_pin, OUTPUT);

  // Transmitir alternando HIGH/LOW con los timings suavizados
  for (int i = 0; i < lastSmoothCount; i += 2) {
    digitalWrite(tx_pin, HIGH);
    delayMicroseconds((unsigned int)samplesmooth[i]);
    digitalWrite(tx_pin, LOW);
    if (i + 1 < lastSmoothCount) delayMicroseconds((unsigned int)samplesmooth[i + 1]);
  }
  cc1101SetSidle();

  Serial.print(F("OK: replay module="));
  Serial.print(modToken);
  Serial.print(F(" timings="));
  Serial.print(lastSmoothCount);
  Serial.print(F(" freq="));
  Serial.print(lastSmoothFreq, 5);
  Serial.print(F(" orig_module="));
  Serial.println(lastSmoothModule);
}

// ---- v2.10: JSON profile helpers ----

// writeProfileJson: escribe /profiles/<name>.json con los parametros dados
static bool writeProfileJson(const String &name, float freq_, float bw_,
                              int mod_, float dev_, int rate_, int power_,
                              const String &module_) {
  LittleFS.mkdir("/profiles");  // no-op si ya existe
  String path = "/profiles/" + name + ".json";
  if (LittleFS.exists(path.c_str())) {
    LittleFS.remove(path.c_str());
  }
  File f = LittleFS.open(path.c_str(), FILE_WRITE);
  if (!f) return false;
  f.println(F("{"));
  f.print(F("\"freq\": "));   f.print(freq_, 6);  f.println(F(","));
  f.print(F("\"bw\": "));     f.print(bw_, 3);    f.println(F(","));
  f.print(F("\"mod\": "));    f.print(mod_);       f.println(F(","));
  f.print(F("\"dev\": "));    f.print(dev_, 3);    f.println(F(","));
  f.print(F("\"rate\": "));   f.print(rate_);      f.println(F(","));
  f.print(F("\"power\": "));  f.print(power_);     f.println(F(","));
  f.print(F("\"module\": \"")); f.print(module_); f.println(F("\""));
  f.println(F("}"));
  f.close();
  return true;
}

// save <name>: guarda config activa en /profiles/<name>.json
void cmdSave(const String &name) {
  if (name.length() == 0 || name.length() > 20) {
    Serial.println(F("ERR: save name_invalid max_20_chars"));
    return;
  }
  for (unsigned int i = 0; i < name.length(); i++) {
    char c = name[i];
    if (!isAlphaNumeric(c) && c != '_' && c != '-') {
      Serial.println(F("ERR: save name_chars_invalid use_alphanumeric_-_"));
      return;
    }
  }

  String path = "/profiles/" + name + ".json";
  if (LittleFS.exists(path.c_str())) {
    Serial.print(F("OK: save overwriting existing path="));
    Serial.println(path);
  }

  if (!writeProfileJson(name, frequency, setrxbw, modulationMode,
                        deviation, datarate, power_jammer, tmp_module)) {
    Serial.print(F("ERR: save fs_open_failed path="));
    Serial.println(path);
    return;
  }

  Serial.print(F("OK: saved path="));
  Serial.print(path);
  Serial.print(F(" freq="));   Serial.print(frequency, 5);
  Serial.print(F(" bw="));     Serial.print(setrxbw, 3);
  Serial.print(F(" mod="));    Serial.print(modulationMode);
  Serial.print(F(" dev="));    Serial.print(deviation, 3);
  Serial.print(F(" rate="));   Serial.print(datarate);
  Serial.print(F(" power="));  Serial.println(power_jammer);
}

// load <name>: carga /profiles/<name>.json; fallback a /cfg_<name>.cfg (formato antiguo)
void cmdLoad(const String &name) {
  if (name.length() == 0 || name.length() > 20) {
    Serial.println(F("ERR: load name_invalid"));
    return;
  }

  String path = "/profiles/" + name + ".json";
  bool isJson = LittleFS.exists(path.c_str());
  if (!isJson) {
    path = "/cfg_" + name + ".cfg";
    if (!LittleFS.exists(path.c_str())) {
      Serial.print(F("ERR: load file_not_found name="));
      Serial.println(name);
      return;
    }
  }

  File f = LittleFS.open(path.c_str(), "r");
  if (!f) {
    Serial.print(F("ERR: load fs_open_failed path="));
    Serial.println(path);
    return;
  }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (isJson) {
      // Parsear "key": value[,]
      int q1 = line.indexOf('"');
      if (q1 < 0) continue;
      int q2 = line.indexOf('"', q1 + 1);
      if (q2 < 0) continue;
      String key = line.substring(q1 + 1, q2);
      int colon = line.indexOf(':', q2 + 1);
      if (colon < 0) continue;
      String val = line.substring(colon + 1);
      val.trim();
      if (val.endsWith(",")) val.remove(val.length() - 1);
      val.trim();
      if (val.startsWith("\"") && val.endsWith("\""))
        val = val.substring(1, val.length() - 1);
      if      (key == "freq")   frequency      = val.toFloat();
      else if (key == "bw")     setrxbw        = val.toFloat();
      else if (key == "mod")    modulationMode = val.toInt();
      else if (key == "dev")    deviation      = val.toFloat();
      else if (key == "rate")   datarate       = val.toInt();
      else if (key == "power")  power_jammer   = val.toInt();
      else if (key == "module") tmp_module     = val;
    } else {
      // Formato antiguo key=value
      int eq = line.indexOf('=');
      if (eq < 0) continue;
      String key = line.substring(0, eq);
      String val = line.substring(eq + 1);
      key.trim(); val.trim();
      if      (key == "freq")   frequency      = val.toFloat();
      else if (key == "bw")     setrxbw        = val.toFloat();
      else if (key == "mod")    modulationMode = val.toInt();
      else if (key == "dev")    deviation      = val.toFloat();
      else if (key == "rate")   datarate       = val.toInt();
      else if (key == "power")  power_jammer   = val.toInt();
      else if (key == "module") tmp_module     = val;
    }
  }
  f.close();

  Serial.print(F("OK: loaded path="));
  Serial.print(path);
  Serial.print(F(" freq="));   Serial.print(frequency, 5);
  Serial.print(F(" bw="));     Serial.print(setrxbw, 3);
  Serial.print(F(" mod="));    Serial.print(modulationMode);
  Serial.print(F(" dev="));    Serial.print(deviation, 3);
  Serial.print(F(" rate="));   Serial.print(datarate);
  Serial.print(F(" power="));  Serial.print(power_jammer);
  Serial.print(F(" module="));  Serial.println(tmp_module);
}

// profiles: lista perfiles en /profiles/
void cmdProfiles() {
  LittleFS.mkdir("/profiles");  // crea si no existe
  File dir = LittleFS.open("/profiles");
  if (!dir || !dir.isDirectory()) {
    Serial.println(F("OK: profiles none"));
    return;
  }
  int count = 0;
  Serial.println(F("OK: profiles:"));
  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      String fname = String(entry.name());
      int lastSlash = fname.lastIndexOf('/');
      if (lastSlash >= 0) fname = fname.substring(lastSlash + 1);
      if (fname.endsWith(".json")) fname.remove(fname.length() - 5);
      Serial.print(F("  "));
      Serial.print(fname);
      Serial.print(F("  ("));
      Serial.print(entry.size());
      Serial.println(F(" bytes)"));
      count++;
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  if (count == 0) Serial.println(F("  (ninguno)"));
  Serial.print(F("OK: profiles total="));
  Serial.println(count);
}

// profile-del <name>: elimina /profiles/<name>.json
void cmdProfileDel(const String &name) {
  if (name.length() == 0 || name.length() > 20) {
    Serial.println(F("ERR: profile-del name_invalid"));
    return;
  }
  String path = "/profiles/" + name + ".json";
  if (!LittleFS.exists(path.c_str())) {
    Serial.print(F("ERR: profile-del not_found path="));
    Serial.println(path);
    return;
  }
  if (LittleFS.remove(path.c_str())) {
    Serial.print(F("OK: profile-del deleted path="));
    Serial.println(path);
  } else {
    Serial.print(F("ERR: profile-del failed path="));
    Serial.println(path);
  }
}

// ============================================================
// export <name> <format>: exporta la ultima senal en URH o RTL
// ============================================================

void cmdExport(const String &nameToken, const String &fmtToken) {
  String name = nameToken; name.toLowerCase();
  String fmt  = fmtToken;  fmt.toLowerCase();

  if (name != "last") {
    Serial.println(F("ERR: export only 'last' is supported as name"));
    return;
  }
  if (lastRawCount == 0) {
    Serial.println(F("ERR: export no_signal_in_memory - capture a signal with 'rx' first"));
    return;
  }

  if (fmt == "urh") {
    // ---- URH pulse format: +HIGH_us -LOW_us +HIGH_us ... ----
    Serial.println(F("[EXPORT-URH-BEGIN]"));
    Serial.print(F("# Evil-Crow-RF export format=URH\n"));
    Serial.print(F("# freq_mhz="));      Serial.println(lastSmoothFreq, 5);
    Serial.print(F("# modulation="));    Serial.println(lastSmoothMod);
    Serial.print(F("# samples="));       Serial.println(lastRawCount);
    Serial.print(F("# captured_module=")); Serial.println(lastRawModule);
    // Pulses: index 0 = HIGH, 1 = LOW, alternating
    for (int i = 0; i < lastRawCount; i++) {
      if (i > 0) Serial.print(' ');
      Serial.print(i % 2 == 0 ? '+' : '-');
      Serial.print(lastRawSamples[i]);
    }
    Serial.println();
    Serial.println(F("[EXPORT-URH-END]"));
    Serial.print(F("OK: export format=urh samples="));
    Serial.print(lastRawCount);
    Serial.print(F(" freq="));
    Serial.println(lastSmoothFreq, 5);

  } else if (fmt == "rtl") {
    // ---- rtl_433 compatible JSON ----
    Serial.println(F("[EXPORT-RTL-BEGIN]"));
    Serial.print(F("{\"time\":\"@"));
    Serial.print(millis() / 1000.0, 3);
    Serial.print(F("s\",\"model\":\"Evil-Crow-RF\","));
    Serial.print(F("\"freq\":"));
    Serial.print((long)(lastSmoothFreq * 1000000.0));
    Serial.print(F(",\"modulation\":"));
    Serial.print(lastSmoothMod);
    Serial.print(F(",\"num_pulses\":"));
    Serial.print(lastRawCount);
    // Array de duraciones en microsegundos
    Serial.print(F(",\"pulses\":["));
    for (int i = 0; i < lastRawCount; i++) {
      if (i > 0) Serial.print(',');
      Serial.print(lastRawSamples[i]);
    }
    // Cadena de pulsos al estilo rtl_433 codes
    Serial.print(F("],\"codes\":\""));
    for (int i = 0; i < lastRawCount; i++) {
      if (i > 0) Serial.print(' ');
      Serial.print(i % 2 == 0 ? '+' : '-');
      Serial.print(lastRawSamples[i]);
    }
    Serial.println(F("\"}"));
    Serial.println(F("[EXPORT-RTL-END]"));
    Serial.print(F("OK: export format=rtl samples="));
    Serial.print(lastRawCount);
    Serial.print(F(" freq="));
    Serial.println(lastSmoothFreq, 5);

  } else {
    Serial.print(F("ERR: export format_unknown="));
    Serial.println(fmtToken);
    Serial.println(F("ERR: export available_formats=urh|rtl"));
  }
}

// ============================================================
// analyze <mod>: re-analiza la ultima senal capturada sin nueva captura
// ============================================================

void cmdAnalyze(const String &modToken) {
  if (lastRawCount == 0) {
    Serial.println(F("ERR: analyze no_captured_signal - capture a signal with 'rx' first"));
    return;
  }

  int newMod = modToken.toInt();
  if (newMod < 0 || newMod > 4) {
    Serial.print(F("ERR: analyze modulation_invalid="));
    Serial.println(modToken);
    Serial.println(F("ERR: analyze modulation must be 0=2-FSK 1=GFSK 2=ASK/OOK 3=4-FSK 4=MSK"));
    return;
  }

  // Restaurar raw en buffers globales usados por signalanalyse()
  samplecount = lastRawCount;
  for (int i = 0; i < lastRawCount; i++) sample[i] = lastRawSamples[i];
  // Aplicar la modulacion solicitada para que el analisis la tenga en cuenta
  modulationMode = newMod;

  Serial.print(F("OK: analyze samples="));
  Serial.print(lastRawCount);
  Serial.print(F(" mod="));
  Serial.print(newMod);
  Serial.print(F(" freq="));
  Serial.println(lastSmoothFreq, 5);

  signalanalyse();
}

// ============================================================
// debug <on|off>: activa/desactiva traza SPI de operaciones CC1101
// ============================================================

void cmdDebug(const String &stateToken) {
  String s = stateToken;
  s.toLowerCase();
  if (s == "on") {
    debugMode = true;
    Serial.println(F("OK: debug on"));
    Serial.println(F("OK: debug Cada operacion CC1101 emitira lineas prefijadas DBG:"));
    Serial.println(F("OK: debug   DBG: >> <funcion>(<param>)   <- operacion ejecutada"));
    Serial.println(F("OK: debug   DBG:   REG[0xNN]=0xVV (NOMBRE)  <- registro leido"));
    Serial.println(F("OK: debug   DBG:   STATUS[0xNN]=0xVV (NOMBRE) <- estado leido"));
  } else if (s == "off") {
    debugMode = false;
    Serial.println(F("OK: debug off"));
  } else {
    Serial.print(F("ERR: debug estado_invalido="));
    Serial.println(stateToken);
    Serial.println(F("ERR: debug uso: debug on | debug off"));
  }
}

// ============================================================
// registers <mod>: vuelca los 47 registros de configuracion CC1101
// ============================================================

// Tabla de nombres de registros CC1101 (0x00-0x2E)
static const char* const cc1101RegNames[47] = {
  "IOCFG2","IOCFG1","IOCFG0","FIFOTHR","SYNC1","SYNC0","PKTLEN",
  "PKTCTRL1","PKTCTRL0","ADDR","CHANNR","FSCTRL1","FSCTRL0",
  "FREQ2","FREQ1","FREQ0","MDMCFG4","MDMCFG3","MDMCFG2","MDMCFG1",
  "MDMCFG0","DEVIATN","MCSM2","MCSM1","MCSM0","FOCCFG","BSCFG",
  "AGCCTRL2","AGCCTRL1","AGCCTRL0","WOREVT1","WOREVT0","WORCTRL",
  "FREND1","FREND0","FSCAL3","FSCAL2","FSCAL1","FSCAL0","RCCTRL1",
  "RCCTRL0","FSTEST","PTEST","AGCTEST","TEST2","TEST1","TEST0"
};

void cmdRegisters(const String &modToken) {
  int modIdx = parseModuleIndex(modToken);
  if (modIdx < 0) return;
  if (!checkCC1101Module(modIdx)) return;

  ELECHOUSE_cc1101.setModul(modIdx);

  Serial.print(F("OK: registers module="));
  Serial.print(modToken);
  Serial.println(F(" count=47"));

  for (byte i = 0; i <= 0x2E; i++) {
    byte val = ELECHOUSE_cc1101.SpiReadReg(i);
    Serial.print(F("REG[0x"));
    if (i < 0x10) Serial.print('0');
    Serial.print(i, HEX);
    Serial.print(F("]=0x"));
    if (val < 0x10) Serial.print('0');
    Serial.print(val, HEX);
    Serial.print(' ');
    Serial.println(cc1101RegNames[i]);
  }
  Serial.println(F("OK: registers done"));

  // Restaurar modulo activo previo
  ELECHOUSE_cc1101.setModul(tmp_module.toInt() - 1);
}

// ============================================================
// meminfo: reporte de uso de memoria y sistema de archivos
// ============================================================

void cmdMeminfo() {
  uint32_t heapFree    = ESP.getFreeHeap();
  uint32_t heapMin     = ESP.getMinFreeHeap();
  uint32_t heapLargest = ESP.getMaxAllocHeap();
  // Fragmentacion: porcentaje del heap libre que NO es el bloque mas grande contiguo
  int fragPct = (heapFree > 0)
                ? (int)(100UL * (heapFree - heapLargest) / heapFree)
                : 0;
  // Stack HWM del task principal (FreeRTOS devuelve palabras de 4 bytes)
  uint32_t stackHwm = uxTaskGetStackHighWaterMark(NULL) * 4UL;

  size_t fsTotal = 0, fsUsed = 0;
  int fsFiles = 0;
  bool fsOk = LittleFS.begin(false);
  if (fsOk) {
    fsTotal = LittleFS.totalBytes();
    fsUsed  = LittleFS.usedBytes();
    File root = LittleFS.open("/");
    if (root) {
      File entry = root.openNextFile();
      while (entry) {
        fsFiles++;
        entry.close();
        entry = root.openNextFile();
      }
      root.close();
    }
  }

  Serial.println(F("OK: meminfo"));
  Serial.print(F("OK: heap_free="));          Serial.print(heapFree);      Serial.println(F(" bytes"));
  Serial.print(F("OK: heap_min_ever="));       Serial.print(heapMin);       Serial.println(F(" bytes"));
  Serial.print(F("OK: heap_largest_block="));  Serial.print(heapLargest);   Serial.println(F(" bytes"));
  Serial.print(F("OK: heap_frag="));           Serial.print(fragPct);       Serial.println(F("%"));
  Serial.print(F("OK: stack_hwm="));           Serial.print(stackHwm);      Serial.println(F(" bytes"));
  if (fsOk) {
    Serial.print(F("OK: fs_total="));  Serial.print(fsTotal);              Serial.println(F(" bytes"));
    Serial.print(F("OK: fs_used="));   Serial.print(fsUsed);               Serial.println(F(" bytes"));
    Serial.print(F("OK: fs_free="));   Serial.print(fsTotal - fsUsed);     Serial.println(F(" bytes"));
    Serial.print(F("OK: fs_files="));  Serial.println(fsFiles);
  } else {
    Serial.println(F("OK: fs=unavailable"));
  }

  // JSON compacto para parsers
  Serial.print(F("{\"event\":\"meminfo\",\"heap_free\":"));
  Serial.print(heapFree);
  Serial.print(F(",\"heap_min_ever\":"));
  Serial.print(heapMin);
  Serial.print(F(",\"heap_largest_block\":"));
  Serial.print(heapLargest);
  Serial.print(F(",\"heap_frag_pct\":"));
  Serial.print(fragPct);
  Serial.print(F(",\"stack_hwm_bytes\":"));
  Serial.print(stackHwm);
  if (fsOk) {
    Serial.print(F(",\"fs_total\":"));  Serial.print(fsTotal);
    Serial.print(F(",\"fs_used\":"));   Serial.print(fsUsed);
    Serial.print(F(",\"fs_files\":"));  Serial.print(fsFiles);
  }
  Serial.println(F("}"));
}

// ============================================================
// v2.5 — relay / bridge: dual-radio relay mode
// ============================================================

// relay <freq> <bw> <mod>: Mod1=RX, Mod2=TX, misma frecuencia
void cmdRelay(float rxFreq, float rxBw, int rxMod) {
  if (rxMod < 0 || rxMod > 4) {
    Serial.print(F("ERR: relay modulation_invalid="));
    Serial.println(rxMod);
    Serial.println(F("ERR: relay modulation: 0=2-FSK 1=GFSK 2=ASK/OOK 3=4-FSK 4=MSK"));
    return;
  }
  if (!checkCC1101Module(0)) return;  // Module 1
  if (!checkCC1101Module(1)) return;  // Module 2

  frequency      = rxFreq;
  setrxbw        = rxBw;
  modulationMode = rxMod;
  relayTxFreq    = rxFreq;  // same frequency for plain relay
  relayTxMod     = rxMod;

  // ── Configure Module 1 as RX ─────────────────────────────────────────────
  ELECHOUSE_cc1101.setModul(0);
  cc1101Init();
  if (rxMod == 2) { cc1101SetDcFilterOff(0); }
  else            { cc1101SetDcFilterOff(1); cc1101SetDeviation(deviation); }
  cc1101SetModulation(rxMod);
  cc1101SetMHZ(rxFreq);
  cc1101SetSyncMode(0);
  cc1101SetPktFormat(3);
  cc1101SetRxBW(rxBw);
  cc1101SetDRate(datarate);

  // ── Configure Module 2: init + same RF params, idle until TX needed ──────
  ELECHOUSE_cc1101.setModul(1);
  cc1101Init();
  cc1101SetModulation(rxMod);
  cc1101SetMHZ(rxFreq);
  cc1101SetDeviation(deviation);
  cc1101SetSidle();
  pinMode(tx_pin2, OUTPUT);

  // ── Arm relay mode ────────────────────────────────────────────────────────
  relayActive = true;
  raw_rx       = "1";
  rxStartTime  = millis();
  tmp_module   = "1";
  enableRelayReceive();

  Serial.print(F("OK: relay freq="));  Serial.print(rxFreq, 5);
  Serial.print(F(" bw="));             Serial.print(rxBw, 3);
  Serial.print(F(" mod="));            Serial.print(rxMod);
  Serial.println(F(" rx=mod1 tx=mod2"));
  Serial.print(F("{\"event\":\"relay_armed\",\"freq\":"));
  Serial.print(rxFreq, 5);
  Serial.print(F(",\"bw\":"));          Serial.print(rxBw, 3);
  Serial.print(F(",\"mod\":"));         Serial.print(rxMod);
  Serial.println(F(",\"rx\":\"mod1\",\"tx\":\"mod2\"}"));
}

// bridge <rx_freq> <tx_freq>: Mod1=RX at rx_freq, Mod2=TX at tx_freq
// Uses current modulation / bw / deviation / datarate globals
void cmdBridge(float rxFreq, float txFreq) {
  if (!checkCC1101Module(0)) return;
  if (!checkCC1101Module(1)) return;

  relayTxFreq = txFreq;
  relayTxMod  = modulationMode;
  frequency   = rxFreq;

  // ── Configure Module 1 as RX at rxFreq ───────────────────────────────────
  ELECHOUSE_cc1101.setModul(0);
  cc1101Init();
  if (modulationMode == 2) { cc1101SetDcFilterOff(0); }
  else                     { cc1101SetDcFilterOff(1); cc1101SetDeviation(deviation); }
  cc1101SetModulation(modulationMode);
  cc1101SetMHZ(rxFreq);
  cc1101SetSyncMode(0);
  cc1101SetPktFormat(3);
  cc1101SetRxBW(setrxbw);
  cc1101SetDRate(datarate);

  // ── Configure Module 2: init + TX params at txFreq, idle ─────────────────
  ELECHOUSE_cc1101.setModul(1);
  cc1101Init();
  cc1101SetModulation(modulationMode);
  cc1101SetMHZ(txFreq);
  cc1101SetDeviation(deviation);
  cc1101SetSidle();
  pinMode(tx_pin2, OUTPUT);

  // ── Arm bridge mode ───────────────────────────────────────────────────────
  relayActive = true;
  raw_rx       = "1";
  rxStartTime  = millis();
  tmp_module   = "1";
  enableRelayReceive();

  Serial.print(F("OK: bridge rx_freq="));  Serial.print(rxFreq, 5);
  Serial.print(F(" tx_freq="));            Serial.print(txFreq, 5);
  Serial.print(F(" mod="));               Serial.println(modulationMode);
  Serial.print(F("{\"event\":\"bridge_armed\",\"rx_freq\":"));
  Serial.print(rxFreq, 5);
  Serial.print(F(",\"tx_freq\":"));
  Serial.print(txFreq, 5);
  Serial.print(F(",\"mod\":"));
  Serial.print(modulationMode);
  Serial.println(F(",\"rx\":\"mod1\",\"tx\":\"mod2\"}"));
}

// ============================================================
// v2.6 — Storage management commands
// ============================================================

// list: todos los archivos en LittleFS con nombre, tamaño y fecha
void cmdList() {
  File root = LittleFS.open("/");
  if (!root || !root.isDirectory()) {
    Serial.println(F("ERR: list fs_open_failed"));
    return;
  }
  int count = 0;
  Serial.println(F("[LIST-BEGIN]"));
  File entry = root.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      time_t t = entry.getLastWrite();
      struct tm *tmInfo = gmtime(&t);
      char dateBuf[20];
      strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%dT%H:%M:%S", tmInfo);
      Serial.print(entry.name());
      Serial.print(F(" size="));
      Serial.print((unsigned long)entry.size());
      Serial.print(F(" date="));
      Serial.println(dateBuf);
      count++;
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
  Serial.print(F("[LIST-END count="));
  Serial.print(count);
  Serial.println(F("]"));
  Serial.print(F("OK: list files="));
  Serial.println(count);
}

// show <name>: vuelca el contenido raw de un archivo al Serial
void cmdShow(const String &name) {
  if (name.length() == 0) {
    Serial.println(F("ERR: show missing_name"));
    return;
  }
  String path = (name[0] == '/') ? name : "/" + name;
  if (!LittleFS.exists(path.c_str())) {
    Serial.print(F("ERR: show file_not_found path="));
    Serial.println(path);
    return;
  }
  File f = LittleFS.open(path.c_str(), "r");
  if (!f) {
    Serial.print(F("ERR: show fs_open_failed path="));
    Serial.println(path);
    return;
  }
  size_t sz = f.size();
  Serial.print(F("[FILE-BEGIN path="));
  Serial.print(path);
  Serial.print(F(" size="));
  Serial.print((unsigned long)sz);
  Serial.println(F("]"));
  while (f.available()) {
    Serial.write(f.read());
  }
  f.close();
  Serial.println(F("\n[FILE-END]"));
  Serial.print(F("OK: show path="));
  Serial.print(path);
  Serial.print(F(" size="));
  Serial.println((unsigned long)sz);
}

// delete <name>: elimina un archivo de LittleFS
void cmdFsDelete(const String &name) {
  if (name.length() == 0) {
    Serial.println(F("ERR: delete missing_name"));
    return;
  }
  String path = (name[0] == '/') ? name : "/" + name;
  if (!LittleFS.exists(path.c_str())) {
    Serial.print(F("ERR: delete file_not_found path="));
    Serial.println(path);
    return;
  }
  if (LittleFS.remove(path.c_str())) {
    Serial.print(F("OK: deleted path="));
    Serial.println(path);
  } else {
    Serial.print(F("ERR: delete failed path="));
    Serial.println(path);
  }
}

// rename <old> <new>: renombra un archivo en LittleFS
void cmdFsRename(const String &oldName, const String &newName) {
  if (oldName.length() == 0 || newName.length() == 0) {
    Serial.println(F("ERR: rename usage: rename <old> <new>"));
    return;
  }
  String oldPath = (oldName[0] == '/') ? oldName : "/" + oldName;
  String newPath = (newName[0] == '/') ? newName : "/" + newName;
  if (!LittleFS.exists(oldPath.c_str())) {
    Serial.print(F("ERR: rename src_not_found path="));
    Serial.println(oldPath);
    return;
  }
  if (LittleFS.exists(newPath.c_str())) {
    Serial.print(F("ERR: rename dst_exists path="));
    Serial.println(newPath);
    return;
  }
  if (LittleFS.rename(oldPath.c_str(), newPath.c_str())) {
    Serial.print(F("OK: renamed "));
    Serial.print(oldPath);
    Serial.print(F(" -> "));
    Serial.println(newPath);
  } else {
    Serial.print(F("ERR: rename failed "));
    Serial.print(oldPath);
    Serial.print(F(" -> "));
    Serial.println(newPath);
  }
}

// info: espacio total, usado y libre de LittleFS
void cmdFsInfo() {
  size_t total  = LittleFS.totalBytes();
  size_t used   = LittleFS.usedBytes();
  size_t freeBytes = total - used;
  int usedPct   = (total > 0) ? (int)(100UL * used / total) : 0;
  int fileCount = 0;
  File root = LittleFS.open("/");
  if (root && root.isDirectory()) {
    File e = root.openNextFile();
    while (e) {
      if (!e.isDirectory()) fileCount++;
      e.close();
      e = root.openNextFile();
    }
    root.close();
  }
  Serial.print(F("OK: info total="));   Serial.print((unsigned long)total);
  Serial.print(F(" used="));            Serial.print((unsigned long)used);
  Serial.print(F(" free="));            Serial.print((unsigned long)freeBytes);
  Serial.print(F(" used_pct="));        Serial.print(usedPct);
  Serial.print(F(" files="));           Serial.println(fileCount);
  Serial.print(F("{\"event\":\"fs_info\",\"total\":"));
  Serial.print((unsigned long)total);
  Serial.print(F(",\"used\":"));         Serial.print((unsigned long)used);
  Serial.print(F(",\"free\":"));         Serial.print((unsigned long)freeBytes);
  Serial.print(F(",\"used_pct\":"));     Serial.print(usedPct);
  Serial.print(F(",\"files\":"));        Serial.print(fileCount);
  Serial.println(F("}"));
}

// autodetect <module> <freq>: prueba mod x bw hasta capturar senal valida
void cmdAutodetect(const String &modToken, float freq) {
  int moduleIdx = parseModuleIndex(modToken);
  if (moduleIdx < 0) return;
  if (!checkCC1101Module(moduleIdx)) return;

  // Combinaciones a probar: mod primero por probabilidad (OOK mas comun)
  const int    MODS[]     = {2, 0, 3, 1};        // OOK, 2FSK, 4FSK, GFSK
  const float  BWS[]      = {812.0, 406.0, 203.0};
  const int    N_MODS     = 4;
  const int    N_BWS      = 3;
  const int    TEST_RATE  = 4;            // kbps fijo para todas las pruebas
  const unsigned long COMBO_TIMEOUT = 3000UL;  // ms por combinacion

  int   rx_pin = (moduleIdx == 0) ? rx_pin1 : rx_pin2;
  bool  savedRelay = relayActive;
  relayActive = true;  // suprime dual-pin check en receiver() ISR

  int   foundMod  = -1;
  float foundBw   = 0.0;

  for (int mi = 0; mi < N_MODS && foundMod < 0; mi++) {
    for (int bi = 0; bi < N_BWS && foundMod < 0; bi++) {
      int   m  = MODS[mi];
      float bw = BWS[bi];

      Serial.print(F("TRY: mod="));
      Serial.print(m);
      Serial.print(F(" bw="));
      Serial.println((int)bw);

      // Configurar CC1101
      ELECHOUSE_cc1101.setModul(moduleIdx);
      cc1101Init();
      if (m == 2) { cc1101SetDcFilterOff(0); }
      else        { cc1101SetDcFilterOff(1); cc1101SetDeviation(47.6); }
      cc1101SetModulation(m);
      cc1101SetMHZ(freq);
      cc1101SetSyncMode(0);
      cc1101SetPktFormat(3);
      cc1101SetRxBW(bw);
      cc1101SetDRate(TEST_RATE);

      // Armar recepcion
      samplecount = 0;
      pinMode(rx_pin, INPUT);
      cc1101SetRx();
      attachInterrupt(digitalPinToInterrupt(rx_pin), receiver, CHANGE);

      // Esperar hasta 3 s
      unsigned long t0 = millis();
      bool captured = false;
      while (millis() - t0 < COMBO_TIMEOUT) {
        if (checkReceived()) { captured = true; break; }
        delay(1);
      }

      // Garantizar que el interrupt quede desconectado siempre:
      // checkReceived() con relayActive=true solo detach rx_pin1;
      // si usamos rx_pin2 (moduleIdx=1, captured=true) necesitamos
      // desconectarlo explicitamente aqui tambien.
      detachInterrupt(digitalPinToInterrupt(rx_pin));
      ELECHOUSE_cc1101.setModul(moduleIdx);
      cc1101SetSidle();

      if (captured) {
        foundMod = m;
        foundBw  = bw;
      }
    }
  }

  relayActive = savedRelay;

  if (foundMod >= 0) {
    // Actualizar globales para que replay/analyze/export funcionen
    modulationMode = foundMod;
    setrxbw        = foundBw;
    frequency      = freq;
    datarate       = TEST_RATE;
    tmp_module     = modToken;

    Serial.print(F("FOUND: mod="));   Serial.print(foundMod);
    Serial.print(F(" bw="));          Serial.print((int)foundBw);
    Serial.print(F(" rate="));        Serial.println(TEST_RATE);

    Serial.print(F("{\"event\":\"autodetect_found\",\"mod\":"));
    Serial.print(foundMod);
    Serial.print(F(",\"bw\":"));      Serial.print((int)foundBw);
    Serial.print(F(",\"rate\":"));    Serial.print(TEST_RATE);
    Serial.print(F(",\"freq\":"));    Serial.print(freq, 5);
    Serial.print(F(",\"samples\":")); Serial.print(samplecount);
    Serial.println(F("}"));
    Serial.println(F("AUTODETECT-LISTO"));
  } else {
    Serial.print(F("ERR: autodetect no_signal_found freq="));
    Serial.println(freq, 5);
  }
}

// brute <module> <freq> <bits> <delay_ms>: transmite todos los codigos de bits bits
void cmdBrute(const String &modToken, float freq, int bits, unsigned long delayMs) {
  if (bits < 1 || bits > 24) {
    Serial.print(F("ERR: brute bits_out_of_range requested="));
    Serial.print(bits);
    Serial.println(F(" max=24"));
    return;
  }

  int moduleIdx = parseModuleIndex(modToken);
  if (moduleIdx < 0) return;
  if (!checkCC1101Module(moduleIdx)) return;

  unsigned long total = 1UL << bits;   // 2^bits codigos
  int tx_pin = (moduleIdx == 0) ? tx_pin1 : tx_pin2;

  // Configurar CC1101 para TX con modulacion activa a la freq indicada
  ELECHOUSE_cc1101.setModul(moduleIdx);
  cc1101Init();
  cc1101SetModulation(modulationMode);
  cc1101SetMHZ(freq);
  cc1101SetTx();
  pinMode(tx_pin, OUTPUT);
  digitalWrite(tx_pin, LOW);

  Serial.print(F("OK: brute module="));  Serial.print(modToken);
  Serial.print(F(" freq="));             Serial.print(freq, 5);
  Serial.print(F(" bits="));             Serial.print(bits);
  Serial.print(F(" total="));            Serial.print(total);
  Serial.print(F(" delay_ms="));         Serial.println(delayMs);
  Serial.println(F("{\"event\":\"brute_started\"}"));

  bruteActive = true;

  // Buffer local para leer "stopbrute" desde Serial durante el loop
  char  stopBuf[16];
  int   stopPos = 0;
  memset(stopBuf, 0, sizeof(stopBuf));

  // Timing OOK: bit1 = pulso largo, bit0 = pulso corto (MSB primero)
  const unsigned int T1H = 600;  // us HIGH para bit 1
  const unsigned int T1L = 200;  // us LOW  para bit 1
  const unsigned int T0H = 200;  // us HIGH para bit 0
  const unsigned int T0L = 600;  // us LOW  para bit 0

  unsigned long n = 0;
  for (n = 0; n < total && bruteActive; n++) {
    // --- Transmitir codigo n (MSB primero) ---
    for (int b = bits - 1; b >= 0; b--) {
      if ((n >> b) & 1UL) {
        digitalWrite(tx_pin, HIGH); delayMicroseconds(T1H);
        digitalWrite(tx_pin, LOW);  delayMicroseconds(T1L);
      } else {
        digitalWrite(tx_pin, HIGH); delayMicroseconds(T0H);
        digitalWrite(tx_pin, LOW);  delayMicroseconds(T0L);
      }
    }
    digitalWrite(tx_pin, LOW);  // estado final LOW

    // --- Progreso cada 1000 codigos (y en el primero) ---
    if (n == 0 || (n + 1) % 1000 == 0) {
      char hexbuf[8];
      snprintf(hexbuf, sizeof(hexbuf), "%06X", (unsigned int)n);
      Serial.print(F("BRUTE: "));
      Serial.print(n + 1);
      Serial.print('/');
      Serial.print(total);
      Serial.print(F(" ultimo=0x"));
      Serial.println(hexbuf);
    }

    // --- Delay entre codigos + polling Serial para stopbrute ---
    unsigned long t0 = millis();
    for (;;) {
      while (Serial.available()) {
        char c = (char)tolower(Serial.read());
        if (c == '\n' || c == '\r') {
          stopBuf[stopPos] = '\0';
          if (strcmp(stopBuf, "stopbrute") == 0) {
            bruteActive = false;
          }
          stopPos = 0;
        } else if (stopPos < (int)sizeof(stopBuf) - 1) {
          stopBuf[stopPos++] = c;
        }
      }
      if (!bruteActive) break;
      if (millis() - t0 >= delayMs) break;
      delay(1);
    }
    yield();  // cede al scheduler FreeRTOS; previene WDT reset
  }

  digitalWrite(tx_pin, LOW);
  cc1101SetSidle();
  bruteActive = false;

  if (n >= total) {
    Serial.print(F("OK: brute done total="));
    Serial.println(total);
  } else {
    Serial.print(F("OK: brute stopped at="));
    Serial.print(n);
    Serial.print('/');
    Serial.println(total);
  }
  Serial.println(F("BRUTE-LISTO"));
}

// ============================================================
// Parser de comandos serie
// ============================================================

void processSerialCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) {
    printPrompt();
    return;
  }

  // Tokenizar por espacios (max 20 tokens)
  String tokens[20];
  int tokenCount = 0;
  int start = 0;
  for (unsigned int i = 0; i <= cmd.length(); i++) {
    if (i == cmd.length() || cmd[i] == ' ') {
      if (i > (unsigned int)start) {
        tokens[tokenCount++] = cmd.substring(start, i);
        if (tokenCount >= 20) break;
      }
      start = i + 1;
    }
  }
  if (tokenCount == 0) return;

  String command = tokens[0];
  command.toLowerCase();

  // ---- Comandos originales (v2.0, prefijo JSON) ----

  if (command == "help") {
    printHelp();

  } else if (command == "status") {
    printStatus();

  } else if (command == "stoprx") {
    detachInterrupt(digitalPinToInterrupt(rx_pin1));
    detachInterrupt(digitalPinToInterrupt(rx_pin2));
    ELECHOUSE_cc1101.setModul(0); cc1101SetSidle();
    ELECHOUSE_cc1101.setModul(1); cc1101SetSidle();
    raw_rx      = "0";
    relayActive = false;
    Serial.println(F("{\"status\":\"ok\",\"cmd\":\"stoprx\",\"msg\":\"RX detenido\"}"));

  } else if (command == "rx") {
    if (tokenCount < 7) {
      Serial.println(F("{\"status\":\"error\",\"cmd\":\"rx\",\"msg\":\"Uso: rx <module> <freq> <bw> <modulation> <deviation> <datarate>\"}"));
      printPrompt(); return;
    }
    int moduleIdx = parseModuleIndex(tokens[1]);
    if (moduleIdx < 0) { printPrompt(); return; }

    float rxFreq = tokens[2].toFloat();
    float rxBw   = tokens[3].toFloat();
    int   rxMod  = tokens[4].toInt();
    float rxDev  = tokens[5].toFloat();
    int   rxRate = tokens[6].toInt();

    if (rxMod < 0 || rxMod > 4) {
      Serial.println(F("{\"status\":\"error\",\"cmd\":\"rx\",\"msg\":\"modulation fuera de rango: 0-4\"}"));
      printPrompt(); return;
    }
    if (!checkCC1101Module(moduleIdx)) { printPrompt(); return; }

    frequency = rxFreq; setrxbw = rxBw;
    modulationMode = rxMod; deviation = rxDev; datarate = rxRate;

    cc1101Init();
    if (modulationMode == 2) { cc1101SetDcFilterOff(0); }
    else { cc1101SetDcFilterOff(1); cc1101SetDeviation(deviation); }
    cc1101SetModulation(modulationMode);
    cc1101SetMHZ(frequency);
    cc1101SetSyncMode(0);
    cc1101SetPktFormat(3);
    cc1101SetRxBW(setrxbw);
    cc1101SetDRate(datarate);
    tmp_module = tokens[1]; rxStartTime = millis(); raw_rx = "1";
    enableReceive();

    Serial.print(F("{\"status\":\"ok\",\"cmd\":\"rx\",\"module\":"));
    Serial.print(tokens[1]);
    Serial.print(F(",\"freq\":"));     Serial.print(frequency);
    Serial.print(F(",\"bw\":"));       Serial.print(setrxbw);
    Serial.print(F(",\"modulation\":"));  Serial.print(modulationMode);
    Serial.print(F(",\"deviation\":"));  Serial.print(deviation);
    Serial.print(F(",\"datarate\":"));   Serial.print(datarate);
    Serial.print(F(",\"timeout_ms\":"));  Serial.print(RX_TIMEOUT_MS);
    Serial.println(F("}"));

  } else if (command == "tx") {
    if (tokenCount < 6) {
      Serial.println(F("{\"status\":\"error\",\"cmd\":\"tx\",\"msg\":\"Uso: tx <module> <freq> <modulation> <deviation> <rawdata>\"}"));
      printPrompt(); return;
    }
    int modIdx = parseModuleIndex(tokens[1]);
    if (modIdx < 0) { printPrompt(); return; }

    float txFreq = tokens[2].toFloat();
    int   txMod  = tokens[3].toInt();
    float txDev  = tokens[4].toFloat();

    if (txMod < 0 || txMod > 4) {
      Serial.println(F("{\"status\":\"error\",\"cmd\":\"tx\",\"msg\":\"modulation fuera de rango: 0-4\"}"));
      printPrompt(); return;
    }

    String rawdata = "";
    for (int i = 5; i < tokenCount; i++) rawdata += tokens[i];
    if (rawdata.length() == 0) {
      Serial.println(F("{\"status\":\"error\",\"cmd\":\"tx\",\"msg\":\"rawdata vacio\"}"));
      printPrompt(); return;
    }

    int counter = 0, pos = 0;
    bool bufOverflow = false;
    for (unsigned int i = 0; i <= rawdata.length(); i++) {
      if (i == rawdata.length() || rawdata[i] == ',') {
        if (counter >= 2000) { bufOverflow = true; break; }
        data_to_send[counter++] = rawdata.substring(pos, i).toInt();
        pos = i + 1;
      }
    }
    if (bufOverflow) {
      Serial.println(F("{\"status\":\"error\",\"cmd\":\"tx\",\"msg\":\"rawdata supera limite de 2000\"}"));
      printPrompt(); return;
    }
    if (!checkCC1101Module(modIdx)) { printPrompt(); return; }

    tmp_module = tokens[1]; frequency = txFreq; modulationMode = txMod; deviation = txDev;
    int tx_pin = (modIdx == 0) ? tx_pin1 : tx_pin2;
    cc1101Init();
    cc1101SetModulation(modulationMode);
    cc1101SetMHZ(frequency);
    cc1101SetDeviation(deviation);
    cc1101SetTx();
    pinMode(tx_pin, OUTPUT);
    for (int i = 0; i < counter; i += 2) {
      digitalWrite(tx_pin, HIGH); delayMicroseconds(data_to_send[i]);
      digitalWrite(tx_pin, LOW);
      if (i + 1 < counter) delayMicroseconds(data_to_send[i + 1]);
    }
    cc1101SetSidle();

    Serial.print(F("{\"status\":\"ok\",\"cmd\":\"tx\",\"module\":"));
    Serial.print(tokens[1]);
    Serial.print(F(",\"freq\":"));        Serial.print(frequency);
    Serial.print(F(",\"modulation\":"));  Serial.print(modulationMode);
    Serial.print(F(",\"timings_sent\":"));  Serial.print(counter);
    Serial.println(F("}"));

  } else if (command == "jammer") {
    if (tokenCount < 4) {
      Serial.println(F("{\"status\":\"error\",\"cmd\":\"jammer\",\"msg\":\"Uso: jammer <module> <freq> <power>\"}"));
      printPrompt(); return;
    }
    int modIdx = parseModuleIndex(tokens[1]);
    if (modIdx < 0) { printPrompt(); return; }

    float jamFreq  = tokens[2].toFloat();
    int   jamPower = tokens[3].toInt();
    if (!checkCC1101Module(modIdx)) { printPrompt(); return; }

    tmp_module = tokens[1]; frequency = jamFreq; power_jammer = jamPower;
    int tx_pin = (modIdx == 0) ? tx_pin1 : tx_pin2;
    pinMode(tx_pin, OUTPUT);
    cc1101Init();
    cc1101SetModulation(2);
    cc1101SetMHZ(frequency);
    cc1101SetPA(power_jammer);
    cc1101SetTx();
    jammer_tx = "1"; lastJammerHeartbeat = millis();

    Serial.print(F("{\"status\":\"ok\",\"cmd\":\"jammer\",\"module\":"));
    Serial.print(tokens[1]);
    Serial.print(F(",\"freq\":"));       Serial.print(frequency);
    Serial.print(F(",\"power_dbm\":"));  Serial.print(power_jammer);
    Serial.println(F("}"));

  } else if (command == "stopjammer") {
    ELECHOUSE_cc1101.setModul(0); cc1101SetSidle();
    ELECHOUSE_cc1101.setModul(1); cc1101SetSidle();
    jammer_tx = "0";
    Serial.println(F("{\"status\":\"ok\",\"cmd\":\"stopjammer\",\"msg\":\"Jammer detenido\"}"));

  } else if (command == "log") {
    if (LittleFS.exists("/logs.txt")) {
      File f = LittleFS.open("/logs.txt", "r");
      size_t fsize = f.size();
      Serial.print(F("[LOG-BEGIN size=")); Serial.print(fsize); Serial.println(F("]"));
      while (f.available()) Serial.write(f.read());
      f.close();
      Serial.println(F("\n[LOG-END]"));
    } else {
      Serial.println(F("{\"status\":\"ok\",\"cmd\":\"log\",\"msg\":\"Sin log disponible\"}"));
    }

  } else if (command == "clearlog") {
    if (deleteFile(LittleFS, "/logs.txt")) {
      Serial.println(F("{\"status\":\"ok\",\"cmd\":\"clearlog\",\"msg\":\"Log borrado\"}"));
    } else {
      Serial.println(F("{\"status\":\"error\",\"cmd\":\"clearlog\",\"msg\":\"No se pudo borrar el log\"}"));
    }

  } else if (command == "reboot") {
    Serial.println(F("{\"status\":\"ok\",\"cmd\":\"reboot\",\"msg\":\"Reiniciando\"}"));
    Serial.flush(); delay(300); ESP.restart();

  // ---- Comandos nuevos v2.1 (prefijo OK:/ERR:) ----

  } else if (command == "freqw") {
    if (tokenCount < 2) {
      Serial.println(F("ERR: freqw missing_module"));
    } else {
      cmdFreqw(tokens[1]);
    }

  } else if (command == "raw") {
    if (tokenCount < 2) {
      Serial.println(F("ERR: raw missing_module"));
    } else {
      cmdRaw(tokens[1]);
    }

  } else if (command == "config") {
    cmdConfig();

  } else if (command == "scan") {
    if (tokenCount < 5) {
      Serial.println(F("ERR: scan usage: scan <module> <start_MHz> <end_MHz> <step_MHz>"));
    } else {
      cmdScan(tokens[1], tokens[2].toFloat(), tokens[3].toFloat(), tokens[4].toFloat());
    }

  } else if (command == "replay") {
    if (tokenCount < 2) {
      Serial.println(F("ERR: replay missing_module"));
    } else {
      cmdReplay(tokens[1]);
    }

  } else if (command == "save") {
    if (tokenCount < 2) {
      Serial.println(F("ERR: save missing_name"));
    } else {
      cmdSave(tokens[1]);
    }

  } else if (command == "load") {
    if (tokenCount < 2) {
      Serial.println(F("ERR: load missing_name"));
    } else {
      cmdLoad(tokens[1]);
    }

  } else if (command == "export") {
    if (tokenCount < 3) {
      Serial.println(F("ERR: export usage: export <name> <format>"));
      Serial.println(F("ERR: export examples: export last urh  |  export last rtl"));
    } else {
      cmdExport(tokens[1], tokens[2]);
    }

  } else if (command == "analyze") {
    if (tokenCount < 2) {
      Serial.println(F("ERR: analyze usage: analyze <modulation>"));
      Serial.println(F("ERR: analyze modulation: 0=2-FSK 1=GFSK 2=ASK/OOK 3=4-FSK 4=MSK"));
    } else {
      cmdAnalyze(tokens[1]);
    }

  } else if (command == "debug") {
    if (tokenCount < 2) {
      Serial.println(F("ERR: debug usage: debug on | debug off"));
    } else {
      cmdDebug(tokens[1]);
    }

  } else if (command == "registers") {
    if (tokenCount < 2) {
      Serial.println(F("ERR: registers usage: registers <module>"));
    } else {
      cmdRegisters(tokens[1]);
    }

  } else if (command == "meminfo") {
    cmdMeminfo();

  } else if (command == "relay") {
    if (tokenCount < 4) {
      Serial.println(F("ERR: relay usage: relay <freq_MHz> <bw_kHz> <modulation>"));
      Serial.println(F("ERR: relay example: relay 433.92 812 2  (ASK/OOK)"));
      Serial.println(F("ERR: relay modulation: 0=2-FSK 1=GFSK 2=ASK/OOK 3=4-FSK 4=MSK"));
    } else {
      cmdRelay(tokens[1].toFloat(), tokens[2].toFloat(), tokens[3].toInt());
    }

  } else if (command == "bridge") {
    if (tokenCount < 3) {
      Serial.println(F("ERR: bridge usage: bridge <rx_freq_MHz> <tx_freq_MHz>"));
      Serial.println(F("ERR: bridge example: bridge 433.92 868.35"));
      Serial.println(F("ERR: bridge uses current mod/bw/dev/rate — set with 'config' or 'load'"));
    } else {
      cmdBridge(tokens[1].toFloat(), tokens[2].toFloat());
    }

  // ---- Comandos nuevos v2.6 (storage management) ----

  } else if (command == "list") {
    cmdList();

  } else if (command == "show") {
    if (tokenCount < 2) {
      Serial.println(F("ERR: show usage: show <name>"));
    } else {
      cmdShow(tokens[1]);
    }

  } else if (command == "delete") {
    if (tokenCount < 2) {
      Serial.println(F("ERR: delete usage: delete <name>"));
    } else {
      cmdFsDelete(tokens[1]);
    }

  } else if (command == "rename") {
    if (tokenCount < 3) {
      Serial.println(F("ERR: rename usage: rename <old> <new>"));
    } else {
      cmdFsRename(tokens[1], tokens[2]);
    }

  } else if (command == "info") {
    cmdFsInfo();

  // ---- Comandos nuevos v2.7 (autodetect) ----

  } else if (command == "autodetect") {
    if (tokenCount < 3) {
      Serial.println(F("ERR: autodetect usage: autodetect <module> <freq_MHz>"));
      Serial.println(F("ERR: autodetect example: autodetect 1 433.92"));
    } else {
      cmdAutodetect(tokens[1], tokens[2].toFloat());
    }

  // ---- Comandos nuevos v2.8 (brute force) ----

  } else if (command == "brute") {
    if (tokenCount < 5) {
      Serial.println(F("ERR: brute usage: brute <module> <freq_MHz> <bits> <delay_ms>"));
      Serial.println(F("ERR: brute example: brute 1 433.92 12 50"));
    } else {
      int bits = tokens[3].toInt();
      if (bits > 24) {
        Serial.print(F("ERR: brute bits>24 max=24 requested="));
        Serial.println(bits);
      } else {
        cmdBrute(tokens[1], tokens[2].toFloat(), bits, (unsigned long)tokens[4].toInt());
      }
    }

  } else if (command == "stopbrute") {
    bruteActive = false;
    Serial.println(F("OK: stopbrute signal_sent"));

  // ---- Comandos nuevos v2.10 (profiles system) ----

  } else if (command == "profiles") {
    cmdProfiles();

  } else if (command == "profile-del") {
    if (tokenCount < 2) {
      Serial.println(F("ERR: profile-del usage: profile-del <name>"));
    } else {
      cmdProfileDel(tokens[1]);
    }

  } else {
    Serial.print(F("{\"status\":\"error\",\"cmd\":\"unknown\",\"input\":\""));
    Serial.print(command);
    Serial.println(F("\",\"msg\":\"Comando no reconocido. Escribe help.\"}"));
  }

  printPrompt();
}

// GAP-18: eco de caracteres y manejo de backspace
void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      Serial.println();
      processSerialCommand(serialBuffer);
      serialBuffer = "";
    } else if (c == '\b' || c == 0x7F) {
      if (serialBuffer.length() > 0) {
        serialBuffer.remove(serialBuffer.length() - 1);
        Serial.print(F("\b \b"));
      }
    } else {
      serialBuffer += c;
      Serial.print(c);
    }
  }
}

// ============================================================
// Setup & Loop
// ============================================================

void setup() {
  Serial.begin(115200);

  if (!LittleFS.begin(true)) {
    Serial.println(F("{\"event\":\"error\",\"msg\":\"LittleFS fallo al montar. Iniciando formato...\"}"));
    Serial.flush();
    LittleFS.format();
    Serial.println(F("{\"event\":\"error\",\"msg\":\"Formato completado. Reiniciando dispositivo...\"}"));
    Serial.flush();
    delay(1000);
    ESP.restart();
  }

  ELECHOUSE_cc1101.addSpiPin(sck_pin, miso_pin, mosi_pin, cs_pin1, 0);
  ELECHOUSE_cc1101.addSpiPin(sck_pin, miso_pin, mosi_pin, cs_pin2, 1);

  // v2.10: crear perfiles predefinidos si no existen
  LittleFS.mkdir("/profiles");
  if (!LittleFS.exists("/profiles/default433.json"))
    writeProfileJson("default433", 433.920, 812.0, 2, 47.6, 4, 10, "1");
  if (!LittleFS.exists("/profiles/default868.json"))
    writeProfileJson("default868", 868.350, 812.0, 2, 47.6, 4, 10, "1");
  if (!LittleFS.exists("/profiles/fsk433.json"))
    writeProfileJson("fsk433",     433.920, 812.0, 0, 47.6, 4, 10, "1");

  // GAP-17: NO enableReceive() en setup — el usuario usa 'rx' para iniciar
  Serial.println(F("{\"event\":\"ready\",\"fw\":\"2.10\",\"msg\":\"Evil Crow RF listo\",\"hint\":\"Escribe help\"}"));
  printPrompt();
}

void loop() {
  handleSerial();

  if (raw_rx == "1") {
    if (checkReceived()) {
      if (relayActive) {
        // Relay/Bridge: retransmit immediately on Module 2, then re-arm Module 1
        int captured = samplecount;
        relayTransmit(captured);
        Serial.print(F("OK: RELAY "));
        Serial.println(captured);
        enableRelayReceive();
      } else {
        // Normal RX: log, analyse, prompt
        Serial.println(F("{\"event\":\"rx_captured\",\"msg\":\"Senal capturada - analizando\"}"));
        printReceived();
        signalanalyse();
        enableReceive();
        printPrompt();
        delay(700);
      }
      rxStartTime = millis();
    }
    if (millis() - rxStartTime > RX_TIMEOUT_MS) {
      Serial.print(F("{\"event\":\"rx_timeout\",\"timeout_ms\":"));
      Serial.print(RX_TIMEOUT_MS);
      Serial.println(F(",\"msg\":\"Sin senal. RX sigue activo.\"}"));
      rxStartTime = millis();
    }
  }

  if (jammer_tx == "1") {
    int tx_pin = (tmp_module == "1") ? tx_pin1 : tx_pin2;
    for (unsigned int i = 0; i + 1 < jammer_len; i += 2) {
      digitalWrite(tx_pin, HIGH); delayMicroseconds(jammer[i]);
      digitalWrite(tx_pin, LOW);  delayMicroseconds(jammer[i + 1]);
    }
    if (millis() - lastJammerHeartbeat > JAMMER_HEARTBEAT_MS) {
      Serial.print(F("{\"event\":\"jammer_heartbeat\",\"freq\":"));
      Serial.print(frequency);
      Serial.print(F(",\"power_dbm\":"));   Serial.print(power_jammer);
      Serial.print(F(",\"uptime_ms\":"));   Serial.print(millis());
      Serial.println(F("}"));
      lastJammerHeartbeat = millis();
    }
  }
}
