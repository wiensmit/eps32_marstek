// ============================================================================
// Marstek Venus E3 Modbus RTU Controller — ESP32
// LilyGO T-CAN485 | REST API | B2500 Meter (Shelly 3EM) integration
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

// ============================================================================
// Hardware config (LilyGO T-CAN485)
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
// Timing
// ============================================================================
unsigned long FAST_CYCLE_MS            = 2000;
const unsigned long SLOW_CYCLE_MS      = 60000;
const unsigned long INTER_TXN_MS       = 50;
const unsigned long WIFI_RECONNECT_MS  = 30000;
const unsigned long WATCHDOG_TIMEOUT_MS = 30000;
const unsigned long ERROR_BACKOFF_BASE  = 2000;
const unsigned long ERROR_BACKOFF_MAX   = 30000;
const uint8_t       ERROR_LIMIT         = 5;
const unsigned long METER_POLL_MS       = 2000;
const unsigned long METER_HTTP_TIMEOUT  = 2000;

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
#define REG_MAX_CHARGE       44002
#define REG_MAX_DISCHARGE    44003

// Monitoring — battery
#define REG_BATT_VOLTAGE     32100
#define REG_BATT_CURRENT     32101
#define REG_BATT_POWER       32102
// 32103 reserved
#define REG_BATT_SOC         32104
#define REG_BATT_TEMP1       32105

// Monitoring — AC
#define REG_AC_VOLTAGE       32200
#define REG_AC_CURRENT       32201
#define REG_AC_POWER         32202
#define REG_AC_FREQ          32203

// Temperatures
#define REG_TEMPS_START      32106
#define REG_TEMPS_COUNT      5

// Schedules
#define REG_SCHED_START      43100
#define REG_SCHED_COUNT      30   // 6 schedules x 5 registers

// Magic value
#define RS485_ENABLE_MAGIC   21930  // 0x55AA

// ============================================================================
// Type definitions — kept above functions so Arduino auto-prototypes see them
// ============================================================================
struct WriteEntry {
  uint16_t addr;
  uint16_t value;
};

enum PollState {
  // Startup sequence
  STATE_STARTUP_RS485,       STATE_STARTUP_WAIT_RS485,
  STATE_STARTUP_WORKMODE,    STATE_STARTUP_WAIT_WORKMODE,
  STATE_STARTUP_MAXCHARGE,   STATE_STARTUP_WAIT_MAXCHARGE,
  STATE_STARTUP_MAXDISCHARGE,STATE_STARTUP_WAIT_MAXDISCHARGE,

  // Fast cycle
  STATE_IDLE,
  STATE_SEND_BATT,           STATE_WAIT_BATT,
  STATE_SEND_AC,             STATE_WAIT_AC,

  // Slow cycle extras
  STATE_SEND_TEMPS,          STATE_WAIT_TEMPS,
  STATE_SEND_CTRL_RS485,     STATE_WAIT_CTRL_RS485,
  STATE_SEND_CTRL_FORCE,     STATE_WAIT_CTRL_FORCE,
  STATE_SEND_CTRL_POWER,     STATE_WAIT_CTRL_POWER,
  STATE_SEND_CTRL_WORKMODE,  STATE_WAIT_CTRL_WORKMODE,
  STATE_SEND_CTRL_MAXPOWER,  STATE_WAIT_CTRL_MAXPOWER,

  // Schedule read (on-demand)
  STATE_SEND_SCHEDULES,      STATE_WAIT_SCHEDULES,

  // Write processing
  STATE_PROCESS_WRITES,
  STATE_WAIT_WRITE,

  // Done
  STATE_CYCLE_COMPLETE
};

// ============================================================================
// Globals
// ============================================================================
ModbusRTU mb;
AsyncWebServer server(80);
Preferences prefs;

volatile bool    txnComplete   = false;
volatile uint8_t txnResultCode = 0;

bool modbusCallback(Modbus::ResultCode event, uint16_t transactionId, void* data) {
  txnResultCode = (uint8_t)event;
  txnComplete   = true;
  return true;
}

// ============================================================================
// Read buffers
// ============================================================================
uint16_t regBuf[30];  // shared buffer for batch reads

// ============================================================================
// Cache — battery monitoring
// ============================================================================
static uint16_t cache_batt_voltage  = 0;
static int16_t  cache_batt_current  = 0;
static int16_t  cache_batt_power    = 0;
static uint16_t cache_batt_soc      = 0;
static int16_t  cache_batt_temp1    = 0;

// Cache — AC
static uint16_t cache_ac_voltage    = 0;
static int16_t  cache_ac_current    = 0;
static int16_t  cache_ac_power      = 0;
static uint16_t cache_ac_freq       = 0;

// Cache — temperatures (32106-32110)
static int16_t  cache_temps[REG_TEMPS_COUNT] = {};

