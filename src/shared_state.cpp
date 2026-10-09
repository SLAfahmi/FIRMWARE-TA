#include "shared_state.h"
#include "storage_manager.h"

// ==========================================
// HARDWARE OBJECTS
// ==========================================
DHT dht(DHT_PIN, DHT_TYPE);
BH1750 lightMeter;
RTC_DS3231 rtc;
WebServer server(80);

// ==========================================
// FREERTOS HANDLES & MUTEX
// ==========================================
TaskHandle_t fuzzyHandle = nullptr;
TaskHandle_t irrigationHandle[NUM_ZONES] = {nullptr};
TaskHandle_t webHandle = nullptr;
TaskHandle_t cloudSyncHandle = nullptr;
TaskHandle_t manualControlHandle = nullptr;
TaskHandle_t scheduleHandle = nullptr;
TaskHandle_t sensorHandle = nullptr;
TaskHandle_t memoryHandle = nullptr;

SemaphoreHandle_t zonaMutex = nullptr;
SemaphoreHandle_t maxConcurrentIrrigationSemaphore = nullptr;
SemaphoreHandle_t logMutex = nullptr;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// ==========================================
// SYSTEM VARIABLES & STATES
// ==========================================
uint32_t lastWateringTime[NUM_ZONES] = {0};
uint32_t cfgCooldown = 4 * 60 * 60; // 4 jam cooldown
bool rtcAvailable = false;
volatile int activeIrrigations = 0;

Zona zona[NUM_ZONES] = {0};
float temperature = 0.0f;
float humidity = 0.0f;
float lightLevel = 0.0f;

int systemMode = 0; // 0=Auto, 1=Manual, 2=Schedule
bool manualRelayMain = false;
bool manualRelayState[NUM_ZONES] = {false};

Schedule zoneSchedules[NUM_ZONES][MAX_SCHEDULES] = {0};
int lastScheduledMinute = -1;

uint32_t cfgSensorInterval = 10; // detik
int cfgManualMode = 0;           // 0=Toggle, 1=Timer
uint32_t cfgManualDur = 10;      // detik
uint32_t cfgLogInterval = 3600;  // detik (1 jam)

uint32_t manualTimerOff[NUM_ZONES] = {0};
uint32_t manualStartTime[NUM_ZONES] = {0};

LogEntry *logBuffer = nullptr;
int logHead = 0;
int logCount = 0;

// ==========================================
// HELPER FUNCTIONS IMPLEMENTATION
// ==========================================
uint32_t getCurrentTimeSeconds() {
  if (rtcAvailable) {
    DateTime now = rtc.now();
    return now.unixtime();
  }
  return millis() / 1000UL;
}

void updateMainRelayState() {
  bool anyOn = false;
  for (int i = 0; i < NUM_ZONES; i++) {
    if (manualRelayState[i]) {
      anyOn = true;
      break;
    }
  }
  manualRelayMain = anyOn;
  digitalWrite(RELAY_MAIN, manualRelayMain ? RELAY_ON : RELAY_OFF);
}

void addLogEntry() {
  if (!logBuffer || !logMutex)
    return;
  if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(200)) != pdTRUE)
    return;

  if (rtcAvailable) {
    DateTime now = rtc.now();
    snprintf(logBuffer[logHead].timestamp, sizeof(logBuffer[logHead].timestamp),
             "%04d/%02d/%02d %02d:%02d:%02d",
             now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
  } else {
    uint32_t sec = millis() / 1000;
    snprintf(logBuffer[logHead].timestamp, sizeof(logBuffer[logHead].timestamp),
             "UPTIME %u s", sec);
  }

  logBuffer[logHead].temp = temperature;
  logBuffer[logHead].hum = humidity;
  logBuffer[logHead].light = lightLevel;
  for (int i = 0; i < NUM_ZONES; i++) {
    logBuffer[logHead].soilBefore[i] = zona[i].soilBefore;
    logBuffer[logHead].soilAfter[i] = zona[i].soil;
    logBuffer[logHead].mode[i] = zona[i].mode;
    logBuffer[logHead].duration[i] = zona[i].duration;
  }
  logBuffer[logHead].sysMode = systemMode;
  logHead = (logHead + 1) % MAX_LOG_ENTRIES;
  if (logCount < MAX_LOG_ENTRIES)
    logCount++;

  saveLogFile();
  xSemaphoreGive(logMutex);
}
