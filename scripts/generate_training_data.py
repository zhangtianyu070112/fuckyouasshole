"""
Main training data generation script.

Generates ~37,000 training samples in ShareGPT conversation format:
  A_GPWS   : 12,600 — 13 GPWS alert types (~1000 each)
  B_PHASE  :  6,400 — 8 flight phase normal summaries (~800 each)
  C_SYSTEM :  7,800 — 13 system parameter deviations (~600 each)
  D_COMBINED: 4,000 — multi-condition overlapping scenarios
  E_NORMAL :  3,000 — normal cruise routine check-in
  F_DREF   :  3,900 — 13 DREF-only alerts randomly injected (~300 each)

Output:
  scripts/data/training_data.jsonl  (~33,500 train samples)
  scripts/data/eval_data.jsonl      (~3,700 eval samples)

Usage:
  python scripts/generate_training_data.py
  python scripts/generate_training_data.py --num-profiles 80  # full diversity
"""

import copy
import json
import math
import os
import random
import sys
from collections import defaultdict
from typing import Any, Dict, List, Optional, Tuple

# Add script directory to path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from flight_profile import (
    FlightProfile, FlightFrame, generate_frame_pool,
    PHASE_NAMES, PHASE_NAMES_CN, PHASE_COUNT,
    PHASE_TAKEOFF, PHASE_CLIMB1, PHASE_CLIMB2, PHASE_CRUISE,
    PHASE_DESCENT, PHASE_APPROACH, PHASE_LANDING, PHASE_TAXI,
)
from text_variants import (
    GPWS_TEMPLATES, SYSTEM_TEMPLATES,
    COMBINED_TEMPLATES,
    ALERT_CATEGORY_MAP, SYSTEM_PROMPT,
    get_templates, get_phase_template, get_normal_template,
)


# =============================================================================
# Configuration
# =============================================================================

# Data distribution (scaled for aviation safety — lower train loss target)
DISTRIBUTION = {
    "A_GPWS":   14000,
    "B_PHASE":   6400,
    "C_SYSTEM":  6000,
    "D_COMBINED": 4000,
    "E_NORMAL":  3000,
}

# GPWS alerts: samples per type (total 14000 across 13 types)
GPWS_PER_TYPE = 1000   # 13 × 1000 = 13000 + MINIMUMS 600 ≈ 13600
# System rules: samples per type (total 6000 across 10 types)
SYSTEM_PER_TYPE = 600  # 10 × 600 = 6000
# Phase summaries per phase (total 6400 across 8 phases)
PHASE_PER_TYPE = 800   # 8 × 800 = 6400

# Frame pool — maximal diversity for safety-critical application
DEFAULT_NUM_PROFILES = 80
FRAME_RATE_HZ = 10
BASE_SEED = 42

# Output
OUTPUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
TRAIN_FILE = os.path.join(OUTPUT_DIR, "training_data.jsonl")
EVAL_FILE = os.path.join(OUTPUT_DIR, "eval_data.jsonl")
STATS_FILE = os.path.join(OUTPUT_DIR, "generation_stats.json")

# =============================================================================
# Rule evaluation — check if a frame triggers an alert
# =============================================================================

def check_alert_condition(frame: FlightFrame, alert_name: str,
                          rules_config: dict) -> bool:
    """
    Check if a flight frame triggers a specific alert condition.

    Uses the exact thresholds from alert_rules.json (extracted from alert_system.c).
    """
    alert_cfg = rules_config.get("alert_types", {}).get(alert_name)
    if not alert_cfg:
        # Check system rules
        alert_cfg = rules_config.get("system_rules", {}).get(alert_name)
    if not alert_cfg:
        return False

    conditions = alert_cfg.get("conditions", {})

    for field, constraint in conditions.items():
        if field == "phase":
            # Special: check phase name match
            expected = constraint
            if frame.phase_name != expected:
                return False
            continue

        # Get field value from frame
        if field == "agl_ft":
            val = frame.agl_ft
        elif field == "vs_fpm":
            val = frame.vs_fpm
        elif field == "ias_kts":
            val = frame.ias_kts
        elif field == "roll_deg_abs":
            val = frame.roll_deg_abs
        elif field == "flap_ratio":
            val = frame.flap_ratio
        elif field == "gear_deployed":
            val = frame.gear_deployed
        elif field == "nav1_cdi_abs":
            val = frame.nav1_cdi_abs
        elif field == "ias_delta_abs":
            val = frame.ias_delta_abs
        elif field == "master_warning":
            val = frame.master_warning
        elif field == "master_caution":
            val = frame.master_caution
        elif field == "egt_max_c" or field == "egt_c_any":
            val = frame.egt_max_c
        elif field == "n1_diff_pct":
            val = frame.n1_diff_pct
        elif field == "fuel_imbalance_lbs":
            val = frame.fuel_imbalance_lbs
        elif field == "oil_press_psi_any" or field == "oil_press_min_psi":
            val = frame.oil_press_min_psi
        elif field == "cabin_alt_ft":
            val = frame.cabin_alt_ft
        elif field == "elec_bus_volts":
            val = frame.elec_bus_volts
        elif field == "elec_bus_volts_alt":
            # Alternative check: over-voltage
            if "min" in constraint and frame.elec_bus_volts < constraint["min"]:
                # Only applies if we're checking low AND high voltage separately
                pass
            continue
        elif field == "fuel_total_lbs":
            val = frame.fuel_total_lbs
        elif field == "oat_c":
            val = frame.oat_c
        elif field == "anti_ice_wing":
            val = frame.anti_ice_wing
        elif field == "apu_egt_c":
            val = frame.apu_egt_c
        elif field == "apu_running":
            val = frame.apu_running
        elif field == "hyd_press_psi_any":
            val = min(frame.hyd_press_psi[0], frame.hyd_press_psi[1])
        elif field == "hyd_qty_pct_any":
            val = min(frame.hyd_qty_pct[0], frame.hyd_qty_pct[1])
        elif field == "oil_temp_c_any":
            val = max(frame.oil_temp_c[0], frame.oil_temp_c[1])
        else:
            continue

        # Check constraints
        if "eq" in constraint and val != constraint["eq"]:
            return False
        if "min" in constraint and val <= constraint["min"]:
            return False
        if "max" in constraint and val >= constraint["max"]:
            return False
        if "min_alt" in constraint:
            # Alternative minimum (used for BUS_VOLT_ABNORM high side)
            if "max" in constraint:
                # Both low and high constraints — must fail both normal ranges
                if constraint.get("max", float("inf")) < val < constraint.get("min_alt", float("-inf")):
                    return False
        if "crossing_descend" in constraint:
            # AGL crossing a threshold while descending — check AGL within
            # ±100ft of the target and aircraft descending.
            target = constraint["crossing_descend"]
            if not (target - 100 < val < target + 100 and frame.vs_fpm < -50):
                return False
        if "crossing_climb" in constraint:
            # AGL crossing a threshold while climbing.
            target = constraint["crossing_climb"]
            if not (target - 100 < val < target + 100 and frame.vs_fpm > 50):
                return False

    return True


