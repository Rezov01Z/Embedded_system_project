#include "sensor_system.h"
#include "ssd1306.h"
#include "font8x8_basic.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

extern SSD1306_t dev;

void task_oled_display(void *pvParameters) {
    // I2C is already initialized by init_oled_i2c() called from app_main
    // Just signal that OLED task is ready
    xEventGroupSetBits(g_system_event_group, BIT_OLED_INIT_OK);
    ESP_LOGI("EVENT_GROUP", "BIT_OLED_INIT_OK set by OLED task");
    
    ssd1306_clear_screen(&dev, false);
    ssd1306_display_text(&dev, 0, "SYSTEM READY", 12, false);
    vTaskDelay(pdMS_TO_TICKS(1500));

    char buffer[24];
    bool diagnostic_mode = false;

    while (1) {
        // Wait for a notification OR timeout after 500ms
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) > 0) {
            ESP_LOGI("TASK_NOTIF", "OLED task received notification, toggling diagnostic mode");
            // We were woken up by a notification, toggle diagnostic mode
            diagnostic_mode = !diagnostic_mode;
        }
        
        // --- Use Mutex to protect access to system_state ---
        if (xSemaphoreTake(g_state_mutex, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI("MUTEX", "Mutex taken by OLED task");
            ssd1306_clear_screen(&dev, false);

            if (diagnostic_mode) {
                ssd1306_display_text(&dev, 0, "*DIAGNOSTIC MODE*", 17, true);
                snprintf(buffer, sizeof(buffer), "Light Val: %lu", system_state.light_level);
                ssd1306_display_text(&dev, 2, buffer, strlen(buffer), false);
                snprintf(buffer, sizeof(buffer), "Motion Cnt: %lu", system_state.motion_count);
                ssd1306_display_text(&dev, 3, buffer, strlen(buffer), false);
                snprintf(buffer, sizeof(buffer), "Heap: %lu", esp_get_free_heap_size());
                ssd1306_display_text(&dev, 5, buffer, strlen(buffer), false);

            } else {
                // Normal display logic
                const char *mode_str = (system_state.mode == SYSTEM_MODE_AUTO) ? "AUTO" : "MANUAL";
                snprintf(buffer, sizeof(buffer), "Mode: %s", mode_str);
                ssd1306_display_text(&dev, 0, buffer, strlen(buffer), false);

                snprintf(buffer, sizeof(buffer), "Light: %3lu%%", system_state.light_level);
                ssd1306_display_text(&dev, 2, buffer, strlen(buffer), false);
                
                snprintf(buffer, sizeof(buffer), "Motion: %3lu", system_state.motion_count);
                ssd1306_display_text(&dev, 3, buffer, strlen(buffer), false);
                
                snprintf(buffer, sizeof(buffer), "LED: %s", system_state.led_state ? "ON" : "OFF");
                ssd1306_display_text(&dev, 4, buffer, strlen(buffer), false);
            }
            
            xSemaphoreGive(g_state_mutex);
            ESP_LOGI("MUTEX", "Mutex released by OLED task");
        }
    }
}