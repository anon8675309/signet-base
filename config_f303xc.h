#ifndef CONFIG_F303_CC_H
#define CONFIG_F303_CC_H
#include "types.h"
#include "regmap.h"

#define HW_REV_3 1
#define HW_BREADBOARD 2
#define HW_REV_4 3

#define CONFIG_PINOUT HW_REV_3
//#define CONFIG_PINOUT HW_BREADBOARD

#if CONFIG_PINOUT == HW_REV_4

#define BUTTON_PORT PORTA
#define BUTTON_PIN 3
#define BUTTON_HANDLER ext3_handler
#define BUTTON_IRQ EXT3_IRQ

#define STATUS_LED_PORT PORTB
#define STATUS_LED_PIN 8

#define USB_PULLUP_PORT PORTA
#define USB_PULLUP_PIN 13

#elif CONFIG_PINOUT==HW_BREADBOARD
#define BUTTON_PORT PORTA
#define BUTTON_PIN 4

#define STATUS_LED_PORT PORTB
#define STATUS_LED_PIN 12

#define USB_PULLUP_PORT PORTA
#define USB_PULLUP_PIN 13

#elif CONFIG_PINOUT == HW_REV_3

#define BUTTON_PORT PORTA
#define BUTTON_PIN 4
#define BUTTON_HANDLER ext4_handler
#define BUTTON_IRQ EXT4_IRQ

#define STATUS_LED_PORT PORTA
#define STATUS_LED_PIN 14

#define USB_PULLUP_PORT PORTB
#define USB_PULLUP_PIN 12

#endif

#define USB_DM_PORT PORTA
#define USB_DM_PIN 11
#define USB_DM_AF 14

#define USB_DP_PORT PORTA
#define USB_DP_PIN 12
#define USB_DP_AF 14

#define HSECLK 12000000
#define HCLK_DIVISOR 4
#define HCLK_DIVISOR_LOG2 2

#define PLLCLK 48000000
#define SYSCLK PLLCLK
#define HCLK (SYSCLK/HCLK_DIVISOR)
#define SYSTICK HCLK
#define PCLK HCLK
#endif