def check_system_condition(frame: FlightFrame, rule_name: str,
                           rules_config: dict) -> bool:
    """Check if a frame triggers a system rule."""
    return check_alert_condition(frame, rule_name, rules_config)


def frame_matches_phase(frame: FlightFrame, phase_names: List[str]) -> bool:
    """Check if frame is in any of the given phases."""
    return frame.phase_name in phase_names


# =============================================================================
# Frame perturbation — force specific alert conditions
# =============================================================================

def perturb_frame_for_alert(frame: FlightFrame, alert_name: str,
                            rules_config: dict, rng: random.Random) -> FlightFrame:
    """
    Modify a frame's parameters to trigger a specific alert.

    This creates "synthetic" alert-triggering frames from base profile frames
    that are in the right phase but don't yet trigger the alert.
    """
    import copy
    f = copy.deepcopy(frame)

    alert_cfg = (rules_config.get("alert_types", {}).get(alert_name) or
                 rules_config.get("system_rules", {}).get(alert_name))
    if not alert_cfg:
        return f

    conditions = alert_cfg.get("conditions", {})

    # Apply perturbations with Gaussian noise to cross threshold
    for field, constraint in conditions.items():
        if field == "phase":
            continue

        noise = rng.gauss(0, 1)

        if field == "agl_ft":
            if "max" in constraint:
                # Need low AGL — use value below threshold
                limit = constraint["max"]
                f.agl_ft = limit * (0.3 + 0.4 * rng.random())
                f.altitude_agl_ft = f.agl_ft
                f.altitude_ft = f.agl_ft  # simplified sea-level
            if "min" in constraint and "max" in constraint:
                lo, hi = constraint["min"], constraint["max"]
                f.agl_ft = lo + (hi - lo) * rng.random()
                f.altitude_agl_ft = f.agl_ft

        elif field == "vs_fpm":
            if "max" in constraint:
                # Need strong descent — use value below (more negative than) threshold
                limit = constraint["max"]
                f.vs_fpm = limit * (1.1 + 0.5 * rng.random())  # 10-60% beyond threshold
                f.vs = f.vs_fpm
            if "min" in constraint:
                limit = constraint["min"]
                f.vs_fpm = limit * (1.0 + 0.3 * rng.random())

        elif field == "ias_kts":
            if "max" in constraint:
                limit = constraint["max"]
                f.ias_kts = limit * (0.5 + 0.4 * rng.random())
            if "min" in constraint:
                limit = constraint["min"]
                f.ias_kts = limit * (1.0 + 0.1 * rng.random())

        elif field == "roll_deg_abs":
            if "min" in constraint:
                limit = constraint["min"]
                sign = 1 if rng.random() > 0.5 else -1
                f.roll_deg = sign * limit * (1.0 + 0.3 * rng.random())
                f.roll_deg_abs = abs(f.roll_deg)

        elif field == "flap_ratio":
            if "max" in constraint:
                limit = constraint["max"]
                f.flap_ratio = limit * rng.random()
            if "min" in constraint:
                limit = constraint["min"]
                f.flap_ratio = limit + (1.0 - limit) * rng.random()

        elif field == "gear_deployed":
            if "eq" in constraint:
                f.gear_deployed = constraint["eq"]
                f.gear_ratio = 1.0 if f.gear_deployed else 0.0

        elif field == "nav1_cdi_abs":
            if "min" in constraint:
                limit = constraint["min"]
                f.nav1_cdi = limit * (1.0 + 0.5 * rng.random()) * (1 if rng.random() > 0.5 else -1)
                f.nav1_cdi_abs = abs(f.nav1_cdi)

        elif field == "ias_delta_abs":
            if "min" in constraint:
                limit = constraint["min"]
                f.ias_delta_abs = limit * (1.0 + 0.5 * rng.random())

        elif field == "master_warning":
            if "eq" in constraint:
                f.master_warning = constraint["eq"]
        elif field == "master_caution":
            if "eq" in constraint:
                f.master_caution = constraint["eq"]

        elif field in ("egt_max_c", "egt_c_any"):
            if "min" in constraint:
                limit = constraint["min"]
                f.egt_max_c = limit * (1.0 + 0.1 * rng.random())
                f.egt_c[0] = f.egt_max_c
                f.egt_c[1] = f.egt_max_c * (0.95 + 0.1 * rng.random())

        elif field == "n1_diff_pct":
            if "min" in constraint:
                limit = constraint["min"]
                f.n1_diff_pct = limit * (1.0 + 0.5 * rng.random())
                mid = (f.n1_pct[0] + f.n1_pct[1]) / 2.0
                f.n1_pct[0] = mid + f.n1_diff_pct / 2.0
                f.n1_pct[1] = mid - f.n1_diff_pct / 2.0

        elif field == "fuel_imbalance_lbs":
            if "min" in constraint:
                limit = constraint["min"]
                f.fuel_imbalance_lbs = limit * (1.0 + 0.5 * rng.random())

        elif field in ("oil_press_psi_any", "oil_press_min_psi"):
            if "max" in constraint:
                limit = constraint["max"]
                f.oil_press_min_psi = limit * (0.5 + 0.4 * rng.random())
                f.oil_press_psi[0] = f.oil_press_min_psi

        elif field == "cabin_alt_ft":
            if "min" in constraint:
                limit = constraint["min"]
                f.cabin_alt_ft = limit * (1.0 + 0.3 * rng.random())

        elif field == "elec_bus_volts":
            if "max" in constraint:
                limit = constraint["max"]
                f.elec_bus_volts = limit * (0.5 + 0.4 * rng.random())
            if "min_alt" in constraint:
                limit = constraint["min_alt"]
                f.elec_bus_volts = limit * (1.0 + 0.2 * rng.random())

        elif field == "fuel_total_lbs":
            if "max" in constraint:
                limit = constraint["max"]
                f.fuel_total_lbs = limit * (0.3 + 0.6 * rng.random())

        elif field == "oat_c":
            if "max" in constraint:
                limit = constraint["max"]
                f.oat_c = limit * (0.5 + 0.5 * rng.random()) - 5.0

        elif field == "anti_ice_wing":
            if "eq" in constraint:
                f.anti_ice_wing = constraint["eq"]

        elif field == "apu_egt_c":
            if "min" in constraint:
                limit = constraint["min"]
                f.apu_egt_c = limit * (1.0 + 0.3 * rng.random())

        elif field == "apu_running":
            if "eq" in constraint:
                f.apu_running = constraint["eq"]

        elif field == "hyd_press_psi_any":
            if "max" in constraint:
                limit = constraint["max"]
                low_val = limit * (0.5 + 0.4 * rng.random())
                f.hyd_press_psi[0] = low_val
                f.hyd_press_psi[1] = limit + 200 + rng.random() * 200  # other system ok

        elif field == "hyd_qty_pct_any":
            if "max" in constraint:
                limit = constraint["max"]
                low_val = limit * rng.random()
                f.hyd_qty_pct[0] = low_val
                f.hyd_qty_pct[1] = 0.90 + rng.random() * 0.10  # other system ok

        elif field == "oil_temp_c_any":
            if "min" in constraint:
                limit = constraint["min"]
                f.oil_temp_c[0] = limit * (1.0 + 0.2 * rng.random())
                f.oil_temp_c[1] = limit * (0.8 + 0.15 * rng.random())  # other also high-ish

    # Update derived fields
    f.alt_ft = f.altitude_ft
    f.agl_ft = f.altitude_agl_ft
    f.vs = f.vs_fpm
    f.roll_deg_abs = abs(f.roll_deg)
    f.nav1_cdi_abs = abs(f.nav1_cdi)
    f.n1_diff_pct = abs(f.n1_pct[0] - f.n1_pct[1])
    f.egt_max_c = max(f.egt_c[0], f.egt_c[1])
    f.oil_press_min_psi = min(f.oil_press_psi[0], f.oil_press_psi[1])

    return f