// Cache — control registers
static uint16_t cache_rs485_enable      = 0;
static uint16_t cache_force_mode        = 0;
static uint16_t cache_target_soc        = 0;
static uint16_t cache_charge_power      = 0;
static uint16_t cache_discharge_power   = 0;
static uint16_t cache_work_mode         = 0;
static uint16_t cache_max_charge        = 0;
static uint16_t cache_max_discharge     = 0;

// Cache — schedules
static uint16_t cache_schedules[REG_SCHED_COUNT] = {};
static bool     cache_schedules_valid = false;

// Cache — timing
static unsigned long cache_last_poll_ms = 0;

// ============================================================================
// Staging buffers (written during Modbus reads, committed at cycle end)
// ============================================================================
struct StagedFast {
  uint16_t batt[6];   // 32100-32105
  uint16_t ac[4];     // 32200-32203
};
static StagedFast staged_fast = {};

struct StagedSlow {
  int16_t  temps[REG_TEMPS_COUNT]; // 32106-32110
  uint16_t rs485_enable;
  uint16_t force_mode;
  uint16_t target_soc;
  uint16_t charge_power;
  uint16_t discharge_power;
  uint16_t work_mode;
  uint16_t max_charge;
  uint16_t max_discharge;
};
static StagedSlow staged_slow = {};

// ============================================================================
// Error tracking
// ============================================================================
static uint8_t       consecutiveErrors = 0;
static unsigned long lastSuccessTime   = 0;
static unsigned long currentBackoffMs  = 0;

void recordSuccess() {
  consecutiveErrors = 0;
  currentBackoffMs  = 0;
  lastSuccessTime   = millis();
}

void recordError(uint8_t code) {
  consecutiveErrors++;
  Serial.printf("[MODBUS] Error 0x%02X, consecutive=%u\n", code, consecutiveErrors);
  if (consecutiveErrors >= ERROR_LIMIT) {
    uint8_t shift = consecutiveErrors - ERROR_LIMIT;
    if (shift > 4) shift = 4;
    currentBackoffMs = min((unsigned long)(ERROR_BACKOFF_BASE << shift), ERROR_BACKOFF_MAX);
  }
}

// ============================================================================
// Write queue — ring buffer with dedup
// ============================================================================
#define WRITE_QUEUE_SIZE 16
static WriteEntry writeQueue[WRITE_QUEUE_SIZE];
static uint8_t    wqHead = 0;
static uint8_t    wqTail = 0;
static portMUX_TYPE writeMux = portMUX_INITIALIZER_UNLOCKED;

