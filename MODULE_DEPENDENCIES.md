# 模块依赖关系图

生成时间：2026-06-02

## 一、模块依赖矩阵

```
                       ┌─────────────────────────────────────────────────┐
                       │           被依赖模块（提供服务）                  │
  ┌────────────────────┼─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┤
  │ 依赖模块（使用服务）│ HAL │ NFC │ SD  │ ETH │ 4G  │ CFG │TIME │RADAR│
  ├────────────────────┼─────┼─────┼─────┼─────┼─────┼─────┼─────┼─────┤
  │ main.c             │  ●  │  ●  │  ●  │  ●  │  ●  │  ●  │  ●  │  ●  │
  │ app.c              │  ●  │  ●  │  ●  │  ●  │  ●  │  ●  │  ●  │  ●  │
  │ web_config.c       │     │     │  ●  │     │     │  ●  │  ●  │     │
  │ nfc_pn532.c        │     │     │     │     │     │     │     │     │
  │ radar_input.c      │  ●  │     │     │     │     │  ●  │     │     │
  │ time_sync.c        │     │     │     │  ●  │  ●  │     │     │     │
  │ ota_update.c       │     │     │     │     │     │     │     │     │
  │ app_config.c       │     │     │  ●  │     │     │     │     │     │
  │ board_hal.c        │     │     │     │     │     │     │     │     │
  │ net_eth.c          │     │     │     │     │     │     │     │     │
  │ modem_4g.c         │     │     │     │     │     │     │     │     │
  │ storage_sd.c       │     │     │     │     │     │     │     │     │
  └────────────────────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘

● = 强依赖
```

## 二、调用层次图

```
                            ┌──────────┐
                            │  main.c  │
                            │  (入口)   │
                            └────┬─────┘
                                 │
                ┌────────────────┼────────────────┐
                ↓                ↓                ↓
         ┌──────────┐     ┌──────────┐    ┌──────────┐
         │  app.c   │     │web_config│    │ota_update│
         │ (核心)    │     │  (Web)   │    │  (OTA)   │
         └────┬─────┘     └────┬─────┘    └──────────┘
              │                │
      ┌───────┼────────┬───────┴─────┐
      ↓       ↓        ↓             ↓
┌──────────┐┌──────────┐┌──────────┐┌──────────┐
│  NFC     ││  RADAR   ││app_config││time_sync │
│pn532.c   ││input.c   ││   .c     ││   .c     │
└────┬─────┘└────┬─────┘└────┬─────┘└────┬─────┘
     │           │            │           │
     └───────────┴────────────┴───────────┘
                      ↓
              ┌─────────────┐
              │  board_hal  │
              │    (HAL)    │
              └──────┬──────┘
                     │
         ┌───────────┼───────────┐
         ↓           ↓           ↓
   ┌─────────┐ ┌─────────┐ ┌─────────┐
   │ GPIO    │ │  I2C    │ │  PCF    │
   │ driver  │ │ driver  │ │  8574   │
   └─────────┘ └─────────┘ └─────────┘
```

## 三、数据流图

### 3.1 NFC刷卡数据流

```
┌──────────┐
│ NFC卡片  │
└────┬─────┘
     │ UID + Data
     ↓
┌──────────────┐
│ nfc_pn532.c  │ ← 300ms扫描任务
│ (扫描任务)    │
└────┬─────────┘
     │ nfc_card_command_t
     ↓
┌──────────────┐
│   app.c      │
│ (主任务100ms) │
└────┬─────────┘
     │
     ├─→ [1] app_config 查找规则 → class_id + pulse_count
     │
     ├─→ [2] radar_input_cancel_pending() → 取消雷达
     │
     ├─→ [3] runtime_apply_card_action() → 执行动作
     │         │
     │         ├─→ board_hal_pulse_opto12() → 光耦脉冲
     │         ├─→ board_hal_set_relay() → 继电器
     │         └─→ board_hal_set_led1/2() → LED
     │
     └─→ [4] app_log_enqueue() → 日志队列
              ↓
         ┌──────────────┐
         │ 日志刷新任务  │ ← 1小时周期
         │ (后台任务)    │
         └────┬─────────┘
              │
              ↓
         ┌──────────┐
         │ SD卡文件 │ /NFCLOG/YYYYMMDD.TXT
         └──────────┘
```

### 3.2 雷达触发数据流

```
┌──────────┐
│ 雷达传感器│
└────┬─────┘
     │ GPIO电平
     ↓
┌──────────────┐
│radar_input.c │ ← 50ms轮询任务
│ (轮询任务)    │
└────┬─────────┘
     │
     ├─→ [1] 读取GPIO (IO_IN1, IO_IN2)
     │
     ├─→ [2] radar_filter_step() → 过滤算法
     │         │
     │         ├─→ 检测持续高电平 → 标记noisy
     │         ├─→ 延时2秒确认
     │         └─→ 连续3次噪声 → 锁定5分钟
     │
     └─→ [3] post_radar_ready() → 控制队列
              ↓
         ┌──────────────┐
         │   app.c      │
         │ (主任务)      │
         └────┬─────────┘
              │
              ├─→ [检查] runtime_can_enter_radar_state()
              │         │
              │         ├─→ IDLE → 允许
              │         └─→ 工作中 → 忽略
              │
              └─→ [执行] runtime_fire_radar_action()
                        │ (等同类别1卡)
                        │
                        └─→ 光耦脉冲 + 继电器 + LED
```