# =============================================================================
# Alert state collection
# =============================================================================

def get_active_alert_list(frame: FlightFrame, rules_config: dict) -> list:
    """Collect all alert states for a frame as (name, is_active) tuples.

    Returns a list ordered by: GPWS alerts (by priority), system rules.
    Used by format_user_input() to build the alert state block.
    """
    results = []

    # GPWS alert types (sorted by priority from alert_rules.json)
    alert_types = rules_config.get("alert_types", {})
    for alert_name in alert_types:
        is_active = check_alert_condition(frame, alert_name, rules_config)
        results.append((alert_name, is_active))

    # System rules (derived from flight data)
    system_rules = rules_config.get("system_rules", {})
    for rule_name in system_rules:
        is_active = check_system_condition(frame, rule_name, rules_config)
        results.append((rule_name, is_active))

    # DREF-only alerts (read from frame boolean fields — randomly injected)
    dref_alerts = rules_config.get("dref_alerts", {})
    DREF_FIELD_MAP = {
        "FIRE_ENG1": "fire_eng1", "FIRE_ENG2": "fire_eng2",
        "FIRE_APU": "fire_apu", "FIRE_WHEEL_WELL": "fire_wheel_well",
        "FIRE_CARGO": "fire_cargo",
        "TCAS_TA": "tcas_ta", "TCAS_RA": "tcas_ra",
        "STALL_WARNING": "stall_warning",
        "DOOR_OPEN": "door_open",
        "ELEC_FAULT": "elec_fault", "ANTI_ICE_FAULT": "anti_ice_fault",
        "AP_DISENGAGE": "ap_disengage", "AT_DISENGAGE": "at_disengage",
    }
    for alert_name in dref_alerts:
        if alert_name.startswith("_"):
            continue  # skip metadata keys
        field_name = DREF_FIELD_MAP.get(alert_name)
        if field_name:
            is_active = bool(getattr(frame, field_name, 0))
        else:
            is_active = False
        results.append((alert_name, is_active))

    return results


