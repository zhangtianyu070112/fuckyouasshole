/**
 * @file    data_flywheel.c
 * @brief   AI Copilot Data Flywheel — implementation.
 *
 * Design rationale (see docs/数据飞轮系统设计方案.md for full discussion):
 *
 *   L1 (Auto-trigger by alert):
 *     The 39-type alert system is a built-in, high-precision labeler.
 *     When TERRAIN_PULL_UP fires, we know with certainty that the preceding
 *     seconds contain a terrain-avoidance scenario.  No human annotation needed.
 *
 *   L2 (Low-confidence capture):
 *     When the AI model's output softmax entropy is high (top-1 and top-2
 *     probabilities close), the model is uncertain.  These edge cases are
 *     the highest-value training data — they directly address model weaknesses.
 *
 *   L3 (Random stratified sampling):
 *     Periodic random snapshots prevent distributional drift and ensure
 *     coverage of nominal (non-alert) flight phases that L1/L2 miss.
 *
 * Memory budget:
 *   Ring buffer: ring_seconds × capture_hz frames.
 *   Default 60s × 20Hz = 1200 frames × ~600 bytes/frame ≈ 720 KB.
 *   Each active capture references ring buffer indices (no frame copy).
 *   Only finalized captures copy frame data into heap JSON buffers.
 *
 * Thread safety:
 *   Designed for single-threaded use from the main loop (same as AIAdvisor).
 *   If the UDP thread needs to trigger captures, the caller must serialize.
 */

#include "data_flywheel.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
  #include <windows.h>
  #define FLYWHEEL_MKDIR(p)  CreateDirectoryA((p), NULL)
#else
  #include <sys/stat.h>
  #include <sys/types.h>
  #define FLYWHEEL_MKDIR(p)  mkdir((p), 0755)
#endif

#include <SDL2/SDL.h>

/* =========================================================================
 *  Constants
 * ========================================================================= */

#define FW_ID_MAX_LEN        64
#define FW_PATH_MAX_LEN      512
#define FW_JSON_BUF_SIZE     (4 * 1024 * 1024)  /* 4 MB — enough for ~1200 frames */
#define FW_MAX_CAPTURES      64                   /* max concurrent active captures */
#define FW_ALERT_NAME_MAX    64
#define FW_LABEL_MAX         384
#define FW_PREDICTION_MAX    512
#define FW_PHASE_NAME_MAX    16

/* Default capture windows (seconds) */
#define FW_DEFAULT_L1_PRE    30
#define FW_DEFAULT_L1_POST   10
#define FW_DEFAULT_L2_PRE    15
#define FW_DEFAULT_L2_POST    5
#define FW_DEFAULT_L3_WINDOW 10

/* L1 cooldown: same alert type won't trigger again within this (seconds) */
#define FW_L1_COOLDOWN_SEC   5

/* L3 default interval (seconds); 0 = disabled */
#define FW_DEFAULT_L3_INTERVAL  120

/* =========================================================================
 *  Ring buffer frame
 * ========================================================================= */

typedef struct {
    FlightDataValues fd;          /* full flight state snapshot */
    uint64_t         ticks;       /* SDL_GetTicks() at capture time */
    FlightPhase      phase;       /* detected phase (cached for speed) */
} FlywheelFrame;

/* =========================================================================
 *  Capture descriptor
 * ========================================================================= */

typedef struct {
    /* Identity */
    char     id[FW_ID_MAX_LEN];
    int      source;              /* FlywheelSource enum */

    /* Trigger info */
    char     trigger_alert[FW_ALERT_NAME_MAX];  /* L1: alert name */
    float    ai_confidence;                     /* L2: 0..1 */
    char     ai_prediction[FW_PREDICTION_MAX];  /* L2: model output text */

    /* Ring buffer window */
    int      ring_start;          /* first frame index (inclusive) */
    int      ring_end;            /* last frame index (inclusive) */
    int      frame_count;
    int      trigger_frame_idx;   /* ring index of the exact trigger moment */

    /* Metadata */
    uint64_t    trigger_ticks;
    FlightPhase trigger_phase;
    char        label[FW_LABEL_MAX];
    int         label_source;     /* 0 = auto, 1 = human_reviewed */

    /* State machine */
    int         finalized;          /* 0 = collecting post-trigger frames */
    uint64_t    post_deadline_ticks;/* when post-trigger window ends */

    /* Serialized JSON cache (populated on finalize, freed on flush) */
    char*       json_buf;
    int         json_len;
} FlywheelCapture;

