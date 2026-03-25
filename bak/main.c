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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "pn532_com.h"
#include "ssd1306.h"
#include "ssd1306_tests.h"
#include "ssd1306_fonts.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
__IO uint32_t     Transfer_Direction = 0;
__IO uint32_t     Xfer_Complete = 0;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c1;
I2C_HandleTypeDef hi2c2;

SPI_HandleTypeDef hspi1;

osThreadId defaultTaskHandle;
osThreadId myTask532Handle;
osThreadId myTaskRxTxI2c1Handle;
osThreadId myTaskOLEDHandle;
osMessageQId myQueueToMasterHandle;
osMessageQId myQueueOLEDHandle;
osTimerId myTimerKeyHandle;
osTimerId myTimerOneSecHandle;
/* USER CODE BEGIN PV */
pn532_result_t response={};
uint8_t uid[32]={};
uint8_t slaveTxData[64] = {};
uint8_t slaveRxData;
pn532_t pn532;
// emulated I2C RAM
struct keyb{
	uint8_t buf[16];
	uint16_t offset;
};
struct keyb keyb={.buf={0},.offset=0};
uint8_t ram[256];
uint8_t date_time[19];
static uint8_t offset; 	// index of current RAM cell
static uint8_t first=1;	// first byte --> new offset
static int pn_i2c_fault=1;
uint8_t *tx_buf = NULL;
__IO size_t tx_len=0;
__IO uint8_t debounce=1;
GPIO_InitTypeDef GPIO_InitStructPrivate = {0}, GPIO_InitStructPrivate1={0};
  uint32_t previousMillis = 0;
  uint32_t currentMillis = 0;
uint8_t final_input=0;
  __IO  uint8_t key_buf_offset=0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_I2C2_Init(void);
static void MX_SPI1_Init(void);
void StartDefaultTask(void const * argument);
void StartTask532(void const * argument);
void StartTaskRxTxI2c1(void const * argument);
void StartTaskOLED(void const * argument);
void cb_keyTimer(void const * argument);
void cb_OneSec(void const * argument);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
	  uint8_t keyPressed = 0;
	  uint16_t sig1=0x01;
  currentMillis = HAL_GetTick();
