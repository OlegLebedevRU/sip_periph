/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */
typedef enum {
	PACKET_NULL,
	PACKET_UID_532,
	PACKET_PIN,
	PACKET_WIEGAND,
	PACKET_HMI,
	PACKET_TIME,
	PACKET_PIN_HMI,
	PACKET_ACK,
	PACKET_NACK,
	PACKET_ERROR,
	PACKET_QR_GM810,
	PACKET_MAX
}I2cPacketType_t;
typedef struct {
	I2cPacketType_t type;
	uint8_t *payload;
	size_t len;
	uint32_t ttl;
}I2cPacketToMaster_t;
struct i2c_test_s {
	uint8_t adr_count;
	uint8_t rcv_start;
	uint8_t rcv_cplt;
};
struct keyb {
	uint8_t buf[16];
	uint16_t offset;
};
struct i2c_seq_ctrl_s {
	uint8_t offset;
	uint8_t first;
	uint8_t second;
	uint8_t final;
	uint8_t last_base_ram_rcvd_addr;
	uint8_t rx_count;
	uint8_t tx_count;
};
#define WIEGAND8_ESQ_CODE 0x5A
#define CARDNUMLENGTH 8
#define PINUMLENGTH 6

typedef enum {
	_ZEROREADER,
	_WIEGAND,
	_WIEGAND_COLLECT_DIGITS,
	_I2C_READER,
	_TOUCH_KEYPAD,
	_API_CODE_INPUT,
	_PN532,
	_MATRIX_KEY,
	_READER_MAX
} mreader_t;
typedef struct {
	uint8_t rtype; // zero if time left
	uint8_t bitlength;
	uint8_t rdata[CARDNUMLENGTH *2];
} READER_t;
typedef struct {
	size_t psize;
//int snd_delay;
	uint8_t *uart_buf;
} MsgUart_t;
enum lock {
		UNLOCKED=0, LOCKED=1
	};
