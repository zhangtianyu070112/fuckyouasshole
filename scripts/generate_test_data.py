"""
Test data generator for LoRA AI Copilot model evaluation.

Generates a compact but comprehensive test set (~100 samples) covering:
  - All 13 GPWS alert types (single and realistic combos)
  - All 13 system deviation types
  - All 13 DREF-only alert types
  - All 8 flight phases (normal reports)
  - 10 combined multi-alert scenarios
  - 5 edge/corner cases
  - 5 normal cruise reports

Output: scripts/data/test_data.jsonl (ShareGPT conversation v2 format)

Usage:
  python scripts/generate_test_data.py
"""

import json
import os
import random
import sys

random.seed(20260709)  # Reproducible test set

# =============================================================================
# System prompt (identical to training)
# =============================================================================
SYSTEM_PROMPT = (
    "你是B737-800驾驶舱AI副驾驶。监控飞行参数，用中文提供简洁的操作建议或态势说明。"
    "输出不超过50字。一切正常时报告「正常」，异常时指出具体问题和建议操作。"
    "你提供的建议仅供参考，不可替代SOP和GPWS硬告警。"
)

# =============================================================================
# Alert list in canonical order (39 items + master_warning + master_caution)
# =============================================================================
ALERT_LIST = [
    "PULL_UP", "WINDSHEAR", "MASTER_WARNING", "MASTER_CAUTION",
    "TERRAIN", "SINK_RATE", "TOO_LOW_GEAR", "TOO_LOW_FLAPS",
    "GLIDESLOPE", "BANK_ANGLE", "OVERSPEED", "STALL", "MINIMUMS",
    "ENG_OVERHEAT", "ENG_ASYM", "FUEL_IMBALANCE", "OIL_PRESS_LOW",
    "CABIN_ALT_HIGH", "BUS_VOLT_ABNORM", "TAKEOFF_CONFIG", "LOW_FUEL",
    "ICING_CONDITION", "APU_FIRE", "OIL_TEMP_HIGH", "HYD_PRESS_LOW",
    "HYD_QTY_LOW", "FIRE_ENG1", "FIRE_ENG2", "FIRE_APU",
    "FIRE_WHEEL_WELL", "FIRE_CARGO", "TCAS_TA", "TCAS_RA",
    "STALL_WARNING", "DOOR_OPEN", "ELEC_FAULT", "ANTI_ICE_FAULT",
    "AP_DISENGAGE", "AT_DISENGAGE",
]

# =============================================================================
# Flight phase Chinese labels
# =============================================================================
PHASE_NAMES = {
    "TAKEOFF": "起飞",
    "CLIMB1": "初始爬升",
    "CLIMB2": "爬升至巡航",
    "CRUISE": "巡航",
    "DESCENT": "下降",
    "APPROACH": "进近",
    "LANDING": "着陆",
    "TAXI": "滑行",
}


def build_alert_block(active_alerts, master_warning=False, master_caution=False):
    """Build the 告警状态 block with given active alerts."""
    lines = ["告警状态:"]
    active_set = set(active_alerts)

    for alert in ALERT_LIST:
        if alert in active_set:
            lines.append(f"  {alert:<25} ⚠ ACTIVE")
        else:
            lines.append(f"  {alert:<25} inactive")

    lines.append(f"  {'master_warning':<25} {'⚠ ON ' if master_warning else 'OFF'}")
    lines.append(f"  {'master_caution':<25} {'⚠ ON ' if master_caution else 'OFF'}")
    return "\n".join(lines)


def build_user_input(phase, alt_msl, alt_agl, ias, mach, gs, vs,
                     roll, pitch, hdg_t, hdg_m,
                     n1_l, n1_r, n2_l, n2_r, egt_l, egt_r,
                     oil_press_l, oil_press_r, oil_temp_l, oil_temp_r,
                     fuel_total, fuel_flow, fuel_imbal,
                     flap_pct, gear_down, spdbrk_deployed,
                     hyd_a, hyd_b, bus_v,
                     cabin_alt, cabin_diff,
                     oat, wind_str, wind_dir,
                     ap, at, apu, anti_ice,
                     active_alerts, master_warning=False, master_caution=False):
    """Build the full v2 user input string."""
    phase_cn = PHASE_NAMES.get(phase, phase)
    gear_str = "放下" if gear_down else "收上"
    spdbrk_str = "放出" if spdbrk_deployed else "收上"
    ap_str = "接通" if ap else "断开"
    at_str = "接通" if at else "断开"
    apu_str = "运转" if apu else "关闭"
    anti_ice_str = "开" if anti_ice else "关"

    lines = [
        f"飞行阶段: {phase_cn}",
        f"高度: {alt_msl}ft MSL / {alt_agl}ft AGL",
        f"空速: {ias} KIAS / M{mach:.2f} / GS {gs}kt",
        f"垂直速度: {vs:+d} fpm",
        f"姿态: 坡度 {roll:+.1f}° / 俯仰 {pitch:+.1f}°",
        f"航向: {hdg_t}°T / {hdg_m}°M",
        f"发动机: N1 {n1_l}%/{n1_r}%  N2 {n2_l}%/{n2_r}%  EGT {egt_l}°C/{egt_r}°C",
        f"滑油: 压力 {oil_press_l}/{oil_press_r} psi  温度 {oil_temp_l}/{oil_temp_r}°C",
        f"燃油: 总量 {fuel_total} lbs  流量 {fuel_flow} pph  左右差 {fuel_imbal} lbs",
        f"襟翼: {flap_pct}%  |  起落架: {gear_str}  |  减速板: {spdbrk_str}",
        f"液压: A={hyd_a} psi  B={hyd_b} psi",
        f"电源: 总线 {bus_v:.1f}V",
        f"座舱: 高度 {cabin_alt}ft  /  压差 {cabin_diff:.1f} psi",
        f"环境: OAT {oat:+d}°C  /  风 {wind_str}kt@{wind_dir}°",
        f"AP: {ap_str}  |  A/T: {at_str}  |  APU: {apu_str}  |  防冰: {anti_ice_str}",
        "",
        build_alert_block(active_alerts, master_warning, master_caution),
    ]
    return "\n".join(lines)


