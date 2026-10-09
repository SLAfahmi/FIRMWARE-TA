#include "tasks/manual_task.h"
#include "config.h"
#include "types.h"
#include "shared_state.h"

void ManualControlTask(void *pv) {
  while (1) {
    if (systemMode == 1 && cfgManualMode == 1) {
      uint32_t now = millis();
      bool changed = false;

      for (int i = 0; i < NUM_ZONES; i++) {
        if (manualRelayState[i] && manualTimerOff[i] > 0 && now >= manualTimerOff[i]) {
          manualRelayState[i] = false;
          digitalWrite(RELAY_PIN[i], RELAY_OFF);
          manualTimerOff[i] = 0;

          zona[i].duration = (now - manualStartTime[i]) / 1000.0f;
          addLogEntry();
          zona[i].duration = 0.0f;

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
