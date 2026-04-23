// ============================================================================
// Marstek Venus E3 Controller — ESP32 authoritative
// LilyGO T-CAN485 | REST API | 15-min schedule | self-consumption + max modes
// ----------------------------------------------------------------------------
// Design: the Marstek is treated as a dumb actuator. All policy (mode per
// 15-min slot, caps, meter IP, tuning) lives in ESP32 NVS. Every cycle the
// ESP32 re-asserts its desired state, so any register the Marstek silently
// drops is restored within one cycle.
// ============================================================================

#define MODBUSRTU_TIMEOUT 2000

#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <ModbusRTU.h>
#include <Preferences.h>
#include <time.h>

// ============================================================================
// Hardware (LilyGO T-CAN485)
// ============================================================================
#define SLAVE_ID      1
#define BAUDRATE      115200
#define SERIAL_CONFIG SERIAL_8N1
#define RX_PIN        21
#define TX_PIN        22
#define RS485_EN_PIN  17
#define RS485_SE_PIN  19
#define RS485_5V_PIN  16

// ============================================================================
// WiFi
// ============================================================================
const char* ssid     = "shittyrobot";
const char* password = "sHitty120";

IPAddress staticIP(192, 168, 2, 102);
IPAddress gateway(192, 168, 2, 254);
IPAddress subnet(255, 255, 255, 0);

// ============================================================================
// Time — Europe/Amsterdam (CET/CEST with DST)
// ============================================================================
#define TZ_STRING     "CET-1CEST,M3.5.0,M10.5.0/3"
#define NTP_SERVER_1  "pool.ntp.org"
#define NTP_SERVER_2  "time.google.com"

// ============================================================================
// Timing
// ============================================================================
const unsigned long SLOW_CYCLE_MS       = 60000;
const unsigned long INTER_TXN_MS        = 50;
const unsigned long WIFI_RECONNECT_MS   = 30000;
const unsigned long ERROR_BACKOFF_BASE  = 2000;
const unsigned long ERROR_BACKOFF_MAX   = 30000;
const uint8_t       ERROR_LIMIT         = 5;
const unsigned long METER_POLL_MS       = 2000;
const unsigned long METER_HTTP_TIMEOUT  = 2000;
const unsigned long METER_STALE_MS      = 10000;

// ============================================================================
// Marstek v3 register addresses
// ============================================================================
// Control
#define REG_RS485_ENABLE     42000
#define REG_FORCE_MODE       42010
#define REG_TARGET_SOC       42011
#define REG_CHARGE_POWER     42020
#define REG_DISCHARGE_POWER  42021
#define REG_WORK_MODE        43000
#define REG_CHARGE_CUTOFF    44000  // Hard BMS cap; scale ÷10 (1000 = 100%)
#define REG_DISCHARGE_CUTOFF 44001  // Hard BMS floor; scale ÷10 (100 = 10%)
#define REG_MAX_CHARGE       44002
#define REG_MAX_DISCHARGE    44003

// Monitoring — battery (32100-32105)
#define REG_BATT_VOLTAGE     32100
#define REG_BATT_CURRENT     32101
#define REG_BATT_POWER       32102
#define REG_BATT_SOC         32104
#define REG_BATT_TEMP1       32105

// Monitoring — AC (32200-32203)
#define REG_AC_VOLTAGE       32200
#define REG_AC_CURRENT       32201
#define REG_AC_POWER         32202
#define REG_AC_FREQ          32203

// Temperatures (32106-32110)
#define REG_TEMPS_START      32106
#define REG_TEMPS_COUNT      5

// Lifetime energy counters (uint32 each, raw × 0.01 = kWh)
#define REG_TOTAL_CHARGE_ENERGY     33000  // regs 33000+33001
#define REG_TOTAL_DISCHARGE_ENERGY  33002  // regs 33002+33003

// Magic
#define RS485_ENABLE_MAGIC   21930  // 0x55AA

// ============================================================================
// Types
// ============================================================================
struct WriteEntry {
  uint16_t addr;
  uint16_t value;
};

enum BatteryMode : uint8_t {
  MODE_OFF              = 0,
  MODE_MAX_CHARGE       = 1,
  MODE_MAX_DISCHARGE    = 2,
  MODE_SELF_CONSUMPTION = 3,
};

#define SCHEDULE_SLOTS 96  // 24h × 4 slots/hr

struct Config {
  uint8_t  schedule[SCHEDULE_SLOTS];
  uint16_t max_charge_w;
  uint16_t max_discharge_w;
  // self-consumption tuning
  uint8_t  sc_min_soc;
  uint8_t  sc_max_soc;
  float    sc_offset_w;
  float    sc_smoothing;
  float    sc_deadband_w;
  // meter
  char     meter_ip[40];
  // timing
  unsigned long fast_cycle_ms;
};

enum PollState {
  STATE_STARTUP_RS485,       STATE_STARTUP_WAIT_RS485,
  STATE_STARTUP_WORKMODE,    STATE_STARTUP_WAIT_WORKMODE,
  STATE_STARTUP_MAXCHARGE,   STATE_STARTUP_WAIT_MAXCHARGE,
  STATE_STARTUP_MAXDISCHARGE,STATE_STARTUP_WAIT_MAXDISCHARGE,

  STATE_IDLE,
  STATE_SEND_BATT,           STATE_WAIT_BATT,
  STATE_SEND_AC,             STATE_WAIT_AC,

  STATE_SEND_TEMPS,          STATE_WAIT_TEMPS,
  STATE_SEND_CTRL_RS485,     STATE_WAIT_CTRL_RS485,
  STATE_SEND_CTRL_FORCE,     STATE_WAIT_CTRL_FORCE,
  STATE_SEND_CTRL_POWER,     STATE_WAIT_CTRL_POWER,
  STATE_SEND_CTRL_WORKMODE,  STATE_WAIT_CTRL_WORKMODE,
  STATE_SEND_CTRL_MAXPOWER,  STATE_WAIT_CTRL_MAXPOWER,
  STATE_SEND_TOTALS,         STATE_WAIT_TOTALS,

  STATE_PROCESS_WRITES,
  STATE_WAIT_WRITE,

  STATE_CYCLE_COMPLETE
};

// ============================================================================
// Globals
// ============================================================================
ModbusRTU       mb;
AsyncWebServer  server(80);
Preferences     prefs;

static Config      config;
static BatteryMode current_mode  = MODE_OFF;
static bool        time_synced   = false;

// Last computed desired (for /status reporting)
static uint16_t last_desired_fmode = 0;
static uint16_t last_desired_cpow  = 0;
static uint16_t last_desired_dpow  = 0;

volatile bool    txnComplete   = false;
volatile uint8_t txnResultCode = 0;

bool modbusCallback(Modbus::ResultCode event, uint16_t transactionId, void* data) {
  txnResultCode = (uint8_t)event;
  txnComplete   = true;
  return true;
}

uint16_t regBuf[30];

// --- Cache: monitoring ---
static uint16_t cache_batt_voltage = 0;
static int16_t  cache_batt_current = 0;
static int32_t  cache_batt_power   = 0;  // 32102-32103 combined; scale 1 W (signed)
static uint16_t cache_batt_soc     = 0;
static int16_t  cache_batt_temp1   = 0;

