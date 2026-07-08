# LoRA 微调 ChatGLM3-6B 驾驶舱 AI 副驾驶 — 完整实施方案

> **创建日期**: 2026-07-07
> **硬件**: 2× RTX 3090 (24GB×2)
> **基座模型**: ChatGLM3-6B (`THUDM/chatglm3-6b`)
> **微调方法**: QLoRA 4-bit + DeepSpeed ZeRO-2
> **总工期**: 10 天

---

## 一、Context

**目标**: 微调一个轻量级 LLM，根据飞机实时状态参数输出中文简要操作指示/态势说明。

**约束**:
- 2× RTX 3090 (24GB×2)，10 天时间
- **零手工操作** — 不允许手动触发模拟器告警，所有数据必须自动生成
- 基座模型：ChatGLM3-6B

**现状**: 项目中已有：
1. 13 种 GPWS 告警规则 + 10 条高度喊话，精确阈值已知（`src/audio/alert_system.c`）
2. 310 秒全剖面 B737 模拟数据发生器（`src/net/mock_data.c`）
3. HTTP 服务器框架，已有 `/api/position`, `/api/route` 端点（`src/cabin/cabin_server.c`）
4. 80+ 飞行参数在 `FlightDataValues` 结构体中统一定义（`src/data/flight_data.h`）

**核心发现**: mock_data.c 的垂直速度严重失真（爬升率 15,800 fpm，真实 B737 约 2,500 fpm），且完全无随机噪声。因此**必须用 Python 重写飞行剖面模拟器**，使用真实 B737 飞行包线 + 高斯噪声扰动。

---

## 二、总体架构

```
┌─────────────────────────────────────────────────────┐
│  数据生成层 (Python)                                   │
│  scripts/generate_training_data.py                    │
│  ├── flight_profile.py — B737 真实飞行剖面模拟器        │
│  ├── alert_rules.json — 23 条告警/系统规则阈值          │
│  ├── text_variants.py — 每条规则 6 种中文表述变体        │
│  └── 输出: scripts/data/*.jsonl (9500 训练 + 500 测试)  │
└─────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────┐
│  训练层 (LLaMA-Factory)                                │
│  QLoRA r=16 alpha=32, DeepSpeed ZeRO-2, 3 epochs      │
│  预计 5-6 小时                                         │
│                    ↓                                  │
│  models/cockpit-advisor/ (LoRA adapter → merge → GGUF) │
└─────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────┐
│  推理层 (Python FastAPI + llama.cpp)                   │
│  POST /api/copilot/advisory → 飞行状态JSON → 建议文本   │
│  端口: 8090, 延迟: <200ms                              │
└─────────────────────────────────────────────────────┘
                    ↑ HTTP POST (每 500ms 轮询)
┌─────────────────────────────────────────────────────┐
│  C 端                                                │
│  cabin_server.c: GET /api/fullstate → 完整状态 JSON    │
│  ai/ai_advisor.c: 异步 HTTP 客户端                     │
│  instruments/eicas.c: AI ADVISORY 文本显示              │
│  ⚠️ GPWS 硬告警始终优先，LLM 仅为补充软建议             │
└─────────────────────────────────────────────────────┘
```

**安全原则**: GPWS 硬告警 (`alert_system.c`) 保持不可绕过，LLM 输出永远标注 "AI ADVISORY" 且以较暗颜色显示。

---

## 三、10 天时间线

### 🔴 Day 1-2: 数据生成引擎（最关键的两天）

#### 文件 1: `scripts/flight_profile.py` — Python 飞行剖面模拟器

用 Python 重写 mock_data.c，修正为真实 B737 包线：

| 参数 | 原 C mock_data | 修正为真实 B737 |
|------|---------------|-------------------|
| 爬升率 | 15,800 fpm | 2,500 (CLIMB1) / 1,800 (CLIMB2) fpm |
| 下降率 | -23,000 fpm | -1,800 (DESCENT) / -800 (APPROACH) fpm |
| N1 范围 | 22-96% | 22-98% (带高斯噪声 σ=3%) |
| 巡航高度 | 固定 FL350 | 每周期随机 FL330-FL370 |
| 进近速度 | 固定 140 kts | Vref+5, 随机 130-150 kts |

