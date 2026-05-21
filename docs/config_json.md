# NFC 路灯控制器配置文件说明

本文档说明新版固件使用的配置文件格式，供硬件、测试和现场配置使用。

## 配置读取优先级

设备启动后按以下顺序读取配置：

1. **SD 卡配置文件**：`/sdcard/CONFIG.JSN`
2. **ESP32 NVS 备份配置**：上一次网页保存或 SD 配置同步后的备份
3. **固件内置默认配置**：没有 SD 卡、SD 卡无配置文件、NVS 也没有配置时使用

如果 SD 卡中存在有效的 `CONFIG.JSN`，设备会优先使用它，并同步一份到 ESP32 NVS。网页保存配置时，会同时尝试写入 SD 卡和 NVS；没有 SD 卡时，至少会保存到 NVS。

> 注意：当前 FATFS 配置关闭了长文件名支持，因此 SD 卡配置文件使用 8.3 文件名 `CONFIG.JSN`，不是 `config.json`。

## 完整示例

把下面内容保存为 SD 卡根目录的 `CONFIG.JSN`：

```json
{
  "version": 2,
  "schedule": {
    "relay1": {
      "start": "18:00",
      "end": "07:00"
    },
    "relay2": {
      "start": "18:00",
      "end": "07:00"
    }
  },
  "nfc": {
    "rules": [
      {
        "data": "00",
        "name": "1类卡",
        "opto12_pulses": 1,
        "open_relay4": false
      },
      {
        "data": "01",
        "name": "2类卡",
        "opto12_pulses": 2,
        "open_relay4": false
      },
      {
        "data": "02",
        "name": "3类卡",
        "opto12_pulses": 3,
        "open_relay4": true
      },
      {
        "data": "03",
        "name": "4类卡",
        "opto12_pulses": 4,
        "open_relay4": true
      },
      {
        "data": "04",
        "name": "5类卡",
        "opto12_pulses": 5,
        "open_relay4": true
      },
      {
        "data": "05",
        "name": "6类卡",
        "opto12_pulses": 6,
        "open_relay4": true
      }
    ]
  },
  "radar": {
    "enabled": true,
    "active_level": 1,
    "trigger_delay_ms": 5000,
    "cycle_window_ms": 20000,
    "opto12_pulses": 1
  }
}
```

## 字段说明

### `version`

配置格式版本号。当前固定为 `2`。

### `schedule`

继电器定时配置。

- `relay1.start`：继电器 1 闭合开始时间，格式为 `HH:MM`。
- `relay1.end`：继电器 1 闭合结束时间，格式为 `HH:MM`。
- `relay2.start`：继电器 2 闭合开始时间，格式为 `HH:MM`。
- `relay2.end`：继电器 2 闭合结束时间，格式为 `HH:MM`。

时间段支持跨天。例如 `18:00` 到 `07:00` 表示当天 18:00 闭合，次日 07:00 断开。

### `nfc.rules`

NFC 卡数据和板子动作的映射表。

- `data`：匹配卡内 `data[0..1]`，写两位十六进制字符，例如 `00`、`01`、`04`。固件只按这个数据匹配，不按 UID 匹配。
- `name`：规则名称，只用于网页显示和日志识别。
- `opto12_pulses`：匹配后 `OPTO1 + OPTO2` 同时输出的脉冲个数。
- `open_relay4`：是否在匹配后断开继电器 4。继电器 4 开机默认闭合，`true` 表示触发后断开，`false` 表示不动作。

内置默认规则如下：

| NFC data | 默认动作 |
| --- | --- |
| `00` | `OPTO1 + OPTO2` 输出 1 个脉冲 |
| `01` | `OPTO1 + OPTO2` 输出 2 个脉冲 |
| `02` | `OPTO1 + OPTO2` 输出 3 个脉冲，并断开继电器 4 |
| `03` | `OPTO1 + OPTO2` 输出 4 个脉冲，并断开继电器 4 |
| `04` | `OPTO1 + OPTO2` 输出 5 个脉冲，并断开继电器 4 |
| `05` | `OPTO1 + OPTO2` 输出 6 个脉冲，并断开继电器 4 |

### `radar`

外部雷达输入配置。

- `enabled`：是否启用雷达触发。
- `active_level`：雷达 INT 有效电平。新版 PCB 默认为高电平有效，所以默认是 `1`。
- `trigger_delay_ms`：收到雷达触发后延时多久输出脉冲，默认 `5000` ms。
- `cycle_window_ms`：雷达周期窗口，默认 `20000` ms。`IO_IN1` 和 `IO_IN2` 共用同一个周期窗口。
- `opto12_pulses`：雷达触发后 `OPTO1 + OPTO2` 同时输出的脉冲个数，默认 `1`。

雷达输入引脚：

| 信号 | ESP32-S3 引脚 | 说明 |
| --- | --- | --- |
| `IO_IN1` | `GPIO16` | 雷达 INT 输入，高电平有效 |
| `IO_IN2` | `GPIO38` | 雷达 INT 输入，高电平有效 |

雷达动作逻辑：

1. 任意一路雷达输入出现高电平触发。
2. 两路输入共同进入一个周期窗口。
3. 延时 `trigger_delay_ms` 后，`OPTO1 + OPTO2` 输出 `opto12_pulses` 个脉冲。
4. 在 `cycle_window_ms` 内再次收到高电平，仍判断为同一个周期，不重复输出脉冲。
5. 窗口结束后，下一次触发才会开启新周期。

## 网页配置

设备启动后会开启 SoftAP：

- SSID：`traffic_light_XXXX`
- 密码：`12345678`
- 配置地址：`http://192.168.4.1/`

网页按左侧栏分区：

1. **时间配置**：默认打开的主要页面，用于配置继电器 1/2 的闭合时间。
2. **NFC 映射**：编辑 NFC `data` 对应的脉冲数和继电器 4 断开动作。
3. **雷达输入**：编辑雷达启用状态、触发延时、周期窗口和脉冲数。
4. **系统状态**：查看配置来源、SD 卡状态和 SoftAP 信息。
