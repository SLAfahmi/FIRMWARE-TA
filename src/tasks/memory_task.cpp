#include "tasks/memory_task.h"
#include "config.h"
#include "types.h"
#include "shared_state.h"
#include <esp_heap_caps.h>

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

  for (int i = 0; i < NUM_ZONES; i++) {
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
