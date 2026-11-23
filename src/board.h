// Pin map for board b43 (see docs/hardware.md).
#pragma once
#include "driver/gpio.h"

#define PIN_PEDAL1      GPIO_NUM_0   // JPED1, contact to GND, no ext pull-up
#define PIN_PEDAL2      GPIO_NUM_1   // JPED2, contact to GND, no ext pull-up
#define PIN_FACE1       GPIO_NUM_2   // BTN1 west, to GND
#define PIN_VSYS_SENSE  GPIO_NUM_3   // ADC1_CH3, VSYS/2 (schematic net "VBAT_SENSE": the divider is on VSYS, after QBAT1)
#define PIN_VBUS_SENSE  GPIO_NUM_4   // ADC1_CH4, 0.6*VBUS
#define PIN_PWR_EN      GPIO_NUM_5   // high = VSYS + 3V3_SW on
#define PIN_FACE2       GPIO_NUM_6   // BTN2 south-east, to GND
#define PIN_FACE3       GPIO_NUM_7   // BTN3 north-east, to GND
#define PIN_BOOT        GPIO_NUM_9   // BOOT1, to GND
#define PIN_LED_DIN     GPIO_NUM_10  // WS2812 chain
#define PIN_AMP_EN      GPIO_NUM_11  // NS4150C CTRL, high = on
#define PIN_PDM         GPIO_NUM_18  // PDM audio out
#define PIN_DISP_DIO1   GPIO_NUM_19
#define PIN_DISP_DIO2   GPIO_NUM_20
#define PIN_DISP_CLK1   GPIO_NUM_21
#define PIN_DISP_CLK2   GPIO_NUM_22
#define PIN_STAT_LED    GPIO_NUM_23  // red, high = on

// WS2812 chain: 0 = LED1 centre (team 1), 1 = LED2 centre (team 2),
// 2..7 = ring LED3..LED8 (NE, N, NW, SW, S, SE: anticlockwise from above),
// 8.. = external strip on JLEDX1.
#define LED_COUNT_BOARD 8
#define LED_COUNT_EXT   16
#define LED_COUNT       (LED_COUNT_BOARD + LED_COUNT_EXT)
#define LED_IDX_TEAM1   0
#define LED_IDX_TEAM2   1
#define LED_IDX_EXT     8
