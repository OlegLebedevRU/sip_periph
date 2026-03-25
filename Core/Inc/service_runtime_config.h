/*
 * service_runtime_config.h
 *
 * Runtime configuration service.
 * Единственный источник истины для параметров, которые мастер (ESP32)
 * записывает в регистры 0xE0..0xE4 после каждой обработки пакета.
 *
 * Refresh: вызывать runtime_config_apply_from_ram() раз в секунду
 * (из service_tca6408 при событии TCA_P0_DS3231_1HZ) — дополнительный
 * таймер не требуется.
 */

#ifndef INC_SERVICE_RUNTIME_CONFIG_H_
#define INC_SERVICE_RUNTIME_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* Регистровая карта Ex (0xE0..0xE4) — из docs/stm32_i2c_bus_checkup.md §12 */
#define SVC_CFG_REG_E0   (0xE0U)  /* флаги */
#define SVC_CFG_REG_E1   (0xE1U)  /* relay timing */
#define SVC_CFG_REG_E3   (0xE3U)  /* matrix freeze */
#define SVC_CFG_REG_E4   (0xE4U)  /* reader interval */

/*
 * runtime_config_t — текущие рабочие параметры периферийного контроллера.
 * Поля соответствуют регистру Ex (0xE0..0xE4), который пишет мастер.
 */
typedef struct {
    uint8_t relay_pulse_en;      /* E0.bit0: 1 = разрешить реле от внешней кнопки */
    uint8_t auth_timeout_act;    /* E0.bit1: зарезервировано */
    uint8_t auth_fail_act;       /* E0.bit2: зарезервировано */
    uint8_t relay_act_sec;       /* E1[3:0]: время активации реле, секунды */
    uint8_t relay_before_100ms;  /* E1[7:4]: предзадержка реле × 100 ms */
    uint8_t matrix_freeze_sec;   /* E3[3:0]: заморозка matrix kbd после ввода '#' */
    uint8_t reader_interval_sec; /* E4[3:0]: пауза между сканами PN532, секунды */
} runtime_config_t;

/*
 * app_context_t — контекст приложения, содержащий конфигурацию времени выполнения
 * и указатель на RAM для I2C.
 */
typedef struct {
    runtime_config_t runtime_config;
    uint8_t *i2c_ram;
} app_context_t;

/*
 * Записать значения по умолчанию в ram[] и применить конфигурацию.
 * Вызывать однократно из StartDefaultTask до osKernelStart.
 * ram — указатель на массив ram[256] из app_i2c_slave.
 */
void runtime_config_init_defaults(uint8_t *ram);

/*
 * Прочитать актуальные значения из ram[0xE0..0xE4] и обновить s_cfg.
 * Вызывать:
 *   - из service_tca6408 при TCA_P0_DS3231_1HZ (раз в секунду)
 *   - из app_i2c_slave при записи мастером в 0x30 или 0xE0
 * ram — указатель на массив ram[256].
 */
void runtime_config_apply_from_ram(const uint8_t *ram);

/*
 * Получить указатель на текущую конфигурацию (read-only).
 * Возвращает NULL до первого вызова runtime_config_init_defaults.
 */
const runtime_config_t *runtime_config_get(void);

/*
 * Получить указатель на контекст приложения (read-only).
 * Возвращает NULL до первого вызова runtime_config_init_defaults.
 */
app_context_t *app_context_get(void);
const app_context_t *app_context_get_const(void);

/*
 * Установить активный контекст приложения.
 */
void app_context_set_active(app_context_t *context);

/*
 * Привязать указатель на RAM для I2C к контексту приложения.
 */
void app_context_bind_i2c_ram(app_context_t *context, uint8_t *ram);

/*
 * Получить указатель на RAM для I2C из контекста приложения.
 */
uint8_t *app_context_i2c_ram(app_context_t *context);
const uint8_t *app_context_i2c_ram_const(const app_context_t *context);

#ifdef __cplusplus
}
#endif

#endif /* INC_SERVICE_RUNTIME_CONFIG_H_ */