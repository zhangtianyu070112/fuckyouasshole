# B737-800 告警 ↔ X-Plane 12 数据源映射表

> 创建日期: 2026-07-08
> 用途: LoRA 训练数据特征工程 + 告警逻辑树数据源
> 原则: 模型不检测告警，只基于告警状态给建议

---

## 数据源三类

| 来源 | 说明 | 当前状态 |
|------|------|---------|
| **UDP DATA Group** | X-Plane 12 原生 DATA 包，已通过 DSEL 订阅 | ✅ 已接收 |
| **DREF (RREF)** | X-Plane 12 DataRef，需主动请求订阅 | ❌ 未实现 |
| **Derived** | 从已有参数计算得出，无需额外数据源 | ✅ 可计算 |

---

## 一、GPWS 告警 (13 条)

### 已覆盖 — `alert_rules.json` alert_types

| # | 告警名 | 优先级 | 触发条件 | 数据源 | X-Plane 数据路径 | FlightDataValues 字段 | 输入? |
|---|--------|--------|---------|--------|-----------------|----------------------|-------|
| 1 | **PULL_UP** | 0 | AGL<100ft + VS>-2000fpm | UDP G20+G4 | `altitude_agl_ft` + `vs_fpm` | `altitude_agl_ft`, `vs_fpm` | ❌AGL/VVI未显式列出 |
| 2 | **WINDSHEAR** | 1 | IAS突变>20kt + AGL<2000ft | Derived | `ias_kts` 时间差分 | `ias_kts` (需维护历史) | ❌ |
| 3 | **MASTER_WARNING** | 2 | master_warning=1 | UDP G113 | Group 113 field[0] | `master_warning` | ❌ |
| 4 | **MASTER_CAUTION** | 3 | master_caution=1 | UDP G113 | Group 113 field[1] | `master_caution` | ❌ |
| 5 | **TERRAIN** | 4 | 100<AGL<1000 + VS>-1500fpm | UDP G20+G4 | 同 PULL_UP | 同 PULL_UP | ❌ |
| 6 | **SINK_RATE** | 5 | 500<AGL<2500 + VS>-1500fpm | UDP G20+G4 | 同上 | 同上 | ❌ |
| 7 | **TOO_LOW_GEAR** | 6 | AGL<500 + gear=0 + VS>-50fpm | UDP G20+G14 | `altitude_agl_ft` + `gear_deployed` | `gear_deployed` | ☆部分 |
| 8 | **TOO_LOW_FLAPS** | 7 | AGL<500 + flap<25% + VS>-50fpm | UDP G20+G13 | `altitude_agl_ft` + `flap_ratio` | `flap_ratio` | ☆部分 |
| 9 | **GLIDESLOPE** | 8 | AGL<1000 + CDI>0.5dot | UDP G20+G99 | `altitude_agl_ft` + `nav1_cdi` | `nav1_cdi` (struct 有，input 无) | ❌ |
| 10 | **BANK_ANGLE** | 9 | AGL<2000 + roll>35° | UDP G17 | Group 17 field[1] | `roll_deg` (struct 有，input 无) | ❌ |
| 11 | **OVERSPEED** | 10 | IAS>340kt | UDP G3 | Group 3 field[0] | `ias_kts` | ☆已有 |
| 12 | **STALL** | 11 | IAS<110kt | UDP G3 | Group 3 field[0] | `ias_kts` | ☆已有 |
| 13 | **MINIMUMS** | 12 | AGL 过 200ft (下降中) | UDP G20 | `altitude_agl_ft` | (struct 有，input 无) | ❌ |

> ☆ = 字段在 input 但未在告警状态块中显式标注
> ❌ = 字段完全未出现在 format_user_input 中

### 告警状态位 DREF（直接读硬信号，最可靠）

如果读 DREF 就能跳过阈值判断，直接得到告警是否活跃：

| 告警 | 告警状态 DREF |
|------|-------------|
| BANK_ANGLE | `sim/cockpit/warnings/annunciators/bank_angle` |
| GEAR (TOO_LOW_GEAR) | `sim/cockpit/warnings/annunciators/gear_warning` |
| GPWS 总告警 | `sim/cockpit/warnings/annunciators/GPWS` |
| STALL | `sim/cockpit/warnings/annunciators/stall_warning` |
| OVERSPEED | `sim/cockpit/warnings/annunciators/overspeed` |
| WINDSHEAR | `sim/cockpit/warnings/annunciators/windshear` |
| MASTER_WARNING | `sim/cockpit/warnings/annunciators/master_warning` |
| MASTER_CAUTION | `sim/cockpit/warnings/annunciators/master_caution` |