# =============================================================================
# User input formatting (ShareGPT "user" role content)
# =============================================================================

def format_user_input(frame: FlightFrame,
                      active_alerts: list = None) -> str:
    """Format flight parameters and alert states for the model input.

    Args:
        frame:         Current flight data snapshot.
        active_alerts: List of (alert_name, is_active) tuples from
                       get_active_alert_list(). If None, alert block is omitted.
    """
    phase_cn = PHASE_NAMES_CN[frame.phase]

    # Status text
    gear_text = "放下" if frame.gear_deployed else "收上"
    ap_text = "接通" if frame.ap_engaged else "断开"
    at_text = "接通" if frame.ap_athr_engaged else "断开"
    apu_text = "运转" if frame.apu_running else "关闭"
    anti_ice_text = "开" if frame.anti_ice_wing else "关"
    spdbrk_text = "放出" if frame.speedbrake_ratio > 0.1 else "收上"

    # --- Flight Parameters Block ---
    lines = [
        f"飞行阶段: {phase_cn}",
        f"高度: {frame.altitude_ft:.0f}ft MSL / {frame.altitude_agl_ft:.0f}ft AGL",
        f"空速: {frame.ias_kts:.0f} KIAS / M{frame.mach:.2f} / GS {frame.gs_kts:.0f}kt",
        f"垂直速度: {frame.vs_fpm:+.0f} fpm",
        f"姿态: 坡度 {frame.roll_deg:+.1f}° / 俯仰 {frame.pitch_deg:+.1f}°",
        f"航向: {frame.heading_true_deg:.0f}°T / {frame.heading_mag_deg:.0f}°M",
        f"发动机: N1 {frame.n1_pct[0]:.0f}%/{frame.n1_pct[1]:.0f}%  "
        f"N2 {frame.n2_pct[0]:.0f}%/{frame.n2_pct[1]:.0f}%  "
        f"EGT {frame.egt_c[0]:.0f}°C/{frame.egt_c[1]:.0f}°C",
        f"滑油: 压力 {frame.oil_press_psi[0]:.0f}/{frame.oil_press_psi[1]:.0f} psi  "
        f"温度 {frame.oil_temp_c[0]:.0f}/{frame.oil_temp_c[1]:.0f}°C",
        f"燃油: 总量 {frame.fuel_total_lbs:.0f} lbs  "
        f"流量 {frame.fuel_flow_total_pph:.0f} pph  "
        f"左右差 {frame.fuel_imbalance_lbs:.0f} lbs",
        f"襟翼: {frame.flap_ratio*100:.0f}%  |  起落架: {gear_text}  |  减速板: {spdbrk_text}",
        f"液压: A={frame.hyd_press_psi[0]:.0f} psi  B={frame.hyd_press_psi[1]:.0f} psi",
        f"电源: 总线 {frame.elec_bus_volts:.1f}V",
        f"座舱: 高度 {frame.cabin_alt_ft:.0f}ft  /  压差 {frame.cabin_diff_psi:.1f} psi",
        f"环境: OAT {frame.oat_c:+.0f}°C  /  风 {frame.wind_speed_kts:.0f}kt@{frame.wind_dir_deg:.0f}°",
        f"AP: {ap_text}  |  A/T: {at_text}  |  APU: {apu_text}  |  防冰: {anti_ice_text}",
    ]

    # --- Alert State Block ---
    if active_alerts is not None:
        lines.append("")
        lines.append("告警状态:")
        for alert_name, is_active in active_alerts:
            tag = "⚠ ACTIVE" if is_active else "  inactive"
            lines.append(f"  {alert_name:<24} {tag}")
        # Master warning/caution at bottom
        mw = "⚠ ON " if frame.master_warning else "  OFF"
        mc = "⚠ ON " if frame.master_caution else "  OFF"
        lines.append(f"  {'master_warning':<24} {mw}")
        lines.append(f"  {'master_caution':<24} {mc}")

    return "\n".join(lines)

    return "\n".join(lines)


# =============================================================================
# Template filling
# =============================================================================