def make_sample(phase, params, active_alerts, assistant_text,
                category, alert_type=None, system_rule=None,
                dref_alert=None, combined_alerts=None,
                master_warning=False, master_caution=False):
    """Build a complete ShareGPT conversation sample."""
    user_input = build_user_input(
        phase=phase,
        alt_msl=params.get("alt_msl", 0),
        alt_agl=params.get("alt_agl", 0),
        ias=params.get("ias", 250),
        mach=params.get("mach", 0.78),
        gs=params.get("gs", 450),
        vs=params.get("vs", 0),
        roll=params.get("roll", 0.0),
        pitch=params.get("pitch", 2.5),
        hdg_t=params.get("hdg_t", 90),
        hdg_m=params.get("hdg_m", 85),
        n1_l=params.get("n1_l", 60),
        n1_r=params.get("n1_r", 60),
        n2_l=params.get("n2_l", 55),
        n2_r=params.get("n2_r", 55),
        egt_l=params.get("egt_l", 710),
        egt_r=params.get("egt_r", 710),
        oil_press_l=params.get("oil_press_l", 52),
        oil_press_r=params.get("oil_press_r", 52),
        oil_temp_l=params.get("oil_temp_l", 96),
        oil_temp_r=params.get("oil_temp_r", 96),
        fuel_total=params.get("fuel_total", 15000),
        fuel_flow=params.get("fuel_flow", 6000),
        fuel_imbal=params.get("fuel_imbal", 80),
        flap_pct=params.get("flap_pct", 0),
        gear_down=params.get("gear_down", False),
        spdbrk_deployed=params.get("spdbrk_deployed", False),
        hyd_a=params.get("hyd_a", 3000),
        hyd_b=params.get("hyd_b", 3000),
        bus_v=params.get("bus_v", 28.0),
        cabin_alt=params.get("cabin_alt", 6500),
        cabin_diff=params.get("cabin_diff", 8.3),
        oat=params.get("oat", -50),
        wind_str=params.get("wind_str", 15),
        wind_dir=params.get("wind_dir", 270),
        ap=params.get("ap", True),
        at=params.get("at", True),
        apu=params.get("apu", False),
        anti_ice=params.get("anti_ice", True),
        active_alerts=active_alerts,
        master_warning=master_warning,
        master_caution=master_caution,
    )

    sample = {
        "conversations": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": user_input},
            {"role": "assistant", "content": assistant_text},
        ],
        "category": category,
    }
    if alert_type:
        sample["alert_type"] = alert_type
    if system_rule:
        sample["system_rule"] = system_rule
    if dref_alert:
        sample["dref_alert"] = dref_alert
    if combined_alerts:
        sample["combined_alerts"] = combined_alerts

    return sample