增强项：
- 每个参数加高斯噪声 (σ=2-5% 量程)
- 每周期随机初始条件：重量 ±10%、巡航高度 ±2000ft、OAT 偏差 ±10°C ISA
- 产生 20 条不同剖面，每条 310s @ 10Hz = 3,100 帧 × 20 = **62,000 帧数据池**

8 个相位时序（与 C 版本一致）：
```
TAKEOFF(10s) → CLIMB1(30s) → CLIMB2(50s) → CRUISE(70s) →
DESCENT(60s) → APPROACH(60s) → LANDING(20s) → TAXI(10s) → 循环
```

#### 文件 2: `scripts/alert_rules.json` — 规则阈值配置

从 `alert_system.c` 精确提取的 13 条 GPWS 规则 + 10 条系统规则 + 10 条高度喊话：

```json
{
  "alert_types": {
    "PULL_UP": {
      "priority": 0, "cooldown_s": 2.5,
      "conditions": {"agl_ft": {"max": 100}, "vs_fpm": {"max": -2000}},
      "plausible_phases": ["DESCENT", "APPROACH", "LANDING"]
    },
    "WINDSHEAR": {
      "priority": 1, "cooldown_s": 2.0,
      "conditions": {"ias_delta_abs": {"min": 20}, "agl_ft": {"max": 2000}},
      "plausible_phases": ["TAKEOFF", "CLIMB1", "APPROACH", "LANDING"]
    },
    "STALL": {
      "priority": 11, "cooldown_s": 1.5,
      "conditions": {"ias_kts": {"max": 110}, "agl_ft": {"min": 0}},
      "plausible_phases": ["TAKEOFF", "CLIMB1", "APPROACH", "LANDING"]
    },
    "MASTER_WARNING": {
      "priority": 2, "cooldown_s": 2.0,
      "conditions": {"master_warning": {"eq": 1}}
    },
    "MASTER_CAUTION": {
      "priority": 3, "cooldown_s": 2.0,
      "conditions": {"master_caution": {"eq": 1}}
    },
    "TERRAIN": {
      "priority": 4, "cooldown_s": 3.0,
      "conditions": {"agl_ft": {"min": 100, "max": 1000}, "vs_fpm": {"max": -1500}},
      "plausible_phases": ["DESCENT", "APPROACH"]
    },
    "SINK_RATE": {
      "priority": 5, "cooldown_s": 3.0,
      "conditions": {"agl_ft": {"min": 500, "max": 2500}, "vs_fpm": {"max": -1500}},
      "plausible_phases": ["DESCENT", "APPROACH"]
    },
    "TOO_LOW_GEAR": {
      "priority": 6, "cooldown_s": 2.0,
      "conditions": {"agl_ft": {"max": 500}, "gear_deployed": {"eq": 0}, "vs_fpm": {"max": -50}},
      "plausible_phases": ["APPROACH", "LANDING"]
    },
    "TOO_LOW_FLAPS": {
      "priority": 7, "cooldown_s": 2.0,
      "conditions": {"agl_ft": {"max": 500}, "flap_ratio": {"max": 0.25}, "vs_fpm": {"max": -50}},
      "plausible_phases": ["APPROACH", "LANDING"]
    },
    "GLIDESLOPE": {
      "priority": 8, "cooldown_s": 2.0,
      "conditions": {"agl_ft": {"max": 1000}, "nav1_cdi_abs": {"min": 0.5}},
      "plausible_phases": ["APPROACH"]
    },
    "BANK_ANGLE": {
      "priority": 9, "cooldown_s": 2.0,
      "conditions": {"agl_ft": {"max": 2000}, "roll_deg_abs": {"min": 35}},
      "plausible_phases": ["CLIMB1", "CLIMB2", "DESCENT", "APPROACH"]
    },
    "OVERSPEED": {
      "priority": 10, "cooldown_s": 1.5,
      "conditions": {"ias_kts": {"min": 340}},
      "plausible_phases": ["CRUISE", "DESCENT"]
    },
    "MINIMUMS": {
      "priority": 12, "cooldown_s": 5.0,
      "conditions": {"agl_ft": {"crossing_descend": 200}},
      "plausible_phases": ["APPROACH"]
    }
  },
  "altitude_callouts": [500, 400, 300, 200, 100, 50, 40, 30, 20, 10],
  "system_rules": {
    "ENG_OVERHEAT":    {"conditions": {"egt_c_any": {"min": 850}}, "plausible_phases": ["TAKEOFF", "CLIMB1", "CLIMB2"]},
    "ENG_ASYM":        {"conditions": {"n1_diff_pct": {"min": 5.0}}},
    "FUEL_IMBALANCE":  {"conditions": {"fuel_imbalance_lbs": {"min": 1000}}},
    "OIL_PRESS_LOW":   {"conditions": {"oil_press_psi_any": {"max": 25}}},
    "CABIN_ALT_HIGH":  {"conditions": {"cabin_alt_ft": {"min": 10000}}},
    "BUS_VOLT_ABNORM": {"conditions": {"elec_bus_volts": {"max": 24}, "elec_bus_volts_alt": {"min": 32}}},
    "TAKEOFF_CONFIG":  {"conditions": {"phase": "TAKEOFF", "flap_ratio": {"max": 0.1}}},
    "LOW_FUEL":        {"conditions": {"fuel_total_lbs": {"max": 3000}}},
    "ICING_CONDITION": {"conditions": {"oat_c": {"max": 10}, "anti_ice_wing": {"eq": 0}}},
    "APU_FIRE":        {"conditions": {"apu_egt_c": {"min": 760}, "apu_running": {"eq": 1}}}
  },
  "normal_ranges": {
    "n1_pct": [20, 97], "egt_c": [100, 950], "oil_press_psi": [25, 65],
    "oil_temp_c": [20, 120], "cabin_alt_ft": [0, 8000], "cabin_diff_psi": [0, 8.5],
    "hyd_press_psi": [2800, 3200], "elec_bus_volts": [24, 32],
    "fuel_total_lbs": [500, 20000]
  }
}
```

