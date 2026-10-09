#include "tasks/schedule_task.h"
#include "config.h"
#include "types.h"
#include "shared_state.h"

void ScheduleTask(void *pv) {
  while (1) {
    if (systemMode == 2 && rtcAvailable) {
      DateTime now = rtc.now();
      int currentHour = now.hour();
      int currentMinute = now.minute();

      // Cek setiap pergantian menit
      if (lastScheduledMinute != currentMinute) {
        lastScheduledMinute = currentMinute;

        if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
          for (int i = 0; i < NUM_ZONES; i++) {
            for (int j = 0; j < MAX_SCHEDULES; j++) {
              if (zoneSchedules[i][j].active &&
                  zoneSchedules[i][j].hour == currentHour &&
                  zoneSchedules[i][j].minute == currentMinute) {
                // Cek jika soil moisture <= 60%
                if (zona[i].soil <= 60.0f) {
                  zona[i].duration = zoneSchedules[i][j].duration;
                  zona[i].mode = 4; // Queued
                  if (irrigationHandle[i] != nullptr) {
                    xTaskNotifyGive(irrigationHandle[i]);
                  }
                }
                break; // Cukup 1 jadwal per zona di menit ini
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