---

## 二、系统告警 (10 条 → 扩展到 ~20 条)

### 已有 — `alert_rules.json` system_rules

| # | 告警名 | 触发条件 | 数据源 | X-Plane 数据路径 | FlightDataValues 字段 |
|---|--------|---------|--------|-----------------|----------------------|
| 14 | **ENG_OVERHEAT** | EGT>850°C | UDP G47 | Group 47 | `egt_c[0..3]` |
| 15 | **ENG_ASYM** | N1差>5% | UDP G41 | Group 41 | `n1_pct[0..3]` |
| 16 | **FUEL_IMBALANCE** | 左右差>1000lbs | UDP G62 (total only) | 需各油箱分别读 | `fuel_total_lbs` (不够) |
| 17 | **OIL_PRESS_LOW** | 滑油压力<25psi | UDP G49 | Group 49 | `oil_press_psi[0..3]` |
| 18 | **CABIN_ALT_HIGH** | 座舱高度>10000ft | UDP G120 | Group 120 field[0] | `cabin_alt_ft` |
| 19 | **BUS_VOLT_ABNORM** | 电压<23V 或 >33V | UDP G121 | Group 121 field[5] | `elec_bus_volts` |
| 20 | **TAKEOFF_CONFIG** | 起飞+襟翼<10% | UDP G13 + phase derive | `flap_ratio` + phase logic | `flap_ratio` |
| 21 | **LOW_FUEL** | 总燃油<3000lbs | UDP G62 | Group 62 | `fuel_total_lbs` |
| 22 | **ICING_CONDITION** | OAT<10°C + 防冰未开 | UDP G5+G109 | Group 5 field[3] + Group 109 | `oat_c`, `anti_ice_wing` |
| 23 | **APU_FIRE** | APU EGT>760°C + APU运行 | UDP G121 | Group 121 field[1],[2] | `apu_egt_c`, `apu_running` |

### 建议新增 — 系统级告警

| # | 告警名 | 触发条件 | 数据源 | X-Plane 数据路径 | DREF (如需) |
|---|--------|---------|--------|-----------------|------------|
| 24 | **STALL_WARNING** | 失速警告活跃 | DREF | `sim/cockpit/warnings/annunciators/stall_warning` | ✅ 直接读 |
| 25 | **CONFIG_LANDING** | AGL<500 + (gear=0 或 flap<25%) | Derived (TOO_LOW_GEAR ∪ TOO_LOW_FLAPS) | 组合判断 | — |
| 26 | **CONFIG_TAKEOFF** | 起飞推力+构型不满足 | Derived | TAKEOFF_CONFIG 变体 | — |
| 27 | **HYD_PRESS_LOW** | 液压 < 2800 psi | UDP ❌ → DREF | `sim/cockpit2/hydraulics/indicators/hydraulic_pressure_psi[0]` | ✅ 订阅 |
| 28 | **HYD_QTY_LOW** | 液压油量 < 20% | UDP ❌ → DREF | `sim/cockpit2/hydraulics/indicators/hydraulic_fluid_ratio[0]` | ✅ 订阅 |
| 29 | **OIL_TEMP_HIGH** | 滑油温度 > 120°C | UDP G50 | Group 50 | `oil_temp_c[0..3]` (struct 已有) |
| 30 | **DOOR_OPEN** | 任意门未关锁 (飞行中) | DREF | `sim/cockpit/warnings/annunciators/door` | ✅ 直接读 |
| 31 | **ELEC_FAULT** | 发电机/电压异常 | DREF | `sim/cockpit/warnings/annunciators/generator` | ✅ 直接读 |
| 32 | **ANTI_ICE_FAULT** | 防冰系统故障 | DREF | `sim/cockpit/warnings/annunciators/anti_ice` | ✅ 直接读 |

---

## 三、火警告警 (新增 5 条)

