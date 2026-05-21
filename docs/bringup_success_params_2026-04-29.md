# 带测成功参数（2026-04-29）

本文档记录已经在运行日志中验证成功的参数，供后续联调和问题定位参考。

## 已验证成功的器件

1) `PCF8574`（I2C IO 扩展芯片）
- I2C 端口：`I2C_NUM_0`
- SDA：`GPIO21`
- SCL：`GPIO47`
- 速率：`100000 Hz`
- 从地址：`0x20`
- 初始化结果：成功（`PCF8574 ready @0x20`）
- 新版 PCB 对应关系：继电器接 `P0..P3`，`LED1/LED2` 接 `P5/P6`

2) `PN532` 串口（HSU）
- 串口端口：`UART_NUM_1`
- TX：`GPIO48`
- RX：`GPIO45`
- 波特率：`115200`
- 帧格式：`8N1`
- 流控：关闭
- 初始化结果：成功（`NFC UART ready`）

3) 基本 GPIO 带测
- `LEDB`：`GPIO8`（低电平有效）
- `LED1/LED2`：`PCF8574RGTR P5/P6`（低电平点亮）
- `KEY1/KEY2`：`GPIO13/GPIO14`（输入，上拉）
- `IO_IN1/IO_IN2`：`GPIO16/GPIO38`（输入，上拉）
- `RTC INT`：`GPIO15`，来自 `RX8025T-UB`（外部下拉）
- `IO_OUT1..4`：`GPIO9/10/11/12`（其中 `IO_OUT1/2` 为低电平空闲的光耦输出）
- 4G 控制：`PWRKEY=GPIO39`，`RESET=GPIO40`
- `W5500` 控制线：`CS=GPIO17`，`RST=GPIO18`，`INT=GPIO4`

## 仍在调试中的部分

1) `SD` 卡
- 主模式：`SDMMC` 1-bit（`DET=1`，`D0=2`，`CMD=41`，`CLK=42`）
- 备用模式：`SDSPI`（`MOSI=41`，`MISO=2`，`SCLK=42`，`CS=1`）
- 当前状态：挂载超时 / 读卡失败

2) `W5500` 以太网
- SPI 引脚：`MOSI=5`，`MISO=6`，`SCLK=7`，`CS=17`，`RST=18`，`INT=4`
- 已尝试组合：`SPI2/SPI3`，时钟 `4MHz/1MHz`
- 当前状态：`w5500_reset timeout`
