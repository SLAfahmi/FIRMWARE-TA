#include "tasks/sensor_task.h"
#include "config.h"
#include "types.h"
#include "shared_state.h"

void SensorTask(void *pv) {
  // Beri jeda 500ms agar semua task & hardware siap
  vTaskDelay(pdMS_TO_TICKS(500));

  while (1) {
    if (xSemaphoreTake(zonaMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      for (int i = 0; i < NUM_ZONES; i++) {
        int adc = analogRead(SOIL_PIN[i]);

        // 1. Regresi Linier (ADC ke Moisture Analyzer Y)
        float a = 41.7453982f;
        float b = -0.012665492f;
        float Y = a + (b * adc);

        // 2. Mapping nilai Y (10 - 35) ke persentase (0 - 100%)
        float mappedSoil = (Y - 10.0f) * (100.0f / 25.0f);

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
      temperature = 0.0f;
    if (isnan(humidity))
      humidity = 0.0f;
    if (isnan(lightLevel))
      lightLevel = 0.0f;

    static uint32_t lastHourlyLog = 0;
    uint32_t currentSec = getCurrentTimeSeconds();

    // Log rutin (interval default 3600 detik)
    if (lastHourlyLog == 0 || (currentSec - lastHourlyLog >= cfgLogInterval)) {
      addLogEntry();
      lastHourlyLog = currentSec;
    }

    // Picu evaluasi fuzzy jika dalam mode Auto dan tidak sedang menyiram
    if (systemMode == 0 && activeIrrigations == 0 && fuzzyHandle != nullptr) {
      xTaskNotifyGive(fuzzyHandle);
    }

    vTaskDelay(pdMS_TO_TICKS(cfgSensorInterval * 1000));
  }
}