/* =========================================================================
 *  DataFlywheel struct
 * ========================================================================= */

struct DataFlywheel {
    /* Ring buffer */
    FlywheelFrame* ring_buf;
    int            ring_capacity;   /* total slots */
    int            ring_seconds;    /* ring buffer duration (seconds) */
    int            ring_head;       /* next write position */
    int            ring_count;      /* total frames ever written (wraps) */
    int            frame_stride;    /* capture every N update() calls (downsampling) */
    int            frame_counter;   /* update() call counter for downsampling */

    /* Active captures (collecting post-trigger data) */
    FlywheelCapture captures[FW_MAX_CAPTURES];
    int             capture_count;

    /* L1 alert cooldown tracking */
    uint64_t  alert_last_trigger_ticks[ALERT_ID_COUNT];

    /* L3 random sampling */
    uint64_t  l3_last_sample_ticks;
    int       l3_interval_ms;
    int       l3_window_sec;

    /* Output */
    char      output_dir[256];
    int       sample_counter;      /* monotonically increasing sample ID */

    /* Statistics */
    int       stats[3];            /* L1, L2, L3 counts */

    /* AI model metadata (set once at creation) */
    char      model_name[64];
    char      model_version[32];
};

/* =========================================================================
 *  Internal: flight phase heuristic
 * ========================================================================= */

FlightPhase flywheel_detect_phase(const FlightDataValues* fd)
{
    float agl  = fd->altitude_agl_ft;
    float vs   = fd->vs_fpm;
    float ias  = fd->ias_kts;
    int   gear = fd->gear_deployed;
    float flap = fd->flap_ratio;

    /* On ground: very low AGL and low speed */
    if (agl < 50.0f && ias < 50.0f)
        return FLIGHT_PHASE_GROUND;

    /* Takeoff roll / initial climb: low AGL, climbing, accelerating */
    if (agl < 1000.0f && vs > 500.0f && ias > 100.0f)
        return FLIGHT_PHASE_TAKEOFF;

    /* Landing: very low AGL, descending or level, gear likely down */
    if (agl < 100.0f && (gear || flap > 0.3f))
        return FLIGHT_PHASE_LANDING;

    /* Approach: gear/flaps out, descending through 3000ft */
    if (agl < 3000.0f && vs < -300.0f && (gear || flap > 0.1f))
        return FLIGHT_PHASE_APPROACH;

    /* Climb: positive VS above 1000ft */
    if (vs > 500.0f && agl > 1000.0f)
        return FLIGHT_PHASE_CLIMB;

    /* Cruise: low |VS|, high altitude */
    if (fabsf(vs) < 500.0f && agl > 10000.0f)
        return FLIGHT_PHASE_CRUISE;

    /* Descent: negative VS above 1000ft */
    if (vs < -500.0f && agl > 1000.0f)
        return FLIGHT_PHASE_DESCENT;

    return FLIGHT_PHASE_UNKNOWN;
}

const char* flywheel_phase_name(FlightPhase phase)
{
    static const char* names[] = {
        "unknown", "ground", "takeoff", "climb",
        "cruise", "descent", "approach", "landing"
    };
    int idx = (int)phase;
    if (idx < 0 || idx > 7) return "unknown";
    return names[idx];
}

/* =========================================================================
 *  Internal: generate unique sample ID
 * ========================================================================= */

static void make_sample_id(char* buf, int buf_size, int counter, int source)
{
    /* Format: fw_YYYYMMDD_HHMMSS_S{source}{counter} — deterministic for tests */
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    if (tm_info) {
        snprintf(buf, (size_t)buf_size,
                 "fw_%04d%02d%02d_%02d%02d%02d_s%d_%04d",
                 tm_info->tm_year + 1900, tm_info->tm_mon + 1,
                 tm_info->tm_mday, tm_info->tm_hour,
                 tm_info->tm_min, tm_info->tm_sec,
                 source, counter);
    } else {
        snprintf(buf, (size_t)buf_size,
                 "fw_unknown_s%d_%04d", source, counter);
    }
}

