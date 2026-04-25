> **Status:** archived report — оставлено для истории. Актуальные контракты см. в `docs/` (i2c_*_contract.md). Индекс документации: `docs/README.md`.

# Диагностика и тестирование TIME пакета

## Сигналы для контроля (можно подключить анализатор/осциллограф)

```
DS3231M (I2C2, адрес 0xD0)
├── INT/SQW выход
│   └── Должен генерировать 1Hz импульс
│
└── Подключен к I2C2 на STM32F411
    └── SDA = GPIO5 (GPIOB)
    └── SCL = GPIO6 (GPIOB)

TCA6408a (I2C2, адрес 0x20)
├── p0 вход (DS3231 INT/SQW connected)
│   └── При 1Hz импульсе: LOW каждую секунду
│
├── IRQ выход (открытый коллектор)
│   └── Генерирует LOW при изменении p0
│
└── Подключен к I2C2

EXT_INT (GPIO_PIN_13, GPIOC)
├── Входит от TCA6408a IRQ
│   └── Конфигурация: GPIO_MODE_IT_FALLING (падающий фронт)
│
└── Вызывает EXTI15_10_IRQHandler()
    └── Который вызывает HAL_GPIO_EXTI_Callback(GPIO_Pin)

STM32F411 Interrupt Flow:
┌─────────────────────────────────────┐
│ EXT_INT (GPIO_PIN_13) LOW           │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│ EXTI15_10_IRQHandler()              │
│ (в stm32f4xx_it.c)                  │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│ HAL_GPIO_EXTI_IRQHandler(EXT_INT)   │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│ HAL_GPIO_EXTI_Callback(GPIO_PIN_13) │ ◄── НОВАЯ ФУНКЦИЯ
│ xQueueSendFromISR(myQueueTCA6408)   │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│ StartTasktca6408a() просыпается     │
│ xQueueReceive() возвращает сигнал    │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│ tca6408a_read_reg(0x00, &gpio_ex)   │
│ Читает состояние GPIO p0            │
└──────────────┬──────────────────────┘
               │
               ├─ if(gpio_ex & 0x01 == 0)
               │  └─> p0 = 0 (INT от DS3231)
               │      ds3231_read_time(hw_time)
               │      xQueueSend(myQueueToMaster)
               │
               ├─ if(gpio_ex & 0x08 == 0)
               │  └─> p3 = 0 (PN532 IRQ)
               │      osSemaphoreRelease(pn532Sem)
               │
               └─ if(gpio_ex & 0x04 == 0)
                  └─> p2 = 0 (External button)
                      osTimerStart(rele_timer)
```

## Контрольные точки для тестирования

### 1. Аппаратная часть
- [ ] DS3231M выдает 1Hz импульс на INT/SQW
- [ ] TCA6408a получает сигнал на p0 (LOW каждую секунду)
- [ ] TCA6408a генерирует прерывание на IRQ (LOW фронт)
- [ ] STM32 видит LOW на EXT_INT (GPIO_PIN_13)

### 2. Уровень прерывания
```c
// В HAL_GPIO_EXTI_Callback добавить debug:
if (GPIO_Pin == EXT_INT_Pin)
{
    HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);  // Видно миганием
    // ...
}
```

### 3. Уровень очереди
```c
// В StartTasktca6408a добавить:
xQueueReceive(myQueueTCA6408Handle, &tca, osWaitForever);
// ... если здесь получает сигнал - очередь работает
```

### 4. Уровень TCA прочтения
```c
tca6408a_read_reg(0x00, &gpio_ex);
// gpio_ex должен показывать p0=0 при импульсе
```

### 5. Уровень I2C
```c
// Должен отправляться PACKET_TIME в очередь:
I2cPacketToMaster_t pckt = {
    .payload = hw_time,
    .len = 7,
    .type = PACKET_TIME,
    .ttl = 1000
};
xQueueSend(myQueueToMasterHandle, &pckt, 0);
```

### 6. Уровень ESP32
- [ ] На ESP32 читается адрес 0x80 каждую секунду
- [ ] Получаются 7 BCD байт времени
- [ ] В логе ESP32: `PACKET_TIME received`

## Возможные причины отсутствия TIME пакетов

| Причина | Симптом | Проверка |
|---------|---------|----------|
| DS3231 не работает | Нет импульса на INT/SQW | Осциллограф, BCD регистры DS3231 |
| TCA6408 не получает сигнал | p0 всегда HIGH | I2C диагностика TCA, адрес 0x20 |
| TCA6408 не генерирует IRQ | EXT_INT всегда HIGH | Настройка TCA (reg 0x02 - input select) |
| GPIO EXTI callback отсутствует | Очередь пуста | **✅ ИСПРАВЛЕНО** |
| Задача TCA повешена в xQueueReceive | На консоли нет сообщений | Добавить debug в callback |
| ds3231_read_time возвращает ошибку | PACKET_TIME не отправляется | Проверить I2C, адрес 0xD0 |
| Очередь к Master полная | PACKET_TIME теряется | Увеличить размер очереди |

## Логирование для отладки

Добавить в `HAL_GPIO_EXTI_Callback`:
```c
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == EXT_INT_Pin)
  {
    // Debug: считаем количество импульсов
    static uint32_t int_count = 0;
    int_count++;
    
    // Debug: миганием LED
    static uint32_t blink_div = 0;
    if ((blink_div++ & 0x01) == 0) {
        HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
    }
    
    BaseType_t prior = pdFALSE;
    uint16_t sig = 0x01;
    xQueueSendFromISR(myQueueTCA6408Handle, &sig, &prior);
    portYIELD_FROM_ISR(prior);
  }
}
```

Добавить в `StartTasktca6408a`:
```c
xQueueReceive(myQueueTCA6408Handle, &tca, osWaitForever);
printf("[TCA] Got interrupt signal (0x%04X)\n", tca);  // Debug

tca6408a_read_reg(0x00, &gpio_ex);
printf("[TCA] GPIO state: 0x%02X\n", gpio_ex);  // Debug

if((gpio_ex & 0x01)==0){
    printf("[TCA] p0 interrupt detected (DS3231)\n");  // Debug
    if(ds3231_read_time(hw_time)){
        printf("[TCA] Time read: %02X:%02X:%02X\n", hw_time[2], hw_time[1], hw_time[0]);
        I2cPacketToMaster_t pckt={...};
        xQueueSend(myQueueToMasterHandle, &pckt, 0);
        printf("[TCA] PACKET_TIME sent to master queue\n");  // Debug
    }
}
```
