# 交通信号灯系统设计缺陷修复方案

## 一、问题清单与优先级

| 优先级 | 问题 | 影响 | 修复难度 |
|--------|------|------|---------|
| P0 | 时间溢出风险 | 系统运行>49天崩溃 | 中 |
| P0 | 错误处理不一致 | 硬件状态不可靠 | 低 |
| P0 | 缺少状态恢复 | 重启后状态错误 | 中 |
| P1 | 雷达逻辑需重构 | 当前实现不符合需求 | 高 |
| P2 | 类别3尾脉冲 | 应执行完整类别1动作 | 低 |
| P3 | 防御性日志 | 调试困难 | 低 |

---

## 二、详细修复方案

### 2.1 时间溢出修复（P0）

#### 当前问题
```c
// 多个文件中使用uint32_t毫秒时间戳
static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}
// 问题：49.7天后溢出
```

#### 修复方案：改用64位时间戳
```c
// 创建新的公共时间工具头文件 main/include/time_utils.h
#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"

// 64位毫秒时间戳，584,942,417年后才溢出
static inline uint64_t time_ms_64(void) {
    return (uint64_t)xTaskGetTickCount() * (uint64_t)portTICK_PERIOD_MS;
}

// 时间比较（处理64位）
static inline bool time_after_64(uint64_t a, uint64_t b) {
    return (int64_t)(a - b) > 0;
}

static inline bool time_reached_64(uint64_t now, uint64_t deadline) {
    return (int64_t)(now - deadline) >= 0;
}
```

#### 修改的文件
1. **main/include/time_utils.h** (新建)
2. **main/src/app.c** - 替换所有uint32_t时间戳为uint64_t
3. **main/src/radar_input.c** - 替换所有uint32_t时间戳为uint64_t
4. **main/include/app.h** - app_control_event_t.timestamp_ms改为uint64_t
5. **main/src/app_config.c** - 配置结构不变（配置参数仍用int）

---

### 2.2 错误处理改进（P0）

#### 当前问题
18处使用`ESP_ERROR_CHECK_WITHOUT_ABORT`，硬件失败被静默吞掉

#### 修复方案：改进错误处理策略

**策略1：关键硬件操作 - 重试机制**
```c
// 创建带重试的硬件设置函数
static esp_err_t set_relay_with_retry(int relay_num, bool closed, int retries) {
    esp_err_t err;
    for (int i = 0; i < retries; i++) {
        err = board_hal_set_relay(relay_num, closed);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Relay%d set failed (attempt %d/%d): %s", 
                 relay_num, i+1, retries, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGE(TAG, "Relay%d set failed after %d retries", relay_num, retries);
    return err;
}
```

**策略2：非关键操作 - 记录错误但继续**
```c
// LED操作失败不影响业务
esp_err_t err = board_hal_set_led1(false);
if (err != ESP_OK) {
    ESP_LOGW(TAG, "LED1 set failed (non-critical): %s", esp_err_to_name(err));
}
```

#### 修改的文件
1. **main/src/app.c** - 替换所有ESP_ERROR_CHECK_WITHOUT_ABORT
   - 继电器操作：使用重试机制（关键）
   - LED操作：记录错误（非关键）
   - 光耦操作：使用重试机制（关键）

---

### 2.3 启动状态恢复（P0）

#### 当前问题
```c
// app_work_task启动时强制设置，不管实际状态
ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_ledb(true));
ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_led1(false));
ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_relay(3, false));
ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_relay(4, true));
```

#### 修复方案：读取硬件实际状态

**步骤1：在board_hal.c中添加读取PCF8574状态的函数**
```c
// main/src/board_hal.c 新增函数
esp_err_t board_hal_read_pcf_state(uint8_t *state) {
    if (!s_pcf_ready || state == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    xSemaphoreTake(s_pcf_mutex, portMAX_DELAY);
    esp_err_t err = pcf8574_port_read(&s_pcf_dev, state);
    xSemaphoreGive(s_pcf_mutex);
    
    return err;
}
```

**步骤2：在board_hal.h中声明**
```c
// main/include/board_hal.h 新增
esp_err_t board_hal_read_pcf_state(uint8_t *state);
```

