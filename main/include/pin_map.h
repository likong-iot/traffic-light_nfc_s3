#pragma once

#include "driver/gpio.h"

/*
 * Pin mapping extracted from NFC红绿灯控制系统V1.0.pdf
 * - Page 2: 主控
 * - Page 3: NFC
 * - Page 4: 网络
 * - Page 5: 输出
 */

/* Status / control */
#define PIN_LED1                  GPIO_NUM_8
#define LED_ACTIVE_LEVEL          0

/* Local key inputs */
#define PIN_KEY1                  GPIO_NUM_13
#define PIN_KEY2                  GPIO_NUM_14

/* Relay opto drive (IO_OUTx) */
#define PIN_IO_OUT1               GPIO_NUM_9
#define PIN_IO_OUT2               GPIO_NUM_10
#define PIN_IO_OUT3               GPIO_NUM_11
#define PIN_IO_OUT4               GPIO_NUM_12

/* I2C bus to PCF8574 */
#define PIN_I2C1_SDA              GPIO_NUM_21
#define PIN_I2C1_SCL              GPIO_NUM_47
#define I2C_PORT                  I2C_NUM_0
#define I2C_FREQ_HZ               100000
#define PCF8574_ADDR              0x21

/* PN532 (HSU UART) */
#define NFC_UART_PORT             UART_NUM_1
#define PIN_NFC_TX                GPIO_NUM_48 /* MCU TX -> PN532 RX */
#define PIN_NFC_RX                GPIO_NUM_45 /* MCU RX <- PN532 TX */
#define NFC_UART_BAUD             115200

/* W5500 SPI Ethernet signals */
#define PIN_W5500_INT             GPIO_NUM_4
#define PIN_W5500_MOSI            GPIO_NUM_5
#define PIN_W5500_MISO            GPIO_NUM_6
#define PIN_W5500_SCLK            GPIO_NUM_7
#define PIN_W5500_CS              GPIO_NUM_17
#define PIN_W5500_RST             GPIO_NUM_18

/* 4G module control lines (AIR780ER) */
#define PIN_4G_PWRKEY             GPIO_NUM_39
#define PIN_4G_RESET              GPIO_NUM_40
#define PIN_4G_USB_DP             GPIO_NUM_19
#define PIN_4G_USB_DN             GPIO_NUM_20

/* TF/SD lines */
#define PIN_SD_DET                GPIO_NUM_1
#define PIN_SD_D0                 GPIO_NUM_2
#define PIN_SD_CMD                GPIO_NUM_41
#define PIN_SD_CLK                GPIO_NUM_42