static uint16_t cache_ac_voltage = 0;
static int16_t  cache_ac_current = 0;
static int16_t  cache_ac_power   = 0;
static uint16_t cache_ac_freq    = 0;

static int16_t  cache_temps[REG_TEMPS_COUNT] = {};

// --- Cache: control registers ---
static uint16_t cache_rs485_enable    = 0;
static uint16_t cache_force_mode      = 0;
static uint16_t cache_target_soc      = 0;
static uint16_t cache_charge_power    = 0;
static uint16_t cache_discharge_power = 0;
static uint16_t cache_work_mode       = 0;
static uint16_t cache_max_charge      = 0;
static uint16_t cache_max_discharge   = 0;
static uint16_t cache_charge_cutoff    = 0;  // Hard BMS cap,   scale ÷10 (1000 = 100%)
static uint16_t cache_discharge_cutoff = 0;  // Hard BMS floor, scale ÷10 (120 = 12%)

// Raw uint32 from 2-register modbus reads; consumer multiplies × 0.01 to get kWh
static uint32_t cache_total_charge_energy    = 0;
static uint32_t cache_total_discharge_energy = 0;

// --- Staging ---
struct StagedFast { uint16_t batt[6]; uint16_t ac[4]; };
static StagedFast staged_fast = {};

struct StagedSlow {
  int16_t  temps[REG_TEMPS_COUNT];
  uint16_t rs485_enable;
  uint16_t force_mode;
  uint16_t target_soc;
  uint16_t charge_power;
  uint16_t discharge_power;
  uint16_t work_mode;
  uint16_t max_charge;
  uint16_t max_discharge;
  uint16_t totals[4];  // 33000-33003: charge_hi, charge_lo, discharge_hi, discharge_lo
};
static StagedSlow staged_slow = {};

static unsigned long cache_last_poll_ms = 0;

// --- Error tracking ---
static uint8_t       consecutiveErrors = 0;
static unsigned long lastSuccessTime   = 0;
static unsigned long currentBackoffMs  = 0;

void recordSuccess() { consecutiveErrors = 0; currentBackoffMs = 0; lastSuccessTime = millis(); }
void recordError(uint8_t code) {
  consecutiveErrors++;
  Serial.printf("[MODBUS] Error 0x%02X, consecutive=%u\n", code, consecutiveErrors);
  if (consecutiveErrors >= ERROR_LIMIT) {
    uint8_t shift = consecutiveErrors - ERROR_LIMIT;
    if (shift > 4) shift = 4;
    currentBackoffMs = min((unsigned long)(ERROR_BACKOFF_BASE << shift), ERROR_BACKOFF_MAX);
  }
}

// --- Write queue ---
#define WRITE_QUEUE_SIZE 16
static WriteEntry writeQueue[WRITE_QUEUE_SIZE];
static uint8_t    wqHead = 0;
static uint8_t    wqTail = 0;
static portMUX_TYPE writeMux = portMUX_INITIALIZER_UNLOCKED;

bool queueWrite(uint16_t addr, uint16_t value) {
  portENTER_CRITICAL(&writeMux);
  for (uint8_t i = wqTail; i != wqHead; i = (i + 1) % WRITE_QUEUE_SIZE) {
    if (writeQueue[i].addr == addr) {
      writeQueue[i].value = value;
      portEXIT_CRITICAL(&writeMux);
      return true;
    }
  }
  uint8_t next = (wqHead + 1) % WRITE_QUEUE_SIZE;
  if (next == wqTail) {
    portEXIT_CRITICAL(&writeMux);
    Serial.println("[WRITE] Queue full!");
    return false;
  }
  writeQueue[wqHead] = {addr, value};
  wqHead = next;
  portEXIT_CRITICAL(&writeMux);
  return true;
}

bool dequeueWrite(WriteEntry &entry) {
  portENTER_CRITICAL(&writeMux);
  if (wqHead == wqTail) { portEXIT_CRITICAL(&writeMux); return false; }
  entry = writeQueue[wqTail];
  wqTail = (wqTail + 1) % WRITE_QUEUE_SIZE;
  portEXIT_CRITICAL(&writeMux);
  return true;
}

// ============================================================================
// Power meter (Shelly Pro 3EM JSON shape — served by HA addon)
// ============================================================================
static float   meter_total_power  = 0.0;
static float   meter_a_power      = 0.0;
static float   meter_b_power      = 0.0;
static float   meter_c_power      = 0.0;
static unsigned long meter_last_update = 0;
static unsigned long meter_last_poll   = 0;
static bool    meter_valid         = false;
static uint32_t meter_telegram_count = 0;

void pollMeter() {
  unsigned long now = millis();
  if (now - meter_last_poll < METER_POLL_MS) return;
  meter_last_poll = now;

  if (WiFi.status() != WL_CONNECTED) return;
  if (config.meter_ip[0] == '\0') return;

  HTTPClient http;
  String url = "http://" + String(config.meter_ip) + "/rpc/EM.GetStatus?id=0";
  http.begin(url);
  http.setTimeout(METER_HTTP_TIMEOUT);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      meter_total_power = doc["total_act_power"] | 0.0f;
      meter_a_power     = doc["a_act_power"]    | 0.0f;
      meter_b_power     = doc["b_act_power"]    | 0.0f;
      meter_c_power     = doc["c_act_power"]    | 0.0f;
      meter_last_update = millis();
      meter_valid       = true;
      meter_telegram_count++;
    } else {
      Serial.printf("[METER] JSON parse error: %s\n", err.c_str());
    }
  } else {
    Serial.printf("[METER] HTTP error: %d\n", httpCode);
  }
  http.end();
}

// ============================================================================
// Time / scheduler
// ============================================================================
bool getLocalTm(struct tm &out) {
  time_t now;
  time(&now);
  if (now < 1700000000) return false;  // not synced yet
  localtime_r(&now, &out);
  return true;
}

int8_t currentSlotIndex() {
  struct tm t;
  if (!getLocalTm(t)) return -1;
  return t.tm_hour * 4 + (t.tm_min / 15);
}

BatteryMode scheduledMode() {
  int8_t slot = currentSlotIndex();
  if (slot < 0) return MODE_OFF;  // clock not synced → stay safe
  uint8_t v = config.schedule[slot];
  if (v > MODE_SELF_CONSUMPTION) return MODE_OFF;
  return (BatteryMode)v;
}

const char* modeToString(BatteryMode m) {
  switch (m) {
    case MODE_OFF:              return "off";
    case MODE_MAX_CHARGE:       return "max_charge";
    case MODE_MAX_DISCHARGE:    return "max_discharge";
    case MODE_SELF_CONSUMPTION: return "self_consumption";
  }
  return "unknown";
}

bool modeFromString(const char* s, BatteryMode &out) {
  if (!s) return false;
  if (!strcmp(s, "off"))              { out = MODE_OFF;              return true; }
  if (!strcmp(s, "max_charge"))       { out = MODE_MAX_CHARGE;       return true; }
  if (!strcmp(s, "max_discharge"))    { out = MODE_MAX_DISCHARGE;    return true; }
  if (!strcmp(s, "self_consumption")) { out = MODE_SELF_CONSUMPTION; return true; }
  return false;
}