| # | 告警名 | 触发条件 | 数据源 | X-Plane 数据路径 |
|---|--------|---------|--------|-----------------|
| 33 | **FIRE_ENG1** | 左发火警 | DREF | `sim/cockpit/warnings/annunciators/engine_fire` + `sim/operation/failures/rel_engfire0` |
| 34 | **FIRE_ENG2** | 右发火警 | DREF | 同上 `rel_engfire1` |
| 35 | **FIRE_APU** | APU 火警 | DREF | `sim/operation/failures/rel_fire_apu` |
| 36 | **FIRE_WHEEL_WELL** | 轮舱火警 | DREF | `sim/cockpit/warnings/annunciators/fire_warning` (泛) |
| 37 | **FIRE_CARGO** | 货舱火警 | DREF | `sim/cockpit/warnings/annunciators/cargo_door`? (不确定) |

> **注**: 火警告警在训练数据中只能通过 DREF 状态位直接读。FlightFrame 模拟器可通过随机注入故障来生成。

---

## 四、TCAS 告警 (新增 2 条)

| # | 告警名 | 触发条件 | 数据源 | X-Plane 数据路径 |
|---|--------|---------|--------|-----------------|
| 38 | **TCAS_TA** | Traffic Advisory | DREF | 从 `sim/cockpit2/tcas/indicators/` 数组推导 |
| 39 | **TCAS_RA** | Resolution Advisory | DREF | 同上 |

> **注**: TCAS 数据较复杂。X-Plane 不直接给 TA/RA 布尔值，需要从周边飞机的相对高度/距离推导。训练数据中可以简化为随机注入。

---

## 五、AP/AT 脱开告警 (新增 2 条)

| # | 告警名 | 触发条件 | 数据源 | X-Plane 数据路径 |
|---|--------|---------|--------|-----------------|
| 40 | **AP_DISENGAGE** | 自动驾驶脱开 | UDP G108 + DREF | G108 field[0] 边缘检测 (1→0) ；DREF: `sim/cockpit/warnings/annunciators/autopilot_disconnect` |
| 41 | **AT_DISENGAGE** | 自动油门脱开 | UDP G108 + DREF | G108 field[5] 边缘检测 (1→0) |

> **注**: AP/AT 脱开是**边缘事件**——不是在每个 AP_OFF 帧都告警，而是在 AP 从 ON 变 OFF 的那一帧触发。

---

## 六、汇总：训练数据需要输出的告警位 (41 条)

```
GPWS (13):   PULL_UP, WINDSHEAR, MASTER_WARNING, MASTER_CAUTION,
             TERRAIN, SINK_RATE, TOO_LOW_GEAR, TOO_LOW_FLAPS,
             GLIDESLOPE, BANK_ANGLE, OVERSPEED, STALL, MINIMUMS

系统 (11):   ENG_OVERHEAT, ENG_ASYM, FUEL_IMBALANCE, OIL_PRESS_LOW,
             CABIN_ALT_HIGH, BUS_VOLT_ABNORM, TAKEOFF_CONFIG, LOW_FUEL,
             ICING_CONDITION, APU_FIRE, OIL_TEMP_HIGH

火警 (5):    FIRE_ENG1, FIRE_ENG2, FIRE_APU, FIRE_WHEEL_WELL, FIRE_CARGO

构型 (2):    CONFIG_LANDING, CONFIG_TAKEOFF

液压 (2):    HYD_PRESS_LOW, HYD_QTY_LOW

电气 (1):    ELEC_FAULT

舱门 (1):    DOOR_OPEN

防冰 (1):    ANTI_ICE_FAULT

TCAS (2):    TCAS_TA, TCAS_RA

AP/AT (2):   AP_DISENGAGE, AT_DISENGAGE

其他 (1):    STALL_WARNING (独立于 STALL，来自 DREF 硬信号)
```

---

## 七、训练数据 format_user_input 目标格式