/* =========================================================================
 *  Internal: JSON escaping
 * ========================================================================= */

static void json_escape(const char* src, char* dst, int dst_size)
{
    int j = 0;
    for (const char* s = src; *s && j < dst_size - 2; s++) {
        switch (*s) {
        case '"':  dst[j++] = '\\'; dst[j++] = '"';  break;
        case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
        case '\n': dst[j++] = '\\'; dst[j++] = 'n';  break;
        case '\r': dst[j++] = '\\'; dst[j++] = 'r';  break;
        case '\t': dst[j++] = '\\'; dst[j++] = 't';  break;
        default:   dst[j++] = *s; break;
        }
    }
    dst[j] = '\0';
}

/* =========================================================================
 *  Internal: serialize one frame to JSON
 * ========================================================================= */

static int serialize_frame(const FlywheelFrame* frm, char* buf, int buf_size)
{
    const FlightDataValues* f = &frm->fd;
    return snprintf(buf, (size_t)buf_size,
        "{"
        "\"t_ms\":%llu,"
        "\"phase\":\"%s\","
        "\"alt_ft\":%.0f,\"agl_ft\":%.0f,"
        "\"ias_kts\":%.0f,\"vs_fpm\":%.0f,"
        "\"mach\":%.3f,\"roll_deg\":%.1f,\"pitch_deg\":%.1f,"
        "\"heading\":%.0f,"
        "\"lat\":%.6f,\"lon\":%.6f,"
        "\"n1_pct\":[%.1f,%.1f],"
        "\"egt_c\":[%.0f,%.0f],"
        "\"flap_ratio\":%.2f,\"gear_down\":%d,"
        "\"ap_engaged\":%d,\"athr_engaged\":%d,"
        "\"fuel_lbs\":%.0f,\"oat_c\":%.0f,"
        "\"master_warning\":%d,\"master_caution\":%d,"
        "\"wind_speed_kts\":%.0f,\"wind_dir_deg\":%.0f,"
        "\"cabin_alt_ft\":%.0f,\"cabin_diff_psi\":%.1f"
        "}",
        (unsigned long long)frm->ticks,
        flywheel_phase_name(frm->phase),
        (double)f->altitude_ft, (double)f->altitude_agl_ft,
        (double)f->ias_kts, (double)f->vs_fpm,
        (double)f->mach, (double)f->roll_deg, (double)f->pitch_deg,
        (double)f->heading_mag_deg,
        f->lat_deg, f->lon_deg,
        (double)f->n1_pct[0], (double)f->n1_pct[1],
        (double)f->egt_c[0], (double)f->egt_c[1],
        (double)f->flap_ratio, f->gear_deployed,
        f->ap_engaged, f->ap_athr_engaged,
        (double)f->fuel_total_lbs, (double)f->oat_c,
        f->master_warning, f->master_caution,
        (double)f->wind_speed_kts, (double)f->wind_dir_deg,
        (double)f->cabin_alt_ft, (double)f->cabin_diff_psi);
}

/* =========================================================================
 *  Internal: serialize alert bitmask as JSON array of active alert names
 * ========================================================================= */

static const char* alert_id_to_name(AlertID id)
{
    /* Must match the enum in alert_system.h */
    static const char* names[ALERT_ID_COUNT] = {
        "PULL_UP", "WINDSHEAR", "MASTER_WARNING", "MASTER_CAUTION",
        "TERRAIN", "SINK_RATE", "TOO_LOW_GEAR", "TOO_LOW_FLAPS",
        "GLIDESLOPE", "BANK_ANGLE", "OVERSPEED", "STALL", "MINIMUMS",
        "ENG_OVERHEAT", "ENG_ASYM", "FUEL_IMBALANCE", "OIL_PRESS_LOW",
        "CABIN_ALT_HIGH", "BUS_VOLT_ABNORM", "TAKEOFF_CONFIG", "LOW_FUEL",
        "ICING_CONDITION", "APU_FIRE", "OIL_TEMP_HIGH", "HYD_PRESS_LOW",
        "HYD_QTY_LOW",
        "FIRE_ENG1", "FIRE_ENG2", "FIRE_APU", "FIRE_WHEEL_WELL",
        "FIRE_CARGO", "TCAS_TA", "TCAS_RA", "STALL_WARNING",
        "DOOR_OPEN", "ELEC_FAULT", "ANTI_ICE_FAULT", "AP_DISENGAGE",
        "AT_DISENGAGE"
    };
    int idx = (int)id;
    if (idx < 0 || idx >= ALERT_ID_COUNT) return "UNKNOWN";
    return names[idx];
}