// ============================================================================
// Self-consumption — integral controller on grid power
// ============================================================================
// Classic zero-export control: treat grid power as the process variable with
// setpoint 0 (house neither imports nor exports), and the battery AC setpoint
// as the manipulated variable. Each cycle, adjust the battery setpoint by a
// fraction of the current grid reading:
//
//   setpoint -= Ki * (grid - target_grid)
//
// Positive setpoint = charge, negative = discharge. Integrator has no feed-
// forward term, so command-lag in the battery's physical response never causes
// overshoot: the grid meter closes the loop through physical reality. Ki < 1
// guarantees convergence without oscillation. SC_KI = 0.5 converges in ~5
// cycles (~5 s at fast_cycle_ms = 1000), well within the noise floor.
// Battery physical response has a transport delay of ~3-5 s (Modbus write
// propagation + inverter ramp). If the control loop's time constant (1/Ki)
// is comparable or shorter, the integrator winds up while the battery is
// still responding to the previous command — classic oscillation. We choose
// Ki so the loop time constant is well ABOVE the ramp time, and slew-limit
// the setpoint so no single cycle can command more than the battery can
// physically follow.
static const float SC_KI             = 0.1f;    // τ_loop = 10 s >> τ_battery ~3-5 s
static const float SC_TARGET_GRID    = 0.0f;    // 0 W = net zero; negative for an export bias
static const float SC_MODE_HYST_W    = 1000.0f; // once in a direction, setpoint must cross this to flip
static const float SC_MAX_SLEW_W     = 200.0f;  // max setpoint change per second — keeps the commanded
                                                // power change slower than the physical ramp rate

static float   sc_setpoint       = 0.0f;   // signed battery setpoint (W)
static float   sc_smoothed_grid_w = 0.0f;
static bool    sc_smoothed_init   = false;
static uint8_t sc_mode            = 0;     // 0 = idle, 1 = charge, 2 = discharge

void resetSelfConsumption() {
  sc_smoothed_init = false;
  sc_setpoint      = 0.0f;
  sc_mode          = 0;
}

void computeSelfConsumption(uint16_t &fmode, uint16_t &cpow, uint16_t &dpow) {
  fmode = 0; cpow = 0; dpow = 0;

  // Safety: stale / missing meter → idle
  if (!meter_valid || (millis() - meter_last_update) > METER_STALE_MS) {
    sc_setpoint = 0.0f;
    sc_mode     = 0;
    return;
  }

  // EMA smoothing of the grid meter reading (reduces jitter from the P1 meter).
  if (!sc_smoothed_init) {
    sc_smoothed_grid_w = meter_total_power;
    sc_smoothed_init   = true;
  } else {
    sc_smoothed_grid_w = config.sc_smoothing * meter_total_power
                       + (1.0f - config.sc_smoothing) * sc_smoothed_grid_w;
  }

  // Integrator update. grid > 0 = importing → reduce setpoint (discharge more
  // or charge less). grid < 0 = exporting → increase setpoint (charge more).
  // Slew-limited so rapid cyclic loads (induction hob burst-firing) don't
  // drag the setpoint between extremes every cycle.
  float error = sc_smoothed_grid_w - SC_TARGET_GRID;
  float delta = -SC_KI * error;
  if (delta >  SC_MAX_SLEW_W) delta =  SC_MAX_SLEW_W;
  if (delta < -SC_MAX_SLEW_W) delta = -SC_MAX_SLEW_W;
  sc_setpoint += delta;

  // SoC guards.
  if (cache_batt_soc >= config.sc_max_soc && sc_setpoint > 0.0f) sc_setpoint = 0.0f;
  if (cache_batt_soc <= config.sc_min_soc && sc_setpoint < 0.0f) sc_setpoint = 0.0f;

  // Power limits (prevent integrator windup).
  if (sc_setpoint >  (float)config.max_charge_w)     sc_setpoint =  (float)config.max_charge_w;
  if (sc_setpoint < -(float)config.max_discharge_w)  sc_setpoint = -(float)config.max_discharge_w;

  // Mode-switch hysteresis. Once committed to a direction, require the setpoint
  // to cross zero by SC_MODE_HYST_W before flipping. Stops rapid charge↔discharge
  // cycling when the grid reading straddles 0. From idle, the normal deadband
  // (sc_offset_w) governs entry into a mode.
  uint8_t new_mode = sc_mode;
  if (sc_mode == 0) {
    if      (sc_setpoint >  config.sc_offset_w) new_mode = 1;
    else if (sc_setpoint < -config.sc_offset_w) new_mode = 2;
  } else if (sc_mode == 1) {
    if (sc_setpoint < -SC_MODE_HYST_W) new_mode = 2;
    else if (sc_setpoint < config.sc_offset_w) new_mode = 0;
  } else if (sc_mode == 2) {
    if (sc_setpoint >  SC_MODE_HYST_W) new_mode = 1;
    else if (sc_setpoint > -config.sc_offset_w) new_mode = 0;
  }
  sc_mode = new_mode;

  // Idle: don't command anything, but let the integrator keep winding so it can
  // cross the deadband when the grid moves sustainedly.
  if (sc_mode == 0) return;

  // Clamp setpoint to the active direction so we never send a "charge at
  // negative watts" or vice versa.
  if (sc_mode == 1 && sc_setpoint < 0.0f) sc_setpoint = 0.0f;
  if (sc_mode == 2 && sc_setpoint > 0.0f) sc_setpoint = 0.0f;

  if (sc_mode == 1) { fmode = 1; cpow = (uint16_t)sc_setpoint; }
  else              { fmode = 2; dpow = (uint16_t)(-sc_setpoint); }
}

// ============================================================================
// Desired state + enforcement
// ============================================================================
void computeDesired(uint16_t &fmode, uint16_t &cpow, uint16_t &dpow) {
  fmode = 0; cpow = 0; dpow = 0;
  BatteryMode mode = scheduledMode();
  current_mode = mode;

  switch (mode) {
    case MODE_OFF:
      break;
    case MODE_MAX_CHARGE:
      if (cache_batt_soc < 100) {
        fmode = 1;
        cpow  = config.max_charge_w;
      }
      break;
    case MODE_MAX_DISCHARGE:
      if (cache_batt_soc > 0) {
        fmode = 2;
        dpow  = config.max_discharge_w;
      }
      break;
    case MODE_SELF_CONSUMPTION:
      computeSelfConsumption(fmode, cpow, dpow);
      break;
  }
}

