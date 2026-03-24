#include "usb_task.h"

#include <stdio.h>
#include "pico/stdlib.h"
#include "tusb.h"
#include "FreeRTOS.h"
#include "task.h"

static void prvUsbTask(void *pvParameters)
{
    (void)pvParameters;

    /* Initialise USB CDC stdio now that the scheduler is running.
     * The SDK's alarm-based tud_task() background poller is disabled
     * (PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0 in CMake), so we
     * drive tud_task() ourselves from this task. */
    stdio_init_all();

    for (;;) {
        tud_task();
        vTaskDelay(1);  /* 1 ms — keeps USB responsive */
    }
}

void vStartUsbTask(void)
{
    xTaskCreate(prvUsbTask, "USB", 512, NULL,
                configMAX_PRIORITIES - 1, NULL);
}
