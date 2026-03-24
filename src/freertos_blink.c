#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "FreeRTOS.h"
#include "task.h"
#include "usb_task.h"

static void vBlinkTask(void *pvParameters)
{
    (void)pvParameters;

    /* Wait for USB to enumerate before printing */
    vTaskDelay(pdMS_TO_TICKS(2000));
    printf("FreeRTOS blink — running\n");

    int on = 0;
    for (;;) {
        on = !on;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
        printf("blink %s (tick=%lu)\n", on ? "ON" : "OFF",
               (unsigned long)xTaskGetTickCount());
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

int main(void)
{
    if (cyw43_arch_init()) {
        return -1;
    }

    vStartUsbTask();
    xTaskCreate(vBlinkTask, "Blink", 512, NULL, 1, NULL);
    vTaskStartScheduler();

    for (;;);
}