void enforceDesiredState() {
  // Static-policy registers. Cache is initialized to 0 and only updated when
  // the queue writes them, so after startup the guards here fire ONCE to
  // bring cache in line. That post-startup write seems to be what primes the
  // Marstek to accept force_mode commands — the startup-sequence writes
  // (via trySendWriteHreg) don't have the same effect for reasons unclear.
  // After the one-shot, cache matches and no further writes happen, so we
  // don't risk disturbing an active charge.
  if (cache_rs485_enable != RS485_ENABLE_MAGIC) queueWrite(REG_RS485_ENABLE, RS485_ENABLE_MAGIC);
  if (cache_work_mode != 0)                     queueWrite(REG_WORK_MODE, 0);
  if (cache_max_charge != config.max_charge_w)  queueWrite(REG_MAX_CHARGE, config.max_charge_w);
  if (cache_max_discharge != config.max_discharge_w) queueWrite(REG_MAX_DISCHARGE, config.max_discharge_w);
  // BMS SoC caps. target_soc (42011) is the soft cap used by Marstek's auto
  // mode; charge_cutoff (44000) and discharge_cutoff (44001) are the hard
  // BMS limits (scale ÷10) that supersede force_mode. Cache starts at 0 so
  // these fire exactly once post-startup, same pattern as the other static
  // writes. 1000 = 100% upper, 120 = 12% lower.
  if (cache_target_soc != 100)                    queueWrite(REG_TARGET_SOC, 100);
  if (cache_charge_cutoff != 1000)                queueWrite(REG_CHARGE_CUTOFF, 1000);
  if (cache_discharge_cutoff != 120)              queueWrite(REG_DISCHARGE_CUTOFF, 120);

  // Mode-derived
  uint16_t fmode, cpow, dpow;
  computeDesired(fmode, cpow, dpow);

  last_desired_fmode = fmode;
  last_desired_cpow  = cpow;
  last_desired_dpow  = dpow;

  // Marstek's internal charge/discharge action times out ~60s after the last
  // register write even though the register keeps its value — so write every
  // cycle unconditionally. Combined with fast_cycle_ms = 1000, this refreshes
  // at 1 Hz and charging stays active indefinitely.
  queueWrite(REG_FORCE_MODE,      fmode);
  queueWrite(REG_CHARGE_POWER,    cpow);
  queueWrite(REG_DISCHARGE_POWER, dpow);
}

// ============================================================================
// NVS persistence
// ============================================================================
void setConfigDefaults() {
  memset(config.schedule, MODE_OFF, SCHEDULE_SLOTS);
  config.max_charge_w    = 2500;
  config.max_discharge_w = 2500;
  config.sc_min_soc      = 12;   // matches BMS discharge_cutoff (44001 = 120 ÷10)
  config.sc_max_soc      = 100;  // matches BMS charge_cutoff    (44000 = 1000 ÷10)
  config.sc_offset_w     = 50.0f;
  config.sc_smoothing    = 0.05f; // τ ≈ 20 s — must be slower than battery's physical ramp
  config.sc_deadband_w   = 100.0f;
  strncpy(config.meter_ip, "192.168.2.56:8088", sizeof(config.meter_ip) - 1);
  config.meter_ip[sizeof(config.meter_ip) - 1] = '\0';
  config.fast_cycle_ms = 1000;  // 1 Hz — required for force_mode/power heartbeat
}

void loadConfig() {
  setConfigDefaults();

  prefs.begin("marstek", true);
  size_t got = prefs.getBytes("schedule", config.schedule, SCHEDULE_SLOTS);
  if (got != SCHEDULE_SLOTS) {
    memset(config.schedule, MODE_OFF, SCHEDULE_SLOTS);
  }
  config.max_charge_w    = prefs.getUShort("max_charge",   config.max_charge_w);
  config.max_discharge_w = prefs.getUShort("max_discharge", config.max_discharge_w);
  config.sc_offset_w     = prefs.getFloat ("sc_offset",    config.sc_offset_w);
  config.sc_deadband_w   = prefs.getFloat ("sc_deadband",  config.sc_deadband_w);
  // Force ESP-side self-consumption guards AFTER reading NVS so stale NVS
  // values can't override them. SoC guards match BMS cutoffs (44001 = 12 %
  // floor, 44000 = 100 % ceiling). Smoothing tuned for bursty loads.
  config.sc_min_soc    = 12;
  config.sc_max_soc    = 100;
  config.sc_smoothing  = 0.05f;  // τ ≈ 20 s — heavy grid-reading damping so the
                                 // integrator doesn't react to transients caused
                                 // by the battery's own response
  config.sc_deadband_w = 100.0f;
  String ip = prefs.getString("meter_ip", "");
  if (ip.length() > 0 && ip.length() < sizeof(config.meter_ip)) {
    strncpy(config.meter_ip, ip.c_str(), sizeof(config.meter_ip) - 1);
    config.meter_ip[sizeof(config.meter_ip) - 1] = '\0';
  }
  // Force 1 Hz regardless of NVS — required for the Marstek force_mode /
  // charge_power write heartbeat (Marstek stops acting ~60s after last write).
  config.fast_cycle_ms = 1000;
  prefs.end();
}

void saveConfig() {
  prefs.begin("marstek", false);
  prefs.putBytes ("schedule",     config.schedule, SCHEDULE_SLOTS);
  prefs.putUShort("max_charge",   config.max_charge_w);
  prefs.putUShort("max_discharge", config.max_discharge_w);
  prefs.putUChar ("sc_min_soc",   config.sc_min_soc);
  prefs.putUChar ("sc_max_soc",   config.sc_max_soc);
  prefs.putFloat ("sc_offset",    config.sc_offset_w);
  prefs.putFloat ("sc_smooth",    config.sc_smoothing);
  prefs.putFloat ("sc_deadband",  config.sc_deadband_w);
  prefs.putString("meter_ip",     config.meter_ip);
  prefs.putULong ("fast_cycle",   config.fast_cycle_ms);
  prefs.end();
}

// ============================================================================
// Poll state machine
// ============================================================================
static PollState     pollState           = STATE_STARTUP_RS485;
static unsigned long lastFastCycleStart  = 0;
static unsigned long lastSlowCycleStart  = 0;
static unsigned long lastTxnTime         = 0;
static bool          isSlowCycle         = false;
static bool          cycle_enforce_done  = false;

static uint8_t       startupRetries = 0;
#define MAX_STARTUP_RETRIES 10

static WriteEntry currentWrite;

bool trySendReadHreg(uint16_t addr, uint16_t count, PollState nextState) {
  txnComplete = false;
  if (mb.readHreg(SLAVE_ID, addr, regBuf, count, modbusCallback)) {
    lastTxnTime = millis();
    pollState = nextState;
    return true;
  }
  recordError(0xFF);
  pollState = STATE_IDLE;
  lastFastCycleStart = millis();
  return false;
}

bool trySendWriteHreg(uint16_t addr, uint16_t value, PollState nextState) {
  txnComplete = false;
  uint16_t val = value;
  if (mb.writeHreg(SLAVE_ID, addr, &val, 1, modbusCallback)) {
    lastTxnTime = millis();
    pollState = nextState;
    return true;
  }
  recordError(0xFF);
  return false;
}