def fill_template(template: str, frame: FlightFrame) -> str:
    """Replace {placeholder} slots in a template with actual frame values."""
    fl = frame.altitude_ft / 100.0  # flight level
    hours = max(0.1, frame.fuel_total_lbs / 6000.0)  # rough endurance
    phase_cn = PHASE_NAMES_CN[frame.phase]
    spdbrk = "放出" if frame.speedbrake_ratio > 0.1 else "收上"
    reverser = "开锁" if frame.phase == PHASE_LANDING else "关闭"
    gear_status = "已放下" if frame.gear_deployed else "未放下"

    result = template.format(
        agl=frame.altitude_agl_ft,
        alt=frame.altitude_ft,
        vs=abs(frame.vs_fpm),  # absolute value for display
        ias=frame.ias_kts,
        gs=frame.gs_kts,
        mach=frame.mach,
        roll=abs(frame.roll_deg),
        pitch=frame.pitch_deg,
        hdg=frame.heading_true_deg,
        n1_0=frame.n1_pct[0],
        n1_1=frame.n1_pct[1],
        egt_0=frame.egt_c[0],
        egt_1=frame.egt_c[1],
        egt_max=frame.egt_max_c,
        flap=frame.flap_ratio,
        gear=frame.gear_deployed,
        fuel=frame.fuel_total_lbs,
        cdi=frame.nav1_cdi_abs,
        delta=frame.ias_delta_abs,
        phase=phase_cn,
        oat=frame.oat_c,
        cabin_alt=frame.cabin_alt_ft,
        oil_press=frame.oil_press_min_psi,
        n1_diff=frame.n1_diff_pct,
        fuel_imb=frame.fuel_imbalance_lbs,
        bus_volts=frame.elec_bus_volts,
        apu_egt=frame.apu_egt_c,
        apu_n1=frame.apu_n1_pct,
        hours=hours,
        fl=fl,
        spdbrk=spdbrk,
        reverser=reverser,
        gear_status=gear_status,
        # New placeholders for derived + DREF alert templates
        p0=frame.hyd_press_psi[0],
        p1=frame.hyd_press_psi[1],
        q0=frame.hyd_qty_pct[0],
        q1=frame.hyd_qty_pct[1],
        oil_temp_0=frame.oil_temp_c[0],
    )
    return result


# =============================================================================
# ShareGPT record assembly
# =============================================================================

def make_sharegpt_record(user_content: str, assistant_content: str,
                         category: str = "") -> dict:
    """Build a ShareGPT-format conversation record."""
    record = {
        "conversations": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": user_content},
            {"role": "assistant", "content": assistant_content},
        ]
    }
    if category:
        record["category"] = category
    return record


# =============================================================================
# Generation functions for each category
# =============================================================================

def generate_gpws_samples(frames: List[FlightFrame], rules_config: dict,
                          rng: random.Random) -> List[dict]:
    """
    Generate A_GPWS samples: 13 alert types × ~270 samples each.

    Strategy:
      - Find frames naturally in plausible phases
      - Perturb parameters to cross alert thresholds
      - Also find frames that naturally trigger alerts (from demo steep turns etc.)
      - Select random text variant and fill template
    """
    samples = []
    alert_types = list(rules_config.get("alert_types", {}).keys())

    # Pre-index frames by phase for efficiency
    frames_by_phase: Dict[str, List[FlightFrame]] = defaultdict(list)
    for f in frames:
        frames_by_phase[f.phase_name].append(f)

    for alert_name in alert_types:
        alert_cfg = rules_config["alert_types"][alert_name]
        plausible_phases = alert_cfg.get("plausible_phases", PHASE_NAMES)
        templates = GPWS_TEMPLATES.get(alert_name, [])

        if not templates:
            print(f"  WARNING: No templates for alert {alert_name}, skipping")
            continue

        # Collect candidate frames from plausible phases
        candidates = []
        for ph in plausible_phases:
            candidates.extend(frames_by_phase.get(ph, []))

        if not candidates:
            print(f"  WARNING: No candidate frames for {alert_name} in phases {plausible_phases}")
            continue

        # Generate samples
        target_count = GPWS_PER_TYPE
        if alert_name == "MINIMUMS":
            target_count = 600  # Fewer variants for MINIMUMS (simpler message)

        for i in range(target_count):
            base_frame = rng.choice(candidates)
            # Perturb to trigger alert
            perturbed = perturb_frame_for_alert(base_frame, alert_name, rules_config, rng)

            # Verify alert condition (re-perturb if needed)
            retries = 0
            while not check_alert_condition(perturbed, alert_name, rules_config) and retries < 10:
                perturbed = perturb_frame_for_alert(base_frame, alert_name, rules_config, rng)
                retries += 1

            # Select random template variant
            template = rng.choice(templates)

            # Add slight noise to frame values for diversity
            perturbed.altitude_agl_ft += rng.gauss(0, 5)
            perturbed.vs_fpm += rng.gauss(0, 50)
            perturbed.ias_kts += rng.gauss(0, 2)

            # Fill template
            assistant_text = fill_template(template, perturbed)
            active_alerts = get_active_alert_list(perturbed, rules_config)
            user_text = format_user_input(perturbed, active_alerts)

            sample = make_sharegpt_record(user_text, assistant_text, "A_GPWS")
            sample["alert_type"] = alert_name
            samples.append(sample)

        print(f"  {alert_name:<20}: {target_count} samples")

    return samples


