// sensor_system.c
#include "sensor_system.h"
#include "ssd1306.h"
#include "font8x8_basic.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

static const char *TAG = "SENSOR";

adc_oneshot_unit_handle_t adc1_handle;

// --- Global variable definitions ---
system_state_t system_state = {
    .light_level = 50,
    .motion_count = 0,
    .led_state = false,
    .mode = SYSTEM_MODE_AUTO,
    .last_motion_time = 0
};
// SSD1306 device instance used by OLED tasks and drivers
SSD1306_t dev;
// Note: Other handles are defined in main.c

// --- ISR Handler ---
// This function is called from an interrupt. It must be fast.
void IRAM_ATTR button_isr_handler(void* arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(button_press_sem, &xHigherPriorityTaskWoken);
    // Log chỉ nên thực hiện ở task, không phải trong ISR
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

void led_off_timer_callback(TimerHandle_t xTimer) {
    ESP_LOGI("TIMER", "Timer expired, sending EVENT_MOTION_TIMEOUT");
    event_message_t msg = { .type = EVENT_MOTION_TIMEOUT };
    xQueueSend(system_event_queue, &msg, 0);
}

void init_hardware() {
    // --- Button (with interrupt) ---
    gpio_config_t btn_config = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE // Interrupt on falling edge
    };
    gpio_config(&btn_config);

    // Install ISR service and add handler for the button pin
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_PIN, button_isr_handler, NULL);

    // --- Other hardware init (motion, led, adc) ---
    gpio_config_t motion_config = {
        .pin_bit_mask = (1ULL << MOTION_SENSOR_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&motion_config);

    gpio_config_t led_config = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&led_config);

    // --- ADC ---
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_0, &config));

    // --- Signal that sensor hardware is ready ---
    xEventGroupSetBits(g_system_event_group, BIT_SENSORS_INIT_OK);
    ESP_LOGI("EVENT_GROUP", "BIT_SENSORS_INIT_OK set");
}

// --- I2C and OLED initialization (called once from app_main) ---
void init_oled_i2c(void) {
    ESP_LOGI(TAG, "Initializing I2C and OLED...");
    i2c_master_init(&dev, OLED_SDA_PIN, OLED_SCL_PIN, -1);
    vTaskDelay(pdMS_TO_TICKS(100)); // Give I2C time to settle
    
    ssd1306_init(&dev, 128, 64);
    ESP_LOGI(TAG, "SSD1306 initialized successfully");
    xEventGroupSetBits(g_system_event_group, BIT_OLED_INIT_OK);
    ESP_LOGI("EVENT_GROUP", "BIT_OLED_INIT_OK set");
}

/*
 * Motion task:
 * - Detect rising edge (LOW -> HIGH) only
 * - Send EVENT_MOTION into queue
 * - Does NOT directly modify system_state (to avoid races) —
 *   system_state will be updated in task_led_controller when receiving the event.
 */
void task_motion_sensor(void *pvParameters) {
    int last_motion_state = 0;

    while (1) {
        int motion_state = gpio_get_level(MOTION_SENSOR_PIN);

        if (motion_state == 1 && last_motion_state == 0) {
            event_message_t msg = {
                .type = EVENT_MOTION,
                .value = 1
            };
            BaseType_t res = xQueueSend(system_event_queue, &msg, 0);
            if (res == pdPASS) {
                ESP_LOGI(TAG, "Motion detected -> event queued");
            } else {
                ESP_LOGW(TAG, "Motion detected but queue full");
            }
        }

        last_motion_state = motion_state;
        vTaskDelay(pdMS_TO_TICKS(200)); // debounce / reduce spam
    }
}

/*
 * Light sensor task (reads ADC, updates system_state.light_level)
 * We keep updating light_level directly for responsiveness; if you prefer,
 * we can also send EVENT_LIGHT via queue instead.
 */