/* =========================================================================
 *  Internal: build complete sample JSON for a finalized capture
 * ========================================================================= */

static char* build_sample_json(const DataFlywheel* fw,
                               const FlywheelCapture* cap,
                               int* out_len)
{
    char* buf = (char*)malloc(FW_JSON_BUF_SIZE);
    if (!buf) return NULL;

    char escaped_label[FW_LABEL_MAX * 2];
    char escaped_pred[FW_PREDICTION_MAX * 2];
    json_escape(cap->label, escaped_label, sizeof(escaped_label));
    json_escape(cap->ai_prediction, escaped_pred, sizeof(escaped_pred));

    int pos = 0;
    pos += snprintf(buf + pos, (size_t)(FW_JSON_BUF_SIZE - pos),
        "{\n"
        "  \"id\": \"%s\",\n"
        "  \"source\": \"%s\",\n"
        "  \"model_name\": \"%s\",\n"
        "  \"model_version\": \"%s\",\n"
        "  \"trigger_alert\": \"%s\",\n"
        "  \"trigger_timestamp\": %llu,\n"
        "  \"flight_phase\": \"%s\",\n"
        "  \"ai_confidence\": %.4f,\n"
        "  \"ai_prediction\": \"%s\",\n"
        "  \"label\": \"%s\",\n"
        "  \"label_source\": \"%s\",\n"
        "  \"frame_count\": %d,\n"
        "  \"frame_rate_hz\": %d,\n"
        "  \"frames\": [\n",
        cap->id,
        cap->source == FLYWHEEL_L1_AUTO_ALERT     ? "l1_auto_alert" :
        cap->source == FLYWHEEL_L2_LOW_CONFIDENCE ? "l2_low_confidence" :
                                                     "l3_random_sample",
        fw->model_name, fw->model_version,
        cap->trigger_alert,
        (unsigned long long)cap->trigger_ticks,
        flywheel_phase_name(cap->trigger_phase),
        (double)cap->ai_confidence,
        escaped_pred,
        escaped_label,
        cap->label_source == 0 ? "auto" : "human_reviewed",
        cap->frame_count,
        fw->ring_capacity / (fw->ring_seconds > 0 ? fw->ring_seconds : 60));

    /* Serialize each frame */
    int ring_cap = fw->ring_capacity;
    for (int i = 0; i < cap->frame_count; i++) {
        int ring_idx = (cap->ring_start + i) % ring_cap;
        char frame_json[2048];
        int frame_len = serialize_frame(&fw->ring_buf[ring_idx],
                                        frame_json, sizeof(frame_json));
        if (frame_len > 0 && pos + frame_len + 4 < FW_JSON_BUF_SIZE) {
            if (i > 0) buf[pos++] = ',';
            buf[pos++] = '\n';
            buf[pos++] = ' ';
            buf[pos++] = ' ';
            buf[pos++] = ' ';
            buf[pos++] = ' ';
            memcpy(buf + pos, frame_json, (size_t)frame_len);
            pos += frame_len;
        }
    }

    pos += snprintf(buf + pos, (size_t)(FW_JSON_BUF_SIZE - pos),
        "\n  ]\n}\n");

    *out_len = pos;
    return buf;
}

/* =========================================================================
 *  Internal: ensure output directory exists
 * ========================================================================= */

static int ensure_output_dir(const char* dir)
{
    FLYWHEEL_MKDIR(dir);
    /* Check if it worked — try creating a temp file */
    char test_path[FW_PATH_MAX_LEN];
    snprintf(test_path, sizeof(test_path), "%s/.flywheel_test", dir);
    FILE* f = fopen(test_path, "w");
    if (f) {
        fclose(f);
        remove(test_path);
        return 0;
    }
    return -1;
}

/* =========================================================================
 *  Lifecycle
 * ========================================================================= */

