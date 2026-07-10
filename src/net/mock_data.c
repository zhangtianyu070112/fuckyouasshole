/**
 * @file    mock_data.c
 * @brief   Mock flight data generator — simulates a complete B737-style
 *          flight profile looping continuously.
 *
 * Flight phases (auto-looping):
 *   TAKEOFF   (0–10s)   — Full thrust, rotate, gear up
 *   CLIMB1    (10–40s)  — Initial climb, flaps retract
 *   CLIMB2    (40–90s)  — Climb to cruise altitude
 *   CRUISE    (90–160s) — Level at FL350, Mach 0.78
 *   DESCENT   (160–220s)— Descend, reduce speed
 *   APPROACH  (220–280s)— Gear down, flaps out, descend on glideslope
 *   LANDING   (280–300s)— Flare, touchdown, rollout
 *   TAXI      (300–310s)— Decelerate, then reset to cruise
 *   (loop back to CRUISE at phase end)
 *
 * All values are loosely based on a Boeing 737-800 flight envelope.
 */

#include "mock_data.h"
#include "utils/logger.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* =========================================================================
 *  Flight profile constants
 * ========================================================================= */

#define CRUISE_ALT_FT      35000.0f
#define CRUISE_IAS_KTS     280.0f
#define CRUISE_MACH        0.78f
#define APPROACH_IAS_KTS   140.0f
#define LANDING_IAS_KTS    125.0f
#define MAX_N1_PCT         96.0f
#define IDLE_N1_PCT        22.0f
#define CRUISE_N1_PCT      60.0f
#define CLIMB_VS_FPM       2500.0f
#define DESCENT_VS_FPM    -1800.0f
#define APPROACH_VS_FPM   -800.0f

/* =========================================================================
 *  Phase timing (seconds, cumulative)
 * ========================================================================= */

typedef enum {
    PHASE_TAKEOFF,
    PHASE_CLIMB1,
    PHASE_CLIMB2,
    PHASE_CRUISE,
    PHASE_DESCENT,
    PHASE_APPROACH,
    PHASE_LANDING,
    PHASE_TAXI,
    PHASE_COUNT
} FlightPhase;

static const float phase_duration[PHASE_COUNT] = {
    10.0f,   /* TAKEOFF   — short, dramatic */
    30.0f,   /* CLIMB1    — initial climb */
    50.0f,   /* CLIMB2    — climb to cruise */
    70.0f,   /* CRUISE    — level flight */
    60.0f,   /* DESCENT   — descend */
    60.0f,   /* APPROACH  — approach + landing config */
    20.0f,   /* LANDING   — flare + rollout */
    10.0f,   /* TAXI      — brief taxi then loop back */
};

static const float phase_alt_ft[PHASE_COUNT] = {
    100.0f,       /* TAKEOFF end   — just airborne */
    8000.0f,      /* CLIMB1 end    — passing 8000 */
    CRUISE_ALT_FT,/* CLIMB2 end    — cruise level */
    CRUISE_ALT_FT,/* CRUISE end    — still level */
    12000.0f,     /* DESCENT end   — mid descent */
    2000.0f,      /* APPROACH end  — on final */
    0.0f,         /* LANDING end   — on ground */
    0.0f,         /* TAXI end      — on ground */
};

static const float phase_ias[PHASE_COUNT] = {
    150.0f,       /* TAKEOFF  — V2+10 */
    210.0f,       /* CLIMB1   — accelerating */
    CRUISE_IAS_KTS,/* CLIMB2  — cruise speed */
    CRUISE_IAS_KTS,/* CRUISE  — level */
    260.0f,       /* DESCENT  — still fast */
    APPROACH_IAS_KTS,/* APPROACH — slowing */
    LANDING_IAS_KTS, /* LANDING — Vref */
    40.0f,         /* TAXI     — taxi speed */
};

/* =========================================================================
 *  Smooth interpolation helper
 * ========================================================================= */

