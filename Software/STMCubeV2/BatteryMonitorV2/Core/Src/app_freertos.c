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
#include "cdc_print.h"
#include "direct_io.h"
#include "display_controller.h"
#include "i2c_bus.h"
#include "ina238_task.h"
#include "monitor_state.h"
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
  /* cdc_print owns a mutex-protected format buffer so any task can
   * cdc_printf() (or plain printf) without its own scratch. Must come
   * before any task that logs at startup. */
  cdc_print_init();
  /* I2C1 is shared between INA238 and PN532 — i2c_bus owns the mutex + DMA
   * completion semaphore, plus the HAL transfer-complete callbacks. */
  i2c_bus_init();
  /* monitor_state owns a mutex-protected snapshot of the latest readings
   * shared between the INA238 producer task and the display consumer task. */
  monitor_state_init();
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
  /* INA238 smoke task disabled — it was an I2C-interface bring-up test,
   * not a useful runtime path yet. The driver itself (Core/Src/ina238.c)
   * stays linked so a future controller can use it directly without
   * waking this CDC-noisy logger. Re-enable when we want telemetry on
   * the CDC port for debugging. */
  /* ina238_task_start(); */
  display_controller_start();
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
  fan_set_duty(50U);
  /* Heartbeat loop — toggle MCU_LED at 1 Hz (500 ms half-period). */
  for(;;)
  {
    led_toggle();
    osDelay(500);
    fan_set_duty(50U);

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

/* Telemetry task — emits a free-running ASCII counter once a minute over CDC.
 * Counter increments whether a host is connected or not; cdc_printf drops
 * the bytes silently when no host is listening. 60 s (was 1 s) keeps debug
 * noise down while the INA238 bring-up log is in focus. */
void StartTelemetryTask(void *argument)
{
  (void)argument;
  uint32_t counter = 0;

  for(;;)
  {
    cdc_printf("tick %lu\r\n", (unsigned long)counter++);
    osDelay(60000);
  }
}

/* USER CODE END Application */