# =============================================================================
# Parameter templates for each flight phase
# =============================================================================
PHASE_PARAMS = {
    "TAKEOFF": dict(alt_msl=50, alt_agl=50, ias=140, mach=0.32, gs=145, vs=+2000,
                    roll=3.0, pitch=12.0, hdg_t=180, hdg_m=175,
                    n1_l=98, n1_r=98, n2_l=90, n2_r=90, egt_l=900, egt_r=905,
                    oil_press_l=60, oil_press_r=60, oil_temp_l=119, oil_temp_r=119,
                    fuel_total=15500, fuel_flow=9000, fuel_imbal=50,
                    flap_pct=25, gear_down=True, spdbrk_deployed=False,
                    hyd_a=3020, hyd_b=3010, bus_v=28.4, cabin_alt=20, cabin_diff=0.0,
                    oat=15, wind_str=15, wind_dir=270,
                    ap=False, at=False, apu=False, anti_ice=False),
    "CLIMB1": dict(alt_msl=3000, alt_agl=3000, ias=210, mach=0.42, gs=250, vs=+2400,
                   roll=2.0, pitch=14.0, hdg_t=200, hdg_m=195,
                   n1_l=94, n1_r=94, n2_l=87, n2_r=87, egt_l=880, egt_r=885,
                   oil_press_l=58, oil_press_r=58, oil_temp_l=115, oil_temp_r=116,
                   fuel_total=15000, fuel_flow=8600, fuel_imbal=70,
                   flap_pct=10, gear_down=False, spdbrk_deployed=False,
                   hyd_a=3025, hyd_b=3015, bus_v=28.3, cabin_alt=2000, cabin_diff=3.0,
                   oat=5, wind_str=15, wind_dir=270,
                   ap=False, at=False, apu=False, anti_ice=False),
    "CLIMB2": dict(alt_msl=25000, alt_agl=25000, ias=270, mach=0.70, gs=400, vs=+1800,
                   roll=-2.0, pitch=8.0, hdg_t=250, hdg_m=245,
                   n1_l=75, n1_r=75, n2_l=69, n2_r=69, egt_l=780, egt_r=785,
                   oil_press_l=55, oil_press_r=55, oil_temp_l=105, oil_temp_r=105,
                   fuel_total=14500, fuel_flow=7200, fuel_imbal=90,
                   flap_pct=0, gear_down=False, spdbrk_deployed=False,
                   hyd_a=3010, hyd_b=3000, bus_v=28.0, cabin_alt=6000, cabin_diff=8.0,
                   oat=-30, wind_str=15, wind_dir=270,
                   ap=True, at=True, apu=False, anti_ice=True),
    "CRUISE": dict(alt_msl=35000, alt_agl=35000, ias=280, mach=0.80, gs=480, vs=0,
                   roll=-2.0, pitch=2.5, hdg_t=90, hdg_m=85,
                   n1_l=60, n1_r=60, n2_l=55, n2_r=55, egt_l=710, egt_r=712,
                   oil_press_l=52, oil_press_r=52, oil_temp_l=96, oil_temp_r=96,
                   fuel_total=15000, fuel_flow=6000, fuel_imbal=80,
                   flap_pct=0, gear_down=False, spdbrk_deployed=False,
                   hyd_a=3000, hyd_b=3000, bus_v=27.8, cabin_alt=6500, cabin_diff=8.3,
                   oat=-50, wind_str=15, wind_dir=270,
                   ap=True, at=True, apu=False, anti_ice=True),
    "DESCENT": dict(alt_msl=20000, alt_agl=20000, ias=290, mach=0.78, gs=460, vs=-1800,
                    roll=-3.0, pitch=3.0, hdg_t=120, hdg_m=115,
                    n1_l=48, n1_r=48, n2_l=44, n2_r=44, egt_l=650, egt_r=655,
                    oil_press_l=50, oil_press_r=50, oil_temp_l=90, oil_temp_r=90,
                    fuel_total=14000, fuel_flow=5000, fuel_imbal=100,
                    flap_pct=0, gear_down=False, spdbrk_deployed=True,
                    hyd_a=3010, hyd_b=3005, bus_v=28.1, cabin_alt=6000, cabin_diff=7.8,
                    oat=-40, wind_str=15, wind_dir=270,
                    ap=True, at=True, apu=False, anti_ice=True),
    "APPROACH": dict(alt_msl=5000, alt_agl=5000, ias=180, mach=0.40, gs=200, vs=-800,
                     roll=1.0, pitch=2.0, hdg_t=300, hdg_m=295,
                     n1_l=55, n1_r=55, n2_l=50, n2_r=50, egt_l=680, egt_r=685,
                     oil_press_l=51, oil_press_r=51, oil_temp_l=92, oil_temp_r=92,
                     fuel_total=13000, fuel_flow=5500, fuel_imbal=100,
                     flap_pct=30, gear_down=False, spdbrk_deployed=False,
                     hyd_a=3010, hyd_b=3005, bus_v=28.2, cabin_alt=2000, cabin_diff=3.0,
                     oat=5, wind_str=15, wind_dir=270,
                     ap=True, at=True, apu=False, anti_ice=False),
    "LANDING": dict(alt_msl=200, alt_agl=200, ias=140, mach=0.33, gs=145, vs=-700,
                    roll=0.5, pitch=2.5, hdg_t=350, hdg_m=345,
                    n1_l=45, n1_r=45, n2_l=41, n2_r=41, egt_l=630, egt_r=635,
                    oil_press_l=49, oil_press_r=49, oil_temp_l=87, oil_temp_r=87,
                    fuel_total=12500, fuel_flow=4800, fuel_imbal=90,
                    flap_pct=80, gear_down=True, spdbrk_deployed=True,
                    hyd_a=3000, hyd_b=3000, bus_v=27.8, cabin_alt=100, cabin_diff=0.2,
                    oat=5, wind_str=10, wind_dir=260,
                    ap=False, at=False, apu=False, anti_ice=False),
    "TAXI": dict(alt_msl=0, alt_agl=0, ias=20, mach=0.20, gs=15, vs=0,
                 roll=0.0, pitch=0.0, hdg_t=45, hdg_m=40,
                 n1_l=22, n1_r=22, n2_l=20, n2_r=20, egt_l=500, egt_r=505,
                 oil_press_l=45, oil_press_r=45, oil_temp_l=75, oil_temp_r=75,
                 fuel_total=12000, fuel_flow=2500, fuel_imbal=50,
                 flap_pct=0, gear_down=True, spdbrk_deployed=False,
                 hyd_a=2990, hyd_b=2990, bus_v=27.5, cabin_alt=0, cabin_diff=0.0,
                 oat=20, wind_str=5, wind_dir=180,
                 ap=False, at=False, apu=True, anti_ice=False),
}


def vary_params(base, **overrides):
    """Return a copy of base params with specified overrides."""
    p = dict(base)
    p.update(overrides)
    return p


