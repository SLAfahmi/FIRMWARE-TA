#include "tasks/web_task.h"
#include "config.h"
#include "shared_state.h"
#include "storage_manager.h"
#include "types.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>


static void serveLittleFS(const char *path, const char *contentType) {
  if (LittleFS.exists(path)) {
    File file = LittleFS.open(path, "r");
    if (file) {
      server.streamFile(file, contentType);
      file.close();
      return;
    }
  }
  server.send(404, "text/plain", "File Not Found");
}

static void handleRoot() { serveLittleFS("/index.html", "text/html"); }
static void handleCSS() { serveLittleFS("/style.css", "text/css"); }

static void handleSyncTime() {
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

static void handleData() {
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
    snprintf(rtcStr, sizeof(rtcStr), "%04d/%02d/%02d %02d:%02d:%02d",
             now.year(), now.month(), now.day(), now.hour(), now.minute(),
             now.second());
    doc["rtcTime"] = rtcStr;
  } else {
    doc["rtcTime"] = "RTC Error";
  }

  JsonArray arr = doc["zona"].to<JsonArray>();
  if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (int i = 0; i < NUM_ZONES; i++) {
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

static void handleOverride() {
  if (server.hasArg("mode")) {
    systemMode = server.arg("mode").toInt();

    manualRelayMain = false;
    digitalWrite(RELAY_MAIN, RELAY_OFF);

    for (int i = 0; i < NUM_ZONES; i++) {
      manualRelayState[i] = false;
      manualTimerOff[i] = 0;
      digitalWrite(RELAY_PIN[i], RELAY_OFF);
    }

    if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      for (int i = 0; i < NUM_ZONES; i++) {
        zona[i].mode = 0;
        zona[i].duration = 0.0f;
      }
      xSemaphoreGive(zonaMutex);
    }
  }
  server.send(200, "text/plain", "OK");
}

static void handleRelay() {
  if (systemMode == 1 && server.hasArg("relay")) {
    String relay = server.arg("relay");
    bool isTimer = (cfgManualMode == 1);
    uint32_t durMs = cfgManualDur * 1000;

    if (relay == "main") {
      manualRelayMain = !manualRelayMain;
      digitalWrite(RELAY_MAIN, manualRelayMain ? RELAY_ON : RELAY_OFF);
    } else {
      int idx = relay.toInt();
      if (idx >= 0 && idx < NUM_ZONES) {
        if (!manualRelayState[idx]) {
          // Cek batas maksimum zona aktif bersamaan
          int activeCount = 0;
          for (int i = 0; i < NUM_ZONES; i++) {
            if (manualRelayState[i])
              activeCount++;
          }
          if (activeCount >= NUM_ZONES) {
            server.send(400, "text/plain", "MAKSIMAL SEMUA ZONA AKTIF");
            return;
          }

          manualRelayState[idx] = true;
          manualStartTime[idx] = millis();
          if (isTimer) {
            manualTimerOff[idx] = millis() + durMs;
            zona[idx].duration = cfgManualDur;
          }
          digitalWrite(RELAY_PIN[idx], RELAY_ON);
        } else {
          manualRelayState[idx] = false;
          digitalWrite(RELAY_PIN[idx], RELAY_OFF);
          manualTimerOff[idx] = 0;

          zona[idx].duration = (millis() - manualStartTime[idx]) / 1000.0f;
          addLogEntry();
          zona[idx].duration = 0.0f;
        }
        updateMainRelayState();
      }
    }
  }
  server.send(200, "text/plain", "OK");
}

static void handleConfigGet() {
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

static void handleConfigPost() {
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

static void handleScheduleGet() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < NUM_ZONES; i++) {
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

static void handleSchedulePost() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    if (!error && doc.is<JsonArray>()) {
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
      saveSchedule();
      server.send(200, "text/plain", "OK");
      return;
    }
  }
  server.send(400, "text/plain", "Bad Request");
}

static void handleLogGet() {
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
      for (int j = 0; j < NUM_ZONES; j++) {
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

static void handleLogReset() {
  if (logMutex && xSemaphoreTake(logMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    logHead = 0;
    logCount = 0;
    xSemaphoreGive(logMutex);
  }
  if (LittleFS.exists(LOG_FILE)) {
    LittleFS.remove(LOG_FILE);
  }
  server.send(200, "text/plain", "OK");
}

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
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
