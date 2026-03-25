/*
 * service_runtime_config.c
 *
 * Реализация сервиса рабочей конфигурации.
 * Читает параметры из регистрового пространства ram[0xE0..0xE4],
 * которые мастер (ESP32) записывает после каждой обработки пакета.
 *
 * Refresh: вызывается из StartTasktca6408a раз в секунду (TCA P0 = DS3231 1Hz).
 */

#include "service_runtime_config.h"
#include "main.h"   /* RELE1_ACT_SEC_DEFAULT, RELE1_MODE_FLAG_DEFAULT и др. */

/* ---- Статическое хранилище -------------------------------------------- */
static app_context_t     s_app_context = {0};
static app_context_t    *s_active_context = &s_app_context;
static uint8_t          s_initialized = 0U;

/* ---- Публичные функции -------------------------------------------------- */

static runtime_config_t *runtime_config_active_mut(void)
{
    if (s_active_context == NULL) {
        s_active_context = &s_app_context;
    }
    return &s_active_context->runtime_config;
}

static const runtime_config_t *runtime_config_active(void)
{
    return runtime_config_active_mut();
}

void runtime_config_init_defaults(uint8_t *ram)
{
    if (ram == NULL) return;

    app_context_bind_i2c_ram(app_context_get(), ram);

    /* Записать дефолты в регистровый образ RAM */
    /* E1[3:0] = relay_act_sec, E1[7:4] = relay_before_100ms */
    ram[SVC_CFG_REG_E1] = (ram[SVC_CFG_REG_E1] & 0xF0U)
                        | (0x0FU & RELE1_ACT_SEC_DEFAULT);
    ram[SVC_CFG_REG_E1] = (ram[SVC_CFG_REG_E1] & 0x0FU)
                        | (0xF0U & ((uint8_t)(RELE1_BEFORE_100MS_DEFAULT << 4)));

    /* E0: флаги */
    if (RELE1_MODE_FLAG_DEFAULT & 0x01U)
        ram[SVC_CFG_REG_E0] |= 0x01U;
    else
        ram[SVC_CFG_REG_E0] &= ~0x01U;

    if (RELE1_AUTH_TIMEOUT_ACT_FLAG_DEFAULT & 0x01U)
        ram[SVC_CFG_REG_E0] |= 0x02U;
    else
        ram[SVC_CFG_REG_E0] &= ~0x02U;

    if (RELE1_AUTH_FAIL_ACT_FLAG_DEFAULT & 0x01U)
        ram[SVC_CFG_REG_E0] |= 0x04U;
    else
        ram[SVC_CFG_REG_E0] &= ~0x04U;

    /* E3[3:0] = matrix_freeze_sec */
    ram[SVC_CFG_REG_E3] = (ram[SVC_CFG_REG_E3] & 0xF0U)
                        | (0x0FU & MATRIX_KEYB_FREEZE_SEC_DEFAULT);

    /* E4[3:0] = reader_interval_sec */
    ram[SVC_CFG_REG_E4] = (ram[SVC_CFG_REG_E4] & 0xF0U)
                        | (0x0FU & READER_INTERVAL_SEC_DEFAULT);

    /* Применить из только что записанного */
    runtime_config_apply_from_ram(ram);
    s_initialized = 1U;
}

void runtime_config_apply_from_ram(const uint8_t *ram)
{
    runtime_config_t *cfg = NULL;

    if (ram == NULL) return;

    cfg = runtime_config_active_mut();
    cfg->relay_pulse_en      = (ram[SVC_CFG_REG_E0] & 0x01U) ? 1U : 0U;
    cfg->auth_timeout_act    = (ram[SVC_CFG_REG_E0] & 0x02U) ? 1U : 0U;
    cfg->auth_fail_act       = (ram[SVC_CFG_REG_E0] & 0x04U) ? 1U : 0U;
    cfg->relay_act_sec       = ram[SVC_CFG_REG_E1] & 0x0FU;
    cfg->relay_before_100ms  = (ram[SVC_CFG_REG_E1] >> 4U) & 0x0FU;
    cfg->matrix_freeze_sec   = ram[SVC_CFG_REG_E3] & 0x0FU;
    cfg->reader_interval_sec = ram[SVC_CFG_REG_E4] & 0x0FU;

    s_initialized = 1U;
}

const runtime_config_t *runtime_config_get(void)
{
    if (!s_initialized) return NULL;
    return runtime_config_active();
}

app_context_t *app_context_get(void)
{
    if (s_active_context == NULL) {
        s_active_context = &s_app_context;
    }
    return s_active_context;
}

const app_context_t *app_context_get_const(void)
{
    return app_context_get();
}

void app_context_set_active(app_context_t *context)
{
    s_active_context = (context != NULL) ? context : &s_app_context;
}

void app_context_bind_i2c_ram(app_context_t *context, uint8_t *ram)
{
    app_context_t *target = (context != NULL) ? context : app_context_get();
    target->i2c_ram = ram;
}

uint8_t *app_context_i2c_ram(app_context_t *context)
{
    app_context_t *target = (context != NULL) ? context : app_context_get();
    return target->i2c_ram;
}

const uint8_t *app_context_i2c_ram_const(const app_context_t *context)
{
    const app_context_t *target = (context != NULL) ? context : app_context_get_const();
    return target->i2c_ram;
}