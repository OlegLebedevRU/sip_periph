# Refactoring Plan: sip_periph — декомпозиция потоков

**Проект:** sip_periph (STM32F411CEUx, FreeRTOS)
**Дата составления:** 2026-03-22
**Статус:** 🔄 В работе

---

## 1. Контекст и диагностика

### 1.1 Аппаратная платформа

| Компонент | Интерфейс | Назначение |
|-----------|-----------|------------|
| STM32F411CEUx | — | MCU, slave |
| ESP32 (Master) | I2C1 (0x11), IRQ=PB8 | Главный контроллер |
| TCA6408A | I2C2 (0x40) | GPIO expander, источник событий |
| DS3231M | I2C2 (0xD0) | RTC, генерирует 1Hz SQW |
| PN532 | I2C2 (0x48) | NFC reader |
| DWIN | USART2 (115200) | HMI touchscreen |
| Matrix KB | GPIOA/B (ROW/COL) | 4×3 keyboard |
| Wiegand | GPIOC PC14/PC15 | Карточный ридер |
| OLED SSD1306 | SPI1 | Дисплей статуса |
| RELE1 | PB15 | Реле замка |
| BUZZER | PA8 | Зуммер |

### 1.2 Карта входов TCA6408A

```
P0 (0x01) — DS3231M INT/SQW  — 1Hz square wave от RTC-чипа, ДРЕБЕЗГА НЕТ
P2 (0x04) — External Button  — механическая кнопка, ДРЕБЕЗГ + мусорные IRQ
P3 (0x08) — PN532 IRQ        — уровень-LOW от чипа PN532, ДРЕБЕЗГА НЕТ
P1,P4-P7  — не используются  — зарезервированы
```

TCA IRQ → STM32 EXT_INT (PC13, EXTI15_10). Высокое быстродействие не требуется —
FreeRTOS-очередь `myQueueTCA6408Handle` полностью достаточна.

### 1.3 RTOS-объекты (текущие)

**Задачи:**
| Имя | Приоритет | Стек | Файл |
|-----|-----------|------|------|
| defaultTask | Normal | 128 | main.c |
| myTask532 | AboveNormal | 256 | main.c |
| myTaskRxTxI2c1 | AboveNormal | 512 | main.c |
| myTaskOLED | Normal | 256 | main.c |
| myTaskWiegand | Idle | 256 | wiegand.c |
| myTaskHmi | Idle | 1024 | hmi.c |
| myTaskHmiMsg | Idle | 256 | hmi.c |
| myTask_tca6408a | AboveNormal | 256 | main.c |

**Очереди:**
| Имя | Размер | Тип элемента | Назначение |
|-----|--------|--------------|------------|
| myQueueToMaster | 8 | I2cPacketToMaster_t | Пакеты → ESP32 |
| myQueueTCA6408 | 16 | uint16_t | IRQ от TCA |
| myQueueWiegand | 64 | uint8_t | Биты Wiegand |
| myQueueHMIRecvRaw | 32 | MsgUart_t | DWIN пакеты |
| myQueueHmiMsg | 8 | MsgHmi_t | Сообщения → HMI |
| myQueueOLED | 8 | uint16_t | Сигналы → OLED |

**Таймеры:**
| Имя | Тип | Callback | Назначение |
|-----|-----|----------|------------|
| myTimerKey | Once | cb_keyTimer | Debounce matrix kbd + restore IRQ |
| myTimerOneSec | Periodic | cb_OneSec | 1 сек (пустой, зарезервирован) |
| myTimerWiegand_pin | Once | cb_WiegandPinTimer | Reset Wiegand при таймауте PIN |
| myTimerWiegand_fin | Once | cb_WiegandFinTimer | Финализация Wiegand пакета |
| myTimerWiegand_lock | Once | cb_WiegandLock | Разблокировка Wiegand IRQ |
| myTimerHmiTimeout | Periodic | cb_Hmi_Pin_Timeout | Таймаут ввода PIN |
| myTimerHmiTtl | Once | cb_Hmi_Ttl | TTL сообщения от ESP |
| myTimerReleBefore | Once | cb_TmReleBefore | Предзадержка реле |
| myTimerReleAct | Once | cb_TmReleAct | Время активации реле |
| myTimerBuzzerOff | Once | cb_Tm_buzzerOff | Выключение зуммера |

**Мьютексы/семафоры:**
| Имя | Тип | Назначение |
|-----|-----|------------|
| i2c2_Mutex | Mutex | Защита I2C2 шины (DS3231 + PN532 + TCA) |
| pn532Semaphore | Binary | Сигнал "PN532 IRQ low" → StartTask532 |

### 1.4 Текущие проблемы архитектуры

1. **God object `main.c` (1876 строк):** ISR, I2C slave FSM, DS3231 драйвер, matrix scan, TCA task, relay timers, time sync — всё в одном файле.
2. **Разбросанные magic numbers:** `0x01/0x04/0x08` для TCA пинов, дублируются в нескольких местах.
3. **Глобальные переменные конфигурации:** `rele1_act_sec`, `rele_mode_flag`, `matrix_keyb_freeze`, `reader_interval_sec` — разбросаны, нет единой структуры.
4. **Unsafe payload pointers:** `I2cPacketToMaster_t.payload` указывает на изменяемые глобалы в путях UID_532, WIEGAND, TIME.
5. **malloc в ISR:** `HAL_UART_RxCpltCallback` вызывает `malloc()` — потенциальная проблема фрагментации.
6. **`StartTaskHmiMsg` busy-loop:** при `hmi_lock==LOCKED` крутится с `osDelay(10)` вместо блокировки на очереди.
7. **Прямой GPIO в timer callbacks:** `cb_TmReleBefore/Act` напрямую управляют `RELE1_Pin` — нет изоляции актуатора.
8. **Периодическое чтение Ex:** нет явного места, `apply_runtime_settings_from_ram()` вызывается непредсказуемо из нескольких ISR.

### 1.5 Контракт I2C ESP32 ↔ STM32

**Параметры:** Master=ESP32, Slave=STM32 addr=0x11, 400kHz, IRQ=PB8 (negedge)

**Wire-формат:**
- READ: ESP32 пишет `[reg_addr, len]`, затем читает `len` байт
- WRITE: ESP32 пишет `[reg_addr, len, payload...]`

