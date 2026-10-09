#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>

#include "config.h"
#include "types.h"
#include "shared_state.h"
#include "storage_manager.h"

// Task Headers
#include "tasks/sensor_task.h"
#include "tasks/fuzzy_task.h"
#include "tasks/irrigation_task.h"
#include "tasks/manual_task.h"
#include "tasks/schedule_task.h"
#include "tasks/web_task.h"
#include "tasks/cloud_task.h"
#include "tasks/memory_task.h"

void setup() {
  Serial.begin(115200);
  delay(1000); // Beri jeda 1 detik agar Serial Monitor USB CDC siap
  Serial.println("\n==========================================");
  Serial.println("   SMART NURSERY BOOTING (TEL-U)       ");
  Serial.println("==========================================");

  // Set I2C Timeout agar tidak hang jika sensor terlepas/salah pin
  Wire.setTimeOut(1000);
  Wire.begin();

  // Inisialisasi Storage LittleFS
  initStorage();

  // Inisialisasi Pin Relay
  pinMode(RELAY_MAIN, OUTPUT);
  digitalWrite(RELAY_MAIN, RELAY_OFF);
  for (int i = 0; i < NUM_ZONES; i++) {
    pinMode(RELAY_PIN[i], OUTPUT);
    digitalWrite(RELAY_PIN[i], RELAY_OFF);
  }

  Serial.println("[BOOT] Memulai Sensor (DHT22, BH1750, RTC)...");
  dht.begin();

  if (!lightMeter.begin()) {
    Serial.println("[WARN] Gagal menemukan sensor BH1750!");
  }

  if (!rtc.begin()) {
    rtcAvailable = false;
    Serial.println("[WARN] Gagal menemukan RTC DS3231! Menggunakan millis() untuk cooldown");
  } else {
    rtcAvailable = true;
    if (rtc.lostPower()) {
      Serial.println("[RTC] RTC kehilangan daya, mengatur waktu ke waktu kompilasi...");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }

  Serial.println("[BOOT] Memulai WiFi AP & STA...");
  // Mode WIFI_AP_STA: ESP tetap sebagai Access Point lokal
  // DAN sekaligus terhubung ke Router Internet untuk Cloud Sync
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  WiFi.setSleep(false); // Nonaktifkan Modem Sleep agar respon web server tidak delay/lag

  Serial.print("[WiFi] Access Point IP: ");
  Serial.println(WiFi.softAPIP());

  Serial.println("[WiFi] AP dimulai, CloudSyncTask akan konek ke router...");

  // Inisialisasi Mutex & Semaphore
  zonaMutex = xSemaphoreCreateMutex();
  maxConcurrentIrrigationSemaphore = xSemaphoreCreateCounting(NUM_ZONES, NUM_ZONES);
  logMutex = xSemaphoreCreateMutex();

  // Alokasikan logBuffer di heap dinamis
  logBuffer = new LogEntry[MAX_LOG_ENTRIES];
  if (!logBuffer) {
    Serial.println("FATAL: Gagal alokasi logBuffer di heap!");
  }

  // Peluncuran FreeRTOS Tasks dengan Pinning Core (ESP32-S3 Dual Core):
  // Core 0: Khusus Network / Wi-Fi stack & CloudSyncTask
  // Core 1: Web Server handling, Sensor, Fuzzy, dan Aktuator
  xTaskCreatePinnedToCore(SensorTask, "Sensor", 4096, NULL, 5, &sensorHandle, 1);
  xTaskCreatePinnedToCore(FuzzyTask, "Fuzzy", 4096, NULL, 4, &fuzzyHandle, 1);

  for (int i = 0; i < NUM_ZONES; i++) {
    char taskName[16];
    snprintf(taskName, sizeof(taskName), "Irrig_%d", i + 1);
    xTaskCreatePinnedToCore(IrrigationTask, taskName, 3072, (void *)(uintptr_t)i, 3, &irrigationHandle[i], 1);
  }

  // WebTask untuk server lokal AP (Core 1, Priority 3 agar sangat responsif melayani client web)
  xTaskCreatePinnedToCore(WebTask, "Web", 8192, NULL, 3, &webHandle, 1);

  // CloudSyncTask (Core 0, Priority 2 bersama Network Wi-Fi Stack)
  xTaskCreatePinnedToCore(CloudSyncTask, "CloudSync", 14336, NULL, 2, &cloudSyncHandle, 0);

  // Task manual control timeout
  xTaskCreatePinnedToCore(ManualControlTask, "ManualControl", 3072, NULL, 1, &manualControlHandle, 1);

  // Task jadwal RTC
  xTaskCreatePinnedToCore(ScheduleTask, "Schedule", 4096, NULL, 1, &scheduleHandle, 1);

  // Task pemantauan memori
  xTaskCreatePinnedToCore(MemoryTask, "Memory", 2048, NULL, 1, &memoryHandle, 1);

  // Load konfigurasi tersimpan
  loadSchedule();
  loadLogFile();

  Serial.println("Smart Nursery Siap!");
  printMemoryInfo();
}

void loop() {
  vTaskDelete(NULL); // Hapus task loop default Arduino untuk hemat resource
}
