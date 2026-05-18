/**
 * tusb_config.h — TinyUSB user configuration for BatteryMonitorV2
 *
 * Target: STM32C071 (USB_DRD_FS in device mode) using TinyUSB's stm32_fsdev DCD.
 * RTOS:   FreeRTOS via CMSIS-RTOS V2.
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/* --------------- Hardware target --------------- */
#define CFG_TUSB_MCU              OPT_MCU_STM32C0
#define CFG_TUSB_OS               OPT_OS_FREERTOS
#define CFG_TUSB_DEBUG            0

/* Memory placement / alignment for USB buffers */
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN        __attribute__((aligned(4)))
#endif

/* --------------- Device side --------------- */
#define CFG_TUD_ENABLED           1
#define CFG_TUH_ENABLED           0
#define CFG_TUD_ENDPOINT0_SIZE    64

/* CDC ACM (Virtual COM Port) */
#define CFG_TUD_CDC               1
#define CFG_TUD_CDC_RX_BUFSIZE    64
#define CFG_TUD_CDC_TX_BUFSIZE    64
#define CFG_TUD_CDC_EP_BUFSIZE    64

/* All other device classes off */
#define CFG_TUD_HID               0
#define CFG_TUD_MSC               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            0

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
