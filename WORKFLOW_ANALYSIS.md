# NFC刷卡与雷达工作流程分析

## 一、整体架构

系统采用**双任务并行**设计：
- **雷达任务** (`radar_input_task`) - 50ms轮询周期，独立运行
- **应用工作任务** (`app_work_task`) - 100ms轮询周期，处理NFC和控制事件

两者通过**控制队列**通信，NFC具有**绝对优先权**。

---

## 二、NFC刷卡工作流程

### 2.1 卡片扫描（100ms周期）

```
app_work_task (每100ms)
    ↓
读取NFC卡片 (nfc_pn532_read_card_command)
    ↓
    ├─ ESP_OK: 检测到卡片
    │   ↓
    │   比较UID（防重复触发）
    │   ↓
    │   [新卡片] → runtime_handle_nfc_card()
    │
    └─ ESP_ERR_NOT_FOUND: 无卡片
        ↓
        清除latch状态
```

### 2.2 卡片识别与匹配

```
runtime_handle_nfc_card()
    ↓
从配置查找规则 (data[0], data[1] → 类别ID)
    ↓
    ├─ 匹配成功: 获取 class_id (1-6) + rule_name + pulse_count
    │
    └─ 未匹配: 使用遗留command字段 (最多5) → class_id = command+1
    ↓
检查当前状态是否允许该类别卡片
    ↓
    ├─ 不允许: 记录日志，忽略
    │
    └─ 允许: 继续
        ↓
        **取消雷达待处理周期** (radar_input_cancel_pending)
        ↓
        执行动作 (runtime_apply_card_action)
        ↓
        记录到日志队列
```

### 2.3 六类卡片动作

#### **类别1卡 (临时触发)**
```
runtime_enter_class1_state()
    ↓
1. 状态: WORK_MODE_CLASS1
2. 输出脉冲: OPTO1+OPTO2 × N次 (50ms高+50ms低)
3. 继电器4: 闭合
4. LED1: 点亮，保持 class1_led1_hold_ms (默认20秒)
5. LED2: 关闭
    ↓
[20秒后] LED1自动熄灭 → 回到 IDLE
```

#### **类别2卡 (较长触发)**
```
runtime_enter_class2_state()
    ↓
1. 状态: WORK_MODE_CLASS2
2. 输出脉冲: OPTO1+OPTO2 × N次
3. 继电器4: 闭合
4. LED1: 点亮，保持 class2_led1_hold_ms (默认35秒)
5. LED2: 关闭
    ↓
[35秒后] LED1自动熄灭 → 回到 IDLE
```

#### **类别3卡 (等待模式 + 尾脉冲)**
```
runtime_enter_class3_wait_state()
    ↓
1. 状态: WORK_MODE_CLASS3_WAIT
2. 输出脉冲: OPTO1+OPTO2 × N次
3. 继电器4: 闭合
4. LED2: 点亮，保持 class3_led2_hold_ms (默认10分钟)
5. LED1: 关闭
6. 标记: class3_tail_pending = true
    ↓
[在此状态下]
    ├─ 允许类别3卡再次刷卡 (刷新倒计时)
    └─ 允许类别5卡刷卡 (切换到锁定状态)
    ↓
[10分钟后] 
    1. 自动发送1次尾脉冲 (OPTO1+OPTO2 × 1)
    2. LED2熄灭 → 回到 IDLE
```

#### **类别4卡 (永久锁定模式)**
```
runtime_enter_class4_lock_state()
    ↓
1. 状态: WORK_MODE_CLASS4_LOCK
2. 输出脉冲: OPTO1+OPTO2 × N次
3. 继电器4: 保持之前状态
4. LED2: 永久点亮 (不自动熄灭)
5. LED1: 关闭
    ↓
[锁定状态] 只能刷类别1卡解除 → 回到 CLASS1 模式
```

#### **类别5卡 (释放锁定 + 永久亮灯)**
```
runtime_enter_class5_lock_state()
    ↓
1. 状态: WORK_MODE_CLASS5_LOCK
2. 输出脉冲: OPTO1+OPTO2 × N次
3. 继电器4: **释放** (打开)
4. LED2: 永久点亮
5. LED1: 关闭
6. 取消class3尾脉冲
    ↓
[锁定状态] 只能刷类别1卡解除
```

#### **类别6卡 (锁定模式变体)**
```
runtime_enter_class6_lock_state()
    ↓
1. 状态: WORK_MODE_CLASS6_LOCK
2. 输出脉冲: OPTO1+OPTO2 × N次
3. 继电器4: 保持之前状态
4. LED2: 永久点亮
5. LED1: 关闭
    ↓
[锁定状态] 只能刷类别1卡解除
```

### 2.4 卡片互斥规则