def generate_phase_samples(frames: List[FlightFrame], rules_config: dict,
                          rng: random.Random) -> List[dict]:
    """
    Generate B_PHASE samples: 8 phases × ~312 normal summaries each.

    Strategy: sample frames from each phase (avoiding alert-trigger frames),
    fill phase summary templates.
    """
    samples = []

    frames_by_phase: Dict[str, List[FlightFrame]] = defaultdict(list)
    for f in frames:
        frames_by_phase[f.phase_name].append(f)

    for phase_name in PHASE_NAMES:
        candidates = frames_by_phase.get(phase_name, [])
        if not candidates:
            continue

        templates = get_phase_template(phase_name)
        if not templates:
            continue

        for i in range(PHASE_PER_TYPE):
            frame = rng.choice(candidates)
            template = rng.choice(templates)
            assistant_text = fill_template(template, frame)
            user_text = format_user_input(frame, get_active_alert_list(frame, rules_config))

            sample = make_sharegpt_record(user_text, assistant_text, "B_PHASE")
            sample["phase"] = phase_name
            samples.append(sample)

        print(f"  {phase_name:<10}: {PHASE_PER_TYPE} samples")

    return samples


def generate_system_samples(frames: List[FlightFrame], rules_config: dict,
                            rng: random.Random) -> List[dict]:
    """
    Generate C_SYSTEM samples: 10 system deviations × ~250 each.

    Strategy: similar to GPWS but for system-level deviations.
    """
    samples = []
    system_rules = rules_config.get("system_rules", {})

    frames_by_phase: Dict[str, List[FlightFrame]] = defaultdict(list)
    for f in frames:
        frames_by_phase[f.phase_name].append(f)

    for rule_name, rule_cfg in system_rules.items():
        plausible_phases = rule_cfg.get("plausible_phases", PHASE_NAMES)
        templates = SYSTEM_TEMPLATES.get(rule_name, [])

        if not templates:
            print(f"  WARNING: No templates for system rule {rule_name}, skipping")
            continue

        candidates = []
        for ph in plausible_phases:
            candidates.extend(frames_by_phase.get(ph, []))

        if not candidates:
            print(f"  WARNING: No candidate frames for {rule_name}")
            continue

        for i in range(SYSTEM_PER_TYPE):
            base_frame = rng.choice(candidates)
            perturbed = perturb_frame_for_alert(base_frame, rule_name, rules_config, rng)

            template = rng.choice(templates)
            assistant_text = fill_template(template, perturbed)
            active_alerts = get_active_alert_list(perturbed, rules_config)
            user_text = format_user_input(perturbed, active_alerts)

            sample = make_sharegpt_record(user_text, assistant_text, "C_SYSTEM")
            sample["system_rule"] = rule_name
            samples.append(sample)

        print(f"  {rule_name:<20}: {SYSTEM_PER_TYPE} samples")

    return samples


def generate_combined_samples(frames: List[FlightFrame], rules_config: dict,
                              rng: random.Random) -> List[dict]:
    """
    Generate D_COMBINED samples: multi-condition overlapping scenarios.

    Strategy: pick 2-3 compatible alerts, perturb frame to trigger both,
    select or generate combined text.
    """
    samples = []
    target = DISTRIBUTION["D_COMBINED"]

    # Compatible alert pairs (alerts that can fire together)
    compatible_pairs = [
        (["SINK_RATE", "TOO_LOW_GEAR"], "SINK_RATE + TOO_LOW_GEAR"),
        (["SINK_RATE", "TOO_LOW_FLAPS"], "SINK_RATE + TOO_LOW_FLAPS"),
        (["TERRAIN", "SINK_RATE"], "TERRAIN + SINK_RATE"),
        (["TOO_LOW_GEAR", "TOO_LOW_FLAPS"], "GEAR + FLAPS"),
        (["GLIDESLOPE", "SINK_RATE"], "GLIDESLOPE + SINK_RATE"),
        (["BANK_ANGLE", "TERRAIN"], "BANK_ANGLE + TERRAIN"),
        (["STALL", "PULL_UP"], "STALL + PULL_UP"),
        (["WINDSHEAR", "MASTER_WARNING"], "WINDSHEAR + MASTER WARNING"),
        (["OVERSPEED", "MASTER_CAUTION"], "OVERSPEED + CAUTION"),
        (["ENG_OVERHEAT", "MASTER_CAUTION"], "ENG OVERHEAT + CAUTION"),
    ]

    # Build candidate maps
    frames_by_phase: Dict[str, List[FlightFrame]] = defaultdict(list)
    for f in frames:
        frames_by_phase[f.phase_name].append(f)

    # Predefined combined templates
    combined_templates = COMBINED_TEMPLATES

    samples_per_pair = target // len(compatible_pairs) + 1

    for alert_names, label in compatible_pairs:
        count = min(samples_per_pair, target - len(samples))
        if count <= 0:
            break

        for i in range(count):
            # Find common plausible phases
            phases_sets = []
            for an in alert_names:
                cfg = (rules_config.get("alert_types", {}).get(an) or
                       rules_config.get("system_rules", {}).get(an))
                if cfg:
                    phases_sets.append(set(cfg.get("plausible_phases", PHASE_NAMES)))

            if phases_sets:
                common_phases = list(set.intersection(*phases_sets)) if len(phases_sets) > 1 else list(phases_sets[0])
            else:
                common_phases = ["APPROACH", "DESCENT"]

            candidates = []
            for ph in common_phases:
                candidates.extend(frames_by_phase.get(ph, []))

            if not candidates:
                candidates = frames  # fallback

            base_frame = rng.choice(candidates)

            # Perturb for first alert
            f1 = perturb_frame_for_alert(base_frame, alert_names[0], rules_config, rng)
            # Perturb again for second alert
            for an in alert_names[1:]:
                f1 = perturb_frame_for_alert(f1, an, rules_config, rng)

            # Build combined output
            if combined_templates:
                # Use predefined combined text
                template = rng.choice(combined_templates)
                assistant_text = fill_template(template, f1)
            else:
                # Generate per-alert text and concatenate
                parts = []
                for an in alert_names:
                    tmpls = get_templates(an)
                    if tmpls:
                        parts.append(fill_template(rng.choice(tmpls), f1))
                assistant_text = " ".join(parts)

            user_text = format_user_input(f1, get_active_alert_list(f1, rules_config))

            sample = make_sharegpt_record(user_text, assistant_text, "D_COMBINED")
            sample["combined_alerts"] = alert_names
            samples.append(sample)

    print(f"  Combined scenarios: {len(samples)} samples")
    return samples


