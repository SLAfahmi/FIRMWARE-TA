#pragma once
#include <Arduino.h>
#include <DHT.h>
#include <BH1750.h>
#include <RTClib.h>
#include <WebServer.h>
#include "config.h"
#include "types.h"

// ==========================================
// HARDWARE OBJECTS (EXTERN)
// ==========================================
extern DHT dht;
extern BH1750 lightMeter;
extern RTC_DS3231 rtc;
extern WebServer server;

// ==========================================
// FREERTOS HANDLES & MUTEX (EXTERN)
// ==========================================
extern TaskHandle_t fuzzyHandle;
extern TaskHandle_t irrigationHandle[NUM_ZONES];
extern TaskHandle_t webHandle;
extern TaskHandle_t cloudSyncHandle;
extern TaskHandle_t manualControlHandle;
extern TaskHandle_t scheduleHandle;
extern TaskHandle_t sensorHandle;
extern TaskHandle_t memoryHandle;

extern SemaphoreHandle_t zonaMutex;
extern SemaphoreHandle_t maxConcurrentIrrigationSemaphore;
extern SemaphoreHandle_t logMutex;
extern portMUX_TYPE mux;

// ==========================================
// SYSTEM VARIABLES & STATES (EXTERN)
// ==========================================
extern uint32_t lastWateringTime[NUM_ZONES];
extern uint32_t cfgCooldown;
extern bool rtcAvailable;
extern volatile int activeIrrigations;

extern Zona zona[NUM_ZONES];
extern float temperature;
extern float humidity;
extern float lightLevel;

extern int systemMode; // 0=Auto, 1=Manual, 2=Schedule
extern bool manualRelayMain;
extern bool manualRelayState[NUM_ZONES];

extern Schedule zoneSchedules[NUM_ZONES][MAX_SCHEDULES];
extern int lastScheduledMinute;

// Web Config Variables
extern uint32_t cfgSensorInterval;
extern int cfgManualMode;
extern uint32_t cfgManualDur;
extern uint32_t cfgLogInterval;

// Manual Timers
extern uint32_t manualTimerOff[NUM_ZONES];
extern uint32_t manualStartTime[NUM_ZONES];

// Log Buffer
extern LogEntry *logBuffer;
extern int logHead;
extern int logCount;

// Helper Functions
uint32_t getCurrentTimeSeconds();
void updateMainRelayState();
void addLogEntry();
