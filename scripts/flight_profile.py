"""
B737-800 Realistic Flight Profile Simulator.

Python rewrite of src/net/mock_data.c with corrections:
  - Vertical speed fixed to real B737 values (was 15,800 fpm → now 2,500 fpm)
  - Gaussian noise on every parameter (sigma = 2-5% of range)
  - Randomized initial conditions per profile (weight ±10%, cruise alt ±2000ft, OAT ±10°C)
  - Produces 20 profile variations × 310s @ 10 Hz = 62,000 frame data pool

Usage:
    from flight_profile import FlightProfile, PHASE_NAMES
    profile = FlightProfile(seed=42)
    frames = profile.generate_profile()  # list of FlightFrame dicts
"""

import math
import random
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

# =============================================================================
# Constants — Real B737-800 flight envelope
# =============================================================================

# Altitude
CRUISE_ALT_FT      = 35000.0
CRUISE_ALT_RANGE_FT = 2000.0    # ±2000ft randomized
CRUISE_IAS_KTS      = 280.0
CRUISE_MACH         = 0.78
APPROACH_IAS_KTS    = 140.0
LANDING_IAS_KTS     = 125.0

# N1 limits
MAX_N1_PCT          = 98.0      # Real 737: ~98% for TOGA
IDLE_N1_PCT         = 22.0
CRUISE_N1_PCT       = 60.0

# Vertical speed (CORRECTED — was severely distorted in C mock_data.c)
CLIMB1_VS_FPM       = 2500.0    # Initial climb
CLIMB2_VS_FPM       = 1800.0    # Climb to cruise
DESCENT_VS_FPM      = -1800.0   # Descent
APPROACH_VS_FPM     = -800.0    # Approach glideslope

# Fuel
FUEL_CAPACITY_LBS   = 15000.0
FUEL_BURN_BASE_PPH  = 6000.0    # Combined both engines at cruise

# Engine EGT
EGT_IDLE_C          = 400.0
EGT_MAX_C           = 950.0

# APU
APU_EGT_RUNNING_C   = 550.0
APU_N1_PCT          = 98.0

# Hydraulics
HYD_NOMINAL_PSI     = 3000.0

# Electrical
ELEC_BUS_NOMINAL_V  = 28.0

# Cabin pressurization
CABIN_ALT_NOMINAL_FT = 6500.0
CABIN_DIFF_MAX_PSI   = 8.3

# Noise levels (sigma as fraction of parameter range)
NOISE_SIGMA_FRAC    = 0.03     # 3% of range

# =============================================================================
# Flight phases
# =============================================================================

PHASE_TAKEOFF  = 0
PHASE_CLIMB1   = 1
PHASE_CLIMB2   = 2
PHASE_CRUISE   = 3
PHASE_DESCENT  = 4
PHASE_APPROACH = 5
PHASE_LANDING  = 6
PHASE_TAXI     = 7
PHASE_COUNT    = 8

PHASE_NAMES = [
    "TAKEOFF", "CLIMB1", "CLIMB2", "CRUISE",
    "DESCENT", "APPROACH", "LANDING", "TAXI"
]

PHASE_NAMES_CN = [
    "起飞", "初始爬升", "爬升至巡航", "巡航",
    "下降", "进近", "着陆", "滑行"
]

# Phase durations in seconds
PHASE_DURATION = [10.0, 30.0, 50.0, 70.0, 60.0, 60.0, 20.0, 10.0]

# Target altitude at end of each phase (ft MSL)
PHASE_ALT_FT = [
    100.0,          # TAKEOFF  — just airborne
    8000.0,         # CLIMB1   — passing 8000
    CRUISE_ALT_FT,  # CLIMB2   — cruise level
    CRUISE_ALT_FT,  # CRUISE   — level
    12000.0,        # DESCENT  — mid descent
    2000.0,         # APPROACH — on final
    0.0,            # LANDING  — on ground
    0.0,            # TAXI     — on ground
]

# Target IAS at end of each phase (knots)
PHASE_IAS = [
    150.0,              # TAKEOFF  — V2+10
    210.0,              # CLIMB1   — accelerating
    CRUISE_IAS_KTS,     # CLIMB2   — cruise speed
    CRUISE_IAS_KTS,     # CRUISE   — level
    260.0,              # DESCENT  — still fast
    APPROACH_IAS_KTS,   # APPROACH — slowing
    LANDING_IAS_KTS,    # LANDING  — Vref
    40.0,               # TAXI     — taxi speed
]

