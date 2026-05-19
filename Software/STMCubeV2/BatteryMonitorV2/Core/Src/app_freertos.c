/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : app_freertos.c
  * Description        : FreeRTOS applicative file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
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
#include "app_freertos.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "main.h"
#include "gpio.h"
#include "tim.h"
#include "tusb.h"
#include "direct_io.h"
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
osThreadId_t usbDeviceTaskHandle;
const osThreadAttr_t usbDeviceTask_attributes = {
  .name = "usbd",
  .priority = (osPriority_t) osPriorityAboveNormal,
  .stack_size = 256 * 4
};

osThreadId_t telemetryTaskHandle;
const osThreadAttr_t telemetryTask_attributes = {
  .name = "telemetry",
  .priority = (osPriority_t) osPriorityNormal,
  .stack_size = 256 * 4
};
/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .priority = (osPriority_t) osPriorityNormal,
  .stack_size = 128 * 4
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
void StartUsbDeviceTask(void *argument);
void StartTelemetryTask(void *argument);
/* USER CODE END FunctionPrototypes */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  usbDeviceTaskHandle = osThreadNew(StartUsbDeviceTask, NULL, &usbDeviceTask_attributes);
  telemetryTaskHandle = osThreadNew(StartTelemetryTask, NULL, &telemetryTask_attributes);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}
/* USER CODE BEGIN Header_StartDefaultTask */
/**
* @brief Function implementing the defaultTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN defaultTask */
  /* Safe boot state: relay + bleed off, fan PWM running at 0% duty. */
  relay_disable();
  bleed_disable();
  fan_init();

  /* Heartbeat loop — toggle MCU_LED at 1 Hz (500 ms half-period). */
  for(;;)
  {
    led_toggle();
    osDelay(500);
  }
  /* USER CODE END defaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* TinyUSB device task — initializes the stack then drives event processing.
 * tud_task() blocks on TinyUSB's FreeRTOS queue until the USB IRQ posts work. */
void StartUsbDeviceTask(void *argument)
{
  (void)argument;
  tud_init(0);
  for(;;)
  {
    tud_task();
  }
}

/* Telemetry task — emits a free-running ASCII counter at 1 Hz over CDC.
 * Counter increments whether a host is connected or not; we only write when it is. */
void StartTelemetryTask(void *argument)
{
  (void)argument;
  uint32_t counter = 0;
  char line[32];

  for(;;)
  {
    int n = snprintf(line, sizeof(line), "tick %lu\r\n", (unsigned long)counter++);
    if (tud_cdc_connected() && n > 0 && n < (int)sizeof(line))
    {
      tud_cdc_write(line, (uint32_t)n);
      tud_cdc_write_flush();
    }
    osDelay(1000);
  }
}

/* USER CODE END Application */