**Карта регистров:**
```
0x00  PACKET_TYPE        (ro) тип текущего пакета
0x01  PN532 UID          (ro) PACKET_UID_532, 15 байт
0x10  Matrix PIN         (ro) PACKET_PIN, 13 байт
0x20  Wiegand            (ro) PACKET_WIEGAND, 15 байт
0x30  Counter            (rw) count1[2], ESP пишет после обработки
0x40  HMI PIN input      (ro) PACKET_PIN_HMI, 15 байт
0x50  HMI Message        (rw) ESP пишет сообщение для дисплея
0x70  Auth result        (rw) ESP пишет результат авторизации
0x80  HW Time read       (ro) PACKET_TIME, 8 байт BCD
0x88  HW Time write      (rw) ESP пишет текущее время, 7 байт BCD
0xE0  Config (Ex)        (rw) ESP пишет 16 байт cfg_stm32 из NVS
0xF0  STM32 Error        (ro) PACKET_ERROR
```

**Цикл после приёма пакета (ESP32 всегда пишет):**
1. Запись в `0x30` (counter, 2 байта)
2. Запись в `0xE0` (cfg из NVS, 16 байт) → STM32 обновляет `runtime_config_t`

**Ex register (0xE0):**
```
bit0: relay_pulse_en    — разрешить реле от внешней кнопки (TCA P2)
bit1: auth_timeout_act  — зарезервировано
bit2: auth_fail_act     — зарезервировано
E1[3:0]: relay_act_sec        — время активации реле (секунды)
E1[7:4]: relay_before_100ms   — предзадержка × 100ms
E3[3:0]: matrix_freeze_sec    — заморозка matrix kbd после ввода
E4[3:0]: reader_interval_sec  — пауза между сканами PN532
```

---

## 2. Целевая структура файлов

```
Core/
  Inc/
    main.h                    СУЩЕСТВУЮЩИЙ — types, packet/register defines
    tca6408a_map.h            ШАГ 1 (НОВЫЙ) — карта пинов TCA6408A
    service_runtime_config.h  ШАГ 2 (НОВЫЙ) — runtime_config_t + API
    app_irq_router.h          ШАГ 3 (НОВЫЙ) — extern для wiring из main
    app_uart_dwin.h           ШАГ 4 (НОВЫЙ) — dwin_uart_start extern
    service_time_sync.h       ШАГ 5 (НОВЫЙ) — DS3231 + sync API
    service_relay_actuator.h  ШАГ 6 (НОВЫЙ) — relay_request_pulse API
    service_matrix_kbd.h      ШАГ 7 (НОВЫЙ) — kbd scan API + cb_keyTimer
    app_i2c_slave.h           ШАГ 8 (НОВЫЙ) — I2C slave FSM API
    pn532_com.h               СУЩЕСТВУЮЩИЙ — без изменений
    ssd1306.h                 СУЩЕСТВУЮЩИЙ — без изменений
    FreeRTOSConfig.h          СУЩЕСТВУЮЩИЙ — без изменений
  Src/
    main.c                    ШАГ 9 (СОКРАЩАЕТСЯ до ~250 строк)
    freertos.c                СУЩЕСТВУЮЩИЙ — без изменений
    service_runtime_config.c  ШАГ 2 (НОВЫЙ)
    app_irq_router.c          ШАГ 3 (НОВЫЙ)
    app_uart_dwin.c           ШАГ 4 (НОВЫЙ)
    service_time_sync.c       ШАГ 5 (НОВЫЙ)
    service_relay_actuator.c  ШАГ 6 (НОВЫЙ)
    service_matrix_kbd.c      ШАГ 7 (НОВЫЙ)
    app_i2c_slave.c           ШАГ 8 (НОВЫЙ)
    hmi.c                     СУЩЕСТВУЮЩИЙ — минимальные правки (ШАГ 4)
    wiegand.c                 СУЩЕСТВУЮЩИЙ — без изменений
    pn532_com.c               СУЩЕСТВУЮЩИЙ — без изменений
    ssd1306.c / ssd1306_fonts.c  СУЩЕСТВУЮЩИЕ — без изменений
```

---

## 3. Атомарные шаги реализации

---

### ШАГ 1 — `tca6408a_map.h`: карта входов TCA6408A

**Цель:** устранить magic numbers `0x01/0x04/0x08` в `StartTasktca6408a` и зафиксировать
контракт источников сигналов (дребезг / без дребезга).

**Файлы:**
- СОЗДАТЬ: `Core/Inc/tca6408a_map.h`
- ИЗМЕНИТЬ: `Core/Src/main.c` → заменить 0x01/0x04/0x08 в `StartTasktca6408a`

**Содержимое `tca6408a_map.h`:**
```c
#ifndef TCA6408A_MAP_H
#define TCA6408A_MAP_H

/*
 * TCA6408A GPIO Expander — карта входов (I2C2, addr=0x40)
 * Все пины настроены как INPUT, reg=0x03 (polarity normal)
 * IRQ от TCA → STM32 EXT_INT (PC13, EXTI15_10, falling edge)
 */

/* P0: DS3231M INT/SQW — 1Hz square wave
 * Источник: чип DS3231M (аппаратный осциллятор)
 * Дребезг: НЕТ — цифровой выход RTC
 * Полярность: LOW активен (SQW низкий в активной фазе)
 * Обработка: уровень, проверять gpio_ex & TCA_P0_DS3231_1HZ == 0 */
#define TCA_P0_DS3231_1HZ     (0x01U)

/* P2: External mechanical button
 * Источник: механическая кнопка (нормально разомкнута, к GND)
 * Дребезг: ДА — может генерировать множественные IRQ TCA на одно нажатие
 * Полярность: LOW активен (нажата)
 * Обработка: определять по перепаду HIGH→LOW + debounce окно EXT_BTN_DEBOUNCE_MS
 *             повторный приём разрешать только после отпускания (P2 вернулся в HIGH) */
#define TCA_P2_EXT_BUTTON     (0x04U)

/* P3: PN532 NFC reader IRQ
 * Источник: чип PN532 (уровень-LOW при наличии ответа)
 * Дребезг: НЕТ — цифровой выход PN532
 * Полярность: LOW активен
 * Обработка: уровень, проверять gpio_ex & TCA_P3_PN532_IRQ == 0 */
#define TCA_P3_PN532_IRQ      (0x08U)

/* Маска используемых входов */
#define TCA_USED_INPUTS_MASK  (TCA_P0_DS3231_1HZ | TCA_P2_EXT_BUTTON | TCA_P3_PN532_IRQ)

/* TCA6408A register addresses */
#define TCA6408A_REG_INPUT    (0x00U)   /* read current pin state */
#define TCA6408A_REG_OUTPUT   (0x01U)   /* output latch */
#define TCA6408A_REG_POLARITY (0x02U)   /* polarity inversion */
#define TCA6408A_REG_CONFIG   (0x03U)   /* direction: 1=input, 0=output */
#define TCA6408A_ALL_INPUTS   (0xFFU)   /* configure all as inputs */

#endif /* TCA6408A_MAP_H */
```

