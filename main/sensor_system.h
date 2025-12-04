#ifndef SENSOR_SYSTEM_H
#define SENSOR_SYSTEM_H

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"      // For Software Timers
#include "freertos/semphr.h"      // For Semaphores and Mutexes
#include "freertos/event_groups.h" // For Event Groups
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

// --- Pin Definitions ---
#define BUTTON_PIN          GPIO_NUM_2
#define MOTION_SENSOR_PIN   GPIO_NUM_3
#define LIGHT_SENSOR_PIN    ADC_CHANNEL_0
#define LED_PIN             GPIO_NUM_1

#define OLED_SDA_PIN        GPIO_NUM_5
#define OLED_SCL_PIN        GPIO_NUM_4

// --- Event Group Bits ---
#define BIT_OLED_INIT_OK (1 << 0)
#define BIT_SENSORS_INIT_OK (1 << 1)

// --- System Mode ---
typedef enum {
    SYSTEM_MODE_AUTO,
    SYSTEM_MODE_MANUAL
} system_mode_t;

// --- System State Struct ---
typedef struct {
    uint32_t light_level;
    uint32_t motion_count;
    bool led_state;
    system_mode_t mode;
    TickType_t last_motion_time;
} system_state_t;

// --- Queue Message Struct ---
typedef enum {
    EVENT_SINGLE_PRESS,
    EVENT_DOUBLE_PRESS,
    EVENT_LONG_PRESS,      // New event for long press
    EVENT_MOTION,
    EVENT_MOTION_TIMEOUT,
    EVENT_LIGHT,  // New event from software timer
} event_type_t;

typedef struct {
    event_type_t type;
    int value;          // optional: ví dụ 1 = motion detected
} event_message_t;


// --- Global Handles for FreeRTOS objects ---
extern system_state_t system_state;
extern QueueHandle_t system_event_queue;
extern SemaphoreHandle_t g_state_mutex;
extern SemaphoreHandle_t button_press_sem;
extern EventGroupHandle_t g_system_event_group;
extern TimerHandle_t led_off_timer;
extern TaskHandle_t g_oled_task_handle; // For notifications

// --- Function Prototypes ---
void init_hardware(void);
void init_oled_i2c(void);
void task_motion_sensor(void *pvParameters);
void task_light_sensor(void *pvParameters);
void task_led_controller(void *pvParameters);
void task_button_handler(void *pvParameters);
void task_oled_display(void *pvParameters);

#endif // SENSOR_SYSTEM_H