```
飞行阶段: 进近
高度: 1140ft MSL / 1140ft AGL
空速: 267 KIAS / 马赫 0.52
垂直速度: -1832 fpm
坡度: 38° | 俯仰: -2°
航向: 301°T / 298°M
N1: 50% / 50% | N2: 72% / 72%
EGT: 661°C / 658°C
滑油压力: 42 / 43 psi | 滑油温度: 85 / 87°C
燃油: 14372 lbs (左右差: 320 lbs) | 流量: 2400 pph
襟翼: 3% | 起落架: 未放下 | 减速板: 收上
液压: A=2950 B=3010 psi
总线电压: 28.0V | 发电机: 150 / 152 A
OAT: -12°C | TAT: +8°C
座舱高度: 2500ft | 压差: 6.2 psi
AP: 接通 | A/T: 接通
APU: 运转 | 防冰: OFF

告警状态:
  BANK_ANGLE          [ACTIVE]
  SINK_RATE           [ACTIVE]
  TOO_LOW_GEAR        [INACTIVE]
  TOO_LOW_FLAPS       [INACTIVE]
  GLIDESLOPE          [INACTIVE]
  TERRAIN             [INACTIVE]
  PULL_UP             [INACTIVE]
  WINDSHEAR           [INACTIVE]
  OVERSPEED           [INACTIVE]
  STALL               [INACTIVE]
  STALL_WARNING       [INACTIVE]
  MINIMUMS            [INACTIVE]
  ENG_OVERHEAT        [INACTIVE]
  ENG_ASYM            [INACTIVE]
  OIL_PRESS_LOW       [INACTIVE]
  OIL_TEMP_HIGH       [INACTIVE]
  CABIN_ALT_HIGH      [INACTIVE]
  BUS_VOLT_ABNORM     [INACTIVE]
  LOW_FUEL            [INACTIVE]
  FUEL_IMBALANCE      [INACTIVE]
  ICING_CONDITION     [INACTIVE]
  TAKEOFF_CONFIG      [INACTIVE]
  CONFIG_LANDING      [INACTIVE]
  APU_FIRE            [INACTIVE]
  FIRE_ENG1           [INACTIVE]
  FIRE_ENG2           [INACTIVE]
  FIRE_APU            [INACTIVE]
  FIRE_WHEEL_WELL     [INACTIVE]
  FIRE_CARGO          [INACTIVE]
  HYD_PRESS_LOW       [INACTIVE]
  HYD_QTY_LOW         [INACTIVE]
  ELEC_FAULT          [INACTIVE]
  DOOR_OPEN           [INACTIVE]
  ANTI_ICE_FAULT      [INACTIVE]
  TCAS_TA             [INACTIVE]
  TCAS_RA             [INACTIVE]
  AP_DISENGAGE        [INACTIVE]
  AT_DISENGAGE        [INACTIVE]
  MASTER_WARNING      [OFF]
  MASTER_CAUTION      [ON]
```

---

## 八、数据源优先级策略

```
Level 1 (UDP DATA Group — 已接收，零延迟):
  所有飞行参数: IAS, ALT, VS, roll, pitch, heading, N1, EGT, fuel, flap, gear, ...
  master_warning, master_caution
  ap_engaged, ap_athr_engaged
  cabin_alt_ft, elec_bus_volts, oat_c, anti_ice_wing, apu_*

Level 2 (DREF — 需新增 RREF 订阅，低频率 1-4 Hz 足够):
  告警灯硬信号: bank_angle, stall_warning, gear_warning, GPWS, overspeed, windshear
  火警: engine_fire, fire_warning, rel_engfire*, rel_fire_apu
  液压: hydraulic_pressure_psi[], hydraulic_fluid_ratio[]
  舱门: door, cargo_door, door_open_ratio[]
  电气: generator, voltage
  防冰: anti_ice
  TCAS: tcas/indicators/*

Level 3 (Derived — 纯计算):
  CONFIG_LANDING = TOO_LOW_GEAR ∪ TOO_LOW_FLAPS
  CONFIG_TAKEOFF = flap < takeoff_min + phase = TAKEOFF
  IAS 突变检测 → WINDSHEAR
  AP 边缘检测 → AP_DISENGAGE, AT_DISENGAGE
```

---

## 九、实施路线

