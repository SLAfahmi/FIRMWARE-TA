#include "tasks/fuzzy_task.h"
#include "config.h"
#include "types.h"
#include "shared_state.h"
#include "fuzzy_logic.h"

void FuzzyTask(void *pv) {
  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (systemMode != 0) {
      continue; // Skip evaluasi jika bukan mode Auto
    }

    if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      uint32_t currentTime = getCurrentTimeSeconds();

      for (int i = 0; i < NUM_ZONES; i++) {
        float durasi = prosesFuzzyMamdani(zona[i].soil, temperature, lightLevel);

        // Cek cooldown
        if (lastWateringTime[i] != 0 && (currentTime - lastWateringTime[i]) < cfgCooldown) {
          durasi = 0.0f;
        }

        zona[i].duration = durasi;
        if (durasi > 0.0f) {
          zona[i].mode = 4; // 4 = Queued
          if (irrigationHandle[i] != nullptr) {
            xTaskNotifyGive(irrigationHandle[i]);
          }
        } else {
          zona[i].mode = 0;
          zona[i].duration = 0.0f;
        }
      }
      xSemaphoreGive(zonaMutex);
    }
  }
}
