/*
 * service_time_sync.h
 *
 * Сервис синхронизации времени: DS3231M RTC + BCD-утилиты.
 *
 * Источник тика:
 *   DS3231M SQW 1Hz → TCA6408A P0 (LOW) → ~INT → PC13 EXTI →
 *   myQueueTCA6408Handle → service_tca6408 → tca_handle_ds3231() →
 *   service_time_sync_on_tick(ram)
 *
 * service_time_sync_on_tick():
 *   - читает 7 BCD байт из DS3231 по I2C
 *   - копирует в ram[0x60..0x66]
 *   - ставит PACKET_TIME в myQueueToMasterHandle
 *
 * service_time_sync_from_master():
 *   - вызывается из app_i2c_slave при записи в 0x88
 *   - сравнивает master-время с RTC, при drift > 5s перезаписывает RTC
 */

#ifndef INC_SERVICE_TIME_SYNC_H_
#define INC_SERVICE_TIME_SYNC_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/*
 * Однократная инициализация DS3231M: отключить INTCN, включить SQW 1Hz.
 * Вызывать из StartDefaultTask до цикла, после создания i2c2_MutexHandle.
 */
void service_time_sync_init(void);

/*
 * Вызывать из service_tca6408 при событии TCA_P0_DS3231_1HZ.
 * Читает DS3231, копирует в ram[0x60..0x66], ставит PACKET_TIME в очередь.
 * ram — указатель на app_i2c_slave_get_ram().
 */
bool service_time_sync_on_tick(void);

/*
 * Обработать запрос синхронизации от мастера.
 * Вызывать из app_i2c_slave при записи в регистр 0x88 (I2C_REG_HW_TIME_SET_ADDR).
 * master_bcd7 — 7 BCD байт [sec,min,hour,wday,mday,mon,year].
 * rx_count    — количество принятых байт (должно быть I2C_TIME_SYNC_WRITE_LEN).
 * ram         — указатель на app_i2c_slave_get_ram() для обновления ram[0x6x].
 */
void service_time_sync_from_master(const uint8_t *master_bcd7, uint8_t rx_count);

/*
 * Проверить валидность BCD пакета времени.
 * Используется в app_i2c_slave для валидации 0x88 записи.
 */
bool service_time_sync_validate_packet(const uint8_t *buf, uint8_t len);

/*
 * Форматировать дату/время в строку "DD.MM.YY-HH:MM:SS" для OLED.
 * Результат записывается во внутренний буфер, доступный через
 * service_time_sync_get_datetime_str().
 */
void service_time_sync_datetimepack(void);

/*
 * Получить указатель на строку форматированного времени.
 * Строка действительна до следующего вызова service_time_sync_datetimepack().
 */
const char *service_time_sync_get_datetime_str(void);

/*
 * Notify the time-sync module that a PACKET_TIME entry was removed from the
 * shared queue.  Clears the coalesce flag so the next 1Hz tick can enqueue
 * a fresh TIME packet.  Call from StartTaskRxTxI2c1 immediately after
 * xQueueReceive() returns a packet of type PACKET_TIME.
 */
void service_time_sync_packet_consumed(void);

/*
 * Return the monotonic uptime counter in whole seconds, driven by the
 * DS3231 1Hz SQW signal.  Starts at 0 on power-on and increments once per
 * DS3231 tick regardless of whether the I2C register read succeeds.
 * Safe to call from any task or ISR context (volatile read).
 * Used as the clock source for packet TTL expiry.
 */
uint32_t service_time_sync_get_uptime_sec(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_SERVICE_TIME_SYNC_H_ */