#include "sensor_system.h"
#include "esp_log.h"

#define DEBOUNCE_TIME_MS 50
#define LONG_PRESS_TIME_MS 1500
#define DOUBLE_PRESS_WINDOW_MS 400

static const char *TAG = "BUTTON";

// This task is now driven by interrupts via a semaphore.
void task_button_handler(void *pvParameters) {
    // State machine to handle button logic
    typedef enum { IDLE, DEBOUNCING, PRESSED, DOUBLE_PRESS_WAIT } state_t;
    state_t state = IDLE;
    TickType_t last_press_time = 0;

    while (1) {
        switch (state) {
            case IDLE:
                // Wait indefinitely for a press from the ISR
                if (xSemaphoreTake(button_press_sem, portMAX_DELAY) == pdTRUE) {
                    state = DEBOUNCING;
                }
                break;

            case DEBOUNCING:
                vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_TIME_MS));
                if (gpio_get_level(BUTTON_PIN) == 0) { // Still pressed, it's a valid press
                    state = PRESSED;
                    last_press_time = xTaskGetTickCount();
                } else { // It was a glitch
                    state = IDLE;
                }
                break;

            case PRESSED:
                // Button is being held down. Check for release or long press.
                if (gpio_get_level(BUTTON_PIN) == 1) { // Button has been released
                    // Move to a state to wait for a potential second press
                    state = DOUBLE_PRESS_WAIT;
                } else {
                    // Check if it has been held long enough for a long press
                    if ((xTaskGetTickCount() - last_press_time) * portTICK_PERIOD_MS > LONG_PRESS_TIME_MS) {
                        ESP_LOGI(TAG, "Long press detected");
                        event_message_t msg = { .type = EVENT_LONG_PRESS };
                        xQueueSend(system_event_queue, &msg, 0);
                        // Wait for the button to be released before resetting state
                        while (gpio_get_level(BUTTON_PIN) == 0) {
                            vTaskDelay(pdMS_TO_TICKS(10));
                        }
                        state = IDLE;
                    }
                }
                break;

            case DOUBLE_PRESS_WAIT:
                // After a release, wait for a short window to see if another press occurs.
                if (xSemaphoreTake(button_press_sem, pdMS_TO_TICKS(DOUBLE_PRESS_WINDOW_MS)) == pdTRUE) {
                    // A second press arrived within the window! It's a double press.
                    ESP_LOGI(TAG, "Double press detected");
                    event_message_t msg = { .type = EVENT_DOUBLE_PRESS };
                    xQueueSend(system_event_queue, &msg, 0);
                    // Wait for the final release
                    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_TIME_MS)); // Debounce
                    while (gpio_get_level(BUTTON_PIN) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    state = IDLE;
                } else {
                    // The window timed out, so it was just a single press.
                    ESP_LOGI(TAG, "Single press detected");
                    event_message_t msg = { .type = EVENT_SINGLE_PRESS };
                    xQueueSend(system_event_queue, &msg, 0);
                    state = IDLE;
                }
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // Prevent busy-waiting in some states
    }
}