DataFlywheel* flywheel_create(const char* output_dir,
                              int ring_seconds, int capture_hz)
{
    DataFlywheel* fw = (DataFlywheel*)calloc(1, sizeof(DataFlywheel));
    if (!fw) return NULL;

    /* Ring buffer */
    fw->ring_capacity = ring_seconds * capture_hz;
    if (fw->ring_capacity < 100) fw->ring_capacity = 100;
    fw->ring_buf = (FlywheelFrame*)calloc(
        (size_t)fw->ring_capacity, sizeof(FlywheelFrame));
    if (!fw->ring_buf) {
        free(fw);
        return NULL;
    }
    fw->ring_seconds = ring_seconds;
    fw->frame_stride = 1;  /* capture every update() by default */
    fw->frame_counter = 0;

    /* Default L3 settings */
    fw->l3_interval_ms = FW_DEFAULT_L3_INTERVAL * 1000;
    fw->l3_window_sec  = FW_DEFAULT_L3_WINDOW;
    fw->l3_last_sample_ticks = 0;

    /* Output */
    strncpy(fw->output_dir, output_dir, sizeof(fw->output_dir) - 1);
    fw->output_dir[sizeof(fw->output_dir) - 1] = '\0';
    ensure_output_dir(fw->output_dir);

    /* Model metadata — will be updated from config */
    strncpy(fw->model_name, "Qwen2.5-3B-FullFT", sizeof(fw->model_name) - 1);
    strncpy(fw->model_version, "v2.0", sizeof(fw->model_version) - 1);

    return fw;
}

void flywheel_destroy(DataFlywheel* fw)
{
    if (!fw) return;

    /* Flush any remaining samples before shutting down */
    flywheel_flush(fw);

    /* Free JSON buffers of any unfinalized captures */
    for (int i = 0; i < fw->capture_count; i++) {
        if (fw->captures[i].json_buf) {
            free(fw->captures[i].json_buf);
        }
    }

    free(fw->ring_buf);
    free(fw);
}

/* =========================================================================
 *  Per-frame update
 * ========================================================================= */

