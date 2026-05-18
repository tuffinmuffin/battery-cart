/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32c0xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

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
#define INA_ALERT_EXTI14_Pin GPIO_PIN_14
#define INA_ALERT_EXTI14_GPIO_Port GPIOC
#define INA_ALERT_EXTI14_EXTI_IRQn EXTI4_15_IRQn
#define MCU_LED_Pin GPIO_PIN_15
#define MCU_LED_GPIO_Port GPIOC
#define ADC_NTC_Pin GPIO_PIN_0
#define ADC_NTC_GPIO_Port GPIOA
#define GPIO_K1_Pin GPIO_PIN_1
#define GPIO_K1_GPIO_Port GPIOA
#define GPIO_BLEED_Pin GPIO_PIN_2
#define GPIO_BLEED_GPIO_Port GPIOA
#define FAN_PWM_Pin GPIO_PIN_3
#define FAN_PWM_GPIO_Port GPIOA
#define SW_A_Pin GPIO_PIN_5
#define SW_A_GPIO_Port GPIOA
#define SW_B_Pin GPIO_PIN_0
#define SW_B_GPIO_Port GPIOB
#define SW_C_Pin GPIO_PIN_1
#define SW_C_GPIO_Port GPIOB
#define SW_D_Pin GPIO_PIN_2
#define SW_D_GPIO_Port GPIOB
#define VSense_EXTI7_Pin GPIO_PIN_7
#define VSense_EXTI7_GPIO_Port GPIOB
#define VSense_EXTI7_EXTI_IRQn EXTI4_15_IRQn

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