def generate_normal_samples(frames: List[FlightFrame], rules_config: dict,
                           rng: random.Random) -> List[dict]:
    """
    Generate E_NORMAL samples: normal cruise routine check-in.

    Strategy: pick frames from CRUISE/CLIMB2 phases with all parameters
    in normal range, generate "一切正常" responses.
    """
    samples = []
    target = DISTRIBUTION["E_NORMAL"]

    # Filter to normal cruise frames
    cruise_frames = [f for f in frames if f.phase in (PHASE_CRUISE, PHASE_CLIMB2, PHASE_DESCENT)
                     and not f.master_warning and not f.master_caution
                     and f.egt_max_c < 800 and f.n1_diff_pct < 3.0]

    for i in range(target):
        frame = rng.choice(cruise_frames) if cruise_frames else rng.choice(frames)
        template = rng.choice(get_normal_template())
        assistant_text = fill_template(template, frame)
        user_text = format_user_input(frame, get_active_alert_list(frame, rules_config))

        sample = make_sharegpt_record(user_text, assistant_text, "E_NORMAL")
        samples.append(sample)

    print(f"  Normal cruise: {target} samples")
    return samples


# =============================================================================
# DREF-only alert sample generation (random injection)
# =============================================================================

# Per-alert sample counts
DREF_SAMPLES_PER_TYPE = 300   # 13 types × 300 = 3900 samples

DREF_FIELD_MAP = {
    "FIRE_ENG1": "fire_eng1", "FIRE_ENG2": "fire_eng2",
    "FIRE_APU": "fire_apu", "FIRE_WHEEL_WELL": "fire_wheel_well",
    "FIRE_CARGO": "fire_cargo",
    "TCAS_TA": "tcas_ta", "TCAS_RA": "tcas_ra",
    "STALL_WARNING": "stall_warning",
    "DOOR_OPEN": "door_open",
    "ELEC_FAULT": "elec_fault", "ANTI_ICE_FAULT": "anti_ice_fault",
    "AP_DISENGAGE": "ap_disengage", "AT_DISENGAGE": "at_disengage",
}


def generate_dref_samples(frames: List[FlightFrame], rules_config: dict,
                          rng: random.Random) -> List[dict]:
    """Generate F_DREF samples with randomly injected DREF-only alert states.

    Each sample: pick a random frame, inject 1 DREF alert as active, generate
    the appropriate advice. This teaches the model to respond to DREF alerts
    without confusing it about how alerts are detected.
    """
    samples = []
    dref_alerts = rules_config.get("dref_alerts", {})

    for alert_name, alert_cfg in dref_alerts.items():
        if alert_name.startswith("_"):
            continue  # skip metadata keys
        templates = get_templates(alert_name)
        if not templates:
            print(f"  WARNING: No templates for DREF alert {alert_name}, skipping")
            continue

        plausible_phases = alert_cfg.get("plausible_phases", PHASE_NAMES)
        candidates = [f for f in frames if f.phase_name in plausible_phases]
        if not candidates:
            candidates = frames

        field_name = DREF_FIELD_MAP.get(alert_name, "")

        for i in range(DREF_SAMPLES_PER_TYPE):
            frame = copy.deepcopy(rng.choice(candidates))

            # Inject DREF alert (set boolean field to 1)
            if field_name:
                setattr(frame, field_name, 1)

            # For multi-alert scenarios: sometimes inject a second alert (10%)
            second_alerts = []
            if rng.random() < 0.10:
                other_alerts = [a for a in dref_alerts if a != alert_name]
                if other_alerts:
                    second = rng.choice(other_alerts)
                    second_field = DREF_FIELD_MAP.get(second, "")
                    if second_field:
                        setattr(frame, second_field, 1)
                        second_alerts.append(second)

            # Generate active alert list
            active_alerts = get_active_alert_list(frame, rules_config)

            # Select template
            template = rng.choice(templates)
            assistant_text = fill_template(template, frame)
            user_text = format_user_input(frame, active_alerts)

            sample = make_sharegpt_record(user_text, assistant_text, "F_DREF")
            sample["dref_alert"] = alert_name
            if second_alerts:
                sample["combined_dref"] = [alert_name] + second_alerts
            samples.append(sample)

    print(f"\n  F_DREF total: {len(samples)}")
    return samples


