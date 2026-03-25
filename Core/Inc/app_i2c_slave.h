/*
 * app_i2c_slave.h
 *
 * I2C1 slave FSM wrapper.
 * Цели:
 *  - вынести I2C slave протокол из main.c
 *  - предотвратить залипание шины и clock stretching
 *  - жёстко контролировать длины входных пакетов
 *  - гарантированно возвращать FSM в LISTEN state
 */

#ifndef INC_APP_I2C_SLAVE_H_
#define INC_APP_I2C_SLAVE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "main.h"

#ifndef APP_I2C_SLAVE_AUTH_HMI_DIAG_ENABLE
#define APP_I2C_SLAVE_AUTH_HMI_DIAG_ENABLE 0
#endif

typedef struct {
    uint32_t progress_timeout_count;
    uint32_t stuck_scl_count;
    uint32_t stuck_sda_count;
    uint32_t abort_count;
    uint32_t relisten_count;
    uint32_t hard_recover_count;
    uint32_t malformed_count;
    uint32_t recover_fail_count;
    uint32_t last_recovery_ms;
    uint32_t max_recovery_ms;
    uint32_t last_progress_tick;
} app_i2c_slave_diag_t;

void app_i2c_slave_init(void);
uint8_t *app_i2c_slave_get_ram(void);
struct i2c_test_s *app_i2c_slave_get_test_state(void);
struct i2c_seq_ctrl_s *app_i2c_slave_get_seq_state(void);
const app_i2c_slave_diag_t *app_i2c_slave_get_diag(void);
void app_i2c_slave_poll_recovery(void);
void app_i2c_slave_sync_diag_to_ram(void);
uint8_t app_i2c_slave_has_errors(void);
void app_i2c_slave_format_diag_line(uint8_t index, char *buf, size_t buflen);
uint8_t app_i2c_slave_diag_line_count(void);

void app_i2c_slave_publish(const I2cPacketToMaster_t *pckt);
void StartTaskRxTxI2c1(void const *argument);
void StartTaskI2cGuard(void const *argument);

/* HAL callback delegates */
void app_i2c_slave_abort_complete(I2C_HandleTypeDef *hi2c);
void app_i2c_slave_listen_complete(I2C_HandleTypeDef *hi2c);
void app_i2c_slave_addr_callback(I2C_HandleTypeDef *hi2c, uint8_t Direction, uint16_t AddrMatch);
void app_i2c_slave_rx_complete(I2C_HandleTypeDef *hi2c);
void app_i2c_slave_tx_complete(I2C_HandleTypeDef *hi2c);
void app_i2c_slave_error(I2C_HandleTypeDef *hi2c);

#ifdef __cplusplus
}
#endif

#endif /* INC_APP_I2C_SLAVE_H_ */