#### 文件 3: `scripts/text_variants.py` — 每条规则 6 种中文表述

```python
ALERT_TEMPLATES = {
    "PULL_UP": [
        "PULL UP！无线电高度{agl:.0f}英尺，下降率{vs:.0f}英尺/分钟，立即拉起！",
        "警告！近地！高度{agl:.0f}英尺，下降率{vs:.0f}英尺/分钟，执行改出程序！",
        "拉起！拉起！地形接近！高度{agl:.0f}英尺！推大油门带杆拉起！",
        "PULL UP！{agl:.0f}英尺！下降率{vs:.0f}英尺/分钟！最大推力复飞！",
        "紧急拉起！{agl:.0f}英尺地形迫近！垂直速度{vs:.0f}英尺/分钟！",
        "PULL UP! PULL UP! AGL {agl:.0f}ft, sink {vs:.0f}fpm, ROTATE NOW!",
    ],
    "STALL": [
        "失速警告！空速{ias:.0f}节低于110节，推杆减小迎角，增加推力！",
        "失速！失速！空速仅{ias:.0f}节！推杆！推油门至TOGA！",
        "STALL！{ias:.0f}节！立即减小迎角，最大推力！",
        "失速警告！空速{ias:.0f}节，收减速板，推油门至最大！",
        "飞机失速！{ias:.0f}节，执行失速改出：推杆→推力→改平！",
        "STALL WARNING! {ias:.0f}kts, PUSH NOSE DOWN, MAX THRUST!",
    ],
    "BANK_ANGLE": [
        "坡度警告！当前坡度{roll:.0f}度超过35度限制，立即改平坡度！",
        "BANK ANGLE！坡度{roll:.0f}度！向下压杆减小坡度至30度以下！",
        "坡度{roll:.0f}度过大！检查飞行指引，向反方向压杆改平！",
        "坡度超限！坡度{roll:.0f}度，高度仅{agl:.0f}英尺，立即改平！",
        "坡度角{roll:.0f}度！收杆改平！保持机翼水平！",
        "CAUTION! BANK ANGLE {roll:.0f}° at {agl:.0f}ft AGL! LEVEL WINGS!",
    ],
    "SINK_RATE": [
        "注意：下降率{vs:.0f}英尺/分钟过大！当前高度{agl:.0f}英尺，减小下降率至1000以下！",
        "SINK RATE！垂直速度{vs:.0f}英尺/分钟，带杆减小下降率！",
        "下沉率过大！{vs:.0f}英尺/分钟，高度{agl:.0f}英尺，控制下降率！",
        "下降率警告！{vs:.0f}fpm，{agl:.0f}英尺，立即减小至1500fpm以下！",
        "SINK RATE {vs:.0f}fpm! {agl:.0f}ft AGL! REDUCE SINK RATE!",
        "注意下沉！当前{agl:.0f}英尺，下降率{vs:.0f}，柔和带杆修正！",
    ],
    "TOO_LOW_GEAR": [
        "起落架未放下！高度低于500英尺！立即放下起落架！",
        "TOO LOW GEAR！{agl:.0f}英尺！起落架手柄放下！",
        "警告：起落架未放下！当前高度{agl:.0f}英尺，执行复飞或立即放轮！",
        "GEAR NOT DOWN! {agl:.0f}ft! LOWER LANDING GEAR NOW!",
        "起落架！起落架！高度{agl:.0f}英尺，起落架手柄未放下！",
        "高度{agl:.0f}英尺，起落架未放出，立即检查并放下起落架！",
    ],
    "TOO_LOW_FLAPS": [
        "襟翼未放置着陆构型！高度{agl:.0f}英尺！当前襟翼{flap:.0%}，需至少25%！",
        "TOO LOW FLAPS！{agl:.0f}英尺襟翼仅{flap:.0%}！放襟翼至着陆位！",
        "警告：着陆襟翼未设置！{agl:.0f}英尺襟翼{flap:.0%}，立即放襟翼！",
        "FLAPS NOT SET! {agl:.0f}ft, flaps {flap:.0%}, SET FLAPS!",
        "襟翼警告！高度{agl:.0f}英尺，襟翼{flap:.0%}不满足着陆要求！",
        "进近构型警告：{agl:.0f}英尺，襟翼仅{flap:.0%}，需放至襟翼30或40！",
    ],
    "GLIDESLOPE": [
        "下滑道偏离！CDI偏差{cdi:.1f}个点，{agl:.0f}英尺，修正下滑道！",
        "GLIDESLOPE！偏差{cdi:.1f}点！{agl:.0f}英尺！复飞或修正！",
        "下滑道警告！偏离{cdi:.1f}个点，高度{agl:.0f}英尺，修正航迹！",
        "GLIDESLOPE DEVIATION {cdi:.1f} dots at {agl:.0f}ft! CORRECT!",
        "注意下滑道！当前偏差{cdi:.1f}点，低于{agl:.0f}英尺，立即修正！",
        "下滑道偏离{cdi:.1f}点！{agl:.0f}英尺！检查ILS进近参数！",
    ],
    "OVERSPEED": [
        "超速警告！空速{ias:.0f}节超过340节限制！收油门减速！",
        "OVERSPEED！{ias:.0f}节！立即减速至340节以下！",
        "速度过快！{ias:.0f}节超限！收油门，必要时放出减速板！",
        "OVERSPEED {ias:.0f}kts! REDUCE THRUST, EXTEND SPEEDBRAKE!",
        "超速！{ias:.0f}节！减小推力！减速至巡航速度！",
        "速度警告：{ias:.0f}节超过Vmo限制，收油门减速至340节以下！",
    ],
    "TERRAIN": [
        "地形警告！{agl:.0f}英尺！下降率{vs:.0f}英尺/分钟！拉起！",
        "TERRAIN! TERRAIN! {agl:.0f}ft, {vs:.0f}fpm descent! PULL UP!",
        "地形迫近！高度{agl:.0f}英尺，下沉率{vs:.0f}英尺/分钟！",
        "注意地形！{agl:.0f}英尺！下降率过大！立即减小下降率！",
        "TERRAIN AHEAD! {agl:.0f}ft AGL, sink {vs:.0f}fpm! CORRECT!",
        "地形警告：{agl:.0f}英尺，{vs:.0f}英尺/分钟下降中，立即改平！",
    ],
    "WINDSHEAR": [
        "风切变！空速突变{delta:.0f}节！{agl:.0f}英尺！最大推力保持姿态！",
        "WINDSHEAR! WINDSHEAR! IAS change {delta:.0f}kts at {agl:.0f}ft!",
        "风切变警告！空速变化{delta:.0f}节！设置TOGA推力！保持俯仰姿态！",
        "风切变！{delta:.0f}节速度突变！{agl:.0f}英尺！执行风切变改出程序！",
        "WINDSHEAR! {delta:.0f}kts! {agl:.0f}ft! TOGA THRUST, PITCH 15°!",
        "风切变探测！空速变化{delta:.0f}节，{agl:.0f}英尺，立即执行改出！",
    ],
    "MASTER_WARNING": [
        "主警告！检查EICAS告警信息！立即确认并执行相应检查单！",
        "MASTER WARNING! 检查所有系统告警！确认故障源！",
        "主警告灯亮！检查发动机/燃油/液压/电源系统状态！",
        "WARNING! MASTER WARNING ACTIVE! CHECK SYSTEMS IMMEDIATELY!",
        "注意！主警告触发！扫描仪表确认故障，执行非正常检查单！",
        "主警告！立即检查各系统参数，确认故障后执行QRH程序！",
    ],
    "MASTER_CAUTION": [
        "主警戒！检查系统异常指示，确认并评估是否需要机组响应！",
        "MASTER CAUTION! 检查EICAS异常的琥珀色参数！",
        "主警戒灯亮！某项系统参数偏离正常范围，核实具体故障！",
        "CAUTION! MASTER CAUTION ACTIVE! CHECK AMBER ALERTS!",
        "注意：主警戒触发了！检查各系统有无琥珀色异常显示！",
        "主警戒触发，扫描仪表确认非正常参数，按需执行检查单！",
    ],
    "MINIMUMS": [
        "决断高度！200英尺！",
        "MINIMUMS! 200ft! DECISION HEIGHT!",
        "决断高200英尺！",
        "MINIMUMS! DECISION HEIGHT REACHED!",
        "200英尺决断高度！",
        "决断高度！MINIMUMS! 200ft!",
    ],
    "NORMAL_CRUISE": [
        "巡航中：FL350，马赫0.78，航向{hdg:.0f}，双发正常，燃油{fuel:.0f}磅，一切正常。",
        "当前{phase}：{alt:.0f}英尺，IAS{ias:.0f}节，N1各{n1_0:.0f}%/{n1_1:.0f}%，状态正常。",
        "飞行状态正常：{phase}，{alt:.0f}英尺，{ias:.0f}节，双发{egt_0:.0f}/{egt_1:.0f}°C。",
        "正常巡航：FL{fl:.0f}，马赫{ mach:.2f}，燃油{ fuel:.0f}磅，预计续航{hours:.1f}小时。",
        "一切正常：{phase}，地速{gs:.0f}节，OAT{oat:.0f}°C，航向{hdg:.0f}。",
        "NORMAL: {phase}, FL{fl:.0f}, M{mach:.2f}, {ias:.0f}KIAS, FUEL {fuel:.0f}LBS.",
    ],
    # ... more templates for each phase and each system deviation rule
}
```