void runStateMachine() {
  unsigned long now = millis();

  switch (pollState) {

  // ===== Startup: lock baseline =====
  case STATE_STARTUP_RS485:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    Serial.println("[STARTUP] Enabling RS485 control...");
    if (!trySendWriteHreg(REG_RS485_ENABLE, RS485_ENABLE_MAGIC, STATE_STARTUP_WAIT_RS485)) {
      if (++startupRetries > MAX_STARTUP_RETRIES) {
        Serial.println("[STARTUP] Failed after retries, continuing anyway");
        pollState = STATE_IDLE;
      }
    }
    break;

  case STATE_STARTUP_WAIT_RS485:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) {
      Serial.println("[STARTUP] RS485 enabled");
      recordSuccess();
      pollState = STATE_STARTUP_WORKMODE;
    } else {
      recordError(txnResultCode);
      pollState = STATE_STARTUP_RS485;
      if (++startupRetries > MAX_STARTUP_RETRIES) {
        Serial.println("[STARTUP] RS485 enable failed, continuing");
        pollState = STATE_IDLE;
      }
    }
    break;

  case STATE_STARTUP_WORKMODE:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    Serial.println("[STARTUP] Setting Manual mode...");
    trySendWriteHreg(REG_WORK_MODE, 0, STATE_STARTUP_WAIT_WORKMODE);
    break;

  case STATE_STARTUP_WAIT_WORKMODE:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) { Serial.println("[STARTUP] Manual mode set"); recordSuccess(); }
    else                                     { recordError(txnResultCode); }
    pollState = STATE_STARTUP_MAXCHARGE;
    break;

  case STATE_STARTUP_MAXCHARGE:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    Serial.printf("[STARTUP] Locking max charge to %uW...\n", config.max_charge_w);
    trySendWriteHreg(REG_MAX_CHARGE, config.max_charge_w, STATE_STARTUP_WAIT_MAXCHARGE);
    break;

  case STATE_STARTUP_WAIT_MAXCHARGE:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) { Serial.println("[STARTUP] Max charge locked"); recordSuccess(); }
    else                                     { recordError(txnResultCode); }
    pollState = STATE_STARTUP_MAXDISCHARGE;
    break;

  case STATE_STARTUP_MAXDISCHARGE:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    Serial.printf("[STARTUP] Locking max discharge to %uW...\n", config.max_discharge_w);
    trySendWriteHreg(REG_MAX_DISCHARGE, config.max_discharge_w, STATE_STARTUP_WAIT_MAXDISCHARGE);
    break;

  case STATE_STARTUP_WAIT_MAXDISCHARGE:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) { Serial.println("[STARTUP] Max discharge locked"); recordSuccess(); }
    else                                     { recordError(txnResultCode); }
    Serial.println("[STARTUP] Complete — entering normal operation");
    lastFastCycleStart = millis();
    lastSlowCycleStart = millis();
    pollState = STATE_IDLE;
    break;

  // ===== Idle → cycle start =====
  case STATE_IDLE: {
    unsigned long interval = config.fast_cycle_ms + currentBackoffMs;
    if (now - lastFastCycleStart >= interval) {
      isSlowCycle = (now - lastSlowCycleStart >= SLOW_CYCLE_MS);
      cycle_enforce_done = false;
      pollState = STATE_SEND_BATT;
    }
    break;
  }

  // ===== Fast cycle: battery (32100-32105) =====
  case STATE_SEND_BATT:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    trySendReadHreg(REG_BATT_VOLTAGE, 6, STATE_WAIT_BATT);
    break;

  case STATE_WAIT_BATT:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) { memcpy(staged_fast.batt, regBuf, 6 * sizeof(uint16_t)); recordSuccess(); }
    else                                     { recordError(txnResultCode); }
    pollState = STATE_SEND_AC;
    break;

  // ===== Fast cycle: AC (32200-32203) =====
  case STATE_SEND_AC:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    trySendReadHreg(REG_AC_VOLTAGE, 4, STATE_WAIT_AC);
    break;

  case STATE_WAIT_AC:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) { memcpy(staged_fast.ac, regBuf, 4 * sizeof(uint16_t)); recordSuccess(); }
    else                                     { recordError(txnResultCode); }
    // Slow cycle now skips temps (32106-32110) and control-register reads
    // — Marstek pauses charging when any of those regions are accessed.
    // Goes straight to totals (33000-33003) which we're testing as the only
    // remaining slow-cycle read.
    pollState = isSlowCycle ? STATE_SEND_TOTALS : STATE_PROCESS_WRITES;
    break;

  // ===== Slow cycle: temps =====
  case STATE_SEND_TEMPS:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    trySendReadHreg(REG_TEMPS_START, REG_TEMPS_COUNT, STATE_WAIT_TEMPS);
    break;

  case STATE_WAIT_TEMPS:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) { memcpy(staged_slow.temps, regBuf, REG_TEMPS_COUNT * sizeof(int16_t)); recordSuccess(); }
    else                                     { recordError(txnResultCode); }
    // Skip control-register reads (42000/42010/42020/43000/44002 range): Marstek
    // pauses charging for ~50s when any of those addresses is read. Reads of
    // 32xxx (battery/AC/temps) and 33xxx (totals) are safe. Since force_mode /
    // charge_power / discharge_power are now written unconditionally at 1 Hz,
    // we don't need the drift-check reads anyway.
    pollState = STATE_SEND_TOTALS;
    break;

  // ===== Slow cycle: control register drift check =====
  case STATE_SEND_CTRL_RS485:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    trySendReadHreg(REG_RS485_ENABLE, 1, STATE_WAIT_CTRL_RS485);
    break;

  case STATE_WAIT_CTRL_RS485:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) { staged_slow.rs485_enable = regBuf[0]; recordSuccess(); }
    else                                     { recordError(txnResultCode); }
    pollState = STATE_SEND_CTRL_FORCE;
    break;

  case STATE_SEND_CTRL_FORCE:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    trySendReadHreg(REG_FORCE_MODE, 2, STATE_WAIT_CTRL_FORCE);
    break;

  case STATE_WAIT_CTRL_FORCE:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) {
      staged_slow.force_mode = regBuf[0];
      staged_slow.target_soc = regBuf[1];
      recordSuccess();
    } else { recordError(txnResultCode); }
    pollState = STATE_SEND_CTRL_POWER;
    break;

  case STATE_SEND_CTRL_POWER:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    trySendReadHreg(REG_CHARGE_POWER, 2, STATE_WAIT_CTRL_POWER);
    break;

  case STATE_WAIT_CTRL_POWER:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) {
      staged_slow.charge_power    = regBuf[0];
      staged_slow.discharge_power = regBuf[1];
      recordSuccess();
    } else { recordError(txnResultCode); }
    pollState = STATE_SEND_CTRL_WORKMODE;
    break;

  case STATE_SEND_CTRL_WORKMODE:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    trySendReadHreg(REG_WORK_MODE, 1, STATE_WAIT_CTRL_WORKMODE);
    break;

  case STATE_WAIT_CTRL_WORKMODE:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) { staged_slow.work_mode = regBuf[0]; recordSuccess(); }
    else                                     { recordError(txnResultCode); }
    pollState = STATE_SEND_CTRL_MAXPOWER;
    break;

  case STATE_SEND_CTRL_MAXPOWER:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    trySendReadHreg(REG_MAX_CHARGE, 2, STATE_WAIT_CTRL_MAXPOWER);
    break;

  case STATE_WAIT_CTRL_MAXPOWER:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) {
      staged_slow.max_charge    = regBuf[0];
      staged_slow.max_discharge = regBuf[1];
      recordSuccess();
    } else { recordError(txnResultCode); }
    pollState = STATE_SEND_TOTALS;
    break;

  // ===== Lifetime energy counters (33000-33003) =====
  case STATE_SEND_TOTALS:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    trySendReadHreg(REG_TOTAL_CHARGE_ENERGY, 4, STATE_WAIT_TOTALS);
    break;

  case STATE_WAIT_TOTALS:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) {
      memcpy(staged_slow.totals, regBuf, 4 * sizeof(uint16_t));
      recordSuccess();
    } else { recordError(txnResultCode); }
    pollState = STATE_PROCESS_WRITES;
    break;

  // ===== Commit cache + enforce desired (once), then drain write queue =====
  case STATE_PROCESS_WRITES: {
    // Commit + enforce runs exactly once per cycle; subsequent re-entries
    // (after each write's WAIT_WRITE) only drain the queue.
    if (!cycle_enforce_done) {
      cache_batt_voltage = staged_fast.batt[0];
      cache_batt_current = (int16_t)staged_fast.batt[1];
      // REG_BATT_POWER (32102) is int32 spanning 32102-32103, scale 1 W.
      // Big-endian: high word = staged_fast.batt[2], low word = batt[3].
      cache_batt_power   = (int32_t)(((uint32_t)staged_fast.batt[2] << 16) | (uint32_t)staged_fast.batt[3]);
      cache_batt_soc     = staged_fast.batt[4];
      cache_batt_temp1   = (int16_t)staged_fast.batt[5];

      cache_ac_voltage   = staged_fast.ac[0];
      cache_ac_current   = (int16_t)staged_fast.ac[1];
      cache_ac_power     = (int16_t)staged_fast.ac[2];
      cache_ac_freq      = staged_fast.ac[3];

      if (isSlowCycle) {
        // Only commit temps + totals. Control-register cache fields stay at
        // the values we wrote (updated in STATE_WAIT_WRITE); reading them
        // back from Marstek is what was causing the ~50s charging pause.
        memcpy(cache_temps, staged_slow.temps, sizeof(cache_temps));
        cache_total_charge_energy    = ((uint32_t)staged_slow.totals[0] << 16) | staged_slow.totals[1];
        cache_total_discharge_energy = ((uint32_t)staged_slow.totals[2] << 16) | staged_slow.totals[3];
      }

      enforceDesiredState();
      cycle_enforce_done = true;
    }

    // Drain queue
    if (now - lastTxnTime < INTER_TXN_MS) break;
    if (dequeueWrite(currentWrite)) {
      txnComplete = false;
      uint16_t val = currentWrite.value;
      if (mb.writeHreg(SLAVE_ID, currentWrite.addr, &val, 1, modbusCallback)) {
        lastTxnTime = now;
        pollState = STATE_WAIT_WRITE;
      } else {
        recordError(0xFF);
        pollState = STATE_PROCESS_WRITES;
      }
    } else {
      pollState = STATE_CYCLE_COMPLETE;
    }
    break;
  }

  case STATE_WAIT_WRITE:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) {
      Serial.printf("[WRITE] reg %u = %u OK\n", currentWrite.addr, currentWrite.value);
      recordSuccess();
      // Optimistic cache update — reflects reality immediately instead of
      // waiting for the next slow-cycle read.
      switch (currentWrite.addr) {
        case REG_RS485_ENABLE:    cache_rs485_enable    = currentWrite.value; break;
        case REG_FORCE_MODE:      cache_force_mode      = currentWrite.value; break;
        case REG_TARGET_SOC:      cache_target_soc      = currentWrite.value; break;
        case REG_CHARGE_POWER:    cache_charge_power    = currentWrite.value; break;
        case REG_DISCHARGE_POWER: cache_discharge_power = currentWrite.value; break;
        case REG_WORK_MODE:       cache_work_mode       = currentWrite.value; break;
        case REG_MAX_CHARGE:      cache_max_charge      = currentWrite.value; break;
        case REG_MAX_DISCHARGE:   cache_max_discharge   = currentWrite.value; break;
        case REG_CHARGE_CUTOFF:   cache_charge_cutoff   = currentWrite.value; break;
        case REG_DISCHARGE_CUTOFF: cache_discharge_cutoff = currentWrite.value; break;
      }
    } else {
      Serial.printf("[WRITE] reg %u = %u FAILED (0x%02X)\n", currentWrite.addr, currentWrite.value, txnResultCode);
      recordError(txnResultCode);
    }
    pollState = STATE_PROCESS_WRITES;
    break;

  case STATE_CYCLE_COMPLETE:
    if (isSlowCycle) lastSlowCycleStart = millis();
    cache_last_poll_ms = millis();
    lastFastCycleStart = millis();
    pollState = STATE_IDLE;
    break;
  }
}