//  if (currentMillis - previousMillis > 200) {
	  if(debounce){
	  previousMillis = currentMillis;
	//  keyPressed=0x20;
    /*Configure GPIO pins : ROWs to GPIO_INPUT*/
    GPIO_InitStructPrivate.Pin = ROW1_Pin|ROW2_Pin;
    GPIO_InitStructPrivate.Mode = GPIO_MODE_INPUT;
    GPIO_InitStructPrivate.Pull = GPIO_PULLDOWN;
    GPIO_InitStructPrivate.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStructPrivate);
    GPIO_InitStructPrivate.Pin = ROW3_Pin|ROW4_Pin;;
    GPIO_InitStructPrivate.Mode = GPIO_MODE_INPUT;
    GPIO_InitStructPrivate.Pull = GPIO_PULLDOWN; //GPIO_NOPULL;
    GPIO_InitStructPrivate.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStructPrivate);

    HAL_GPIO_WritePin(GPIOB, COL1_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, COL2_Pin|COL3_Pin, GPIO_PIN_RESET);

    if(GPIO_Pin == ROW1_Pin && HAL_GPIO_ReadPin(ROW1_GPIO_Port, ROW1_Pin))
    {
      keyPressed = 0x31; //ASCII value of 1
    }
    else if(GPIO_Pin == ROW2_Pin && HAL_GPIO_ReadPin(ROW2_GPIO_Port, ROW2_Pin))
    {
      keyPressed = 0x34; //ASCII value of 4
    }
    else if(GPIO_Pin == ROW3_Pin && HAL_GPIO_ReadPin(ROW3_GPIO_Port, ROW3_Pin))
    {
      keyPressed = 0x37; //ASCII value of 7
    }
    else if(GPIO_Pin == ROW4_Pin && HAL_GPIO_ReadPin(ROW4_GPIO_Port, ROW4_Pin))
    {
      keyPressed = 0x2A; //ASCII value of *
    }

    HAL_GPIO_WritePin(GPIOB, COL2_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, COL1_Pin|COL3_Pin, GPIO_PIN_RESET);

    if(GPIO_Pin == ROW1_Pin && HAL_GPIO_ReadPin(ROW1_GPIO_Port, ROW1_Pin))
    {
      keyPressed = 0x32; //ASCII value of 2
    }
    else if(GPIO_Pin == ROW2_Pin && HAL_GPIO_ReadPin(ROW2_GPIO_Port, ROW2_Pin))
    {
      keyPressed = 0x35; //ASCII value of 5
    }
    else if(GPIO_Pin == ROW3_Pin && HAL_GPIO_ReadPin(ROW3_GPIO_Port, ROW3_Pin))
    {
      keyPressed = 0x38; //ASCII value of 8
    }
    else if(GPIO_Pin == ROW4_Pin && HAL_GPIO_ReadPin(ROW4_GPIO_Port, ROW4_Pin))
    {
      keyPressed = 0x30; //ASCII value of 0
    }

    HAL_GPIO_WritePin(GPIOB, COL3_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, COL1_Pin|COL2_Pin, GPIO_PIN_RESET);

    if(GPIO_Pin == ROW1_Pin && HAL_GPIO_ReadPin(ROW1_GPIO_Port, ROW1_Pin))
    {
      keyPressed = 0x33; //ASCII value of 3
    }
    else if(GPIO_Pin == ROW2_Pin && HAL_GPIO_ReadPin(ROW2_GPIO_Port, ROW2_Pin))
    {
      keyPressed = 0x36; //ASCII value of 6
    }
    else if(GPIO_Pin == ROW3_Pin && HAL_GPIO_ReadPin(ROW3_GPIO_Port, ROW3_Pin))
    {
      keyPressed = 0x39; //ASCII value of 9
    }
    else if(GPIO_Pin == ROW4_Pin && HAL_GPIO_ReadPin(ROW4_GPIO_Port, ROW4_Pin))
    {
      keyPressed = 0x23; //ASCII value of #
    }
  if(keyPressed!=0 && keyPressed!=0x23) {
 //   if(keyb.offset>4)keyb.offset=0;
    keyb.buf[keyb.offset]=keyPressed;
    keyb.offset++;
    keyb.buf[keyb.offset]=0;
    xQueueSendFromISR(myQueueOLEDHandle, &sig1, 0);

  // if( keyb.offset>7) keyb.offset=0; see callback
    osTimerStart(myTimerKeyHandle,300);
  } else if (keyPressed==0x23) {
	  keyb.buf[keyb.offset]=keyPressed;
	      keyb.offset++;
	      keyb.buf[keyb.offset]=0;
	  I2cPacketToMaster_t pckt;
	  //pckt.payload = malloc(64);
	 // memcpy(pckt.payload, keyb.buf,keyb.offset);
	  pckt.payload=&keyb.buf[0];
	  pckt.len=keyb.offset; //slaveTxData[13]+1;
	  pckt.type=PACKET_PIN;
	  pckt.ttl=uid_ttl;
	  xQueueSendFromISR(myQueueToMasterHandle, &pckt, 0);
	  xQueueSendFromISR(myQueueOLEDHandle, &sig1, 0);
	  osTimerStart(myTimerKeyHandle,5000);
	  final_input=1;
  }
  else osTimerStart(myTimerKeyHandle,1);
    //
//
//    if(keyPressed==0x2A){
//	//keyPressed==0x23
//	keyb.offset=0;
//	memset(keyb.buf,0,16);
//	uint16_t sig0=0x01;
//	xQueueSendFromISR(myQueueOLEDHandle, &sig0, 0);
//	osTimerStart(myTimerKeyHandle,3);
//} else {
//      if(keyb.offset>10 || keyPressed==0x23) {
//      	keyb.buf[keyb.offset]=0x23;
//      	keyb.offset++;
//      	 osTimerStart(myTimerKeyHandle,3);
//      } else {
//      	keyb.buf[keyb.offset]=keyPressed;
//      	    osTimerStart(myTimerKeyHandle,5000);
//
//      	    if(keyPressed !=0x20) {
//      	    	 xQueueSendFromISR(myQueueOLEDHandle, &sig1, 0);
//      	    	 keyb.offset++;
//      	    }
//      }
//  }