#### 文件 4: `scripts/generate_training_data.py` — 数据生成主脚本

```python
"""
主数据生成脚本。

生成 10,000 条训练样本 (ShareGPT 格式):
  A_GPWS   : 3,500 条 — 13 种 GPWS 告警，每条约 270 条
  B_PHASE  : 2,500 条 — 8 个飞行阶段的正常状态摘要
  C_SYSTEM : 2,500 条 — 10 种系统参数偏差
  D_COMBINED: 1,000 条 — 多条件重叠场景
  E_NORMAL :   500 条 — 正常巡航例行检查

输出:
  scripts/data/training_data.jsonl  (9,000 条)
  scripts/data/eval_data.jsonl      (1,000 条)
"""

# 工作流程:
# 1. 从 alert_rules.json 加载 23 条规则
# 2. 初始化 FlightProfile (seed=42)
# 3. 生成 20 条剖面 → 62,000 帧数据池
# 4. 对每条规则:
#    a. 从剖面池筛选匹配飞行阶段的帧
#    b. 参数空间采样 + 高斯噪声扰动
#    c. 随机选择 1 个文本变体模板
#    d. 替换 {agl} {vs} {n1} 等占位符
#    e. 对同一帧生成 "正常" 对照样本 (告警未触发但有参数时)
# 5. Shuffle + 90/10 Split
# 6. 写 JSONL 文件 + 统计报告
```