// ============================================================================
// WiFi management
// ============================================================================
static unsigned long lastWiFiAttempt = 0;

void handleWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastWiFiAttempt < WIFI_RECONNECT_MS) return;
  lastWiFiAttempt = now;
  Serial.println("[WIFI] Disconnected, reconnecting...");
  WiFi.disconnect();
  WiFi.config(staticIP, gateway, subnet, gateway, IPAddress(8, 8, 8, 8));
  WiFi.begin(ssid, password);
}

// ============================================================================
// Time sync monitoring
// ============================================================================
void updateTimeSynced() {
  time_t now;
  time(&now);
  time_synced = (now > 1700000000);
}

// ============================================================================
// HTTP: JSON builders + routes
// ============================================================================
void buildConfigJson(JsonDocument &doc) {
  doc["max_charge_w"]    = config.max_charge_w;
  doc["max_discharge_w"] = config.max_discharge_w;
  doc["meter_ip"]        = config.meter_ip;
  doc["fast_cycle_ms"]   = config.fast_cycle_ms;

  JsonObject sc = doc.createNestedObject("self_consumption");
  sc["min_soc"]    = config.sc_min_soc;
  sc["max_soc"]    = config.sc_max_soc;
  sc["offset_w"]   = config.sc_offset_w;
  sc["smoothing"]  = config.sc_smoothing;
  sc["deadband_w"] = config.sc_deadband_w;
}

void buildScheduleJson(JsonDocument &doc) {
  JsonArray arr = doc.createNestedArray("schedule");
  for (int i = 0; i < SCHEDULE_SLOTS; i++) {
    arr.add(modeToString((BatteryMode)config.schedule[i]));
  }
}

