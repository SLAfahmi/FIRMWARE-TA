#include <Arduino.h>
#include <WiFi.h>
#include <BH1750.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <WiFiClientSecure.h>
#include <Wire.h>

#include <esp_heap_caps.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#include <WebServer.h>

// ==========================================
// WIFI
// ==========================================

const char *apSsid = "SmartNursery-TelU";
const char *apPassword = "12345678";

// ==========================================
// WIFI STATION (Router Internet)
// ==========================================
const char *staSsid = "IT_SRS";
const char *staPassword = "SRS@2026";

// ==========================================
// CLOUD SERVER CONFIG
// ==========================================
// URL ke cloud server
const char *cloudUrl = "http://10.9.116.145:8000/api/v1/telemetry";
const char *cloudBatchUrl =
    "http://10.9.116.145:8000/api/v1/telemetry/sync-batch";
const char *deviceKey = "NURSERY_01";

// File antrian offline di LittleFS (bukan SPIFFS)
const char *OFFLINE_QUEUE_FILE = "/offline_q.json";
#define MAX_OFFLINE_ENTRIES 50

// ==========================================
// PIN
// ==========================================

// Pin Moisture Sensor (ADC1 ESP32-S3: GPIO 1, 2, 3, 6, 7)
// Catatan: GPIO 33, 34, 35, 36 dipakai oleh Octal PSRAM N16R8, jangan
// digunakan!
const int soilPin[5] = {1, 2, 3, 6, 7};

#define DHT_PIN 4
#define DHT_TYPE DHT22

#define RELAY_MAIN 13
// #define RELAY_POMPA 13 //udah gadipakai
// RELAY PER ZONA (Pin 19 tidak boleh dipakai karena merupakan USB D- pada
// ESP32-S3!)
const int relayPin[5] = {15, 18, 14, 5, 27};

// RELAY ACTIVE LOW
#define RELAY_ON LOW
#define RELAY_OFF HIGH

// ==========================================
// SENSOR & WEBSERVER
// ==========================================

DHT dht(DHT_PIN, DHT_TYPE);
BH1750 lightMeter;
RTC_DS3231 rtc;
WebServer server(80);

// ==========================================
// TASK HANDLE
// ==========================================

TaskHandle_t fuzzyHandle;
TaskHandle_t irrigationHandle[5];
TaskHandle_t webHandle;
TaskHandle_t cloudSyncHandle;
TaskHandle_t manualControlHandle;
TaskHandle_t scheduleHandle;
SemaphoreHandle_t zonaMutex;
SemaphoreHandle_t maxConcurrentIrrigationSemaphore;

// ==========================================
// VARIABLE
// ==========================================

uint32_t lastWateringTime[5] = {
    0}; // Menyimpan waktu terakhir disiram (Unix time / fallback millis)
uint32_t cfgCooldown =
    4 * 60 * 60; // Cooldown setelah penyiraman ke penyiraman berikutnya 4 jam
bool rtcAvailable = false;

uint32_t getCurrentTimeSeconds() {
  if (rtcAvailable) {
    DateTime now = rtc.now();
    return now.unixtime();
  }
  return millis() / 1000UL;
}

void printMemoryInfo();
void MemoryTask(void *pv);

struct Zona {
  float soil;
  float soilBefore;
  float duration;
  int mode; // 0=Off, 1=Short, 2=Medium, 3=Long
};

Zona zona[5];

float temperature;
float humidity;
float lightLevel;

// Count of active irrigation tasks
volatile int activeIrrigations = 0;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// 0 = Auto, 1 = Manual, 2 = Schedule
int systemMode = 0;
bool manualRelayMain = false;
bool manualRelayState[5] = {false};

struct Schedule {
  bool active;
  uint8_t hour;
  uint8_t minute;
  uint32_t duration;
};
#define MAX_SCHEDULES 3
Schedule zoneSchedules[5][MAX_SCHEDULES];
int lastScheduledMinute = -1;

// ==========================================
// CONFIGURATIONS (WEB)
// ==========================================
uint32_t cfgSensorInterval = 10; // detik
int cfgManualMode = 0;           // 0 = Toggle, 1 = Timer
uint32_t cfgManualDur = 10;      // detik (durasi timer manual)
uint32_t cfgLogInterval = 3600;  // detik (interval log otomatis)
// File log persistensi SPIFFS
const char *LOG_FILE = "/logs.txt";

// Timer untuk manual mode
uint32_t manualTimerOff[5] = {0};
uint32_t manualStartTime[5] = {0};

void updateMainRelayState() {
  bool anyOn = false;
  for (int i = 0; i < 5; i++) {
    if (manualRelayState[i]) {
      anyOn = true;
      break;
    }
  }
  manualRelayMain = anyOn;
  digitalWrite(RELAY_MAIN, manualRelayMain ? RELAY_ON : RELAY_OFF);
  // digitalWrite(RELAY_POMPA, manualRelayMain ? RELAY_ON : RELAY_OFF);
}

// ==========================================
// LOG BUFFER
// ==========================================
struct LogEntry {
  char timestamp[20];
  float temp;
  float hum;
  float light;
  float soilBefore[5];
  float soilAfter[5];
  int mode[5];
  float duration[5];
  int sysMode;
};
#define MAX_LOG_ENTRIES 100
// Alokasikan di heap bukan static RAM agar tidak memakan DRAM sejak boot
LogEntry *logBuffer = nullptr;
int logHead = 0;
int logCount = 0;
SemaphoreHandle_t logMutex = nullptr;

bool saveLogFile() {
  if (!logBuffer)
    return false;
  File file = SPIFFS.open(LOG_FILE, "w");
  if (!file)
    return false;

  for (int i = 0; i < logCount; i++) {
    int idx = (logHead + MAX_LOG_ENTRIES - logCount + i) % MAX_LOG_ENTRIES;
    file.printf("%s,%.1f,%.1f,%.1f", logBuffer[idx].timestamp,
                logBuffer[idx].temp, logBuffer[idx].hum, logBuffer[idx].light);
    for (int j = 0; j < 5; j++) {
      file.printf(",%.1f,%.1f,%d,%.1f", logBuffer[idx].soilBefore[j],
                  logBuffer[idx].soilAfter[j], logBuffer[idx].mode[j],
                  logBuffer[idx].duration[j]);
    }
    file.printf(",%d\n", logBuffer[idx].sysMode);
  }

  file.close();
  return true;
}

