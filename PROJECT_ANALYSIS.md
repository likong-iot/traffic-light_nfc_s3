# 交通信号灯NFC系统 - 项目分析报告

生成时间：2026-06-02

## 项目概览

**项目名称：** traffic-light_nfc_s3  
**版本：** 1.0.0  
**目标平台：** ESP32-S3  
**开发框架：** ESP-IDF v5.5  

## 项目描述

这是一个基于ESP32-S3的智能交通信号灯控制系统，集成了NFC读卡、雷达感应、以太网连接、4G通信、SD卡存储等多种功能。系统支持Web配置界面和OTA无线升级。

## 硬件配置

### 核心芯片
- **MCU：** ESP32-S3 (Xtensa双核处理器)
- **Flash分区：** 双OTA分区设计，每个分区3.5MB

### 外设模块

1. **NFC读卡器**
   - 芯片：PN532
   - 接口：HSU (UART1)
   - 引脚：TX=GPIO45, RX=GPIO48
   - 波特率：115200

2. **以太网**
   - 芯片：W5500
   - 接口：SPI2
   - 时钟：8MHz
   - 引脚：MOSI=GPIO5, MISO=GPIO6, SCLK=GPIO7, CS=GPIO17, RST=GPIO18

3. **SD卡存储**
   - 接口：SDMMC (1-line mode)
   - 引脚：D0=GPIO2, CMD=GPIO41, CLK=GPIO42, DET=GPIO1

4. **4G模块**
   - 型号：AIR780ER
   - 接口：USB OTG
   - 控制引脚：PWRKEY=GPIO39, RESET=GPIO40

5. **RTC时钟**
   - 芯片：RX8025T-UB
   - 接口：I2C (SDA=GPIO21, SCL=GPIO47)

6. **I/O扩展**
   - 芯片：PCF8574 (I2C地址0x20)
   - 功能：控制4路继电器 + 2路LED

7. **GPIO输出**
   - 4路直连GPIO输出 (GPIO9-12)
   - 2路光耦输出 (空闲低电平，触发高电平脉冲)

8. **输入接口**
   - 2路本地按键 (GPIO13, GPIO14)
   - 2路PCB输入 (GPIO16, GPIO38)
   - RTC中断输入 (GPIO15)
   - 雷达输入 (通过IO_IN1)

## 软件架构

### 核心模块（28个源文件，约5605行代码）

1. **main.c / app.c** - 主程序和应用逻辑
   - 设备初始化流程
   - NFC卡片工作流程
   - 继电器调度控制
   - 日志记录系统

2. **nfc_pn532.c** - NFC读卡模块
   - 支持MIFARE Classic和NTAG卡片
   - 后台扫描任务
   - 卡片UID和数据块读取

3. **radar_input.c** - 雷达输入处理
   - 防抖和干扰过滤
   - 周期触发检测
   - 锁定机制

4. **web_config.c** - Web配置界面
   - AP模式配置服务器
   - 登录认证系统
   - JSON配置API
   - OTA升级进度显示

5. **ota_update.c** - OTA升级功能
   - 双分区AB升级
   - 下载进度跟踪
   - 自动回滚保护

6. **app_config.c** - 配置管理
   - NVS持久化存储
   - SD卡配置文件支持
   - NFC规则管理（最多16条）
   - 雷达参数配置

7. **board_hal.c** - 硬件抽象层
   - GPIO控制
   - I2C通信
   - PCF8574驱动
   - 继电器和LED控制

8. **net_eth.c** - 以太网驱动
   - W5500 SPI驱动
   - TCP/IP栈集成
   - 链路状态监控

9. **storage_sd.c** - SD卡存储
   - FAT文件系统
   - 日志文件管理
   - 配置文件读取

10. **modem_4g.c** - 4G模块控制
    - USB CDC通信
    - AT命令接口
    - 后台状态机

11. **time_sync.c** - 时间同步
    - RTC硬件时钟
    - NTP网络同步
    - 时区支持（CST-8）

## 构建分析

### 固件大小

```
Bootloader:  21KB  (35% free)
Application: 1.4MB (61% free, 使用39%)
OTA分区:     3.5MB (每个分区)
```

### 内存使用

| 内存类型 | 已用 | 占用率 | 剩余 | 总计 |
|---------|------|--------|------|------|
| Flash代码 | 954KB | - | - | - |
| Flash数据 | 329KB | - | - | - |
| DIRAM | 139KB | 40.5% | 203KB | 342KB |
| IRAM | 16KB | 100% | 0 | 16KB |
| RTC FAST | 56B | 0.7% | 8KB | 8KB |

**总镜像大小：** 1,418,105 字节

### 主要组件占用（Top 15）

1. esp_app_format: 170KB (主要是证书数据)
2. net80211: 144KB (WiFi协议栈)
3. lwip: 133KB (TCP/IP协议栈)
4. mbedtls: 100KB (TLS库)
5. libc: 97KB (标准C库)
6. mbedcrypto: 80KB (加密库)
7. wpa_supplicant: 66KB (WiFi安全)
8. pp: 64KB (物理层)
9. main: 62KB (应用代码)
10. esp_hw_support: 38KB
11. phy: 36KB
12. usb: 35KB
13. hal: 28KB
14. freertos: 23KB
15. fatfs: 19KB

## 功能特性

### NFC卡片管理
- 支持6类卡片配置（可扩展至16类）
- 每类卡片可配置名称和触发脉冲数
- 卡片刷卡记录自动保存到SD卡
- 支持UID和数据块读取

### 雷达感应
- 可配置触发电平和延时
- 防干扰过滤（连续干扰锁定）
- 周期窗口检测
- 触发后自动锁定300秒

