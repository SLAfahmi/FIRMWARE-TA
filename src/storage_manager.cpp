#include "storage_manager.h"
#include "config.h"
#include "types.h"
#include "shared_state.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>

bool initStorage() {
  Serial.println("[BOOT] Mounting LittleFS...");
  if (!LittleFS.begin(false)) {
    Serial.println("[BOOT] LittleFS mount awal gagal, mencoba format...");
    if (!LittleFS.begin(true)) {
      Serial.println("[BOOT] Error mounting LittleFS!");
      return false;
    }
    Serial.println("[BOOT] LittleFS berhasil diformat dan dipasang!");
  } else {
    Serial.println("[BOOT] LittleFS terpasang sukses!");
  }
  return true;
}

bool saveLogFile() {
  if (!logBuffer)
    return false;
  File file = LittleFS.open(LOG_FILE, "w");
  if (!file)
    return false;

  for (int i = 0; i < logCount; i++) {
    int idx = (logHead + MAX_LOG_ENTRIES - logCount + i) % MAX_LOG_ENTRIES;
    file.printf("%s,%.1f,%.1f,%.1f", logBuffer[idx].timestamp,
                logBuffer[idx].temp, logBuffer[idx].hum, logBuffer[idx].light);
    for (int j = 0; j < NUM_ZONES; j++) {
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
  if (!LittleFS.exists(LOG_FILE))
    return false;

  File file = LittleFS.open(LOG_FILE, "r");
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

    for (int j = 0; j < NUM_ZONES; j++) {
      token = strtok(NULL, ",");
      entry.soilBefore[j] = token ? atof(token) : 0.0f;
      token = strtok(NULL, ",");
      entry.soilAfter[j] = token ? atof(token) : 0.0f;
      token = strtok(NULL, ",");
      entry.mode[j] = token ? atoi(token) : 0;
      token = strtok(NULL, ",");
      entry.duration[j] = token ? atof(token) : 0.0f;
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

void saveSchedule() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < NUM_ZONES; i++) {
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
  File file = LittleFS.open(SCHEDULE_FILE, "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
  }
}

void loadSchedule() {
  if (LittleFS.exists(SCHEDULE_FILE)) {
    File file = LittleFS.open(SCHEDULE_FILE, "r");
    if (file) {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, file);
      if (!error) {
        JsonArray arr = doc.as<JsonArray>();
        for (int i = 0; i < NUM_ZONES && i < arr.size(); i++) {
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

void saveToOfflineBuffer(const String &jsonPayload) {
  if (!logMutex || xSemaphoreTake(logMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return;
  }
  int count = 0;
  File rf = LittleFS.open(OFFLINE_QUEUE_FILE, "r");
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
  File wf = LittleFS.open(OFFLINE_QUEUE_FILE, "a");
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
  if (!LittleFS.exists(OFFLINE_QUEUE_FILE)) {
    xSemaphoreGive(logMutex);
    return;
  }
  File file = LittleFS.open(OFFLINE_QUEUE_FILE, "r");
  if (!file || file.size() == 0) {
    if (file)
      file.close();
    LittleFS.remove(OFFLINE_QUEUE_FILE);
    xSemaphoreGive(logMutex);
    return;
  }
  Serial.println("[Offline] Mengunggah buffer offline ke server...");

  JsonDocument batchDoc;
  batchDoc["device_key"] = DEVICE_KEY;
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
    LittleFS.remove(OFFLINE_QUEUE_FILE);
    xSemaphoreGive(logMutex);
    return;
  }
  xSemaphoreGive(logMutex); // Lepaskan mutex sebelum HTTP blocking

  String batchPayload;
  serializeJson(batchDoc, batchPayload);

  HTTPClient http;
  http.begin(CLOUD_BATCH_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Token", DEVICE_KEY);
  http.setTimeout(10000);
  int code = http.POST(batchPayload);

  if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    if (code == HTTP_CODE_OK || code == HTTP_CODE_CREATED) {
      Serial.printf("[Offline] Batch sync OK: %d entri terkirim.\n", count);
      LittleFS.remove(OFFLINE_QUEUE_FILE);
    } else {
      Serial.printf("[Offline] Batch sync gagal: HTTP %d\n", code);
    }
    xSemaphoreGive(logMutex);
  }
  http.end();
}
