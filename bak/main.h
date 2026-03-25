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
	PACKET_ACK,
	PACKET_NACK,
	PACKET_MAX
}I2cPacketType_t;
typedef struct {
	I2cPacketType_t type;
	uint8_t *payload;
	size_t len;
	uint32_t ttl;
}I2cPacketToMaster_t;
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

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define uid_ttl 2000
#define ROW1_Pin GPIO_PIN_0
#define ROW1_GPIO_Port GPIOA
#define ROW1_EXTI_IRQn EXTI0_IRQn
#define ROW2_Pin GPIO_PIN_1
#define ROW2_GPIO_Port GPIOA
#define ROW2_EXTI_IRQn EXTI1_IRQn
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
#define ROW4_EXTI_IRQn EXTI15_10_IRQn
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
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