# =============================================================================
# Data structures
# =============================================================================

@dataclass
class FlightFrame:
    """A single frame of flight data — mirrors FlightDataValues from flight_data.h."""

    # Timing
    sim_time_s: float = 0.0
    phase: int = PHASE_CRUISE
    phase_name: str = "CRUISE"
    phase_progress: float = 0.0  # 0..1 within phase

    # Attitude
    roll_deg: float = 0.0
    pitch_deg: float = 0.0
    heading_true_deg: float = 0.0
    heading_mag_deg: float = 0.0

    # Air data
    ias_kts: float = 0.0
    tas_kts: float = 0.0
    gs_kts: float = 0.0
    mach: float = 0.0
    vs_fpm: float = 0.0
    altitude_ft: float = 0.0
    altitude_agl_ft: float = 0.0

    # Position
    lat_deg: float = 31.2
    lon_deg: float = 121.4

    # Engine (2 engines)
    n1_pct: List[float] = field(default_factory=lambda: [0.0, 0.0])
    n2_pct: List[float] = field(default_factory=lambda: [0.0, 0.0])
    egt_c: List[float] = field(default_factory=lambda: [0.0, 0.0])
    fuel_flow_pph: List[float] = field(default_factory=lambda: [0.0, 0.0])
    oil_press_psi: List[float] = field(default_factory=lambda: [0.0, 0.0])
    oil_temp_c: List[float] = field(default_factory=lambda: [0.0, 0.0])

    # Fuel
    fuel_total_lbs: float = 0.0
    fuel_flow_total_pph: float = 0.0

    # Flight controls
    throttle: List[float] = field(default_factory=lambda: [0.0, 0.0])
    flap_ratio: float = 0.0
    speedbrake_ratio: float = 0.0
    gear_deployed: int = 0
    gear_ratio: float = 0.0

    # Autopilot
    ap_engaged: int = 0
    ap_hdg: float = 0.0
    ap_alt: float = 0.0
    ap_spd: float = 0.0
    ap_vs: float = 0.0
    ap_athr_engaged: int = 0

    # Navigation
    nav1_cdi: float = 0.0
    nav2_cdi: float = 0.0
    nav1_cdi_abs: float = 0.0

    # Environment
    wind_speed_kts: float = 0.0
    wind_dir_deg: float = 0.0
    oat_c: float = 0.0

    # Pressurization
    cabin_alt_ft: float = 0.0
    cabin_diff_psi: float = 0.0

    # Electrical
    elec_bus_volts: float = 0.0

    # Anti-ice
    anti_ice_wing: int = 0
    anti_ice_eng: List[int] = field(default_factory=lambda: [0, 0])

    # Hydraulics
    hyd_press_psi: List[float] = field(default_factory=lambda: [0.0, 0.0])
    hyd_qty_pct: List[float] = field(default_factory=lambda: [0.0, 0.0])

    # APU
    apu_n1_pct: float = 0.0
    apu_egt_c: float = 0.0
    apu_running: int = 0

    # Annunciators
    master_warning: int = 0
    master_caution: int = 0

    # DREF-only digital status (randomly injected for training, read from DREF at runtime)
    fire_eng1: int = 0
    fire_eng2: int = 0
    fire_apu: int = 0
    fire_wheel_well: int = 0
    fire_cargo: int = 0
    tcas_ta: int = 0
    tcas_ra: int = 0
    stall_warning: int = 0     # DREF annunciator (separate from IAS-based STALL)
    door_open: int = 0
    elec_fault: int = 0
    anti_ice_fault: int = 0
    ap_disengage: int = 0
    at_disengage: int = 0

    # Derived / convenience
    roll_deg_abs: float = 0.0
    n1_diff_pct: float = 0.0          # |N1_left - N1_right|
    fuel_imbalance_lbs: float = 0.0   # estimated from N1 diff
    egt_max_c: float = 0.0
    oil_press_min_psi: float = 0.0
    ias_delta_abs: float = 0.0        # |dIAS| over 1s for windshear
    agl_ft: float = 0.0               # alias
    alt_ft: float = 0.0               # alias
    vs: float = 0.0                   # alias


