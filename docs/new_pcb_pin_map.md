# 新版 PCB 引脚对照表

本文档用于说明新版 NFC 路灯控制器 PCB 上，固件视角对应的引脚、总线和外设关系。适合给硬件、测试和联调同事直接对照使用。

## 主要变更说明

| 项目 | 新版对应 | 固件影响 |
| --- | --- | --- |
| GPIO8 | `LEDB` | 原来代码里用于 `LED1` 的直连 GPIO，现改为 `PIN_LEDB` |
| LED1 | `PCF8574RGTR P5` | 由 `board_hal_set_led1()` 控制 |
| LED2 | `PCF8574RGTR P6` | 由 `board_hal_set_led2()` 控制 |
| GPIO15 | `RX8025T-UB INT` | 现映射为 `PIN_RTC_INT`，不再作为 ESP32 RTC 晶振相关引脚 |
| GPIO16 | `IO_IN1` | 现映射为 `PIN_IO_IN1` |
| GPIO38 | `IO_IN2` | 现映射为 `PIN_IO_IN2` |
| I2C 总线 | `PCF8574RGTR` + `RX8025T-UB` | 共用 `GPIO21/GPIO47` |

## ESP32-S3 直连 GPIO

| 信号 | GPIO | 固件宏 | 方向 | 默认 / 有效电平 | 说明 |
| --- | ---: | --- | --- | --- | --- |
| LEDB | GPIO8 | `PIN_LEDB` | 输出 | 低电平有效 | 板载指示灯，由 `board_hal_set_ledb()` 控制 |
| RTC_INT | GPIO15 | `PIN_RTC_INT` | 输入 | 外部下拉 | `RX8025T-UB` 的中断输入 |
| IO_IN1 | GPIO16 | `PIN_IO_IN1` | 输入 | 固件上拉 | 通用输入 1 |
| IO_IN2 | GPIO38 | `PIN_IO_IN2` | 输入 | 固件上拉 | 通用输入 2 |
| KEY1 | GPIO13 | `PIN_KEY1` | 输入 | 固件上拉 | 本地按键输入 |
| KEY2 | GPIO14 | `PIN_KEY2` | 输入 | 固件上拉 | 本地按键输入 |
| IO_OUT1 / OPTO1 | GPIO9 | `PIN_IO_OUT1` | 输出 | 空闲低电平，脉冲高电平 | NFC 命令脉冲输出 |
| IO_OUT2 / OPTO2 | GPIO10 | `PIN_IO_OUT2` | 输出 | 空闲低电平，脉冲高电平 | NFC 命令脉冲输出 |
| IO_OUT3 | GPIO11 | `PIN_IO_OUT3` | 输出 | 初始化为高电平 | 通用输出 |
| IO_OUT4 | GPIO12 | `PIN_IO_OUT4` | 输出 | 初始化为高电平 | 通用输出 |
| SD_DET | GPIO1 | `PIN_SD_DET` | 输入 | 上拉，低电平表示有卡 | SD 卡检测 |
| SD_D0 | GPIO2 | `PIN_SD_D0` | SDMMC | 总线信号 | SDMMC 1-bit 数据线 |
| SD_CMD | GPIO41 | `PIN_SD_CMD` | SDMMC | 总线信号 | SDMMC 命令线 |
| SD_CLK | GPIO42 | `PIN_SD_CLK` | SDMMC | 总线信号 | SDMMC 时钟 |

## I2C 总线

| 信号 | GPIO / 数值 | 固件宏 | 说明 |
| --- | ---: | --- | --- |
| SDA | GPIO21 | `PIN_I2C1_SDA` | `PCF8574RGTR` 和 `RX8025T-UB` 共用 |
| SCL | GPIO47 | `PIN_I2C1_SCL` | `PCF8574RGTR` 和 `RX8025T-UB` 共用 |
| 端口 | I2C0 | `I2C_PORT` | `I2C_NUM_0` |
| 速率 | 100 kHz | `I2C_FREQ_HZ` | 当前固件总线速率 |
| `PCF8574` 地址 | 0x20 | `PCF8574_ADDR` | 当前 IO 扩展芯片地址 |
| `RX8025T-UB` 地址 | 0x32 | `RX8025T_I2C_ADDR` | 7 位 I2C 地址；手册中的写/读传输字节为 0x64/0x65 |

## RX8025T-UB 时钟芯片

| 项目 | 固件对应 | 说明 |
| --- | --- | --- |
| I2C 地址 | `RX8025T_I2C_ADDR` = 0x32 | 固件使用 7 位地址；逻辑分析仪上可看到写字节 0x64、读字节 0x65 |
| 中断脚 | `PIN_RTC_INT` = GPIO15 | 只作为输入读取；当前固件已保留更新中断配置接口，默认启动流程不依赖该中断触发 |
| 时间寄存器 | `main/include/rtc_rx8025t.h` | 秒、分、时、星期、日、月、年按 BCD 格式读写 |
| 时间保存策略 | `main/src/rtc_rx8025t.c` | RTC 内部按 UTC 保存，网页定时任务仍通过 `CST-8` 显示和计算北京时间 |
| 掉电标志 | `VLF` / `VDET` | 读 RTC 时若发现电压异常标志，固件不会用该时间恢复系统时间 |

启动时，`time_sync` 会先初始化 `RX8025T-UB` 并读取 RTC 时间。若时间有效，立即用 `settimeofday()` 恢复系统时间，让继电器定时任务在没有网络时也能工作。之后固件同时监听 4G PPP 和 W5500 以太网，谁先拿到可用 IP 就先尝试 SNTP 校时；如果该链路校时失败，会等网络状态变化，例如另一条链路上线后再尝试。SNTP 成功后会把系统时间写回 `RX8025T-UB`，用于下一次开机恢复。