void flywheel_update(DataFlywheel* fw, const FlightDataValues* fd)
{
    if (!fw || !fd) return;

    uint64_t now = SDL_GetTicks();

    /* Downsampling: only capture every frame_stride-th update */
    fw->frame_counter++;
    if (fw->frame_counter % fw->frame_stride != 0) return;

    /* --- Write to ring buffer --- */
    FlywheelFrame* frm = &fw->ring_buf[fw->ring_head];
    frm->fd    = *fd;
    frm->ticks = now;
    frm->phase = flywheel_detect_phase(fd);

    fw->ring_head = (fw->ring_head + 1) % fw->ring_capacity;
    fw->ring_count++;

    /* --- Check active captures for post-trigger finalization --- */
    for (int i = 0; i < fw->capture_count; /* manual increment */) {
        FlywheelCapture* cap = &fw->captures[i];

        if (cap->finalized) {
            i++;
            continue;
        }

        if (now >= cap->post_deadline_ticks) {
            /* Post-trigger window complete — finalize the capture */
            int ring_cap = fw->ring_capacity;
            int total_frames = fw->ring_count;
            int trigger_idx = cap->trigger_frame_idx;

            /* Calculate how many frames are now available after trigger */
            int frames_after = 0;
            int idx = trigger_idx;
            while (idx != fw->ring_head && frames_after < ring_cap) {
                frames_after++;
                idx = (idx + 1) % ring_cap;
            }

            /* Recalculate: trigger_idx backwards for pre-window */
            int pre_frames = 0;
            idx = trigger_idx;
            /* Walk backwards from trigger up to ring_capacity */
            int earliest_frame = (total_frames > ring_cap)
                ? (total_frames - ring_cap) : 0;
            for (int j = 0; j < ring_cap; j++) {
                int prev = (trigger_idx - j + ring_cap) % ring_cap;
                int frame_num = total_frames - frames_after - j;
                if (frame_num >= earliest_frame) {
                    pre_frames++;
                } else {
                    break;
                }
            }

            cap->ring_start = (trigger_idx - pre_frames + 1 + ring_cap) % ring_cap;
            cap->ring_end   = (trigger_idx + frames_after - 1) % ring_cap;
            cap->frame_count = pre_frames + frames_after;
            if (cap->frame_count > ring_cap)
                cap->frame_count = ring_cap;

            cap->finalized = 1;

            /* Build JSON now (while data is fresh in ring buffer) */
            cap->json_buf = build_sample_json(fw, cap, &cap->json_len);

            /* Update stats */
            if (cap->source >= 0 && cap->source < 3)
                fw->stats[cap->source]++;

            i++;
        } else {
            i++;
        }
    }

    /* Compact: remove finalized captures that have been flushed
     * (json_buf == NULL means flushed). Keep finalized+unflushed ones. */
    /* This is done lazily — we only compact when the array is full. */

    /* --- L3 random sampling --- */
    if (fw->l3_interval_ms > 0) {
        if (fw->l3_last_sample_ticks == 0) {
            fw->l3_last_sample_ticks = now;
        } else if (now - fw->l3_last_sample_ticks >=
                   (uint64_t)fw->l3_interval_ms) {
            /* Take a snapshot of the last l3_window_sec seconds */
            if (fw->capture_count < FW_MAX_CAPTURES) {
                FlywheelCapture* cap = &fw->captures[fw->capture_count++];
                memset(cap, 0, sizeof(*cap));

                fw->sample_counter++;
                make_sample_id(cap->id, FW_ID_MAX_LEN,
                               fw->sample_counter, FLYWHEEL_L3_RANDOM_SAMPLE);
                cap->source = FLYWHEEL_L3_RANDOM_SAMPLE;
                cap->ai_confidence = -1.0f;  /* N/A */
                cap->trigger_phase = flywheel_detect_phase(fd);
                cap->label_source = 0;  /* auto — phase-based label */

                /* Auto-label: describe the nominal flight phase */
                snprintf(cap->label, FW_LABEL_MAX,
                         "正常%s阶段飞行数据采样",
                         flywheel_phase_name(cap->trigger_phase));

                /* L3 has no post-trigger window — finalize immediately */
                int capture_hz = (fw->ring_seconds > 0)
                    ? (fw->ring_capacity / fw->ring_seconds) : 20;
                int window_frames = fw->l3_window_sec * capture_hz;
                if (window_frames > fw->ring_capacity) window_frames = fw->ring_capacity;
                if (window_frames < 10) window_frames = 10;

                cap->trigger_frame_idx = (fw->ring_head - 1 + fw->ring_capacity) % fw->ring_capacity;
                cap->ring_start = (cap->trigger_frame_idx - window_frames + 1 + fw->ring_capacity) % fw->ring_capacity;
                cap->ring_end   = (fw->ring_head - 1 + fw->ring_capacity) % fw->ring_capacity;
                cap->frame_count = window_frames;
                cap->trigger_ticks = now;
                cap->finalized = 1;

                cap->json_buf = build_sample_json(fw, cap, &cap->json_len);
                fw->stats[FLYWHEEL_L3_RANDOM_SAMPLE]++;
            }
            fw->l3_last_sample_ticks = now;
        }
    }
}

/* =========================================================================
 *  L1 — Alert-triggered capture
 * ========================================================================= */

