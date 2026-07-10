# 工作站端 TODO — AI 告警优先级修复

> C 端（驾驶舱）已完成：JSON 飞行状态新增 7 个告警位。以下为工作站端需同步修改的内容。

---

## TODO 1: 解析新增的 7 个告警字段

C 端 `serialize_state_json()` 现在多发了这些字段（0=正常, 1=告警中）：

```python
state.get("stall", 0)         # 失速警告
state.get("gpws", 0)          # GPWS 地形警告
state.get("windshear", 0)     # 风切变
state.get("overspeed", 0)     # 超速
state.get("bank_angle", 0)    # 坡度超限
state.get("gear_warn", 0)     # 起落架未放下
state.get("engine_fire", 0)   # 发动机火警
```

---

## TODO 2: 在 `build_prompt()` 开头注入告警块

在构造 prompt 时，**先检查告警，再写飞行参数**。告警信息放在 prompt 最前面，让模型优先关注。

### 告警优先级（从高到低）

| 优先级 | 字段 | 告警文本 |
|--------|------|---------|
| P0 致命 | `engine_fire=1` | `‼ 发动机火警！立即执行发动机火警检查单！` |
| P0 致命 | `stall=1` | `‼ 失速警告！立即推杆减小迎角，最大推力！` |
| P0 致命 | `gpws=1` | `‼ GPWS地形警告！立即拉升！` |
| P0 致命 | `windshear=1` | `‼ 风切变！TOGA推力，保持姿态！` |
| P1 严重 | `overspeed=1` | `⚠ 超速警告！收油门减速！` |
| P1 严重 | `bank_angle=1` | `⚠ 坡度超限！改平坡度！` |
| P1 严重 | `gear_warn=1` | `⚠ 起落架未放下！` |

### 代码示例

```python
def build_prompt(state: dict) -> str:
    lines = []

    # === 1. 活跃告警（最高优先级，放在 prompt 最前面）===
    critical = []
    severe = []

    if state.get("engine_fire", 0):
        critical.append("‼ 发动机火警！立即执行发动机火警检查单！")
    if state.get("stall", 0):
        critical.append("‼ 失速警告！立即推杆减小迎角，最大推力！")
    if state.get("gpws", 0):
        critical.append("‼ GPWS地形警告！立即拉升！")
    if state.get("windshear", 0):
        critical.append("‼ 风切变！TOGA推力，保持姿态！")

    if state.get("overspeed", 0):
        severe.append("⚠ 超速警告！收油门减速！")
    if state.get("bank_angle", 0):
        severe.append("⚠ 坡度超限！改平坡度！")
    if state.get("gear_warn", 0):
        severe.append("⚠ 起落架未放下！")

    if critical:
        lines.append("【致命告警】" + " ".join(critical))
    if severe:
        lines.append("【严重告警】" + " ".join(severe))
    if critical or severe:
        lines.append("")  # 空行分隔

    # === 2. 飞行阶段 (接已有逻辑，不变) ===
    # ... 后面保持现有 build_prompt 逻辑不变 ...
```

---

## TODO 3: 强化 System Prompt

确认 System Prompt 包含告警优先级指令：

```
你是B737-800驾驶舱AI副驾驶。监控飞行参数，用中文提供简洁的操作建议或态势说明。
输出不超过50字。
重要规则：
1. 致命告警（发动机火警、失速、GPWS、风切变）必须优先响应
2. 同时存在多个告警时，先处理最高优先级的
3. 一切正常时报告「正常」
4. 你提供的建议仅供参考，不可替代SOP和GPWS硬告警。
```

---

## TODO 4: 验证

重启推理服务器后，触发多告警并发场景（如 BANK ANGLE + STALL），确认 ADVISORY 优先报告失速而非次要告警。

### 测试 Payload

```json
{
  "alt_ft": 15000, "agl_ft": 8000,
  "ias_kts": 95, "vs_fpm": -3500,
  "mach": 0.15, "roll_deg": 55, "pitch_deg": 15,
  "heading": 270,
  "n1_pct": [45, 48], "egt_c": [720, 735],
  "flap_ratio": 0.1, "gear_down": 1,
  "fuel_lbs": 5000, "oat_c": 5,
  "ap_engaged": 0,
  "master_warning": 1, "master_caution": 0,
  "stall": 1, "gpws": 0, "windshear": 0,
  "overspeed": 0, "bank_angle": 1,
  "gear_warn": 0, "engine_fire": 0
}
```

预期输出应优先提及 `失速` 和 `坡度`，而非次要告警。