**Правки в `main.c` (`StartTasktca6408a`):**
- Добавить `#include "tca6408a_map.h"` в начало USER CODE секции includes
- Заменить `tca6408a_write_reg(0x03, 0xFF)` → `tca6408a_write_reg(TCA6408A_REG_CONFIG, TCA6408A_ALL_INPUTS)`
- Заменить `tca6408a_read_reg(0x00, &gpio_ex)` → `tca6408a_read_reg(TCA6408A_REG_INPUT, &gpio_ex)`
- Заменить `(gpio_ex & 0x08)==0` → `(gpio_ex & TCA_P3_PN532_IRQ)==0`
- Заменить `(gpio_ex & 0x01)==0` → `(gpio_ex & TCA_P0_DS3231_1HZ)==0`
- Заменить `(gpio_ex & 0x04U)==0` → `(gpio_ex & TCA_P2_EXT_BUTTON)==0`
- Заменить `(gpio_ex & 0x04U)!=0` → `(gpio_ex & TCA_P2_EXT_BUTTON)!=0`

**Контрольные точки:**
- [ ] Файл `tca6408a_map.h` создан
- [ ] `main.c` собирается без ошибок и предупреждений
- [ ] Поведение `StartTasktca6408a` не изменилось (только имена констант)
- [ ] `grep -n "0x01\|0x04\|0x08" main.c` в зоне TCA task не выдаёт прямых чисел

---

### ШАГ 2 — `service_runtime_config`: структура конфигурации

**Цель:** убрать разбросанные глобальные переменные `rele1_act_sec`, `rele_mode_flag`,
`matrix_keyb_freeze`, `reader_interval_sec`; ввести единую `runtime_config_t`; связать
refresh конфигурации с 1Hz сигналом DS.

**Файлы:**
- СОЗДАТЬ: `Core/Inc/service_runtime_config.h`
- СОЗДАТЬ: `Core/Src/service_runtime_config.c`
- ИЗМЕНИТЬ: `Core/Src/main.c` — убрать глобальные переменные конфигурации, добавить вызов `runtime_config_get()`

**Публичный API (`service_runtime_config.h`):**
```c
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t relay_pulse_en;       /* E0.bit0: 1=разрешить реле от ext btn */
    uint8_t auth_timeout_act;     /* E0.bit1: зарезервировано */
    uint8_t auth_fail_act;        /* E0.bit2: зарезервировано */
    uint8_t relay_act_sec;        /* E1[3:0]: время активации реле, сек */
    uint8_t relay_before_100ms;   /* E1[7:4]: предзадержка × 100ms */
    uint8_t matrix_freeze_sec;    /* E3[3:0]: заморозка kbd после ввода */
    uint8_t reader_interval_sec;  /* E4[3:0]: пауза между сканами PN532 */
} runtime_config_t;

void runtime_config_apply_from_ram(const uint8_t *ram);
const runtime_config_t *runtime_config_get(void);
void runtime_config_init_defaults(uint8_t *ram);
```

**Реализация (`service_runtime_config.c`):**
- Статическая переменная `static runtime_config_t s_cfg`
- `runtime_config_apply_from_ram()` — парсит `ram[0xE0..0xE4]`, обновляет `s_cfg`
- `runtime_config_get()` — возвращает `&s_cfg`
- `runtime_config_init_defaults()` — записывает дефолты в ram[], вызывает apply

**Вызов refresh:** в `StartTasktca6408a`, ветка `TCA_P0_DS3231_1HZ` — добавить вызов
`runtime_config_apply_from_ram(ram)` раз в секунду. Дополнительный таймер не нужен.

**Правки в `main.c`:**
- Удалить объявления: `__IO uint8_t rele1_act_sec, rele1_tm_before_100ms, rele_mode_flag, matrix_keyb_freeze, reader_interval_sec`
- Заменить все прямые обращения на `runtime_config_get()->relay_act_sec` и т.д.
- `apply_runtime_settings_from_ram()` → делегировать в `runtime_config_apply_from_ram()`

**Контрольные точки:**
- [ ] Файлы `service_runtime_config.h/.c` созданы
- [ ] Глобальных `rele1_act_sec`, `rele_mode_flag`, `matrix_keyb_freeze`, `reader_interval_sec` в `main.c` нет
- [ ] `runtime_config_init_defaults` вызывается из `StartDefaultTask`
- [ ] `runtime_config_apply_from_ram` вызывается из TCA task (P0 ветка) и из I2C write 0x30/0xE0
- [ ] Компиляция чистая

---

### ШАГ 3 — `app_irq_router.c`: тонкий ISR-маршрутизатор

**Цель:** вынести `HAL_GPIO_EXTI_Callback` (~140 строк) из `main.c`; оставить в ISR
только `xQueueSendFromISR`/`portYIELD_FROM_ISR`; убрать column-scan matrix keyboard
из ISR в отдельную функцию сервиса.

**Файлы:**
- СОЗДАТЬ: `Core/Inc/app_irq_router.h`
- СОЗДАТЬ: `Core/Src/app_irq_router.c`
- ИЗМЕНИТЬ: `Core/Src/main.c` — удалить тело `HAL_GPIO_EXTI_Callback`, добавить extern вызов

**`app_irq_router.h`:**
```c
void app_irq_router_exti_callback(uint16_t GPIO_Pin);
```

**`app_irq_router.c` — структура:**
```c
#include "main.h"
#include "cmsis_os.h"
#include "tca6408a_map.h"
#include "app_irq_router.h"
#include "service_matrix_kbd.h"   /* будет создан в шаге 7 */

/* extern RTOS handles — объявлены в main.c */
extern osMessageQId myQueueTCA6408Handle, myQueueWiegandHandle;
extern TIM_HandleTypeDef htim11;

void app_irq_router_exti_callback(uint16_t GPIO_Pin)
{
    BaseType_t prior = pdFALSE;

    if (GPIO_Pin == EXT_INT_Pin) {
        /* TCA6408A IRQ — все источники (DS/PN532/кнопка) */
        /* Дребезг кнопки обрабатывается в StartTasktca6408a (уровень задачи) */
        HAL_NVIC_DisableIRQ(EXT_INT_EXTI_IRQn);
        uint16_t tca = 1U;
        xQueueSendFromISR(myQueueTCA6408Handle, &tca, &prior);
        portYIELD_FROM_ISR(prior);
        return;
    }

    if (GPIO_Pin == W_D0_Pin || GPIO_Pin == W_D1_Pin) {
        /* Wiegand D0/D1 bit edge */
        HAL_NVIC_DisableIRQ(W_D0_EXTI_IRQn);
        uint8_t wmsg = (GPIO_Pin == W_D1_Pin) ? 1U : 0U;
        HAL_TIM_PWM_Start_IT(&htim11, TIM_CHANNEL_1);
        xQueueSendFromISR(myQueueWiegandHandle, &wmsg, &prior);
        portYIELD_FROM_ISR(prior);
        return;
    }

    /* Matrix keyboard ROW interrupt */
    matrix_kbd_exti_from_isr(GPIO_Pin, &prior);
    portYIELD_FROM_ISR(prior);
}
```