typedef struct {
	size_t psize;
	uint8_t msg_buf[12];
	uint8_t msg_ttl;
	enum lock hmi_lock;

} MsgHmi_t;
/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */
void dwin_text_output(const uint16_t inaddr, const uint8_t *text_to_hmi, size_t elen);
void hmi_show_auth_result(uint8_t auth_result);

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define TTL_PACKET_SEC  10U
#define RELE1_ACT_SEC_DEFAULT 5
#define RELE1_MODE_FLAG_DEFAULT 1
#define MATRIX_KEYB_FREEZE_SEC_DEFAULT 5
#define HOST_MSG_RAM_ADDR 0x50
#define HOST_AUTH_RESULT_RAM_ADDR 0x70
#define BUZZER_TIMER1_MS 1000
#define BUZZER_TIMER2_MS 20
#define HMI_MSG_KEY "-- KEY --"
#define READER_INTERVAL_SEC_DEFAULT 5
#define HMI_INPUT_INTERVAL_SEC_DEFAULT 10
#define HMI_AUTODELETE_SEC_DEFAULT 10
#define EXT_INT_Pin GPIO_PIN_13
#define EXT_INT_GPIO_Port GPIOC
#define EXT_INT_EXTI_IRQn EXTI15_10_IRQn
#define W_D1_Pin GPIO_PIN_14
#define W_D1_GPIO_Port GPIOC
#define W_D1_EXTI_IRQn EXTI15_10_IRQn
#define W_D0_Pin GPIO_PIN_15
#define W_D0_GPIO_Port GPIOC
#define W_D0_EXTI_IRQn EXTI15_10_IRQn
#define ROW1_Pin GPIO_PIN_0
#define ROW1_GPIO_Port GPIOA
#define ROW1_EXTI_IRQn EXTI0_IRQn
#define ROW2_Pin GPIO_PIN_1
#define ROW2_GPIO_Port GPIOA
#define ROW2_EXTI_IRQn EXTI1_IRQn
#define TX2_HMI_Pin GPIO_PIN_2
#define TX2_HMI_GPIO_Port GPIOA
#define RX2_HMI_Pin GPIO_PIN_3
#define RX2_HMI_GPIO_Port GPIOA
#define TFT_CS_Pin GPIO_PIN_4
#define TFT_CS_GPIO_Port GPIOA
#define TFT_RST_Pin GPIO_PIN_0
#define TFT_RST_GPIO_Port GPIOB
#define TFT_DC_Pin GPIO_PIN_1
#define TFT_DC_GPIO_Port GPIOB
#define TFT_LED_Pin GPIO_PIN_2
#define TFT_LED_GPIO_Port GPIOB
#define ROW3_Pin GPIO_PIN_12
#define ROW3_GPIO_Port GPIOB
#define ROW3_EXTI_IRQn EXTI15_10_IRQn
#define ROW4_Pin GPIO_PIN_13
#define ROW4_GPIO_Port GPIOB
#define RELE1_Pin GPIO_PIN_15
#define RELE1_GPIO_Port GPIOB
#define BUZZER_Pin GPIO_PIN_8
#define BUZZER_GPIO_Port GPIOA
#define TX1_485_Pin GPIO_PIN_9
#define TX1_485_GPIO_Port GPIOA
#define RX1_485_Pin GPIO_PIN_10
#define RX1_485_GPIO_Port GPIOA
#define RX6_GM810_Pin GPIO_PIN_11
#define RX6_GM810_GPIO_Port GPIOA
#define TX6_GM810_Pin GPIO_PIN_12
#define TX6_GM810_GPIO_Port GPIOA
#define DE485_Pin GPIO_PIN_15
#define DE485_GPIO_Port GPIOA
#define COL1_Pin GPIO_PIN_4
#define COL1_GPIO_Port GPIOB
#define COL2_Pin GPIO_PIN_5
#define COL2_GPIO_Port GPIOB
#define PIN_EVENT_TO_ESP_Pin GPIO_PIN_8
#define PIN_EVENT_TO_ESP_GPIO_Port GPIOB
#define COL3_Pin GPIO_PIN_9
#define COL3_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */
#define OLED_CS_Pin TFT_CS_Pin
#define OLED_CS_GPIO_Port TFT_CS_GPIO_Port
#define OLED_DC_Pin TFT_DC_Pin
#define OLED_DC_GPIO_Port TFT_DC_GPIO_Port
#define OLED_Res_Pin TFT_RST_Pin
#define OLED_Res_GPIO_Port TFT_RST_GPIO_Port
#define RELE1_BEFORE_100MS_DEFAULT 1U
#define RELE1_AUTH_TIMEOUT_ACT_FLAG_DEFAULT 1U
#define RELE1_AUTH_FAIL_ACT_FLAG_DEFAULT 1U
#define HW_PROFILE_GM810_USART6      1U
#if HW_PROFILE_GM810_USART6
#define HW_PROFILE_USB_OTG_FS        0U
#else
#define HW_PROFILE_USB_OTG_FS        1U
#endif
#define I2C_PACKET_TYPE_ADDR      0x00U
#define I2C_REG_532_ADDR          0x01U
#define I2C_REG_MATRIX_PIN_ADDR   0x10U
#define I2C_REG_WIEGAND_ADDR      0x20U
#define I2C_REG_COUNTER_ADDR      0x30U
#define I2C_REG_HMI_PIN_ADDR      0x40U
#define I2C_REG_HMI_MSG_ADDR      0x50U
#define I2C_REG_HMI_ACT_ADDR      0x70U
#define I2C_REG_HW_TIME_ADDR      0x80U
#define I2C_REG_HW_TIME_SET_ADDR  0x88U
#define I2C_REG_QR_GM810_ADDR     0x90U
#define I2C_REG_CFG_ADDR          0xE0U
#define I2C_REG_STM32_ERROR_ADDR  0xF0U
#define I2C_PACKET_UID_532_LEN    15U
#define I2C_PACKET_PIN_LEN        13U
#define I2C_PACKET_WIEGAND_LEN    15U
#define I2C_PACKET_PIN_HMI_LEN    15U
#define I2C_PACKET_TIME_LEN       8U
#define I2C_PACKET_QR_GM810_LEN   16U
#define GM810_QR_DATA_MAX_LEN     12U
#define GM810_QR_FLAG_PROTOCOL_MODE  (1U << 0)
#define GM810_QR_FLAG_RESERVED_CHUNK (1U << 1)
#define GM810_QR_FLAG_ERROR_OVERSIZE (1U << 2)
#define GM810_QR_FLAG_ERROR_NON_ASCII (1U << 3)
#define I2C_TIME_SYNC_WRITE_LEN   7U
#define TIME_SYNC_MIN_YEAR        26U
#define TIME_SYNC_MIN_MONTH       3U
#define TIME_SYNC_MIN_DAY         19U
#define TIME_SYNC_DRIFT_SEC       5U
#define DS3231_I2C_ADDR           0xD0U
#define DS3231_REG_TIME_BASE      0x00U
#define DS3231_REG_CONTROL        0x0EU
// Ex register (master -> STM32) at RAM 0xE0
#define REG_EX_ADDR                       0xE0U
#define REG_EX_RELAY_PULSE_EN_BIT         (1U << 0) // 1: allow external button relay pulse
#define REG_EX_AUTH_TIMEOUT_ACT_BIT       (1U << 1) // reserved/action flag
#define REG_EX_AUTH_FAIL_ACT_BIT          (1U << 2) // reserved/action flag
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