**步骤3：在app.c启动时读取并打印实际状态**
```c
// app_work_task启动时
uint8_t pcf_state = 0xFF;
if (board_hal_read_pcf_state(&pcf_state) == ESP_OK) {
    bool relay1 = !(pcf_state & (1 << PCF8574_RELAY1_BIT));
    bool relay2 = !(pcf_state & (1 << PCF8574_RELAY2_BIT));
    bool relay3 = !(pcf_state & (1 << PCF8574_RELAY3_BIT));
    bool relay4 = !(pcf_state & (1 << PCF8574_RELAY4_BIT));
    
    ESP_LOGI(TAG, "Hardware state at startup: relay1=%d relay2=%d relay3=%d relay4=%d",
             relay1, relay2, relay3, relay4);
}

// 然后设置期望的初始状态
// ...
```

#### 修改的文件
1. **main/include/board_hal.h** - 新增函数声明
2. **main/src/board_hal.c** - 实现读取函数
3. **main/src/app.c** - 启动时读取并记录状态

---

### 2.4 雷达逻辑重构（P1）

#### 新需求规格

**架构：**
```
GPIO引脚1/2 → [高电平中断 + 轮询检测] → 雷达Task → 状态机（仅IDLE状态接收）
```

**新的过滤规则：**
1. **触发确认延时**：检测到高电平后延时5秒确认（保留原有trigger_delay_ms，默认5000）
2. **窗口去重**：20秒窗口内只输出1次（cycle_window_ms=20000）
3. **干扰检测条件A**：窗口内累计高电平时间≥15秒（新增high_level_threshold_ms）
4. **干扰检测条件B**：窗口内触发次数≥5次（新增trigger_count_threshold）
5. **干扰锁定**：连续N个周期为干扰 → 锁定10分钟（interference_cycles + lockout_ms）

#### 新的配置结构
```c
// main/include/app_config.h 修改
typedef struct {
    bool enabled;
    int active_level;                // 触发电平
    
    // 触发确认
    int trigger_delay_ms;            // 延时确认（5秒）保留
    
    // 窗口与去重
    int cycle_window_ms;             // 窗口时间（20秒）
    
    // 干扰检测条件A：累计高电平
    int high_level_threshold_ms;     // 新增：累计高电平阈值（15秒）
    
    // 干扰检测条件B：触发次数
    int trigger_count_threshold;     // 新增：触发次数阈值（5次）
    
    // 干扰锁定
    int interference_cycles;         // 连续干扰周期阈值（3次）
    int lockout_ms;                  // 锁定时长（10分钟）
    
    // 输出
    int opto12_pulses;
} app_radar_config_t;
```

#### 新的状态结构
```c
// main/src/radar_input.c 重构状态结构
typedef enum {
    RADAR_STATE_IDLE,           // 空闲，等待触发
    RADAR_STATE_DEBOUNCING,     // 延时确认中（5秒）
    RADAR_STATE_WINDOW_ACTIVE,  // 窗口激活（20秒去重）
    RADAR_STATE_LOCKED_OUT,     // 干扰锁定
} radar_state_t;

typedef struct {
    // 状态机
    radar_state_t state;
    uint64_t state_enter_ms;        // 进入当前状态的时间
    
    // 触发统计
    uint64_t first_trigger_ms;      // 本周期第一次触发时间
    int trigger_count;              // 本周期触发次数
    
    // 高电平统计
    uint64_t high_level_start_ms;   // 高电平开始时间（0=当前低电平）
    uint64_t accumulated_high_ms;   // 本周期累计高电平时间
    
    // 干扰检测
    int interference_streak;        // 连续干扰周期计数
    uint64_t lockout_until_ms;      // 锁定截止时间
    
    // 日志控制
    uint64_t last_status_log_ms;
    uint64_t last_lockout_log_ms;
} radar_filter_t;
```