void flywheel_alert_trigger(DataFlywheel* fw, AlertID alert_id,
                            const char* alert_name,
                            int pre_sec, int post_sec)
{
    if (!fw) return;
    if (alert_id < 0 || alert_id >= ALERT_ID_COUNT) return;

    uint64_t now = SDL_GetTicks();

    /* Cooldown check — avoid duplicate triggers of the same alert */
    if (fw->alert_last_trigger_ticks[alert_id] > 0 &&
        now - fw->alert_last_trigger_ticks[alert_id] <
        (uint64_t)(FW_L1_COOLDOWN_SEC * 1000)) {
        return;
    }
    fw->alert_last_trigger_ticks[alert_id] = now;

    /* Check capacity */
    if (fw->capture_count >= FW_MAX_CAPTURES) {
        /* Try to compact: remove flushed captures to make room */
        int write_pos = 0;
        for (int i = 0; i < fw->capture_count; i++) {
            if (fw->captures[i].json_buf == NULL && fw->captures[i].finalized) {
                /* Already flushed — skip (compact) */
                continue;
            }
            if (i != write_pos) {
                fw->captures[write_pos] = fw->captures[i];
            }
            write_pos++;
        }
        fw->capture_count = write_pos;

        if (fw->capture_count >= FW_MAX_CAPTURES) {
            /* Still full — drop oldest finalized capture */
            int oldest_idx = -1;
            for (int i = 0; i < fw->capture_count; i++) {
                if (fw->captures[i].finalized && fw->captures[i].json_buf) {
                    if (oldest_idx < 0 ||
                        fw->captures[i].trigger_ticks <
                        fw->captures[oldest_idx].trigger_ticks) {
                        oldest_idx = i;
                    }
                }
            }
            if (oldest_idx >= 0) {
                free(fw->captures[oldest_idx].json_buf);
                fw->captures[oldest_idx] =
                    fw->captures[fw->capture_count - 1];
                fw->capture_count--;
            } else {
                return;  /* No room */
            }
        }
    }

    /* Create new capture */
    FlywheelCapture* cap = &fw->captures[fw->capture_count++];
    memset(cap, 0, sizeof(*cap));

    fw->sample_counter++;
    make_sample_id(cap->id, FW_ID_MAX_LEN, fw->sample_counter,
                   FLYWHEEL_L1_AUTO_ALERT);

    cap->source         = FLYWHEEL_L1_AUTO_ALERT;
    cap->trigger_ticks  = now;
    cap->trigger_phase  = flywheel_detect_phase(
        &fw->ring_buf[(fw->ring_head - 1 + fw->ring_capacity) % fw->ring_capacity].fd);
    cap->label_source   = 0;  /* auto-labeled */
    cap->finalized      = 0;  /* still collecting post-trigger frames */
    cap->post_deadline_ticks = now + (uint64_t)(post_sec * 1000);
    cap->ai_confidence  = -1.0f;

    /* Record trigger frame index (most recently written frame) */
    cap->trigger_frame_idx = (fw->ring_head - 1 + fw->ring_capacity)
                             % fw->ring_capacity;

    /* Store alert info */
    strncpy(cap->trigger_alert, alert_name, FW_ALERT_NAME_MAX - 1);
    cap->trigger_alert[FW_ALERT_NAME_MAX - 1] = '\0';

    /* Auto-label from alert type:
     * "GPWS触发{告警名称}，需立即执行相应处置程序" */
    snprintf(cap->label, FW_LABEL_MAX,
             "GPWS触发%s告警，需立即执行相应处置程序", alert_name);
}

/* =========================================================================
 *  L2 — Low-confidence capture
 * ========================================================================= */

void flywheel_confidence_trigger(DataFlywheel* fw,
                                 float confidence, const char* ai_output,
                                 int pre_sec, int post_sec)
{
    if (!fw) return;

    uint64_t now = SDL_GetTicks();

    /* L2 does not have cooldown — low confidence is already rate-limited
     * by the AI model's inference frequency (~1-5 Hz). We still guard
     * against burst captures by checking for duplicates within 2 seconds. */

    if (fw->capture_count >= FW_MAX_CAPTURES) {
        /* Same compact logic as L1 */
        int write_pos = 0;
        for (int i = 0; i < fw->capture_count; i++) {
            if (fw->captures[i].json_buf == NULL && fw->captures[i].finalized)
                continue;
            if (i != write_pos)
                fw->captures[write_pos] = fw->captures[i];
            write_pos++;
        }
        fw->capture_count = write_pos;

        if (fw->capture_count >= FW_MAX_CAPTURES) {
            /* Drop oldest finalized */
            int oldest_idx = -1;
            for (int i = 0; i < fw->capture_count; i++) {
                if (fw->captures[i].finalized && fw->captures[i].json_buf) {
                    if (oldest_idx < 0 ||
                        fw->captures[i].trigger_ticks <
                        fw->captures[oldest_idx].trigger_ticks) {
                        oldest_idx = i;
                    }
                }
            }
            if (oldest_idx >= 0) {
                free(fw->captures[oldest_idx].json_buf);
                fw->captures[oldest_idx] =
                    fw->captures[fw->capture_count - 1];
                fw->capture_count--;
            } else {
                return;
            }
        }
    }

    FlywheelCapture* cap = &fw->captures[fw->capture_count++];
    memset(cap, 0, sizeof(*cap));

    fw->sample_counter++;
    make_sample_id(cap->id, FW_ID_MAX_LEN, fw->sample_counter,
                   FLYWHEEL_L2_LOW_CONFIDENCE);

    cap->source         = FLYWHEEL_L2_LOW_CONFIDENCE;
    cap->trigger_ticks  = now;
    cap->trigger_phase  = flywheel_detect_phase(
        &fw->ring_buf[(fw->ring_head - 1 + fw->ring_capacity) % fw->ring_capacity].fd);
    cap->label_source   = 1;  /* needs human review */
    cap->finalized      = 0;  /* waiting for post-trigger window */
    cap->post_deadline_ticks = now + (uint64_t)(post_sec * 1000);

    cap->trigger_frame_idx = (fw->ring_head - 1 + fw->ring_capacity)
                             % fw->ring_capacity;

    cap->ai_confidence = confidence;

    if (ai_output) {
        strncpy(cap->ai_prediction, ai_output, FW_PREDICTION_MAX - 1);
        cap->ai_prediction[FW_PREDICTION_MAX - 1] = '\0';
    } else {
        cap->ai_prediction[0] = '\0';
    }

    /* Placeholder label — will be filled by human reviewer */
    snprintf(cap->label, FW_LABEL_MAX,
             "[待审核] AI副驾驶低置信度建议(confidence=%.2f) — 需人工标注",
             (double)confidence);

    strncpy(cap->trigger_alert, "LOW_CONFIDENCE", FW_ALERT_NAME_MAX - 1);
}

