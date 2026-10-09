#include "tasks/cloud_task.h"
#include "config.h"
#include "types.h"
#include "shared_state.h"
#include "storage_manager.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static String buildTelemetryJson(bool isOffline) {
  JsonDocument doc;
  doc["device_key"] = DEVICE_KEY;
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
    for (int i = 0; i < NUM_ZONES; i++) {
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

void CloudSyncTask(void *pv) {
  // Tunggu 3 detik agar SoftAP stabil dulu sebelum konek ke router
  vTaskDelay(pdMS_TO_TICKS(3000));

  WiFi.begin(STA_SSID, STA_PASSWORD);
  Serial.println("[WiFi] Mencoba konek ke router internet...");

  uint32_t lastReconnectAttempt = millis();

  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {

      // 1. Flush buffer offline jika ada data yang belum terkirim
      if (LittleFS.exists(OFFLINE_QUEUE_FILE)) {
        flushOfflineBuffer();
      }

      // 2. Kirim data sensor live ke Laravel
      String payload = buildTelemetryJson(false);
      HTTPClient http;
      http.begin(CLOUD_URL);
      http.addHeader("Content-Type", "application/json");
      http.addHeader("X-Device-Token", DEVICE_KEY);
      http.setTimeout(3000);

      int code = http.POST(payload);
      if (code == HTTP_CODE_OK || code == HTTP_CODE_CREATED) {
        String resp = http.getString();
        Serial.printf("[CloudSync] Telemetry OK (%d)\n", code);

        // Update konfigurasi/perintah dari Web Dashboard Laravel
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
              Serial.printf("[CloudSync] manualModeType -> %d\n", cfgManualMode);
            }
            if (cfgManualDur != newManualDur) {
              cfgManualDur = newManualDur;
              Serial.printf("[CloudSync] manualDuration -> %u detik\n", cfgManualDur);
            }

            // 3. Update jadwal per zona dari schedules_json
            if (!cmd["schedules"].isNull()) {
              JsonArray schArr = cmd["schedules"].as<JsonArray>();
              if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                for (JsonObject zoneObj : schArr) {
                  int zoneIdx = (int)zoneObj["zone"] - 1; // 1-based ke 0-based
                  if (zoneIdx < 0 || zoneIdx >= NUM_ZONES)
                    continue;

                  JsonArray items = zoneObj["items"].as<JsonArray>();
                  int si = 0;
                  for (JsonObject item : items) {
                    if (si >= MAX_SCHEDULES)
                      break;
                    zoneSchedules[zoneIdx][si].active = item["active"] | false;
                    zoneSchedules[zoneIdx][si].hour = (uint8_t)((int)item["hour"] | 0);
                    zoneSchedules[zoneIdx][si].minute = (uint8_t)((int)item["minute"] | 0);
                    zoneSchedules[zoneIdx][si].duration = (uint32_t)((int)item["duration"] | 0);
                    si++;
                  }
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
        Serial.printf("[CloudSync] Gagal HTTP %d. Data disimpan ke buffer.\n", code);
        saveToOfflineBuffer(payload);
      }
      http.end();

    } else {
      // WiFi tidak tersambung – simpan ke buffer offline setiap 30 detik (agar tidak spam flash I/O)
      static uint32_t lastOfflineSave = 0;
      if (millis() - lastOfflineSave >= 30000 || lastOfflineSave == 0) {
        Serial.println("[CloudSync] Wi-Fi STA terputus. Menyimpan telemetry ke buffer offline.");
        String payload = buildTelemetryJson(true);
        saveToOfflineBuffer(payload);
        lastOfflineSave = millis();
      }

      // Reconnect secara aman tanpa merusak AP (hindari WiFi.disconnect)
      if (millis() - lastReconnectAttempt > 20000) {
        Serial.println("[WiFi] Mencoba menyambung kembali ke router internet...");
        WiFi.reconnect();
        lastReconnectAttempt = millis();
      }
    }

    // Interval sinkronisasi: setiap 5 detik
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}