# =============================================================================
# Helper functions
# =============================================================================

def smoothstep(a: float, b: float, t: float) -> float:
    """Hermite blend between a and b."""
    t = max(0.0, min(1.0, t))
    x = t * t * (3.0 - 2.0 * t)
    return a + (b - a) * x


def lerp(a: float, b: float, t: float) -> float:
    """Linear interpolation."""
    return a + (b - a) * t


def gaussian_noise(rng: random.Random, sigma: float) -> float:
    """Generate Gaussian noise with given sigma."""
    return rng.gauss(0.0, sigma)


def clamp(value: float, lo: float, hi: float) -> float:
    """Clamp value to [lo, hi]."""
    return max(lo, min(hi, value))


# =============================================================================
# Flight Profile Generator
# =============================================================================

class FlightProfile:
    """
    Generates a realistic B737-800 flight profile.

    Each instance produces one full flight cycle (310 seconds @ 10 Hz = 3100 frames).
    Multiple instances with different seeds produce the 62,000 frame data pool.
    """

    def __init__(self, seed: int = 42, update_rate_hz: int = 10):
        self.rng = random.Random(seed)
        self.update_rate_hz = update_rate_hz
        self.dt = 1.0 / update_rate_hz

        # Randomized initial conditions
        self.cruise_alt_ft = CRUISE_ALT_FT + self.rng.uniform(-CRUISE_ALT_RANGE_FT, CRUISE_ALT_RANGE_FT)
        self.cruise_ias_kts = CRUISE_IAS_KTS + self.rng.uniform(-10.0, 10.0)
        self.oat_isa_dev_c = self.rng.uniform(-10.0, 10.0)   # ISA deviation
        self.initial_fuel_lbs = FUEL_CAPACITY_LBS * self.rng.uniform(0.90, 1.10)
        self.base_heading = self.rng.uniform(0.0, 360.0)

        # Phase boundaries
        self.phase_start = [0.0] * (PHASE_COUNT + 1)
        self.phase_start[0] = 0.0
        for i in range(PHASE_COUNT):
            self.phase_start[i + 1] = self.phase_start[i] + PHASE_DURATION[i]
        self.cycle_total = self.phase_start[PHASE_COUNT]

        # Pseudo-random phase altitude targets (slight variation)
        self.phase_alt = list(PHASE_ALT_FT)
        self.phase_alt[PHASE_CLIMB2] = self.cruise_alt_ft
        self.phase_alt[PHASE_CRUISE] = self.cruise_alt_ft

    # -------------------------------------------------------------------------
    # Noise helpers
    # -------------------------------------------------------------------------

    def _noise(self, sigma: float) -> float:
        """Gaussian noise."""
        return gaussian_noise(self.rng, sigma)

    def _noisy(self, value: float, range_val: float, frac: float = NOISE_SIGMA_FRAC) -> float:
        """Add noise proportional to parameter range."""
        return value + self._noise(range_val * frac)

    # -------------------------------------------------------------------------
    # Main generation
    # -------------------------------------------------------------------------

    def generate_profile(self) -> List[FlightFrame]:
        """Generate one complete flight profile (3100 frames @ 10 Hz)."""
        frames: List[FlightFrame] = []
        sim_time = 0.0

        # State tracking
        last_ias_kts = 150.0
        fuel_remaining = self.initial_fuel_lbs

        total_frames = int(self.cycle_total * self.update_rate_hz)

        for _ in range(total_frames):
            frame = FlightFrame(sim_time_s=sim_time)
            self._compute_frame(frame, sim_time, last_ias_kts, fuel_remaining)

            # Update persistent state
            last_ias_kts = frame.ias_kts
            fuel_remaining = frame.fuel_total_lbs

            frames.append(frame)
            sim_time += self.dt

        return frames

    def _compute_frame(self, f: FlightFrame, sim_time: float,
                       last_ias_kts: float, fuel_remaining: float):
        """Fill a single FlightFrame with computed values."""

        # --- Phase detection ---
        t_cycle = sim_time % self.cycle_total
        phase_idx = PHASE_CRUISE
        t_phase = t_cycle
        for i in range(PHASE_COUNT):
            if t_cycle < self.phase_start[i + 1]:
                phase_idx = i
                t_phase = t_cycle - self.phase_start[i]
                break

        progress = t_phase / PHASE_DURATION[phase_idx]
        progress = clamp(progress, 0.0, 1.0)

        f.phase = phase_idx
        f.phase_name = PHASE_NAMES[phase_idx]
        f.phase_progress = progress

        # --- Altitude ---
        prev_phase = (phase_idx - 1) if phase_idx > 0 else (PHASE_COUNT - 1)
        alt_start = self.phase_alt[prev_phase]
        alt_end = self.phase_alt[phase_idx]
        f.altitude_ft = smoothstep(alt_start, alt_end, progress)
        f.altitude_ft = self._noisy(f.altitude_ft, 500.0)  # ±15ft noise
        f.altitude_agl_ft = f.altitude_ft  # simplified: airport at sea level
        f.alt_ft = f.altitude_ft
        f.agl_ft = f.altitude_agl_ft

        # --- IAS ---
        ias_start = PHASE_IAS[prev_phase]
        ias_end = PHASE_IAS[phase_idx]
        f.ias_kts = smoothstep(ias_start, ias_end, progress)
        f.ias_kts = self._noisy(f.ias_kts, 20.0, 0.02)  # ±0.4 kts noise

        # --- TAS / GS ---
        f.tas_kts = f.ias_kts * (1.0 + f.altitude_ft * 0.00002)
        f.gs_kts = f.tas_kts + self.rng.uniform(-5.0, 15.0)  # variable wind component

        # --- Mach ---
        if f.altitude_ft > 20000.0:
            f.mach = 0.45 + (f.altitude_ft / CRUISE_ALT_FT) * 0.35
        else:
            f.mach = 0.20 + (f.ias_kts / 300.0) * 0.30
        f.mach = self._noisy(f.mach, 0.05, 0.02)

        # --- Vertical speed (CORRECTED REAL B737 VALUES) ---
        # VS is set to realistic phase-appropriate values, NOT derived from
        # compressed altitude changes (which would give 15800+ fpm).
        f.vs_fpm = self._compute_realistic_vs(phase_idx, progress, alt_start, alt_end)

        # --- Pitch ---
        if phase_idx == PHASE_TAKEOFF and progress > 0.5:
            f.pitch_deg = smoothstep(0.0, 15.0, (progress - 0.5) * 2.0)
        elif f.vs_fpm > 100.0:
            f.pitch_deg = 5.0 + (f.vs_fpm / CLIMB1_VS_FPM) * 10.0
        elif f.vs_fpm < -100.0:
            f.pitch_deg = 2.0 + (f.vs_fpm / DESCENT_VS_FPM) * 5.0
        else:
            f.pitch_deg = 2.5
        f.pitch_deg = self._noisy(f.pitch_deg, 2.0, 0.05)

        # --- Roll ---
        turn_phase = sim_time * 0.3 % (2.0 * math.pi)
        f.roll_deg = math.sin(turn_phase) * 5.0

        # Demo: steep banks during approach to trigger BANK_ANGLE alert
        if phase_idx == PHASE_APPROACH:
            f.roll_deg = math.sin(turn_phase * 1.5) * 40.0
        if phase_idx == PHASE_DESCENT and 0.7 < progress < 0.9:
            f.roll_deg = math.sin(sim_time * 4.0) * 38.0
        if phase_idx in (PHASE_TAXI, PHASE_LANDING):
            f.roll_deg *= 0.1

        f.roll_deg = self._noisy(f.roll_deg, 3.0, 0.05)
        f.roll_deg_abs = abs(f.roll_deg)

        # --- Heading ---
        f.heading_true_deg = (self.base_heading + sim_time * 2.0) % 360.0
        f.heading_mag_deg = (f.heading_true_deg - 5.0) % 360.0
        if f.heading_mag_deg < 0:
            f.heading_mag_deg += 360.0

        # --- Engine parameters ---
        n1_target = self._compute_n1(phase_idx, progress)
        for e in range(2):
            n1 = self._noisy(n1_target, 3.0, 0.03)  # ±0.09% noise
            # Introduce slight asymmetry
            n1 += (e * 0.5 - 0.25)
            f.n1_pct[e] = clamp(n1, IDLE_N1_PCT, MAX_N1_PCT)
            f.n2_pct[e] = f.n1_pct[e] * 0.92
            f.egt_c[e] = 400.0 + (f.n1_pct[e] / 100.0) * 520.0
            f.egt_c[e] = self._noisy(f.egt_c[e], 30.0, 0.05)
            f.fuel_flow_pph[e] = 600.0 + (f.n1_pct[e] / 100.0) * 4000.0
            f.oil_press_psi[e] = 40.0 + (f.n1_pct[e] / 100.0) * 20.0
            f.oil_press_psi[e] = self._noisy(f.oil_press_psi[e], 5.0, 0.03)
            f.oil_temp_c[e] = 60.0 + (f.n1_pct[e] / 100.0) * 60.0
            f.throttle[e] = f.n1_pct[e] / 100.0

        f.n1_diff_pct = abs(f.n1_pct[0] - f.n1_pct[1])
        f.egt_max_c = max(f.egt_c[0], f.egt_c[1])
        f.oil_press_min_psi = min(f.oil_press_psi[0], f.oil_press_psi[1])

        # --- Fuel ---
        f.fuel_flow_total_pph = f.fuel_flow_pph[0] + f.fuel_flow_pph[1]
        fuel_burn_per_frame = f.fuel_flow_total_pph / 3600.0 * self.dt
        f.fuel_total_lbs = fuel_remaining - fuel_burn_per_frame
        if f.fuel_total_lbs < 500.0:
            f.fuel_total_lbs = fuel_remaining  # prevent going below minimum
        f.fuel_total_lbs = self._noisy(f.fuel_total_lbs, 200.0, 0.01)
        f.fuel_imbalance_lbs = f.n1_diff_pct * 200.0  # rough estimate

        # --- Flight controls ---
        self._compute_controls(f, phase_idx, progress)

        # --- Autopilot ---
        f.ap_engaged = 1 if (PHASE_CLIMB2 <= phase_idx <= PHASE_APPROACH) else 0
        f.ap_hdg = f.heading_true_deg
        f.ap_alt = alt_end
        f.ap_spd = f.ias_kts
        f.ap_vs = f.vs_fpm
        f.ap_athr_engaged = f.ap_engaged

        # --- Navigation (CDI) ---
        if phase_idx in (PHASE_APPROACH, PHASE_LANDING):
            f.nav1_cdi = math.sin(sim_time * 0.5) * 0.15
            f.nav2_cdi = math.cos(sim_time * 0.4) * 0.10
        else:
            f.nav1_cdi = 0.0
            f.nav2_cdi = 0.0
        f.nav1_cdi_abs = abs(f.nav1_cdi)

        # --- Environment ---
        f.wind_speed_kts = self._noisy(15.0, 5.0)
        f.wind_dir_deg = self._noisy(270.0, 15.0)
        # ISA standard lapse rate + deviation
        f.oat_c = 15.0 - (f.altitude_ft / 1000.0) * 1.98 + self.oat_isa_dev_c
        f.oat_c = self._noisy(f.oat_c, 2.0, 0.05)

        # --- Pressurization ---
        if f.altitude_ft > 10000.0:
            f.cabin_alt_ft = CABIN_ALT_NOMINAL_FT + math.sin(sim_time * 0.05) * 500.0
            f.cabin_diff_psi = 7.5 + (f.altitude_ft / CRUISE_ALT_FT) * 1.0
            f.cabin_diff_psi = min(f.cabin_diff_psi, CABIN_DIFF_MAX_PSI)
        else:
            f.cabin_alt_ft = f.altitude_ft * 0.3
            f.cabin_diff_psi = (f.altitude_ft / 10000.0) * 5.0
        f.cabin_alt_ft = self._noisy(f.cabin_alt_ft, 200.0, 0.03)

        # --- Electrical ---
        f.elec_bus_volts = ELEC_BUS_NOMINAL_V + math.sin(sim_time * 0.3) * 0.5
        f.elec_bus_volts = self._noisy(f.elec_bus_volts, 1.0, 0.02)

        # --- Anti-ice ---
        f.anti_ice_wing = 1 if (f.altitude_ft > 15000.0 and f.oat_c < 5.0) else 0
        f.anti_ice_eng[0] = 1 if (f.altitude_ft > 10000.0 and f.oat_c < 10.0) else 0
        f.anti_ice_eng[1] = f.anti_ice_eng[0]

        # --- Hydraulics ---
        f.hyd_press_psi[0] = HYD_NOMINAL_PSI + math.sin(sim_time * 2.0) * 50.0
        f.hyd_press_psi[1] = HYD_NOMINAL_PSI + math.cos(sim_time * 2.0) * 50.0
        f.hyd_press_psi[0] = self._noisy(f.hyd_press_psi[0], 50.0, 0.02)
        f.hyd_press_psi[1] = self._noisy(f.hyd_press_psi[1], 50.0, 0.02)
        f.hyd_qty_pct[0] = 0.95 + self._noisy(0.0, 0.02, 0.01)  # nominal 95%
        f.hyd_qty_pct[1] = 0.94 + self._noisy(0.0, 0.02, 0.01)

        # --- APU ---
        if phase_idx == PHASE_TAXI:
            f.apu_n1_pct = APU_N1_PCT
            f.apu_egt_c = APU_EGT_RUNNING_C
            f.apu_running = 1
        elif phase_idx == PHASE_TAKEOFF and progress < 0.2:
            f.apu_n1_pct = smoothstep(APU_N1_PCT, 20.0, progress * 5.0)
            f.apu_egt_c = smoothstep(APU_EGT_RUNNING_C, 300.0, progress * 5.0)
            f.apu_running = 1 if progress < 0.15 else 0
        else:
            f.apu_n1_pct = 0.0
            f.apu_egt_c = 15.0
            f.apu_running = 0

        # --- Annunciators (will be overridden by rule-based generation) ---
        f.master_warning = 0
        f.master_caution = 0

        # --- DREF-only digital status (random injection during training) ---
        f.fire_eng1 = 0
        f.fire_eng2 = 0
        f.fire_apu = 0
        f.fire_wheel_well = 0
        f.fire_cargo = 0
        f.tcas_ta = 0
        f.tcas_ra = 0
        f.stall_warning = 0
        f.door_open = 0
        f.elec_fault = 0
        f.anti_ice_fault = 0
        f.ap_disengage = 0
        f.at_disengage = 0
        f.master_caution = 0
        if phase_idx == PHASE_TAKEOFF and f.flap_ratio < 0.1 and progress > 0.5:
            f.master_caution = 1
        if phase_idx == PHASE_APPROACH and f.gear_deployed == 0 and progress > 0.7:
            f.master_warning = 1
        if f.fuel_total_lbs < 2500.0:
            f.master_caution = 1

        # --- IAS delta (for windshear detection) ---
        f.ias_delta_abs = abs(f.ias_kts - last_ias_kts)

        # --- Aliases ---
        f.vs = f.vs_fpm

    # -------------------------------------------------------------------------
    # Realistic vertical speed computation
    # -------------------------------------------------------------------------

    def _compute_realistic_vs(self, phase_idx: int, progress: float,
                               alt_start: float, alt_end: float) -> float:
        """
        Return a realistic VS (fpm) based on flight phase.

        Uses real B737-800 climb/descent rates, NOT derived from compressed
        altitude deltas (which would produce 15,800+ fpm in the demo profile).

        B737-800 typical values:
          - Initial climb: 2,000–3,000 fpm → target 2,500
          - Cruise climb:  1,500–2,000 fpm → target 1,800
          - Cruise:        0 fpm (level)
          - Descent:       -1,500 to -2,000 fpm → target -1,800
          - Approach:      -700 to -900 fpm → target -800
        """
        noise = self._noise(50.0)  # ±50 fpm base noise

        if phase_idx == PHASE_TAKEOFF:
            if progress < 0.5:
                return 0.0 + noise
            else:
                # Rotate and initial climb: ~2,000 fpm building to 2,500
                return smoothstep(0.0, CLIMB1_VS_FPM, (progress - 0.5) * 2.0) + noise

        elif phase_idx == PHASE_CLIMB1:
            # Initial climb: ~2,500 fpm, tapering near end
            if progress < 0.8:
                return CLIMB1_VS_FPM + noise
            else:
                return smoothstep(CLIMB1_VS_FPM, CLIMB2_VS_FPM, (progress - 0.8) * 5.0) + noise

        elif phase_idx == PHASE_CLIMB2:
            # Climb to cruise: ~1,800 fpm, gradually reducing near cruise
            if progress < 0.7:
                return CLIMB2_VS_FPM + noise
            else:
                return smoothstep(CLIMB2_VS_FPM, 0.0, (progress - 0.7) / 0.3) + noise

        elif phase_idx == PHASE_CRUISE:
            # Level flight — tiny variations only
            return 0.0 + noise * 0.5

        elif phase_idx == PHASE_DESCENT:
            # Descent: -1,800 fpm
            if progress < 0.2:
                return smoothstep(0.0, DESCENT_VS_FPM, progress / 0.2) + noise
            elif progress < 0.8:
                return DESCENT_VS_FPM + noise
            else:
                return smoothstep(DESCENT_VS_FPM, APPROACH_VS_FPM, (progress - 0.8) * 5.0) + noise

        elif phase_idx == PHASE_APPROACH:
            # Approach: -800 fpm glideslope
            return APPROACH_VS_FPM + noise

        elif phase_idx == PHASE_LANDING:
            # Flare: decreasing descent rate
            if progress < 0.3:
                return smoothstep(APPROACH_VS_FPM, -300.0, progress / 0.3) + noise
            elif progress < 0.7:
                return smoothstep(-300.0, -50.0, (progress - 0.3) / 0.4) + noise
            else:
                return 0.0 + noise * 0.3  # Touchdown — nearly zero

        elif phase_idx == PHASE_TAXI:
            return 0.0

        return 0.0

    # -------------------------------------------------------------------------
    # N1 computation
    # -------------------------------------------------------------------------

    def _compute_n1(self, phase_idx: int, progress: float) -> float:
        """Compute target N1 based on flight phase."""
        if phase_idx == PHASE_TAKEOFF:
            return MAX_N1_PCT
        elif phase_idx == PHASE_CLIMB1:
            return smoothstep(MAX_N1_PCT, 92.0, progress)
        elif phase_idx == PHASE_CLIMB2:
            return smoothstep(92.0, CRUISE_N1_PCT, progress)
        elif phase_idx == PHASE_CRUISE:
            return CRUISE_N1_PCT
        elif phase_idx == PHASE_DESCENT:
            return smoothstep(CRUISE_N1_PCT, 45.0, progress)
        elif phase_idx == PHASE_APPROACH:
            return smoothstep(45.0, 55.0, progress)
        elif phase_idx == PHASE_LANDING:
            return smoothstep(55.0, IDLE_N1_PCT, progress)
        elif phase_idx == PHASE_TAXI:
            return smoothstep(IDLE_N1_PCT, CRUISE_N1_PCT, progress)
        return IDLE_N1_PCT

    # -------------------------------------------------------------------------
    # Flight controls computation
    # -------------------------------------------------------------------------

    def _compute_controls(self, f: FlightFrame, phase_idx: int, progress: float):
        """Compute flap, gear, speedbrake positions."""
        if phase_idx == PHASE_TAKEOFF:
            f.flap_ratio = smoothstep(0.25, 0.40, progress)
            f.gear_deployed = 1 if progress < 0.3 else 0
            f.gear_ratio = 1.0 if f.gear_deployed else 0.0
            f.speedbrake_ratio = 0.0

        elif phase_idx == PHASE_CLIMB1:
            f.flap_ratio = smoothstep(0.40, 0.05, progress)
            f.gear_deployed = 0
            f.gear_ratio = 0.0
            f.speedbrake_ratio = 0.0

        elif phase_idx == PHASE_CLIMB2:
            f.flap_ratio = smoothstep(0.05, 0.0, progress)
            f.gear_deployed = 0
            f.speedbrake_ratio = 0.0

        elif phase_idx == PHASE_CRUISE:
            f.flap_ratio = 0.0
            f.gear_deployed = 0
            f.speedbrake_ratio = 0.0

        elif phase_idx == PHASE_DESCENT:
            f.flap_ratio = 0.0
            f.gear_deployed = 0
            f.speedbrake_ratio = smoothstep(0.0, 0.3, (progress - 0.5) * 2.0) if progress > 0.5 else 0.0

        elif phase_idx == PHASE_APPROACH:
            f.flap_ratio = smoothstep(0.0, 0.80, progress)
            # Delay gear to trigger TOO_LOW_GEAR alert
            f.gear_deployed = 1 if progress > 0.75 else 0
            f.gear_ratio = smoothstep(0.0, 1.0, (progress - 0.75) * 4.0) if progress > 0.75 else 0.0
            f.speedbrake_ratio = 0.0

        elif phase_idx == PHASE_LANDING:
            f.flap_ratio = 0.80
            f.gear_deployed = 1
            f.gear_ratio = 1.0
            f.speedbrake_ratio = 1.0 if progress > 0.3 else 0.0

        elif phase_idx == PHASE_TAXI:
            f.flap_ratio = smoothstep(0.80, 0.05, progress)
            f.gear_deployed = 1
            f.gear_ratio = 1.0
            f.speedbrake_ratio = smoothstep(1.0, 0.0, progress)

        # Add noise to flap and gear
        f.flap_ratio = clamp(f.flap_ratio + self._noise(0.02), 0.0, 1.0)
        f.gear_ratio = clamp(f.gear_ratio + self._noise(0.01), 0.0, 1.0)


