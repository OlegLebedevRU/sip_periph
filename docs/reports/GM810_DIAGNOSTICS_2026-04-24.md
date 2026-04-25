> **Status:** archived report — оставлено для истории. Актуальные контракты см. в `docs/` (i2c_*_contract.md). Индекс документации: `docs/README.md`.

# Диагностика проблемы: GM810 не получает UART события

**Дата:** 2026-04-24
**Статус:** КРИТИЧЕСКАЯ ПРОБЛЕМА НАЙДЕНА
**Влияние:** GM810 не получает прерывания от UART, `gm810_restart_receive_it()` не вызывается

---

## 1. Результаты анализа кода

### 1.1 Что было проверено

#### ✅ Инициализация USART6 (`MX_USART6_UART_Init`)
- **Статус:** Реализована корректно
- **Расположение:** `Core/Src/main.c:721`
- **Параметры:** `9600, 8N1` как в плане
- **Условие:** Выполняется если `HW_PROFILE_GM810_USART6 == 1` ✓
- **Вывод:** USART6 контроллер инициализируется

#### ✅ Инициализация service_gm810_uart
- **Статус:** Реализована корректно
- **Последовательность:**
  1. `service_gm810_uart_init()` вызывается ДО RTOS (main.c:238)
  2. `service_gm810_uart_start()` вызывается ПОСЛЕ создания очередей (main.c:366)
- **Условие:** `HW_PROFILE_GM810_USART6 == 1` ✓
- **Вывод:** Инициализация правильного порядка

#### ✅ Регистрация callback'ов UART
- **Статус:** Реализована через weak functions
- **Расположение:** `Core/Src/main.c:184-190`
- **Реализация:**
```cpp
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    app_uart_dwin_rx_callback(huart);          // DWIN (USART2)
    service_gm810_uart_rx_callback(huart);     // GM810 (USART6)
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
    if (huart == &huart2) {
        dwin_uart_start();
    } else if (huart == &huart6) {
        service_gm810_uart_error_callback(huart);
    }
}
```
- **Вывод:** Callback'ы предусмотрены и зарегистрированы

#### ✅ USART6 IRQ Handler
- **Статус:** Реализован корректно
- **Расположение:** `Core/Src/stm32f4xx_it.c:331`
- **Реализация:**
```cpp
void USART6_IRQHandler(void) {
    HAL_UART_IRQHandler(&huart6);
}
```
- **Вывод:** IRQ handler существует и вызывает HAL

#### ✅ Flag HW_PROFILE_GM810_USART6
- **Статус:** Установлен на 1
- **Расположение:** `Core/Inc/main.h:211-212`
- **Вывод:** Все условные компиляции активны

---

## 2. **КРИТИЧЕСКАЯ ПРОБЛЕМА НАЙДЕНА**

### 🔴 **GPIO пины PA11/PA12 НЕ инициализируются для USART6**

#### Описание проблемы

В функции `MX_GPIO_Init()` (main.c:739+) не выполняется инициализация GPIO пинов **PA11 и PA12** для функции **USART6_TX и USART6_RX**.

#### Текущее состояние

1. **USART6 контроллер инициализируется** (MX_USART6_UART_Init ✓)
2. **GPIO пины НЕ инициализируются** ❌
3. **Результат:** USART6 работает в режиме контроллера, но пины никогда не подключены к USART6 функции

#### Проверка в .ioc файле

```
grep "PA11\|PA12\|USART6" sip_periph.ioc
→ No results found
```

- PA11/PA12 вообще не упомянуты в конфигурации STM32CubeMX
- USART6 не добавлен в список периферии

#### Факт из планов

По плану интеграции GM810 (docs/gm810_integration_plan_2026-04-23.md):
- Раздел 2.2: "использовать `USART6`, пины STM32: `PA11/PA12`"
- Раздел 2.2: "выбор `USB` vs `USART6` на `PA11/PA12` должен быть явно зафиксирован как build-time опция"

**Проблема:** Опция создана (HW_PROFILE_GM810_USART6), но GPIO конфигурация НЕ реализована!

---

## 3. Почему это приводит к тому, что callback не вызывается

### Цепочка причин