**В `main.c`:** тело `HAL_GPIO_EXTI_Callback` заменяется на:
```c
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
    app_irq_router_exti_callback(GPIO_Pin);
}
```

**Важно:** `matrix_kbd_exti_from_isr()` — это будет создана в шаге 7. На шаге 3
можно временно оставить inline column-scan в `app_irq_router.c` и выделить на шаге 7.

**Контрольные точки:**
- [ ] `app_irq_router.c/.h` созданы
- [ ] `HAL_GPIO_EXTI_Callback` в `main.c` — однострочный вызов делегата
- [ ] Wiegand ISR-путь работает (очередь `myQueueWiegandHandle` заполняется)
- [ ] TCA ISR-путь работает (очередь `myQueueTCA6408Handle` заполняется)
- [ ] Компиляция чистая

---

### ШАГ 4 — `app_uart_dwin.c`: DWIN UART FSM

**Цель:** вынести DWIN receive FSM (`HAL_UART_RxCpltCallback`, `dwin_uart_start`,
буферы `dwin_hdr/dwin_body`) из `main.c`; исправить `malloc` в ISR-контексте.

**Файлы:**
- СОЗДАТЬ: `Core/Inc/app_uart_dwin.h`
- СОЗДАТЬ: `Core/Src/app_uart_dwin.c`
- ИЗМЕНИТЬ: `Core/Src/main.c` — удалить DWIN FSM, добавить callback делегат
- ИЗМЕНИТЬ: `Core/Src/hmi.c` — `extern void dwin_uart_start(void)` → `#include "app_uart_dwin.h"`

**`app_uart_dwin.h`:**
```c
void dwin_uart_start(void);
void app_uart_dwin_rx_callback(UART_HandleTypeDef *huart);
```

**`app_uart_dwin.c` — ключевые изменения vs текущего кода:**
- `dwin_hdr[3]`, `dwin_body[32]` — статические локалы файла
- `dwin_phase` — статический локал
- `txbuf[256]` → переехал из `hmi.c`, остаётся в `hmi.c` (не затрагивается)
- **malloc замена:** вместо `malloc(total)` — статический пул из 4 буферов:
  ```c
  #define DWIN_POOL_SIZE 4
  static uint8_t   dwin_pool_buf[DWIN_POOL_SIZE][DWIN_HDR_SIZE + DWIN_MAX_BODY];
  static uint8_t   dwin_pool_used[DWIN_POOL_SIZE];
  /* allocate/free из пула — ISR-safe, без heap фрагментации */
  ```
  Освобождение в `StartTaskHmi` после `free(uart_msg.uart_buf)` → `dwin_pool_free(ptr)`

**В `main.c`:**
```c
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    app_uart_dwin_rx_callback(huart);
}
```

**Контрольные точки:**
- [ ] `app_uart_dwin.c/.h` созданы
- [ ] `HAL_UART_RxCpltCallback` в `main.c` — однострочный вызов делегата
- [ ] `dwin_uart_start()` больше не объявлена в `main.c`, `hmi.c` использует заголовок
- [ ] `malloc` в DWIN callback заменён на пул буферов
- [ ] Приём DWIN пакетов работает (очередь `myQueueHMIRecvRawHandle` заполняется)
- [ ] Компиляция чистая

---

### ШАГ 5 — `service_time_sync.c`: DS3231 + синхронизация

**Цель:** вынести весь DS3231 код и time sync логику из `main.c` в изолированный сервис.

**Файлы:**
- СОЗДАТЬ: `Core/Inc/service_time_sync.h`
- СОЗДАТЬ: `Core/Src/service_time_sync.c`
- ИЗМЕНИТЬ: `Core/Src/main.c` — удалить DS3231 функции, добавить include

**Что переезжает из `main.c`:**
```
static bool ds3231_read_time(uint8_t *buf)          → service_time_sync.c (static)
static bool ds3231_write_time(const uint8_t *buf)   → service_time_sync.c (static)
static void sync_ds3231_from_master_time(...)        → service_time_sync.c (публичная)
static uint32_t rtc_bcd_to_seconds(...)             → service_time_sync.c (static)
static uint8_t bcd_to_dec(uint8_t)                  → service_time_sync.c (static)
static bool is_bcd_valid(uint8_t, uint8_t)          → service_time_sync.c (static)
static bool is_valid_time_sync_packet(...)           → service_time_sync.c (публичная)
void datetimepack()                                  → service_time_sync.c (публичная)
static uint8_t hw_time[8]                            → service_time_sync.c (static)
uint8_t date_time[19]                               → service_time_sync.c (static, getter)
uint8_t RTC_ConvertFromDec(uint8_t)                 → service_time_sync.c (static)
uint8_t RTC_ConvertFromBinDec(uint8_t)              → service_time_sync.c (static)
```

**`service_time_sync.h` — публичный API:**
```c
#include <stdint.h>
#include <stdbool.h>

/* Инициализация (вызвать из StartDefaultTask до планировщика) */
void service_time_sync_init(void);

/* Вызывать из StartTasktca6408a при TCA_P0_DS3231_1HZ событии.
 * Читает DS3231, копирует в ram[0x60..0x66], формирует PACKET_TIME.
 * Возвращает true если время прочитано и пакет поставлен в очередь. */
bool service_time_sync_on_tick(uint8_t *ram);

/* Обработать запрос синхронизации от мастера (вызов из I2C write 0x88) */
void service_time_sync_from_master(const uint8_t *master_bcd7, uint8_t len,
                                   uint8_t *ram);

/* Форматировать дату/время для OLED (формат "DD.MM.YY-HH:MM:SS") */
void service_time_sync_datetimepack(const uint8_t *ram);

/* Получить указатель на строку date_time для OLED */
const char *service_time_sync_get_datetime_str(void);

/* Валидация BCD пакета — используется и в app_i2c_slave */
bool service_time_sync_validate_packet(const uint8_t *buf, uint8_t len);
```

**Зависимости сервиса:** `hi2c2`, `i2c2_MutexHandle` (extern), `myQueueToMasterHandle` (extern).

**Правки в `StartTasktca6408a` (main.c → после ШАГ 5):**
```c
if ((gpio_ex & TCA_P0_DS3231_1HZ) == 0) {
    service_time_sync_on_tick(ram);
    runtime_config_apply_from_ram(ram);  /* обновлять Ex раз в секунду */
}
```

