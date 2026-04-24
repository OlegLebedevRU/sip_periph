# 🧪 GM810 USART6 - Пошаговый План Диагностики

**Дата:** 2026-04-24  
**Целевой результат:** Определить на каком этапе инициализации или обработки данных происходит сбой

---

## 📝 Предварительные Условия

### Необходимое оборудование:
- [ ] STM32F411 микроконтроллер (STM32F4-main/sip_periph проект)
- [ ] Отладчик (SWD/JTAG)
- [ ] IDE (STM32CubeIDE, Keil, GDB)
- [ ] GM810 QR сканер с UART выходом
- [ ] USB-TTL преобразователь (опционально для логирования)

### Перед началом:
- [ ] Проект откомпилирован успешно
- [ ] Флаг `HW_PROFILE_GM810_USART6 = 1U` в main.h
- [ ] Батарея STM32 заряжена
- [ ] GM810 подключен к стенду питания

---

## 🔧 БЛОК 1: Проверка Инициализации (До запуска RTOS)

### Тест 1.1: MX_GPIO_Init() Выполнение

**Что тестируем:** Инициализация PA11/PA12 как Alternate Function 8 для USART6

**Шаги:**
1. Разместить breakpoint в `main.c` на строке 754 (начало MX_GPIO_Init)
2. Запустить отладчик
3. Шагово пройти до строки 845 (конец GP_INIT блока)

**Проверяемые значения:**

При попадании на строку 845 `HAL_GPIO_Init(GPIOA, &GPIO_InitStruct)`:
```c
// Проверить в отладчике (Watch окно):
GPIO_InitStruct.Pin         // ДОЛЖНО БЫТЬ: 0x1800 (PA11 + PA12)
GPIO_InitStruct.Mode        // ДОЛЖНО БЫТЬ: 0x2 (GPIO_MODE_AF_PP)
GPIO_InitStruct.Pull        // ДОЛЖНО БЫТЬ: 0x0 (GPIO_NOPULL)
GPIO_InitStruct.Speed       // ДОЛЖНО БЫТЬ: 0x3 (GPIO_SPEED_FREQ_VERY_HIGH)
GPIO_InitStruct.Alternate   // ДОЛЖНО БЫТЬ: 0x8 (GPIO_AF8_USART6)
```

**Ожидаемый результат:**
- ✅ Функция вызывается (breakpoint срабатывает)
- ✅ GPIO_InitStruct заполнена правильно
- ✅ HAL_GPIO_Init успешно выполняется

