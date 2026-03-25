/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include "FreeRTOS.h"
#include "service_runtime_config.h"
#include "app_irq_router.h"
#include "app_uart_dwin.h"
#include "service_time_sync.h"
#include "service_relay_actuator.h"
#include "service_matrix_kbd.h"
#include "app_i2c_slave.h"
#include "service_pn532_task.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

#ifndef rtos_require_alloc
#define rtos_require_alloc(handle_) configASSERT((handle_) != NULL)
#endif

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c1;
I2C_HandleTypeDef hi2c2;

SPI_HandleTypeDef hspi1;

TIM_HandleTypeDef htim11;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;

osThreadId defaultTaskHandle;
osThreadId myTask532Handle;
osThreadId myTaskRxTxI2c1Handle;
osThreadId myTaskOLEDHandle;
osThreadId myTaskWiegandHandle;
osThreadId myTaskHmiHandle;
osThreadId myTaskHmiMsgHandle;
osThreadId myTask_tca6408aHandle;
osThreadId myTaskI2cGuardHandle;
osMessageQId myQueueToMasterHandle;
osMessageQId myQueueOLEDHandle;
osMessageQId myQueueWiegandHandle;
osMessageQId myQueueHMIRecvRawHandle;
osMessageQId myQueueHmiMsgHandle;
osMessageQId myQueueTCA6408Handle;
osTimerId myTimerKeyHandle;
osTimerId myTimerOneSecHandle;
osTimerId myTimerWiegand_pinHandle;
osTimerId myTimerWiegand_finHandle;
osTimerId myTimerWiegand_lockHandle;
osTimerId myTimerHmiTimeoutHandle;
osTimerId myTimerHmiTtlHandle;
osTimerId myTimerReleBeforeHandle;
osTimerId myTimerReleActHandle;
osTimerId myTimerBuzzerOffHandle;
osMutexId i2c2_MutexHandle;
osSemaphoreId pn532SemaphoreHandle;
/* USER CODE BEGIN PV */
/* USER CODE END PV */

/* USER CODE BEGIN TCA6408A_HELPERS */
/* TCA6408A low-level access and business logic moved to service_tca6408.c */
/* USER CODE END TCA6408A_HELPERS */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_I2C2_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_TIM11_Init(void);
void StartDefaultTask(void const * argument);
extern void StartTask532(void const * argument);
extern void StartTaskRxTxI2c1(void const * argument);
extern void StartTaskOLED(void const * argument);
extern void StartTaskWiegand(void const * argument);
extern void StartTaskHmi(void const * argument);
extern void StartTaskHmiMsg(void const * argument);
extern void StartTasktca6408a(void const * argument);
extern void StartTaskI2cGuard(void const * argument);
extern void cb_keyTimer(void const * argument);
void cb_OneSec(void const * argument);
extern void cb_WiegandPinTimer(void const * argument);
extern void cb_WiegandFinTimer(void const * argument);
extern void cb_WiegandLock(void const * argument);
extern void cb_Hmi_Pin_Timeout(void const * argument);
extern void cb_Hmi_Ttl(void const * argument);
extern void cb_TmReleBefore(void const * argument);
extern void cb_TmReleAct(void const * argument);
extern void cb_Tm_buzzerOff(void const * argument);

/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
	app_irq_router_exti_callback(GPIO_Pin);
}
void HAL_I2C_AbortCpltCallback(I2C_HandleTypeDef *hi2c) {
	app_i2c_slave_abort_complete(hi2c);
}
void HAL_I2C_ListenCpltCallback(I2C_HandleTypeDef *hi2c) {
	app_i2c_slave_listen_complete(hi2c);
}

void HAL_I2C_SlaveRxCpltCallback(I2C_HandleTypeDef *hi2c) {
	app_i2c_slave_rx_complete(hi2c);
}
void HAL_I2C_SlaveTxCpltCallback(I2C_HandleTypeDef *hi2c) {
	app_i2c_slave_tx_complete(hi2c);
}

