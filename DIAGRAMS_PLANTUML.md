# PlantUML Diagrams - Smart Light System

## Use Case Diagram

```plantuml
@startuml SmartLight_UseCase
!define PUML_COLOR_USER_CLASS #FFD700
!define PUML_COLOR_SYSTEM_CLASS #87CEEB
!define PUML_COLOR_DATA_CLASS #90EE90
!define PUML_COLOR_ACTOR_CLASS #FFB6C1

actor User
actor Environment

rectangle SmartLightSystem {
    usecase UC1 as "Control LED\n(Manual/Auto)"
    usecase UC2 as "Detect Motion"
    usecase UC3 as "Measure Light Level"
    usecase UC4 as "Press Button"
    usecase UC5 as "Display Status"
    usecase UC6 as "Auto-off after 10s"
    usecase UC7 as "Switch AUTO/MANUAL"
    usecase UC8 as "View Diagnostics"
    
    UC1 --> UC6 : includes
    UC1 --> UC7 : extends
    UC3 --> UC1 : triggers
    UC2 --> UC1 : triggers
    UC4 --> UC7 : triggers
    UC4 --> UC8 : long press
    UC5 --> UC1 : displays
}

User -->|Single/Double/Long Press| UC4
Environment -->|Darkness/Motion| UC3
Environment -->|Darkness/Motion| UC2
UC1 -->|Bật/Tắt| User
UC5 -->|Hiển thị| User

@enduml
```

## Sequence Diagram - Motion Detection Flow

```plantuml
@startuml MotionDetectionFlow
participant "PIR Sensor\n(GPIO3)" as PIR
participant "task_motion_sensor\n(6s priority)" as MotionTask
participant "system_event_queue\n(10 slots)" as Queue
participant "task_led_controller\n(8s priority)" as LEDTask
participant "GPIO1\n(LED)" as LED

== Initialization ==
activate MotionTask
MotionTask -> MotionTask: last_motion_state = 0

== Main Loop ==
loop Every 200ms
    MotionTask -> PIR: gpio_get_level(GPIO3)
    PIR --> MotionTask: 0 (no motion)
    MotionTask -> MotionTask: Check rising edge\nLast=0, Current=0\nNo edge detected
end

== Motion Triggered ==
PIR ->> MotionTask: Sensor HIGH (motion)
MotionTask -> PIR: gpio_get_level(GPIO3)
PIR --> MotionTask: 1 (motion detected)

MotionTask -> MotionTask: Detect rising edge\nLast=0, Current=1\n✓ Edge detected!

MotionTask -> Queue: xQueueSend(EVENT_MOTION)
activate Queue
Note right of Queue: EVENT_MOTION added\nQueue depth: 1/10

MotionTask ->> MotionTask: Log: "Motion detected"

== LED Controller Processes ==
activate LEDTask
LEDTask -> Queue: xQueueReceive(blocking)
Queue -> LEDTask: EVENT_MOTION

LEDTask -> LEDTask: Get Mutex\n(g_state_mutex)
Note right of LEDTask: Protected section

LEDTask -> LEDTask: if mode == AUTO:\n  motion_count++\n  last_motion_time = NOW

LEDTask -> LED: gpio_set_level(LED_PIN, 1)
activate LED
LED ->> LED: ⚡ LED turns ON

LEDTask -> LEDTask: led_state = true\nxTimerReset(10s)

LEDTask -> LEDTask: Release Mutex

== Next Loop ==
MotionTask -> PIR: gpio_get_level(GPIO3)
PIR --> MotionTask: (still detecting)

== After 10 seconds (if no light change) ==
LEDTask -> LEDTask: Timer expires!

LEDTask -> Queue: xQueueSend(EVENT_MOTION_TIMEOUT)
activate Queue
Note right of Queue: EVENT_MOTION_TIMEOUT\nQueue depth: 1/10

LEDTask -> Queue: xQueueReceive()
Queue -> LEDTask: EVENT_MOTION_TIMEOUT

LEDTask -> LED: gpio_set_level(LED_PIN, 0)
LED ->> LED: 🌑 LED turns OFF

deactivate LED

@enduml
```

