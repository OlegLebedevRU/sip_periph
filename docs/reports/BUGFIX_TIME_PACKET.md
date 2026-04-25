> **Status:** archived report — оставлено для истории. Актуальные контракты см. в `docs/` (i2c_*_contract.md). Индекс документации: `docs/README.md`.

# Исправление проблемы отправки пакета TIME

## Проблема
Пакет TIME не отправлялся даже несмотря на работающий DS3231M с 1Hz выходом.

## Причина
**Отсутствовала функция `HAL_GPIO_EXTI_Callback`**, которая должна обрабатывать прерывание от TCA6408a:
- DS3231M генерирует 1Hz импульс на выходе INT/SQW
- Этот импульс подключен к **p0 входу TCA6408a** (I2C GPIO expander)
- TCA6408a генерирует **прерывание на выходе IRQ** -> вход **EXT_INT** (GPIO_PIN_13) на STM32
- **Это прерывание было НЕ обработано**, задача `StartTasktca6408a` оставалась заблокирована в `xQueueReceive()`

## Решение
Добавлена функция обработчика GPIO EXTI callback в `Core/Src/main.c`:

```c
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  /* USER CODE BEGIN GPIO_EXTI_Callback */
  if (GPIO_Pin == EXT_INT_Pin)
  {
    BaseType_t prior = pdFALSE;
    uint16_t sig = 0x01;
    xQueueSendFromISR(myQueueTCA6408Handle, &sig, &prior);
    portYIELD_FROM_ISR(prior);
  }
  /* USER CODE END GPIO_EXTI_Callback */
}
```

## Правильный поток выполнения

```
DS3231M INT/SQW (1Hz)
    ↓
TCA6408a p0 вход
    ↓
TCA6408a IRQ выход (LOW)
    ↓
EXT_INT (GPIO_PIN_13, GPIOC)
    ↓
EXTI15_10_IRQHandler() → HAL_GPIO_EXTI_Callback()
    ↓
xQueueSendFromISR(myQueueTCA6408Handle, &sig)
    ↓
StartTasktca6408a просыпается
    ↓
tca6408a_read_reg(0x00, &gpio_ex)  // Читает состояние GPIO
    ↓
if((gpio_ex & 0x01)==0)  // Проверяет p0 = 0
    ↓
ds3231_read_time(hw_time)  // Читает время
    ↓
xQueueSend(myQueueToMasterHandle, &pckt)  // Отправляет PACKET_TIME
    ↓
ESP32 читает пакет через I2C
```

## Сделанные изменения

### 1. Добавлен обработчик GPIO EXTI callback (новая функция)
Файл: `Core/Src/main.c` (~1813 строка)

Эта функция вызывается автоматически HAL когда приходит EXTI15_10 прерывание на пин EXT_INT.

### 2. Очищена функция cb_OneSec
Файл: `Core/Src/main.c` (строки 1760-1767)

Таймер 1 сек не использует очередь TCA6408. Уточнен комментарий.

## Связанные компоненты

- **DS3231M**: Real-time clock с 1Hz INT/SQW выходом
- **TCA6408a**: I2C GPIO expander, подключенный на I2C2
- **STM32F411**: Обрабатывает прерывание EXT_INT
- **ESP32**: Master, читает TIME пакеты по I2C (адрес 0x80)

## Реестр адресов TIME пакета (из контракта)
- Адрес: `0x80` 
- Длина: 8 байт (7 BCD + 1 зарезервированный)
- Формат: `sec, min, hour, wday, mday, mon, year` (в BCD)
- `wday`: Понедельник=1, Воскресенье=7

## Тестирование
1. Убедиться что DS3231M генерирует 1Hz импульс на INT/SQW
2. Проверить что TCA6408a получает сигнал на p0
3. Проверить что STM32 получает прерывания EXT_INT (можно через индикатор LED)
4. Убедиться что пакеты TIME отправляются в I2C каждую секунду
5. На ESP32 проверить в логах что PACKET_TIME регулярно приходит