bool loadLogFile() {
  if (!logBuffer)
    return false;
  if (!SPIFFS.exists(LOG_FILE))
    return false;

  File file = SPIFFS.open(LOG_FILE, "r");
  if (!file)
    return false;

  logHead = 0;
  logCount = 0;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.length() < 5)
      continue;

    char buf[256];
    line.toCharArray(buf, sizeof(buf));
    char *token = strtok(buf, ",");
    if (!token)
      continue;

    LogEntry entry;
    strncpy(entry.timestamp, token, sizeof(entry.timestamp) - 1);
    entry.timestamp[sizeof(entry.timestamp) - 1] = '\0';

    token = strtok(NULL, ",");
    if (!token)
      continue;
    entry.temp = atof(token);
    token = strtok(NULL, ",");
    entry.hum = atof(token);
    token = strtok(NULL, ",");
    entry.light = atof(token);

    for (int j = 0; j < 5; j++) {
      token = strtok(NULL, ",");
      entry.soilBefore[j] = token ? atof(token) : 0.0;
      token = strtok(NULL, ",");
      entry.soilAfter[j] = token ? atof(token) : 0.0;
      token = strtok(NULL, ",");
      entry.mode[j] = token ? atoi(token) : 0;
      token = strtok(NULL, ",");
      entry.duration[j] = token ? atof(token) : 0.0;
    }

    token = strtok(NULL, ",");
    entry.sysMode = token ? atoi(token) : 0;

    logBuffer[logHead] = entry;
    logHead = (logHead + 1) % MAX_LOG_ENTRIES;
    if (logCount < MAX_LOG_ENTRIES)
      logCount++;
  }

  file.close();
  return true;
}

void addLogEntry() {
  if (!logBuffer || !logMutex)
    return;
  if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(200)) != pdTRUE)
    return;

  DateTime now = rtc.now();
  sprintf(logBuffer[logHead].timestamp, "%04d/%02d/%02d %02d:%02d:%02d",
          now.year(), now.month(), now.day(), now.hour(), now.minute(),
          now.second());
  logBuffer[logHead].temp = temperature;
  logBuffer[logHead].hum = humidity;
  logBuffer[logHead].light = lightLevel;
  for (int i = 0; i < 5; i++) {
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

// ==========================================
// FUZZY FUNCTION (UMUM)
// ==========================================

float fungsiMin(float a, float b) { return (a < b) ? a : b; }

float fungsiMin3(float a, float b, float c) {
  return fungsiMin(a, fungsiMin(b, c));
}

float fungsiMax(float a, float b) { return (a > b) ? a : b; }

// ==========================================
// INPUT 1: KELEMBABAN TANAH (%)
// NOTE: nilai sensor kelembaban dari nilai Regresi Linear (ADC) yang sudah
// di-mapping ke 0-100% (ADC kecil = basah, ADC besar = kering). 0% = kering,
// 100% = basah "basah/kering" ditangani di dalam fungsi keanggotaan, bukan di
// mapping ADC -> lihat SensorTask.
// ==========================================
// ==========================================
// FUNGSI KEANGGOTAAN FUZZY (BERDASARKAN Y)
// ==========================================

// 1. Himpunan Kering
float f_Kering(float y) {
  if (y <= 30.0)
    return 1.0;
  if (y > 30.0 && y < 45.0)
    return (45.0 - y) / (45.0 - 30.0);
  return 0.0;
}

// 2. Himpunan Lembab
float f_Lembab(float y) {
  if (y <= 30.0 || y >= 65.0)
    return 0.0;
  if (y > 30.0 && y < 45.0)
    return (y - 30.0) / (45.0 - 30.0);
  if (y >= 45.0 && y <= 50.0)
    return 1.0;
  if (y > 50.0 && y < 65.0)
    return (65.0 - y) / (65.0 - 50.0);
  return 0.0;
}

// 3. Himpunan Basah (65 ke atas = 1.0)
float f_Basah(float y) {
  if (y <= 50.0)
    return 0.0;
  if (y > 50.0 && y < 65.0)
    return (y - 50.0) / (65.0 - 50.0);
  return 1.0;
}

// ==========================================
// INPUT 2: SUHU UDARA (°C)
// ==========================================
float f_Dingin(float x) {
  if (x <= 22)
    return 1.0;
  if (x > 22 && x < 26)
    return (26.0 - x) / (26.0 - 22.0);
  return 0.0;
}

float f_Normal(float x) {
  if (x <= 24 || x >= 32)
    return 0.0;
  if (x > 24 && x <= 28)
    return (x - 24.0) / (28.0 - 24.0);
  if (x > 28 && x < 32)
    return (32.0 - x) / (32.0 - 28.0);
  return 0.0;
}

float f_Panas(float x) {
  if (x <= 30)
    return 0.0;
  if (x > 30 && x < 35)
    return (x - 30.0) / (35.0 - 30.0);
  return 1.0;
}

// ==========================================
// INPUT 3: CAHAYA (Lux) -> Batas Max 65.000 Lux
// ==========================================
float f_Redup(float x) {
  if (x <= 5000)
    return 1.0;
  if (x > 5000 && x < 25000)
    return (25000.0 - x) / (25000.0 - 5000.0);
  return 0.0;
}

// Himpunan Sedang: Segitiga (Transisi antara Redup & Terang)
float f_Sedang(float x) {
  if (x <= 5000 || x >= 65000)
    return 0.0;
  if (x > 5000 && x <= 35000)
    return (x - 5000.0) / (35000.0 - 5000.0);
  if (x > 35000 && x < 65000)
    return (65000.0 - x) / (65000.0 - 35000.0);
  return 0.0;
}

// Himpunan Terang: Trapesium kanan
float f_Terang(float x) {
  if (x <= 45000)
    return 0.0;
  if (x > 45000 && x < 65000)
    return (x - 45000.0) / (65000.0 - 45000.0);
  return 1.0;
}

// ==========================================
// OUTPUT: DURASI PENYIRAMAN (Detik)
// ==========================================
float f_Mati(float z) {
  if (z <= 0)
    return 1.0;
  if (z > 0 && z < 10)
    return (10.0 - z) / (10.0 - 0.0);
  return 0.0;
}

float f_Pendek(float z) {
  if (z <= 10 || z >= 90)
    return 0.0;
  if (z > 10 && z <= 50)
    return (z - 10.0) / (50.0 - 10.0);
  if (z > 50 && z < 90)
    return (90.0 - z) / (90.0 - 50.0);
  return 0.0;
}

float f_SedangOut(float z) {
  if (z <= 70 || z >= 190)
    return 0.0;
  if (z > 70 && z <= 130)
    return (z - 70.0) / (130.0 - 70.0);
  if (z > 130 && z < 190)
    return (190.0 - z) / (190.0 - 130.0);
  return 0.0;
}

float f_Lama(float z) {
  if (z <= 170)
    return 0.0;
  if (z > 170 && z < 222)
    return (z - 170.0) / (222.0 - 170.0);
  if (z >= 222)
    return 1.0;
  return 0.0;
}

// ==========================================
// FUZZY MAMDANI PROSES
// ==========================================
float prosesFuzzyMamdani(float kelembaban, float suhu, float cahaya) {
  float mu_Kering = f_Kering(kelembaban);
  float mu_Lembab = f_Lembab(kelembaban);
  float mu_Basah = f_Basah(kelembaban);

  float mu_Dingin = f_Dingin(suhu);
  float mu_Normal = f_Normal(suhu);
  float mu_Panas = f_Panas(suhu);

  float mu_Redup = f_Redup(cahaya);
  float mu_Sedang = f_Sedang(cahaya);
  float mu_Terang = f_Terang(cahaya);

  float r_Mati = 0.0;
  float r_Pendek = 0.0;
  float r_Sedang = 0.0;
  float r_Lama = 0.0;

  // --- Rule Base ---
  // Urutan argumen fungsiMin3 = (Kelembaban, Suhu, Cahaya)

  // OUTPUT: SEDANG
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Kering, mu_Dingin, mu_Redup));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Kering, mu_Dingin, mu_Sedang));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Kering, mu_Normal, mu_Redup));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Lembab, mu_Normal, mu_Terang));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Lembab, mu_Panas, mu_Redup));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Lembab, mu_Panas, mu_Sedang));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Lembab, mu_Panas, mu_Terang));

  // OUTPUT: LAMA
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Dingin, mu_Terang));
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Normal, mu_Sedang));
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Normal, mu_Terang));
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Panas, mu_Redup));
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Panas, mu_Sedang));
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Panas, mu_Terang));

  // OUTPUT: PENDEK
  r_Pendek = fungsiMax(r_Pendek, fungsiMin3(mu_Lembab, mu_Dingin, mu_Redup));
  r_Pendek = fungsiMax(r_Pendek, fungsiMin3(mu_Lembab, mu_Dingin, mu_Sedang));
  r_Pendek = fungsiMax(r_Pendek, fungsiMin3(mu_Lembab, mu_Dingin, mu_Terang));
  r_Pendek = fungsiMax(r_Pendek, fungsiMin3(mu_Lembab, mu_Normal, mu_Redup));
  r_Pendek = fungsiMax(r_Pendek, fungsiMin3(mu_Lembab, mu_Normal, mu_Sedang));

  // OUTPUT: MATI (tanah BASAH -> pompa tidak menyiram sama sekali)
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Dingin, mu_Redup));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Dingin, mu_Sedang));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Dingin, mu_Terang));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Normal, mu_Redup));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Normal, mu_Sedang));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Normal, mu_Terang));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Panas, mu_Redup));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Panas, mu_Sedang));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Panas, mu_Terang));

  // --- Defuzzifikasi (Metode Centroid) ---
  float pembilang = 0.0;
  float penyebut = 0.0;

  // Evaluasi dari 0 hingga 222 detik
  for (float z = 0; z <= 222; z += 1.0) {
    float m = fungsiMin(f_Mati(z), r_Mati);
    float p = fungsiMin(f_Pendek(z), r_Pendek);
    float s = fungsiMin(f_SedangOut(z), r_Sedang);
    float l = fungsiMin(f_Lama(z), r_Lama);

    float total = fungsiMax(fungsiMax(m, p), fungsiMax(s, l));

    pembilang += z * total;
    penyebut += total;
  }

  if (penyebut == 0.0)
    return 0.0;

  float hasil = pembilang / penyebut;

  // safety guard: jika hasil sangat kecil mendekati nol, bulatkan ke 0.
  if (hasil < 10.0)
    return 0.0;

  return hasil;
}
// ==========================================
// WEBSERVER HANDLERS
// ==========================================