**ShareGPT 输出格式**:
```json
{
  "conversations": [
    {
      "role": "system",
      "content": "你是B737-800驾驶舱AI副驾驶。监控飞行参数，用中文提供简洁的操作建议或态势说明。输出不超过50字。一切正常时报告"正常"，异常时指出具体问题和建议操作。你提供的建议仅供参考，不可替代SOP和GPWS硬告警。"
    },
    {
      "role": "user",
      "content": "飞行阶段: APPROACH\n高度: 1500ft MSL / 350ft AGL\n空速: 145 KIAS\n垂直速度: -2100 fpm\nN1: 58%, 61%\nEGT: 680°C, 695°C\n襟翼: 0.15 (15%)\n起落架: 未放下\n航向: 090°\n燃油: 8500 lbs\nAP: 接通"
    },
    {
      "role": "assistant",
      "content": "注意：SINK RATE! 下降率2100英尺/分钟，350英尺高度，立即减小下降率。起落架未放下！高度低于500英尺，立刻放下起落架。"
    }
  ]
}
```

#### 文件 5: `scripts/validate_data.py` — 数据质量检查

- 检查占位符全部替换（无 `{agl}` 残留）
- 检查文本长度 (20-200 字符)
- 字段完整性检查
- 精确去重
- 打印每类别统计 + 3 条随机样本