**Контрольные точки:**
- [ ] `service_time_sync.c/.h` созданы
- [ ] В `main.c` нет функций `ds3231_*`, `bcd_to_dec`, `datetimepack`, `RTC_Convert*`
- [ ] `StartTaskOLED` получает строку через `service_time_sync_get_datetime_str()`
- [ ] `PACKET_TIME` продолжает генерироваться каждую секунду
- [ ] Sync от мастера (запись 0x88) корректно обрабатывается через `service_time_sync_from_master()`
- [ ] Компиляция чистая

---

### ШАГ 6 — `service_relay_actuator.c`: реле + buzzer

**Цель:** инкапсулировать управление `RELE1_Pin`, `BUZZER_Pin`, таймеры реле;
дать единственный публичный entry point `relay_request_pulse(source)`.

**Файлы:**
- СОЗДАТЬ: `Core/Inc/service_relay_actuator.h`
- СОЗДАТЬ: `Core/Src/service_relay_actuator.c`
- ИЗМЕНИТЬ: `Core/Src/main.c` — удалить `cb_TmReleBefore/Act/BuzzerOff`, `ext_btn_flag`, `ext_btn_pulse_active`

**`service_relay_actuator.h`:**
```c
#include <stdint.h>

typedef enum {
    RELAY_SRC_AUTH_OK = 0,  /* результат авторизации от мастера (0x70 = 0x01) */
    RELAY_SRC_EXT_BTN,      /* внешняя кнопка (TCA P2, после debounce) */
} relay_source_t;

/* Инициализация — вызвать из StartDefaultTask */
void service_relay_actuator_init(void);

/* Запросить импульс реле.
 * Для RELAY_SRC_AUTH_OK:  всегда выполняется (мастер разрешил).
 * Для RELAY_SRC_EXT_BTN:  выполняется только если relay_pulse_en==1 в runtime_config.
 * Использует taймеры myTimerReleBeforeHandle, myTimerReleActHandle. */
void relay_request_pulse(relay_source_t source);

/* Обработчики таймеров (регистрируются в main.c как osTimerDef callbacks) */
void cb_TmReleBefore(void const *argument);
void cb_TmReleAct(void const *argument);
void cb_Tm_buzzerOff(void const *argument);
```

**Реализация ключевых деталей:**
- `static uint8_t s_ext_btn_flag` и `static uint8_t s_ext_btn_pulse_active` — статические локалы сервиса
- `relay_request_pulse(RELAY_SRC_EXT_BTN)`: проверяет `runtime_config_get()->relay_pulse_en`
- `relay_request_pulse(RELAY_SRC_AUTH_OK)`: всегда стартует `myTimerReleBeforeHandle`
- `cb_TmReleBefore`: логика `ext_btn_pulse_active ? SET : (mode==1 ? SET : RESET)`, затем запуск `myTimerReleActHandle`
- `cb_TmReleAct`: логика `ext_btn_pulse_active ? RESET+clear : (mode==1 ? RESET : SET)`
- Buzzer: `relay_request_pulse` также включает BUZZER + стартует `myTimerBuzzerOffHandle`

**Вызов в `StartTasktca6408a` (кнопка TCA P2):**
```c
if (((gpio_ex & TCA_P2_EXT_BUTTON) == 0U) && ((gpio_ex_prev & TCA_P2_EXT_BUTTON) != 0U)) {
    uint32_t now = HAL_GetTick();
    if ((now - ext_btn_last_tick) >= EXT_BTN_DEBOUNCE_MS) {
        ext_btn_last_tick = now;
        relay_request_pulse(RELAY_SRC_EXT_BTN);   /* ← вместо прямого GPIO */
    }
}
```

**Вызов при auth result (I2C write 0x70):**
```c
/* в app_i2c_slave.c или inline в main.c до ШАГ 8 */
if (ram[0x70] == 0x01U) {
    relay_request_pulse(RELAY_SRC_AUTH_OK);
}
```

**Контрольные точки:**
- [ ] `service_relay_actuator.c/.h` созданы
- [ ] В `main.c` нет `ext_btn_flag`, `ext_btn_pulse_active`
- [ ] Реле щёлкает однократно на нажатие при Ex.bit0=1; мусорные IRQ игнорируются
- [ ] Реле срабатывает от auth_result `0x01` от мастера
- [ ] Buzzer работает в обоих сценариях
- [ ] Компиляция чистая

---

### ШАГ 7 — `service_matrix_kbd.c`: matrix keyboard

**Цель:** вынести ~130 строк column-scan ISR логики, `struct keyb`, `debounce`,
`cb_keyTimer` из `main.c`; дать чистый ISR entry point для `app_irq_router`.

**Файлы:**
- СОЗДАТЬ: `Core/Inc/service_matrix_kbd.h`
- СОЗДАТЬ: `Core/Src/service_matrix_kbd.c`
- ИЗМЕНИТЬ: `Core/Src/main.c` — удалить matrix scan из ISR, `cb_keyTimer`, `struct keyb`, `debounce`
- ИЗМЕНИТЬ: `Core/Src/app_irq_router.c` — убрать inline scan, вызвать `matrix_kbd_exti_from_isr()`

**`service_matrix_kbd.h`:**
```c
#include <stdint.h>
#include "cmsis_os.h"

/* Инициализация — вызвать из StartDefaultTask */
void service_matrix_kbd_init(void);

/* Вызывается из HAL_GPIO_EXTI_Callback (app_irq_router) — ISR контекст */
void matrix_kbd_exti_from_isr(uint16_t GPIO_Pin, BaseType_t *pxHigherPriorityTaskWoken);

/* Timer callback — зарегистрировать как osTimerDef(myTimerKey, ...) в main.c */
void cb_keyTimer(void const *argument);
```

**Реализация:**
- `static struct keyb s_keyb` и `static uint8_t s_debounce` — статические локалы
- `static uint8_t s_final_input` — статический локал
- `matrix_kbd_exti_from_isr()`: полная логика column-scan (сейчас inline в `HAL_GPIO_EXTI_Callback`)
  - GPIO reconfigure → COL scan → decode keycode
  - Если digit: `xQueueSendFromISR(myQueueOLEDHandle, ...)`
  - Если '#': `xQueueSendFromISR(myQueueToMasterHandle, &pckt, ...)` с `PACKET_PIN`
  - `debounce = 0`, `osTimerStart(myTimerKeyHandle, ...)`
- `cb_keyTimer()`: восстановление COL lines, reconfig ROW IRQ, `debounce = 1`
- `service_matrix_kbd_init()`: init `s_keyb`, `s_debounce = 1`, `s_final_input = 0`