| 阶段 | 内容 | 文件 |
|------|------|------|
| **Phase 1** | `format_user_input` 补全已有参数 (roll, pitch, cdi, cabin_alt, oil_press, bus_volts, oat 等) | `scripts/generate_training_data.py` |
| **Phase 2** | `alert_rules.json` 新增 ~18 条规则 (fire, tcas, hyd, door, elec, ap_disc 等) | `scripts/alert_rules.json` |
| **Phase 3** | `format_user_input` 新增告警状态块 (41 位全部输出) | `scripts/generate_training_data.py` |
| **Phase 4** | 重新生成训练数据 (33k+ 条，新格式) | `python scripts/generate_training_data.py` |
| **Phase 5** | C 层 `alert_system.c` 改造，暴露 `get_active_alerts()` 接口 | `src/audio/alert_system.c/.h` |
| **Phase 6** | C 层新增 RREF 订阅模块，读取 Level 2 DREF 告警位 | `src/net/xplane.c` (或新文件) |
| **Phase 7** | 推理时：C 层告警状态 + 飞行参数 → LoRA 模型 → 建议输出 | `src/app.c` |

---

## 附录 A：FlightDataValues 完整字段 ↔ format_user_input 对照

| FlightDataValues 字段 | 类型 | UDP 来源 | 当前在 input? | 应加入? |
|----------------------|------|---------|-------------|---------|
| `roll_deg` | float | G17[1] | ❌ | ✅ 必须 |
| `pitch_deg` | float | G17[0] | ❌ | ✅ 必须 |
| `heading_true_deg` | float | G17[2] | ☆ | ✅ (已有) |
| `heading_mag_deg` | float | G17[3] | ❌ | ✅ |
| `ias_kts` | float | G3[0] | ☆ | ✅ (已有) |
| `tas_kts` | float | G3[2] | ❌ | ○ 可选 |
| `gs_kts` | float | G3[3] | ❌ | ○ 可选 |
| `mach` | float | G4[0] | ❌ | ✅ 推荐 |
| `vs_fpm` | float | G4[2] | ☆ | ✅ (已有) |
| `altitude_ft` | float | G20[2] | ☆ | ✅ (已有) |
| `altitude_agl_ft` | float | G20[3] | ❌ | ✅ 必须 (已在同一行) |
| `n1_pct[0..1]` | float[4] | G41 | ☆ | ✅ (已有) |
| `n2_pct[0..1]` | float[4] | G42 | ❌ | ○ 可选 |
| `egt_c[0..1]` | float[4] | G47 | ☆ | ✅ (已有) |
| `oil_press_psi[0..1]` | float[4] | G49 | ❌ | ✅ 必须 |
| `oil_temp_c[0..1]` | float[4] | G50 | ❌ | ✅ 必须 |
| `fuel_total_lbs` | float | G62 | ☆ | ✅ (已有) |
| `fuel_flow_pph[0..1]` | float[4] | G45 | ❌ | ○ 可选 |
| `flap_ratio` | float | G13[0] | ☆ | ✅ (已有) |
| `speedbrake_ratio` | float | G13[3] | ❌ | ✅ 推荐 |
| `gear_deployed` | int | G14[0]>0.9 | ☆ | ✅ (已有) |
| `elevator_deg` | float | G8[0] | ❌ | ○ 可选 |
| `aileron_deg` | float | G8[1] | ❌ | ○ 可选 |
| `rudder_deg` | float | G8[2] | ❌ | ○ 可选 |
| `nav1_cdi` | float | G99[0] | ❌ | ✅ 必须 (GLIDESLOPE) |
| `ap_engaged` | int | G108[0] | ☆ | ✅ (已有) |
| `ap_athr_engaged` | int | G108[5] | ❌ | ✅ 必须 |
| `cabin_alt_ft` | float | G120[0] | ❌ | ✅ 必须 |
| `cabin_diff_psi` | float | G120[2] | ❌ | ○ 可选 |
| `elec_bus_volts` | float | G121[5] | ❌ | ✅ 必须 |
| `elec_gen_amps[0..1]` | float[2] | G121[3],[4] | ❌ | ✅ 推荐 |
| `oat_c` | float | G5[3] | ❌ | ✅ 必须 |
| `tat_c` | float | Derived | ❌ | ○ 可选 |
| `wind_speed_kts` | float | G5[1] | ❌ | ○ 可选 |
| `wind_dir_deg` | float | G5[2] | ❌ | ○ 可选 |
| `anti_ice_wing` | int | G109[0] | ❌ | ✅ 必须 (ICING_CONDITION) |
| `apu_n1_pct` | float | G121[0] | ❌ | ○ 可选 |
| `apu_egt_c` | float | G121[1] | ❌ | ✅ 必须 (APU_FIRE) |
| `apu_running` | int | G121[2] | ❌ | ✅ 必须 (APU_FIRE) |
| `master_warning` | int | G113[0] | ❌ | ✅ 必须 |
| `master_caution` | int | G113[1] | ❌ | ✅ 必须 |
| `hyd_press_psi[0..1]` | float[2] | ❌ UDP | ❌ | ✅ 需 DREF |
| `hyd_qty_pct[0..1]` | float[2] | ❌ UDP | ❌ | ✅ 需 DREF |
| `brake_temp_c[0..1]` | float[2] | G14[2],[3] | ❌ | ○ 可选 |

