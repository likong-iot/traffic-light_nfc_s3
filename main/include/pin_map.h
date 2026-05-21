#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"

/*
 * 引脚映射整理自《NFC红绿灯控制系统V1.0.pdf》
 * - 第 2 页：主控
 * - 第 3 页：NFC
 * - 第 4 页：网络
 * - 第 5 页：输出
 */

/* 状态 / 控制 */
#define PIN_LEDB                  GPIO_NUM_8
#define LEDB_ACTIVE_LEVEL         0

/* 新版 PCB 输入 */
#define PIN_RTC_INT               GPIO_NUM_15 /* RX8025T-UB INT，外部下拉 */
#define PIN_IO_IN1                GPIO_NUM_16
#define PIN_IO_IN2                GPIO_NUM_38

/* 本地按键输入 */
#define PIN_KEY1                  GPIO_NUM_13
#define PIN_KEY2                  GPIO_NUM_14

/* 直连 GPIO 输出。IO_OUT1/2 为光耦输出：空闲低电平，触发时输出高电平脉冲。 */
#define PIN_IO_OUT1               GPIO_NUM_9
#define PIN_IO_OUT2               GPIO_NUM_10
#define PIN_IO_OUT3               GPIO_NUM_11
#define PIN_IO_OUT4               GPIO_NUM_12

/*
 * I2C 总线连接 PCF8574RGTR 和 RX8025T-UB RTC。
 * PCF8574 P0/P1/P2/P3 驱动继电器 1/2/3/4，低电平有效：
 * - LOW  = 继电器闭合
 * - HIGH = 继电器释放 / 默认上拉
 * PCF8574 P5/P6 驱动 LED1/LED2，低电平点亮。
 */
#define PIN_I2C1_SDA              GPIO_NUM_21
#define PIN_I2C1_SCL              GPIO_NUM_47
#define I2C_PORT                  I2C_NUM_0
#define I2C_FREQ_HZ               100000
#define PCF8574_ADDR              0x20
/* RX8025T-UB 的 7-bit I2C 地址；手册里的写/读传输字节为 0x64/0x65。 */
#define RX8025T_I2C_ADDR          0x32
#define PCF8574_RELAY1_BIT        0
#define PCF8574_RELAY2_BIT        1
#define PCF8574_RELAY3_BIT        2
#define PCF8574_RELAY4_BIT        3
#define PCF8574_LED1_BIT          5
#define PCF8574_LED2_BIT          6
#define PCF8574_LED_ACTIVE_LEVEL  0

/* PN532（HSU 串口） */
#define NFC_UART_PORT             UART_NUM_1
/*
 * 运行带测已确认该串口方向：
 * - ESP32 GPIO45 是 UART TX，连接 PN532 HSU_RX
 * - ESP32 GPIO48 是 UART RX，连接 PN532 HSU_TX
 */
#define PIN_NFC_TX                GPIO_NUM_45 /* MCU TX -> PN532 HSU_RX */
#define PIN_NFC_RX                GPIO_NUM_48 /* MCU RX <- PN532 HSU_TX */
#define NFC_UART_BAUD             115200

/* W5500 SPI 以太网信号 */
#define PIN_W5500_INT             GPIO_NUM_4
#define PIN_W5500_MOSI            GPIO_NUM_5
#define PIN_W5500_MISO            GPIO_NUM_6
#define PIN_W5500_SCLK            GPIO_NUM_7
#define PIN_W5500_CS              GPIO_NUM_17
#define PIN_W5500_RST             GPIO_NUM_18
#define W5500_SPI_HOST            SPI2_HOST
#define W5500_SPI_CLOCK_HZ        (8 * 1000 * 1000)
#define W5500_SPI_PROBE_CLOCK_HZ  (1 * 1000 * 1000)
#define W5500_PHY_ADDR            1

/* 4G 模块控制线（AIR780ER） */
#define PIN_4G_PWRKEY             GPIO_NUM_39
#define PIN_4G_RESET              GPIO_NUM_40
#define PIN_4G_USB_DP             GPIO_NUM_19
#define PIN_4G_USB_DN             GPIO_NUM_20

/* TF/SD 线 */
#define PIN_SD_DET                GPIO_NUM_1
#define PIN_SD_D0                 GPIO_NUM_2
#define PIN_SD_CMD                GPIO_NUM_41
#define PIN_SD_CLK                GPIO_NUM_42