## Sequence Diagram - Button Press Detection

```plantuml
@startuml ButtonPressFlow
participant "Button\n(GPIO2)" as Button
participant "button_isr_handler\n(ISR)" as ISR
participant "button_press_sem\n(Binary Sem)" as Sem
participant "task_button_handler\n(10s priority)" as BtnTask
participant "system_event_queue" as Queue

== ISR Setup ==
Button ->> ISR: Falling edge (press)
activate ISR
ISR -> Sem: xSemaphoreGiveFromISR()
Note right of Sem: Semaphore = Given
ISR -> ISR: portYIELD_FROM_ISR()
deactivate ISR

== Button Handler Task Wakes ==
activate BtnTask
BtnTask -> BtnTask: State: IDLE
BtnTask -> Sem: xSemaphoreTake(∞)\nWAITING...
Note right of BtnTask: Task blocked

Sem --> BtnTask: Semaphore acquired!\nTask wakes up

BtnTask -> BtnTask: State → DEBOUNCING

== Debounce Phase ==
BtnTask -> BtnTask: vTaskDelay(50ms)
Note right of BtnTask: Wait for settling

BtnTask -> Button: gpio_get_level(GPIO2)
Button --> BtnTask: 0 (still pressed)

BtnTask -> BtnTask: State → PRESSED
BtnTask -> BtnTask: last_press_time = NOW

== Press Detection ==
BtnTask -> BtnTask: Wait in PRESSED state

loop Check for long press (every 10ms)
    BtnTask -> Button: gpio_get_level(GPIO2)
    Button --> BtnTask: 0 (still pressed)
    
    alt Long press (>1500ms)?
        BtnTask -> Queue: xQueueSend(EVENT_LONG_PRESS)
        Note right of Queue: Long press detected!
        BtnTask -> BtnTask: Wait for release
        BtnTask -> BtnTask: State → IDLE
    else Button released
        BtnTask -> Button: gpio_get_level(GPIO2)
        Button --> BtnTask: 1 (released)
        BtnTask -> BtnTask: State → DOUBLE_PRESS_WAIT
        
        == Double Click Window ==
        BtnTask -> Sem: xSemaphoreTake(400ms)
        
        alt 2nd press within 400ms?
            BtnTask -> Queue: xQueueSend(EVENT_DOUBLE_PRESS)
            Note right of Queue: Double press!
        else Timeout (400ms passed)
            BtnTask -> Queue: xQueueSend(EVENT_SINGLE_PRESS)
            Note right of Queue: Single press!
        end
        
        BtnTask -> BtnTask: State → IDLE
    end
end

deactivate BtnTask

@enduml
```

## State Machine Diagram - LED Controller

```plantuml
@startuml LEDControllerStateMachine
state LEDController {
    state WaitEvent: "Waiting on Queue\n[xQueueReceive]"
    state GetMutex: "Acquire Mutex\n[xSemaphoreTake]"
    state CheckMode: "Check\nmode == AUTO?"
    
    state HandleMotion: "EVENT_MOTION\n✓ motion_count++\n✓ LED ON\n✓ Timer reset"
    
    state HandleLight: "EVENT_LIGHT\nvalue==1 (dark)?\n[yes] LED ON\n[no] LED OFF"
    
    state HandleTimeout: "EVENT_TIMEOUT\n✓ LED OFF\n✓ Timer stop"
    
    state HandlePress: "EVENT_*_PRESS\n[SINGLE] Toggle\n[DOUBLE] Auto/Manual\n[LONG] Notify OLED"
    
    state ReleaseMutex: "Release Mutex\n[xSemaphoreGive]"
    
    WaitEvent --> GetMutex: Event received
    GetMutex --> CheckMode: Mutex acquired
    
    CheckMode --> HandleMotion: EVENT_MOTION
    CheckMode --> HandleLight: EVENT_LIGHT
    CheckMode --> HandleTimeout: EVENT_TIMEOUT
    CheckMode --> HandlePress: EVENT_*_PRESS
    
    HandleMotion --> ReleaseMutex
    HandleLight --> ReleaseMutex
    HandleTimeout --> ReleaseMutex
    HandlePress --> ReleaseMutex
    
    ReleaseMutex --> WaitEvent: Loop back
}

note right of HandleMotion
    Only if mode == AUTO
    Increment motion counter
    Set LED ON (GPIO=1)
    Reset 10s timer
end note

note right of HandleLight
    Only if mode == AUTO
    Dark (value=1): LED ON
    Bright (value=0): LED OFF
    Also manage timer
end note

@enduml
```

