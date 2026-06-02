# 雷达逻辑重构完整计划

## 任务目标
完全重构雷达输入过滤逻辑，实现新的需求规格

---

## 新需求规格

### 触发流程
1. **延时确认**：检测到高电平 → 延时5秒确认（保留trigger_delay_ms）
2. **窗口去重**：20秒窗口内只输出1次信号
3. **统计数据**：
   - 累计高电平持续时间
   - 触发次数（电平变化次数）

### 干扰检测（满足任一条件）
- **条件A**：20秒窗口内，累计高电平时间 ≥ 15秒
- **条件B**：20秒窗口内，触发次数 ≥ 5次

### 干扰锁定
- 连续N个周期（默认3次）为干扰 → 锁定10分钟
- 锁定期间忽略所有触发

---

## 实施步骤

### 步骤1：修改配置结构
**文件**：`main/include/app_config.h`
- 保留：trigger_delay_ms, cycle_window_ms, interference_cycles, lockout_ms
- 新增：
  - `int high_level_threshold_ms;` // 累计高电平阈值（15秒）
  - `int trigger_count_threshold;` // 触发次数阈值（5次）

### 步骤2：修改配置默认值
**文件**：`main/src/app_config.c`
- `cfg->radar.trigger_delay_ms = 5000;` // 改为5秒
- `cfg->radar.high_level_threshold_ms = 15000;` // 新增：15秒
- `cfg->radar.trigger_count_threshold = 5;` // 新增：5次

### 步骤3：修改配置JSON解析
**文件**：`main/src/app_config.c`
- 添加JSON字段解析：high_level_threshold_ms, trigger_count_threshold

### 步骤4：重写雷达状态机
**文件**：`main/src/radar_input.c`

#### 新状态结构
```c
typedef enum {
    RADAR_STATE_IDLE,           // 空闲
    RADAR_STATE_DEBOUNCING,     // 延时确认（5秒）
    RADAR_STATE_WINDOW_ACTIVE,  // 窗口激活（20秒）
    RADAR_STATE_LOCKED_OUT,     // 锁定
} radar_state_t;

typedef struct {
    // 状态
    radar_state_t state;
    uint64_t state_enter_ms;
    
    // 触发统计
    uint64_t cycle_start_ms;
    int trigger_count;
    bool last_level_active;
    
    // 高电平统计
    uint64_t high_level_start_ms;
    uint64_t accumulated_high_ms;
    
    // 干扰检测
    int interference_streak;
    uint64_t lockout_until_ms;
    
    // 日志
    uint64_t last_status_log_ms;
} radar_filter_t;
```

#### 核心算法
```
状态机转换：
IDLE → (检测到高电平) → DEBOUNCING
DEBOUNCING → (5秒后) → 发送信号 + WINDOW_ACTIVE
WINDOW_ACTIVE → (20秒后) → 检查干扰 → IDLE 或 LOCKED_OUT
LOCKED_OUT → (10分钟后) → IDLE
```

---

## 预计工作量
- 修改配置：15分钟
- 重写状态机：45分钟
- 测试编译：10分钟
- **总计**：70分钟

---

## 验证清单
- [ ] 配置结构正确
- [ ] 默认值正确
- [ ] JSON解析正确
- [ ] 状态机逻辑完整
- [ ] 编译无错误
- [ ] 日志输出清晰
