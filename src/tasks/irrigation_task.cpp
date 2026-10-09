#include "tasks/irrigation_task.h"
#include "config.h"
#include "types.h"
#include "shared_state.h"

void IrrigationTask(void *pv) {
  int id = (int)(uintptr_t)pv;

  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (systemMode == 1) {
      continue;
    }

    // Batasi concurrent irrigation sesuai semaphore
    xSemaphoreTake(maxConcurrentIrrigationSemaphore, portMAX_DELAY);

    // Cek ulang mode jika user mengubah ke manual saat task sedang di antrian (queue)
    if (systemMode == 1) {
      if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        zona[id].mode = 0;
        zona[id].duration = 0.0f;
        xSemaphoreGive(zonaMutex);
      }
      xSemaphoreGive(maxConcurrentIrrigationSemaphore);
      continue;
    }

    // Set mode aktual (Active) setelah keluar dari antrian
    if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (systemMode == 2) {
        zona[id].mode = 3; // Schedule mode
      } else {
        if (zona[id].duration <= 20.0f)
          zona[id].mode = 1;
        else if (zona[id].duration <= 40.0f)
          zona[id].mode = 2;
        else
          zona[id].mode = 3;
      }
      xSemaphoreGive(zonaMutex);
    }

    portENTER_CRITICAL(&mux);
    activeIrrigations++;
    portEXIT_CRITICAL(&mux);

    digitalWrite(RELAY_PIN[id], RELAY_ON);
    digitalWrite(RELAY_MAIN, RELAY_ON);

    vTaskDelay(pdMS_TO_TICKS((uint32_t)(zona[id].duration * 1000)));

    if (systemMode != 1) {
      digitalWrite(RELAY_PIN[id], RELAY_OFF);
    }

    // Matikan relay utama jika tidak ada zona lain yang aktif
    portENTER_CRITICAL(&mux);
    activeIrrigations--;
    if (activeIrrigations == 0 && systemMode != 1) {
      digitalWrite(RELAY_MAIN, RELAY_OFF);
    }
    portEXIT_CRITICAL(&mux);

    // Jeda penyerapan air 15 detik
    vTaskDelay(pdMS_TO_TICKS(15000));

    addLogEntry();

    // Catat waktu selesai penyiraman untuk cooldown
    lastWateringTime[id] = getCurrentTimeSeconds();

    // Reset mode ke Idle
    if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      zona[id].mode = 0;
      zona[id].duration = 0.0f;
      xSemaphoreGive(zonaMutex);
    }

    xSemaphoreGive(maxConcurrentIrrigationSemaphore);
  }
}