**Если FAIL:**
- Breakpoint не срабатывает → Условная компиляция отключена
- GPIO_InitStruct не заполнена → Код не выполняется (проверить #if директивы)
- HAL_GPIO_Init возвращает ошибку → Проблема с GPIOA или конфликт пинов

---

### Тест 1.2: MX_USART6_UART_Init() Выполнение

**Что тестируем:** Инициализация USART6 контроллера на 9600 bps

**Шаги:**
1. Разместить breakpoint в `main.c` на строке 721 (начало MX_USART6_UART_Init)
2. Шагово пройти до строки 732 `HAL_UART_Init(&huart6)`
3. Проверить return value

**Проверяемые значения:**

При попадании на строку 732:
```c
huart6.Instance              // ДОЛЖНО БЫТЬ: 0x40011800 (USART6 адрес)
huart6.Init.BaudRate         // ДОЛЖНО БЫТЬ: 9600
huart6.Init.WordLength       // ДОЛЖНО БЫТЬ: 0x0 (8 bits)
huart6.Init.StopBits         // ДОЛЖНО БЫТЬ: 0x0 (1 stop bit)
huart6.Init.Parity           // ДОЛЖНО БЫТЬ: 0x0 (None)
huart6.Init.Mode             // ДОЛЖНО БЫТЬ: 0x3 (TX + RX)
huart6.Init.HwFlowCtl        // ДОЛЖНО БЫТЬ: 0x0 (None)
huart6.Init.OverSampling     // ДОЛЖНО БЫТЬ: 16
```

**После вызова HAL_UART_Init:**
```c
huart6.gState                // ДОЛЖНО БЫТЬ: HAL_UART_STATE_READY (0)
huart6.RxState               // ДОЛЖНО БЫТЬ: HAL_UART_STATE_READY (0)
```

**Ожидаемый результат:**
- ✅ HAL_UART_Init возвращает HAL_OK
- ✅ huart6.gState = READY
- ✅ Нет вызова Error_Handler

**Если FAIL:**
- HAL_UART_Init возвращает HAL_ERROR → Проверить наличие часов USART6 в RCC
- huart6.gState ≠ READY → Контроллер не инициализирован
- Вызван Error_Handler → Критическая проблема, проверить конфигурацию

---

### Тест 1.3: Проверка RCC Clock для USART6

**Что тестируем:** USART6 получает системный тактовый сигнал

**Шаги:**
1. После HAL_UART_Init, разместить breakpoint
2. В отладчике посмотреть память RCC регистров:

**Проверяемые значения:**

```c
// Address: 0x40023800 + 0x44 = 0x40023844 (APB2ENR)
// Проверить бит 5 (USART6EN)
// ДОЛЖНО БЫТЬ: SET (1)

RCC->APB2ENR & (1 << 5)      // ДОЛЖНО БЫТЬ: 0x20 (ненулевое)
```

**Ожидаемый результат:**
- ✅ Бит USART6EN установлен в 1

**Если FAIL:**
- USART6EN не установлен → HAL_UART_Init не вызвал __HAL_RCC_USART6_CLK_ENABLE()
- Результат: USART6 будет мёртв, никакой активности

---

## 🔧 БЛОК 2: Проверка Вызовов Инициализации (В main())

### Тест 2.1: service_gm810_uart_init() Вызов

**Что тестируем:** Инициализация парсера и буферов GM810

**Шаги:**
1. Разместить breakpoint в `main.c` на строке 242 `service_gm810_uart_init()`
2. Войти в функцию (Step Into)
3. Проверить что парсер сброшен

**Проверяемые значения:**

После `service_gm810_uart_init()`:
```c
s_rx.state                   // ДОЛЖНО БЫТЬ: 0 (GM810_RX_WAIT_HEADER)
s_rx.expected_len            // ДОЛЖНО БЫТЬ: 0
s_rx.received_len            // ДОЛЖНО БЫТЬ: 0
s_rx_byte                    // ДОЛЖНО БЫТЬ: 0
s_publish_slot_index         // ДОЛЖНО БЫТЬ: 0
```

**Ожидаемый результат:**
- ✅ Все статические переменные парсера обнулены

**Если FAIL:**
- Переменные не обнулены → memset не вызвана корректно

---

### Тест 2.2: RTOS Очередь myQueueToMasterHandle Создание

**Что тестируем:** Очередь создана ДО вызова service_gm810_uart_start()

**Шаги:**
1. Разместить breakpoint в `main.c` на строке 364 (после создания myQueueToMasterHandle)
2. Проверить указатель очереди

**Проверяемые значения:**

```c
myQueueToMasterHandle        // ДОЛЖНО БЫТЬ: не NULL (адрес очереди)
```

**Ожидаемый результат:**
- ✅ myQueueToMasterHandle не равна NULL

**Если FAIL:**
- myQueueToMasterHandle = NULL → Очередь не создана, osMessageCreate ошибка

---

### Тест 2.3: service_gm810_uart_start() Вызов

**Что тестируем:** Запуск UART приёма и инициализация gm810_restart_receive_it()

**Шаги:**
1. Разместить breakpoint в `main.c` на строке 367 `service_gm810_uart_start()`
2. Войти в функцию (Step Into)
3. Шагово пройти через `gm810_restart_receive_it()`

**Проверяемые значения:**

При входе в `gm810_restart_receive_it()`:
```c
// service_gm810_uart.c строка 44
HAL_UART_Receive_IT(&huart6, &s_rx_byte, 1U);
// После вызова, проверить:

huart6.RxState               // ДОЛЖНО БЫТЬ: HAL_UART_STATE_BUSY_RX (1)
```

**Проверить настройку UART6 RX IT:**
```c
// В USART6 регистре USART_CR1 должны быть установлены:
USART6->CR1 & (1 << 5)       // Бит RXNEIE (RX Not Empty Interrupt) = 1
USART6->CR1 & (1 << 7)       // Бит UE (UART Enable) = 1
```

**Ожидаемый результат:**
- ✅ HAL_UART_Receive_IT вызвана успешно
- ✅ huart6.RxState = BUSY_RX
- ✅ RXNEIE бит установлен

**Если FAIL:**
- huart6.RxState ≠ BUSY_RX → UART6 RX IT не запущена
- RXNEIE бит не установлен → Прерывания не будут генерироваться
- UART6 не включён (UE бит) → Контроллер мёртв

---

## 🔧 БЛОК 3: Проверка GPIO Состояния (После Инициализации)

### Тест 3.1: PA11/PA12 Mode Регистры

**Что тестируем:** GPIO пины находятся в режиме Alternate Function

**Шаги:**
1. После инициализации, разместить breakpoint
2. Проверить GPIOA->MODER регистр

**Проверяемые значения:**

```c
// GPIOA->MODER (GPIO_MODER_MODER11 и GPIO_MODER_MODER12)
// Позиция битов: PA11 = биты [23:22], PA12 = биты [25:24]
// Значение: 0x2 = Alternate Function Mode

(GPIOA->MODER >> 22) & 0x3  // PA11 Mode - ДОЛЖНО БЫТЬ: 2 (AF mode)
(GPIOA->MODER >> 24) & 0x3  // PA12 Mode - ДОЛЖНО БЫТЬ: 2 (AF mode)
```

**Ожидаемый результат:**
- ✅ PA11 Mode = 2
- ✅ PA12 Mode = 2

**Если FAIL:**
- Mode = 0 (Input) → GPIO не переведены в AF режим
- Mode = 1 (Output) → GPIO в режиме обычного вывода
- Mode = 3 (Analog) → GPIO в аналоговом режиме

---

### Тест 3.2: PA11/PA12 Alternate Function

**Что тестируем:** Правильно выбрана Alternate Function 8 (USART6)

**Шаги:**
1. После инициализации GPIO, проверить AFR регистры
2. PA11 и PA12 находятся в AFR[1] (высоко регистр)

**Проверяемые значения:**

```c
// GPIOA->AFR[1] (HIGH регистр для PA8-PA15)
// PA11: биты [15:12] = 0x8 (AF8)
// PA12: биты [19:16] = 0x8 (AF8)

(GPIOA->AFR[1] >> 12) & 0xF  // PA11 AF - ДОЛЖНО БЫТЬ: 8 (USART6)
(GPIOA->AFR[1] >> 16) & 0xF  // PA12 AF - ДОЛЖНО БЫТЬ: 8 (USART6)
```

**Ожидаемый результат:**
- ✅ PA11 AF = 8
- ✅ PA12 AF = 8

**Если FAIL:**
- AF value ≠ 8 → PA11/PA12 связаны с другой периферией
- AF value = 0 → AF не установлена (GPIO просто input/output)

---

## 🔧 БЛОК 4: Проверка UART6 Рабочего Состояния

### Тест 4.1: USART6 Status Register

**Что тестируем:** UART6 готов к приёму данных

**Шаги:**
1. После инициализации и запуска приёма, проверить SR регистр
2. В отладчике посмотреть:

**Проверяемые значения:**

```c
// USART6->SR (Status Register)
USART6->SR & (1 << 5)        // Бит RXNE (RX Not Empty) = 0 (буфер пуст - нормально)
USART6->SR & (1 << 4)        // Бит IDLE = 1 (линия свободна - нормально)
USART6->CR1 & (1 << 5)       // Бит RXNEIE = 1 (RX IT включено)
USART6->CR1 & (1 << 0)       // Бит PE = 0 (Parity Error - нет ошибок)
```

**Ожидаемый результат:**
- ✅ SR не содержит ошибок
- ✅ RXNEIE бит установлен (приём включен)

**Если FAIL:**
- PE бит установлен → Ошибка чётности (конфликт конфигурации)
- RXNEIE не установлен → Приём отключен (HAL_UART_Receive_IT не сработала)

---

## 🔧 БЛОК 5: Проверка IRQ Handler (При Получении Данных)

### Тест 5.1: USART6_IRQHandler Срабатывание

**Что тестируем:** IRQ handler генерируется при приёме данных

**Шаги:**
1. Разместить breakpoint в `USART6_IRQHandler()` (stm32f4xx_it.c:297)
2. Отправить 1 байт данных с GM810 (или TTL)
3. Проверить что breakpoint срабатывает

**Ожидаемый результат:**
- ✅ USART6_IRQHandler вызывается при приёме байта

**Если FAIL:**
- IRQ не срабатывает → Проверить:
  - Вызвана ли `HAL_UART_Receive_IT()`?
  - Включено ли прерывание в NVIC?
  - Данные ли приходят на PA12 (RX)?

---

### Тест 5.2: HAL_UART_IRQHandler Выполнение

**Что тестируем:** HAL обработчик корректно обрабатывает прерывание

**Шаги:**
1. Разместить breakpoint в `HAL_UART_IRQHandler(&huart6)` (внутри IRQ handler)
2. Шагово пройти обработку
3. После выхода из HAL_UART_IRQHandler проверить состояние

**Проверяемые значения:**

После HAL_UART_IRQHandler:
```c
s_rx_byte                    // ДОЛЖНО БЫТЬ: заполнено полученным байтом
huart6.RxXferCount           // ДОЛЖНО БЫТЬ: уменьшено на 1
```

**Ожидаемый результат:**
- ✅ s_rx_byte содержит данные
- ✅ RxXferCount = 0 (перемещено)

**Если FAIL:**
- s_rx_byte пуст → Буфер не обновлён
- RxXferCount не изменился → IT обработка не выполнена

---

### Тест 5.3: HAL_UART_RxCpltCallback Вызов

**Что тестируем:** Callback генерируется после завершения приёма

**Шаги:**
1. Разместить breakpoint в `HAL_UART_RxCpltCallback()` (main.c:184)
2. Отправить данные на UART6
3. Проверить что breakpoint срабатывает

**Проверяемые значения:**

```c
huart                        // ДОЛЖНО БЫТЬ: указатель на huart6
```

**Ожидаемый результат:**
- ✅ HAL_UART_RxCpltCallback вызывается
- ✅ huart == &huart6

**Если FAIL:**
- Callback не вызывается → HAL_UART_IRQHandler не генерирует событие
- huart != &huart6 → Ошибка в коде инициализации

---

## 🔧 БЛОК 6: Проверка service_gm810_uart_rx_callback()

### Тест 6.1: service_gm810_uart_rx_callback Вызов

**Что тестируем:** GM810 callback переадресуется из глобального HAL callback

**Шаги:**
1. Разместить breakpoint в `service_gm810_uart_rx_callback()` (service_gm810_uart.c:136)
2. Отправить 1 байт на UART6
3. Проверить что breakpoint срабатывает

**Ожидаемый результат:**
- ✅ service_gm810_uart_rx_callback вызывается при приёме

**Если FAIL:**
- Callback не вызывается → Проверить:
  - Регистрируется ли в HAL_UART_RxCpltCallback?
  - Есть ли проверка `if (huart != &huart6) return;`?

---

### Тест 6.2: gm810_restart_receive_it() Перезапуск

**Что тестируем:** После обработки каждого байта, приём перезапускается

**Шаги:**
1. Разместить breakpoint в `gm810_restart_receive_it()` (service_gm810_uart.c:44)
2. Пройти несколько итераций приёма
3. Проверить что функция вызывается каждый раз

**Ожидаемый результат:**
- ✅ gm810_restart_receive_it вызывается после каждого байта
- ✅ HAL_UART_Receive_IT всегда успешна

**Если FAIL:**
- gm810_restart_receive_it не вызывается → Парсер заклинил
- HAL_UART_Receive_IT возвращает ошибку → Порт занят или ошибка

---

### Тест 6.3: Парсер State Machine

**Что тестируем:** Состояние парсера правильно обновляется

**Шаги:**
1. Отправить последовательность: [0x03] [0x05] ["HELLO"]
   - 0x03: header byte
   - 0x05: length = 5 байт
   - "HELLO": 5 байт данных
2. Проверить s_rx.state переходы

**Ожидаемое поведение:**
```c
// Получен 0x03 (header)
s_rx.state = GM810_RX_WAIT_LEN       // ✓

// Получен 0x05 (length)
s_rx.state = GM810_RX_WAIT_PAYLOAD   // ✓
s_rx.expected_len = 5                // ✓

// Получены H, E, L, L, O
s_rx.payload[0..4] = "HELLO"         // ✓
s_rx.received_len = 5                // ✓

// Завершение фрейма
gm810_publish_completed_frame_from_isr() вызов  // ✓
s_rx.state = GM810_RX_WAIT_HEADER    // ✓ (reset)
```

**Если FAIL:**
- State не меняется → Парсер не работает
- received_len не растёт → Байты не добавляются
- publish не вызывается → Фрейм не генерируется

---

## 🔧 БЛОК 7: Проверка Очереди RTOS

### Тест 7.1: Данные в myQueueToMasterHandle

**Что тестируем:** Завершённые фреймы попадают в очередь

**Шаги:**
1. Отправить корректный QR-фрейм на GM810
2. В `gm810_publish_completed_frame_from_isr()` разместить breakpoint перед `xQueueSendToFrontFromISR`
3. Проверить содержимое пакета

**Проверяемые значения:**

```c
// Перед xQueueSendToFrontFromISR:
pckt.type                    // ДОЛЖНО БЫТЬ: PACKET_QR_GM810 (0x0A)
pckt.len                     // ДОЛЖНО БЫТЬ: I2C_PACKET_QR_GM810_LEN (16)
pckt.payload[0]              // ДОЛЖНО БЫТЬ: длина данных (1-12)
pckt.payload[4..15]          // ДОЛЖНО БЫТЬ: QR-данные или "?"
```

**Ожидаемый результат:**
- ✅ xQueueSendToFrontFromISR возвращает pdTRUE
- ✅ Сообщение добавлено в очередь

**Если FAIL:**
- xQueueSendToFrontFromISR возвращает pdFALSE → Очередь полна или поломана
- Пакет не отправляется → Duplicate suppression активна?

---

### Тест 7.2: Чтение Очереди из Task

**Что тестируем:** Очередь доступна другим потокам

**Шаги:**
1. В `StartTaskRxTxI2c1()` добавить код чтения очереди
2. Отправить данные GM810
3. Проверить что задача получает сообщение

**Пример кода для диагностики:**

```c
I2cPacketToMaster_t msg;
if (xQueueReceive(myQueueToMasterHandle, &msg, portMAX_DELAY) == pdTRUE) {
    if (msg.type == PACKET_QR_GM810) {
        // ✓ Успешно получены данные GM810!
        // msg.payload содержит 16-байтный пакет
        // msg.payload[0] = длина
        // msg.payload[4..15] = данные QR кода
    }
}
```

**Ожидаемый результат:**
- ✅ Задача получает PACKET_QR_GM810
- ✅ Данные корректны

---

## 🔧 БЛОК 8: Проверка I2C Slave (ESP32 сторона)

### Тест 8.1: I2C Регистр 0x90 Содержимое

**Что тестируем:** ESP32 может читать QR-данные из I2C

**Шаги:**
1. Отправить QR-код на GM810
2. На ESP32 выполнить I2C чтение:
   ```c
   uint8_t buffer[16];
   i2c_master_read_from_slave(0x50, 0x90, buffer, 16);
   ```

**Ожидаемые значения:**

```c
buffer[0]   // ДОЛЖНО БЫТЬ: 1-12 (длина QR-данных)
buffer[1]   // ДОЛЖНО БЫТЬ: флаги (минимум 0x01)
buffer[4..] // ДОЛЖНО БЫТЬ: QR-данные или "?"
```

**Ожидаемый результат:**
- ✅ I2C чтение успешно
- ✅ Данные QR видны в буфере

**Если FAIL:**
- I2C ошибка → Проверить I2C соединение STM32-ESP32
- Данные нулевые → app_i2c_slave не обновляет регистр 0x90
- Данные старые → Новые пакеты QR не публикуются

---

## 📊 Таблица Результатов Диагностики

| БЛОК | Тест | Результат | Статус | Следующий Шаг |
|------|------|-----------|--------|---------------|
| 1 | 1.1 GPIO Init | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → Проверить #if |
| 1 | 1.2 USART6 Init | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → Check RCC |
| 1 | 1.3 RCC Clock | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → Check HAL |
| 2 | 2.1 GM810 Init | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → Check memset |
| 2 | 2.2 Queue Create | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → Check RTOS |
| 2 | 2.3 UART Start | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → Check IT |
| 3 | 3.1 GPIO Mode | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → GPIO error |
| 3 | 3.2 GPIO AF | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → AF error |
| 4 | 4.1 UART Status | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → UART error |
| 5 | 5.1 IRQ Handler | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → Data issue |
| 5 | 5.2 HAL Handler | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → Buffer issue |
| 5 | 5.3 Callback | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → Dispatch issue |
| 6 | 6.1 GM810 Callback | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → Link issue |
| 6 | 6.2 Restart IT | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → Parser stuck |
| 6 | 6.3 Parser FSM | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → FSM error |
| 7 | 7.1 Queue Write | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → Queue full |
| 7 | 7.2 Queue Read | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → Task issue |
| 8 | 8.1 I2C Slave | [ ] PASS / [ ] FAIL | [ ] | Если FAIL → I2C issue |

---

## 🎯 Краткая Справка Значений Регистров

### GPIO MODER (Mode Register)
```
00 = Input
01 = General purpose output
10 = Alternate function
11 = Analog
```

### GPIO AFR (Alternate Function Register)
```
AF8 = 0x8 (для USART6)
```

### USART SR Флаги
```
Бит 5 (RXNE) = RX Not Empty
Бит 4 (IDLE) = IDLE Line
Бит 3 (ORE)  = Overrun Error
Бит 2 (NF)   = Noise Flag
Бит 1 (FE)   = Framing Error
Бит 0 (PE)   = Parity Error
```

### USART CR1 Флаги
```
Бит 13 (UE)    = UART Enable
Бит 7 (TCIE)   = Transmission Complete IT
Бит 6 (TXEIE)  = TX Empty IT
Бит 5 (RXNEIE) = RX Not Empty IT
Бит 0 (PE)     = Parity Enable
```

---

**Совет:** Выполняйте тесты блок за блоком. Если какой-то тест FAIL, закончите на нём и исправьте проблему перед тем как идти дальше.