/**
 * @brief Smoothstep (Hermite) interpolation between a and b.
 * @param t  Normalized progress 0..1.
 */
static float smoothstep(float a, float b, float t)
{
    float x = t * t * (3.0f - 2.0f * t);  /* Hermite blend */
    return a + (b - a) * x;
}

/**
 * @brief Linear interpolation.
 */
static float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

/* =========================================================================
 *  Main mock data generation
 * ========================================================================= */

MockDataCtx* mock_data_create(FlightData* fd, int update_rate_hz)
{
    MockDataCtx* ctx = calloc(1, sizeof(MockDataCtx));
    if (!ctx) {
        LOG_ERROR("Out of memory allocating MockDataCtx");
        return NULL;
    }
    ctx->flight_data    = fd;
    ctx->update_rate_hz = (update_rate_hz > 0) ? update_rate_hz : 20;
    ctx->running        = 1;
    LOG_INFO("MockData generator created (%d Hz)", ctx->update_rate_hz);
    return ctx;
}

void mock_data_free(MockDataCtx* ctx)
{
    if (ctx) free(ctx);
}

int mock_data_thread(void* userdata)
{
    MockDataCtx* ctx = (MockDataCtx*)userdata;
    if (!ctx || !ctx->flight_data) return -1;

    const int   rate_hz   = ctx->update_rate_hz;
    const float dt        = 1.0f / (float)rate_hz;
    const float phase_len = 30.0f;  /* seconds per phase on average… no,
                                       actually use the phase_duration array */

    /* Compute total cycle length and phase boundaries */
    float phase_start[PHASE_COUNT + 1];
    phase_start[0] = 0.0f;
    for (int i = 0; i < PHASE_COUNT; i++) {
        phase_start[i + 1] = phase_start[i] + phase_duration[i];
    }
    float cycle_total = phase_start[PHASE_COUNT];

    float sim_time = 0.0f; /* simulation time — wraps around */

    /* Initial heading (randomized per session for variety) */
    float base_heading = 180.0f; /* roughly southbound departure */

    LOG_INFO("MockData thread: cycle=%.0fs, phases=%d, dt=%.3fs",
             (double)cycle_total, PHASE_COUNT, (double)dt);

    while (ctx->running) {
        /* Determine current phase and intra-phase progress */
        float t_cycle = fmodf(sim_time, cycle_total);

        int   phase_idx = 0;
        float t_phase   = t_cycle;
        for (int i = 0; i < PHASE_COUNT; i++) {
            if (t_cycle < phase_start[i + 1]) {
                phase_idx = i;
                t_phase   = t_cycle - phase_start[i];
                break;
            }
        }
        float progress = t_phase / phase_duration[phase_idx];
        if (progress > 1.0f) progress = 1.0f;

        /* --- Build FlightDataValues --- */
        FlightDataValues d;
        memset(&d, 0, sizeof(d));

        /* Interpolate altitude */
        float alt_start, alt_end;
        int prev_phase = (phase_idx == 0) ? (PHASE_COUNT - 1) : (phase_idx - 1);
        alt_start = phase_alt_ft[prev_phase];
        alt_end   = phase_alt_ft[phase_idx];
        d.altitude_ft     = smoothstep(alt_start, alt_end, progress);
        d.alt_msl_ft      = d.altitude_ft;
        d.altitude_agl_ft = d.altitude_ft;  /* simplified: airport at sea level */

        /* Interpolate IAS */
        float ias_start = phase_ias[prev_phase];
        float ias_end   = phase_ias[phase_idx];
        d.ias_kts = smoothstep(ias_start, ias_end, progress);
        d.tas_kts = d.ias_kts * (1.0f + d.altitude_ft * 0.00002f); /* rough TAS */
        d.gs_kts  = d.tas_kts + 10.0f; /* slight tailwind */

        /* Mach */
        d.mach = (d.altitude_ft > 20000.0f)
                     ? 0.45f + (d.altitude_ft / CRUISE_ALT_FT) * 0.35f
                     : 0.20f + (d.ias_kts / 300.0f) * 0.30f;

        /* Vertical speed — derivative of altitude trend */
        {
            float d_alt = alt_end - alt_start;
            float duration = phase_duration[phase_idx];
            if (duration > 0.01f) {
                d.vs_fpm = (d_alt / duration) * 60.0f; /* ft/min */
            }
            /* Smooth transitions at phase boundaries */
            if (progress < 0.1f) {
                float prev_d_alt = alt_start;
                if (prev_phase < phase_idx) {
                    /* entering new phase — blend */
                }
            }
        }

        /* Pitch — based on vertical speed */
        if (phase_idx == PHASE_TAKEOFF && progress > 0.5f) {
            d.pitch_deg = smoothstep(0.0f, 15.0f, (progress - 0.5f) * 2.0f);
        } else if (d.vs_fpm > 100.0f) {
            d.pitch_deg = 5.0f + (d.vs_fpm / CLIMB_VS_FPM) * 10.0f;
        } else if (d.vs_fpm < -100.0f) {
            d.pitch_deg = 2.0f + (d.vs_fpm / DESCENT_VS_FPM) * 5.0f;
        } else {
            d.pitch_deg = 2.5f; /* cruise attitude */
        }

        /* Roll — gentle banks during turns, wing level mostly */
        {
            float turn_phase = fmodf(sim_time * 0.3f, 2.0f * (float)M_PI);
            d.roll_deg = sinf(turn_phase) * 5.0f;
            /* Demo: aggressive bank during approach to trigger BANK ANGLE alert */
            if (phase_idx == PHASE_APPROACH) {
                /* Occasional steep turns (reaches 40° at sin peak) */
                d.roll_deg = sinf(turn_phase * 1.5f) * 40.0f;
            }
            /* Demo: brief steep bank at end of descent for alert testing */
            if (phase_idx == PHASE_DESCENT && progress > 0.7f && progress < 0.9f) {
                d.roll_deg = sinf(sim_time * 4.0f) * 38.0f;
            }
            if (phase_idx == PHASE_TAXI || phase_idx == PHASE_LANDING) {
                d.roll_deg *= 0.1f;
            }
        }

        /* Heading — gradual turn to simulate navigation */
        d.heading_true_deg = fmodf(base_heading + sim_time * 2.0f, 360.0f);
        d.heading_mag_deg  = fmodf(d.heading_true_deg - 5.0f, 360.0f);
        if (d.heading_mag_deg < 0.0f) d.heading_mag_deg += 360.0f;

        /* Position — simulate ~N30° E120° area (Shanghai region) */
        {
            double start_lat = 31.2;
            double start_lon = 121.4;
            float  gs_ms     = d.gs_kts * 0.51444f; /* knots → m/s */
            float  heading_rad = d.heading_true_deg * (float)M_PI / 180.0f;
            float  dist_m     = gs_ms * sim_time;
            /* crude lat/lon update */
            double dlat = (double)(cosf(heading_rad) * gs_ms * dt / 111320.0);
            double dlon = (double)(sinf(heading_rad) * gs_ms * dt
                                   / (111320.0 * cos(start_lat * M_PI / 180.0)));
            /* Start fresh each cycle to prevent drift */
            d.lat_deg = start_lat + dlat * (double)(fmodf(sim_time, 600.0f));
            d.lon_deg = start_lon + dlon * (double)(fmodf(sim_time, 600.0f));
        }

        /* --- Engine parameters (2 engines, twin-jet like 737) --- */
        {
            float n1_cruise = CRUISE_N1_PCT;
            float n1_takeoff = MAX_N1_PCT;
            float n1_idle = IDLE_N1_PCT;
            float n1;

            switch (phase_idx) {
            case PHASE_TAKEOFF:
                n1 = n1_takeoff;
                break;
            case PHASE_CLIMB1:
                n1 = smoothstep(n1_takeoff, 92.0f, progress);
                break;
            case PHASE_CLIMB2:
                n1 = smoothstep(92.0f, n1_cruise, progress);
                break;
            case PHASE_CRUISE:
                n1 = n1_cruise;
                break;
            case PHASE_DESCENT:
                n1 = smoothstep(n1_cruise, 45.0f, progress);
                break;
            case PHASE_APPROACH:
                n1 = smoothstep(45.0f, 55.0f, progress);
                break;
            case PHASE_LANDING:
                n1 = smoothstep(55.0f, n1_idle, progress);
                break;
            case PHASE_TAXI:
                n1 = smoothstep(n1_idle, n1_cruise, progress); /* spool back up */
                break;
            default:
                n1 = n1_idle;
            }

            for (int e = 0; e < 2; e++) {
                d.n1_pct[e]    = n1 + ((float)((e * 3) % 5) - 2.0f) * 0.3f; /* slight asymmetry */
                d.n2_pct[e]    = n1 * 0.92f;  /* N2 slightly lower than N1 */
                d.egt_c[e]     = 400.0f + (n1 / 100.0f) * 500.0f;
                d.fuel_flow_pph[e] = 600.0f + (n1 / 100.0f) * 4000.0f;
                d.oil_press_psi[e] = 40.0f + (n1 / 100.0f) * 20.0f;
                d.oil_temp_c[e]    = 60.0f + (n1 / 100.0f) * 60.0f;
                d.throttle[e]  = n1 / 100.0f;
            }
            d.fuel_flow_total_pph = d.fuel_flow_pph[0] + d.fuel_flow_pph[1];
            d.fuel_total_lbs      = 15000.0f - sim_time * 15.0f; /* fuel burn */
            if (d.fuel_total_lbs < 1000.0f) d.fuel_total_lbs = 15000.0f; /* reset per loop */
            /* Per-tank distribution (B737: ~46% left wing, ~8% center, ~46% right wing) */
            {
                float remaining = d.fuel_total_lbs;
                d.fuel_tank_lbs[0] = remaining * 0.46f;  /* left wing  */
                d.fuel_tank_lbs[1] = remaining * 0.08f;  /* center     */
                d.fuel_tank_lbs[2] = remaining * 0.46f;  /* right wing */
            }
        }

        /* --- Flight controls --- */
        {
            switch (phase_idx) {
            case PHASE_TAKEOFF:
                d.flap_ratio        = smoothstep(0.25f, 0.4f, progress);
                d.gear_deployed     = (progress < 0.3f) ? 1 : 0;
                d.gear_ratio        = d.gear_deployed ? 1.0f : 0.0f;
                d.speedbrake_ratio  = 0.0f;
                break;
            case PHASE_CLIMB1:
                d.flap_ratio        = smoothstep(0.40f, 0.05f, progress);
                d.gear_deployed     = 0;
                d.gear_ratio        = 0.0f;
                d.speedbrake_ratio  = 0.0f;
                break;
            case PHASE_CLIMB2:
                d.flap_ratio        = smoothstep(0.05f, 0.0f, progress);
                d.gear_deployed     = 0;
                d.speedbrake_ratio  = 0.0f;
                break;
            case PHASE_CRUISE:
                d.flap_ratio        = 0.0f;
                d.gear_deployed     = 0;
                d.speedbrake_ratio  = 0.0f;
                break;
            case PHASE_DESCENT:
                d.flap_ratio        = 0.0f;
                d.gear_deployed     = 0;
                d.speedbrake_ratio  = (progress > 0.5f) ? smoothstep(0.0f, 0.3f, (progress - 0.5f) * 2.0f) : 0.0f;
                break;
            case PHASE_APPROACH:
                d.flap_ratio        = smoothstep(0.0f, 0.8f, progress);
                /* Demo: delay gear until AGL < 500ft to trigger TOO LOW GEAR alert */
                d.gear_deployed     = (progress > 0.75f) ? 1 : 0;
                d.gear_ratio        = (progress > 0.75f) ? smoothstep(0.0f, 1.0f, (progress - 0.75f) * 4.0f) : 0.0f;
                d.speedbrake_ratio  = 0.0f;
                break;
            case PHASE_LANDING:
                d.flap_ratio        = 0.8f;
                d.gear_deployed     = 1;
                d.gear_ratio        = 1.0f;
                d.speedbrake_ratio  = (progress > 0.3f) ? 1.0f : 0.0f; /* spoilers on touchdown */
                break;
            case PHASE_TAXI:
                d.flap_ratio        = smoothstep(0.80f, 0.05f, progress);
                d.gear_deployed     = 1;
                d.gear_ratio        = 1.0f;
                d.speedbrake_ratio  = smoothstep(1.0f, 0.0f, progress);
                break;
            }

            d.elevator_deg = -d.pitch_deg * 0.3f;
            d.aileron_deg  = d.roll_deg * 0.2f;
            d.rudder_deg   = 0.0f;
        }

        /* --- Brakes --- */
        d.parking_brake = (phase_idx == PHASE_TAXI && progress < 0.3f) ? 1 : 0;
        if (phase_idx == PHASE_LANDING && progress > 0.5f) {
            d.brake_temp_c[0] = 300.0f + progress * 200.0f;  /* Hot after landing */
            d.brake_temp_c[1] = 310.0f + progress * 190.0f;
        } else if (phase_idx == PHASE_TAKEOFF && progress < 0.3f) {
            d.brake_temp_c[0] = 200.0f;  /* Warm after takeoff roll */
            d.brake_temp_c[1] = 200.0f;
        } else {
            d.brake_temp_c[0] = 50.0f + sinf(sim_time * 0.1f) * 15.0f;  /* Cool */
            d.brake_temp_c[1] = 55.0f + cosf(sim_time * 0.1f) * 15.0f;
        }

        /* --- Autopilot (engaged in cruise and descent) --- */
        d.ap_engaged      = (phase_idx >= PHASE_CLIMB2 && phase_idx <= PHASE_APPROACH) ? 1 : 0;
        d.ap_hdg          = d.heading_true_deg;
        d.ap_alt          = alt_end;
        d.ap_spd          = d.ias_kts;
        d.ap_vs           = d.vs_fpm;
        d.ap_athr_engaged = d.ap_engaged;

        /* --- Navigation --- */
        d.nav1_freq   = 110.90f;  /* ILS freq */
        d.nav2_freq   = 113.70f;
        d.nav1_course = d.heading_true_deg;
        d.nav2_course = d.heading_true_deg + 15.0f;
        d.nav1_radial = d.heading_true_deg;
        d.nav2_radial = fmodf(d.heading_true_deg + 90.0f, 360.0f);
        /* CDI: centered in cruise, drifting on approach for realism */
        if (phase_idx == PHASE_APPROACH || phase_idx == PHASE_LANDING) {
            d.nav1_cdi = sinf(sim_time * 0.5f) * 0.15f;
            d.nav2_cdi = cosf(sim_time * 0.4f) * 0.10f;
        } else {
            d.nav1_cdi = 0.0f;
            d.nav2_cdi = 0.0f;
        }
        d.dme_dist_nm  = (phase_idx <= PHASE_CRUISE) ? 50.0f - sim_time * 0.1f : 5.0f;
        if (d.dme_dist_nm < 0.0f) d.dme_dist_nm = 50.0f;
        d.dme_speed_kts = d.gs_kts;

        /* --- COM Radio --- */
        d.com1_freq = 122.800f;  /* UNICOM */
        d.com2_freq = 121.500f;  /* Emergency / Guard */

        /* --- Transponder --- */
        d.xpdr_code = 1200;
        d.xpdr_mode = 3;          /* Mode C / ALT */

        /* --- Environment --- */
        d.wind_speed_kts = 15.0f;
        d.wind_dir_deg   = 270.0f;
        d.oat_c          = 15.0f - (d.altitude_ft / 1000.0f) * 1.98f; /* ISA standard lapse rate */
        d.oat_isa_dev_c  = 0.0f;

        /* --- Pressurization --- */
        if (d.altitude_ft > 10000.0f) {
            d.cabin_alt_ft   = 6000.0f + sinf(sim_time * 0.05f) * 500.0f;
            d.cabin_diff_psi = 7.5f + (d.altitude_ft / CRUISE_ALT_FT) * 1.0f;
            if (d.cabin_diff_psi > 8.3f) d.cabin_diff_psi = 8.3f;
        } else {
            d.cabin_alt_ft   = d.altitude_ft * 0.3f;
            d.cabin_diff_psi = (d.altitude_ft / 10000.0f) * 5.0f;
        }

        /* --- Electrical --- */
        d.elec_bus_volts   = 28.0f + sinf(sim_time * 0.3f) * 0.5f;  /* 28V DC bus */
        {
            float n1_avg = (d.n1_pct[0] + d.n1_pct[1]) / 200.0f;  /* 0..1 */
            d.elec_gen_amps[0] = 60.0f + n1_avg * 80.0f;            /* 60-140A */
            d.elec_gen_amps[1] = 60.0f + n1_avg * 80.0f;
        }

        /* --- Anti-ice --- */
        d.anti_ice_wing   = (d.altitude_ft > 15000.0f && d.oat_c < 5.0f) ? 1 : 0;
        d.anti_ice_eng[0] = (d.altitude_ft > 10000.0f && d.oat_c < 10.0f) ? 1 : 0;
        d.anti_ice_eng[1] = d.anti_ice_eng[0];

        /* --- Hydraulics --- */
        d.hyd_press_psi[0] = 3000.0f + sinf(sim_time * 2.0f) * 50.0f;  /* System A */
        d.hyd_press_psi[1] = 3000.0f + cosf(sim_time * 2.0f) * 50.0f;  /* System B */
        d.hyd_qty_pct[0]   = 95.0f;
        d.hyd_qty_pct[1]   = 92.0f;

        /* --- APU --- */
        if (phase_idx == PHASE_TAXI) {
            d.apu_n1_pct  = 98.0f;
            d.apu_egt_c   = 550.0f;
            d.apu_running = 1;
        } else if (phase_idx == PHASE_TAKEOFF && progress < 0.2f) {
            d.apu_n1_pct  = smoothstep(98.0f, 20.0f, progress * 5.0f);
            d.apu_egt_c   = smoothstep(550.0f, 300.0f, progress * 5.0f);
            d.apu_running = (progress < 0.15f) ? 1 : 0;
        } else {
            d.apu_n1_pct  = 0.0f;
            d.apu_egt_c   = 15.0f;
            d.apu_running = 0;
        }

        /* --- Annunciators --- */
        d.master_warning = 0;
        d.master_caution = 0;
        /* Trigger master caution for config warnings */
        if (phase_idx == PHASE_TAKEOFF && d.flap_ratio < 0.1f && progress > 0.5f)
            d.master_caution = 1;  /* Takeoff config warning */
        if (phase_idx == PHASE_APPROACH && d.gear_deployed == 0 && progress > 0.7f)
            d.master_warning = 1;  /* Gear not down */
        if (d.fuel_total_lbs < 2500.0f && d.fuel_total_lbs > 0.0f)
            d.master_caution = 1;  /* Low fuel */

        /* --- Timestamp --- */
        d.last_update_ticks = SDL_GetTicks();

        /* Push to shared flight data */
        flight_data_update(ctx->flight_data, &d);

        /* Sleep for one tick */
        SDL_Delay((Uint32)(dt * 1000.0f));
        sim_time += dt;

        /* Safety: if running flag cleared externally */
        /* checked via the SDL_Delay above — no busy loop */
    }

    LOG_INFO("MockData thread exiting (running=%d)", ctx->running);
    return 0;
}