```
┌─────────────────────────────────────┐
│ PA11/PA12 НЕ инициализированы       │ ← КРИТИЧЕСКАЯ ПРОБЛЕМА
│ как GPIO (AF для USART6)            │
└────────────────┬────────────────────┘
                 ↓
┌─────────────────────────────────────┐
│ PA11/PA12 остаются в режиме GPIO    │
│ по умолчанию (обычный input/output) │
│ или вообще не включены               │
└────────────────┬────────────────────┘
                 ↓
┌─────────────────────────────────────┐
│ USART6 RX не получает сигнал        │
│ (пины физически не соединены с      │
│  USART6 periph на уровне STM32)    │
└────────────────┬────────────────────┘
                 ↓
┌─────────────────────────────────────┐
│ USART6 не генерирует RX interrupt   │
│ (даже если данные на UART есть)     │
└────────────────┬────────────────────┘
                 ↓
┌─────────────────────────────────────┐
│ HAL_UART_IRQHandler(&huart6) НЕ     │
│ вызывается (или вызывается без      │
│ обработки данных)                   │
└────────────────┬────────────────────┘
                 ↓
┌─────────────────────────────────────┐
│ HAL_UART_RxCpltCallback НЕ вызы-    │
│ вается (нет события для callback)    │
└────────────────┬────────────────────┘
                 ↓
┌─────────────────────────────────────┐
│ service_gm810_uart_rx_callback()     │
│ НИКОГДА НЕ ВЫЗЫВАЕТСЯ ❌            │
└─────────────────────────────────────┘
```

---

## 4. План решения

### Решение 1️⃣: Ручное добавление GPIO инициализации в main.c

**Преимущества:**
- Быстрое решение
- Не требует переконфигурации STM32CubeMX
- Понятно и явно

**Недостатки:**
- STM32CubeMX может перезаписать изменения при регенерации
- Не документируется в .ioc файле

**Рекомендуется для первого тестирования ✓**

**Код для добавления в MX_GPIO_Init() перед HAL_GPIO_Init() для остальных пинов:**

```cpp
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* ... existing GPIO init code ... */

  /* USER CODE BEGIN USART6_GPIO_INIT */
#if HW_PROFILE_GM810_USART6
  /* Configure GPIO pins for USART6 */
  /* PA11: USART6_TX (AF8) */
  /* PA12: USART6_RX (AF8) */
  
  GPIO_InitStruct.Pin = GPIO_PIN_11 | GPIO_PIN_12;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF8_USART6;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
#endif
  /* USER CODE END USART6_GPIO_INIT */
}
```

### Решение 2️⃣: Добавить USART6 в STM32CubeMX конфигурацию (.ioc файл)

**Преимущества:**
- Надежное решение
- Документируется в .ioc файле
- Автоматические регенерации будут корректны

**Недостатки:**
- Требует изменения .ioc файла (может быть несовместим с другими версиями CubeMX)

**Действия:**
1. Открыть `sip_periph.ioc` в STM32CubeMX
2. Перейти в "Connectivity" → добавить "USART6"
3. Назначить PA11 → USART6_TX (AF8)
4. Назначить PA12 → USART6_RX (AF8)
5. Выбрать параметры: 9600, 8N1
6. Отключить USB_OTG_FS (чтобы не конфликтовал с PA11/PA12)
7. Регенерировать код

**Рекомендуется для окончательной фиксации ✓**

---

## 5. План диагностики (тестирование решения)

После применения решения выполнить следующие шаги:

### Шаг 1: Подтвердить инициализацию GPIO

```cpp
// Добавить в service_gm810_uart_start()
void service_gm810_uart_start(void)
{
    // Диагностический вывод
    uint32_t pa_mode_11 = GPIOA->MODER & (3U << 22);  // PA11
    uint32_t pa_mode_12 = GPIOA->MODER & (3U << 24);  // PA12
    uint32_t pa_af_11 = GPIOA->AFR[1] & (0xFU << 12); // PA11 в AFR[1]
    uint32_t pa_af_12 = GPIOA->AFR[1] & (0xFU << 16); // PA12 в AFR[1]
    
    // Должно быть:
    // pa_mode_11 == 0x00400000 (AF mode)
    // pa_mode_12 == 0x01000000 (AF mode)
    // pa_af_11 == 0x00008000 (AF8)
    // pa_af_12 == 0x00080000 (AF8)
    
    gm810_parser_reset();
    gm810_restart_receive_it();
}
```