## PCF8574RGTR 扩展口

| 信号 | 扩展口位 | 固件宏 | 方向 | 有效电平 | 说明 |
| --- | ---: | --- | --- | --- | --- |
| RELAY1 | P0 | `PCF8574_RELAY1_BIT` | 输出 | 低电平闭合 | 定时控制继电器 |
| RELAY2 | P1 | `PCF8574_RELAY2_BIT` | 输出 | 低电平闭合 | 定时控制继电器 |
| RELAY3 | P2 | `PCF8574_RELAY3_BIT` | 输出 | 低电平闭合 | 上电默认释放 |
| RELAY4 | P3 | `PCF8574_RELAY4_BIT` | 输出 | 低电平闭合 | 上电默认闭合，5 类卡可打开 |
| LED1 | P5 | `PCF8574_LED1_BIT` | 输出 | 低电平点亮 | 由 `board_hal_set_led1()` 控制 |
| LED2 | P6 | `PCF8574_LED2_BIT` | 输出 | 低电平点亮 | 由 `board_hal_set_led2()` 控制 |

## PN532 NFC 串口

| 信号 | GPIO / 数值 | 固件宏 | 说明 |
| --- | ---: | --- | --- |
| MCU TX → PN532 HSU_RX | GPIO45 | `PIN_NFC_TX` | UART1 TX |
| MCU RX ← PN532 HSU_TX | GPIO48 | `PIN_NFC_RX` | UART1 RX |
| 串口端口 | UART1 | `NFC_UART_PORT` | `UART_NUM_1` |
| 波特率 | 115200 | `NFC_UART_BAUD` | 8N1 |

## W5500 以太网

| 信号 | GPIO / 数值 | 固件宏 | 说明 |
| --- | ---: | --- | --- |
| INT | GPIO4 | `PIN_W5500_INT` | 中断输入 |
| MOSI | GPIO5 | `PIN_W5500_MOSI` | SPI 数据输出 |
| MISO | GPIO6 | `PIN_W5500_MISO` | SPI 数据输入 |
| SCLK | GPIO7 | `PIN_W5500_SCLK` | SPI 时钟 |
| CS | GPIO17 | `PIN_W5500_CS` | SPI 片选 |
| RST | GPIO18 | `PIN_W5500_RST` | 芯片复位 |
| SPI host | SPI2 | `W5500_SPI_HOST` | `SPI2_HOST` |
| SPI 时钟 | 8 MHz | `W5500_SPI_CLOCK_HZ` | 运行时工作时钟 |
| 探测时钟 | 1 MHz | `W5500_SPI_PROBE_CLOCK_HZ` | 上电探测时钟 |
| PHY 地址 | 1 | `W5500_PHY_ADDR` | W5500 PHY 地址 |

## AIR780ER 4G 模块

| 信号 | GPIO | 固件宏 | 说明 |
| --- | ---: | --- | --- |
| PWRKEY | GPIO39 | `PIN_4G_PWRKEY` | 开机按键控制 |
| RESET | GPIO40 | `PIN_4G_RESET` | 复位控制 |
| USB DP | GPIO19 | `PIN_4G_USB_DP` | USB D+ |
| USB DN | GPIO20 | `PIN_4G_USB_DN` | USB D- |

## 固件对应关系

| 功能范围 | 文件 | 相关符号 |
| --- | --- | --- |
| 引脚定义 | `main/include/pin_map.h` | `PIN_LEDB`、`PIN_RTC_INT`、`PIN_IO_IN1`、`PIN_IO_IN2`、`PCF8574_*_BIT` |
| 板级接口 | `main/include/board_hal.h` | `board_hal_set_ledb()`、`board_hal_set_led1()`、`board_hal_set_led2()`、`board_inputs_t` |
| 板级初始化 | `main/src/board_hal.c` | GPIO 初始化、`PCF8574` 初始化、输入读取、LED/继电器控制 |
| 主工作任务 | `main/src/app.c` | 启动状态灯、继电器默认状态 |
| 时间同步日志 | `main/src/time_sync.c` | 说明 `RX8025T-UB` 挂在 I2C 上，`INT` 接 `GPIO15` |
| 系统默认配置 | `sdkconfig.defaults` | 已去掉 GPIO15/GPIO16 作为 ESP32 RTC 晶振的配置说明 |

## 联调检查清单

- 确认 `GPIO8` 的 `LEDB` 可以通过 `board_hal_set_ledb()` 正常点亮和熄灭。
- 确认 `LED1` 和 `LED2` 能通过 `PCF8574RGTR P5/P6` 正常控制。
- 确认 `RELAY1..4` 仍然由 `PCF8574RGTR P0..P3` 控制，且低电平闭合。
- 确认 `IO_IN1`、`IO_IN2` 和 `RTC_INT` 能在 `board_hal_read_inputs()` 中读到。
- 确认 `RX8025T-UB` 和 `PCF8574RGTR` 可以共挂在 `GPIO21/GPIO47` 这条 I2C 总线上。
- 确认 `GPIO15/GPIO16` 不再被配置成 ESP32 的 RTC 慢时钟晶振引脚。

## 4G 启动诊断

- 如果日志停在 `USB host driver installed` 之前，先查 USBH 控制器或任务创建。
- 如果停在 `USB enumerated; waiting for AT response`，说明 USB 已经起来，但 AT 通道还没通。
- 如果停在 `SIM ready; checking signal`，通常是 SIM 未插、SIM PIN 未解、或卡未就绪。
- 如果停在 `network registered; waiting for PPP dial`，说明已经注册到网络，但 PPP 拨号阶段失败。
- 如果最终打印 `Boot timeout`，最后一条 `diag` 会给出更具体的阶段和可能原因。