| 当前状态 | 允许刷卡类别 | 说明 |
|---------|------------|------|
| IDLE | 1-6全部 | 空闲状态接受所有卡片 |
| CLASS1 | 任意 | 临时状态，可被打断 |
| CLASS2 | 任意 | 临时状态，可被打断 |
| CLASS3_WAIT | 3, 5 | 只允许刷新(3)或切换锁定(5) |
| CLASS4_LOCK | 1 | 只能用类别1解除锁定 |
| CLASS5_LOCK | 1 | 只能用类别1解除锁定 |
| CLASS6_LOCK | 1 | 只能用类别1解除锁定 |

### 2.5 防重复触发机制

```
每次读卡后：
    比较 UID
    ↓
    ├─ 相同UID: card_latched = true，忽略
    └─ 不同UID: 执行新动作，更新latch
        ↓
        卡片移开后 (ESP_ERR_NOT_FOUND)
        ↓
        card_latched = false，允许下次刷卡
```

---

## 三、雷达工作流程

### 3.1 雷达轮询（50ms周期）

```
radar_task (每50ms)
    ↓
读取GPIO (IO_IN1, IO_IN2)
    ↓
检查是否达到触发电平 (active_level, 默认1=高电平)
    ↓
level_active = (IN1==active_level) || (IN2==active_level)
    ↓
radar_filter_step(level_active)
```

### 3.2 雷达过滤状态机

雷达采用**周期窗口过滤算法**，防止误触发和持续干扰：

#### 状态1: IDLE（空闲）
```
检测到触发电平 (level_active=true)
    ↓
    ├─ 处于锁定期: 忽略，记录剩余锁定时间
    │
    └─ 不在锁定期:
        ↓
        开始过滤周期
        ↓
        1. cycle_start_ms = now
        2. fire_at_ms = now + trigger_delay_ms (默认2000ms)
        3. cycle_end_ms = now + cycle_window_ms (默认20000ms)
        4. active = true, fired = false, noisy = false
```

#### 状态2: 过滤周期激活中
```
[时间线]
cycle_start ───────> fire_at ─────────────────> cycle_end
    0ms              2000ms                        20000ms
    ↓                  ↓                             ↓
  开始检测          确认触发                      周期结束
```

##### 情况A: 短脉冲（正常触发）
```
0ms: 检测到高电平 → 启动周期
    ↓
<2000ms: 电平回到低电平
    ↓
2000ms到达: fired = true，发送 APP_CONTROL_EVENT_RADAR_READY
    ↓
应用任务收到事件 → 执行类别1动作
    ↓
20000ms: 周期结束，noisy = false，noisy_streak = 0
```

##### 情况B: 持续高电平（噪声）
```
0ms: 检测到高电平 → 启动周期
    ↓
>50ms: 电平仍然是高电平 (连续触发)
    ↓
noisy = true, continuous_logged = true
    ↓
2000ms: 即使fired=true，但已标记噪声
    ↓
20000ms: 周期结束
    ↓
noisy_streak++
    ↓
if (noisy_streak >= interference_cycles, 默认3):
    ↓
    进入锁定期 lockout_ms (默认300000ms = 5分钟)
    ↓
    连续3次噪声 → 暂停雷达5分钟
```

### 3.3 雷达防干扰机制

```
干扰检测算法:
    ↓
连续3个周期内，每次都检测到"持续高电平"
    ↓
判定为干扰 (interference)
    ↓
启动锁定: lockout_until_ms = now + 300000ms
    ↓
[5分钟内] 所有雷达触发被忽略
    ↓
5分钟后，自动恢复检测
```

### 3.4 NFC优先权机制

```
NFC刷卡时:
    ↓
调用 radar_input_cancel_pending()
    ↓
    ├─ 如果雷达周期active = true:
    │   ↓
    │   清除过滤状态 (clear_cycle_locked)
    │   ↓
    │   记录日志: "radar cycle canceled by NFC priority"
    │
    └─ 如果雷达IDLE: 无动作
        ↓
**NFC动作优先执行，雷达等待下次周期**
```

### 3.5 雷达触发条件

```
雷达事件能触发的前提:
    ↓
1. radar.enabled = true (配置开关)
    ↓
2. 应用状态 = IDLE (runtime_can_enter_radar_state)
    ↓
    ├─ 如果处于任何工作状态:
    │   ↓
    │   事件被忽略，记录日志
    │
    └─ 如果IDLE:
        ↓
        执行 runtime_fire_radar_action()
        ↓
        等同于类别1卡动作:
        - OPTO1+OPTO2 脉冲 × N次 (配置项)
        - 继电器4闭合
        - LED1点亮保持20秒
```

---

## 四、时序图

### 4.1 NFC刷卡时序

```
时间轴: ─────────────────────────────────────────>

雷达任务:  [周期active] ─X─ [取消] ─ [空闲]
                         ↑
                    NFC优先取消
                         ↓
应用任务:  [检测到卡] ─ [执行动作] ─ [LED1亮20s] ─ [自动熄灭]

硬件输出:     ██ (脉冲)         ████████████████  (LED1)
              ↑                  ↑                ↑
           t=0ms              t=100ms          t=20000ms
```