**Контрольные точки:**
- [ ] `service_matrix_kbd.c/.h` созданы
- [ ] В `main.c` нет matrix scan кода, нет `struct keyb keyb`, нет `debounce`, нет `final_input`
- [ ] `cb_keyTimer` объявлен в `service_matrix_kbd.h`, zарегистрирован в `main.c`
- [ ] Ввод цифр с matrix keyboard отображается на OLED
- [ ] Ввод '#' генерирует `PACKET_PIN` в `myQueueToMasterHandle`
- [ ] Debounce работает (повторные нажатия в течение `myTimerKey` ms игнорируются)
- [ ] Компиляция чистая

---

### ШАГ 8 — `app_i2c_slave.c`: I2C slave FSM

**Цель:** вынести ~400 строк I2C slave логики из `main.c`; изолировать RAM, FSM,
outbox, регистровые обработчики; дать единый init + run вызов.

**Файлы:**
- СОЗДАТЬ: `Core/Inc/app_i2c_slave.h`
- СОЗДАТЬ: `Core/Src/app_i2c_slave.c`
- ИЗМЕНИТЬ: `Core/Src/main.c` — удалить все HAL_I2C callbacks, outbox, ram[]

**Что переезжает:**
```
HAL_I2C_AddrCallback               → app_i2c_slave.c
HAL_I2C_SlaveRxCpltCallback        → app_i2c_slave.c
HAL_I2C_SlaveTxCpltCallback        → app_i2c_slave.c
HAL_I2C_ListenCpltCallback         → app_i2c_slave.c
HAL_I2C_AbortCpltCallback          → app_i2c_slave.c
HAL_I2C_ErrorCallback (hi2c1 ветка) → app_i2c_slave.c
i2c_outbox_publish()               → app_i2c_slave.c (публичная)
i2c_outbox_complete_ack()          → app_i2c_slave.c (static)
i2c_outbox_recover_after_error()   → app_i2c_slave.c (static)
i2c_packet_reg_for_type()          → app_i2c_slave.c (static)
i2c_packet_len_for_type()          → app_i2c_slave.c (static)
uint8_t ram[256]                   → app_i2c_slave.c (static, getter)
struct i2c_seq_ctrl_s i2c_sec_ctrl_h → app_i2c_slave.c (static)
volatile i2c_outbox_* переменные   → app_i2c_slave.c (static)
struct i2c_test_s i2c_test         → app_i2c_slave.c (static)
StartTaskRxTxI2c1()                → app_i2c_slave.c (публичная)
```

**`app_i2c_slave.h` — публичный API:**
```c
#include <stdint.h>

/* Инициализация — вызвать из StartDefaultTask перед планировщиком */
void app_i2c_slave_init(void);

/* Задача RxTx — зарегистрировать как osThreadDef(myTaskRxTxI2c1, ...) */
void StartTaskRxTxI2c1(void const *argument);

/* Поставить пакет в outbox (вызывается из задач сервисов) */
void i2c_outbox_publish(const I2cPacketToMaster_t *pckt);

/* Получить указатель на RAM (для сервисов, которым нужен доступ к регистрам) */
uint8_t *app_i2c_slave_get_ram(void);
```

**Регистровые обработчики при записи (`HAL_I2C_SlaveRxCpltCallback`, final ветка):**
```
base == 0x50  → process_host_message_write → xQueueSendFromISR(myQueueHmiMsgHandle)
base == 0x70  → process_auth_result_write  → relay_request_pulse(RELAY_SRC_AUTH_OK)
                                           + buzzer (через service_relay_actuator)
base == 0x88  → service_time_sync_from_master(ram+0x88, rx_count, ram)
base == 0x30  → runtime_config_apply_from_ram(ram)
base == 0xE0  → runtime_config_apply_from_ram(ram)
```

**Важно для `HAL_I2C_ErrorCallback`:** ветка `hi2c2` (PN532/DS3231 fault) остаётся
в `main.c` или переезжает в отдельную функцию — `app_i2c_slave.c` обрабатывает
только `hi2c1` (slave шина к ESP32).

**Контрольные точки:**
- [ ] `app_i2c_slave.c/.h` созданы
- [ ] В `main.c` нет `HAL_I2C_*Callback` тел, нет `ram[]`, нет outbox переменных
- [ ] `StartTaskRxTxI2c1` работает: пакеты из `myQueueToMasterHandle` доходят до ESP32
- [ ] Запись мастером в 0x50 обновляет HMI дисплей
- [ ] Запись мастером в 0x70 (0x01) активирует реле
- [ ] Запись мастером в 0x88 синхронизирует DS3231
- [ ] Запись мастером в 0xE0 обновляет `runtime_config_t`
- [ ] I2C ERROR recovery (BERR → DeInit/Init) работает
- [ ] Компиляция чистая

---

### ШАГ 9 — Финализация `main.c`: bootstrap only

**Цель:** довести `main.c` до ~250 строк — только init, RTOS wiring, минимальные задачи.

**Что остаётся в `main.c` после всех предыдущих шагов:**
```
SystemClock_Config()
MX_GPIO_Init(), MX_I2C1/2_Init(), MX_SPI1_Init(), MX_USART1/2_Init(), MX_TIM11_Init()
lock_i2c2() / unlock_i2c2()
HAL_GPIO_EXTI_Callback()          ← 1 строка: app_irq_router_exti_callback(pin)
HAL_UART_RxCpltCallback()         ← 1 строка: app_uart_dwin_rx_callback(huart)
HAL_I2C_AbortCpltCallback()       ← 2 строки: hi2c1 → HAL_I2C_EnableListen_IT
HAL_I2C_ErrorCallback() (hi2c2)   ← DeInit/Init для PN532/DS3231 fault
HAL_TIM_PeriodElapsedCallback()   ← HAL_IncTick для TIM10
Error_Handler()                   ← NVIC_SystemReset
main() — RTOS object creation + osKernelStart
StartDefaultTask()                ← init ram defaults, DS3231 ctrl reg, relay/kbd init
StartTask532()                    ← PN532 scan task (остаётся в main.c или выносится позже)
StartTaskOLED()                   ← OLED refresh task
StartTasktca6408a()               ← TCA event dispatcher (использует tca6408a_map.h,
                                     service_time_sync, service_relay_actuator, runtime_config)
```

