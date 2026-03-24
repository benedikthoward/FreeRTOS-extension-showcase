#ifndef USB_TASK_H
#define USB_TASK_H

/**
 * Create a high-priority FreeRTOS task that initialises USB CDC stdio and
 * polls tud_task() at 1 ms intervals.  Call this from main() BEFORE
 * vTaskStartScheduler() — the actual USB init happens inside the task
 * (i.e. after the scheduler is running), which is required on the RP2350.
 */
void vStartUsbTask(void);

#endif /* USB_TASK_H */
