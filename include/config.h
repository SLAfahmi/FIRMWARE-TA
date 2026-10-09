#pragma once
#include <Arduino.h>

// ==========================================
// SYSTEM & ZONE CONFIGURATION
// ==========================================
#define NUM_ZONES 3

// ==========================================
// WIFI ACCESS POINT
// ==========================================
constexpr const char *AP_SSID = "SmartNursery-TelU";
constexpr const char *AP_PASSWORD = "12345678";

// ==========================================
// WIFI STATION (Router Internet)
// ==========================================
constexpr const char *STA_SSID = "IT_SRS";
constexpr const char *STA_PASSWORD = "SRS@2026";

// ==========================================
// CLOUD SERVER CONFIG (Laravel REST API)
// ==========================================
constexpr const char *CLOUD_URL = "http://10.9.116.145:8000/api/v1/telemetry";
constexpr const char *CLOUD_BATCH_URL = "http://10.9.116.145:8000/api/v1/telemetry/sync-batch";
constexpr const char *DEVICE_KEY = "NURSERY_01";

// ==========================================
// STORAGE CONFIG (LittleFS)
// ==========================================
constexpr const char *LOG_FILE = "/logs.txt";
constexpr const char *SCHEDULE_FILE = "/schedule.json";
constexpr const char *OFFLINE_QUEUE_FILE = "/offline_q.json";

#define MAX_LOG_ENTRIES 100
#define MAX_OFFLINE_ENTRIES 50
#define MAX_SCHEDULES 3

// ==========================================
// PIN DEFINITIONS (ESP32-S3)
// ==========================================
// Soil Moisture Sensors (ADC1: GPIO 1, 2, 3)
const int SOIL_PIN[NUM_ZONES] = {1, 2, 3};

// DHT22 Pin
#define DHT_PIN 4
#define DHT_TYPE DHT22

// Relay Pins
#define RELAY_MAIN 13
const int RELAY_PIN[NUM_ZONES] = {15, 18, 14};

// Relay Active Level (Active LOW)
#define RELAY_ON LOW
#define RELAY_OFF HIGH