### 3.3 配置数据流

```
┌──────────────────────────────────────┐
│          配置来源（优先级）            │
├──────────────────────────────────────┤
│ 1. SD卡 /CONFIG.JSN (最高)            │
│ 2. NVS Flash (持久化)                 │
│ 3. 默认值 (硬编码)                    │
└────────────────┬─────────────────────┘
                 ↓
         ┌──────────────┐
         │app_config.c  │
         │ (配置管理)    │
         └────┬─────────┘
              │
              ├─→ app_config_init() → 启动时加载
              │
              ├─→ app_config_get() → 全局读取
              │
              └─→ app_config_save() → Web保存
                   ↓
            ┌──────────┐
            │   NVS    │ 持久化存储
            └──────────┘
```

### 3.4 Web配置数据流

```
┌──────────┐
│  浏览器  │
└────┬─────┘
     │ HTTP请求
     ↓
┌──────────────┐
│web_config.c  │ ← ESP32 AP模式
│ (HTTP服务器) │
└────┬─────────┘
     │
     ├─→ GET /api/config → 读取配置
     │         │
     │         └─→ app_config_get()
     │
     ├─→ POST /api/config → 保存配置
     │         │
     │         ├─→ JSON解析
     │         ├─→ app_config_save()
     │         └─→ NVS持久化
     │
     ├─→ POST /api/ota → OTA升级
     │         │
     │         └─→ ota_update_start(url)
     │               ↓
     │         ┌──────────────┐
     │         │ota_update.c  │ ← 后台下载任务
     │         │ (下载任务)    │
     │         └──────────────┘
     │
     └─→ GET /api/ota_status → 进度查询
           │
           └─→ ota_update_get_status()
```

## 四、任务通信图

```
┌─────────────────────────────────────────────────────────────┐
│                    FreeRTOS 任务架构                         │
└─────────────────────────────────────────────────────────────┘

任务1: app_work_task (优先级3, 栈8KB, 周期100ms)
   │
   ├─→ 读取 NFC
   ├─→ 处理控制队列 ← [radar_ready事件]
   ├─→ 处理定时器
   └─→ 写入日志队列

任务2: nfc_scan_task (优先级4, 栈4KB, 周期300ms)
   │
   └─→ UART扫描卡片 → nfc_card_command_t

任务3: radar_input_task (优先级3, 栈4KB, 周期50ms)
   │
   ├─→ GPIO轮询
   ├─→ 过滤算法
   └─→ 发送 → s_control_queue

任务4: nfc_log_task (优先级2, 栈4KB, 周期1小时)
   │
   ├─→ 从 s_log_queue 读取
   └─→ 写入SD卡

任务5: time_sync_task (优先级?, 栈?, 按需)
   │
   ├─→ NTP同步
   └─→ RTC同步

任务6: modem_4g_task (优先级?, 栈?, 状态机)
   │
   └─→ USB CDC通信 → AT命令

任务7: ota_download_task (优先级4, 栈8KB, 按需)
   │
   └─→ HTTPS下载固件

任务8: peripheral_test_task (优先级?, 栈?, 可选)
   │
   └─→ 外设测试循环

┌─────────────────────────────────────────────────────────────┐
│                         队列通信                             │
└─────────────────────────────────────────────────────────────┘

s_control_queue (16条)
   radar_input_task  ──[APP_CONTROL_EVENT_RADAR_READY]──> app_work_task

s_log_queue (64条)
   app_work_task  ──[nfc_log_entry_t]──> nfc_log_task

┌─────────────────────────────────────────────────────────────┐
│                        互斥锁保护                            │
└─────────────────────────────────────────────────────────────┘

s_config_mutex → app_config_t (多任务读写配置)
s_state_mutex → radar_filter_state_t (雷达状态)
s_mutex → ota_status_t (OTA状态)
s_pcf_mutex → PCF8574 I2C设备 (硬件访问)
```

## 五、模块接口清单

### 5.1 核心API

#### app.h
```c
esp_err_t app_work_start(const app_devices_t *devices);
esp_err_t app_post_control_event(const app_control_event_t *event);
```

#### app_config.h
```c
esp_err_t app_config_init(void);
const app_config_t *app_config_get(void);
void app_config_copy(app_config_t *out);
esp_err_t app_config_save(const app_config_t *cfg);
esp_err_t app_config_find_nfc_action(uint8_t data0, uint8_t data1, ...);
void app_config_build_default_ap_name(char *out, size_t out_len);
```