**Убрать из `main.c`:**
- ~~`HAL_GPIO_EXTI_Callback` тело~~ → app_irq_router.c
- ~~`HAL_UART_RxCpltCallback` тело~~ → app_uart_dwin.c
- ~~`HAL_I2C_*Callback` тела~~ → app_i2c_slave.c
- ~~`ds3231_*`, `bcd_to_dec`, `datetimepack`~~ → service_time_sync.c
- ~~`cb_TmReleBefore/Act/BuzzerOff`~~ → service_relay_actuator.c
- ~~matrix scan in ISR~~, ~~`cb_keyTimer`~~, ~~`struct keyb`~~ → service_matrix_kbd.c
- ~~`i2c_outbox_*`, `ram[]`, `i2c_sec_ctrl_s`~~ → app_i2c_slave.c
- ~~глобальные переменные конфигурации~~ → service_runtime_config.c

**Правки `StartDefaultTask` после рефакторинга:**
```c
void StartDefaultTask(void const *argument) {
    MX_USB_DEVICE_Init();
    service_matrix_kbd_init();
    service_relay_actuator_init();
    service_time_sync_init();
    app_i2c_slave_init();
    uint8_t *ram = app_i2c_slave_get_ram();
    runtime_config_init_defaults(ram);
    /* DS3231 control register: disable INTCN, enable SQW 1Hz */
    lock_i2c2(100);
    /* ... DS3231 ctrl reg write ... */
    unlock_i2c2();
    for (;;) { osDelay(100); }
}
```

**Контрольные точки:**
- [ ] `main.c` менее 300 строк (без учёта MX_* init функций)
- [ ] Все `#include` в `main.c` сведены к минимуму (убраны прямые `pn532_com.h`, `ssd1306.h` если не нужны)
- [ ] Нет глобальных переменных вне тех, что требует HAL (hi2c1, hi2c2, hspi1, ...)
- [ ] Полная сборка проекта: 0 ошибок, 0 предупреждений
- [ ] Функциональное тестирование всех потоков (п. 4)

---

### ШАГ 11 — `service_pn532_task.c`: PN532 NFC reader task

**Цель:** вынести `StartTask532` (~70 строк), `pn532_probe_bounded()`, буферы
`pn532_t pn532`, `slaveTxData[64]`, `uid[32]`, `response`, `pn_i2c_fault` из `main.c`.
Заменить прямое обращение к `reader_interval_sec` (legacy-глобал) на `runtime_config_get()`.

**Файлы:**
- СОЗДАТЬ: `Core/Inc/service_pn532_task.h`
- СОЗДАТЬ: `Core/Src/service_pn532_task.c`
- ИЗМЕНИТЬ: `Core/Src/main.c` — удалить тело `StartTask532`, `pn532_probe_bounded`,
  перенесённые глобалы; добавить `service_pn532_init()` в `StartDefaultTask`;
  заменить `pn_i2c_fault = 1` в `HAL_I2C_ErrorCallback` на `service_pn532_notify_i2c_fault()`

**Публичный API (`service_pn532_task.h`):**
```c
void service_pn532_init(void);
void StartTask532(void const *argument);
void service_pn532_notify_i2c_fault(void);
uint8_t *service_pn532_get_slaveTxData(void);
```

**Контрольные точки:**
- [x] `service_pn532_task.h/.c` созданы
- [x] В `main.c` нет `StartTask532` тела, нет `pn532_probe_bounded`, нет `pn532_t pn532`
- [x] `reader_interval_sec` заменён на `runtime_config_get()->reader_interval_sec`
- [x] `HAL_I2C_ErrorCallback` hi2c2 ветка вызывает `service_pn532_notify_i2c_fault()`
- [x] Компиляция чистая

---

### ШАГ 12 — `service_oled_task.c`: OLED display task

**Цель:** вынести `StartTaskOLED` (~30 строк) из `main.c`.
Задача не имеет собственного состояния — чистый consumer очереди `myQueueOLEDHandle`.

**Файлы:**
- СОЗДАТЬ: `Core/Inc/service_oled_task.h`
- СОЗДАТЬ: `Core/Src/service_oled_task.c`
- ИЗМЕНИТЬ: `Core/Src/main.c` — удалить тело `StartTaskOLED`, добавить `extern` прототип,
  убрать `#include "ssd1306.h"`, `"ssd1306_tests.h"`, `"ssd1306_fonts.h"`, `"pn532_com.h"`

**Публичный API (`service_oled_task.h`):**
```c
void StartTaskOLED(void const *argument);
```

**Контрольные точки:**
- [x] `service_oled_task.h/.c` созданы
- [x] В `main.c` нет `StartTaskOLED` тела
- [x] Неиспользуемые `#include` удалены из `main.c`
- [x] Компиляция чистая

---

### ШАГ 13 — Очистка мёртвого кода в `main.c`

**Цель:** удалить переменные, функции, прототипы, дефайны и закомментированный код,
которые больше не имеют потребителей после завершения шагов 1–12.

**Удалено:**
- Dead-переменные: `Transfer_Direction`, `Xfer_Complete`, `key_buf_offset`,
  `slaveRxData`, `rxbuf[128]` — ноль читателей
- Legacy-глобалы: `rele1_act_sec`, `rele1_tm_before_100ms`, `rele_mode_flag`,
  `matrix_keyb_freeze`, `reader_interval_sec` — все модули используют `runtime_config_get()`
- `apply_runtime_settings_from_ram()` — единственный потребитель удалённых глобалов
- Dead-прототип `wiegand_bit_event` — без тела, без вызовов
- Дублированные `#define TIME_SYNC_MIN_YEAR/MONTH/DAY` — уже в `main.h`
- Закомментированный код в `StartDefaultTask` и post-scheduler `while(1)` loop
- Неиспользуемые `#include`: `stdio.h`, `stdlib.h`, `stdbool.h`, `event_groups.h`, `tca6408a_map.h`

**Контрольные точки:**
- [x] `grep` по всем `Core/Src/*.c` подтверждает ноль потребителей удалённых символов
- [x] Компиляция чистая (0 ошибок)
- [x] Ни один поток событий не затронут

---

### ШАГ 14 — Вынос `StartTasktca6408a` и DS3231 init

**Цель:** убрать последнее пользовательское тело задачи из `main.c` и убрать прямую
работу с DS3231 по I2C из `StartDefaultTask`.

**Что перенесено:**
- `StartTasktca6408a` (~15 строк) → `service_tca6408.c` (прототип в `.h`)
- DS3231 control register init (read INTCN, clear bit2, write back) →
  новая функция `service_time_sync_init()` в `service_time_sync.c/.h`
- `StartDefaultTask` теперь вызывает `service_time_sync_init()` вместо inline I2C кода

**Удалены из `main.c`:**
- Тело `StartTasktca6408a` — заменено на `extern` прототип
- Inline DS3231 I2C код из `StartDefaultTask` (6 строк)
- Неиспользуемые `#include`: `service_tca6408.h`, `service_oled_task.h`