---

### 🟡 Day 3: 环境搭建

#### Python 环境
```bash
conda create -n cockpit-lora python=3.10
conda activate cockpit-lora
pip install torch==2.1.2 --index-url https://download.pytorch.org/whl/cu121
pip install transformers==4.38.2 datasets accelerate peft bitsandbytes
pip install deepspeed sentencepiece protobuf
git clone https://github.com/hiyouga/LLaMA-Factory.git
cd LLaMA-Factory && pip install -e ".[torch,metrics]"
pip install llama-cpp-python --extra-index-url https://abetlen.github.io/llama-cpp-python/whl/cu121
```

#### 训练配置: `config/cockpit_lora.yaml`
```yaml
model_name_or_path: THUDM/chatglm3-6b
stage: sft
finetuning_type: lora
quantization_bit: 4
quantization_method: nf4
lora_rank: 16
lora_alpha: 32
lora_dropout: 0.05
lora_target: all

dataset: cockpit_advisory
template: chatglm3
cutoff_len: 2048

output_dir: models/cockpit-advisor
per_device_train_batch_size: 8
gradient_accumulation_steps: 2
learning_rate: 2.0e-4
num_train_epochs: 3.0
lr_scheduler_type: cosine
warmup_steps: 100
bf16: true

deepspeed: config/ds_z2_config.json
ddp_timeout: 180000000

logging_steps: 10
save_steps: 500
eval_steps: 500
val_size: 0.1
```