void HAL_I2C_AddrCallback(I2C_HandleTypeDef *hi2c, uint8_t Direction, uint16_t AddrMatch) {
	app_i2c_slave_addr_callback(hi2c, Direction, AddrMatch);
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c) {
	if (hi2c == &hi2c1) {
		app_i2c_slave_error(hi2c);
	} else if (hi2c == &hi2c2){
		HAL_I2C_DeInit(hi2c);
		HAL_I2C_Init(hi2c);
		service_pn532_notify_i2c_fault();
	}
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
	app_uart_dwin_rx_callback(huart);
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_I2C2_Init();
  MX_SPI1_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  MX_TIM11_Init();
  /* USER CODE BEGIN 2 */
	service_pn532_init();
	memset(app_i2c_slave_get_ram(), 0, 256);
  /* USER CODE END 2 */

  /* Create the mutex(es) */
  /* definition and creation of i2c2_Mutex */
  osMutexDef(i2c2_Mutex);
  i2c2_MutexHandle = osMutexCreate(osMutex(i2c2_Mutex));

  /* USER CODE BEGIN RTOS_MUTEX */
	/* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* Create the semaphores(s) */
  /* definition and creation of pn532Semaphore */
  osSemaphoreDef(pn532Semaphore);
  pn532SemaphoreHandle = osSemaphoreCreate(osSemaphore(pn532Semaphore), 1);
  rtos_require_alloc(pn532SemaphoreHandle);

  /* USER CODE BEGIN RTOS_SEMAPHORES */
	/* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* Create the timer(s) */
  /* definition and creation of myTimerKey */
  osTimerDef(myTimerKey, cb_keyTimer);
  myTimerKeyHandle = osTimerCreate(osTimer(myTimerKey), osTimerPeriodic, NULL);
  configASSERT(myTimerKeyHandle != NULL);
  rtos_require_alloc(myTimerKeyHandle);

  /* definition and creation of myTimerOneSec */
  osTimerDef(myTimerOneSec, cb_OneSec);
  myTimerOneSecHandle = osTimerCreate(osTimer(myTimerOneSec), osTimerPeriodic, NULL);
  rtos_require_alloc(myTimerOneSecHandle);

  /* definition and creation of myTimerWiegand_pin */
  osTimerDef(myTimerWiegand_pin, cb_WiegandPinTimer);
  myTimerWiegand_pinHandle = osTimerCreate(osTimer(myTimerWiegand_pin), osTimerOnce, NULL);
  rtos_require_alloc(myTimerWiegand_pinHandle);

  /* definition and creation of myTimerWiegand_fin */
  osTimerDef(myTimerWiegand_fin, cb_WiegandFinTimer);
  myTimerWiegand_finHandle = osTimerCreate(osTimer(myTimerWiegand_fin), osTimerOnce, NULL);
  rtos_require_alloc(myTimerWiegand_finHandle);

  /* definition and creation of myTimerWiegand_lock */
  osTimerDef(myTimerWiegand_lock, cb_WiegandLock);
  myTimerWiegand_lockHandle = osTimerCreate(osTimer(myTimerWiegand_lock), osTimerOnce, NULL);
  rtos_require_alloc(myTimerWiegand_lockHandle);

  /* definition and creation of myTimerHmiTimeout */
  osTimerDef(myTimerHmiTimeout, cb_Hmi_Pin_Timeout);
  myTimerHmiTimeoutHandle = osTimerCreate(osTimer(myTimerHmiTimeout), osTimerPeriodic, NULL);
  rtos_require_alloc(myTimerHmiTimeoutHandle);

  /* definition and creation of myTimerHmiTtl */
  osTimerDef(myTimerHmiTtl, cb_Hmi_Ttl);
  myTimerHmiTtlHandle = osTimerCreate(osTimer(myTimerHmiTtl), osTimerOnce, NULL);
  rtos_require_alloc(myTimerHmiTtlHandle);

  /* definition and creation of myTimerReleBefore */
  osTimerDef(myTimerReleBefore, cb_TmReleBefore);
  myTimerReleBeforeHandle = osTimerCreate(osTimer(myTimerReleBefore), osTimerOnce, NULL);
  rtos_require_alloc(myTimerReleBeforeHandle);

  /* definition and creation of myTimerReleAct */
  osTimerDef(myTimerReleAct, cb_TmReleAct);
  myTimerReleActHandle = osTimerCreate(osTimer(myTimerReleAct), osTimerOnce, NULL);
  rtos_require_alloc(myTimerReleActHandle);

  /* definition and creation of myTimerBuzzerOff */
  osTimerDef(myTimerBuzzerOff, cb_Tm_buzzerOff);
  myTimerBuzzerOffHandle = osTimerCreate(osTimer(myTimerBuzzerOff), osTimerOnce, NULL);
  rtos_require_alloc(myTimerBuzzerOffHandle);

  /* USER CODE BEGIN RTOS_TIMERS */
	/* start timers, add new ones, ... */
	osTimerStart(myTimerKeyHandle, 0);
	osTimerStart(myTimerOneSecHandle, 1000);
  /* USER CODE END RTOS_TIMERS */

  /* Create the queue(s) */
  /* definition and creation of myQueueToMaster */
  osMessageQDef(myQueueToMaster, 8, I2cPacketToMaster_t);
  myQueueToMasterHandle = osMessageCreate(osMessageQ(myQueueToMaster), NULL);
  rtos_require_alloc(myQueueToMasterHandle);

  /* definition and creation of myQueueOLED */
  osMessageQDef(myQueueOLED, 8, uint16_t);
  myQueueOLEDHandle = osMessageCreate(osMessageQ(myQueueOLED), NULL);
  rtos_require_alloc(myQueueOLEDHandle);

  /* definition and creation of myQueueWiegand */
  osMessageQDef(myQueueWiegand, 64, uint8_t);
  myQueueWiegandHandle = osMessageCreate(osMessageQ(myQueueWiegand), NULL);
  rtos_require_alloc(myQueueWiegandHandle);

  /* definition and creation of myQueueHMIRecvRaw */
  osMessageQDef(myQueueHMIRecvRaw, 32, MsgUart_t);
  myQueueHMIRecvRawHandle = osMessageCreate(osMessageQ(myQueueHMIRecvRaw), NULL);
  rtos_require_alloc(myQueueHMIRecvRawHandle);

  /* definition and creation of myQueueHmiMsg */
  osMessageQDef(myQueueHmiMsg, 8, MsgHmi_t);
  myQueueHmiMsgHandle = osMessageCreate(osMessageQ(myQueueHmiMsg), NULL);
  rtos_require_alloc(myQueueHmiMsgHandle);

  /* definition and creation of myQueueTCA6408 */
  osMessageQDef(myQueueTCA6408, 16, uint16_t);
  myQueueTCA6408Handle = osMessageCreate(osMessageQ(myQueueTCA6408), NULL);
  rtos_require_alloc(myQueueTCA6408Handle);

  /* USER CODE BEGIN RTOS_QUEUES */
	/* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* definition and creation of defaultTask */
  osThreadDef(defaultTask, StartDefaultTask, osPriorityNormal, 0, 128);
  defaultTaskHandle = osThreadCreate(osThread(defaultTask), NULL);
  rtos_require_alloc(defaultTaskHandle);

  /* definition and creation of myTask532 */
  osThreadDef(myTask532, StartTask532, osPriorityAboveNormal, 0, 256);
  myTask532Handle = osThreadCreate(osThread(myTask532), NULL);
  rtos_require_alloc(myTask532Handle);

  /* definition and creation of myTaskRxTxI2c1 */
  osThreadDef(myTaskRxTxI2c1, StartTaskRxTxI2c1, osPriorityAboveNormal, 0, 512);
  myTaskRxTxI2c1Handle = osThreadCreate(osThread(myTaskRxTxI2c1), NULL);
  rtos_require_alloc(myTaskRxTxI2c1Handle);

  /* definition and creation of myTaskOLED */
  osThreadDef(myTaskOLED, StartTaskOLED, osPriorityNormal, 0, 256);
  myTaskOLEDHandle = osThreadCreate(osThread(myTaskOLED), NULL);
  rtos_require_alloc(myTaskOLEDHandle);

  /* definition and creation of myTaskWiegand */
  osThreadDef(myTaskWiegand, StartTaskWiegand, osPriorityIdle, 0, 256);
  myTaskWiegandHandle = osThreadCreate(osThread(myTaskWiegand), NULL);
  rtos_require_alloc(myTaskWiegandHandle);

  /* definition and creation of myTaskHmi */
  osThreadDef(myTaskHmi, StartTaskHmi, osPriorityIdle, 0, 1024);
  myTaskHmiHandle = osThreadCreate(osThread(myTaskHmi), NULL);
  rtos_require_alloc(myTaskHmiHandle);

  /* definition and creation of myTaskHmiMsg */
  osThreadDef(myTaskHmiMsg, StartTaskHmiMsg, osPriorityIdle, 0, 256);
  myTaskHmiMsgHandle = osThreadCreate(osThread(myTaskHmiMsg), NULL);
  rtos_require_alloc(myTaskHmiMsgHandle);

  /* definition and creation of myTask_tca6408a */
  osThreadDef(myTask_tca6408a, StartTasktca6408a, osPriorityAboveNormal, 0, 256);
  myTask_tca6408aHandle = osThreadCreate(osThread(myTask_tca6408a), NULL);
  rtos_require_alloc(myTask_tca6408aHandle);

  /* definition and creation of myTaskI2cGuard — dedicated I2C1 bus health monitor */
  osThreadDef(myTaskI2cGuard, StartTaskI2cGuard, osPriorityHigh, 0, 256);
  myTaskI2cGuardHandle = osThreadCreate(osThread(myTaskI2cGuard), NULL);
  rtos_require_alloc(myTaskI2cGuardHandle);

  /* USER CODE BEGIN RTOS_THREADS */
	/* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
	while (1) {
		HAL_Delay(500);
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	}
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 15;
  RCC_OscInitStruct.PLL.PLLN = 144;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 5;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 400000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 34;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief I2C2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C2_Init(void)
{

  /* USER CODE BEGIN I2C2_Init 0 */

  /* USER CODE END I2C2_Init 0 */

  /* USER CODE BEGIN I2C2_Init 1 */

  /* USER CODE END I2C2_Init 1 */
  hi2c2.Instance = I2C2;
  hi2c2.Init.ClockSpeed = 400000;
  hi2c2.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c2.Init.OwnAddress1 = 0;
  hi2c2.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c2.Init.OwnAddress2 = 0;
  hi2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c2.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C2_Init 2 */

  /* USER CODE END I2C2_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief TIM11 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM11_Init(void)
{

  /* USER CODE BEGIN TIM11_Init 0 */

  /* USER CODE END TIM11_Init 0 */

  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM11_Init 1 */

  /* USER CODE END TIM11_Init 1 */
  htim11.Instance = TIM11;
  htim11.Init.Prescaler = 83;
  htim11.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim11.Init.Period = 1000;
  htim11.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim11.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim11) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim11) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM2;
  sConfigOC.Pulse = 1500;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim11, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM11_Init 2 */

  /* USER CODE END TIM11_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, TFT_CS_Pin|BUZZER_Pin|DE485_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, TFT_RST_Pin|TFT_DC_Pin|RELE1_Pin|COL1_Pin
                          |COL2_Pin|COL3_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, TFT_LED_Pin|PIN_EVENT_TO_ESP_Pin, GPIO_PIN_SET);

  /*Configure GPIO pins : EXT_INT_Pin W_D1_Pin W_D0_Pin */
  GPIO_InitStruct.Pin = EXT_INT_Pin|W_D1_Pin|W_D0_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : ROW1_Pin ROW2_Pin */
  GPIO_InitStruct.Pin = ROW1_Pin|ROW2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : TFT_CS_Pin */
  GPIO_InitStruct.Pin = TFT_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(TFT_CS_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : TFT_RST_Pin TFT_DC_Pin */
  GPIO_InitStruct.Pin = TFT_RST_Pin|TFT_DC_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : TFT_LED_Pin RELE1_Pin COL1_Pin COL2_Pin
                           COL3_Pin */
  GPIO_InitStruct.Pin = TFT_LED_Pin|RELE1_Pin|COL1_Pin|COL2_Pin
                          |COL3_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : ROW3_Pin */
  GPIO_InitStruct.Pin = ROW3_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(ROW3_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : ROW4_Pin */
  GPIO_InitStruct.Pin = ROW4_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(ROW4_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : BUZZER_Pin */
  GPIO_InitStruct.Pin = BUZZER_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(BUZZER_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : DE485_Pin */
  GPIO_InitStruct.Pin = DE485_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(DE485_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : PIN_EVENT_TO_ESP_Pin */
  GPIO_InitStruct.Pin = PIN_EVENT_TO_ESP_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(PIN_EVENT_TO_ESP_GPIO_Port, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI0_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);

  HAL_NVIC_SetPriority(EXTI1_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(EXTI1_IRQn);

  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
 * @brief  Function implementing the defaultTask thread.
 * @param  argument: Not used
 * @retval None
 */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void const * argument)
{
  /* init code for USB_DEVICE */
  MX_USB_DEVICE_Init();
  /* USER CODE BEGIN 5 */
  HAL_GPIO_WritePin(GPIOB, COL1_Pin | COL2_Pin | COL3_Pin, GPIO_PIN_RESET);
  	osTimerStart(myTimerKeyHandle, 300);
  	service_relay_actuator_init();
  	service_matrix_kbd_init();
  	app_i2c_slave_init();
  	/* Инициализировать дефолтные значения конфигурации через сервис */
  	runtime_config_init_defaults(app_i2c_slave_get_ram());
  	service_time_sync_init();
	/* Infinite loop */
	for (;;) {
		osDelay(100);
	}
  /* USER CODE END 5 */
}

/* USER CODE BEGIN Header_StartTask532 */
/**
 * @brief Function implementing the myTask532 thread.
 * @param argument: Not used
 * @retval None
 */
/* USER CODE END Header_StartTask532 */

/* StartTask532 moved to service_pn532_task.c (шаг 10 рефакторинга) */

/* USER CODE BEGIN Header_StartTaskOLED */
/**
 * @brief Function implementing the myTaskOLED thread.
 * @param argument: Not used
 * @retval None
 */
/* USER CODE END Header_StartTaskOLED */

/* StartTaskOLED moved to service_oled_task.c (шаг 11 рефакторинга) */

/* USER CODE BEGIN Header_StartTasktca6408a */
/**
* @brief Function implementing the myTask_tca6408a thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTasktca6408a */

/* StartTasktca6408a moved to service_tca6408.c (шаг 14 рефакторинга) */

/* cb_OneSec function */
void cb_OneSec(void const * argument)
{
  /* USER CODE BEGIN cb_OneSec */
	(void)argument;
	/* This callback is called every 1 second but is not used for TIME packet */
	/* TIME packets are generated by TCA6408a interrupt from DS3231 1Hz output */
  /* USER CODE END cb_OneSec */
}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM10 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM10)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
	/* User can add his own implementation to report the HAL error return state */
	__disable_irq();
	NVIC_SystemReset();
	while (1) {
	}
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