**Контрольные точки:**
- [x] `StartTasktca6408a` живёт в `service_tca6408.c`, прототип в `.h`
- [x] `service_time_sync_init()` — инициализирует DS3231 SQW 1Hz
- [x] `main.c` не содержит прямых обращений к DS3231 регистрам
- [x] Компиляция чистая (0 ошибок)

---

## 4. Функциональный тест после рефакторинга

После завершения всех шагов проверить каждый поток:

### Поток 1: DS3231 → TIME пакет (каждую секунду)
```
DS3231 1Hz → TCA P0 LOW → EXT_INT IRQ → myQueueTCA6408 →
StartTasktca6408a → service_time_sync_on_tick() → ds3231_read_time() →
PACKET_TIME → myQueueToMaster → StartTaskRxTxI2c1 → I2C 0x80 → ESP32
```
✓ Признак: ESP32 логирует `PACKET_TIME received`, age_sec = 0..2

### Поток 2: PN532 → UID пакет
```
PN532 обнаружил карту → IRQ LOW → TCA P3 LOW → EXT_INT IRQ → myQueueTCA6408 →
StartTasktca6408a → osSemaphoreRelease(pn532Semaphore) →
StartTask532 разблокирован → pn532_read() → PACKET_UID_532 →
myQueueToMaster → I2C 0x01 → ESP32
```
✓ Признак: ESP32 логирует `PACKET_UID_532`, auth результат записывается в 0x70

### Поток 3: Внешняя кнопка → реле
```
Кнопка нажата → TCA P2 LOW (возможен дребезг) → EXT_INT IRQ (мусорные) →
myQueueTCA6408 → StartTasktca6408a → edge detect + debounce (60ms) →
relay_request_pulse(RELAY_SRC_EXT_BTN) → проверка Ex.bit0 →
cb_TmReleBefore → RELE1 SET → cb_TmReleAct → RELE1 RESET
```
✓ Признак: реле щёлкает однократно на нажатие при Ex.bit0=1; мусорные IRQ игнорируются

### Поток 4: Auth result → реле
```
ESP32 пишет [0x70, 5, 0x01, ...] → I2C slave → HAL_I2C_SlaveRxCpltCallback →
app_i2c_slave: base=0x70 → relay_request_pulse(RELAY_SRC_AUTH_OK) →
cb_TmReleBefore → RELE1 SET → cb_TmReleAct → RELE1 RESET
```
✓ Признак: реле срабатывает после успешной авторизации

### Поток 5: DWIN HMI → PIN HMI пакет
```
Нажатие на DWIN → USART2 пакет [5A A5 ...] → HAL_UART_RxCpltCallback →
app_uart_dwin FSM → MsgUart_t → myQueueHMIRecvRaw → StartTaskHmi →
pin буфер → '#' → PACKET_PIN_HMI → myQueueToMaster → I2C 0x40 → ESP32
```
✓ Признак: ESP32 логирует `PACKET_PIN_HMI`, HMI дисплей получает ответ

### Поток 6: Wiegand → WIEGAND пакет
```
Карта → W_D0/D1 falling edge → EXTI15_10 → app_irq_router →
myQueueWiegand → StartTaskWiegand → bit assembly → cb_WiegandFinTimer →
PACKET_WIEGAND → myQueueToMaster → I2C 0x20 → ESP32
```
✓ Признак: ESP32 логирует `PACKET_WIEGAND`

### Поток 7: Matrix keyboard → PIN пакет
```
Нажатие ROW → EXTI → app_irq_router → matrix_kbd_exti_from_isr() →
column scan → keycode → '#' → PACKET_PIN → myQueueToMaster → I2C 0x10 → ESP32
```
✓ Признак: ESP32 логирует `PACKET_PIN`, OLED отображает символы

### Поток 8: Конфигурация Ex (периодически от мастера)
```
ESP32 после обработки любого пакета пишет [0xE0, 16, cfg...] →
I2C slave → app_i2c_slave: base=0xE0 → runtime_config_apply_from_ram() →
runtime_config_t обновлён → следующий relay/button запрос использует новые значения
```
✓ Признак: изменение relay_act_sec в NVS ESP32 влияет на время удержания реле

---

## 5. Риски и меры

| Риск | Вероятность | Мера |
|------|-------------|------|
| I2C slave FSM регрессия после ШАГ 8 | Высокая | Пошаговое тестирование каждого типа пакета до и после |
| Потеря payload pointer (UID/Wiegand/TIME) | Средняя | Проверить ownership буфера в каждом `xQueueSend` вызове |
| malloc в DWIN ISR вызовет Hard Fault | Низкая | Заменить пулом на ШАГ 4, тест на длительной работе |
| Мусорные TCA IRQ от кнопки переполняют очередь | Средняя | `myQueueTCA6408` размер 16, debounce в task — достаточно |
| pn532Semaphore race при быстрой смене карт | Низкая | Семафор бинарный — накапливается максимум 1 событие |

---

## 6. Прогресс

| Шаг | Статус | Файлы | Дата |
|-----|--------|-------|------|
| 1. tca6408a_map.h | ✅ Выполнен | tca6408a_map.h, main.c | 2026-03-22 |
| 2. service_runtime_config | ✅ Выполнен | service_runtime_config.h/.c, main.c | 2026-03-22 |
| 3. app_irq_router | ✅ Выполнен | app_irq_router.h/.c, main.c | 2026-03-22 |
| 4. app_uart_dwin | ✅ Выполнен | app_uart_dwin.h/.c, main.c, hmi.c | 2026-03-22 |
| 5. service_time_sync | ✅ Выполнен | service_time_sync.h/.c, main.c | 2026-03-22 |
| 6. service_relay_actuator | ✅ Выполнен | service_relay_actuator.h/.c, main.c | 2026-03-22 |
| 7. service_matrix_kbd | ✅ Выполнен | service_matrix_kbd.h/.c, main.c, app_irq_router.c | 2026-03-22 |
| 8. app_i2c_slave | ✅ Выполнен | app_i2c_slave.h/.c, main.c | 2026-03-22 |
| 9. main.c bootstrap | ✅ Выполнен | main.c | 2026-03-22 |
| 10. Валидация компиляции | ✅ Выполнена | изменённые файлы | 2026-03-22 |
| 11. service_pn532_task | ✅ Выполнен | service_pn532_task.h/.c, main.c | 2026-03-25 |
| 12. service_oled_task | ✅ Выполнен | service_oled_task.h/.c, main.c | 2026-03-25 |
| 13. Очистка мёртвого кода | ✅ Выполнен | main.c | 2026-03-25 |
| 14. TCA task + DS3231 init | ✅ Выполнен | service_tca6408.h/.c, service_time_sync.h/.c, main.c | 2026-03-25 |