void serveSpiffs(const char *path, const char *contentType) {
  if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (SPIFFS.exists(path)) {
      File file = SPIFFS.open(path, "r");
      server.streamFile(file, contentType);
      file.close();
      xSemaphoreGive(logMutex);
      return;
    }
    xSemaphoreGive(logMutex);
  }
  server.send(404, "text/plain", "File Not Found");
}

void handleRoot() { serveSpiffs("/index.html", "text/html"); }

void handleCSS() { serveSpiffs("/style.css", "text/css"); }

void handleSyncTime() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    if (!error) {
      int y = doc["y"];
      int m = doc["m"];
      int d = doc["d"];
      int h = doc["h"];
      int min = doc["min"];
      int s = doc["s"];
      if (rtcAvailable) {
        rtc.adjust(DateTime(y, m, d, h, min, s));
        server.send(200, "text/plain", "OK");
      } else {
        server.send(500, "text/plain", "RTC Not Available");
      }
      return;
    }
  }
  server.send(400, "text/plain", "Bad Request");
}

void handleData() {
  JsonDocument doc;
  doc["temperature"] = temperature;
  doc["humidity"] = humidity;
  doc["lightLevel"] = lightLevel;
  doc["systemMode"] = systemMode;
  doc["relayMain"] = manualRelayMain;
  doc["wifiConnected"] = (WiFi.status() == WL_CONNECTED);

  if (rtcAvailable) {
    DateTime now = rtc.now();
    char rtcStr[20];
    sprintf(rtcStr, "%04d/%02d/%02d %02d:%02d:%02d", now.year(), now.month(),
            now.day(), now.hour(), now.minute(), now.second());
    doc["rtcTime"] = rtcStr;
  } else {
    doc["rtcTime"] = "RTC Error";
  }

  JsonArray arr = doc["zona"].to<JsonArray>();
  if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (int i = 0; i < 5; i++) {
      JsonObject z = arr.add<JsonObject>();
      z["id"] = i + 1;
      z["soil"] = zona[i].soil;
      z["mode"] = zona[i].mode;
      z["duration"] = zona[i].duration;
      z["relayState"] = manualRelayState[i];
    }
    xSemaphoreGive(zonaMutex);
  }

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

