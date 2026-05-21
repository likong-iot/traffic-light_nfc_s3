# 配置模板目录

这个目录存放 NFC 路灯控制器的配置文件模板和说明。现场调试时，可以直接复制模板到 SD 卡根目录使用。

## 文件列表

| 文件 | 用途 |
| --- | --- |
| `CONFIG.JSN` | 标准完整模板。推荐直接复制到 SD 卡根目录使用。 |
| `CONFIG_MIN.JSN` | 内容更紧凑的标准模板，字段和 `CONFIG.JSN` 一致。 |
| `NFC_RULES_EXAMPLE.JSN` | NFC 自定义数据示例，用于测试非默认 `data` 映射。 |
| `RADAR_ONLY_EXAMPLE.JSN` | 雷达参数调整示例，演示修改延时和周期窗口。 |
| `字段说明.md` | 每个字段的中文解释和取值范围。 |
| `动作表.md` | 默认 NFC 卡、继电器、雷达动作表。 |
| `现场使用步骤.md` | SD 卡配置文件的复制、上电、网页保存和排查步骤。 |

## 使用方式

1. 选择一个模板文件。
2. 如果文件名不是 `CONFIG.JSN`，先把文件复制一份并重命名为 `CONFIG.JSN`。
3. 把 `CONFIG.JSN` 放到 SD 卡根目录。
4. 插入 SD 卡后上电，设备会优先读取 SD 卡里的 `/sdcard/CONFIG.JSN`。
5. 网页保存配置时，会同时写入 SD 卡和 ESP32 NVS。

## 文件名注意事项

当前固件的 FATFS 配置关闭了长文件名支持，所以真正给设备使用的文件名必须是 8.3 格式：

```text
CONFIG.JSN
```

不要把正式配置文件命名为 `config.json` 或 `traffic_light_config.json`，否则设备可能读不到。
