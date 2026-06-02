# 单位切换机制说明

## ✅ 后端完全不受影响！

### 工作原理

单位切换是**纯前端功能**，后端始终使用毫秒，完全不受影响。

---

## 📊 数据流详解

### 1. 配置加载（后端 → 前端）

```
后端C代码:
  cfg.radar.trigger_delay_ms = 5000;  // 始终是毫秒

后端JSON (GET /config):
  {"radar": {"trigger_delay_ms": 5000}}

前端JavaScript:
  setVal('radar_delay', 5000);  // 显示5000

用户切换到秒模式:
  前端计算: 5000 ÷ 1000 = 5.00
  显示更新: [5.00] (仅前端显示变化)
```

---

### 2. 用户编辑

```
秒模式下用户输入: 8.5

前端暂存: input.value = 8.5
  (此时仅存在于HTML input中，未发送到后端)
```

---

### 3. 配置保存（前端 → 后端）

```javascript
// JavaScript saveConfig() 函数
function saveConfig(ev) {
  ev.preventDefault();
  
  // 检查当前是否秒模式
  var unitInSeconds = byId('radar_unit_s').checked;
  
  if (unitInSeconds) {
    // ✅ 保存前转换：秒 → 毫秒
    var inputs = document.querySelectorAll('input[data-unit="time"]');
    for (var i = 0; i < inputs.length; i++) {
      var input = inputs[i];
      var val = parseFloat(input.value) || 0;
      input.value = Math.round(val * 1000);  // 8.5 × 1000 = 8500
    }
  }
  
  // 序列化表单并发送
  request('POST', '/save', serializeForm(...));
}
```

**结果**：
```
POST /save
body: radar_delay=8500  // 已转回毫秒！

后端接收:
  cfg->radar.trigger_delay_ms = 8500;  // 毫秒，完全一致！

后端保存:
  NVS: trigger_delay_ms = 8500
  SD卡JSON: {"trigger_delay_ms": 8500}
```

---

## ✅ 后端代码不需要任何修改

### C代码保持不变
```c
// main/src/app_config.c - 完全不变
typedef struct {
    int trigger_delay_ms;       // 始终是毫秒
    int cycle_window_ms;        // 始终是毫秒
    int high_level_threshold_ms;// 始终是毫秒
    int lockout_ms;             // 始终是毫秒
} app_radar_config_t;
```

### JSON格式不变
```json
{
  "radar": {
    "trigger_delay_ms": 5000,
    "cycle_window_ms": 20000,
    "high_level_threshold_ms": 15000,
    "lockout_ms": 300000
  }
}
```

### 后端API不变
```c
// GET /config - 返回毫秒
cfg.radar.trigger_delay_ms

// POST /save - 接收毫秒
parse_int_range(value, 0, 60000, &cfg->radar.trigger_delay_ms)
```

---

## 🎯 总结

| 层级 | 单位 | 说明 |
|------|------|------|
| **后端C代码** | 毫秒 | 完全不变 |
| **NVS存储** | 毫秒 | 完全不变 |
| **SD卡JSON** | 毫秒 | 完全不变 |
| **HTTP API** | 毫秒 | 完全不变 |
| **前端显示** | ms或s | 用户选择 |
| **前端保存** | 毫秒 | 自动转换后发送 |

**结论**：单位切换是100%纯前端功能，后端完全无感知！ ✅

---

## 🔧 已修复的问题

### 问题1：NFC动作时间不受开关影响 ✅

**修复前**：
```html
时长(ms)<input name="class1_led1_hold_ms">
```

**修复后**：
```html
时长<span class="unit-label">(ms)</span>
<input name="class1_led1_hold_ms" data-unit="time">
```

**效果**：NFC动作的3个时间输入框现在也受开关控制了！

---

### 问题2：后端是否受影响 ✅

**回答**：完全不受影响！

- 后端始终使用毫秒
- 前端保存时自动转换回毫秒
- 后端无需修改任何代码

---

*更新时间：2026-06-02*  
*状态：✅ 已修复并验证*