// handle untuk mengubah mode system biar tidak tabrakan sama handle relay dan
// fuzzy
void handleOverride() {
  if (server.hasArg("mode")) {
    systemMode = server.arg("mode").toInt();

    // Reset seluruh relay fisik dan timer manual saat berpindah mode (Auto,
    // Manual, Schedule)
    manualRelayMain = false;
    digitalWrite(RELAY_MAIN, RELAY_OFF);
    // digitalWrite(RELAY_POMPA, RELAY_OFF);

    for (int i = 0; i < 5; i++) {
      manualRelayState[i] = false;
      manualTimerOff[i] = 0;
      digitalWrite(relayPin[i], RELAY_OFF);
    }

    // Reset status mode & durasi pada setiap zona agar aktifitas fuzzy/manual
    // langsung berhenti
    if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      for (int i = 0; i < 5; i++) {
        zona[i].mode = 0;
        zona[i].duration = 0;
      }
      xSemaphoreGive(zonaMutex);
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleRelay() {
  if (systemMode == 1 && server.hasArg("relay")) {
    String relay = server.arg("relay");
    bool isTimer = (cfgManualMode == 1);
    uint32_t durMs = cfgManualDur * 1000;

    if (relay == "main") {
      manualRelayMain = !manualRelayMain;
      digitalWrite(RELAY_MAIN, manualRelayMain ? RELAY_ON : RELAY_OFF);
      // digitalWrite(RELAY_POMPA, manualRelayMain ? RELAY_ON : RELAY_OFF);
    } else {
      int idx = relay.toInt();
      if (idx >= 0 && idx < 5) {
        if (!manualRelayState[idx]) {
          // Cek maksimal 3 sub aktif
          int activeCount = 0;
          for (int i = 0; i < 5; i++) {
            if (manualRelayState[i])
              activeCount++;
          }
          if (activeCount >= 3) {
            server.send(400, "text/plain",
                        "MAKSIMAL 3 ZONA AKTIF | GAKUAT AKI NYA");
            return;
          }

          // Turn ON
          manualRelayState[idx] = true;
          manualStartTime[idx] = millis();
          if (isTimer) {
            manualTimerOff[idx] = millis() + durMs;
            zona[idx].duration = cfgManualDur;
          }
          digitalWrite(relayPin[idx], RELAY_ON);
        } else {
          // Turn OFF
          manualRelayState[idx] = false;
          digitalWrite(relayPin[idx], RELAY_OFF);
          manualTimerOff[idx] = 0;

          // Log final duration
          zona[idx].duration = (millis() - manualStartTime[idx]) / 1000.0;
          addLogEntry();
          zona[idx].duration = 0;
        }
        updateMainRelayState();
      }
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleConfigGet() {
  JsonDocument doc;
  doc["sensorInterval"] = cfgSensorInterval;
  doc["manualMode"] = cfgManualMode;
  doc["manualDur"] = cfgManualDur;
  doc["logInterval"] = cfgLogInterval;
  doc["cooldown"] = cfgCooldown;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleConfigPost() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    if (!error) {
      if (doc["sensorInterval"].is<uint32_t>())
        cfgSensorInterval = doc["sensorInterval"];
      if (doc["manualMode"].is<int>())
        cfgManualMode = doc["manualMode"];
      if (doc["manualDur"].is<uint32_t>())
        cfgManualDur = doc["manualDur"];
      if (doc["logInterval"].is<uint32_t>())
        cfgLogInterval = doc["logInterval"];
      if (doc["cooldown"].is<uint32_t>())
        cfgCooldown = doc["cooldown"];
      server.send(200, "text/plain", "OK");
      return;
    }
  }
  server.send(400, "text/plain", "Bad Request");
}

void saveSchedule() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < 5; i++) {
    JsonObject z = arr.add<JsonObject>();
    JsonArray sArr = z["schedules"].to<JsonArray>();
    for (int j = 0; j < MAX_SCHEDULES; j++) {
      JsonObject sObj = sArr.add<JsonObject>();
      sObj["active"] = zoneSchedules[i][j].active;
      sObj["hour"] = zoneSchedules[i][j].hour;
      sObj["minute"] = zoneSchedules[i][j].minute;
      sObj["duration"] = zoneSchedules[i][j].duration;
    }
  }
  File file = SPIFFS.open("/schedule.json", "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
  }
}

void loadSchedule() {
  if (SPIFFS.exists("/schedule.json")) {
    File file = SPIFFS.open("/schedule.json", "r");
    if (file) {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, file);
      if (!error) {
        JsonArray arr = doc.as<JsonArray>();
        for (int i = 0; i < 5 && i < arr.size(); i++) {
          JsonObject z = arr[i];
          JsonArray sArr = z["schedules"];
          for (int j = 0; j < MAX_SCHEDULES && j < sArr.size(); j++) {
            JsonObject sObj = sArr[j];
            zoneSchedules[i][j].active = sObj["active"] | false;
            zoneSchedules[i][j].hour = sObj["hour"] | 0;
            zoneSchedules[i][j].minute = sObj["minute"] | 0;
            zoneSchedules[i][j].duration = sObj["duration"] | 10;
          }
        }
      }
      file.close();
    }
  }
}

void handleScheduleGet() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < 5; i++) {
    JsonObject z = arr.add<JsonObject>();
    z["id"] = i + 1;
    JsonArray sArr = z["schedules"].to<JsonArray>();
    for (int j = 0; j < MAX_SCHEDULES; j++) {
      JsonObject sObj = sArr.add<JsonObject>();
      sObj["active"] = zoneSchedules[i][j].active;
      sObj["hour"] = zoneSchedules[i][j].hour;
      sObj["minute"] = zoneSchedules[i][j].minute;
      sObj["duration"] = zoneSchedules[i][j].duration;
    }
  }
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSchedulePost() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    if (!error && doc.is<JsonArray>()) {
      JsonArray arr = doc.as<JsonArray>();
      for (int i = 0; i < 5 && i < arr.size(); i++) {
        JsonObject z = arr[i];
        JsonArray sArr = z["schedules"];
        for (int j = 0; j < MAX_SCHEDULES && j < sArr.size(); j++) {
          JsonObject sObj = sArr[j];
          zoneSchedules[i][j].active = sObj["active"] | false;
          zoneSchedules[i][j].hour = sObj["hour"] | 0;
          zoneSchedules[i][j].minute = sObj["minute"] | 0;
          zoneSchedules[i][j].duration = sObj["duration"] | 10;
        }
      }
      saveSchedule();
      server.send(200, "text/plain", "OK");
      return;
    }
  }
  server.send(400, "text/plain", "Bad Request");
}