# =============================================================================
# Test case definitions
# =============================================================================
def generate_all_samples():
    samples = []

    # ------------------------------------------------------------------
    # A_GPWS — 13 GPWS alert types (one realistic scenario each)
    # ------------------------------------------------------------------
    gpws_cases = [
        # PULL_UP: very low AGL, high sink rate
        ("PULL_UP", "APPROACH", vary_params(PHASE_PARAMS["APPROACH"],
            alt_msl=200, alt_agl=80, ias=160, vs=-2800, flap_pct=30, gear_down=False),
         ["PULL_UP", "SINK_RATE"],
         "PULL UP! 80英尺！下降率2800英尺/分钟！最大推力复飞！", {}),
        # WINDSHEAR: large airspeed change at low altitude
        ("WINDSHEAR", "TAKEOFF", vary_params(PHASE_PARAMS["TAKEOFF"],
            alt_msl=300, alt_agl=300, ias=110, vs=+500),
         ["WINDSHEAR"],
         "风切变！空速突变30节！300英尺！设置TOGA推力！保持俯仰姿态！", {}),
        # MASTER_WARNING
        ("MASTER_WARNING", "CLIMB2", vary_params(PHASE_PARAMS["CLIMB2"],
            egt_l=910, egt_r=915),
         ["MASTER_WARNING", "ENG_OVERHEAT"],
         "主警告！检查EICAS告警信息！立即确认并执行相应检查单！",
         {"master_warning": True}),
        # MASTER_CAUTION
        ("MASTER_CAUTION", "CRUISE", vary_params(PHASE_PARAMS["CRUISE"],
            fuel_imbal=1500),
         ["MASTER_CAUTION", "FUEL_IMBALANCE"],
         "主警戒！检查系统异常指示，确认并评估是否需要机组响应！",
         {"master_caution": True}),
        # TERRAIN: moderate AGL with high sink rate
        ("TERRAIN", "DESCENT", vary_params(PHASE_PARAMS["DESCENT"],
            alt_msl=3000, alt_agl=800, vs=-2000),
         ["TERRAIN", "SINK_RATE"],
         "地形警告！800英尺！下降率2000英尺/分钟！拉起！"),
        # SINK_RATE
        ("SINK_RATE", "APPROACH", vary_params(PHASE_PARAMS["APPROACH"],
            alt_msl=3000, alt_agl=2000, vs=-1900),
         ["SINK_RATE"],
         "注意：下降率1900英尺/分钟过大！当前高度2000英尺，减小下降率至1000以下！"),
        # TOO_LOW_GEAR
        ("TOO_LOW_GEAR", "LANDING", vary_params(PHASE_PARAMS["LANDING"],
            alt_msl=400, alt_agl=400, gear_down=False, flap_pct=75),
         ["TOO_LOW_GEAR"],
         "起落架未放下！高度低于500英尺！立即放下起落架！"),
        # TOO_LOW_FLAPS
        ("TOO_LOW_FLAPS", "APPROACH", vary_params(PHASE_PARAMS["APPROACH"],
            alt_msl=400, alt_agl=400, flap_pct=10, gear_down=True, vs=-50),
         ["TOO_LOW_FLAPS"],
         "襟翼未放置着陆构型！高度400英尺！当前襟翼10%，需至少25%！"),
        # GLIDESLOPE
        ("GLIDESLOPE", "APPROACH", vary_params(PHASE_PARAMS["APPROACH"],
            alt_msl=800, alt_agl=800, flap_pct=50, gear_down=True, vs=-700),
         ["GLIDESLOPE"],
         "下滑道偏离！CDI偏差0.8个点，800英尺，修正下滑道！"),
        # BANK_ANGLE
        ("BANK_ANGLE", "CLIMB1", vary_params(PHASE_PARAMS["CLIMB1"],
            alt_msl=2000, alt_agl=2000, roll=42.0),
         ["BANK_ANGLE"],
         "坡度警告！当前坡度42度超过35度限制，立即改平坡度！"),
        # OVERSPEED
        ("OVERSPEED", "CRUISE", vary_params(PHASE_PARAMS["CRUISE"],
            ias=355, mach=0.87, gs=510),
         ["OVERSPEED"],
         "超速警告！空速355节超过340节限制！收油门减速！"),
        # STALL
        ("STALL", "TAKEOFF", vary_params(PHASE_PARAMS["TAKEOFF"],
            alt_msl=1500, alt_agl=1500, ias=100, vs=-200, pitch=18.0),
         ["STALL"],
         "失速警告！空速100节低于110节，推杆减小迎角，增加推力！"),
        # MINIMUMS
        ("MINIMUMS", "APPROACH", vary_params(PHASE_PARAMS["APPROACH"],
            alt_msl=400, alt_agl=200, flap_pct=70, gear_down=True, vs=-600),
         ["MINIMUMS"],
         "决断高度！200英尺！"),
    ]
    for alert_type, phase, params, active, text, *extra in gpws_cases:
        kw = {"master_warning": False, "master_caution": False}
        if extra:
            kw.update(extra[0] if isinstance(extra[0], dict) else {})
        samples.append(make_sample(phase, params, active, text,
                                   category="A_GPWS", alert_type=alert_type, **kw))

    # ------------------------------------------------------------------
    # C_SYSTEM — 13 system deviation types
    # ------------------------------------------------------------------
    system_cases = [
        ("ENG_OVERHEAT", "TAKEOFF", vary_params(PHASE_PARAMS["TAKEOFF"],
            egt_l=870, egt_r=875),
         ["ENG_OVERHEAT"],
         "发动机超温！EGT 870/875°C超限！减小推力并监控EGT！"),
        ("ENG_ASYM", "CLIMB2", vary_params(PHASE_PARAMS["CLIMB2"],
            n1_l=80, n1_r=72),
         ["ENG_ASYM"],
         "发动机推力不对称！N1偏差8%！左右N1分别为80%和72%，检查油门！"),
        ("FUEL_IMBALANCE", "CRUISE", vary_params(PHASE_PARAMS["CRUISE"],
            fuel_imbal=1800),
         ["FUEL_IMBALANCE"],
         "注意：燃油不平衡！左右差1800磅！检查交输供油，平衡左右油箱！"),
        ("OIL_PRESS_LOW", "CRUISE", vary_params(PHASE_PARAMS["CRUISE"],
            oil_press_l=22, oil_press_r=52),
         ["OIL_PRESS_LOW"],
         "滑油压力低！左发22psi低于25psi限制！检查发动机状态！"),
        ("CABIN_ALT_HIGH", "CRUISE", vary_params(PHASE_PARAMS["CRUISE"],
            cabin_alt=12000, cabin_diff=3.0),
         ["CABIN_ALT_HIGH"],
         "座舱高度过高！12000英尺！检查增压系统，必要时紧急下降！"),
        ("BUS_VOLT_ABNORM", "DESCENT", vary_params(PHASE_PARAMS["DESCENT"],
            bus_v=22.5),
         ["BUS_VOLT_ABNORM"],
         "电源异常！总线电压22.5V低于正常范围！核实发电机电门！"),
        ("TAKEOFF_CONFIG", "TAKEOFF", vary_params(PHASE_PARAMS["TAKEOFF"],
            flap_pct=5, gear_down=True),
         ["TAKEOFF_CONFIG"],
         "起飞构型警告！襟翼仅5%不满足起飞要求！检查襟翼设置！"),
        ("LOW_FUEL", "DESCENT", vary_params(PHASE_PARAMS["DESCENT"],
            fuel_total=2500),
         ["LOW_FUEL"],
         "低燃油警告！仅剩2500磅燃油！联系ATC申请优先着陆！"),
        ("ICING_CONDITION", "APPROACH", vary_params(PHASE_PARAMS["APPROACH"],
            oat=-2, anti_ice=False),
         ["ICING_CONDITION"],
         "结冰条件！OAT -2°C且防冰未开！立即接通发动机和机翼防冰！"),
        ("APU_FIRE", "TAXI", vary_params(PHASE_PARAMS["TAXI"],
            apu=True),
         ["APU_FIRE"],
         "APU火警！立即执行APU火警检查单：APU关断→灭火手柄拉出！"),
        ("OIL_TEMP_HIGH", "CLIMB2", vary_params(PHASE_PARAMS["CLIMB2"],
            oil_temp_l=128, oil_temp_r=115),
         ["OIL_TEMP_HIGH"],
         "滑油超温警告！左发128°C超限！核实滑油量和滑油压力，减速降温！"),
        ("HYD_PRESS_LOW", "APPROACH", vary_params(PHASE_PARAMS["APPROACH"],
            hyd_a=1800, hyd_b=2950),
         ["HYD_PRESS_LOW"],
         "液压A系统压力低！1800psi！检查液压泵和液压油量！"),
        ("HYD_QTY_LOW", "DESCENT", vary_params(PHASE_PARAMS["DESCENT"],
            ),  # Params are normal but alert is active via DREF
         ["HYD_QTY_LOW"],
         "液压油量低！检查液压系统泄漏！可能影响操纵！"),
    ]
    for rule, phase, params, active, text in system_cases:
        samples.append(make_sample(phase, params, active, text,
                                   category="C_SYSTEM", system_rule=rule))

    # ------------------------------------------------------------------
    # F_DREF — 13 DREF-only alert types (fire, faults, TCAS, etc.)
    # ------------------------------------------------------------------
    dref_cases = [
        ("FIRE_ENG1", "CRUISE", vary_params(PHASE_PARAMS["CRUISE"],
            egt_l=950, egt_r=710, oil_press_l=15, oil_press_r=52),
         ["FIRE_ENG1", "ENG_OVERHEAT"],
         "左发火警！立即执行记忆项目：油门慢车→燃油切断→灭火手柄拉出一发！"),
        ("FIRE_ENG2", "CLIMB1", vary_params(PHASE_PARAMS["CLIMB1"],
            egt_r=960, egt_l=880, oil_press_r=12, oil_press_l=58),
         ["FIRE_ENG2", "ENG_OVERHEAT"],
         "右发火警探测！立即执行记忆项目：油门慢车→燃油切断→灭火手柄拉出二发！"),
        ("FIRE_APU", "TAXI", vary_params(PHASE_PARAMS["TAXI"],
            apu=True),
         ["FIRE_APU"],
         "APU火警！立即执行APU火警检查单：APU关断→灭火手柄拉出！"),
        ("FIRE_WHEEL_WELL", "LANDING", vary_params(PHASE_PARAMS["LANDING"],
            alt_msl=50, alt_agl=50),
         ["FIRE_WHEEL_WELL"],
         "轮舱火警！检查刹车温度，放下起落架风冷，宣布紧急状态！"),
        ("FIRE_CARGO", "CRUISE", vary_params(PHASE_PARAMS["CRUISE"],
            ),
         ["FIRE_CARGO"],
         "货舱火警！释放灭火瓶，宣布紧急状态，尽快备降！"),
        ("TCAS_TA", "DESCENT", vary_params(PHASE_PARAMS["DESCENT"],
            alt_msl=15000, alt_agl=15000),
         ["TCAS_TA"],
         "TCAS交通咨询！注意观察冲突飞机，准备机动避让！"),
        ("TCAS_RA", "DESCENT", vary_params(PHASE_PARAMS["DESCENT"],
            alt_msl=15000, alt_agl=15000, vs=-1500),
         ["TCAS_RA"],
         "TCAS决断咨询！立即执行RA指令！VS调整至指示绿色区域！"),
        ("STALL_WARNING", "APPROACH", vary_params(PHASE_PARAMS["APPROACH"],
            alt_msl=2000, alt_agl=2000, ias=105, pitch=16.0),
         ["STALL_WARNING"],
         "抖杆警告！接近失速迎角！推杆减小迎角！增加推力！"),
        ("DOOR_OPEN", "CLIMB1", vary_params(PHASE_PARAMS["CLIMB1"],
            cabin_diff=1.0, cabin_alt=5000),
         ["DOOR_OPEN"],
         "舱门警告灯亮！核实所有舱门关闭锁好！可能影响座舱增压！"),
        ("ELEC_FAULT", "CRUISE", vary_params(PHASE_PARAMS["CRUISE"],
            bus_v=24.0),
         ["ELEC_FAULT"],
         "电源系统异常！核实发电机开关、汇流条连接和电瓶电压！"),
        ("ANTI_ICE_FAULT", "CLIMB2", vary_params(PHASE_PARAMS["CLIMB2"],
            oat=-35, anti_ice=True),
         ["ANTI_ICE_FAULT"],
         "防冰系统故障！OAT -35°C有结冰风险！监控发动机参数和机翼状况！"),
        ("AP_DISENGAGE", "CRUISE", vary_params(PHASE_PARAMS["CRUISE"],
            ap=False),
         ["AP_DISENGAGE"],
         "自动驾驶断开！注意人工操纵！检查AP断开原因后重新接通！"),
        ("AT_DISENGAGE", "APPROACH", vary_params(PHASE_PARAMS["APPROACH"],
            at=False),
         ["AT_DISENGAGE"],
         "自动油门断开！人工控制推力！监控空速和N1！"),
    ]
    for alert, phase, params, active, text in dref_cases:
        samples.append(make_sample(phase, params, active, text,
                                   category="F_DREF", dref_alert=alert))

    # ------------------------------------------------------------------
    # B_PHASE — 8 normal flight phase summaries
    # ------------------------------------------------------------------
    phase_cases = [
        ("TAKEOFF", "起飞推力设定：N1 98%，空速140节增速正常，正上升率2000fpm。"),
        ("CLIMB1", "初始爬升中：3000英尺，IAS 210节加速至250节，N1 94%，收襟翼中。"),
        ("CLIMB2", "高空爬升：25000英尺，270节，上升率1800fpm，预计FL350巡航。"),
        ("CRUISE", "巡航中：FL350，马赫0.80，航向090°，双发正常，燃油15000磅，一切正常。"),
        ("DESCENT", "巡航下降：20000英尺，290节，1800fpm下降，预计进近，双发正常。"),
        ("APPROACH", "五边进近：5000英尺AGL，空速180节，襟翼30%，下降率800fpm，稳定进近中。"),
        ("LANDING", "着陆构型：襟翼80%，起落架放下，减速板放出，200英尺继续进近。"),
        ("TAXI", "滑行阶段：地速15节，APU运转，空速20节，一切正常。"),
    ]
    for phase, text in phase_cases:
        params = dict(PHASE_PARAMS[phase])
        samples.append(make_sample(phase, params, [], text,
                                   category="B_PHASE", alert_type=phase))

    # ------------------------------------------------------------------
    # D_COMBINED — 10 multi-alert overlapping scenarios
    # ------------------------------------------------------------------
    combined_cases = [
        # TOO_LOW_GEAR + TOO_LOW_FLAPS (classic approach config error)
        ("APPROACH", vary_params(PHASE_PARAMS["APPROACH"],
            alt_msl=300, alt_agl=300, flap_pct=8, gear_down=False, vs=-50),
         ["TOO_LOW_GEAR", "TOO_LOW_FLAPS", "SINK_RATE"],
         "双重构型警告：300英尺，起落架未放+襟翼仅8%！立即复飞或修正构型！",
         ["TOO_LOW_GEAR", "TOO_LOW_FLAPS"]),
        # TERRAIN + SINK_RATE (dangerous descent)
        ("DESCENT", vary_params(PHASE_PARAMS["DESCENT"],
            alt_msl=1500, alt_agl=600, vs=-2300),
         ["TERRAIN", "SINK_RATE"],
         "TERRAIN! TERRAIN! 600ft, 2300fpm descent! 立即减小下降率并拉起！",
         ["TERRAIN", "SINK_RATE"]),
        # BANK_ANGLE + SINK_RATE + GLIDESLOPE (unstabilized approach)
        ("APPROACH", vary_params(PHASE_PARAMS["APPROACH"],
            alt_msl=600, alt_agl=600, roll=38.0, vs=-1500, flap_pct=40, gear_down=True),
         ["BANK_ANGLE", "SINK_RATE", "GLIDESLOPE"],
         "不稳定进近！坡度38°+下滑道偏离+下沉率1500fpm！立即复飞！",
         ["BANK_ANGLE", "SINK_RATE", "GLIDESLOPE"]),
        # STALL + PULL_UP (extreme emergency)
        ("LANDING", vary_params(PHASE_PARAMS["LANDING"],
            alt_msl=100, alt_agl=100, ias=95, vs=-2500, pitch=18.0, gear_down=True, flap_pct=80),
         ["STALL", "PULL_UP"],
         "PULL UP! PULL UP! 失速95节+近地100英尺！最大推力复飞！推杆改出！",
         ["STALL", "PULL_UP"]),
        # OVERSPEED + high speed descent
        ("DESCENT", vary_params(PHASE_PARAMS["DESCENT"],
            alt_msl=25000, alt_agl=25000, ias=360, mach=0.88, vs=-2500),
         ["OVERSPEED", "SINK_RATE"],
         "超速360节！高速下降中！收油门减速！控制下降率！",
         ["OVERSPEED", "SINK_RATE"]),
        # ENG_OVERHEAT + LOW_FUEL (critical endurance)
        ("CLIMB2", vary_params(PHASE_PARAMS["CLIMB2"],
            egt_l=880, egt_r=885, fuel_total=2800),
         ["ENG_OVERHEAT", "LOW_FUEL"],
         "双重紧急：发动机超温+燃油仅2800磅！减小推力并申请优先着陆！",
         ["ENG_OVERHEAT", "LOW_FUEL"]),
        # WINDSHEAR + MASTER_WARNING (worst-case takeoff)
        ("TAKEOFF", vary_params(PHASE_PARAMS["TAKEOFF"],
            alt_msl=800, alt_agl=800, ias=125),
         ["WINDSHEAR", "MASTER_WARNING"],
         "风切变警告+主警告！800英尺！TOGA推力！保持俯仰姿态！",
         ["WINDSHEAR"], {"master_warning": True}),
        # FIRE_CARGO + CABIN_ALT_HIGH (dual emergency)
        ("CRUISE", vary_params(PHASE_PARAMS["CRUISE"],
            cabin_alt=11000, cabin_diff=3.5, fuel_total=10000),
         ["FIRE_CARGO", "CABIN_ALT_HIGH"],
         "货舱火警+座舱高度11000英尺！紧急下降至10000英尺！释放货舱灭火瓶！",
         ["FIRE_CARGO", "CABIN_ALT_HIGH"]),
        # TCAS_RA + simultaneous climb/descent conflict
        ("DESCENT", vary_params(PHASE_PARAMS["DESCENT"],
            alt_msl=12000, alt_agl=12000, vs=-1200),
         ["TCAS_RA", "MASTER_CAUTION"],
         "TCAS RA决断咨询！立即执行垂直速度指令！检查冲突飞机！",
         ["TCAS_RA"], {"master_caution": True}),
        # ICING + ANTI_ICE_FAULT (winter hazard)
        ("APPROACH", vary_params(PHASE_PARAMS["APPROACH"],
            alt_msl=4000, alt_agl=4000, oat=-5, anti_ice=False),
         ["ICING_CONDITION", "ANTI_ICE_FAULT"],
         "结冰条件+防冰故障！OAT -5°C！接通所有可用防冰！监控空速和发动机！",
         ["ICING_CONDITION", "ANTI_ICE_FAULT"]),
    ]
    for phase, params, active, text, *rest in combined_cases:
        kw = {}
        combined = []
        if rest:
            if isinstance(rest[0], list):
                combined = rest[0]
            if len(rest) > 1 and isinstance(rest[1], dict):
                kw = rest[1]
        if isinstance(combined, list):
            if "master_warning" in combined:
                kw["master_warning"] = True
            if "master_caution" in combined:
                kw["master_caution"] = True
            # Filter out master_warning/master_caution from combined_alerts list
            clean_combined = [a for a in combined if a not in ("master_warning", "master_caution")]
        else:
            clean_combined = combined
        samples.append(make_sample(phase, params, active, text,
                                   category="D_COMBINED", combined_alerts=clean_combined, **kw))

    # ------------------------------------------------------------------
    # E_NORMAL — 5 normal cruise/descent reports
    # ------------------------------------------------------------------
    normal_cases = [
        ("CRUISE", "正常巡航：FL350，马赫0.80，燃油15000磅，预计续航2.5小时。"),
        ("CRUISE", "飞行状态正常：巡航，35000英尺，280节，双发710/712°C。"),
        ("DESCENT", "下降中：20000英尺，290节，下降率1800fpm，预计25分钟后进近。"),
        ("CLIMB2", "爬升中：25000英尺，270节，N1 75%/75%，燃油充足，状态正常。"),
        ("APPROACH", "进近准备：5000英尺，180节，襟翼30%，起落架待放，稳定进近。"),
    ]
    for phase, text in normal_cases:
        params = dict(PHASE_PARAMS[phase])
        samples.append(make_sample(phase, params, [], text,
                                   category="E_NORMAL"))

    # ------------------------------------------------------------------
    # EDGE CASES — 5 boundary/corner scenarios
    # ------------------------------------------------------------------
    edge_cases = [
        # All systems failing simultaneously
        ("APPROACH", vary_params(PHASE_PARAMS["APPROACH"],
            alt_msl=500, alt_agl=500, ias=130, vs=-1200, roll=15.0,
            flap_pct=15, gear_down=False,
            egt_l=920, egt_r=925, oil_press_l=18, oil_press_r=19,
            fuel_total=1800, cabin_alt=10500, bus_v=22.0,
            hyd_a=1600, hyd_b=1700),
         ["PULL_UP", "SINK_RATE", "TOO_LOW_GEAR", "TOO_LOW_FLAPS",
          "ENG_OVERHEAT", "OIL_PRESS_LOW", "LOW_FUEL", "CABIN_ALT_HIGH",
          "BUS_VOLT_ABNORM", "HYD_PRESS_LOW", "ICING_CONDITION",
          "STALL_WARNING"],
         "多重紧急！立即执行复飞！近地+失速+构型不全+多系统故障！宣布MAYDAY！",
         ["PULL_UP", "TOO_LOW_GEAR", "TOO_LOW_FLAPS", "ENG_OVERHEAT",
          "OIL_PRESS_LOW", "LOW_FUEL", "CABIN_ALT_HIGH", "HYD_PRESS_LOW"]),
        # Normal takeoff — no alerts
        ("TAKEOFF", vary_params(PHASE_PARAMS["TAKEOFF"],
            alt_msl=80, alt_agl=80, ias=150, vs=+2500, flap_pct=20, gear_down=True),
         [],
         "起飞正常：80英尺离地，空速150节增速，正上升率2500fpm，收轮。"),
        # Rejected takeoff (high-speed abort)
        ("TAKEOFF", vary_params(PHASE_PARAMS["TAKEOFF"],
            alt_msl=0, alt_agl=0, ias=120, vs=0, flap_pct=15, gear_down=True,
            n1_l=98, n1_r=98),
         ["MASTER_WARNING"],
         "中断起飞！立即收油门至慢车！最大刹车！减速板放出！",
         ["MASTER_WARNING"], {"master_warning": True}),
        # Go-around at minimums
        ("LANDING", vary_params(PHASE_PARAMS["LANDING"],
            alt_msl=200, alt_agl=200, ias=145, vs=-600, flap_pct=80, gear_down=True),
         ["MINIMUMS"],
         "决断高度200英尺！跑道不可见！执行复飞：TOGA推力→正上升率→收襟翼至15°！"),
        # AP disconnect during cruise turbulence
        ("CRUISE", vary_params(PHASE_PARAMS["CRUISE"],
            roll=15.0, pitch=5.0, ap=False, at=True),
         ["AP_DISENGAGE"],
         "AP断开+颠簸！人工操纵保持航向和高度！检查并尝试重新接通AP！"),
    ]
    for i, (phase, params, active, text, *extra) in enumerate(edge_cases):
        kw = {"master_warning": False, "master_caution": False}
        combined = None
        if extra:
            if isinstance(extra[0], list):
                combined = extra[0]
            elif isinstance(extra[0], dict):
                kw.update(extra[0])
            if len(extra) > 1 and isinstance(extra[1], dict):
                kw.update(extra[1])
        cat = "D_COMBINED" if combined else "E_NORMAL"
        extra_kw = {}
        if combined:
            extra_kw["combined_alerts"] = combined
        samples.append(make_sample(phase, params, active, text,
                                   category=cat, **extra_kw, **kw))

    # ------------------------------------------------------------------
    # Additional varied GPWS: second round with different params
    # ------------------------------------------------------------------
    extra_gpws = [
        ("PULL_UP", "LANDING", vary_params(PHASE_PARAMS["LANDING"],
            alt_msl=55, alt_agl=55, vs=-3100, flap_pct=70, gear_down=True),
         ["PULL_UP", "SINK_RATE"],
         "紧急拉起！55英尺地形迫近！垂直速度3100英尺/分钟！"),
    ]
    for alert_type, phase, params, active, text in extra_gpws:
        samples.append(make_sample(phase, params, active, text,
                                   category="A_GPWS", alert_type=alert_type))

    return samples


