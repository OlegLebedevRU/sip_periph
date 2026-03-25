/*
 * tca6408a_map.h
 *
 * TCA6408A GPIO Expander — карта входов (I2C2, addr=0x40<<1=0x80)
 * Все пины настроены как INPUT (reg 0x03 = 0xFF).
 * IRQ от TCA → STM32 EXT_INT (PC13, EXTI15_10, falling edge).
 *
 * ======================================================================
 * EVENT FLOW: DS3231 1Hz SQW → TIME packet to master
 * ======================================================================
 *
 *  DS3231M SQW pin (1Hz square wave, ~500ms LOW / ~500ms HIGH)
 *        │
 *        ▼
 *  TCA6408A P0 input ── goes LOW ──► ~INT output asserts (falling edge)
 *        │
 *        ▼
 *  STM32 PC13 (EXT_INT_Pin, GPIO_PIN_13) ── EXTI15_10_IRQn
 *        │
 *        ▼
 *  HAL_GPIO_EXTI_Callback()                          [main.c]
 *    └─► app_irq_router_exti_callback(GPIO_PIN_13)   [app_irq_router.c]
 *          └─► xQueueSendFromISR(myQueueTCA6408Handle, 1)
 *        │
 *        ▼
 *  StartTasktca6408a() ── xQueueReceive(...)          [service_tca6408.c]
 *    └─► service_tca6408_process_irq_event()
 *          ├── I2C read TCA reg[0x00] → curr_inputs
 *          ├── tca_handle_pn532(curr_inputs)          — P3 check
 *          ├── tca_handle_button(changed, curr, now)  — P2 debounce
 *          └── tca_handle_ds3231(curr_inputs, now)    — P0 1Hz tick
 *                │
 *                ▼  (first P0 LOW in cycle, s_ds_low_seen == 0)
 *          service_time_sync_on_tick(ram)              [service_time_sync.c]
 *            ├── ds3231_read_time() — I2C read 7 BCD bytes
 *            ├── memcpy → ram[0x60..0x66]
 *            └── xQueueSend(myQueueToMasterHandle, PACKET_TIME)
 *
 *  Dedup: s_ds_low_seen prevents multiple ticks per SQW cycle.
 *         Reset after P0 returns HIGH + TCA_DS_LOW_LATCH_WINDOW_MS (400ms).
 *
 * ======================================================================
 */

#ifndef INC_TCA6408A_MAP_H_
#define INC_TCA6408A_MAP_H_

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * P0 — DS3231M INT/SQW (1 Hz square wave)
 *   Источник : чип DS3231M, аппаратный осциллятор
 *   Дребезг  : НЕТ — цифровой CMOS-выход RTC
 *   Полярность: LOW в активной фазе (SQW низкий)
 *   Обработка : уровень: (gpio_ex & TCA_P0_DS3231_1HZ) == 0
 * ----------------------------------------------------------------------- */
#define TCA_P0_DS3231_1HZ     (0x01U)

/* -----------------------------------------------------------------------
 * P2 — External mechanical button (нормально разомкнута, к GND)
 *   Источник : механическая кнопка
 *   Дребезг  : ДА — может генерировать множественные IRQ TCA на одно нажатие
 *   Полярность: LOW при нажатии
 *   Обработка : перепад HIGH→LOW + debounce-окно EXT_BTN_DEBOUNCE_MS (ms);
 *               повторный клик разрешён только после отпускания (P2 → HIGH)
 * ----------------------------------------------------------------------- */
#define TCA_P2_EXT_BUTTON     (0x04U)

/* -----------------------------------------------------------------------
 * P3 — PN532 NFC reader IRQ
 *   Источник : чип PN532, уровень-LOW при наличии ответа
 *   Дребезг  : НЕТ — цифровой выход PN532
 *   Полярность: LOW активен
 *   Обработка : уровень: (gpio_ex & TCA_P3_PN532_IRQ) == 0
 * ----------------------------------------------------------------------- */
#define TCA_P3_PN532_IRQ      (0x08U)

/* Маска всех используемых входов */
#define TCA_USED_INPUTS_MASK  (TCA_P0_DS3231_1HZ | TCA_P2_EXT_BUTTON | TCA_P3_PN532_IRQ)

/* -----------------------------------------------------------------------
 * TCA6408A internal register addresses
 * ----------------------------------------------------------------------- */
#define TCA6408A_REG_INPUT    (0x00U)   /* read: текущее состояние пинов     */
#define TCA6408A_REG_OUTPUT   (0x01U)   /* write: выходной лэтч               */
#define TCA6408A_REG_POLARITY (0x02U)   /* write: инверсия полярности         */
#define TCA6408A_REG_CONFIG   (0x03U)   /* write: направление (1=вход, 0=выход) */

/* Значение для конфигурации всех пинов как входов */
#define TCA6408A_ALL_INPUTS   (0xFFU)

#ifdef __cplusplus
}
#endif

#endif /* INC_TCA6408A_MAP_H_ */