void handleLogGet() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  if (logBuffer && logMutex &&
      xSemaphoreTake(logMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    int startIdx = (logCount < MAX_LOG_ENTRIES) ? 0 : logHead;
    for (int i = 0; i < logCount; i++) {
      int idx = (startIdx + i) % MAX_LOG_ENTRIES;
      JsonObject obj = arr.add<JsonObject>();
      obj["timestamp"] = logBuffer[idx].timestamp;
      obj["temp"] = logBuffer[idx].temp;
      obj["hum"] = logBuffer[idx].hum;
      obj["light"] = logBuffer[idx].light;

      JsonArray sBArr = obj["soilBefore"].to<JsonArray>();
      JsonArray sAArr = obj["soilAfter"].to<JsonArray>();
      JsonArray mArr = obj["mode"].to<JsonArray>();
      JsonArray dArr = obj["duration"].to<JsonArray>();
      for (int j = 0; j < 5; j++) {
        sBArr.add(logBuffer[idx].soilBefore[j]);
        sAArr.add(logBuffer[idx].soilAfter[j]);
        mArr.add(logBuffer[idx].mode[j]);
        dArr.add(logBuffer[idx].duration[j]);
      }
      obj["sysMode"] = logBuffer[idx].sysMode;
    }
    xSemaphoreGive(logMutex);
  }

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleLogReset() {
  if (logMutex && xSemaphoreTake(logMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    logHead = 0;
    logCount = 0;
    xSemaphoreGive(logMutex);
  }
  if (SPIFFS.exists(LOG_FILE)) {
    SPIFFS.remove(LOG_FILE);
  }
  server.send(200, "text/plain", "OK");
}

// ==========================================
// SENSOR TASK
// ==========================================

void SensorTask(void *pv) {
  // Tunggu 500ms agar semua task & handle lain selesai dibuat oleh setup()
  vTaskDelay(pdMS_TO_TICKS(500));

  while (1) {
    if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      for (int i = 0; i < 5; i++) {
        int adc = analogRead(soilPin[i]);

        // 1. Regresi Linier (ADC ke Moisture Analyzer Y)
        float a = 41.7453982;
        float b = -0.012665492;
        float Y = a + (b * adc);

        // 2. Mapping nilai Y (10 - 35) ke persentase (0 - 100%)
        // Menggunakan rumus proporsi: (Y - Y_min) * (100 / (Y_max - Y_min))
        float mappedSoil = (Y - 10.0) * (100.0 / 25.0);

        // 3. Batasi nilai agar tetap di rentang 0 - 100
        zona[i].soil = constrain(mappedSoil, 0.0f, 100.0f);

        // Simpan nilai soilBefore jika zona sedang tidak aktif
        if (zona[i].mode == 0 && !manualRelayState[i]) {
          zona[i].soilBefore = zona[i].soil;
        }
      }
      xSemaphoreGive(zonaMutex);
    }

    temperature = dht.readTemperature();
    humidity = dht.readHumidity();
    lightLevel = lightMeter.readLightLevel();

    if (isnan(temperature))
      temperature = 00.0;
    if (isnan(humidity))
      humidity = 00.0;

    static uint32_t lastHourlyLog = 0;
    uint32_t currentSec = getCurrentTimeSeconds();

    // Log rutin (interval default 3600 detik)
    if (lastHourlyLog == 0 || (currentSec - lastHourlyLog >= cfgLogInterval)) {
      addLogEntry();
      lastHourlyLog = currentSec;
    }

    if (systemMode == 0 && activeIrrigations == 0) {
      xTaskNotifyGive(fuzzyHandle);
    }

    vTaskDelay(pdMS_TO_TICKS(cfgSensorInterval * 1000));
  }
}

// ==========================================
// FUZZY TASK
// ==========================================

void FuzzyTask(void *pv) {
  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (systemMode != 0) {
      continue; // Skip fuzzy control if not in Auto mode
    }

    if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      uint32_t currentTime = getCurrentTimeSeconds();

      for (int i = 0; i < 5; i++) {
        float durasi =
            prosesFuzzyMamdani(zona[i].soil, temperature, lightLevel);

        // Cek cooldown
        if (lastWateringTime[i] != 0 &&
            (currentTime - lastWateringTime[i]) < cfgCooldown) {
          durasi = 0;
        }

        zona[i].duration = durasi;
        if (durasi > 0) {
          zona[i].mode = 4; // 4 = Queued
          xTaskNotifyGive(irrigationHandle[i]);
        } else {
          zona[i].mode = 0;
          zona[i].duration = 0;
        }
      }
      xSemaphoreGive(zonaMutex);
    }
  }
}

// ==========================================
// IRRIGATION TASK
// ==========================================

void IrrigationTask(void *pv) {
  int id = (int)(uintptr_t)pv;

  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (systemMode == 1) {
      continue;
    }

    // Tunggu giliran jika sudah ada 3 sub yang aktif
    xSemaphoreTake(maxConcurrentIrrigationSemaphore, portMAX_DELAY);

    // Cek ulang mode jika user mengubah ke manual saat task sedang di antrian
    // (queue)
    if (systemMode == 1) {
      if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        zona[id].mode = 0;
        zona[id].duration = 0;
        xSemaphoreGive(zonaMutex);
      }
      xSemaphoreGive(maxConcurrentIrrigationSemaphore);
      continue;
    }

    // Set mode aktual (Active) setelah berhasil keluar dari antrian
    if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (systemMode == 2) {
        zona[id].mode = 3; // Schedule mode
      } else {
        if (zona[id].duration <= 20)
          zona[id].mode = 1;
        else if (zona[id].duration <= 40)
          zona[id].mode = 2;
        else
          zona[id].mode = 3;
      }
      xSemaphoreGive(zonaMutex);
    }

    portENTER_CRITICAL(&mux);
    activeIrrigations++;
    portEXIT_CRITICAL(&mux);

    digitalWrite(relayPin[id], RELAY_ON);
    digitalWrite(RELAY_MAIN, RELAY_ON);
    // digitalWrite(RELAY_POMPA, RELAY_ON);

    vTaskDelay(pdMS_TO_TICKS((uint32_t)(zona[id].duration * 1000)));

    if (systemMode != 1) {
      digitalWrite(relayPin[id], RELAY_OFF);
    }

    // Matikan relay utama segera setelah penyiraman selesai
    portENTER_CRITICAL(&mux);
    activeIrrigations--;
    if (activeIrrigations == 0 && systemMode != 1) {
      digitalWrite(RELAY_MAIN, RELAY_OFF);
      // digitalWrite(RELAY_POMPA, RELAY_OFF);
    }
    portEXIT_CRITICAL(&mux);

    // Kasih jeda agar tanah bisa melakukan penyerapan air
    // sehingga pembacaan mois sesudah (soilAfter) lebih akurat.
    // Jeda diset 15 detik (15000 ms), bisa disesuaikan kebutuhan.
    vTaskDelay(pdMS_TO_TICKS(15000));

    addLogEntry();

    // Catat waktu selesai penyiraman untuk cooldown
    lastWateringTime[id] = getCurrentTimeSeconds();

    // Reset mode so UI shows IDLE when no zone is actively irrigating
    if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      zona[id].mode = 0;
      zona[id].duration = 0;
      xSemaphoreGive(zonaMutex);
    }

    xSemaphoreGive(maxConcurrentIrrigationSemaphore);
  }
}

