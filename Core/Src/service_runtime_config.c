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
static runtime_config_t s_cfg = {0};
static uint8_t          s_initialized = 0U;

/* ---- Публичные функции -------------------------------------------------- */

void runtime_config_init_defaults(uint8_t *ram)
{
    if (ram == NULL) return;

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
    if (ram == NULL) return;

    s_cfg.relay_pulse_en      = (ram[SVC_CFG_REG_E0] & 0x01U) ? 1U : 0U;
    s_cfg.auth_timeout_act    = (ram[SVC_CFG_REG_E0] & 0x02U) ? 1U : 0U;
    s_cfg.auth_fail_act       = (ram[SVC_CFG_REG_E0] & 0x04U) ? 1U : 0U;
    s_cfg.relay_act_sec       = ram[SVC_CFG_REG_E1] & 0x0FU;
    s_cfg.relay_before_100ms  = (ram[SVC_CFG_REG_E1] >> 4U) & 0x0FU;
    s_cfg.matrix_freeze_sec   = ram[SVC_CFG_REG_E3] & 0x0FU;
    s_cfg.reader_interval_sec = ram[SVC_CFG_REG_E4] & 0x0FU;

    s_initialized = 1U;
}

const runtime_config_t *runtime_config_get(void)
{
    if (!s_initialized) return NULL;
    return &s_cfg;
}