## Component Diagram - System Architecture

```plantuml
@startuml SystemArchitecture
!define RECT_WIDTH 150
!define RECT_HEIGHT 80

component [Button\n(GPIO2)] as BTN
component [Motion PIR\n(GPIO3)] as PIR
component [Light ADC\n(ADC0)] as LDR
component [LED\n(GPIO1)] as LED
component [OLED\nDisplay\n(I2C)] as OLED

component [Button\nISR\nHandler] as BTN_ISR
component [Button\nDebounce\nTask] as BTN_TASK
component [Motion\nSensor\nTask] as MOTION_TASK
component [Light\nSensor\nTask] as LIGHT_TASK
component [LED\nController\nTask] as LED_TASK
component [OLED\nDisplay\nTask] as OLED_TASK

component [system_event_queue\n(10 slots)] as QUEUE
component [g_state_mutex\n(MUTEX)] as MUTEX
component [button_press_sem\n(Binary Sem)] as BTN_SEM
component [g_system_event_group\n(Event Group)] as EG
component [led_off_timer\n(10s, one-shot)] as TIMER

database [system_state\n(Protected)] as STATE

[BTN] --> [BTN_ISR]
[BTN_ISR] --> [BTN_SEM]
[BTN_SEM] --> [BTN_TASK]

[PIR] --> [MOTION_TASK]
[LDR] --> [LIGHT_TASK]

[BTN_TASK] --> [QUEUE]
[MOTION_TASK] --> [QUEUE]
[LIGHT_TASK] --> [QUEUE]
[TIMER] --> [QUEUE]

[QUEUE] --> [LED_TASK]

[LED_TASK] --> [LED]
[LED_TASK] --> [STATE]
[LED_TASK] --> [TIMER]

[LIGHT_TASK] --> [STATE]
[LIGHT_TASK] --> [MUTEX]

[MOTION_TASK] --> [STATE]

[OLED_TASK] --> [STATE]
[OLED_TASK] --> [MUTEX]
[OLED_TASK] --> [OLED]

[BTN_TASK] -.-> [OLED_TASK]

[MOTION_TASK] --> [EG]
[OLED_TASK] --> [EG]

@enduml
```

## Timing Diagram - Event Timeline

```plantuml
@startuml EventTimeline
' Timeline for 20 seconds showing various events

title Smart Light System - Event Timeline (20 seconds)

concise "Light Level" as LIGHT_LEVEL
concise "Motion Detect" as MOTION
concise "LED State" as LED_STATE
concise "Timer State" as TIMER_STATE
concise "OLED Display" as OLED_DISPLAY

LIGHT_LEVEL : 30% (dark)
LIGHT_LEVEL : 30% (dark)
LIGHT_LEVEL : 92% (bright)
LIGHT_LEVEL : 94% (bright)
LIGHT_LEVEL : 30% (dark)

MOTION : no
MOTION : motion detected
MOTION : no
MOTION : motion detected

LED_STATE : OFF
LED_STATE : ON
LED_STATE : OFF
LED_STATE : ON

TIMER_STATE : stopped
TIMER_STATE : counting (10s)
TIMER_STATE : stopped
TIMER_STATE : counting (10s)

OLED_DISPLAY : updating
OLED_DISPLAY : updating
OLED_DISPLAY : normal mode
OLED_DISPLAY : diagnostic

@enduml
```