# =============================================================================
# Utility: generate full data pool
# =============================================================================

def generate_frame_pool(num_profiles: int = 20, update_rate_hz: int = 10,
                        base_seed: int = 42) -> List[FlightFrame]:
    """
    Generate the full frame data pool.

    Args:
        num_profiles: Number of profile variations (default 20).
        update_rate_hz: Frames per second (default 10 Hz).
        base_seed: Random seed for the first profile (incremented per profile).

    Returns:
        List of FlightFrame objects (~62,000 frames for 20 profiles @ 10 Hz).
    """
    all_frames = []
    for i in range(num_profiles):
        seed = base_seed + i * 100
        profile = FlightProfile(seed=seed, update_rate_hz=update_rate_hz)
        frames = profile.generate_profile()
        all_frames.extend(frames)
        print(f"  Profile {i+1:2d}/{num_profiles}: {len(frames)} frames (seed={seed})")
    return all_frames


# =============================================================================
# Self-test
# =============================================================================

if __name__ == "__main__":
    print("=== Flight Profile Simulator — Quick Test ===\n")

    profile = FlightProfile(seed=42)
    frames = profile.generate_profile()

    print(f"Generated {len(frames)} frames @ {profile.update_rate_hz} Hz")
    print(f"Cycle length: {profile.cycle_total:.0f}s\n")

    # Print sample frames from each phase
    printed_phases = set()
    for f in frames:
        if f.phase not in printed_phases:
            printed_phases.add(f.phase)
            print(f"[{f.phase_name:>8}] t={f.sim_time_s:6.1f}s  "
                  f"ALT={f.altitude_ft:7.0f}ft  AGL={f.altitude_agl_ft:7.0f}ft  "
                  f"IAS={f.ias_kts:5.0f}kts  VS={f.vs_fpm:6.0f}fpm  "
                  f"N1=({f.n1_pct[0]:5.1f}%, {f.n1_pct[1]:5.1f}%)  "
                  f"Flap={f.flap_ratio:.2f}  Gear={f.gear_deployed}  "
                  f"Roll={f.roll_deg:5.1f}°")

    # Verify key corrections
    print("\n=== Verification ===")
    climb_frame = next(f for f in frames if f.phase == PHASE_CLIMB1 and f.phase_progress > 0.5)
    print(f"Climb VS: {climb_frame.vs_fpm:.0f} fpm (should be ~2500, NOT 15800)")

    descent_frame = next(f for f in frames if f.phase == PHASE_DESCENT and f.phase_progress > 0.5)
    print(f"Descent VS: {descent_frame.vs_fpm:.0f} fpm (should be ~-1800, NOT -23000)")

    max_n1 = max(f.n1_pct[0] for f in frames)
    print(f"Max N1: {max_n1:.1f}% (should be ~98%)")

    print("\nDone.")