void setupRoutes() {

  // GET / — device info
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json",
      "{\"device\":\"Marstek Venus E3 Controller\",\"version\":\"2.0\"}");
  });

  // GET /health
  server.on("/health", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<384> doc;
    doc["status"]              = "ok";
    doc["uptime"]              = millis() / 1000;
    doc["wifi_rssi"]           = WiFi.RSSI();
    doc["time_synced"]         = time_synced;
    doc["modbus_errors"]       = consecutiveErrors;
    doc["modbus_backoff_ms"]   = currentBackoffMs;
    doc["meter_age_ms"]        = meter_valid ? (long)(millis() - meter_last_update) : -1;
    doc["meter_count"]         = meter_telegram_count;
    int8_t slot = currentSlotIndex();
    doc["slot"]                = slot;
    doc["mode"]                = modeToString(current_mode);
    String json; serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // GET /status — full state
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<2048> doc;

    // Time + mode
    JsonObject t = doc.createNestedObject("time");
    t["synced"] = time_synced;
    struct tm tm;
    if (getLocalTm(tm)) {
      char buf[32];
      strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
      t["local"] = buf;
      t["slot"]  = tm.tm_hour * 4 + (tm.tm_min / 15);
    } else {
      t["local"] = nullptr;
      t["slot"]  = -1;
    }

    JsonObject m = doc.createNestedObject("mode");
    m["current"]   = modeToString(current_mode);
    m["scheduled"] = modeToString(scheduledMode());

    // Battery. Marstek E3 silences REG_BATT_POWER while in force_mode and the
    // current register's scale doesn't match physical reality (V×I comes out
    // ~5× inflated). Best available signal is what we're commanding the
    // battery to do — accurate in magnitude, a few seconds of command-lag.
    // Sign: + = charging, − = discharging.
    float batt_v = cache_batt_voltage / 100.0f;
    float batt_i = cache_batt_current / 100.0f;  // scale unverified on Venus E3
    float batt_p_cmd = (float)last_desired_cpow - (float)last_desired_dpow;
    JsonObject batt = doc.createNestedObject("battery");
    batt["voltage"]     = batt_v;
    batt["current"]     = batt_i;
    batt["power"]       = batt_p_cmd;          // signed; from last command (reliable)
    batt["power_reg"]   = cache_batt_power;    // int32 register (silent in force_mode)
    batt["power_vi"]    = batt_v * batt_i;     // V×I (current scale is wrong)
    batt["soc"]         = cache_batt_soc;
    batt["temp1"]       = cache_batt_temp1 / 10.0;

    // AC — same proxy. REG_AC_POWER also unreliable under force_mode.
    JsonObject ac = doc.createNestedObject("ac");
    ac["voltage"]     = cache_ac_voltage / 10.0;
    ac["current"]     = cache_ac_current / 100.0;
    ac["power"]       = batt_p_cmd;
    ac["power_raw"]   = cache_ac_power;
    ac["frequency"]   = cache_ac_freq / 100.0;

    // Temperatures
    JsonArray temps = doc.createNestedArray("temperatures");
    temps.add(cache_batt_temp1 / 10.0);
    for (int i = 0; i < REG_TEMPS_COUNT; i++) temps.add(cache_temps[i] / 10.0);

    // Lifetime energy totals (raw uint32 × 0.01 = kWh)
    JsonObject totals = doc.createNestedObject("totals");
    totals["charge_energy_raw"]    = cache_total_charge_energy;
    totals["discharge_energy_raw"] = cache_total_discharge_energy;

    // Grid / meter
    JsonObject grid = doc.createNestedObject("grid");
    grid["total_power_w"]    = meter_total_power;
    grid["smoothed_power_w"] = sc_smoothed_grid_w;
    grid["a_power_w"]        = meter_a_power;
    grid["b_power_w"]        = meter_b_power;
    grid["c_power_w"]        = meter_c_power;
    grid["age_ms"]           = meter_valid ? (long)(millis() - meter_last_update) : -1;

    // Desired (what ESP32 most recently computed)
    JsonObject des = doc.createNestedObject("desired");
    des["force_mode"]      = last_desired_fmode;
    des["charge_power"]    = last_desired_cpow;
    des["discharge_power"] = last_desired_dpow;

    // Cached (what Marstek most recently reported)
    JsonObject cur = doc.createNestedObject("cached");
    cur["rs485_enabled"]       = (cache_rs485_enable == RS485_ENABLE_MAGIC);
    cur["work_mode"]           = cache_work_mode;
    cur["force_mode"]          = cache_force_mode;
    cur["charge_power"]        = cache_charge_power;
    cur["discharge_power"]     = cache_discharge_power;
    cur["target_soc"]          = cache_target_soc;
    cur["max_charge_power"]    = cache_max_charge;
    cur["max_discharge_power"] = cache_max_discharge;

    // Diagnostics
    doc["_errors"]       = consecutiveErrors;
    doc["_backoff_ms"]   = currentBackoffMs;
    doc["_last_poll_ms"] = cache_last_poll_ms;
    doc["_uptime"]       = millis() / 1000;
    doc["_rssi"]         = WiFi.RSSI();

    String json; serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // GET /summary — flat shape with the most-used values
  server.on("/summary", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<256> doc;
    doc["power_w"]                  = cache_ac_power;                // ±2500 typ.
    doc["soc"]                      = cache_batt_soc;                // percent, 0-100
    doc["total_charge_energy_raw"]    = cache_total_charge_energy;   // ×0.01 = kWh
    doc["total_discharge_energy_raw"] = cache_total_discharge_energy;
    doc["mode"]                     = modeToString(current_mode);
    String json; serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // GET /meter
  server.on("/meter", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<256> doc;
    doc["total_power_w"] = meter_total_power;
    doc["a_power_w"]     = meter_a_power;
    doc["b_power_w"]     = meter_b_power;
    doc["c_power_w"]     = meter_c_power;
    doc["age_ms"]        = meter_valid ? (long)(millis() - meter_last_update) : -1;
    doc["valid"]         = meter_valid;
    doc["count"]         = meter_telegram_count;
    doc["meter_ip"]      = config.meter_ip;
    String json; serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // GET /config
  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<512> doc;
    buildConfigJson(doc);
    String json; serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // POST /config — partial update
  server.on("/config", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<512> doc;
      DeserializationError err = deserializeJson(doc, data, len);
      if (err) { request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return; }
      JsonObject root = doc.as<JsonObject>();

      if (root.containsKey("max_charge_w"))    config.max_charge_w    = root["max_charge_w"].as<uint16_t>();
      if (root.containsKey("max_discharge_w")) config.max_discharge_w = root["max_discharge_w"].as<uint16_t>();
      if (root.containsKey("meter_ip")) {
        String ip = root["meter_ip"].as<String>();
        if (ip.length() > 0 && ip.length() < sizeof(config.meter_ip)) {
          strncpy(config.meter_ip, ip.c_str(), sizeof(config.meter_ip) - 1);
          config.meter_ip[sizeof(config.meter_ip) - 1] = '\0';
          meter_valid = false;
        }
      }
      if (root.containsKey("fast_cycle_ms")) {
        unsigned long v = root["fast_cycle_ms"].as<unsigned long>();
        if (v >= 1000) config.fast_cycle_ms = v;
      }

      if (root.containsKey("self_consumption")) {
        JsonObject sc = root["self_consumption"].as<JsonObject>();
        if (sc.containsKey("min_soc"))    config.sc_min_soc    = sc["min_soc"].as<uint8_t>();
        if (sc.containsKey("max_soc"))    config.sc_max_soc    = sc["max_soc"].as<uint8_t>();
        if (sc.containsKey("offset_w"))   config.sc_offset_w   = sc["offset_w"].as<float>();
        if (sc.containsKey("smoothing"))  config.sc_smoothing  = sc["smoothing"].as<float>();
        if (sc.containsKey("deadband_w")) config.sc_deadband_w = sc["deadband_w"].as<float>();
        resetSelfConsumption();
      }

      saveConfig();

      StaticJsonDocument<512> resp;
      buildConfigJson(resp);
      String json; serializeJson(resp, json);
      request->send(200, "application/json", json);
    }
  );

  // GET /schedule — 96-slot array
  server.on("/schedule", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<2048> doc;
    buildScheduleJson(doc);
    String json; serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // POST /schedule — accepts either:
  //   {"schedule": ["off", "max_charge", ...]}  (full 96-slot array, ~1.8 KB)
  //   {"range": {"start": 0, "end": 96, "mode": "off"}}
  //   {"slot": 12, "mode": "max_charge"}
  //
  // Body arrives in TCP-sized chunks (~1460 B), so the full 96-slot variant
  // can't be parsed per-chunk. We accumulate chunks into request->_tempObject
  // in the body handler, then parse once in the main request handler after
  // the full body is in. _tempObject is auto-freed when the request ends.
  server.on("/schedule", HTTP_POST,
    // Main handler — runs after the body is fully received.
    [](AsyncWebServerRequest *request) {
      uint8_t* body = (uint8_t*)request->_tempObject;
      size_t total = request->contentLength();
      if (!body || total == 0) {
        request->send(400, "application/json", "{\"error\":\"Empty body\"}");
        return;
      }

      StaticJsonDocument<4096> doc;
      DeserializationError err = deserializeJson(doc, body, total);
      if (err) {
        char msg[96];
        snprintf(msg, sizeof(msg), "{\"error\":\"Invalid JSON: %s\"}", err.c_str());
        request->send(400, "application/json", msg);
        return;
      }
      JsonObject root = doc.as<JsonObject>();

      if (root.containsKey("schedule")) {
        JsonArray arr = root["schedule"].as<JsonArray>();
        if (arr.size() != SCHEDULE_SLOTS) {
          request->send(400, "application/json",
            "{\"error\":\"schedule array must have 96 entries\"}");
          return;
        }
        for (int i = 0; i < SCHEDULE_SLOTS; i++) {
          BatteryMode m;
          const char* s = arr[i].as<const char*>();
          if (!modeFromString(s, m)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "{\"error\":\"invalid mode at slot %d\"}", i);
            request->send(400, "application/json", msg);
            return;
          }
          config.schedule[i] = (uint8_t)m;
        }
      }

      if (root.containsKey("range")) {
        JsonObject r = root["range"].as<JsonObject>();
        int start = r["start"] | 0;
        int end   = r["end"] | 0;
        BatteryMode m;
        if (!modeFromString(r["mode"].as<const char*>(), m)) {
          request->send(400, "application/json", "{\"error\":\"invalid mode in range\"}");
          return;
        }
        if (start < 0) start = 0;
        if (end > SCHEDULE_SLOTS) end = SCHEDULE_SLOTS;
        for (int i = start; i < end; i++) config.schedule[i] = (uint8_t)m;
      }

      if (root.containsKey("slot")) {
        int slot = root["slot"].as<int>();
        BatteryMode m;
        if (slot < 0 || slot >= SCHEDULE_SLOTS) {
          request->send(400, "application/json", "{\"error\":\"slot out of range\"}");
          return;
        }
        if (!modeFromString(root["mode"].as<const char*>(), m)) {
          request->send(400, "application/json", "{\"error\":\"invalid mode\"}");
          return;
        }
        config.schedule[slot] = (uint8_t)m;
      }

      saveConfig();

      StaticJsonDocument<2048> resp;
      buildScheduleJson(resp);
      String json; serializeJson(resp, json);
      request->send(200, "application/json", json);
    },
    NULL,  // no upload handler
    // Body handler — called once per chunk. Accumulate into _tempObject.
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (index == 0) {
        if (total == 0 || total > 8192) return;  // reject empty or oversized
        request->_tempObject = malloc(total);
      }
      if (request->_tempObject && index + len <= total) {
        memcpy((uint8_t*)request->_tempObject + index, data, len);
      }
    }
  );
}

