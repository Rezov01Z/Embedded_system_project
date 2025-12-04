#include "sensor_system.h"
#include "ssd1306.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

// --- Global Handles for FreeRTOS objects ---
QueueHandle_t system_event_queue;
SemaphoreHandle_t g_state_mutex;
SemaphoreHandle_t button_press_sem;
EventGroupHandle_t g_system_event_group;
TimerHandle_t led_off_timer;
TaskHandle_t g_oled_task_handle = NULL; // Initialize to NULL

// --- Timer Callback ---
void led_off_timer_callback(TimerHandle_t xTimer) {
    event_message_t msg = { .type = EVENT_MOTION_TIMEOUT };
    xQueueSend(system_event_queue, &msg, 0);
}

void app_main() {
    // --- Create FreeRTOS Objects BEFORE creating tasks ---
    system_event_queue = xQueueCreate(10, sizeof(event_message_t));
    g_state_mutex = xSemaphoreCreateMutex();
    button_press_sem = xSemaphoreCreateBinary();
    g_system_event_group = xEventGroupCreate();
    led_off_timer = xTimerCreate("LED_Off_Timer", pdMS_TO_TICKS(10000), pdFALSE, (void *)0, led_off_timer_callback);

    // Initialize hardware (this will set one of the event bits)
    init_hardware();

    // --- Initialize I2C and OLED before creating OLED task ---
    init_oled_i2c();

    // --- Create Tasks ---
    xTaskCreate(task_button_handler, "Button Handler", 4096, NULL, 10, NULL);
    xTaskCreate(task_led_controller, "LED Controller", 4096, NULL, 8, NULL);
    xTaskCreate(task_light_sensor, "Light Sensor", 4096, NULL, 7, NULL);
    xTaskCreate(task_motion_sensor, "Motion Sensor", 4096, NULL, 6, NULL);
    // Create OLED task and store its handle for notifications
    xTaskCreate(task_oled_display, "OLED Display", 4096, NULL, 2, &g_oled_task_handle);

    printf("Waiting for system to initialize...\n");

    // --- Wait for both OLED and Sensors to be ready ---
    EventBits_t bits = xEventGroupWaitBits(g_system_event_group,
                                           BIT_OLED_INIT_OK | BIT_SENSORS_INIT_OK,
                                           pdTRUE, // Clear bits on exit
                                           pdTRUE, // Wait for ALL bits
                                           portMAX_DELAY);

    if ((bits & (BIT_OLED_INIT_OK | BIT_SENSORS_INIT_OK)) == (BIT_OLED_INIT_OK | BIT_SENSORS_INIT_OK)) {
        printf("Smart Light System Started!\n");
        printf("Button: GPIO%d, Motion: GPIO%d, Light: ADC%d, LED: GPIO%d\n",
               BUTTON_PIN, MOTION_SENSOR_PIN, LIGHT_SENSOR_PIN, LED_PIN);
    } else {
        printf("Error: System initialization failed.\n");
    }
    // app_main can now exit as the scheduler is running the tasks.
}