# =============================================================================
# Main
# =============================================================================

def main():
    # Parse arguments
    num_profiles = DEFAULT_NUM_PROFILES
    if "--num-profiles" in sys.argv:
        idx = sys.argv.index("--num-profiles")
        if idx + 1 < len(sys.argv):
            num_profiles = int(sys.argv[idx + 1])

    print("=" * 60)
    print("  B737 Cockpit AI Training Data Generator")
    print("=" * 60)
    print()

    # --- Load rules ---
    rules_path = os.path.join(os.path.dirname(__file__), "alert_rules.json")
    with open(rules_path, "r", encoding="utf-8") as f:
        rules_config = json.load(f)
    print(f"[1/5] Loaded alert rules: {len(rules_config['alert_types'])} GPWS + "
          f"{len(rules_config['system_rules'])} system rules + "
          f"{len(rules_config['altitude_callouts'])} altitude callouts")

    # --- Generate frame pool ---
    print(f"\n[2/5] Generating flight profile frame pool ({num_profiles} profiles × 310s @ {FRAME_RATE_HZ}Hz)...")
    all_frames = generate_frame_pool(num_profiles=num_profiles,
                                     update_rate_hz=FRAME_RATE_HZ,
                                     base_seed=BASE_SEED)
    print(f"  Total frame pool: {len(all_frames)} frames")

    # --- Generate each category ---
    print(f"\n[3/5] Generating training samples...")
    rng = random.Random(BASE_SEED)

    all_samples = []

    # A: GPWS alerts
    print("\n  --- A_GPWS ---")
    gpws_samples = generate_gpws_samples(all_frames, rules_config, rng)
    all_samples.extend(gpws_samples)
    print(f"  A_GPWS total: {len(gpws_samples)}")

    # B: Phase summaries
    print("\n  --- B_PHASE ---")
    phase_samples = generate_phase_samples(all_frames, rules_config, rng)
    all_samples.extend(phase_samples)
    print(f"  B_PHASE total: {len(phase_samples)}")

    # C: System deviations
    print("\n  --- C_SYSTEM ---")
    system_samples = generate_system_samples(all_frames, rules_config, rng)
    all_samples.extend(system_samples)
    print(f"  C_SYSTEM total: {len(system_samples)}")

    # D: Combined
    print("\n  --- D_COMBINED ---")
    combined_samples = generate_combined_samples(all_frames, rules_config, rng)
    all_samples.extend(combined_samples)
    print(f"  D_COMBINED total: {len(combined_samples)}")

    # E: Normal
    print("\n  --- E_NORMAL ---")
    normal_samples = generate_normal_samples(all_frames, rules_config, rng)
    all_samples.extend(normal_samples)
    print(f"  E_NORMAL total: {len(normal_samples)}")

    # F: DREF-only alerts (random injection)
    print("\n  --- F_DREF ---")
    dref_samples = generate_dref_samples(all_frames, rules_config, rng)
    all_samples.extend(dref_samples)
    print(f"  F_DREF total: {len(dref_samples)}")

    # --- Shuffle and split ---
    print(f"\n[4/5] Shuffling and splitting (90/10)...")
    rng.shuffle(all_samples)

    split_idx = int(len(all_samples) * 0.9)
    train_samples = all_samples[:split_idx]
    eval_samples = all_samples[split_idx:]

    # --- Write output ---
    print(f"\n[5/5] Writing output files...")
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    with open(TRAIN_FILE, "w", encoding="utf-8") as f:
        for sample in train_samples:
            f.write(json.dumps(sample, ensure_ascii=False) + "\n")

    with open(EVAL_FILE, "w", encoding="utf-8") as f:
        for sample in eval_samples:
            f.write(json.dumps(sample, ensure_ascii=False) + "\n")

    print(f"  Training data: {TRAIN_FILE} ({len(train_samples)} samples)")
    print(f"  Eval data:     {EVAL_FILE} ({len(eval_samples)} samples)")

    # --- Statistics ---
    stats = {
        "total_samples": len(all_samples),
        "train_samples": len(train_samples),
        "eval_samples": len(eval_samples),
        "categories": {},
        "frame_pool_size": len(all_frames),
        "num_profiles": num_profiles,
    }

    for sample in all_samples:
        cat = sample.get("category", "unknown")
        if cat not in stats["categories"]:
            stats["categories"][cat] = 0
        stats["categories"][cat] += 1

    with open(STATS_FILE, "w", encoding="utf-8") as f:
        json.dump(stats, f, ensure_ascii=False, indent=2)

    print(f"\n  Statistics: {STATS_FILE}")
    print(f"\n  Category distribution:")
    for cat, count in sorted(stats["categories"].items()):
        pct = count / len(all_samples) * 100
        print(f"    {cat:<15}: {count:>6} ({pct:5.1f}%)")

    print(f"\n  TOTAL: {len(all_samples)} samples")
    print(f"\n{'=' * 60}")
    print("  Generation complete!")
    print(f"{'=' * 60}")


if __name__ == "__main__":
    main()