#### board_hal.h
```c
esp_err_t board_hal_init(void);
void board_hal_log_map(void);
esp_err_t board_hal_read_inputs(board_inputs_t *inputs);
esp_err_t board_hal_set_ledb(bool on);
esp_err_t board_hal_set_led1(bool on);
esp_err_t board_hal_set_led2(bool on);
esp_err_t board_hal_pulse_opto12(int pulse_count, int active_ms, int gap_ms);
esp_err_t board_hal_set_relay(int relay_num, bool closed);
esp_err_t board_hal_set_all_relays(bool closed);
esp_err_t board_hal_pulse_relay(int relay_num, int pulse_count, ...);
esp_err_t board_hal_pulse_io_out1(int pulse_count, int high_ms, int gap_ms);
```

### 5.2 驱动API

#### nfc_pn532.h
```c
esp_err_t nfc_pn532_init(void);
esp_err_t nfc_pn532_start_card_scan(void);
esp_err_t nfc_pn532_read_card_command(nfc_card_command_t *card);
void nfc_pn532_deinit(void);
```

#### radar_input.h
```c
esp_err_t radar_input_start(void);
void radar_input_cancel_pending(void);
```

#### storage_sd.h
```c
esp_err_t storage_sd_init(void);
const char *storage_sd_mount_point(void);
bool storage_sd_is_mounted(void);
```

#### net_eth.h
```c
esp_err_t net_eth_init(void);
bool net_eth_is_connected(void);
```

#### modem_4g.h
```c
esp_err_t modem_4g_init(void);
bool modem_4g_is_connected(void);
```

### 5.3 功能API

#### ota_update.h
```c
esp_err_t ota_update_init(void);
esp_err_t ota_update_start(const char *url);
void ota_update_get_status(ota_status_t *out);
const char *ota_state_name(ota_state_t state);
const char *ota_running_version(void);
```

#### time_sync.h
```c
esp_err_t time_sync_bootstrap(void);
esp_err_t time_sync_start(void);
bool time_sync_is_synced(void);
```

#### web_config.h
```c
esp_err_t web_config_start(void);
```

## 六、关键数据结构

### 6.1 配置结构

```c
typedef struct {
    int version;
    app_timing_config_t timing;           // LED保持时间
    app_schedule_config_t schedule;       // 继电器时间表
    app_ap_config_t ap;                   // AP热点配置
    app_nfc_rule_t nfc_rules[16];         // NFC规则（最多16条）
    size_t nfc_rule_count;
    app_radar_config_t radar;             // 雷达参数
    bool log_enabled;
    app_config_source_t source;           // 配置来源
} app_config_t;
```

### 6.2 运行时状态

```c
typedef struct {
    work_source_t source;                 // NONE/NFC/RADAR
    work_mode_t mode;                     // IDLE/CLASS1-6
    uint32_t led1_off_at_ms;
    led2_mode_t led2_mode;
    uint32_t led2_hold_until_ms;
    bool class3_tail_pending;
    uint32_t class3_tail_due_ms;
    bool schedule_state_valid;
    bool schedule_wait_logged;
    bool relay1_closed;
    bool relay2_closed;
    uint32_t last_schedule_check_ms;
} app_runtime_state_t;
```

### 6.3 雷达过滤状态

```c
typedef struct {
    bool active;                          // 周期激活
    bool fired;                           // 已触发
    bool noisy;                           // 噪声标记
    bool continuous_logged;
    uint32_t cycle_start_ms;
    uint32_t fire_at_ms;
    uint32_t cycle_end_ms;
    uint32_t lockout_until_ms;            // 锁定截止时间
    uint32_t last_lockout_log_ms;
    uint32_t noisy_streak;                // 连续噪声次数
} radar_filter_state_t;
```

## 七、启动流程

```
┌──────────────────────────────────────────────────────────┐
│                     系统启动流程                          │
└──────────────────────────────────────────────────────────┘

app_main()
   │
   ├─→ [1/3] platform_init()
   │         ├─→ nvs_flash_init()
   │         ├─→ esp_netif_init()
   │         └─→ esp_event_loop_create_default()
   │
   ├─→ [2/3] devices_init()
   │         ├─→ board_hal_init()          → GPIO + I2C + PCF8574
   │         ├─→ nfc_pn532_init()          → PN532 UART初始化
   │         ├─→ storage_sd_init()         → SD卡挂载
   │         ├─→ net_eth_init()            → W5500 SPI初始化
   │         └─→ modem_4g_init()           → AIR780ER USB初始化
   │
   ├─→ [3/3] app_config_init()
   │         └─→ 加载配置 (SD → NVS → 默认)
   │
   ├─→ time_sync_bootstrap()             → RTC读取
   ├─→ time_sync_start()                 → 启动同步任务
   │
   ├─→ app_work_start()                  → 启动主任务
   ├─→ radar_input_start()               → 启动雷达任务
   ├─→ web_config_start()                → 启动Web服务器
   │         ├─→ WiFi AP模式
   │         └─→ HTTP服务器
   │
   └─→ peripheral_test_start()           → (可选)外设测试

   ↓
[系统运行 - 多任务并发]
```

---
*本文档详细描述了系统的模块依赖关系和数据流，便于理解整体架构。*
