/*
 * service_oled_task.c
 *
 * OLED SSD1306 display task.
 * Перенесено из main.c (шаг 11 рефакторинга).
 *
 * Владеет:
 *   - StartTaskOLED task body
 *
 * Зависимости (extern из main.c / других модулей):
 *   - myQueueOLEDHandle
 *   - service_matrix_kbd_get_state()
 *   - app_i2c_slave_get_ram()
 *   - service_time_sync_datetimepack(), service_time_sync_get_datetime_str()
 *   - ssd1306 driver
 */

#include <string.h>
#include "main.h"
#include "cmsis_os.h"
#include "ssd1306.h"
#include "ssd1306_fonts.h"
#include "service_oled_task.h"
#include "service_matrix_kbd.h"
#include "service_time_sync.h"
#include "app_i2c_slave.h"

/* ---- extern RTOS handles from main.c ----------------------------------- */
extern osMessageQId myQueueOLEDHandle;

/* ---- FreeRTOS task body ------------------------------------------------ */

void StartTaskOLED(void const *argument)
{
	/* USER CODE BEGIN StartTaskOLED */
	uint16_t sig1 = 0;
	osDelay(1);
	ssd1306_Init();
	ssd1306_Fill(Black);
	/* Infinite loop */
	for (;;) {
		xQueueReceive(myQueueOLEDHandle, &sig1, osWaitForever);
		char o2[16] = { 0 };
		const struct keyb *kbd = service_matrix_kbd_get_state();
		uint8_t *iram = app_i2c_slave_get_ram();
		if (sig1 == 2) {
			ssd1306_FillCircle(120, 7, 5, White);
			service_time_sync_datetimepack(iram);
			ssd1306_SetCursor(4, 4);
			ssd1306_WriteString((char*) service_time_sync_get_datetime_str(), Font_6x8, White);
		} else if (sig1 == 1) {
			ssd1306_FillCircle(120, 7, 2, Black);
			ssd1306_FillRectangle(1, 16, 127, 62, Black);
			ssd1306_SetCursor(32 - kbd->offset * 2, 32);
			strcpy(o2, (char*) kbd->buf);
			ssd1306_WriteString(o2, Font_16x24, White);
		}
		ssd1306_UpdateScreen();
		osDelay(1);
	}
	/* USER CODE END StartTaskOLED */
}