void task_light_sensor(void *pvParameters) {
    int raw_value;
    while (1) {
        ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_0, &raw_value));
        // convert to percentage: 0..100 (higher = brighter)
        // --- Use Mutex to protect access to system_state ---
        if (xSemaphoreTake(g_state_mutex, portMAX_DELAY) == pdTRUE) {
            system_state.light_level = 100 - (raw_value * 100) / 4095;
            ESP_LOGI(TAG, "ADC raw=%d -> brightness=%lu%%", raw_value, system_state.light_level);
            xSemaphoreGive(g_state_mutex);

            // Notify LED controller about light level (1 = dark, 0 = bright)
            event_message_t light_msg = {
                .type = EVENT_LIGHT,
                .value = (system_state.light_level < 50) ? 1 : 0,
            };
            BaseType_t send_res = xQueueSend(system_event_queue, &light_msg, 0);
            if (send_res != pdPASS) {
                ESP_LOGW(TAG, "Light event not queued (queue full)");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/*
 * LED controller:
 * - Receives events from queue (motion, button presses, ...)
 * - Updates system_state.motion_count and last_motion_time on EVENT_MOTION
 * - Uses threshold is_dark = (light_level > 40) as requested
 * - Maps system_state.led_state == true  -> physical GPIO = 1 (ON)
 *   (This ensures log "ON" matches physical LED lit)
 */
void task_led_controller(void *pvParameters) {
    event_message_t msg;

    while (1) {
        // Wait for any event from the queue
        if (xQueueReceive(system_event_queue, &msg, portMAX_DELAY) == pdPASS) {
            ESP_LOGI("QUEUE", "Received event: %d", msg.type);
            // --- Use Mutex to protect access to system_state ---
            if (xSemaphoreTake(g_state_mutex, portMAX_DELAY) == pdTRUE) {
                ESP_LOGI("MUTEX", "Mutex taken by LED controller");
                switch (msg.type) {
                    case EVENT_MOTION:
                        if (system_state.mode == SYSTEM_MODE_AUTO) {
                            system_state.motion_count++;
                            system_state.last_motion_time = xTaskGetTickCount();
                            // Turn LED ON and start/reset the one-shot timer
                            gpio_set_level(LED_PIN, 1);
                            system_state.led_state = true;
                            xTimerReset(led_off_timer, portMAX_DELAY);
                            ESP_LOGI("TIMER", "Resetting LED off timer");
                        }
                        break;
                    case EVENT_LIGHT:
                        if (system_state.mode == SYSTEM_MODE_AUTO) {
                            if (msg.value == 1) {
                                // It's dark -> turn LED ON (if not already)
                                if (!system_state.led_state) {
                                    gpio_set_level(LED_PIN, 1);
                                    system_state.led_state = true;
                                    xTimerReset(led_off_timer, portMAX_DELAY);
                                    ESP_LOGI(TAG, "Brightness=%lu%% -> LED ON", system_state.light_level);
                                }
                            } else {
                                // It's bright -> turn LED OFF (if currently on)
                                if (system_state.led_state) {
                                    gpio_set_level(LED_PIN, 0);
                                    system_state.led_state = false;
                                    xTimerStop(led_off_timer, portMAX_DELAY);
                                    ESP_LOGI(TAG, "Brightness=%lu%% -> LED OFF", system_state.light_level);
                                }
                            }
                        }
                        break;
                    case EVENT_MOTION_TIMEOUT:
                        if (system_state.mode == SYSTEM_MODE_AUTO) {
                            // Timer expired, turn LED OFF
                            gpio_set_level(LED_PIN, 0);
                            system_state.led_state = false;
                            ESP_LOGI("TIMER", "LED off timer expired, turning off LED");
                        }
                        break;

                    case EVENT_DOUBLE_PRESS:
                        system_state.mode = (system_state.mode == SYSTEM_MODE_AUTO) ? SYSTEM_MODE_MANUAL : SYSTEM_MODE_AUTO;
                        ESP_LOGI("MODE", "Double press: switched to %s mode", system_state.mode == SYSTEM_MODE_AUTO ? "AUTO" : "MANUAL");
                        // Khi chuyển sang MANUAL, tắt đèn
                        if (system_state.mode == SYSTEM_MODE_MANUAL) {
                            system_state.led_state = false;
                            gpio_set_level(LED_PIN, 0);
                        }
                        break;

                    case EVENT_SINGLE_PRESS:
                        if (system_state.mode == SYSTEM_MODE_MANUAL) {
                            system_state.led_state = !system_state.led_state;
                            gpio_set_level(LED_PIN, system_state.led_state);
                            ESP_LOGI("LED", "Single press: LED %s", system_state.led_state ? "ON" : "OFF");
                        }
                        break;
                    
                    case EVENT_LONG_PRESS:
                        // Notify the OLED task to enter diagnostic mode
                        xTaskNotifyGive(g_oled_task_handle);
                        ESP_LOGI("TASK_NOTIF", "Sending notification to OLED task");
                        break;

                    default:
                        break;
                }
                xSemaphoreGive(g_state_mutex);
                ESP_LOGI("MUTEX", "Mutex released by LED controller");
            }
        }
    }
}