#### DeepSpeed: `config/ds_z2_config.json`
```json
{
  "train_batch_size": "auto",
  "train_micro_batch_size_per_gpu": 8,
  "gradient_accumulation_steps": 2,
  "zero_optimization": {"stage": 2},
  "bf16": {"enabled": true}
}
```

---

### 🟢 Day 4-5: 训练 + 评估

#### 训练启动
```bash
cd LLaMA-Factory
llamafactory-cli train config/cockpit_lora.yaml
```
- 预计: **5-6 小时** (2×3090, batch=8×2×2=32, 9000 样本, 3 epochs ≈ 844 steps)
- GPU 内存: ~12-14GB/卡 (QLoRA 4-bit + ZeRO-2)
- 目标: train loss 从 ~2.0 收敛到 ~0.3-0.5

#### 评估脚本: `scripts/evaluate_model.py`
- 加载 GGUF 模型 → 对 100 条 eval 样本推理
- 人工标注: 正确 / 部分正确 / 错误 / 有害
- **目标: 正确率 > 80%, 有害率 < 2%**

#### 模型导出
```bash
# 合并 LoRA → 完整权重
llamafactory-cli export config/cockpit_export.yaml
# 转 GGUF Q4_K_M
python llama.cpp/convert-hf-to-gguf.py models/cockpit-advisor-merged/ \
    --outtype q4_k_m --outfile models/cockpit-advisor-q4km.gguf
```
目标: ~4GB, 推理 < 50ms/token

---

### 🔵 Day 6-7: 迭代优化

1. 根据评估结果标记问题类别
2. 针对性增强数据 (问题类别 ×1.5 样本量)
3. 第二轮训练 (~3 小时)
4. 对比评估，确定最优模型
5. 可选: 尝试 r=32 或 5 epoch

---

### 🟣 Day 8-9: C 端集成

#### 修改 1: `src/cabin/cabin_server.c` — 新增 `/api/fullstate`

参照已有 `api_position()` 模式，新增 `api_state_full()`:
```c
// 序列化 15 个核心飞行参数为 JSON (约 1KB)
// {"alt_ft":35000,"agl_ft":34000,"ias_kts":280,"vs_fpm":50,
//  "roll_deg":2.1,"pitch_deg":2.5,"heading":125.0,
//  "n1_pct":[60.2,60.8],"egt_c":[700,705],
//  "flap_ratio":0.0,"gear_down":0,"fuel_lbs":8500,
//  "ap_engaged":1,"master_warning":0,"master_caution":0,
//  "oat_c":-39,"cabin_alt_ft":6500,"hyd_press_psi":[3000,3000]}
```

路由注册:
```c
if (strcmp(url_path, "/api/fullstate") == 0) {
    char body[2048];
    api_state_full(server, body, sizeof(body));
    // ... HTTP 200 + JSON response
}
```

#### 新增 2: `src/ai/ai_advisor.c` + `src/ai/ai_advisor.h`

轻量 HTTP 客户端，异步请求 Python 推理服务器:
- `ai_advisor_create(host, port)` — 初始化
- `ai_advisor_request(fd)` — 非阻塞请求, 缓存结果
- `ai_advisor_get_advisory()` — 获取最新建议文本
- 轮询频率: 2 Hz, 超时 1s
- 推理服务器不可用时优雅降级 (返回 NULL)

#### 修改 3: `src/instruments/eicas.c` — 显示 AI 建议

在 EICAS 底部添加 AI ADVISORY 文本行:
```
IF GPWS 硬告警激活:
    GPWS 告警 (大红闪烁) — 绝对优先
    AI ADVISORY (小灰字) — "AI ADVISORY — SUPPLEMENTARY"
ELSE:
    AI ADVISORY (黄色正常字) — AI 建议文本
```

#### 修改 4: `config/default.cfg`
```ini
[ai]
# AI Co-pilot inference server
inference_host = 127.0.0.1
inference_port = 8090
enabled = true
```