### 4.2 雷达触发时序（正常）

```
雷达任务:  [检测高] ─ [延时2s] ─ [发送事件] ─ [20s后周期结束]
              ↓                    ↓
           t=0ms                t=2000ms
              
应用任务:                    [收到事件] ─ [执行类别1动作]
                                 ↓
硬件输出:                      ██ (脉冲) ─ ████████ (LED1亮20s)
```

### 4.3 雷达噪声锁定时序

```
周期1: [持续高] ─ [标记noisy] ─ [noisy_streak=1]
         ↓
周期2: [持续高] ─ [标记noisy] ─ [noisy_streak=2]
         ↓
周期3: [持续高] ─ [标记noisy] ─ [noisy_streak=3]
         ↓
      [触发锁定] ─ [5分钟内忽略所有触发] ─ [自动恢复]
```

---

## 五、关键参数配置

### 5.1 NFC参数

| 参数 | 默认值 | 说明 |
|-----|-------|------|
| APP_NFC_POLL_INTERVAL_MS | 100ms | NFC扫描周期 |
| APP_IO_PULSE_HIGH_MS | 50ms | 光耦脉冲高电平时长 |
| APP_IO_PULSE_LOW_GAP_MS | 50ms | 光耦脉冲间隔 |
| class1_led1_hold_ms | 20000ms | 类别1 LED1保持时间 |
| class2_led1_hold_ms | 35000ms | 类别2 LED1保持时间 |
| class3_led2_hold_ms | 600000ms | 类别3 LED2保持时间(10分钟) |

### 5.2 雷达参数

| 参数 | 默认值 | 说明 |
|-----|-------|------|
| RADAR_POLL_MS | 50ms | 雷达轮询周期 |
| radar.enabled | true | 雷达功能开关 |
| radar.active_level | 1 | 触发电平(1=高电平) |
| radar.trigger_delay_ms | 2000ms | 触发延时（防抖） |
| radar.cycle_window_ms | 20000ms | 过滤周期窗口 |
| radar.interference_cycles | 3 | 干扰判定阈值 |
| radar.lockout_ms | 300000ms | 锁定时长(5分钟) |
| radar.opto12_pulses | 1 | 雷达触发脉冲数 |

---

## 六、核心设计亮点

### 6.1 NFC优先权
- NFC刷卡会立即取消雷达待处理周期
- 保证人工操作的响应速度
- 避免冲突和重复触发

### 6.2 雷达防干扰
- **短脉冲检测**: 只有电平变化才算有效触发
- **噪声过滤**: 持续高电平标记为噪声
- **自动锁定**: 连续3次噪声 → 锁定5分钟
- **延时确认**: 2秒延时防止误触发

### 6.3 状态锁定机制
- 类别4/5/6进入永久锁定状态
- 只能用类别1卡解除
- 防止误操作

### 6.4 类别3智能尾脉冲
- 进入等待模式后，10分钟自动发送结束脉冲
- 可在等待期间刷卡刷新倒计时
- 可用类别5卡提前切换状态

### 6.5 防重复触发
- NFC使用UID latch机制
- 雷达使用周期窗口机制
- 避免同一触发源连续激活

---

## 七、典型应用场景

### 场景1: 车辆进入（NFC临时开闸）
```
1. 刷类别1卡 → LED1亮20秒 → 自动恢复
2. 光耦脉冲控制闸机开启
3. 继电器4控制辅助设备
```

### 场景2: 大车进入（NFC延长时间）
```
1. 刷类别2卡 → LED1亮35秒 → 自动恢复
2. 更长时间供大车通过
```

### 场景3: 长期停车（NFC等待模式）
```
1. 刷类别3卡 → LED2亮10分钟
2. 期间可再刷类别3卡刷新倒计时
3. 10分钟后自动发送尾脉冲 → 恢复
4. 或刷类别5卡提前结束并锁定
```

### 场景4: 维护模式（锁定）
```
1. 刷类别4卡 → LED2永久亮，系统锁定
2. 此时只能刷类别1卡解除
3. 防止其他操作干扰
```

### 场景5: 雷达自动触发（行人检测）
```
1. 雷达检测到运动 → 2秒确认 → 自动开闸
2. 等同于类别1卡动作
3. 如有人刷卡，雷达周期被取消
4. 如检测到持续高电平（故障），3次后锁定5分钟
```

---

## 八、故障保护

1. **雷达干扰锁定**: 防止硬件故障导致连续误触发
2. **NFC优先**: 确保人工操作不被自动系统干扰
3. **状态互斥**: 锁定状态只能特定卡片解除
4. **定时器自动恢复**: 临时状态自动回到IDLE，无需人工干预
5. **日志记录**: 所有NFC操作记录到SD卡，可追溯

---

*本文档详细描述了系统的实时控制逻辑，便于理解和维护。*
