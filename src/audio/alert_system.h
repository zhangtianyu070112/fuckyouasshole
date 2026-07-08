/**
 * @file    alert_system.h
 * @brief   GPWS-style audio alert system using SDL2 audio.
 *
 * Synthesizes alert tones in real-time based on flight data conditions.
 * No external audio files or additional libraries required — uses SDL2's
 * built-in audio API to generate square/sine wave tones.
 *
 * Alert types (priority order, highest first):
 *   PULL_UP      — AGL < 100ft + excessive sink rate  (>2000 fpm)
 *   WINDSHEAR    — Sudden airspeed change             (>20 kts in 1s)
 *   TERRAIN      — AGL < 1000ft + high closure rate   (>1500 fpm descent)
 *   SINK_RATE    — AGL < 2500ft + sink rate            (>1500 fpm)
 *   TOO_LOW_GEAR — AGL < 500ft + gear not down
 *   TOO_LOW_FLAPS— AGL < 500ft + flaps < approach config
 *   GLIDESLOPE   — CDI > 0.5 dots + AGL < 1000ft
 *   BANK_ANGLE   — |roll| > 35° + AGL < 2000ft
 *   OVERSPEED    — IAS > 340 kts
 *   MINIMUMS     — AGL crossing decision height (200ft)
 *
 * Plus altitude callouts at: 500, 400, 300, 200, 100, 50, 40, 30, 20, 10 ft
 *
 * Usage:
 *   AlertSystem* as = alert_system_create();
 *   // each frame:
 *   alert_system_update(as, &flight_data_snapshot, delta_time);
 *   // on shutdown:
 *   alert_system_destroy(as);
 */

#ifndef ALERT_SYSTEM_H
#define ALERT_SYSTEM_H

#include "data/flight_data.h"

/* Opaque handle */
typedef struct AlertSystem AlertSystem;

/* --- Lifecycle ----------------------------------------------------------- */

/**
 * @brief Create and initialize the alert system (opens SDL audio device).
 * @return AlertSystem handle, or NULL on failure (gracefully degrades —
 *         alert_system_update becomes a no-op).
 */
AlertSystem* alert_system_create(void);

/**
 * @brief Destroy the alert system and close the audio device.
 */
void alert_system_destroy(AlertSystem* as);

/* --- Per-frame update ---------------------------------------------------- */

/**
 * @brief Evaluate alert conditions against current flight data and trigger
 *        audio alerts as needed. Call once per frame from the main thread.
 *
 * @param as  Alert system handle.
 * @param fd  Current flight data snapshot.
 * @param dt  Delta time since last frame (seconds).
 */
void alert_system_update(AlertSystem* as, const FlightDataValues* fd, float dt);

/* --- Configuration ------------------------------------------------------- */

/**
 * @brief Enable or disable the alert system at runtime.
 * @param enabled  0 = mute all alerts, 1 = normal operation.
 */
void alert_system_set_enabled(AlertSystem* as, int enabled);

/**
 * @brief Check if the alert system's audio device is functional.
 * @return 1 if audio is available, 0 if silently degrading.
 */
int alert_system_audio_ok(const AlertSystem* as);

/* --- Alert state query (for AI / external consumers) ----------------------- */

/**
 * @brief Total number of alert types tracked by the system.
 */
#define ALERT_ID_COUNT  39   /* 13 GPWS + 13 system + 13 DREF */

/**
 * @brief Alert identifiers — one per alert type, matches Python alert_rules.json.
 *
 * Ordering: GPWS (13) → System deviations (13) → DREF-only (13)
 */
typedef enum {
    /* GPWS (13) — priorities 0..12, corresponds to internal AlertType */
    ALERT_ID_PULL_UP = 0,
    ALERT_ID_WINDSHEAR,
    ALERT_ID_MASTER_WARNING,
    ALERT_ID_MASTER_CAUTION,
    ALERT_ID_TERRAIN,
    ALERT_ID_SINK_RATE,
    ALERT_ID_TOO_LOW_GEAR,
    ALERT_ID_TOO_LOW_FLAPS,
    ALERT_ID_GLIDESLOPE,
    ALERT_ID_BANK_ANGLE,
    ALERT_ID_OVERSPEED,
    ALERT_ID_STALL,
    ALERT_ID_MINIMUMS,
    /* System deviations (13) */
    ALERT_ID_ENG_OVERHEAT,
    ALERT_ID_ENG_ASYM,
    ALERT_ID_FUEL_IMBALANCE,
    ALERT_ID_OIL_PRESS_LOW,
    ALERT_ID_CABIN_ALT_HIGH,
    ALERT_ID_BUS_VOLT_ABNORM,
    ALERT_ID_TAKEOFF_CONFIG,
    ALERT_ID_LOW_FUEL,
    ALERT_ID_ICING_CONDITION,
    ALERT_ID_APU_FIRE,
    ALERT_ID_OIL_TEMP_HIGH,
    ALERT_ID_HYD_PRESS_LOW,
    ALERT_ID_HYD_QTY_LOW,
    /* DREF-only (13) — read directly from FlightDataValues.dref_* fields */
    ALERT_ID_FIRE_ENG1,
    ALERT_ID_FIRE_ENG2,
    ALERT_ID_FIRE_APU,
    ALERT_ID_FIRE_WHEEL_WELL,
    ALERT_ID_FIRE_CARGO,
    ALERT_ID_TCAS_TA,
    ALERT_ID_TCAS_RA,
    ALERT_ID_STALL_WARNING,
    ALERT_ID_DOOR_OPEN,
    ALERT_ID_ELEC_FAULT,
    ALERT_ID_ANTI_ICE_FAULT,
    ALERT_ID_AP_DISENGAGE,
    ALERT_ID_AT_DISENGAGE,
} AlertID;

/**
 * @brief Get the active state of all 36 alert types.
 *
 * Combines two sources:
 *   1. Threshold-based checks (UDP DATA parameters) — GPWS + system deviations
 *   2. DREF alert state bits (from RREF subscriptions) — fire, TCAS, doors, etc.
 *
 * @param as         Alert system handle.
 * @param fd         Current flight data snapshot.
 * @param out_states Output array of ALERT_ID_COUNT ints, each 0 or 1.
 *                   Caller allocates (int[ALERT_ID_COUNT]).
 */
void alert_system_get_active_alerts(const AlertSystem* as,
                                    const FlightDataValues* fd,
                                    int* out_states);

/**
 * @brief Test function — play distinctive tone for every active alert.
 *
 * Calls alert_system_get_active_alerts() and triggers a unique audio tone
 * for each active alert type. Each of the 39 alert types has a distinctive
 * frequency/rhythm pattern so you can identify which alert is firing
 * by ear. Useful for verifying RREF/DREF subscriptions against X-Plane.
 *
 * Call this INSTEAD OF alert_system_update() during RREF testing.
 * In production, call alert_system_update() for the normal GPWS audio
 * (which has more sophisticated tone synthesis).
 *
 * @param as  Alert system handle.
 * @param fd  Current flight data snapshot.
 */
void alert_system_test_beeps(AlertSystem* as, const FlightDataValues* fd);

#endif /* ALERT_SYSTEM_H */
