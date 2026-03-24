#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

int main(void)
{
    stdio_init_all();

    if (cyw43_arch_init()) {
        /* No LED available — just spin. */
        for (;;) {}
    }

    int on = 0;

    for (;;) {
        on = !on;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
        printf("blink %s\n", on ? "ON" : "OFF");
        sleep_ms(500);
    }
}