### Шаг 2: Подтвердить UART приём

Подключить модуль GM810 к STM32 (PA11=TX, PA12=RX, GND) и проверить:

1. Отправить тестовый QR-код на GM810
2. Проверить, что USART6 генерирует прерывание (HAL_UART_IRQHandler вызывается)
3. Проверить, что `service_gm810_uart_rx_callback()` вызывается
4. Проверить, что `gm810_restart_receive_it()` вызывается в callback

### Шаг 3: Проверить данные в очереди

```cpp
// В StartTaskRxTxI2c1() проверить очередь
I2cPacketToMaster_t msg;
if (xQueueReceive(myQueueToMasterHandle, &msg, 0) == pdTRUE) {
    if (msg.type == PACKET_QR_GM810) {
        // Данные GM810 успешно попали в очередь!
        // Проверить содержимое msg.payload
    }
}
```

### Шаг 4: Проверить I2C публикацию

Подключить ESP32 и проверить:
1. ESP32 получает IRQ от STM32
2. Чтение регистра 0x00 возвращает `type = 0x0A` (PACKET_QR_GM810)
3. Чтение регистра 0x90 возвращает 16-байтный payload с QR-данными

---

## 6. Дополнительные проверки

### Проверка 1: USB конфликт

Убедиться, что конфликт USB/USART6 разрешен:

- **main.h:** `HW_PROFILE_GM810_USART6 = 1` и `HW_PROFILE_USB_OTG_FS = 0` ✓
- **usb_device.c:** USB инициализация должна быть отключена или обошла PA11/PA12

Текущее состояние:
```
#if HW_PROFILE_GM810_USART6
    #define HW_PROFILE_USB_OTG_FS        0U
#else
    #define HW_PROFILE_USB_OTG_FS        1U
#endif
```
**Статус:** ✓ Конфликта нет, логика корректна

### Проверка 2: Clock гейт для USART6

Убедиться, что RCC clock для USART6 включен:

```cpp
// В stm32f4xx_hal_uart.c::HAL_UART_MspInit()
// Должно быть:
__HAL_RCC_USART6_CLK_ENABLE();
```

**Статус:** HAL должна это сделать автоматически при HAL_UART_Init(&huart6)

---

## 7. Итоговый диагноз

| Компонент | Статус | Проблема |
|-----------|--------|---------|
| USART6 контроллер (MX_USART6_UART_Init) | ✅ OK | Нет |
| GPIO PA11/PA12 инициализация | ❌ **FAIL** | **↑ КРИТИЧЕСКАЯ ↑** |
| USART6 IRQ handler | ✅ OK | Нет |
| Callback регистрация | ✅ OK | Нет |
| service_gm810_uart логика | ✅ OK | Нет |
| HAL_Receive_IT запуск | ✅ OK | Нет |

**Главная причина:** GPIO PA11/PA12 не инициализированы для USART6 Alternate Function

**Решение:** Добавить GPIO инициализацию в MX_GPIO_Init() или через STM32CubeMX

---

## 8. Рекомендуемые дальнейшие действия

1. **НЕМЕДЛЕННО:** Применить Решение 1 (ручное добавление GPIO инициализации в main.c)
2. **Тестирование:** Выполнить Шаги 1-4 плана диагностики
3. **ЗАТЕМ:** Применить Решение 2 (добавить USART6 в .ioc через CubeMX) для надежности
4. **Документация:** Обновить docs/gm810_integration_plan_2026-04-23.md с уточнением о GPIO конфигурации
5. **Проверка:** Убедиться, что MX_GPIO_Init не перезаписывается при следующей регенерации кода из .ioc

---

**Подготовлено:** GitHub Copilot  
**Для применения:** Разработчик STM32F4  
**Срочность:** КРИТИЧЕСКАЯ - без GPIO конфигурации USART6 не будет работать