bool queueWrite(uint16_t addr, uint16_t value) {
  portENTER_CRITICAL(&writeMux);
  // Dedup: update existing entry for same register
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

bool writeQueueEmpty() {
  portENTER_CRITICAL(&writeMux);
  bool empty = (wqHead == wqTail);
  portEXIT_CRITICAL(&writeMux);
  return empty;
}

bool dequeueWrite(WriteEntry &entry) {
  portENTER_CRITICAL(&writeMux);
  if (wqHead == wqTail) {
    portEXIT_CRITICAL(&writeMux);
    return false;
  }
  entry = writeQueue[wqTail];
  wqTail = (wqTail + 1) % WRITE_QUEUE_SIZE;
  portEXIT_CRITICAL(&writeMux);
  return true;
}

// ============================================================================
// Power meter (B2500 Meter / Shelly Pro 3EM)
// ============================================================================
static char    meterIP[40] = "192.168.2.100";  // default, overridden by NVS
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
  if (meterIP[0] == '\0') return;

  HTTPClient http;
  String url = "http://" + String(meterIP) + "/rpc/EM.GetStatus?id=0";
  http.begin(url);
  http.setTimeout(METER_HTTP_TIMEOUT);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      meter_total_power = doc["total_act_power"] | 0.0f;
      meter_a_power     = doc["a_act_power"] | 0.0f;
      meter_b_power     = doc["b_act_power"] | 0.0f;
      meter_c_power     = doc["c_act_power"] | 0.0f;
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
// Auto-control
// ============================================================================
struct AutoControl {
  bool    enabled;
  uint8_t min_soc;
  uint8_t max_soc;
  float   offset_w;
  float   smoothing;    // EMA alpha (0-1)
  float   deadband_w;
};

static AutoControl autoCtrl = {
  .enabled    = false,
  .min_soc    = 10,
  .max_soc    = 95,
  .offset_w   = 50.0,
  .smoothing  = 0.3,
  .deadband_w = 100.0
};

static float   smoothed_grid_w     = 0.0;
static bool    smoothed_init       = false;
static float   auto_last_setpoint  = 0.0;
static uint8_t auto_last_action    = 0;  // 0=idle, 1=charge, 2=discharge
static unsigned long auto_last_update = 0;

void runAutoControl() {
  if (!autoCtrl.enabled) return;
  if (!meter_valid) return;
  if (millis() - meter_last_update > 10000) return;  // stale data

  // EMA smoothing
  if (!smoothed_init) {
    smoothed_grid_w = meter_total_power;
    smoothed_init = true;
  } else {
    smoothed_grid_w = autoCtrl.smoothing * meter_total_power
                    + (1.0 - autoCtrl.smoothing) * smoothed_grid_w;
  }

  float target_power = 0;
  uint8_t action = 0;  // 0=idle

  if (smoothed_grid_w > autoCtrl.offset_w) {
    // Importing from grid -> discharge battery
    if (cache_batt_soc > autoCtrl.min_soc) {
      target_power = min(smoothed_grid_w, 2500.0f);
      action = 2;  // discharge
    }
  } else if (smoothed_grid_w < -autoCtrl.offset_w) {
    // Exporting to grid -> charge battery
    if (cache_batt_soc < autoCtrl.max_soc) {
      target_power = min(fabsf(smoothed_grid_w), 2500.0f);
      action = 1;  // charge
    }
  }

  // Dead-band: only update if action changed or power delta > deadband
  bool shouldUpdate = false;
  if (action != auto_last_action) {
    shouldUpdate = true;
  } else if (fabsf(target_power - auto_last_setpoint) > autoCtrl.deadband_w) {
    shouldUpdate = true;
  }

  if (!shouldUpdate) return;

  uint16_t power_w = (uint16_t)target_power;

  if (action == 1) {
    // Charge
    queueWrite(REG_FORCE_MODE, 1);
    queueWrite(REG_CHARGE_POWER, power_w);
    queueWrite(REG_DISCHARGE_POWER, 0);
  } else if (action == 2) {
    // Discharge
    queueWrite(REG_FORCE_MODE, 2);
    queueWrite(REG_CHARGE_POWER, 0);
    queueWrite(REG_DISCHARGE_POWER, power_w);
  } else {
    // Idle
    queueWrite(REG_FORCE_MODE, 0);
    queueWrite(REG_CHARGE_POWER, 0);
    queueWrite(REG_DISCHARGE_POWER, 0);
  }

  auto_last_action   = action;
  auto_last_setpoint = target_power;
  auto_last_update   = millis();

  Serial.printf("[AUTO] action=%u power=%uW (grid=%.0fW smooth=%.0fW soc=%u%%)\n",
                action, power_w, meter_total_power, smoothed_grid_w, cache_batt_soc);
}

// ============================================================================
// Settings watchdog
// ============================================================================
void checkSettingsWatchdog() {
  bool reapply = false;

  if (cache_rs485_enable != RS485_ENABLE_MAGIC) {
    Serial.println("[WATCHDOG] RS485 control lost! Re-enabling...");
    reapply = true;
  }
  if (cache_max_charge != 2500) {
    Serial.printf("[WATCHDOG] Max charge power is %u, re-locking to 2500\n", cache_max_charge);
    queueWrite(REG_MAX_CHARGE, 2500);
  }
  if (cache_max_discharge != 2500) {
    Serial.printf("[WATCHDOG] Max discharge power is %u, re-locking to 2500\n", cache_max_discharge);
    queueWrite(REG_MAX_DISCHARGE, 2500);
  }
  if (autoCtrl.enabled && cache_work_mode != 0) {
    Serial.printf("[WATCHDOG] Work mode is %u, re-setting to Manual\n", cache_work_mode);
    queueWrite(REG_WORK_MODE, 0);
  }
  if (reapply) {
    queueWrite(REG_RS485_ENABLE, RS485_ENABLE_MAGIC);
    queueWrite(REG_WORK_MODE, 0);
    queueWrite(REG_MAX_CHARGE, 2500);
    queueWrite(REG_MAX_DISCHARGE, 2500);
  }
}

// ============================================================================
// State machine
// ============================================================================
static PollState     pollState           = STATE_STARTUP_RS485;
static unsigned long lastFastCycleStart  = 0;
static unsigned long lastSlowCycleStart  = 0;
static unsigned long lastTxnTime         = 0;
static bool          isSlowCycle         = false;
static bool          scheduleReadPending = false;

// Startup retry tracking
static uint8_t startupRetries = 0;
#define MAX_STARTUP_RETRIES 10

// Current write being processed
static WriteEntry currentWrite;

// ============================================================================
// State machine helpers
// ============================================================================
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

// ============================================================================
// State machine — main
// ============================================================================
void runStateMachine() {
  unsigned long now = millis();

  switch (pollState) {

  // ========== STARTUP SEQUENCE ==========
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
    if (txnResultCode == Modbus::EX_SUCCESS) {
      Serial.println("[STARTUP] Manual mode set");
      recordSuccess();
    } else {
      recordError(txnResultCode);
    }
    pollState = STATE_STARTUP_MAXCHARGE;
    break;

  case STATE_STARTUP_MAXCHARGE:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    Serial.println("[STARTUP] Locking max charge to 2500W...");
    trySendWriteHreg(REG_MAX_CHARGE, 2500, STATE_STARTUP_WAIT_MAXCHARGE);
    break;

  case STATE_STARTUP_WAIT_MAXCHARGE:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) {
      Serial.println("[STARTUP] Max charge locked");
      recordSuccess();
    } else {
      recordError(txnResultCode);
    }
    pollState = STATE_STARTUP_MAXDISCHARGE;
    break;

  case STATE_STARTUP_MAXDISCHARGE:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    Serial.println("[STARTUP] Locking max discharge to 2500W...");
    trySendWriteHreg(REG_MAX_DISCHARGE, 2500, STATE_STARTUP_WAIT_MAXDISCHARGE);
    break;

  case STATE_STARTUP_WAIT_MAXDISCHARGE:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) {
      Serial.println("[STARTUP] Max discharge locked");
      recordSuccess();
    } else {
      recordError(txnResultCode);
    }
    Serial.println("[STARTUP] Complete — entering normal operation");
    lastFastCycleStart = millis();
    lastSlowCycleStart = millis();
    pollState = STATE_IDLE;
    break;

  // ========== IDLE ==========
  case STATE_IDLE: {
    unsigned long interval = FAST_CYCLE_MS + currentBackoffMs;
    if (now - lastFastCycleStart >= interval) {
      isSlowCycle = (now - lastSlowCycleStart >= SLOW_CYCLE_MS);
      pollState = STATE_SEND_BATT;
    }
    break;
  }

  // ========== FAST CYCLE — Battery status (32100-32105, 6 regs) ==========
  case STATE_SEND_BATT:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    trySendReadHreg(REG_BATT_VOLTAGE, 6, STATE_WAIT_BATT);
    break;

  case STATE_WAIT_BATT:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) {
      memcpy(staged_fast.batt, regBuf, 6 * sizeof(uint16_t));
      recordSuccess();
    } else {
      recordError(txnResultCode);
    }
    pollState = STATE_SEND_AC;
    break;

  // ========== FAST CYCLE — AC status (32200-32203, 4 regs) ==========
  case STATE_SEND_AC:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    trySendReadHreg(REG_AC_VOLTAGE, 4, STATE_WAIT_AC);
    break;

  case STATE_WAIT_AC:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) {
      memcpy(staged_fast.ac, regBuf, 4 * sizeof(uint16_t));
      recordSuccess();
    } else {
      recordError(txnResultCode);
    }
    // Branch: slow cycle or skip to writes
    if (isSlowCycle) {
      pollState = STATE_SEND_TEMPS;
    } else if (scheduleReadPending) {
      pollState = STATE_SEND_SCHEDULES;
    } else {
      pollState = STATE_PROCESS_WRITES;
    }
    break;

  // ========== SLOW CYCLE — Temperatures (32106-32110, 5 regs) ==========
  case STATE_SEND_TEMPS:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    trySendReadHreg(REG_TEMPS_START, REG_TEMPS_COUNT, STATE_WAIT_TEMPS);
    break;

  case STATE_WAIT_TEMPS:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) {
      memcpy(staged_slow.temps, regBuf, REG_TEMPS_COUNT * sizeof(int16_t));
      recordSuccess();
    } else {
      recordError(txnResultCode);
    }
    pollState = STATE_SEND_CTRL_RS485;
    break;

  // ========== SLOW CYCLE — Control register reads ==========
  // Read RS485 enable (42000, 1 reg)
  case STATE_SEND_CTRL_RS485:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    trySendReadHreg(REG_RS485_ENABLE, 1, STATE_WAIT_CTRL_RS485);
    break;

  case STATE_WAIT_CTRL_RS485:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) {
      staged_slow.rs485_enable = regBuf[0];
      recordSuccess();
    } else {
      recordError(txnResultCode);
    }
    pollState = STATE_SEND_CTRL_FORCE;
    break;

  // Read force mode + target SOC (42010-42011, 2 regs)
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
    } else {
      recordError(txnResultCode);
    }
    pollState = STATE_SEND_CTRL_POWER;
    break;

  // Read charge + discharge power (42020-42021, 2 regs)
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
    } else {
      recordError(txnResultCode);
    }
    pollState = STATE_SEND_CTRL_WORKMODE;
    break;

  // Read work mode (43000, 1 reg)
  case STATE_SEND_CTRL_WORKMODE:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    trySendReadHreg(REG_WORK_MODE, 1, STATE_WAIT_CTRL_WORKMODE);
    break;

  case STATE_WAIT_CTRL_WORKMODE:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) {
      staged_slow.work_mode = regBuf[0];
      recordSuccess();
    } else {
      recordError(txnResultCode);
    }
    pollState = STATE_SEND_CTRL_MAXPOWER;
    break;

  // Read max charge + discharge (44002-44003, 2 regs)
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
    } else {
      recordError(txnResultCode);
    }
    // Check for schedule read or go to writes
    if (scheduleReadPending) {
      pollState = STATE_SEND_SCHEDULES;
    } else {
      pollState = STATE_PROCESS_WRITES;
    }
    break;

  // ========== SCHEDULE READ (on-demand) ==========
  case STATE_SEND_SCHEDULES:
    if (now - lastTxnTime < INTER_TXN_MS) break;
    trySendReadHreg(REG_SCHED_START, REG_SCHED_COUNT, STATE_WAIT_SCHEDULES);
    break;

  case STATE_WAIT_SCHEDULES:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) {
      memcpy(cache_schedules, regBuf, REG_SCHED_COUNT * sizeof(uint16_t));
      cache_schedules_valid = true;
      recordSuccess();
    } else {
      recordError(txnResultCode);
    }
    scheduleReadPending = false;
    pollState = STATE_PROCESS_WRITES;
    break;

  // ========== WRITE PROCESSING ==========
  case STATE_PROCESS_WRITES: {
    if (now - lastTxnTime < INTER_TXN_MS) break;
    if (dequeueWrite(currentWrite)) {
      txnComplete = false;
      uint16_t val = currentWrite.value;
      if (mb.writeHreg(SLAVE_ID, currentWrite.addr, &val, 1, modbusCallback)) {
        lastTxnTime = now;
        pollState = STATE_WAIT_WRITE;
      } else {
        recordError(0xFF);
        // Try next write
        pollState = STATE_PROCESS_WRITES;
      }
    } else {
      // No more writes
      pollState = STATE_CYCLE_COMPLETE;
    }
    break;
  }

  case STATE_WAIT_WRITE:
    if (!txnComplete) break;
    if (txnResultCode == Modbus::EX_SUCCESS) {
      Serial.printf("[WRITE] reg %u = %u OK\n", currentWrite.addr, currentWrite.value);
      recordSuccess();
    } else {
      Serial.printf("[WRITE] reg %u = %u FAILED (0x%02X)\n",
                    currentWrite.addr, currentWrite.value, txnResultCode);
      recordError(txnResultCode);
    }
    // Process more writes
    pollState = STATE_PROCESS_WRITES;
    break;

  // ========== COMMIT ==========
  case STATE_CYCLE_COMPLETE:
    // Commit fast data
    cache_batt_voltage = staged_fast.batt[0];
    cache_batt_current = (int16_t)staged_fast.batt[1];
    cache_batt_power   = (int16_t)staged_fast.batt[2];
    // batt[3] is reserved
    cache_batt_soc     = staged_fast.batt[4];
    cache_batt_temp1   = (int16_t)staged_fast.batt[5];

    cache_ac_voltage   = staged_fast.ac[0];
    cache_ac_current   = (int16_t)staged_fast.ac[1];
    cache_ac_power     = (int16_t)staged_fast.ac[2];
    cache_ac_freq      = staged_fast.ac[3];

    // Commit slow data if this was a slow cycle
    if (isSlowCycle) {
      memcpy(cache_temps, staged_slow.temps, sizeof(cache_temps));
      cache_rs485_enable    = staged_slow.rs485_enable;
      cache_force_mode      = staged_slow.force_mode;
      cache_target_soc      = staged_slow.target_soc;
      cache_charge_power    = staged_slow.charge_power;
      cache_discharge_power = staged_slow.discharge_power;
      cache_work_mode       = staged_slow.work_mode;
      cache_max_charge      = staged_slow.max_charge;
      cache_max_discharge   = staged_slow.max_discharge;

      // Run watchdog after committing slow data
      checkSettingsWatchdog();
      lastSlowCycleStart = millis();
    }

    // Run auto-control after every fast cycle
    runAutoControl();

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
  WiFi.config(staticIP, gateway, subnet);
  WiFi.begin(ssid, password);
}

