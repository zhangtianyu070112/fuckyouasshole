/**
 * @file    data_flywheel.h
 * @brief   AI Copilot Data Flywheel — continuous flight data capture engine.
 *
 * Implements a three-layer automatic data collection strategy for the
 * AI Co-pilot training pipeline:
 *
 *   L1 (Auto-trigger) — Alert events → capture pre/post windows → auto-label
 *   L2 (Low-confidence) — Model uncertainty → capture context → queue for review
 *   L3 (Random sampling) — Periodic stratified snapshots → diversity pool
 *
 * Architecture:
 *   - Ring buffer (configurable, default 60s at 20 Hz = 1200 frames)
 *   - Continuous background recording — no per-frame allocation
 *   - Deferred finalization for post-trigger windows (L1/L2)
 *   - JSON output format compatible with existing training data pipeline
 *
 * Integration points:
 *   - alert_system.c → flywheel_alert_trigger()       (L1)
 *   - ai_advisor.c  → flywheel_confidence_trigger()   (L2)
 *   - app.c main loop → flywheel_update() each frame  (all layers)
 *
 * All public functions are designed to be called from the main render loop
 * at 60 Hz. The ring buffer write is O(1); capture finalization is O(window)
 * but amortized over many frames.
 *
 * This module is designed as FUTURE WORK — the architecture is complete and
 * compilable, but the downstream training pipeline integration (QLoRA merge,
 * human review UI) is described in docs/数据飞轮系统设计方案.md.
 */

#ifndef DATA_FLYWHEEL_H
#define DATA_FLYWHEEL_H

#include <stdint.h>
#include "data/flight_data.h"
#include "audio/alert_system.h"

/* Opaque handle */
typedef struct DataFlywheel DataFlywheel;

/* =========================================================================
 *  Capture source identifiers
 * ========================================================================= */

typedef enum {
    FLYWHEEL_L1_AUTO_ALERT     = 0,  /* Alert-triggered, auto-labeled */
    FLYWHEEL_L2_LOW_CONFIDENCE = 1,  /* Low AI confidence, needs review */
    FLYWHEEL_L3_RANDOM_SAMPLE  = 2,  /* Periodic random sampling */
} FlywheelSource;

/* =========================================================================
 *  Flight phase heuristic (used for metadata tagging)
 * ========================================================================= */

typedef enum {
    FLIGHT_PHASE_UNKNOWN   = 0,
    FLIGHT_PHASE_GROUND    = 1,
    FLIGHT_PHASE_TAKEOFF   = 2,
    FLIGHT_PHASE_CLIMB     = 3,
    FLIGHT_PHASE_CRUISE    = 4,
    FLIGHT_PHASE_DESCENT   = 5,
    FLIGHT_PHASE_APPROACH  = 6,
    FLIGHT_PHASE_LANDING   = 7,
} FlightPhase;

/* =========================================================================
 *  Lifecycle
 * ========================================================================= */

/**
 * @brief Create the data flywheel engine.
 *
 * @param output_dir    Directory for JSON sample files (e.g. "data/flywheel/").
 * @param ring_seconds  Ring buffer capacity in seconds (e.g. 60).
 * @param capture_hz    Expected update rate (e.g. 20 Hz from DATA stream).
 * @return New DataFlywheel handle, or NULL on allocation failure.
 */
DataFlywheel* flywheel_create(const char* output_dir,
                              int ring_seconds, int capture_hz);

/**
 * @brief Destroy the flywheel, flushing any pending samples first.
 */
void flywheel_destroy(DataFlywheel* fw);

/* =========================================================================
 *  Per-frame update — call from main loop at render rate
 * ========================================================================= */

/**
 * @brief Feed the latest flight data snapshot into the ring buffer.
 *
 * Also checks:
 *   - Active L1/L2 captures whose post-trigger window has elapsed → finalize
 *   - L3 random sampling timer → capture snapshot
 *
 * @param fw  Flywheel handle.
 * @param fd  Latest flight data snapshot (copied into ring buffer).
 */
void flywheel_update(DataFlywheel* fw, const FlightDataValues* fd);

/* =========================================================================
 *  L1 — Alert-triggered capture (auto-label from alert type)
 * ========================================================================= */

/**
 * @brief Trigger an L1 capture on alert activation.
 *
 * Creates an active capture spanning [now - pre_sec, now + post_sec].
 * The capture is finalized when the post-trigger window elapses (checked
 * in flywheel_update()). The alert name is used as the auto-label.
 *
 * Cooldown: consecutive triggers of the SAME alert type within cooldown_sec
 * are ignored to prevent duplicate captures.
 *
 * @param fw          Flywheel handle.
 * @param alert_id    Which alert fired (ALERT_ID_* enum).
 * @param alert_name  Human-readable alert name for auto-labeling.
 * @param pre_sec     Seconds before trigger to capture (default 30).
 * @param post_sec    Seconds after trigger to capture (default 10).
 */
void flywheel_alert_trigger(DataFlywheel* fw, AlertID alert_id,
                            const char* alert_name,
                            int pre_sec, int post_sec);

/* =========================================================================
 *  L2 — Low-confidence capture (model uncertainty)
 * ========================================================================= */

/**
 * @brief Trigger an L2 capture when AI model confidence is below threshold.
 *
 * The capture is queued for human review rather than auto-labeled. The
 * ai_output text is stored for comparison after human correction.
 *
 * @param fw          Flywheel handle.
 * @param confidence  Model confidence score (0.0 .. 1.0, lower = more uncertain).
 * @param ai_output   The advisory text the model generated (may be wrong).
 * @param pre_sec     Seconds before trigger to capture (default 15).
 * @param post_sec    Seconds after trigger to capture (default 5).
 */
void flywheel_confidence_trigger(DataFlywheel* fw,
                                 float confidence, const char* ai_output,
                                 int pre_sec, int post_sec);

/* =========================================================================
 *  L3 — Periodic random sampling
 * ========================================================================= */

/**
 * @brief Configure L3 random sampling interval.
 *
 * @param fw             Flywheel handle.
 * @param interval_sec   Sample every N seconds (0 = disable L3).
 * @param window_sec     Capture this many seconds per sample.
 */
void flywheel_set_random_interval(DataFlywheel* fw,
                                  int interval_sec, int window_sec);

/* =========================================================================
 *  Statistics & flush
 * ========================================================================= */

/**
 * @brief Get the number of completed (finalized, not yet flushed) samples.
 */
int flywheel_pending_count(const DataFlywheel* fw);

/**
 * @brief Get per-source sample counts: out[0]=L1, out[1]=L2, out[2]=L3.
 */
void flywheel_stats(const DataFlywheel* fw, int out_counts[3]);

/**
 * @brief Flush all completed samples to JSON files on disk.
 *
 * Each sample is written as one .json file:
 *   {output_dir}/fw_{id}.json
 *
 * After writing, completed samples are cleared from memory.
 * @return Number of samples written, or -1 on I/O error.
 */
int flywheel_flush(DataFlywheel* fw);

/* =========================================================================
 *  Flight phase detection (utility, also used by ND/EICAS rendering)
 * ========================================================================= */

/**
 * @brief Heuristic flight phase classification from flight data.
 *
 * Simple rule-based classifier — sufficient for metadata tagging.
 * A production system would use a learned classifier or FMS phase state.
 */
FlightPhase flywheel_detect_phase(const FlightDataValues* fd);

/**
 * @brief Get human-readable phase name.
 */
const char* flywheel_phase_name(FlightPhase phase);

#endif /* DATA_FLYWHEEL_H */