### 继电器调度
- 4路继电器独立控制
- 时间段调度（支持跨午夜）
- RTC时钟同步
- 低电平触发（闭合）

### Web配置界面
- AP模式热点配置
- 登录认证保护
- 实时配置修改
- OTA升级界面（带进度条）
- 配置导出/导入（JSON格式）

### 日志系统
- NFC刷卡日志按天存储
- 自动清理200天前日志
- SD卡故障保护
- 队列缓冲防丢失

### 时间同步
- RTC硬件时钟
- NTP网络校时
- 定时同步机制
- 时区支持

### OTA升级
- 双分区A/B升级
- HTTPS安全传输
- 下载进度反馈
- 自动回滚保护
- 升级后自动标记有效

## 分区表

| 名称 | 类型 | 子类型 | 偏移 | 大小 | 说明 |
|------|------|--------|------|------|------|
| nvs | data | nvs | 0x9000 | 24KB | 配置存储 |
| otadata | data | ota | 0xf000 | 8KB | OTA状态 |
| phy_init | data | phy | 0x11000 | 4KB | PHY初始化 |
| ota_0 | app | ota_0 | 0x20000 | 3.5MB | OTA分区0 |
| ota_1 | app | ota_1 | 0x3A0000 | 3.5MB | OTA分区1 |

**总Flash使用：** 约7.1MB（适合8MB Flash）

## 配置系统

### 配置来源优先级
1. SD卡 `/CONFIG.JSN` (最高优先级)
2. NVS闪存存储
3. 默认配置（硬编码）

### 可配置参数

**时间参数：**
- class1_led1_hold_ms: 20秒
- class2_led1_hold_ms: 35秒
- class3_led2_hold_ms: 10分钟

**继电器调度：**
- relay1: 18:00-07:00
- relay2: 18:00-07:00

**雷达参数：**
- enabled: true
- active_level: 1 (高电平触发)
- trigger_delay_ms: 2000
- cycle_window_ms: 20000
- interference_cycles: 3
- lockout_ms: 300000

**AP热点：**
- SSID: traffic_light_XXXX (MAC后缀)
- Password: 12345678

## 开发记录

### 最近提交
1. `af83e56` - 修复Web配置与OTA升级流程
2. `41f621c` - 添加OTA升级功能（双分区+Web进度条）
3. `beb6c33` - 按电平处理雷达持续触发
4. `2125dc8` - 重构NFC刷卡与雷达工作流程
5. `ebf7ed1` - 雷达脉冲宽度从60ms改为50ms

### 构建状态
✅ 编译成功  
✅ 无警告  
✅ 所有组件正常链接  

## 部署指令

### 烧录命令
```bash
idf.py flash
```

### 监控串口
```bash
idf.py monitor
```

### 完整烧录（首次）
```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

### 手动烧录
```bash
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0xf000 build/ota_data_initial.bin \
  0x20000 build/traffic-light_nfc_s3.bin
```

## 依赖组件

### ESP-IDF组件（官方）
- freertos, lwip, mbedtls, esp_http_server
- esp_wifi, esp_eth, nvs_flash, fatfs, sdmmc

### 第三方组件（managed_components）
- garag/esp-idf-pn532 (v0.2.1) - PN532驱动
- esp-idf-lib/pcf8574 (v1.0.10) - PCF8574驱动
- esp-idf-lib/i2cdev (v2.1.1) - I2C设备库
- espressif/iot_usbh_cdc (v3.0.0) - USB CDC主机
- espressif/iot_usbh_modem (v2.0.0) - USB调制解调器
- espressif/ethernet_init (v0.6.1) - 以太网初始化
- espressif/modem_at (v0.1.0) - AT命令解析

## 性能指标

- **NFC扫描周期：** 300ms
- **雷达轮询周期：** 50ms
- **日志刷新周期：** 1小时
- **时间同步周期：** 按需
- **光耦脉冲宽度：** 50ms高电平 + 50ms间隔
- **W5500 SPI速度：** 8MHz

## 安全特性

1. **OTA安全**
   - HTTPS加密传输
   - 证书验证
   - 自动回滚机制

2. **Web认证**
   - 登录Cookie验证
   - 随机Token生成
   - AP密码保护

3. **配置保护**
   - NVS加密存储（可选）
   - 配置版本控制
   - 参数合法性检查

## 已知限制

1. IRAM使用率100%（已满载，难以扩展）
2. 4G模块功能可通过编译开关禁用
3. NFC规则最多16条
4. 日志最多保留200天
5. 雷达触发后锁定5分钟

## 文档资源

- `docs/config_json.md` - 配置文件格式说明
- `docs/new_pcb_pin_map.md` - PCB引脚映射
- `docs/bringup_success_params_2026-04-29.md` - 启动成功参数记录
- `docs/config_templates/` - 配置模板

## 总结

这是一个功能完整、架构清晰的嵌入式IoT项目，展现了ESP32-S3的强大能力：

**优势：**
- ✅ 多外设集成（NFC、以太网、4G、SD卡）
- ✅ 完整的OTA升级方案
- ✅ 友好的Web配置界面
- ✅ 可靠的日志记录系统
- ✅ 灵活的配置管理
- ✅ 双网络支持（以太网+4G）

**技术亮点：**
- 双OTA分区设计保证升级安全
- 雷达防干扰算法
- 多层配置系统（SD/NVS/默认）
- 硬件抽象层设计良好
- 时间调度功能完善

**适用场景：**
停车场、小区门禁、交通信号控制等需要NFC识别和定时控制的场合。

---
*本报告由Claude Code自动生成*