# =============================================================================
# Main
# =============================================================================
def main():
    samples = generate_all_samples()

    output_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, "test_data.jsonl")

    with open(output_path, "w", encoding="utf-8") as f:
        for sample in samples:
            f.write(json.dumps(sample, ensure_ascii=False) + "\n")

    # Statistics
    cats = {}
    for s in samples:
        cat = s["category"]
        cats[cat] = cats.get(cat, 0) + 1

    print(f"[OK] Generated {len(samples)} test samples -> {output_path}")
    print(f"\nCategory distribution:")
    for cat, count in sorted(cats.items()):
        label = {
            "A_GPWS": "GPWS告警", "B_PHASE": "阶段正常", "C_SYSTEM": "系统偏差",
            "D_COMBINED": "组合告警", "E_NORMAL": "正常巡航", "F_DREF": "DREF告警",
        }.get(cat, cat)
        print(f"  {cat} ({label}): {count}")

    # Print 3 random samples for review
    print(f"\n--- Sample preview (3 random) ---")
    for i, idx in enumerate(random.sample(range(len(samples)), min(3, len(samples)))):
        s = samples[idx]
        cat = s["category"]
        alert = s.get("alert_type") or s.get("system_rule") or s.get("dref_alert") or s.get("combined_alerts", "N/A")
        assistant = s["conversations"][2]["content"]
        print(f"\n[{i+1}] {cat} / {alert}")
        print(f"    → {assistant}")


if __name__ == "__main__":
    main()