// ==========================================
// WEB TASK
// ==========================================

void WebTask(void *pv) {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/style.css", HTTP_GET, handleCSS);
  server.on("/api/data", HTTP_GET, handleData);
  server.on("/api/override", HTTP_POST, handleOverride);
  server.on("/api/relay", HTTP_POST, handleRelay);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/schedule", HTTP_GET, handleScheduleGet);
  server.on("/api/schedule", HTTP_POST, handleSchedulePost);
  server.on("/api/log", HTTP_GET, handleLogGet);
  server.on("/api/log/reset", HTTP_POST, handleLogReset);
  server.on("/api/sync-time", HTTP_POST, handleSyncTime);

  server.begin();

  while (1) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ==========================================
// MANUAL CONTROL TASK
// ==========================================

void ManualControlTask(void *pv) {
  while (1) {
    if (systemMode == 1 && cfgManualMode == 1) {
      uint32_t now = millis();
      bool changed = false;

      for (int i = 0; i < 5; i++) {
        if (manualRelayState[i] && manualTimerOff[i] > 0 &&
            now >= manualTimerOff[i]) {
          manualRelayState[i] = false;
          digitalWrite(relayPin[i], RELAY_OFF);
          manualTimerOff[i] = 0;

          zona[i].duration = (now - manualStartTime[i]) / 1000.0;
          addLogEntry();
          zona[i].duration = 0;

          changed = true;
        }
      }

      if (changed) {
        updateMainRelayState();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ==========================================
// SCHEDULE TASK
// ==========================================

void ScheduleTask(void *pv) {
  while (1) {
    if (systemMode == 2) {
      DateTime now = rtc.now();
      int currentHour = now.hour();
      int currentMinute = now.minute();

      // Cek setiap menit saja
      if (lastScheduledMinute != currentMinute) {
        lastScheduledMinute = currentMinute;

        if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
          for (int i = 0; i < 5; i++) {
            for (int j = 0; j < MAX_SCHEDULES; j++) {
              if (zoneSchedules[i][j].active &&
                  zoneSchedules[i][j].hour == currentHour &&
                  zoneSchedules[i][j].minute == currentMinute) {
                // Cek jika soil moisture <= 60 (tidak terlalu basah)
                if (zona[i].soil <= 60.0) {
                  zona[i].duration = zoneSchedules[i][j].duration;
                  // Masukkan ke dalam antrian terlebih dahulu (mode 4)
                  zona[i].mode = 4;
                  xTaskNotifyGive(irrigationHandle[i]);
                }
                break; // Jika salah satu jadwal cocok, tidak usah cek jadwal
                       // lain untuk menit ini
              }
            }
          }
          xSemaphoreGive(zonaMutex);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ==========================================
// CLOUD SYNC HELPERS
// ==========================================

String buildTelemetryJson(bool isOffline) {
  JsonDocument doc;
  doc["device_key"] = deviceKey;
  doc["temperature"] = temperature;
  doc["humidity"] = humidity;
  doc["light_level"] = (int)lightLevel;
  doc["system_mode"] = systemMode;
  doc["is_offline_data"] = isOffline;

  if (rtcAvailable) {
    DateTime now = rtc.now();
    char buf[25];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d", now.year(),
             now.month(), now.day(), now.hour(), now.minute(), now.second());
    doc["recorded_at"] = buf;
  }

  JsonArray zones = doc["zones"].to<JsonArray>();
  if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (int i = 0; i < 5; i++) {
      JsonObject z = zones.add<JsonObject>();
      z["zone_id"] = i + 1;
      z["soil_moisture"] = (int)zona[i].soil;
      z["zone_mode"] = zona[i].mode;
      z["duration"] = (int)zona[i].duration;
      z["relay_status"] = manualRelayState[i];
    }
    xSemaphoreGive(zonaMutex);
  }

  String result;
  serializeJson(doc, result);
  return result;
}

void saveToOfflineBuffer(const String &jsonPayload) {
  if (!logMutex || xSemaphoreTake(logMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return;
  }
  int count = 0;
  File rf = SPIFFS.open(OFFLINE_QUEUE_FILE, "r");
  if (rf) {
    while (rf.available()) {
      if (rf.read() == '\n')
        count++;
    }
    rf.close();
  }
  if (count >= MAX_OFFLINE_ENTRIES) {
    Serial.println("[Offline] Buffer penuh, entri baru diabaikan.");
    xSemaphoreGive(logMutex);
    return;
  }
  File wf = SPIFFS.open(OFFLINE_QUEUE_FILE, "a");
  if (wf) {
    wf.println(jsonPayload);
    wf.close();
    Serial.printf("[Offline] Entri #%d disimpan ke flash.\n", count + 1);
  }
  xSemaphoreGive(logMutex);
}

void flushOfflineBuffer() {
  if (!logMutex || xSemaphoreTake(logMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return;
  }
  if (!SPIFFS.exists(OFFLINE_QUEUE_FILE)) {
    xSemaphoreGive(logMutex);
    return;
  }
  File file = SPIFFS.open(OFFLINE_QUEUE_FILE, "r");
  if (!file || file.size() == 0) {
    if (file)
      file.close();
    SPIFFS.remove(OFFLINE_QUEUE_FILE);
    xSemaphoreGive(logMutex);
    return;
  }
  Serial.println("[Offline] Mengunggah buffer offline ke server...");

  JsonDocument batchDoc;
  batchDoc["device_key"] = deviceKey;
  JsonArray batchArr = batchDoc["batch"].to<JsonArray>();
  int count = 0;

  while (file.available() && count < MAX_OFFLINE_ENTRIES) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() < 5)
      continue;
    JsonDocument entryDoc;
    if (!deserializeJson(entryDoc, line)) {
      batchArr.add(entryDoc);
      count++;
    }
  }
  file.close();
  if (count == 0) {
    SPIFFS.remove(OFFLINE_QUEUE_FILE);
    xSemaphoreGive(logMutex);
    return;
  }
  xSemaphoreGive(logMutex); // Lepaskan sementara untuk request HTTP

  String batchPayload;
  serializeJson(batchDoc, batchPayload);

  HTTPClient http;
  http.begin(cloudBatchUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Token", deviceKey);
  http.setTimeout(10000);
  int code = http.POST(batchPayload);

  // Re-acquire mutex untuk menghapus file
  if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    if (code == HTTP_CODE_OK || code == HTTP_CODE_CREATED) {
      Serial.printf("[Offline] Batch sync OK: %d entri terkirim.\n", count);
      SPIFFS.remove(OFFLINE_QUEUE_FILE);
    } else {
      Serial.printf("[Offline] Batch sync gagal: HTTP %d\n", code);
    }
    xSemaphoreGive(logMutex);
  }
  http.end();
}

// ==========================================
// CLOUD SYNC TASK
// ==========================================

void CloudSyncTask(void *pv) {
  // Tunggu 3 detik agar SoftAP stabil dulu sebelum konek ke router
  // Ini mencegah channel-switching mengganggu akses 192.168.4.1 dari awal boot
  vTaskDelay(pdMS_TO_TICKS(3000));

  WiFi.begin(staSsid, staPassword);
  Serial.println("[WiFi] Mencoba konek ke router internet...");

  uint32_t lastReconnectAttempt = millis();

  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {

      // 1. Flush buffer offline jika ada data yang belum terkirim
      if (SPIFFS.exists(OFFLINE_QUEUE_FILE)) {
        flushOfflineBuffer();
      }

      // 2. Kirim data sensor live ke Laravel
      String payload = buildTelemetryJson(false);
      HTTPClient http;
      http.begin(cloudUrl);
      http.addHeader("Content-Type", "application/json");
      http.addHeader("X-Device-Token", deviceKey);
      http.setTimeout(8000);

      int code = http.POST(payload);
      if (code == HTTP_CODE_OK || code == HTTP_CODE_CREATED) {
        String resp = http.getString();
        Serial.printf("[CloudSync] Telemetry OK (%d)\n", code);

        // Update systemMode dari perintah Web Dashboard Laravel
        JsonDocument resDoc;
        if (!deserializeJson(resDoc, resp)) {
          JsonObject cmd = resDoc["device_command"].as<JsonObject>();
          if (!cmd.isNull()) {

            // 1. Update systemMode
            int newMode = cmd["system_mode"] | systemMode;
            if (newMode != systemMode) {
              systemMode = newMode;
              Serial.printf("[CloudSync] systemMode -> %d\n", systemMode);
            }

            // 2. Update manual mode config
            int newManualType = cmd["manual_mode_type"] | (int)cfgManualMode;
            uint32_t newManualDur = cmd["manual_duration"] | (int)cfgManualDur;
            if ((int)cfgManualMode != newManualType) {
              cfgManualMode = newManualType;
              Serial.printf("[CloudSync] manualModeType -> %d\n",
                            cfgManualMode);
            }
            if (cfgManualDur != newManualDur) {
              cfgManualDur = newManualDur;
              Serial.printf("[CloudSync] manualDuration -> %u detik\n",
                            cfgManualDur);
            }

            // 3. Update jadwal per zona dari schedules_json
            if (!cmd["schedules"].isNull()) {
              JsonArray schArr = cmd["schedules"].as<JsonArray>();
              if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                for (JsonObject zoneObj : schArr) {
                  int zoneIdx = (int)zoneObj["zone"] - 1; // 1-based ke 0-based
                  if (zoneIdx < 0 || zoneIdx >= 5)
                    continue;

                  JsonArray items = zoneObj["items"].as<JsonArray>();
                  int si = 0;
                  for (JsonObject item : items) {
                    if (si >= MAX_SCHEDULES)
                      break;
                    zoneSchedules[zoneIdx][si].active = item["active"] | false;
                    zoneSchedules[zoneIdx][si].hour =
                        (uint8_t)((int)item["hour"] | 0);
                    zoneSchedules[zoneIdx][si].minute =
                        (uint8_t)((int)item["minute"] | 0);
                    zoneSchedules[zoneIdx][si].duration =
                        (uint32_t)((int)item["duration"] | 0);
                    si++;
                  }
                  // Kosongkan sisa slot
                  for (; si < MAX_SCHEDULES; si++) {
                    zoneSchedules[zoneIdx][si] = {false, 0, 0, 0};
                  }
                }
                xSemaphoreGive(zonaMutex);
                Serial.println("[CloudSync] Jadwal zona diperbarui dari web.");
              }
            }
          }
        }
      } else {
        Serial.printf("[CloudSync] Gagal HTTP %d. Data disimpan ke buffer.\n",
                      code);
        saveToOfflineBuffer(payload);
      }
      http.end();

    } else {
      // WiFi tidak tersambung – simpan ke buffer offline di flash
      Serial.println(
          "[CloudSync] Wi-Fi terputus. Menyimpan ke buffer offline.");
      String payload = buildTelemetryJson(true);
      saveToOfflineBuffer(payload);

      // Coba hubungkan kembali setiap 30 detik
      if (millis() - lastReconnectAttempt > 30000) {
        Serial.println("[WiFi] Mencoba menyambung kembali ke router...");
        WiFi.disconnect();
        WiFi.begin(staSsid, staPassword);
        lastReconnectAttempt = millis();
      }
    }

    // Interval sinkronisasi: setiap 5 detik
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// ==========================================
// SETUP
// ==========================================

void setup() {
  Serial.begin(115200);
  delay(1000); // Beri jeda 1 detik agar Serial Monitor USB CDC siap
  Serial.println("\n==========================================");
  Serial.println("   SMART NURSERY BOOTING (ESP32-S3)       ");
  Serial.println("==========================================");

  // Set I2C Timeout agar tidak hang jika sensor terlepas/salah pin
  Wire.setTimeOut(1000);
  Wire.begin();

  Serial.println("[BOOT] Mounting SPIFFS...");
  if (!SPIFFS.begin(false)) {
    Serial.println("[BOOT] SPIFFS mount awal gagal, mencoba format...");
    if (!SPIFFS.begin(true)) {
      Serial.println("[BOOT] Error mounting SPIFFS!");
    } else {
      Serial.println("[BOOT] SPIFFS berhasil diformat dan dipasang!");
    }
  } else {
    Serial.println("[BOOT] SPIFFS terpasang sukses!");
  }

  pinMode(RELAY_MAIN, OUTPUT);
  digitalWrite(RELAY_MAIN, RELAY_OFF);
  for (int i = 0; i < 5; i++) {
    pinMode(relayPin[i], OUTPUT);
    digitalWrite(relayPin[i], RELAY_OFF);
  }

  Serial.println("[BOOT] Memulai Sensor (DHT22, BH1750, RTC)...");
  dht.begin();

  if (!lightMeter.begin()) {
    Serial.println("[WARN] Gagal menemukan sensor BH1750!");
  }

  if (!rtc.begin()) {
    rtcAvailable = false;
    Serial.println("[WARN] Gagal menemukan RTC DS3231! Menggunakan millis() "
                   "untuk cooldown");
  } else {
    rtcAvailable = true;
    if (rtc.lostPower()) {
      Serial.println(
          "[RTC] RTC kehilangan daya, mengatur waktu ke waktu kompilasi...");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }

  Serial.println("[BOOT] Memulai WiFi AP & STA...");
  // Mode WIFI_AP_STA: ESP tetap sebagai Access Point lokal
  // DAN sekaligus terhubung ke Router Internet untuk Cloud Sync
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid, apPassword);

  Serial.print("[WiFi] Access Point IP: ");
  Serial.println(WiFi.softAPIP());

  // Konek ke Wi-Fi Router ditangani oleh CloudSyncTask setelah AP stabil
  Serial.println("[WiFi] AP dimulai, CloudSyncTask akan konek ke router...");

  zonaMutex = xSemaphoreCreateMutex();
  maxConcurrentIrrigationSemaphore = xSemaphoreCreateCounting(3, 3);
  logMutex = xSemaphoreCreateMutex();

  // Alokasikan logBuffer di heap (bukan static RAM)
  logBuffer = new LogEntry[MAX_LOG_ENTRIES];
  if (!logBuffer) {
    Serial.println("FATAL: Gagal alokasi logBuffer di heap!");
  }

  xTaskCreate(SensorTask, "Sensor", 4096, NULL, 5, NULL);
  xTaskCreate(FuzzyTask, "Fuzzy", 4096, NULL, 4, &fuzzyHandle);

  for (int i = 0; i < 5; i++) {
    xTaskCreate(IrrigationTask, "Irrigation", 4096, (void *)(uintptr_t)i, 3,
                &irrigationHandle[i]);
  }

  // Stack untuk WebTask sedikit lebih besar agar aman menangani HTTP request
  // dan JSON
  xTaskCreate(WebTask, "Web", 8192, NULL, 2, &webHandle);

  // Cloud Sync Task: sinkronisasi data ke Laravel & manage offline buffer
  xTaskCreate(CloudSyncTask, "CloudSync", 12288, NULL, 2, &cloudSyncHandle);

  // Task khusus untuk mode manual (menggantikan void loop)
  xTaskCreate(ManualControlTask, "ManualControl", 4096, NULL, 1,
              &manualControlHandle);

  // Task untuk penjadwalan (stack diperbesar agar lebih aman)
  xTaskCreate(ScheduleTask, "Schedule", 4096, NULL, 1, &scheduleHandle);

  // Task untuk pemantauan memori setiap 2 detik
  xTaskCreate(MemoryTask, "Memory", 2048, NULL, 1, NULL);

  // Load jadwal dari SPIFFS
  loadSchedule();

  // Load log persisten dari SPIFFS
  loadLogFile();

  Serial.println("Smart Nursery Siap");
  printMemoryInfo();
}

void printMemoryInfo() {
  Serial.println("\n========== MEMORY ==========");

  Serial.printf("Free Heap      : %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Largest Block  : %u bytes\n", ESP.getMaxAllocHeap());
  Serial.printf("Min Free Heap  : %u bytes\n",
                heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));

  if (fuzzyHandle)
    Serial.printf("Fuzzy Stack    : %u words (%u bytes)\n",
                  uxTaskGetStackHighWaterMark(fuzzyHandle),
                  uxTaskGetStackHighWaterMark(fuzzyHandle) * 4);

  if (webHandle)
    Serial.printf("Web Stack      : %u words (%u bytes)\n",
                  uxTaskGetStackHighWaterMark(webHandle),
                  uxTaskGetStackHighWaterMark(webHandle) * 4);

  if (cloudSyncHandle)
    Serial.printf("CloudSync Stack: %u words (%u bytes)\n",
                  uxTaskGetStackHighWaterMark(cloudSyncHandle),
                  uxTaskGetStackHighWaterMark(cloudSyncHandle) * 4);

  if (manualControlHandle)
    Serial.printf("Manual Stack   : %u words (%u bytes)\n",
                  uxTaskGetStackHighWaterMark(manualControlHandle),
                  uxTaskGetStackHighWaterMark(manualControlHandle) * 4);

  if (scheduleHandle)
    Serial.printf("Schedule Stack : %u words (%u bytes)\n",
                  uxTaskGetStackHighWaterMark(scheduleHandle),
                  uxTaskGetStackHighWaterMark(scheduleHandle) * 4);

  for (int i = 0; i < 5; i++) {
    if (irrigationHandle[i]) {
      Serial.printf("Irrigation %d  : %u words (%u bytes)\n", i + 1,
                    uxTaskGetStackHighWaterMark(irrigationHandle[i]),
                    uxTaskGetStackHighWaterMark(irrigationHandle[i]) * 4);
    }
  }

  Serial.println("============================");
}

void MemoryTask(void *pv) {
  while (1) {
    printMemoryInfo();
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void loop() { vTaskDelete(NULL); }