> ✅ 必须: 告警检测直接依赖  
> ✅ 推荐: 模型建议有用  
> ○ 可选: 锦上添花但非关键  
> ☆ 已有: 当前 `format_user_input` 已包含（但格式可能需调整）

---

## 附录 B：需要新增的 DREF RREF 订阅清单

```
# === 告警状态位 (Level 2, 1-4 Hz 轮询) ===
sim/cockpit/warnings/annunciators/bank_angle
sim/cockpit/warnings/annunciators/stall_warning
sim/cockpit/warnings/annunciators/gear_warning
sim/cockpit/warnings/annunciators/GPWS
sim/cockpit/warnings/annunciators/overspeed
sim/cockpit/warnings/annunciators/windshear
sim/cockpit/warnings/annunciators/master_warning
sim/cockpit/warnings/annunciators/master_caution
sim/cockpit/warnings/annunciators/autopilot_disconnect
sim/cockpit/warnings/annunciators/engine_fire
sim/cockpit/warnings/annunciators/fire_warning
sim/cockpit/warnings/annunciators/door
sim/cockpit/warnings/annunciators/generator
sim/cockpit/warnings/annunciators/anti_ice
sim/cockpit/warnings/annunciators/hydraulic_pressure
sim/cockpit/warnings/annunciators/hydraulic_quantity
sim/cockpit/warnings/annunciators/cabin_altitude
sim/cockpit/warnings/annunciators/fuel_quantity
sim/cockpit/warnings/annunciators/oil_pressure
sim/cockpit/warnings/annunciators/oil_temperature
sim/cockpit/warnings/annunciators/voltage
sim/cockpit/warnings/annunciators/pressurization
sim/cockpit/warnings/annunciators/ice

# === 液压/电气数值 (Level 2, 1-4 Hz) ===
sim/cockpit2/hydraulics/indicators/hydraulic_pressure_psi[0]
sim/cockpit2/hydraulics/indicators/hydraulic_pressure_psi[1]
sim/cockpit2/hydraulics/indicators/hydraulic_fluid_ratio[0]
sim/cockpit2/hydraulics/indicators/hydraulic_fluid_ratio[1]

# === 舱门状态 (Level 2, 1 Hz) ===
sim/flightmodel2/misc/door_open_ratio[0]   # 主登机门
sim/flightmodel2/misc/door_open_ratio[1]   # 前货舱
sim/flightmodel2/misc/door_open_ratio[2]   # 后货舱

# === 火警故障位 (Level 2, 1 Hz) ===
sim/operation/failures/rel_engfire0
sim/operation/failures/rel_engfire1
sim/operation/failures/rel_fire_apu

# === TCAS (Level 2, 2-4 Hz) ===
sim/cockpit2/tcas/indicators/relative_altitude_meters[0..19]
sim/cockpit2/tcas/indicators/relative_distance_meters[0..19]
sim/cockpit2/tcas/indicators/relative_bearing_degs[0..19]
```

---

## 附录 C：X-Plane 12 UDP DATA Group 完整订阅列表 (当前 33 组)

```
3, 4, 5, 8, 13, 14, 17, 20, 25,           ← flight + controls
34, 37, 41, 42, 43, 44, 45, 46, 47, 48,   ← engine
49, 50, 51,                                 ← oil/fuel press
62,                                          ← fuel weights
96, 97, 99, 102, 104,                       ← nav/com/dme/xpdr
108, 118,                                    ← autopilot
109, 113, 120, 121, 67                      ← systems
```
