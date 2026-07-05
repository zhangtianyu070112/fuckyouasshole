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

#endif /* ALERT_SYSTEM_H */
