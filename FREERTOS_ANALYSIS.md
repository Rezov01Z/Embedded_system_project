# Phân Tích Chi Tiết Sử Dụng FreeRTOS trong Dự Án Smart Light System

## 📋 Mục Lục
1. [Tổng Quan Hệ Thống](#tổng-quan-hệ-thống)
2. [FreeRTOS Components Được Sử Dụng](#freertos-components-được-sử-dụng)
3. [Chi Tiết Từng Thành Phần](#chi-tiết-từng-thành-phần)
4. [Use Case Diagram](#use-case-diagram)
5. [Activity Diagram](#activity-diagram)
6. [Timing Diagram](#timing-diagram)
7. [Luồng Dữ Liệu](#luồng-dữ-liệu)

---

## 🎯 Tổng Quan Hệ Thống

**Smart Light System** là một ứng dụng ESP32-C3 thông minh được điều khiển bởi:
- **Cảm biến ánh sáng (ADC)**: Đo độ sáng môi trường
- **Cảm biến chuyển động (PIR)**: Phát hiện chuyển động
- **Nút bấm (Button)**: Bấm để điều khiển (single/double/long press)
- **LED RGB**: Sáng tự động khi tối hoặc có chuyển động
- **Màn hình OLED**: Hiển thị trạng thái hệ thống

---

## 🔧 FreeRTOS Components Được Sử Dụng

| Component | Kiểu | Mục Đích | Vị Trí |
|-----------|------|---------|--------|
| **Tasks** | 5 tasks | Xử lý các sensor, button, LED controller, OLED display | `main.c` |
| **Queue** | 1 queue (size=10) | Gửi events giữa các tasks | `main.c`, `sensor_system.c` |
| **Mutex** | 1 mutex | Bảo vệ `system_state` | `main.c` |
| **Binary Semaphore** | 1 sem | ISR → Button Handler (debounce) | `main.c` |
| **Software Timer** | 1 timer (one-shot) | Tắt LED sau 10 giây | `main.c` |
| **Event Group** | 1 event group | Synchronize init (OLED + sensors) | `main.c` |
| **Task Notifications** | 1 notification | Wake up OLED display mode | `oled_manager.c` |

---

## 🎯 Chi Tiết Từng Thành Phần

### 1️⃣ **TASKS** (5 tasks)

#### Task 1: `task_button_handler` (Priority: 10)
```
┌─────────────────────────────────────────────────────────────┐
│ Chức Năng: Xử lý nút bấm với state machine                 │
├─────────────────────────────────────────────────────────────┤
│ Đầu Vào: ISR → button_press_sem (semaphore)                │
│ Xử Lý:   State machine với 4 trạng thái:                   │
│          - IDLE → DEBOUNCING → PRESSED → DOUBLE_PRESS_WAIT│
│ Đầu Ra:  Queue event (SINGLE/DOUBLE/LONG press)           │
│                                                              │
│ Timing:  - DEBOUNCE: 50ms                                   │
│          - LONG_PRESS: 1500ms                              │
│          - DOUBLE_PRESS_WINDOW: 400ms                       │
└─────────────────────────────────────────────────────────────┘
```

**Quá trình:**
1. ISR phát hiện button press → trao semaphore
2. Task wake up từ `xSemaphoreTake(button_press_sem, portMAX_DELAY)`
3. Debounce 50ms để loại nhiễu
4. Phân biệt: single press, double press, hoặc long press
5. Gửi event tương ứng vào queue

**State Machine:**
```
IDLE 
  ↓ (ISR: Semaphore)
DEBOUNCING (50ms)
  ↓ (Button still pressed)
PRESSED (check hold time)
  ├─ Hold > 1500ms → LONG_PRESS event → IDLE
  └─ Released → DOUBLE_PRESS_WAIT
       ↓ (wait 400ms)
       ├─ 2nd press → DOUBLE_PRESS event → IDLE
       └─ Timeout → SINGLE_PRESS event → IDLE
```

---

#### Task 2: `task_motion_sensor` (Priority: 6)
```
┌─────────────────────────────────────────────────────────────┐
│ Chức Năng: Phát hiện chuyển động từ PIR                    │
├─────────────────────────────────────────────────────────────┤
│ Đầu Vào: GPIO3 (MOTION_SENSOR_PIN)                         │
│ Xử Lý:   Poll rising edge (LOW → HIGH)                     │
│ Đầu Ra:  Queue event (EVENT_MOTION)                        │
│ Chu Kỳ:  Poll mỗi 200ms                                    │
└─────────────────────────────────────────────────────────────┘
```

**Quá trình:**
1. Đọc GPIO3 mỗi 200ms
2. Phát hiện rising edge (last_state=0, current_state=1)
3. Gửi EVENT_MOTION vào queue
4. LED controller sẽ bật LED nếu ở AUTO mode

---

#### Task 3: `task_light_sensor` (Priority: 7)
```
┌─────────────────────────────────────────────────────────────┐
│ Chức Năng: Đọc cảm biến ánh sáng (LDR qua ADC)             │
├─────────────────────────────────────────────────────────────┤
│ Đầu Vào: ADC Channel 0 (LIGHT_SENSOR_PIN)                  │
│ Xử Lý:   - Đọc ADC raw value (0-4095)                      │
│          - Convert → brightness 0-100%                      │
│          - Protect system_state với MUTEX                  │
│          - Gửi EVENT_LIGHT (1=dark, 0=bright)             │
│ Chu Kỳ:  Mỗi 2 giây                                        │
└─────────────────────────────────────────────────────────────┘
```

**Quá trình:**
1. Đọc ADC
2. Convert: `brightness = 100 - ((raw * 100) / 4095)`
3. Lấy MUTEX, cập nhật `system_state.light_level`
4. Nhả MUTEX
5. Gửi EVENT_LIGHT:
   - `value=1` nếu brightness < 50% (tối)
   - `value=0` nếu brightness ≥ 50% (sáng)

---

#### Task 4: `task_led_controller` (Priority: 8)
```
┌─────────────────────────────────────────────────────────────┐
│ Chức Năng: Điều khiển LED dựa trên events từ queue        │
├─────────────────────────────────────────────────────────────┤
│ Đầu Vào: Queue events (MOTION, LIGHT, LIGHT_TIMEOUT, etc) │
│ Xử Lý:   State machine xử lý từng loại event              │
│          - EVENT_MOTION: Bật LED (if AUTO + dark)          │
│          - EVENT_LIGHT: Bật/tắt LED theo ánh sáng         │
│          - EVENT_MOTION_TIMEOUT: Tắt LED                  │
│          - EVENT_SINGLE_PRESS: Toggle (if MANUAL)         │
│          - EVENT_DOUBLE_PRESS: Switch AUTO/MANUAL          │
│ Đầu Ra:  GPIO1 (LED_PIN)                                   │
│          Cập nhật system_state.led_state                   │
│          Reset/stop led_off_timer                           │
└─────────────────────────────────────────────────────────────┘
```

**Quá trình:**
1. Chờ event từ queue (blocking: `xQueueReceive`)
2. Lấy MUTEX để access system_state
3. Xử lý event:
   - `EVENT_MOTION`: 
     - Nếu AUTO mode: increment motion_count, bật LED, reset timer
   - `EVENT_LIGHT`:
     - value=1 (dark): Bật LED + reset timer
     - value=0 (bright): Tắt LED + stop timer
   - `EVENT_MOTION_TIMEOUT`: Tắt LED (timer hết)
   - `EVENT_DOUBLE_PRESS`: Switch AUTO ↔ MANUAL
   - `EVENT_SINGLE_PRESS`: Toggle LED (nếu MANUAL)
   - `EVENT_LONG_PRESS`: Notify OLED task (diagnostic)
4. Nhả MUTEX

---

#### Task 5: `task_oled_display` (Priority: 2)
```
┌─────────────────────────────────────────────────────────────┐
│ Chức Năng: Hiển thị trạng thái hệ thống trên OLED         │
├─────────────────────────────────────────────────────────────┤
│ Đầu Vào: system_state (protected by MUTEX)                 │
│          Task Notification từ button (LONG_PRESS)           │
│ Xử Lý:   - Chờ notification (timeout 500ms)                │
│          - Toggle diagnostic_mode                          │
│          - Lấy MUTEX, đọc system_state                     │
│          - Hiển thị dữ liệu lên OLED                       │
│ Chu Kỳ:  500ms (hoặc khi nhận notification)               │
│ Display: Mode, Light%, Motion Count, LED Status             │
│          (Diagnostic: Light%, Motion, Heap Size)            │
└─────────────────────────────────────────────────────────────┘
```

**Normal Mode Display:**
```
Mode: AUTO
Light: 45%
Motion: 2
LED: ON
```

**Diagnostic Mode Display:**
```
*DIAGNOSTIC MODE*
Light Val: 45
Motion Cnt: 2
Heap: 234567
```

---

### 2️⃣ **QUEUE** - `system_event_queue`

**Định nghĩa:**
```c
QueueHandle_t system_event_queue = xQueueCreate(10, sizeof(event_message_t));
```

**Kích thước:** 10 phần tử, mỗi phần tử = `event_message_t` (enum type + int value)

**Event Types:**
```
EVENT_SINGLE_PRESS    → Button: single click
EVENT_DOUBLE_PRESS    → Button: double click
EVENT_LONG_PRESS      → Button: long press (≥1.5s)
EVENT_MOTION          → PIR: motion detected
EVENT_MOTION_TIMEOUT  → Timer: auto-off timeout
EVENT_LIGHT           → Light Sensor: brightness changed
```

**Producers (gửi event):**
- `task_button_handler` → SINGLE/DOUBLE/LONG_PRESS
- `task_motion_sensor` → MOTION
- `task_light_sensor` → LIGHT
- `led_off_timer_callback` → MOTION_TIMEOUT

**Consumer (nhận event):**
- `task_led_controller` (blocking receive)

**Flow:**
```
Sensors/Button ──[xQueueSend]──→ system_event_queue
                                        ↓
                          [xQueueReceive]
                                        ↓
                          task_led_controller
```

---

### 3️⃣ **MUTEX** - `g_state_mutex`

**Định nghĩa:**
```c
SemaphoreHandle_t g_state_mutex = xSemaphoreCreateMutex();
```

**Bảo vệ:** `system_state_t system_state`

```c
typedef struct {
    uint32_t light_level;      // ADC brightness 0-100%
    uint32_t motion_count;     // Counter motion events
    bool led_state;            // LED ON/OFF
    system_mode_t mode;        // AUTO or MANUAL
    TickType_t last_motion_time; // Timestamp of last motion
} system_state_t;
```

**Các Tasks Sử Dụng:**
- `task_light_sensor`: Cập nhật `light_level`
- `task_motion_sensor`: (read-only, không lock trong version hiện tại)
- `task_led_controller`: Cập nhật `motion_count`, `led_state`, `mode`
- `task_oled_display`: Đọc toàn bộ system_state

**Pattern:**
```c
if (xSemaphoreTake(g_state_mutex, portMAX_DELAY) == pdTRUE) {
    // Protected section
    system_state.light_level = value;
    xSemaphoreGive(g_state_mutex);
}
```

**Tại sao cần MUTEX:**
- Tránh race condition (nhiều tasks access cùng lúc)
- Light sensor update light_level, OLED display read light_level
- LED controller update led_state, OLED display read led_state

---

### 4️⃣ **BINARY SEMAPHORE** - `button_press_sem`

**Định nghĩa:**
```c
SemaphoreHandle_t button_press_sem = xSemaphoreCreateBinary();
```

**Mục Đích:** ISR → Task handoff (từ ngắt đến task xử lý)

**Sơ đồ:**
```
┌──────────────────┐
│   Button Press   │ (GPIO Interrupt)
└────────┬─────────┘
         │
    ┌────▼──────────────┐
    │ button_isr_handler│ (ISR routine)
    │ - Set semaphore  │
    │ - Fast & simple  │
    └────┬─────────────┘
         │
    ┌────▼────────────────────────────┐
    │ task_button_handler             │
    │ - xSemaphoreTake() wake up      │
    │ - Run debounce & state machine  │
    │ - Send event to queue           │
    └─────────────────────────────────┘
```

**Pattern:**
```c
// ISR (IRAM_ATTR)
void IRAM_ATTR button_isr_handler(void* arg) {
    xSemaphoreGiveFromISR(button_press_sem, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

// Task
void task_button_handler(void *pvParameters) {
    while (1) {
        xSemaphoreTake(button_press_sem, portMAX_DELAY); // Wait
        // Process press...
    }
}
```

**Lợi Ích:**
- ISR giữ ngắn (chỉ trao semaphore)
- Debounce logic ở task level (không trong ISR)
- Tránh watchdog timeout do ISR quá lâu

---

### 5️⃣ **SOFTWARE TIMER** - `led_off_timer`

**Định nghĩa:**
```c
led_off_timer = xTimerCreate(
    "LED_Off_Timer", 
    pdMS_TO_TICKS(10000),  // 10 seconds
    pdFALSE,               // One-shot (not recurring)
    (void *)0,             // Timer ID
    led_off_timer_callback // Callback
);

void led_off_timer_callback(TimerHandle_t xTimer) {
    event_message_t msg = { .type = EVENT_MOTION_TIMEOUT };
    xQueueSend(system_event_queue, &msg, 0);
}
```

**Chức Năng:** Auto-off LED sau 10 giây nếu có motion hoặc darkness

**Sơ đồ Timing:**
```
Time: 0s                           10s
      │                            │
      ├─ Motion detected ──────────┤
      │   LED ON                   │
      │   xTimerReset()            │
      │                    Timer expire
      │                     ↓
      │                 EVENT_MOTION_TIMEOUT
      │                 → LED OFF
      │
      ├─ (Nếu motion lại trong 10s)
      │   xTimerReset() → timer reset
      │   10s counter restart
```

**API Sử Dụng:**
- `xTimerReset()`: Reset timer (restart countdown từ 0)
- `xTimerStop()`: Stop timer (when light becomes bright)
- Callback gửi event vào queue (không xử lý trong callback)

---

### 6️⃣ **EVENT GROUP** - `g_system_event_group`

**Định nghĩa:**
```c
EventGroupHandle_t g_system_event_group = xEventGroupCreate();

#define BIT_OLED_INIT_OK    (1 << 0)
#define BIT_SENSORS_INIT_OK (1 << 1)
```

**Mục Đích:** Synchronize khởi tạo hệ thống

**Sơ đồ:**
```
app_main()
    │
    ├─ xEventGroupCreate() → g_system_event_group
    ├─ init_hardware() 
    │  └─ xEventGroupSetBits(..., BIT_SENSORS_INIT_OK)
    ├─ init_oled_i2c()
    ├─ Create tasks:
    │  └─ task_oled_display
    │     └─ xEventGroupSetBits(..., BIT_OLED_INIT_OK)
    │
    └─ xEventGroupWaitBits()
       └─ Wait for BIT_OLED_INIT_OK | BIT_SENSORS_INIT_OK
          (Blocking until both ready)
          
       ✓ Both ready → print "Smart Light System Started!"
```

**Pattern:**
```c
// Producer (in init function)
xEventGroupSetBits(g_system_event_group, BIT_SENSORS_INIT_OK);

// Consumer (in app_main)
EventBits_t bits = xEventGroupWaitBits(
    g_system_event_group,
    BIT_OLED_INIT_OK | BIT_SENSORS_INIT_OK,
    pdTRUE,           // Clear bits on exit
    pdTRUE,           // Wait for ALL bits
    portMAX_DELAY     // Block until ready
);
```

---

### 7️⃣ **TASK NOTIFICATIONS** - `g_oled_task_handle`

**Định nghĩa:**
```c
TaskHandle_t g_oled_task_handle = NULL;
// Set when creating OLED task:
xTaskCreate(task_oled_display, "OLED Display", 4096, NULL, 2, &g_oled_task_handle);
```

**Mục Đích:** Wake up OLED task để toggle diagnostic mode

**Sơ đồ:**
```
task_button_handler
    │ (LONG_PRESS detected)
    └─ xTaskNotifyGive(g_oled_task_handle)
           │
           ↓
task_oled_display
    │ (Waiting on notification)
    └─ ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500))
           │ (Immediate wake-up!)
           ├─ diagnostic_mode = !diagnostic_mode
           └─ Display diagnostic info
```

**Pattern:**
```c
// Sender (button handler)
xTaskNotifyGive(g_oled_task_handle);  // Wake up immediately

// Receiver (OLED task)
if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) > 0) {
    diagnostic_mode = !diagnostic_mode;
}
```

**Lợi Ích:**
- Lightweight (so với queue)
- Direct task-to-task notification
- Thích hợp cho inter-task signaling đơn giản

---

## 📊 Use Case Diagram

```
                              ┌─────────────────────┐
                              │   Smart Light       │
                              │   System            │
                              └────────────────────┐
                                                    │
        ┌──────────────────────────────────────────┼──────────────────────────┐
        │                                           │                          │
        │                                           │                          │
    ┌───▼─────┐                         ┌──────────▼──────────┐    ┌──────────▼────┐
    │  User   │                         │   FreeRTOS Kernel  │    │  Environment  │
    └────┬────┘                         └────────────────────┘    └─────┬─────────┘
         │                                                               │
         ├── Use Case 1: Press Button ◄─────────────────────────────────├──
         │   ├─ Single Press
         │   ├─ Double Press
         │   └─ Long Press (Diagnostic)
         │
         ├── Use Case 2: Detect Motion ◄─────────────────────────────────├──
         │   └─ Wake up LED Controller
         │
         ├── Use Case 3: Read Light Level ◄─────────────────────────────┤──
         │   └─ Automatically adjust LED
         │
         ├── Use Case 4: Control LED ◄──────────────────────────────────┤──
         │   ├─ Auto-on (dark + motion/low-light)
         │   └─ Auto-off (bright or timer expire)
         │
         ├── Use Case 5: Switch Mode ◄───────────────────────────────────┤──
         │   ├─ AUTO: Based on sensors
         │   └─ MANUAL: User control
         │
         └── Use Case 6: View System Status ◄──────────────────────────────┤──
             └─ OLED Display (Normal & Diagnostic)


┌─────────────────────────────────────────────────────┐
│ FreeRTOS PRIMITIVES USED                            │
├─────────────────────────────────────────────────────┤
│ ✓ 5 Tasks (concurrent execution)                    │
│ ✓ 1 Queue (event communication)                     │
│ ✓ 1 Mutex (state protection)                        │
│ ✓ 1 Binary Semaphore (ISR handoff)                  │
│ ✓ 1 Software Timer (auto-off)                       │
│ ✓ 1 Event Group (init sync)                         │
│ ✓ 1 Task Notification (OLED wake)                   │
└─────────────────────────────────────────────────────┘
```

---

## 🔄 Activity Diagram - Main LED Control Flow

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    LED Control Activity Diagram                         │
└─────────────────────────────────────────────────────────────────────────┘

                        ┌───────────────────────────┐
                        │   task_led_controller     │
                        │   Wait on Queue           │
                        └────────────┬──────────────┘
                                     │
                    ┌────────────────┼────────────────┐
                    │                │                │
                    │                │                │
         ┌──────────▼─────────┐   ┌──▼──────────────┐   ┌──▼──────────────┐
         │   EVENT_MOTION     │   │  EVENT_LIGHT    │   │  EVENT_TIMEOUT   │
         └──────────┬─────────┘   └──┬──────────────┘   └──┬──────────────┘
                    │                │                     │
         ┌──────────▼─────────┐   ┌──▼──────────────┐   ┌──▼──────────────┐
         │ Get Mutex          │   │ Get Mutex       │   │ Get Mutex        │
         └──────────┬─────────┘   └──┬──────────────┘   └──┬──────────────┘
                    │                │                     │
         ┌──────────▼─────────┐   ┌──▼──────────────┐   ┌──▼──────────────┐
         │ mode == AUTO?      │   │ mode == AUTO?   │   │ mode == AUTO?    │
         └─────┬──────┬───────┘   └─────┬──┬──────┘   └────────┬─────────┘
             YES│      │NO            YES│ │NO                 │YES
               │       └─────────────────┼─┘────────────────┐   │
               │                        │ (Skip)           │   │
         ┌─────▼──────────────┐   ┌─────▼────────┐   ┌────▼────▼───┐
         │ motion_count++     │   │ value == 1?  │   │ LED = OFF    │
         │ last_motion_time   │   │ (dark)       │   │ led_state=0  │
         └──────────┬─────────┘   └─────┬─────┬──┘   │ Stop timer   │
                    │                  YES│   │NO    └────┬────────┘
         ┌──────────▼──────────┐   ┌─────▼──┐ ┌─▼────┐
         │ light < 50?         │   │LED=ON?─┘ │LED=0?│
         │ (is dark)           │   │ YES   NO │ NO   │
         └────┬──────┬─────────┘   │   │ ┌─────┘ └────┐
            YES│      │NO          │   ├─┤            │
              │       └─────────┐  │   │ └──────┬─────┘
         ┌────▼──────┐    ┌─────▼──▼──┐  │ ┌────▼────┐
         │LED = ON   │    │(no change)│  │ │(skip)   │
         │led_state=1│    └────┬──────┘  │ └────┬────┘
         │Reset timer│         │        │      │
         └────┬──────┘         │        │      │
              └────────────────┼────────┼──────┘
                               │        │
                      ┌────────▼────────▼──┐
                      │ Release Mutex      │
                      └─────────┬──────────┘
                                │
                      ┌─────────▼──────────┐
                      │ Continue loop      │
                      │ Wait on queue      │
                      └────────────────────┘
```

---

## 📈 Activity Diagram - Button Debounce State Machine

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    Button Handler State Machine                         │
└─────────────────────────────────────────────────────────────────────────┘

         ┌──────────────────────────────────────────────┐
         │             IDLE State                       │
         │  Wait indefinitely on semaphore             │
         │  xSemaphoreTake(button_press_sem, ∞)        │
         └────────────┬─────────────────────────────────┘
                      │ (ISR: button pressed)
                      │ Semaphore given
         ┌────────────▼─────────────────────────────────┐
         │         DEBOUNCING State                     │
         │  vTaskDelay(50ms) - wait for settling       │
         └────────┬───────────────────┬─────────────────┘
                  │                   │
       button=0   │                   │  button=1
      (pressed)   │                   │  (released)
                  │                   │
         ┌────────▼──────────┐  ┌─────▼────────────────┐
         │  PRESSED State    │  │ Back to IDLE         │
         │  Check hold time  │  │ (it was noise)       │
         │  for long press   │  └─────────────────────┘
         └────┬────────┬─────┘
              │        │
         >1500ms│       │button released
              │        │
        ┌─────▼──┐   ┌─▼─────────────────────┐
        │Long    │   │ DOUBLE_PRESS_WAIT     │
        │Press   │   │ xSemaphoreTake(400ms) │
        │Event   │   └──┬───┬────────────────┘
        │Sent    │      │   │
        └────┬───┘      │   │
             │     YES  │   │ NO (timeout)
             │  2ndPress│   │
        ┌────▼──────┐ ┌─▼───▼──────┐
        │ Double    │ │ Single     │
        │ Press     │ │ Press      │
        │ Event     │ │ Event      │
        │ Sent      │ │ Sent       │
        └────┬──────┘ └─┬──────────┘
             │         │
             └────┬────┘
                  │
         ┌────────▼──────────┐
         │  Return to IDLE   │
         └───────────────────┘


TIMING DETAILS:
══════════════════════════════════════════════════════════════

Time (ms):  0        50       400              1500
           ┌────────┬──────────────────────────────┐
Button: ┌──┤        │         │                    │ ◄─── still pressed
         └──────────┘─────────────────────────────┘
           Press    Debounce   DOUBLE_PRESS_WAIT  LONG_PRESS
           ISR      complete   expires            detected
```

---

## ⏱️ Timing Diagram - Complete System Timing

```
┌─────────────────────────────────────────────────────────────────────────┐
│               Timing Diagram - Real System Behavior                     │
└─────────────────────────────────────────────────────────────────────────┘

Timeline (seconds): 0    2    4    6    8   10   12   14   16   18   20
                   ├────┼────┼────┼────┼────┼────┼────┼────┼────┼────┤

Light Sensor Poll  │    ▼    │    ▼    │    ▼    │    ▼    │    ▼    │
(2s interval)      ├────◄────┼────◄────┼────◄────┼────◄────┼────◄────┤
Light Level: 30%   │ Dark    │ Dark    │ Bright  │ Bright  │ Bright  │
(dark)             │         │         │ (90%)   │ (92%)   │ (88%)   │

Motion Sensor Poll │    ▼         ▼    │    ▼    │    ▼    │         │
(200ms interval)   ├────◄────────◄────┼────◄────┼────◄────┼────────┤
                   │ (motion at 4s)    │         │         │        │

LED Controller     │              │         │    │         │        │
Events/Queue       ├──────────────┼─────────┼────┼─────────┼────────┤
EVENT_LIGHT (dark) │         ◄────┘                                  │
EVENT_MOTION       │              ◄─────────────────────────────┘   │
EVENT_LIGHT(bright)│                        ◄───────────────────┘  │

LED State          │              │   ┌─────────┐    ┌──────┐      │
                   ├──────────────┤   │         │    │      │      │
ON:════════════════┤     OFF      ├═══╧═════════╪════╧══════╪══════┤
                   │              │  (on by     │  (on by   │
                   │              │   light)    │   motion) │
                   
Timer State        │              │              │           │      │
                   ├──────────────┼──────────────┼───────────┼──────┤
RUNNING:═══════════╪══════════════╪═════════════►│  STOPPED  │ RUN ►
                   │              │ (reset @4s)  │ (bright)  │     
                   │              │              │           │
                   
OLED Display       │              │              │           │      │
(500ms updates)    ├─◄─────◄──────◄──────────────◄──────────┤◄─────┤
Mode: AUTO         │ Light: 30%  │ Light: 90%   │ Light: 88%│      │
Light: 30%         │ Motion: 0   │ Motion: 1    │ Motion: 1 │      │
Motion: 0          │ LED: ON     │ LED: ON      │ LED: OFF  │      │
LED: OFF           │             │              │           │      │


KEY EVENTS TIMELINE:
═══════════════════════════════════════════════════════════════════

t=0s:
  • System starts
  • Event Group waits for init bits
  • All tasks created and running

t=2s:
  • Light Sensor: reads ADC (30% - dark)
  • Sends EVENT_LIGHT (value=1, dark)
  • LED Controller receives → LED ON + start timer

t=4s:
  • Motion Sensor: detects motion (rising edge)
  • Sends EVENT_MOTION
  • LED Controller: already ON, but increments motion_count, resets timer

t=6s:
  • Light Sensor: reads ADC (90% - bright)
  • Sends EVENT_LIGHT (value=0, bright)
  • LED Controller receives → LED OFF + stop timer

t=10s:
  • If motion again at t=8s
  • Timer would expire at t=18s (8s + 10s)
  • But light is bright, so LED already OFF
  • Timer gets stopped

t=12-20s:
  • System continues monitoring
  • OLED updates every 500ms
  • No motion, light bright → LED OFF
```

---

## 📡 Luồng Dữ Liệu (Data Flow)

```
┌──────────────────────────────────────────────────────────────────────┐
│                          DATA FLOW DIAGRAM                           │
└──────────────────────────────────────────────────────────────────────┘

SENSORS (Input)
═══════════════════════════════════════════════════════════════════════

  ┌─────────────────┐        ┌──────────────────────┐
  │  GPIO2: Button  │        │  GPIO3: Motion (PIR) │
  │  (BUTTON_PIN)   │        │ (MOTION_SENSOR_PIN)  │
  └────────┬────────┘        └──────────┬───────────┘
           │                            │
     [ISR: button_isr_handler]   [Polling: 200ms]
           │                            │
    ┌──────▼────────────┐      ┌────────▼──────────┐
    │ Semaphore Give    │      │ task_motion_sensor│
    │ (button_press_sem)│      └────────┬──────────┘
    └──────┬────────────┘               │
           │                    ┌───────▼─────────────┐
           └────────┬───────────┤ EVENT_MOTION        │
                    │           │ → system_event_queue│
                    │           └─────────────────────┘
                    │
           ┌────────▼────────────────────┐
           │  task_button_handler        │
           │  (xSemaphoreTake)           │
           │  State machine:             │
           │  - Debounce 50ms            │
           │  - Single/Double/Long press │
           └────────┬────────────────────┘
                    │
           ┌────────▼──────────────────────┐
           │ EVENT_SINGLE/DOUBLE/LONG_PRESS│
           │ → system_event_queue          │
           └───────────────────────────────┘

  ┌──────────────────────────┐
  │  ADC0: Light Sensor (LDR)│
  │  (LIGHT_SENSOR_PIN)      │
  └────────┬─────────────────┘
           │
     [Polling: 2s]
           │
    ┌──────▼──────────────────────┐
    │ task_light_sensor           │
    │ - Read ADC (0-4095)         │
    │ - Convert 0-100%            │
    │ - Get MUTEX                 │
    │ - Update system_state.      │
    │   light_level              │
    │ - Release MUTEX            │
    └──────┬─────────────────────┘
           │
    ┌──────▼──────────────────────┐
    │ EVENT_LIGHT (value=1 or 0)  │
    │ → system_event_queue        │
    └─────────────────────────────┘


PROCESSING (FreeRTOS Queue)
═══════════════════════════════════════════════════════════════════════

    ┌──────────────────────┐
    │  system_event_queue  │
    │  (size = 10)         │
    │                      │
    │ [SINGLE_PRESS    ]   │
    │ [DOUBLE_PRESS    ]   │
    │ [LONG_PRESS      ]   │
    │ [MOTION          ]   │
    │ [LIGHT           ]   │
    │ [MOTION_TIMEOUT  ]   │
    │ ...                  │
    └──────────┬───────────┘
               │
               │ xQueueReceive (blocking)
               │
    ┌──────────▼──────────────────────┐
    │  task_led_controller            │
    │  - Blocking on queue            │
    │  - Get MUTEX                    │
    │  - Process event based on mode  │
    │    (AUTO/MANUAL)                │
    │  - Update GPIO1 (LED)           │
    │  - Update system_state          │
    │  - Manage timer                 │
    │  - Release MUTEX                │
    └──────────┬─────────────────────┘


TIMER (Peripheral)
═══════════════════════════════════════════════════════════════════════

    ┌──────────────────────────┐
    │  led_off_timer           │
    │  - One-shot, 10 seconds  │
    │  - Start on LED ON       │
    │  - Reset on motion       │
    │  - Stop on bright light  │
    └──────────┬───────────────┘
               │ (Expiry)
    ┌──────────▼──────────────────────┐
    │  led_off_timer_callback         │
    │  - Send EVENT_MOTION_TIMEOUT    │
    │  - → system_event_queue         │
    └─────────────────────────────────┘


SHARED STATE (Protected by MUTEX)
═══════════════════════════════════════════════════════════════════════

    ┌────────────────────────────────┐
    │  system_state (MUTEX protect)  │
    │                                │
    │  - light_level (0-100%)        │◄── Updated by light_sensor
    │  - motion_count                │◄── Updated by LED controller
    │  - led_state (bool)            │◄── Updated by LED controller
    │  - mode (AUTO/MANUAL)          │◄── Updated by LED controller
    │  - last_motion_time            │◄── Updated by LED controller
    └────────────────┬───────────────┘
                     │ (xSemaphoreTake)
                     │ (Protected reads)
                     │
    ┌────────────────▼────────────────┐
    │  task_oled_display              │
    │  - Refresh every 500ms or       │
    │  - Wake up on notification      │
    │  - Toggle diagnostic mode       │
    │  - Display on SSD1306           │
    └─────────────────────────────────┘


OUTPUT (Display & Control)
═══════════════════════════════════════════════════════════════════════

    ┌──────────────────────────┐
    │  GPIO1: LED              │
    │  (LED_PIN)               │
    │                          │
    │  Set by: task_led_ctrl   │
    │  Logic: 1 = ON, 0 = OFF  │
    └──────────────────────────┘

    ┌──────────────────────────┐
    │  I2C Bus                 │
    │  SDA=GPIO5, SCL=GPIO4    │
    │                          │
    │  Address: 0x3C (SSD1306) │
    └──────────┬───────────────┘
               │ (Display commands)
    ┌──────────▼────────────────┐
    │  OLED Display (128x64)    │
    │  - Mode (AUTO/MANUAL)     │
    │  - Light: xx%             │
    │  - Motion: xx             │
    │  - LED: ON/OFF            │
    │  (or Diagnostic info)     │
    └────────────────────────────┘
```

---

## 🎓 Tóm Tắt Sử Dụng FreeRTOS

| Primitive | Số Lượng | Chức Năng | Ứng Dụng |
|-----------|----------|----------|---------|
| **Tasks** | 5 | Concurrent processing | Button, Motion, Light, LED, OLED |
| **Queue** | 1 | Event communication | Inter-task messaging |
| **Mutex** | 1 | Resource protection | Protect system_state |
| **Binary Semaphore** | 1 | ISR-to-Task sync | Button ISR → Handler task |
| **Software Timer** | 1 | Delayed action | Auto-off LED after 10s |
| **Event Group** | 1 | Multi-bit signaling | Synchronize initialization |
| **Task Notification** | 1 | Direct signaling | Diagnostic mode toggle |

---

## 💡 Lợi Ích Của Kiến Trúc FreeRTOS

✅ **Concurrency**: 5 tasks chạy song song, xử lý sensors, button, LED, display độc lập
✅ **Real-time**: Các event được xử lý nhanh (không polling thay vì event-driven)
✅ **ISR efficiency**: Button debounce ở task level, không trong ISR (tránh watchdog)
✅ **Data safety**: MUTEX bảo vệ system_state khỏi race conditions
✅ **Responsiveness**: Queue + Tasks + Notifications làm hệ thống phản ứng nhanh
✅ **Scalability**: Dễ thêm tính năng mới (thêm task/event)
✅ **Low latency**: Priority levels (task scheduling) cho các hoạt động quan trọng

---

*Generated: Smart Light System FreeRTOS Analysis*
*Version: 1.0*