#### 推理服务器: `scripts/inference_server.py`
```python
# FastAPI, 端口 8090
# POST /api/copilot/advisory
#   输入: JSON 飞行状态
#   输出: {"advisory": "...", "severity": "info|caution|warning", "latency_ms": 50}
# 参数: temperature=0.1, max_tokens=128, n_gpu_layers=-1
```

---

### ⚪ Day 10: 测试 + 打磨

1. 端到端测试: cockpit.exe → Mock Data → /api/fullstate → 推理服务器 → EICAS 显示
2. 异常测试: 推理服务器宕机/超时/返回垃圾文本 → 优雅降级
3. 性能: 推理 < 200ms, C 端帧率无下降
4. Demo 视频 + 文档

---

## 四、文件清单

### 新增文件 (17 个)

| # | 文件 | 说明 |
|---|------|------|
| 1 | `scripts/flight_profile.py` | B737 真实飞行剖面 Python 模拟器 |
| 2 | `scripts/alert_rules.json` | 23 条告警/系统规则阈值配置 |
| 3 | `scripts/text_variants.py` | 每条规则 6 种中文表述变体 |
| 4 | `scripts/generate_training_data.py` | 数据生成主脚本 (10,000 条) |
| 5 | `scripts/validate_data.py` | 数据质量验证 |
| 6 | `scripts/evaluate_model.py` | 模型评估 (100 条人工抽检) |
| 7 | `scripts/inference_server.py` | FastAPI 推理服务器 |
| 8 | `scripts/data/` | 生成的训练/评估数据目录 |
| 9 | `config/cockpit_lora.yaml` | LLaMA-Factory 训练配置 |
| 10 | `config/ds_z2_config.json` | DeepSpeed ZeRO-2 配置 |
| 11 | `config/cockpit_export.yaml` | LoRA 合并导出配置 |
| 12 | `src/ai/ai_advisor.h` | C 端 AI 客户端头文件 |
| 13 | `src/ai/ai_advisor.c` | C 端 AI 客户端实现 |
| 14 | `docs/LORA_AI_副驾驶实施方案.md` | 本文档 |

### 修改文件 (5 个)

| # | 文件 | 修改 |
|---|------|------|
| 15 | `src/cabin/cabin_server.c` | 新增 `GET /api/fullstate` 端点 |
| 16 | `src/instruments/eicas.c` | 新增 AI ADVISORY 文本显示行 |
| 17 | `Makefile` | 新增 `src/ai` 目录到编译路径 |
| 18 | `config/default.cfg` | 新增 `[ai]` 配置段 |
| 19 | `src/app.c` / `src/app.h` | 初始化 CopilotClient |

---

## 五、验证方案

| 阶段 | 验证方法 | 通过标准 |
|------|---------|---------|
| Day 2 数据 | `python validate_data.py` | 0 残留占位符, 0 重复, 长度 20-200 |
| Day 5 训练 | TensorBoard loss 曲线 | train loss ~0.3-0.5, val loss 不上升 |
| Day 5 评估 | `python evaluate_model.py` | 正确率 > 80%, 有害率 < 2% |
| Day 9 集成 | curl + 眼检 EICAS | AI 建议随飞行阶段变化, 告警时正确显示 |
| Day 10 性能 | 帧率监控 | EICAS 60fps, AI 推理 < 200ms |

---

## 六、风险矩阵

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|---------|
| 飞行剖面仍不真实 | 低 | 中 | Day 1 先跑一条剖面人工验证参数 |
| GLM-6B 不理解航空参数 | 中 | 高 | system prompt 包含参数中文说明；input 含参数名 |
| 训练 OOM | 低 | 中 | 降 batch_size 到 2, 或用 1 卡训练 |
| 推理延迟 > 500ms | 低 | 低 | llama.cpp Q4_K_M 在 3090 上 6B 通常 < 30ms/token |
| 模型生成危险建议 | 中 | 高 | 低 temperature + 输出后处理 + GPWS 始终优先 |
| 无法完成 10 天工期 | 低 | 高 | 数据生成是关键路径，Day 1-2 投入最多精力 |