// ============================================================================
// NVS persistence
// ============================================================================
void loadPreferences() {
  prefs.begin("marstek", true);  // read-only
  String ip = prefs.getString("meter_ip", "");
  if (ip.length() > 0 && ip.length() < sizeof(meterIP)) {
    strncpy(meterIP, ip.c_str(), sizeof(meterIP) - 1);
    meterIP[sizeof(meterIP) - 1] = '\0';
  }
  autoCtrl.enabled    = prefs.getBool("auto_on", false);
  autoCtrl.min_soc    = prefs.getUChar("auto_min_soc", 10);
  autoCtrl.max_soc    = prefs.getUChar("auto_max_soc", 95);
  autoCtrl.offset_w   = prefs.getFloat("auto_offset", 50.0);
  autoCtrl.smoothing  = prefs.getFloat("auto_smooth", 0.3);
  autoCtrl.deadband_w = prefs.getFloat("auto_deadband", 100.0);
  FAST_CYCLE_MS       = prefs.getULong("fast_cycle", 2000);
  if (FAST_CYCLE_MS < 1000) FAST_CYCLE_MS = 1000;
  prefs.end();
}

void savePreferences() {
  prefs.begin("marstek", false);  // read-write
  prefs.putString("meter_ip", meterIP);
  prefs.putBool("auto_on", autoCtrl.enabled);
  prefs.putUChar("auto_min_soc", autoCtrl.min_soc);
  prefs.putUChar("auto_max_soc", autoCtrl.max_soc);
  prefs.putFloat("auto_offset", autoCtrl.offset_w);
  prefs.putFloat("auto_smooth", autoCtrl.smoothing);
  prefs.putFloat("auto_deadband", autoCtrl.deadband_w);
  prefs.putULong("fast_cycle", FAST_CYCLE_MS);
  prefs.end();
}