#### 核心算法流程
```c
// 伪代码
void radar_filter_step(cfg, level_active, now) {
    switch (state) {
    case IDLE:
        if (锁定期未结束) {
            忽略;
        } else if (level_active) {
            state = DEBOUNCING;
            state_enter_ms = now;
            first_trigger_ms = now;
            trigger_count = 1;
            accumulated_high_ms = 0;
            high_level_start_ms = now;
        }
        break;
        
    case DEBOUNCING:
        // 统计高电平时间
        if (level_active) {
            // 持续高电平，累计时间
        } else {
            // 变低电平，停止累计
            accumulated_high_ms += (now - high_level_start_ms);
            high_level_start_ms = 0;
        }
        
        // 检查延时是否到达
        if (now - state_enter_ms >= trigger_delay_ms) {
            // 延时确认完成，发送触发信号
            post_radar_ready(now);
            state = WINDOW_ACTIVE;
            // 继续统计到窗口结束
        }
        break;
        
    case WINDOW_ACTIVE:
        // 统计高电平时间（继续）
        if (level_active) {
            // 持续高电平
        } else {
            // 变低电平
            if (high_level_start_ms != 0) {
                accumulated_high_ms += (now - high_level_start_ms);
                high_level_start_ms = 0;
            }
        }
        
        // 统计触发次数（窗口内再次触发计数但不输出）
        if (level_active && 上次是低电平) {
            trigger_count++;
        }
        
        // 检查窗口是否结束
        if (now - first_trigger_ms >= cycle_window_ms) {
            // 判断是否为干扰周期
            bool is_interference = false;
            
            // 条件A：累计高电平时间
            if (accumulated_high_ms >= high_level_threshold_ms) {
                is_interference = true;
                ESP_LOGW(TAG, "Interference: accumulated_high=%llu ms >= threshold=%d ms",
                         accumulated_high_ms, high_level_threshold_ms);
            }
            
            // 条件B：触发次数
            if (trigger_count >= trigger_count_threshold) {
                is_interference = true;
                ESP_LOGW(TAG, "Interference: trigger_count=%d >= threshold=%d",
                         trigger_count, trigger_count_threshold);
            }
            
            // 更新干扰计数
            if (is_interference) {
                interference_streak++;
                
                // 检查是否需要锁定
                if (interference_streak >= interference_cycles) {
                    state = LOCKED_OUT;
                    lockout_until_ms = now + lockout_ms;
                    interference_streak = 0;
                    ESP_LOGW(TAG, "Radar locked out for %d ms", lockout_ms);
                } else {
                    state = IDLE;
                }
            } else {
                interference_streak = 0;
                state = IDLE;
            }
        }
        break;
        
    case LOCKED_OUT:
        if (now >= lockout_until_ms) {
            state = IDLE;
            ESP_LOGI(TAG, "Radar lockout ended, resumed");
        }
        break;
    }
}
```

#### 修改的文件
1. **main/include/app_config.h** - 修改app_radar_config_t结构
2. **main/src/app_config.c** - 添加新配置参数的默认值和JSON解析
3. **main/src/radar_input.c** - 完全重写过滤逻辑
4. **main/include/radar_input.h** - 保持接口不变

---

### 2.5 类别3尾脉冲改进（P2）

#### 当前实现
```c
// app.c:554-560
if (s_runtime.class3_tail_pending && ...) {
    ESP_LOGI(TAG, "[work] class3 hold expired; firing tail pulse once");
    board_hal_pulse_opto12(1, APP_IO_PULSE_HIGH_MS, APP_IO_PULSE_LOW_GAP_MS);
}
runtime_set_led2_off();
runtime_set_work_state(WORK_SOURCE_NONE, WORK_MODE_IDLE);
```

#### 修复方案：执行完整类别1动作
```c
// app.c 修改runtime_process_timers函数
if (s_runtime.led2_mode == LED2_MODE_CLASS3_HOLD &&
    s_runtime.led2_hold_until_ms != 0 &&
    time_reached_64(now, s_runtime.led2_hold_until_ms)) {
    
    if (s_runtime.class3_tail_pending && s_runtime.class3_tail_due_ms != 0 &&
        time_reached_64(now, s_runtime.class3_tail_due_ms)) {
        
        // 执行完整的类别1动作，而不是单次脉冲
        ESP_LOGI(TAG, "[work] class3 hold expired; entering class1 state as tail action");
        
        // 先关闭LED2
        runtime_set_led2_off();
        
        // 执行类别1动作（光耦脉冲 + 继电器4 + LED1亮20秒）
        runtime_enter_class1_state(WORK_SOURCE_NFC, cfg, now, 1);
        return; // 不再执行下面的状态清理
    }
    
    runtime_set_led2_off();
    if (s_runtime.mode == WORK_MODE_CLASS3_WAIT) {
        runtime_set_work_state(WORK_SOURCE_NONE, WORK_MODE_IDLE);
    }
}
```