// ============================================================================
// setup()
// ============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n[BOOT] Marstek Venus E3 Controller v2.0");

  loadConfig();
  Serial.printf("[CONFIG] meter_ip=%s fast_cycle=%lums max_charge=%u max_discharge=%u\n",
                config.meter_ip, config.fast_cycle_ms, config.max_charge_w, config.max_discharge_w);

  // RS485 transceiver (all active HIGH on T-CAN485)
  pinMode(RS485_5V_PIN, OUTPUT); digitalWrite(RS485_5V_PIN, HIGH);
  pinMode(RS485_EN_PIN, OUTPUT); digitalWrite(RS485_EN_PIN, HIGH);
  pinMode(RS485_SE_PIN, OUTPUT); digitalWrite(RS485_SE_PIN, HIGH);

  // WiFi
  WiFi.config(staticIP, gateway, subnet, gateway, IPAddress(8, 8, 8, 8));
  WiFi.begin(ssid, password);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Connected. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WIFI] Failed — will retry in loop");
  }

  // Time
  configTzTime(TZ_STRING, NTP_SERVER_1, NTP_SERVER_2);
  Serial.println("[TIME] NTP sync requested");

  // mDNS
  if (MDNS.begin("marstek-battery")) MDNS.addService("http", "tcp", 80);

  // OTA
  ArduinoOTA.setHostname("marstek-battery");
  ArduinoOTA.setPassword("admin");
  ArduinoOTA.onStart   ([]() { Serial.println("[OTA] Starting..."); });
  ArduinoOTA.onEnd     ([]() { Serial.println("\n[OTA] Complete!"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError   ([](ota_error_t error) { Serial.printf("[OTA] Error[%u]\n", error); });
  ArduinoOTA.begin();

  // Modbus RTU
  Serial2.begin(BAUDRATE, SERIAL_CONFIG, RX_PIN, TX_PIN);
  mb.begin(&Serial2);
  mb.master();
  delay(200);

  lastFastCycleStart = millis();
  lastSlowCycleStart = millis();
  lastSuccessTime    = millis();
  lastTxnTime        = 0;

  setupRoutes();
  server.begin();
  Serial.println("[HTTP] Server started on port 80");
  Serial.println("[BOOT] Setup complete");
}

// ============================================================================
// loop()
// ============================================================================
static unsigned long lastTimeCheck = 0;

void loop() {
  ArduinoOTA.handle();
  mb.task();
  handleWiFi();
  pollMeter();
  runStateMachine();

  // Cheap background tick — check NTP sync once a second
  unsigned long now = millis();
  if (now - lastTimeCheck > 1000) {
    lastTimeCheck = now;
    updateTimeSynced();
  }
}