//
    debounce=0;
  //  HAL_GPIO_WritePin(GPIOB, COL1_Pin|COL2_Pin|COL3_Pin, GPIO_PIN_SET);

    /*Configure GPIO pins : PB6 PB7 PB8 PB9 back to EXTI*/
//    GPIO_InitStructPrivate.Pin = ROW1_Pin|ROW2_Pin;
//    GPIO_InitStructPrivate.Mode = GPIO_MODE_IT_RISING;
//    GPIO_InitStructPrivate.Pull = GPIO_PULLDOWN;
//    HAL_GPIO_Init(GPIOA, &GPIO_InitStructPrivate);
//    /*Configure GPIO pins : PB6 PB7 PB8 PB9 back to EXTI*/
//    GPIO_InitStructPrivate.Pin = ROW3_Pin|ROW4_Pin;
//    HAL_GPIO_Init(GPIOB, &GPIO_InitStructPrivate);



  }
}



void HAL_I2C_ListenCpltCallback(I2C_HandleTypeDef *hi2c)
{
	//PRINTF("LCB\n");
	first = 1;
	HAL_I2C_EnableListen_IT(hi2c); // slave is ready again
}

void HAL_I2C_SlaveRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c == &hi2c1)
  {
	  if(first) {
	  	//	PRINTF("RXCB: offset <== %3d\n", offset );
	  		first = 0;
	  	} else {
	 // 		PRINTF("RXCB: ram[%3d] <== %3d\n", offset,  ram[offset] );
	  		offset++;
	  	}
	  	HAL_I2C_Slave_Seq_Receive_IT(hi2c, &ram[offset], 1, I2C_NEXT_FRAME);
  }
}
void HAL_I2C_MasterRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c == &hi2c2)
  {

	 // HAL_I2C_Slave_Transmit_IT(&hi2c1, slaveTxData, 7);
	  for(int i =0;i<13;i++)slaveTxData[i]=uid[i];
  }
}
void HAL_I2C_SlaveTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c == &hi2c1)
  {
	 // HAL_I2C_Slave_Transmit_IT(&hi2c1, slaveTxData, 7);
	//  HAL_I2C_Slave_Receive_IT(&hi2c1, &slaveRxData, 1);
	//  flag_i2c_reg_rx=0;
	//  HAL_GPIO_WritePin(PIN_EVENT_TO_ESP_GPIO_Port, PIN_EVENT_TO_ESP_Pin, GPIO_PIN_RESET);
	  offset++;
	  	HAL_I2C_Slave_Seq_Transmit_IT(hi2c, &ram[offset], 1, I2C_NEXT_FRAME);

	  //	  HAL_I2C_DisableListen_IT(&hi2c1);

  }
}
void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c == &hi2c2)
  {
	 // HAL_I2C_Slave_Transmit_IT(&hi2c1, slaveTxData, 7);
	//  HAL_I2C_Master_Receive_IT(&hi2c2, I2C_ADDRESS,uid, 13);
  }
}
void HAL_I2C_AddrCallback(I2C_HandleTypeDef *hi2c, uint8_t TransferDirection, uint16_t AddrMatchCode)
{
	if( TransferDirection==I2C_DIRECTION_TRANSMIT ) {
			if( first ) {
				HAL_I2C_Slave_Seq_Receive_IT(hi2c, &offset, 1, I2C_NEXT_FRAME);
			} else {
				HAL_I2C_Slave_Seq_Receive_IT(hi2c, &ram[offset], 1, I2C_NEXT_FRAME);
			}
		} else {
			HAL_I2C_Slave_Seq_Transmit_IT(hi2c, &ram[offset], 1, I2C_NEXT_FRAME);
		}
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
	//HAL_GPIO_WritePin( PA9_GPIO_Port, PA9_Pin, GPIO_PIN_SET );
	 if (hi2c == &hi2c1) {
		 HAL_GPIO_WritePin(PIN_EVENT_TO_ESP_GPIO_Port, PIN_EVENT_TO_ESP_Pin, GPIO_PIN_RESET);
		 if( HAL_I2C_GetError(hi2c)==HAL_I2C_ERROR_AF ) {
		 		// transaction terminated by master
		 	//	PRINTF("ECB end\n" );
		 		offset--;
		 	} else {
		 	//	PRINTF("ECB err=0x%02X\n", HAL_I2C_GetError(hi2c) );
		 		HAL_I2C_GetError(hi2c);
		 	}
	 } else {
		 pn_i2c_fault=1;
		 HAL_I2C_GetError(hi2c);
	 }

	//HAL_GPIO_WritePin( PA9_GPIO_Port, PA9_Pin, GPIO_PIN_RESET );
}