#### 修改的文件
1. **main/src/app.c** - runtime_process_timers函数

---

### 2.6 防御性日志（P3）

#### 修复方案：添加非法输入日志
```c
// app.c runtime_apply_card_action函数
switch (class_id) {
case 1: runtime_enter_class1_state(...); break;
case 2: runtime_enter_class2_state(...); break;
case 3: runtime_enter_class3_wait_state(...); break;
case 4: runtime_enter_class4_lock_state(...); break;
case 5: runtime_enter_class5_lock_state(...); break;
case 6: runtime_enter_class6_lock_state(...); break;
default:
    ESP_LOGW(TAG, "[work] Invalid class_id=%u, card action ignored", class_id);
    break;
}
```

#### 修改的文件
1. **main/src/app.c** - runtime_apply_card_action函数

---

## 三、实施步骤

### 阶段1：准备工作
1. 创建time_utils.h公共时间工具
2. 备份当前代码

### 阶段2：P0修复（关键）
1. 修复时间溢出（time_utils.h + 所有时间戳改uint64_t）
2. 改进错误处理（重试机制 + 日志）
3. 添加状态恢复（读取PCF8574）

### 阶段3：P1重构（雷达）
1. 修改配置结构（新增2个参数）
2. 重写radar_input.c的状态机
3. 测试验证

### 阶段4：P2+P3修复（次要）
1. 修改类别3尾脉冲
2. 添加防御性日志

### 阶段5：测试验证
1. 编译测试
2. 功能测试
3. 长时间运行测试

---

## 四、风险评估

| 风险 | 可能性 | 影响 | 缓解措施 |
|------|--------|------|---------|
| uint64_t性能影响 | 低 | 低 | ESP32-S3支持64位运算 |
| 雷达逻辑错误 | 中 | 高 | 详细测试，保留调试日志 |
| 配置兼容性 | 低 | 中 | 新参数有默认值，兼容旧配置 |
| 编译错误 | 低 | 低 | 逐步修改，每步编译验证 |

---

## 五、测试计划

### 5.1 单元测试项
- [ ] 时间工具函数（time_reached_64等）
- [ ] 雷达状态机各状态转换
- [ ] 错误处理重试机制

### 5.2 集成测试项
- [ ] NFC刷卡各类别功能
- [ ] 雷达触发（正常情况）
- [ ] 雷达干扰检测与锁定
- [ ] 类别3超时自动执行类别1
- [ ] 系统重启状态恢复

### 5.3 长期测试
- [ ] 连续运行7天（168小时）
- [ ] 监控内存使用
- [ ] 检查时间戳无溢出

---

## 六、代码注释规范

所有修改的代码必须包含清晰的注释：

```c
// ============================================================================
// 时间工具函数 - 使用64位时间戳防止溢出
// 修复日期: 2026-06-02
// 问题: 原uint32_t时间戳49天溢出
// 方案: 改用uint64_t，584,942,417年后才溢出
// ============================================================================

// ============================================================================
// 雷达过滤状态机 - 重构版
// 修复日期: 2026-06-02
// 新需求:
//   1. 保留5秒延时确认
//   2. 20秒窗口去重
//   3. 统计累计高电平时间和触发次数
//   4. 两种干扰检测条件
//   5. 连续干扰锁定
// ============================================================================
```

---

## 七、完成标准

- [ ] 所有P0问题修复
- [ ] 所有P1问题修复
- [ ] 所有P2+P3问题修复
- [ ] 编译无错误无警告
- [ ] 代码注释完整
- [ ] 基本功能测试通过
- [ ] 生成修改总结文档

---

*此计划在实施前需要用户确认批准*
