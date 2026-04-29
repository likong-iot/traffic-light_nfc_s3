# Bring-Up Success Params (2026-04-29)

This file records parameters that have been verified as successful in runtime logs.

## Successful Devices

1) PCF8574 (I2C IO expander)
- I2C port: `I2C_NUM_0`
- SDA: `GPIO21`
- SCL: `GPIO47`
- Speed: `100000 Hz`
- Slave address: `0x20`
- Init result: success (`PCF8574 ready @0x20`)

2) PN532 UART (HSU)
- UART port: `UART_NUM_1`
- TX: `GPIO48`
- RX: `GPIO45`
- Baud: `115200`
- Frame: `8N1`
- Flow control: disabled
- Init result: success (`NFC UART ready`)

3) Basic GPIO bring-up
- LED1: `GPIO8` (active low)
- KEY1/KEY2: `GPIO13/GPIO14` (input pull-up)
- IO_OUT1..4: `GPIO9/10/11/12` (default high)
- 4G control: `PWRKEY=GPIO39`, `RESET=GPIO40`
- W5500 control lines configured: `CS=GPIO17`, `RST=GPIO18`, `INT=GPIO4`

## Not Yet Successful (still under debug)

1) SD card
- Primary mode: SDMMC 1-bit (`DET=1`, `D0=2`, `CMD=41`, `CLK=42`)
- Fallback mode: SDSPI (`MOSI=41`, `MISO=2`, `SCLK=42`, `CS=1`)
- Current status: mount timeout / card init failed

2) W5500 Ethernet
- SPI pins: `MOSI=5`, `MISO=6`, `SCLK=7`, `CS=17`, `RST=18`, `INT=4`
- Tried combinations: host `SPI2/SPI3`, clock `4MHz/1MHz`
- Current status: `w5500_reset timeout`