/* =========================================================================
 *  L3 configuration
 * ========================================================================= */

void flywheel_set_random_interval(DataFlywheel* fw,
                                  int interval_sec, int window_sec)
{
    if (!fw) return;
    fw->l3_interval_ms = (interval_sec > 0) ? (interval_sec * 1000) : 0;
    fw->l3_window_sec  = (window_sec > 0)  ? window_sec : FW_DEFAULT_L3_WINDOW;
    fw->l3_last_sample_ticks = 0;  /* reset timer */
}

/* =========================================================================
 *  Statistics & flush
 * ========================================================================= */

int flywheel_pending_count(const DataFlywheel* fw)
{
    if (!fw) return 0;
    int count = 0;
    for (int i = 0; i < fw->capture_count; i++) {
        if (fw->captures[i].finalized && fw->captures[i].json_buf)
            count++;
    }
    return count;
}

void flywheel_stats(const DataFlywheel* fw, int out_counts[3])
{
    if (!fw) return;
    out_counts[0] = fw->stats[0];
    out_counts[1] = fw->stats[1];
    out_counts[2] = fw->stats[2];
}

int flywheel_flush(DataFlywheel* fw)
{
    if (!fw) return -1;

    int written = 0;

    for (int i = 0; i < fw->capture_count; i++) {
        FlywheelCapture* cap = &fw->captures[i];

        if (!cap->finalized || !cap->json_buf)
            continue;

        /* Build output path */
        char path[FW_PATH_MAX_LEN];
        snprintf(path, sizeof(path), "%s/%s.json",
                 fw->output_dir, cap->id);

        /* Write JSON file */
        FILE* f = fopen(path, "w");
        if (!f) {
            /* Don't abort on single-file error — continue flushing others */
            continue;
        }

        size_t bytes_written = fwrite(cap->json_buf, 1,
                                      (size_t)cap->json_len, f);
        fclose(f);

        if ((int)bytes_written == cap->json_len) {
            written++;
        }

        /* Free JSON buffer — mark as flushed */
        free(cap->json_buf);
        cap->json_buf = NULL;
        cap->json_len = 0;
    }

    /* Compact the captures array: remove flushed entries */
    int write_pos = 0;
    for (int i = 0; i < fw->capture_count; i++) {
        if (fw->captures[i].json_buf != NULL || !fw->captures[i].finalized) {
            /* Keep: not yet flushed, or still collecting */
            if (i != write_pos) {
                fw->captures[write_pos] = fw->captures[i];
            }
            write_pos++;
        }
        /* else: flushed and finalized — drop */
    }
    fw->capture_count = write_pos;

    return written;
}