## Collaboration Diagram - Task Interaction

```plantuml
@startuml TaskCollaboration
participant "Button\nPress" as USER
participant "task_button_handler" as BTN_HANDLER
participant "task_motion_sensor" as MOTION_HANDLER
participant "task_light_sensor" as LIGHT_HANDLER
participant "task_led_controller" as LED_HANDLER
participant "task_oled_display" as OLED_HANDLER
participant "system_event_queue" as QUEUE
participant "system_state" as STATE

USER -> BTN_HANDLER: [1] Button press ISR
BTN_HANDLER -> BTN_HANDLER: [2] Debounce & detect type
BTN_HANDLER -> QUEUE: [3] Send event (SINGLE/DOUBLE/LONG)

MOTION_HANDLER -> MOTION_HANDLER: [4] Poll GPIO every 200ms
MOTION_HANDLER -> QUEUE: [5] Send EVENT_MOTION

LIGHT_HANDLER -> LIGHT_HANDLER: [6] Read ADC every 2s
LIGHT_HANDLER -> STATE: [7] Update light_level (with MUTEX)
LIGHT_HANDLER -> QUEUE: [8] Send EVENT_LIGHT

QUEUE --> LED_HANDLER: [9] Deliver event (blocking recv)
LED_HANDLER -> STATE: [10] Get MUTEX, read mode
LED_HANDLER -> STATE: [11] Update motion_count, led_state
LED_HANDLER -> LED_HANDLER: [12] Set GPIO (LED ON/OFF)
LED_HANDLER -> LED_HANDLER: [13] Manage timer

OLED_HANDLER -> STATE: [14] Periodic read (with MUTEX)
OLED_HANDLER -> OLED_HANDLER: [15] Format display data
OLED_HANDLER -> OLED_HANDLER: [16] Send to I2C/OLED

note over BTN_HANDLER
    LONG_PRESS → 
    xTaskNotifyGive(OLED)
    Wake up immediately!
end note

@enduml
```

## All Event Types & Flow

```plantuml
@startuml EventFlow
:EVENT_SINGLE_PRESS;
:EVENT_DOUBLE_PRESS;
:EVENT_LONG_PRESS;
:EVENT_MOTION;
:EVENT_LIGHT (value=0/1);
:EVENT_MOTION_TIMEOUT;

split
:SINGLE_PRESS;
if (mode == MANUAL?) then (yes)
    :Toggle LED;
else (no)
    :Ignored;
endif
split again
:DOUBLE_PRESS;
    :Switch AUTO ↔ MANUAL;
split again
:LONG_PRESS;
    :Notify OLED (diagnostic);
split again
:MOTION;
if (mode == AUTO?) then (yes)
    if (brightness < 50%?) then (yes - dark)
        :LED ON;
        :motion_count++;
        :Reset timer;
    else (no - bright)
        :Skip;
    endif
else (no)
    :Skip;
endif
split again
:EVENT_LIGHT;
if (value == 1?) then (yes - dark)
    :LED ON;
    :Reset timer;
else (no - bright)
    :LED OFF;
    :Stop timer;
endif
split again
:EVENT_TIMEOUT;
if (mode == AUTO?) then (yes)
    :LED OFF;
    :Stop timer;
else (no)
    :Skip;
endif
end split
:Continue;

@enduml
```

---

*PlantUML Diagrams - Smart Light FreeRTOS System*
*Copy and paste these into PlantUML Online Editor: http://www.plantuml.com/plantuml/uml/*