// ============================================================================
// setup()
// ============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n[BOOT] Marstek Venus E3 Controller v1.0");

  // Load NVS config
  loadPreferences();
  Serial.printf("[CONFIG] Meter IP: %s\n", meterIP);
  Serial.printf("[CONFIG] Auto: %s, MinSOC=%u, MaxSOC=%u, Offset=%.0f, Smooth=%.2f\n",
                autoCtrl.enabled ? "ON" : "OFF",
                autoCtrl.min_soc, autoCtrl.max_soc,
                autoCtrl.offset_w, autoCtrl.smoothing);
  Serial.printf("[CONFIG] Fast cycle: %lu ms\n", FAST_CYCLE_MS);

  // RS485 transceiver (all active HIGH on T-CAN485)
  pinMode(RS485_5V_PIN, OUTPUT);
  digitalWrite(RS485_5V_PIN, HIGH);
  pinMode(RS485_EN_PIN, OUTPUT);
  digitalWrite(RS485_EN_PIN, HIGH);
  pinMode(RS485_SE_PIN, OUTPUT);
  digitalWrite(RS485_SE_PIN, HIGH);

  // WiFi
  WiFi.config(staticIP, gateway, subnet);
  WiFi.begin(ssid, password);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Connected. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WIFI] Failed — will retry in loop");
  }

  // mDNS
  if (MDNS.begin("marstek-battery")) {
    MDNS.addService("http", "tcp", 80);
  }

  // OTA
  ArduinoOTA.setHostname("marstek-battery");
  ArduinoOTA.setPassword("admin");
  ArduinoOTA.onStart([]() { Serial.println("[OTA] Starting..."); });
  ArduinoOTA.onEnd([]()   { Serial.println("\n[OTA] Complete!"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]\n", error);
  });
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

  // ========== HTTP Routes ==========

  // GET / — device info
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json",
      "{\"device\":\"Marstek Venus E3 Controller\",\"version\":\"1.0\"}");
  });

  // GET /health
  server.on("/health", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<256> doc;
    doc["status"]              = "ok";
    doc["uptime"]              = millis() / 1000;
    doc["wifi_rssi"]           = WiFi.RSSI();
    doc["modbus_errors"]       = consecutiveErrors;
    doc["modbus_backoff_ms"]   = currentBackoffMs;
    doc["meter_age_ms"]        = meter_valid ? (millis() - meter_last_update) : -1;
    doc["meter_count"]         = meter_telegram_count;
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // GET /status — full state
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<1536> doc;

    // Battery
    JsonObject batt = doc.createNestedObject("battery");
    batt["voltage"]  = cache_batt_voltage / 100.0;
    batt["current"]  = cache_batt_current / 100.0;
    batt["power"]    = cache_batt_power;
    batt["soc"]      = cache_batt_soc;
    batt["temp1"]    = cache_batt_temp1 / 10.0;

    // AC
    JsonObject ac = doc.createNestedObject("ac");
    ac["voltage"]    = cache_ac_voltage / 10.0;
    ac["current"]    = cache_ac_current / 100.0;
    ac["power"]      = cache_ac_power;
    ac["frequency"]  = cache_ac_freq / 100.0;

    // Temperatures
    JsonArray temps = doc.createNestedArray("temperatures");
    temps.add(cache_batt_temp1 / 10.0);
    for (int i = 0; i < REG_TEMPS_COUNT; i++) {
      temps.add(cache_temps[i] / 10.0);
    }

    // Grid / meter
    JsonObject grid = doc.createNestedObject("grid");
    grid["total_power_w"]    = meter_total_power;
    grid["smoothed_power_w"] = smoothed_grid_w;
    grid["a_power_w"]        = meter_a_power;
    grid["b_power_w"]        = meter_b_power;
    grid["c_power_w"]        = meter_c_power;
    grid["age_ms"]           = meter_valid ? (long)(millis() - meter_last_update) : -1;

    // Control
    JsonObject ctrl = doc.createNestedObject("control");
    ctrl["rs485_enabled"]     = (cache_rs485_enable == RS485_ENABLE_MAGIC);
    ctrl["work_mode"]         = cache_work_mode;
    ctrl["force_mode"]        = cache_force_mode;
    ctrl["charge_power"]      = cache_charge_power;
    ctrl["discharge_power"]   = cache_discharge_power;
    ctrl["target_soc"]        = cache_target_soc;
    ctrl["max_charge_power"]  = cache_max_charge;
    ctrl["max_discharge_power"] = cache_max_discharge;

    // Auto-control
    JsonObject ac2 = doc.createNestedObject("auto_control");
    ac2["enabled"]        = autoCtrl.enabled;
    ac2["last_action"]    = auto_last_action;
    ac2["last_setpoint_w"] = auto_last_setpoint;

    // Diagnostics
    doc["_errors"]        = consecutiveErrors;
    doc["_backoff_ms"]    = currentBackoffMs;
    doc["_last_poll_ms"]  = cache_last_poll_ms;
    doc["_uptime"]        = millis() / 1000;
    doc["_rssi"]          = WiFi.RSSI();

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // POST /control — manual battery control
  server.on("/control", HTTP_POST, [](AsyncWebServerRequest *request){},
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<256> doc;
      DeserializationError err = deserializeJson(doc, data, len);
      if (err) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      JsonObject root = doc.as<JsonObject>();
      int queued = 0;

      if (root.containsKey("work_mode")) {
        queueWrite(REG_WORK_MODE, root["work_mode"].as<uint16_t>());
        queued++;
      }
      if (root.containsKey("force_mode")) {
        queueWrite(REG_FORCE_MODE, root["force_mode"].as<uint16_t>());
        queued++;
      }
      if (root.containsKey("charge_power")) {
        queueWrite(REG_CHARGE_POWER, root["charge_power"].as<uint16_t>());
        queued++;
      }
      if (root.containsKey("discharge_power")) {
        queueWrite(REG_DISCHARGE_POWER, root["discharge_power"].as<uint16_t>());
        queued++;
      }
      if (root.containsKey("target_soc")) {
        queueWrite(REG_TARGET_SOC, root["target_soc"].as<uint16_t>());
        queued++;
      }
      if (root.containsKey("max_charge_power")) {
        queueWrite(REG_MAX_CHARGE, root["max_charge_power"].as<uint16_t>());
        queued++;
      }
      if (root.containsKey("max_discharge_power")) {
        queueWrite(REG_MAX_DISCHARGE, root["max_discharge_power"].as<uint16_t>());
        queued++;
      }

      StaticJsonDocument<128> resp;
      resp["status"] = "queued";
      resp["writes"] = queued;
      String json;
      serializeJson(resp, json);
      request->send(202, "application/json", json);
    }
  );

  // GET /meter — power meter data
  server.on("/meter", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<256> doc;
    doc["total_power_w"] = meter_total_power;
    doc["a_power_w"]     = meter_a_power;
    doc["b_power_w"]     = meter_b_power;
    doc["c_power_w"]     = meter_c_power;
    doc["age_ms"]        = meter_valid ? (long)(millis() - meter_last_update) : -1;
    doc["valid"]         = meter_valid;
    doc["count"]         = meter_telegram_count;
    doc["meter_ip"]      = meterIP;
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // POST /config — set meter IP and fast cycle
  server.on("/config", HTTP_POST, [](AsyncWebServerRequest *request){},
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<256> doc;
      DeserializationError err = deserializeJson(doc, data, len);
      if (err) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      JsonObject root = doc.as<JsonObject>();
      bool changed = false;

      if (root.containsKey("meter_ip")) {
        String ip = root["meter_ip"].as<String>();
        if (ip.length() > 0 && ip.length() < sizeof(meterIP)) {
          strncpy(meterIP, ip.c_str(), sizeof(meterIP) - 1);
          meterIP[sizeof(meterIP) - 1] = '\0';
          meter_valid = false;
          changed = true;
        }
      }
      if (root.containsKey("fast_cycle_ms")) {
        unsigned long val = root["fast_cycle_ms"].as<unsigned long>();
        if (val >= 1000) {
          FAST_CYCLE_MS = val;
          changed = true;
        }
      }

      if (changed) savePreferences();

      StaticJsonDocument<128> resp;
      resp["status"]        = "ok";
      resp["meter_ip"]      = meterIP;
      resp["fast_cycle_ms"] = FAST_CYCLE_MS;
      String json;
      serializeJson(resp, json);
      request->send(200, "application/json", json);
    }
  );

  // GET /schedules — read all 6 schedules
  server.on("/schedules", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!cache_schedules_valid) {
      // Trigger a schedule read on next cycle
      scheduleReadPending = true;
      request->send(202, "application/json",
        "{\"status\":\"pending\",\"message\":\"Schedule read queued, retry in a few seconds\"}");
      return;
    }

    StaticJsonDocument<1024> doc;
    JsonArray arr = doc.createNestedArray("schedules");

    for (int s = 0; s < 6; s++) {
      JsonObject sched = arr.createNestedObject();
      int base = s * 5;
      sched["index"]      = s;
      uint16_t startRaw   = cache_schedules[base + 0];
      uint16_t endRaw     = cache_schedules[base + 1];
      sched["start_time"] = String(startRaw / 100) + ":" + (startRaw % 100 < 10 ? "0" : "") + String(startRaw % 100);
      sched["end_time"]   = String(endRaw / 100) + ":" + (endRaw % 100 < 10 ? "0" : "") + String(endRaw % 100);
      sched["enabled"]    = (bool)cache_schedules[base + 2];
      sched["mode"]       = cache_schedules[base + 3];
      sched["power"]      = cache_schedules[base + 4];
    }

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // POST /schedules — write a single schedule
  server.on("/schedules", HTTP_POST, [](AsyncWebServerRequest *request){},
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<256> doc;
      DeserializationError err = deserializeJson(doc, data, len);
      if (err) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      JsonObject root = doc.as<JsonObject>();

      if (!root.containsKey("index")) {
        request->send(400, "application/json", "{\"error\":\"Missing 'index' (0-5)\"}");
        return;
      }

      int idx = root["index"].as<int>();
      if (idx < 0 || idx > 5) {
        request->send(400, "application/json", "{\"error\":\"Index must be 0-5\"}");
        return;
      }

      uint16_t baseAddr = REG_SCHED_START + idx * 5;

      if (root.containsKey("start_time")) {
        // Accept "HH:MM" string or HHMM integer
        if (root["start_time"].is<const char*>()) {
          String t = root["start_time"].as<String>();
          int h = t.substring(0, t.indexOf(':')).toInt();
          int m = t.substring(t.indexOf(':') + 1).toInt();
          queueWrite(baseAddr + 0, h * 100 + m);
        } else {
          queueWrite(baseAddr + 0, root["start_time"].as<uint16_t>());
        }
      }
      if (root.containsKey("end_time")) {
        if (root["end_time"].is<const char*>()) {
          String t = root["end_time"].as<String>();
          int h = t.substring(0, t.indexOf(':')).toInt();
          int m = t.substring(t.indexOf(':') + 1).toInt();
          queueWrite(baseAddr + 1, h * 100 + m);
        } else {
          queueWrite(baseAddr + 1, root["end_time"].as<uint16_t>());
        }
      }
      if (root.containsKey("enabled")) {
        queueWrite(baseAddr + 2, root["enabled"].as<bool>() ? 1 : 0);
      }
      if (root.containsKey("mode")) {
        queueWrite(baseAddr + 3, root["mode"].as<uint16_t>());
      }
      if (root.containsKey("power")) {
        queueWrite(baseAddr + 4, root["power"].as<uint16_t>());
      }

      // Invalidate cache so next GET triggers a re-read
      cache_schedules_valid = false;
      scheduleReadPending = true;

      request->send(202, "application/json", "{\"status\":\"queued\"}");
    }
  );

  // POST /auto — configure auto-control
  server.on("/auto", HTTP_POST, [](AsyncWebServerRequest *request){},
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<256> doc;
      DeserializationError err = deserializeJson(doc, data, len);
      if (err) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      JsonObject root = doc.as<JsonObject>();

      if (root.containsKey("enabled"))    autoCtrl.enabled    = root["enabled"].as<bool>();
      if (root.containsKey("min_soc"))    autoCtrl.min_soc    = root["min_soc"].as<uint8_t>();
      if (root.containsKey("max_soc"))    autoCtrl.max_soc    = root["max_soc"].as<uint8_t>();
      if (root.containsKey("offset_w"))   autoCtrl.offset_w   = root["offset_w"].as<float>();
      if (root.containsKey("smoothing"))  autoCtrl.smoothing  = root["smoothing"].as<float>();
      if (root.containsKey("deadband_w")) autoCtrl.deadband_w = root["deadband_w"].as<float>();

      // Reset EMA when config changes
      smoothed_init = false;

      savePreferences();

      // If disabling auto, stop battery
      if (!autoCtrl.enabled) {
        queueWrite(REG_FORCE_MODE, 0);
        queueWrite(REG_CHARGE_POWER, 0);
        queueWrite(REG_DISCHARGE_POWER, 0);
        auto_last_action = 0;
        auto_last_setpoint = 0;
      }

      StaticJsonDocument<256> resp;
      resp["status"]    = "ok";
      resp["enabled"]   = autoCtrl.enabled;
      resp["min_soc"]   = autoCtrl.min_soc;
      resp["max_soc"]   = autoCtrl.max_soc;
      resp["offset_w"]  = autoCtrl.offset_w;
      resp["smoothing"] = autoCtrl.smoothing;
      resp["deadband_w"] = autoCtrl.deadband_w;
      String json;
      serializeJson(resp, json);
      request->send(200, "application/json", json);
    }
  );

  // GET /auto — read auto-control config
  server.on("/auto", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<256> doc;
    doc["enabled"]         = autoCtrl.enabled;
    doc["min_soc"]         = autoCtrl.min_soc;
    doc["max_soc"]         = autoCtrl.max_soc;
    doc["offset_w"]        = autoCtrl.offset_w;
    doc["smoothing"]       = autoCtrl.smoothing;
    doc["deadband_w"]      = autoCtrl.deadband_w;
    doc["last_action"]     = auto_last_action;
    doc["last_setpoint_w"] = auto_last_setpoint;
    doc["smoothed_grid_w"] = smoothed_grid_w;
    doc["fast_cycle_ms"]   = FAST_CYCLE_MS;
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  server.begin();
  Serial.println("[HTTP] Server started on port 80");
  Serial.println("[BOOT] Setup complete, entering main loop");
}

// ============================================================================
// loop()
// ============================================================================
void loop() {
  ArduinoOTA.handle();
  mb.task();
  handleWiFi();
  pollMeter();
  runStateMachine();
}