void HAL_I2C_AbortCpltCallback(I2C_HandleTypeDef *hi2c)
{
	HAL_GPIO_WritePin(PIN_EVENT_TO_ESP_GPIO_Port, PIN_EVENT_TO_ESP_Pin, GPIO_PIN_RESET);
//	PRINTF("aborted\n" );  // never seen...
}

uint8_t RTC_ConvertFromDec(uint8_t c)
{
  uint8_t ch = ((c>>4)*10+(0x0F&c));
        return ch;
}

uint8_t RTC_ConvertFromBinDec(uint8_t c)
{
        uint8_t ch = ((c/10)<<4)|(c%10);
        return ch;
}

void datetimepack()
{
	uint8_t tmp1,tmp2,tmp3;
	tmp1 = RTC_ConvertFromDec(ram[0x64]);
	   date_time[0]= (tmp1/10)%10 + 0x30;
	   date_time[1]= (tmp1%10) + 0x30;
	   date_time[2]='.';
	tmp2 = RTC_ConvertFromDec(ram[0x65]);
		date_time[3]= (tmp2/10)%10 + 0x30;
		date_time[4]= (tmp2%10) + 0x30;
		date_time[5]='.';
	tmp3 = RTC_ConvertFromDec(ram[0x66]);
		date_time[6]= (tmp3/10)%10 + 0x30;
		date_time[7]= (tmp3%10) + 0x30;
		date_time[8]='-';
	tmp1 = RTC_ConvertFromDec(ram[0x62]);
		date_time[9]= (tmp1/10)%10 + 0x30;
		date_time[10]= (tmp1%10) + 0x30;
		date_time[11]=':';
	tmp2 = RTC_ConvertFromDec(ram[0x61]);
		date_time[12]= (tmp2/10)%10 + 0x30;
		date_time[13]= (tmp2%10) + 0x30;
		date_time[14]=':';
	tmp3 = RTC_ConvertFromDec(ram[0x60]);
		date_time[15]= (tmp3/10)%10 + 0x30;
		date_time[16]= (tmp3%10) + 0x30;
		date_time[17]=0;
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
  /* USER CODE BEGIN 2 */
memset(&pn532,0,sizeof(pn532_t));
key_buf_offset=0;
		memset(&ram[0],0,sizeof(ram));
		memset(&date_time[0],0,sizeof(date_time));
  /* USER CODE END 2 */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* Create the timer(s) */
  /* definition and creation of myTimerKey */
  osTimerDef(myTimerKey, cb_keyTimer);
  myTimerKeyHandle = osTimerCreate(osTimer(myTimerKey), osTimerOnce, NULL);

  /* definition and creation of myTimerOneSec */
  osTimerDef(myTimerOneSec, cb_OneSec);
  myTimerOneSecHandle = osTimerCreate(osTimer(myTimerOneSec), osTimerPeriodic, NULL);

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  osTimerStart(myTimerKeyHandle,0);
  osTimerStart(myTimerOneSecHandle,1000);
  /* USER CODE END RTOS_TIMERS */

  /* Create the queue(s) */
  /* definition and creation of myQueueToMaster */
  osMessageQDef(myQueueToMaster, 8, I2cPacketToMaster_t);
  myQueueToMasterHandle = osMessageCreate(osMessageQ(myQueueToMaster), NULL);

  /* definition and creation of myQueueOLED */
  osMessageQDef(myQueueOLED, 8, uint16_t);
  myQueueOLEDHandle = osMessageCreate(osMessageQ(myQueueOLED), NULL);

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* definition and creation of defaultTask */
  osThreadDef(defaultTask, StartDefaultTask, osPriorityNormal, 0, 128);
  defaultTaskHandle = osThreadCreate(osThread(defaultTask), NULL);

  /* definition and creation of myTask532 */
  osThreadDef(myTask532, StartTask532, osPriorityAboveNormal, 0, 256);
  myTask532Handle = osThreadCreate(osThread(myTask532), NULL);

  /* definition and creation of myTaskRxTxI2c1 */
  osThreadDef(myTaskRxTxI2c1, StartTaskRxTxI2c1, osPriorityAboveNormal, 0, 512);
  myTaskRxTxI2c1Handle = osThreadCreate(osThread(myTaskRxTxI2c1), NULL);

  /* definition and creation of myTaskOLED */
  osThreadDef(myTaskOLED, StartTaskOLED, osPriorityNormal, 0, 256);
  myTaskOLEDHandle = osThreadCreate(osThread(myTaskOLED), NULL);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */
  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
//  HAL_I2C_EnableListen_IT(&hi2c1);
  HAL_GPIO_WritePin(GPIOB, COL1_Pin|COL2_Pin|COL3_Pin, GPIO_PIN_RESET);
  osTimerStart(myTimerKeyHandle,300);
//  /*Configure GPIO pins : PB6 PB7 PB8 PB9 back to EXTI*/
//   GPIO_InitStructPrivate.Pin = ROW1_Pin|ROW2_Pin;
//   GPIO_InitStructPrivate.Mode = GPIO_MODE_IT_RISING;
//   GPIO_InitStructPrivate.Pull = GPIO_PULLDOWN;
//   HAL_GPIO_Init(GPIOA, &GPIO_InitStructPrivate);
//   /*Configure GPIO pins : PB6 PB7 PB8 PB9 back to EXTI*/
//   GPIO_InitStructPrivate.Pin = ROW3_Pin|ROW4_Pin;
//   HAL_GPIO_Init(GPIOB, &GPIO_InitStructPrivate);
  osDelay(1);


//  while (1)
//  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
//  }
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
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
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

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
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
  hi2c1.Init.ClockSpeed = 100000;
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
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, TFT_RST_Pin|TFT_DC_Pin|COL1_Pin|COL2_Pin
                          |PIN_EVENT_TO_ESP_Pin|COL3_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(TFT_LED_GPIO_Port, TFT_LED_Pin, GPIO_PIN_SET);

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

  /*Configure GPIO pins : TFT_LED_Pin COL1_Pin COL2_Pin COL3_Pin */
  GPIO_InitStruct.Pin = TFT_LED_Pin|COL1_Pin|COL2_Pin|COL3_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : ROW3_Pin ROW4_Pin */
  GPIO_InitStruct.Pin = ROW3_Pin|ROW4_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

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
  /* USER CODE BEGIN 5 */

  /* Infinite loop */
  for(;;)
  {
	  osDelay(10);
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
void StartTask532(void const * argument)
{
  /* USER CODE BEGIN StartTask532 */
	 uint8_t cmd[2] = {0x01, 0x00};

		 uint8_t probe[1]={0};
		 uint8_t pn_ack[32]={0};
		 uint8_t sam[32]={};
		 uint8_t stat[32]={};
		 osDelay(100);
		// int ret;
	//	 pn532_set_normal_mode(&pn532);
		 osDelay(50);
  /* Infinite loop */
  for(;;)
  {
if(pn_i2c_fault){
	 pn532_send_command(&pn532, SAMConfiguration, cmd, 1);
			 osDelay(1);
					  do{
						 pn532_read(&pn532, probe, 1);
						 osDelay(1);
					  }while(probe[0] != 0x01);
					  probe[0]=0;
					  osDelay(1);
					  pn532_read(&pn532, pn_ack, 7);
					  osDelay(5);
					  	  probe[0]=0;
					  	 	  do{
					  	 		 pn532_read(&pn532, probe, 1);
					  	 		 osDelay(1);
					  	 	  }while(probe[0] != 0x01);
					  	 	  probe[0]=0;
					  	 	  osDelay(1);
					  	  pn532_read(&pn532, sam, 15);
					  	 osDelay(1);
			 pn532_send_command(&pn532, GetGeneralStatus, cmd, 0);
			 osDelay(1);
				  do{
					 pn532_read(&pn532, probe, 1);
					 osDelay(1);
				  }while(probe[0] != 0x01);
				  probe[0]=0;
				  osDelay(1);
				  pn532_read(&pn532, pn_ack, 7);
				  osDelay(5);
				  	  probe[0]=0;
				  	 	  do{
				  	 		 pn532_read(&pn532, probe, 1);
				  	 		 osDelay(1);
				  	 	  }while(probe[0] != 0x01);
				  	 	  probe[0]=0;
				  	 	  osDelay(1);
				  	  pn532_read(&pn532, stat, 15);
	pn_i2c_fault=0;
}

	  osDelay(10);
//	  goto test; //GetGeneralStatus // InListPassiveTarget
	  pn532_send_command(&pn532, InListPassiveTarget, cmd, 2);
	 // read_rfid();
//---------------  01:00:00:ff:00:ff:00
	memset(&pn_ack[0],0xCC,32);
	  probe[0]=0;
	  osDelay(1);
	  HAL_GPIO_WritePin(TFT_LED_GPIO_Port, TFT_LED_Pin, GPIO_PIN_RESET);
	  do{
		  pn532_read(&pn532, probe, 1);
		 osDelay(1);
	  }while(probe[0] != 0x01);
	  osDelay(1);
	  pn532_read(&pn532, pn_ack, 7);
	 // osDelay(3);
//	  pn532_read(&pn532, pn_ack, 7);
//	  if((probe[0] & 0x01)==0)continue;
	  memset(slaveTxData, 0x04,64);
	  uint8_t b0=0;
	//  osDelay(5);
	  probe[0]=0;
	 	  do{
	 		 osDelay(30);
	 		  pn532_read(&pn532, probe, 1);
	 //		 osDelay(10);
	 		HAL_I2C_Master_Transmit(&hi2c2, 0xD0, &b0, 1,1);
	 		HAL_I2C_Master_Receive(&hi2c2, 0xD0, &ram[0x60], 7,5);
	 //		 osDelay(30);
	 	  }while(probe[0] != 0x01);
	 //	  osDelay(1);
	  pn532_read(&pn532, slaveTxData, 64);
	  probe[0]=0;
//test:
//osDelay(1);

I2cPacketToMaster_t pckt;
//pckt.payload = malloc(64);
pckt.payload = &slaveTxData[13];
pckt.len=8; //slaveTxData[13]+1;
pckt.type=PACKET_UID_532;
pckt.ttl=uid_ttl;
xQueueSend(myQueueToMasterHandle, &pckt, 0);
HAL_GPIO_WritePin(TFT_LED_GPIO_Port, TFT_LED_Pin, GPIO_PIN_SET);
//ssd1306_TestAll();
uint16_t sig1=0x01;
	xQueueSend(myQueueOLEDHandle, &sig1, 0);
//	uint8_t dt[]={ 0x00,0x00,0x50,0x13,0x01,0x19,0x05,0x25};

//	 HAL_I2C_Master_Transmit(&hi2c2, 0xD0,dt,8,5);

 osDelay(200);
  }
  /* USER CODE END StartTask532 */
}

/* USER CODE BEGIN Header_StartTaskRxTxI2c1 */
/**
* @brief Function implementing the myTaskRxTxI2c1 thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskRxTxI2c1 */
void StartTaskRxTxI2c1(void const * argument)
{
  /* USER CODE BEGIN StartTaskRxTxI2c1 */
	I2cPacketToMaster_t pckt;
	//uint8_t rx1;
	if(HAL_I2C_EnableListen_IT(&hi2c1)!=HAL_OK){
		Error_Handler();
	}
  /* Infinite loop */
  for(;;)
  {
  //  osDelay(1);
    xQueueReceive(myQueueToMasterHandle, &pckt, osWaitForever);
    while(HAL_I2C_GetState(&hi2c1) != HAL_I2C_STATE_LISTEN){
    	osDelay(2);
    //	HAL_I2C_EnableListen_IT(&hi2c1);
    };
    if(1){
    	// tx_len = pckt.len;
    	 ram[0]=(uint8_t)pckt.type;
    	 if(pckt.type==PACKET_UID_532) {
    		 memcpy(&ram[1], pckt.payload, pckt.len);
    	 } else if(pckt.type==PACKET_PIN) {
    		 memcpy(&ram[0x10], pckt.payload, pckt.len);
    	//	 memset(pckt.payload, 0,pckt.len);
    	 }
    		 HAL_GPIO_WritePin(PIN_EVENT_TO_ESP_GPIO_Port, PIN_EVENT_TO_ESP_Pin, GPIO_PIN_SET);
    	 osDelay(10);
    	}// else printf("seq recv error");
  }
  /* USER CODE END StartTaskRxTxI2c1 */
}

/* USER CODE BEGIN Header_StartTaskOLED */
/**
* @brief Function implementing the myTaskOLED thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskOLED */
void StartTaskOLED(void const * argument)
{
  /* USER CODE BEGIN StartTaskOLED */
	uint16_t sig1=0;
	 osDelay(1);
	ssd1306_Init();
	ssd1306_Fill(Black);
  /* Infinite loop */
  for(;;)
  {
	  xQueueReceive(myQueueOLEDHandle, &sig1, osWaitForever);
	  char o2[16]={0};
	  if(sig1==2){
	  ssd1306_FillCircle(120, 7, 5, White);
	  datetimepack();
	  ssd1306_SetCursor(4,4);
	  ssd1306_WriteString((char*)date_time, Font_6x8, White);
	  }else if(sig1==1){
		//  ssd1306_Fill(Black);
		  ssd1306_FillCircle(120, 7, 2, Black);
		  ssd1306_FillRectangle(1,16,127,62,Black);
		  ssd1306_SetCursor(32-keyb.offset*2,32);
		  if(keyb.offset>6) {keyb.offset=0;memset(keyb.buf,0,sizeof(keyb.buf));}
		  strcpy(o2, keyb.buf);
		  ssd1306_WriteString(o2, Font_16x24, White);
	  }
	  ssd1306_UpdateScreen();
	  osDelay(1);
  }
  /* USER CODE END StartTaskOLED */
}

/* cb_keyTimer function */
void cb_keyTimer(void const * argument)
{
  /* USER CODE BEGIN cb_keyTimer */
if(final_input==1){
	memset(keyb.buf,0,sizeof(keyb.buf));
	keyb.offset=0;
	uint16_t sig1=0x01;
				xQueueSendFromISR(myQueueOLEDHandle, &sig1, 0);
				final_input=0;
}
	debounce=1;
//	if( keyb.offset>9) keyb.offset=0;
	//memset(keyb.buf,0,sizeof(keyb.buf));
	 HAL_GPIO_WritePin(GPIOB, COL1_Pin|COL2_Pin|COL3_Pin, GPIO_PIN_SET);
	  /*Configure GPIO pins : PB6 PB7 PB8 PB9 back to EXTI*/
	   GPIO_InitStructPrivate1.Pin = ROW1_Pin|ROW2_Pin;
	   GPIO_InitStructPrivate1.Mode = GPIO_MODE_IT_RISING;
	   GPIO_InitStructPrivate1.Pull = GPIO_PULLDOWN;
	   HAL_GPIO_Init(GPIOA, &GPIO_InitStructPrivate1);
	   /*Configure GPIO pins : PB6 PB7 PB8 PB9 back to EXTI*/
	   GPIO_InitStructPrivate1.Pin = ROW3_Pin|ROW4_Pin;
	   HAL_GPIO_Init(GPIOB, &GPIO_InitStructPrivate1);

  /* USER CODE END cb_keyTimer */
}

/* cb_OneSec function */
void cb_OneSec(void const * argument)
{
  /* USER CODE BEGIN cb_OneSec */
	uint16_t sig1=0x02;
			xQueueSendFromISR(myQueueOLEDHandle, &sig1, 0);
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
  if (htim->Instance == TIM10) {
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
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
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
