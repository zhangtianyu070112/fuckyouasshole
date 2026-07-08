/**
 * @file    alert_system.c
 * @brief   GPWS audio alert system — SDL2 callback-based tone synthesis.
 *
 * Architecture:
 *   - SDL audio device opened at 22050 Hz mono S16
 *   - Up to MAX_TONES (8) concurrent tone generators
 *   - Audio callback mixes all active tones each sample
 *   - alert_system_update() evaluates flight data and enqueues tones
 *   - Each alert type has a cooldown to prevent spam
 *   - Altitude callouts trigger once per crossing
 */

#include "alert_system.h"
#include "utils/logger.h"

#include <SDL2/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* =========================================================================
 *  Audio constants
 * ========================================================================= */

#define SAMPLE_RATE  22050
#define AUDIO_FORMAT AUDIO_S16SYS
#define CHANNELS     1
#define MAX_TONES    8

/* =========================================================================
 *  Alert type definitions and priorities
 * ========================================================================= */

typedef enum {
    /* === GPWS (13) — existing, unchanged === */
    ALERT_PULL_UP = 0,
    ALERT_WINDSHEAR,
    ALERT_MASTER_WARNING,
    ALERT_MASTER_CAUTION,
    ALERT_TERRAIN,
    ALERT_SINK_RATE,
    ALERT_TOO_LOW_GEAR,
    ALERT_TOO_LOW_FLAPS,
    ALERT_GLIDESLOPE,
    ALERT_BANK_ANGLE,
    ALERT_OVERSPEED,
    ALERT_STALL,
    ALERT_MINIMUMS,
    /* === System deviations (13) === */
    ALERT_ENG_OVERHEAT,
    ALERT_ENG_ASYM,
    ALERT_FUEL_IMBALANCE,
    ALERT_OIL_PRESS_LOW,
    ALERT_CABIN_ALT_HIGH,
    ALERT_BUS_VOLT_ABNORM,
    ALERT_TAKEOFF_CONFIG,
    ALERT_LOW_FUEL,
    ALERT_ICING_CONDITION,
    ALERT_APU_FIRE,
    ALERT_OIL_TEMP_HIGH,
    ALERT_HYD_PRESS_LOW,
    ALERT_HYD_QTY_LOW,
    /* === DREF-only (13) === */
    ALERT_FIRE_ENG1,
    ALERT_FIRE_ENG2,
    ALERT_FIRE_APU,
    ALERT_FIRE_WHEEL_WELL,
    ALERT_FIRE_CARGO,
    ALERT_TCAS_TA,
    ALERT_TCAS_RA,
    ALERT_STALL_WARNING,
    ALERT_DOOR_OPEN,
    ALERT_ELEC_FAULT,
    ALERT_ANTI_ICE_FAULT,
    ALERT_AP_DISENGAGE,
    ALERT_AT_DISENGAGE,
    ALERT_TYPE_COUNT
} AlertType;

/* Priority: lower = higher priority (overrides lower-priority tones) */
static const int alert_priority[ALERT_TYPE_COUNT] = {
    /* GPWS */
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
    /* System — lower priority than GPWS */
    13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
    /* DREF — advisory level, lowest priority */
    26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
};

/* Cooldown between repeated triggers (seconds) */
static const float alert_cooldown[ALERT_TYPE_COUNT] = {
    /* GPWS */
    2.5f, 2.0f, 2.0f, 2.0f, 3.0f, 3.0f, 2.0f, 2.0f, 2.0f, 2.0f, 1.5f, 1.5f, 5.0f,
    /* System — longer cooldown, don't spam */
    5.0f, 5.0f, 8.0f, 5.0f, 8.0f, 5.0f, 5.0f, 8.0f, 5.0f, 5.0f,
    5.0f, 5.0f, 5.0f,
    /* DREF */
    3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 2.0f,   /* fire + tcas + stall */
    5.0f, 5.0f, 5.0f, 3.0f, 3.0f,                       /* door/elec/antiice/ap/at */
};

/* =========================================================================
 *  Tone generator state
 * ========================================================================= */

typedef enum {
    WAVE_SINE   = 0,
    WAVE_SQUARE = 1,
    WAVE_SAW    = 2,
} Waveform;

/**
 * @brief One active tone slot in the audio mixer.
 */
typedef struct {
    int       active;
    AlertType type;
    Waveform  wave;
    float     freq;              /* Current frequency (Hz) — may sweep */
    float     freq_start;        /* Starting frequency for sweeps */
    float     freq_end;          /* Ending frequency for sweeps */
    float     phase;             /* Current phase accumulator (0..1) */
    float     volume;            /* 0.0 .. 1.0 */
    int       samples_total;     /* Total duration in samples */
    int       samples_elapsed;   /* How many rendered so far */
    int       pulse_on_samples;  /* Beep pattern: samples ON per cycle */
    int       pulse_off_samples; /* Beep pattern: samples OFF per cycle */
    int       pulse_counter;     /* Current position in on/off cycle */
    int       pulse_state;       /* 1 = currently sounding */
} ToneSlot;

/* =========================================================================
 *  Alert system state
 * ========================================================================= */

struct AlertSystem {
    SDL_AudioDeviceID dev;
    int               audio_ok;
    int               enabled;

    /* Tone mixer */
    ToneSlot  tones[MAX_TONES];
    SDL_mutex* tone_mutex;       /* Protects tones[] from callback vs main */

    /* Cooldown timers */
    float     cooldown_timer[ALERT_TYPE_COUNT];

    /* State for callout tracking */
    float     last_agl_ft;       /* Previous frame's AGL for crossing detect */
    int       callouts_done[11]; /* Which callouts fired (500,400,...,10) */
    int       minim_triggered;   /* Has MINIMUMS fired this descent? */

    /* State for windshear detection */
    float     last_ias_kts;
    float     ias_delta;         /* Change in airspeed over last second */

    /* Previous frame data for delta calculations */
    FlightDataValues prev_fd;
};

/* =========================================================================
 *  Forward declarations
 * ========================================================================= */

static void audio_callback(void* userdata, Uint8* stream, int len);
static void tone_start(AlertSystem* as, AlertType type);
static void evaluate_alerts(AlertSystem* as, const FlightDataValues* fd, float dt);

/* =========================================================================
 *  Internal: tone definitions per alert type
 * ========================================================================= */

/**
 * @brief Configure a tone slot for a specific alert type.
 */
static void configure_tone(ToneSlot* t, AlertType type)
{
    memset(t, 0, sizeof(*t));
    t->active = 1;
    t->type   = type;
    t->volume = 0.30f;  /* default volume */

    switch (type) {
    case ALERT_PULL_UP:
        /* Alternating 800Hz/400Hz square wave, 200ms each, urgent */
        t->wave        = WAVE_SQUARE;
        t->freq        = 800.0f;
        t->freq_start  = 800.0f;
        t->freq_end    = 400.0f;
        t->volume      = 0.35f;
        t->samples_total   = SAMPLE_RATE * 3;  /* 3 seconds */
        t->pulse_on_samples  = (SAMPLE_RATE * 200) / 1000; /* 200ms on */
        t->pulse_off_samples = (SAMPLE_RATE * 200) / 1000; /* 200ms off */
        t->pulse_state = 1;
        break;

    case ALERT_WINDSHEAR:
        /* Rapid warble: 600/300Hz alternating, 100ms each */
        t->wave        = WAVE_SQUARE;
        t->freq        = 600.0f;
        t->freq_start  = 600.0f;
        t->freq_end    = 300.0f;
        t->volume      = 0.35f;
        t->samples_total   = SAMPLE_RATE * 2;
        t->pulse_on_samples  = (SAMPLE_RATE * 100) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 100) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_MASTER_WARNING:
        /* Fire/Master warning bell simulation: continuous rapid ringing */
        t->wave        = WAVE_SAW;
        t->freq        = 850.0f;
        t->freq_start  = 850.0f;
        t->freq_end    = 850.0f;
        t->volume      = 0.35f;
        t->samples_total   = SAMPLE_RATE * 2;
        t->pulse_on_samples  = (SAMPLE_RATE * 40) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 40) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_MASTER_CAUTION:
        /* Single/Double chime */
        t->wave        = WAVE_SINE;
        t->freq        = 600.0f;
        t->freq_start  = 600.0f;
        t->freq_end    = 600.0f;
        t->volume      = 0.30f;
        t->samples_total   = SAMPLE_RATE * 1;
        t->pulse_on_samples  = (SAMPLE_RATE * 150) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 100) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_STALL:
        /* Clacker / Stick shaker simulation: loud low frequency pulsing */
        t->wave        = WAVE_SAW;
        t->freq        = 100.0f;
        t->freq_start  = 100.0f;
        t->freq_end    = 100.0f;
        t->volume      = 0.40f;
        t->samples_total   = SAMPLE_RATE * 2;
        t->pulse_on_samples  = (SAMPLE_RATE * 50) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 50) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_TERRAIN:
        /* Rapid 400Hz square beeps, 100ms on / 100ms off */
        t->wave        = WAVE_SQUARE;
        t->freq        = 400.0f;
        t->volume      = 0.30f;
        t->samples_total   = SAMPLE_RATE * 3;
        t->pulse_on_samples  = (SAMPLE_RATE * 100) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 100) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_SINK_RATE:
        /* Descending sweep 800→300Hz, continuous */
        t->wave        = WAVE_SINE;
        t->freq        = 800.0f;
        t->freq_start  = 800.0f;
        t->freq_end    = 300.0f;
        t->volume      = 0.25f;
        t->samples_total   = SAMPLE_RATE * 2;
        t->pulse_on_samples  = SAMPLE_RATE * 2;  /* continuous */
        t->pulse_off_samples = 0;
        t->pulse_state = 1;
        break;

    case ALERT_TOO_LOW_GEAR:
        /* Steady 400Hz, moderate */
        t->wave        = WAVE_SQUARE;
        t->freq        = 400.0f;
        t->volume      = 0.25f;
        t->samples_total   = SAMPLE_RATE * 2;
        t->pulse_on_samples  = SAMPLE_RATE * 2;
        t->pulse_off_samples = 0;
        t->pulse_state = 1;
        break;

    case ALERT_TOO_LOW_FLAPS:
        /* Steady 300Hz, lower pitch */
        t->wave        = WAVE_SQUARE;
        t->freq        = 300.0f;
        t->volume      = 0.25f;
        t->samples_total   = SAMPLE_RATE * 2;
        t->pulse_on_samples  = SAMPLE_RATE * 2;
        t->pulse_off_samples = 0;
        t->pulse_state = 1;
        break;

    case ALERT_GLIDESLOPE:
        /* Slow alternating 600/400Hz, 300ms each */
        t->wave        = WAVE_SQUARE;
        t->freq        = 600.0f;
        t->freq_start  = 600.0f;
        t->freq_end    = 400.0f;
        t->volume      = 0.22f;
        t->samples_total   = SAMPLE_RATE * 3;
        t->pulse_on_samples  = (SAMPLE_RATE * 300) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 300) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_BANK_ANGLE:
        /* Pulsing 600Hz, 200ms on / 300ms off */
        t->wave        = WAVE_SINE;
        t->freq        = 600.0f;
        t->volume      = 0.25f;
        t->samples_total   = SAMPLE_RATE * 3;
        t->pulse_on_samples  = (SAMPLE_RATE * 200) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 300) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_OVERSPEED:
        /* Continuous high 1000Hz */
        t->wave        = WAVE_SQUARE;
        t->freq        = 1000.0f;
        t->volume      = 0.22f;
        t->samples_total   = SAMPLE_RATE * 2;
        t->pulse_on_samples  = (SAMPLE_RATE * 150) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 100) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_MINIMUMS:
        /* Triple ping: 1000Hz, 100ms each, 100ms gap */
        t->wave        = WAVE_SINE;
        t->freq        = 1000.0f;
        t->volume      = 0.30f;
        t->samples_total   = (SAMPLE_RATE * 600) / 1000;
        t->pulse_on_samples  = (SAMPLE_RATE * 100) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 100) / 1000;
        t->pulse_state = 1;
        break;

    /* =====================================================================
     *  System deviations — distinctive tones for testing
     * ===================================================================== */

    case ALERT_ENG_OVERHEAT:
        /* Rising sweep 400→800Hz, 500ms, 2 beeps */
        t->wave        = WAVE_SAW;
        t->freq_start  = 400.0f;
        t->freq_end    = 800.0f;
        t->volume      = 0.25f;
        t->samples_total   = (SAMPLE_RATE * 500) / 1000;
        t->pulse_on_samples  = t->samples_total;
        t->pulse_off_samples = 0;
        t->pulse_state = 1;
        break;

    case ALERT_ENG_ASYM:
        /* Alternating 500/700Hz, 150ms each */
        t->wave        = WAVE_SQUARE;
        t->freq_start  = 500.0f;
        t->freq_end    = 700.0f;
        t->volume      = 0.25f;
        t->samples_total   = (SAMPLE_RATE * 600) / 1000;
        t->pulse_on_samples  = (SAMPLE_RATE * 150) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 150) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_FUEL_IMBALANCE:
        /* Low 250Hz hum, 300ms */
        t->wave        = WAVE_SINE;
        t->freq        = 250.0f;
        t->volume      = 0.20f;
        t->samples_total   = (SAMPLE_RATE * 300) / 1000;
        t->pulse_on_samples  = t->samples_total;
        t->pulse_off_samples = 0;
        t->pulse_state = 1;
        break;

    case ALERT_OIL_PRESS_LOW:
        /* Slow pulsing 350Hz, 200ms on/400ms off */
        t->wave        = WAVE_SINE;
        t->freq        = 350.0f;
        t->volume      = 0.22f;
        t->samples_total   = SAMPLE_RATE * 2;
        t->pulse_on_samples  = (SAMPLE_RATE * 200) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 400) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_CABIN_ALT_HIGH:
        /* Urgent 900Hz rapid beeps, 100ms on/100ms off */
        t->wave        = WAVE_SQUARE;
        t->freq        = 900.0f;
        t->volume      = 0.28f;
        t->samples_total   = SAMPLE_RATE * 2;
        t->pulse_on_samples  = (SAMPLE_RATE * 100) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 100) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_BUS_VOLT_ABNORM:
        /* Buzzy 120Hz saw, 400ms */
        t->wave        = WAVE_SAW;
        t->freq        = 120.0f;
        t->volume      = 0.18f;
        t->samples_total   = (SAMPLE_RATE * 400) / 1000;
        t->pulse_on_samples  = t->samples_total;
        t->pulse_off_samples = 0;
        t->pulse_state = 1;
        break;

    case ALERT_TAKEOFF_CONFIG:
        /* Double chime: 600Hz, 150ms, gap 100ms, 600Hz, 150ms */
        t->wave        = WAVE_SINE;
        t->freq        = 600.0f;
        t->volume      = 0.25f;
        t->samples_total   = (SAMPLE_RATE * 400) / 1000;
        t->pulse_on_samples  = (SAMPLE_RATE * 150) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 100) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_LOW_FUEL:
        /* Descending tone 500→200Hz, 1s */
        t->wave        = WAVE_SINE;
        t->freq_start  = 500.0f;
        t->freq_end    = 200.0f;
        t->volume      = 0.22f;
        t->samples_total   = SAMPLE_RATE * 1;
        t->pulse_on_samples  = t->samples_total;
        t->pulse_off_samples = 0;
        t->pulse_state = 1;
        break;

    case ALERT_ICING_CONDITION:
        /* Crisp 700Hz, 200ms beeps × 2 */
        t->wave        = WAVE_SINE;
        t->freq        = 700.0f;
        t->volume      = 0.22f;
        t->samples_total   = (SAMPLE_RATE * 500) / 1000;
        t->pulse_on_samples  = (SAMPLE_RATE * 200) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 50) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_APU_FIRE:
        /* Very high 1200Hz fast pulse, urgent */
        t->wave        = WAVE_SQUARE;
        t->freq        = 1200.0f;
        t->volume      = 0.30f;
        t->samples_total   = SAMPLE_RATE * 2;
        t->pulse_on_samples  = (SAMPLE_RATE * 50) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 50) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_OIL_TEMP_HIGH:
        /* Rising 300→600Hz sweep, 600ms */
        t->wave        = WAVE_SAW;
        t->freq_start  = 300.0f;
        t->freq_end    = 600.0f;
        t->volume      = 0.22f;
        t->samples_total   = (SAMPLE_RATE * 600) / 1000;
        t->pulse_on_samples  = t->samples_total;
        t->pulse_off_samples = 0;
        t->pulse_state = 1;
        break;

    case ALERT_HYD_PRESS_LOW:
        /* Heavy 200Hz square, 300ms pulses */
        t->wave        = WAVE_SQUARE;
        t->freq        = 200.0f;
        t->volume      = 0.22f;
        t->samples_total   = SAMPLE_RATE * 2;
        t->pulse_on_samples  = (SAMPLE_RATE * 300) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 300) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_HYD_QTY_LOW:
        /* Thin 180Hz sine, 400ms */
        t->wave        = WAVE_SINE;
        t->freq        = 180.0f;
        t->volume      = 0.20f;
        t->samples_total   = (SAMPLE_RATE * 400) / 1000;
        t->pulse_on_samples  = t->samples_total;
        t->pulse_off_samples = 0;
        t->pulse_state = 1;
        break;

    /* =====================================================================
     *  DREF-only alerts — fire / TCAS / doors / systems
     * ===================================================================== */

    case ALERT_FIRE_ENG1:
        /* Fire bell: VERY loud 850Hz saw, rapid ring */
        t->wave        = WAVE_SAW;
        t->freq        = 850.0f;
        t->volume      = 0.38f;
        t->samples_total   = SAMPLE_RATE * 3;
        t->pulse_on_samples  = (SAMPLE_RATE * 30) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 30) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_FIRE_ENG2:
        /* Same as Eng1 but slightly different pitch (880Hz) to distinguish */
        t->wave        = WAVE_SAW;
        t->freq        = 880.0f;
        t->volume      = 0.38f;
        t->samples_total   = SAMPLE_RATE * 3;
        t->pulse_on_samples  = (SAMPLE_RATE * 30) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 30) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_FIRE_APU:
        /* APU fire: 1100Hz square rapid beep, 2s */
        t->wave        = WAVE_SQUARE;
        t->freq        = 1100.0f;
        t->volume      = 0.35f;
        t->samples_total   = SAMPLE_RATE * 2;
        t->pulse_on_samples  = (SAMPLE_RATE * 40) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 40) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_FIRE_WHEEL_WELL:
        /* Wheel well: 750Hz, slower pulse */
        t->wave        = WAVE_SQUARE;
        t->freq        = 750.0f;
        t->volume      = 0.32f;
        t->samples_total   = SAMPLE_RATE * 2;
        t->pulse_on_samples  = (SAMPLE_RATE * 100) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 100) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_FIRE_CARGO:
        /* Cargo: 650Hz alternating with 500Hz */
        t->wave        = WAVE_SQUARE;
        t->freq_start  = 650.0f;
        t->freq_end    = 500.0f;
        t->volume      = 0.30f;
        t->samples_total   = SAMPLE_RATE * 3;
        t->pulse_on_samples  = (SAMPLE_RATE * 150) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 150) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_TCAS_TA:
        /* TCAS TA: Soft triple ping at 1000Hz, 50ms each */
        t->wave        = WAVE_SINE;
        t->freq        = 1000.0f;
        t->volume      = 0.25f;
        t->samples_total   = (SAMPLE_RATE * 350) / 1000;
        t->pulse_on_samples  = (SAMPLE_RATE * 50) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 100) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_TCAS_RA:
        /* TCAS RA: Loud double ping 1200+1000Hz alternating */
        t->wave        = WAVE_SQUARE;
        t->freq_start  = 1200.0f;
        t->freq_end    = 1000.0f;
        t->volume      = 0.32f;
        t->samples_total   = SAMPLE_RATE * 1;
        t->pulse_on_samples  = (SAMPLE_RATE * 100) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 100) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_STALL_WARNING:
        /* Stick shaker: Low 80Hz loud saw, rapid pulse */
        t->wave        = WAVE_SAW;
        t->freq        = 80.0f;
        t->volume      = 0.40f;
        t->samples_total   = SAMPLE_RATE * 2;
        t->pulse_on_samples  = (SAMPLE_RATE * 40) / 1000;
        t->pulse_off_samples = (SAMPLE_RATE * 40) / 1000;
        t->pulse_state = 1;
        break;

    case ALERT_DOOR_OPEN:
        /* Single 500Hz chime, 200ms */
        t->wave        = WAVE_SINE;
        t->freq        = 500.0f;
        t->volume      = 0.20f;
        t->samples_total   = (SAMPLE_RATE * 200) / 1000;
        t->pulse_on_samples  = t->samples_total;
        t->pulse_off_samples = 0;
        t->pulse_state = 1;
        break;

    case ALERT_ELEC_FAULT:
        /* Electrical buzz: 60Hz saw, 500ms */
        t->wave        = WAVE_SAW;
        t->freq        = 60.0f;
        t->volume      = 0.18f;
        t->samples_total   = (SAMPLE_RATE * 500) / 1000;
        t->pulse_on_samples  = t->samples_total;
        t->pulse_off_samples = 0;
        t->pulse_state = 1;
        break;

    case ALERT_ANTI_ICE_FAULT:
        /* Cold crisp ping: 900Hz sine, 150ms */
        t->wave        = WAVE_SINE;
        t->freq        = 900.0f;
        t->volume      = 0.20f;
        t->samples_total   = (SAMPLE_RATE * 150) / 1000;
        t->pulse_on_samples  = t->samples_total;
        t->pulse_off_samples = 0;
        t->pulse_state = 1;
        break;

    case ALERT_AP_DISENGAGE:
        /* AP disconnect wail: 400→200Hz sweep, 800ms */
        t->wave        = WAVE_SINE;
        t->freq_start  = 400.0f;
        t->freq_end    = 200.0f;
        t->volume      = 0.25f;
        t->samples_total   = (SAMPLE_RATE * 800) / 1000;
        t->pulse_on_samples  = t->samples_total;
        t->pulse_off_samples = 0;
        t->pulse_state = 1;
        break;

    case ALERT_AT_DISENGAGE:
        /* AT disconnect: 300→150Hz sweep, 600ms */
        t->wave        = WAVE_SINE;
        t->freq_start  = 300.0f;
        t->freq_end    = 150.0f;
        t->volume      = 0.22f;
        t->samples_total   = (SAMPLE_RATE * 600) / 1000;
        t->pulse_on_samples  = t->samples_total;
        t->pulse_off_samples = 0;
        t->pulse_state = 1;
        break;

    default:
        t->active = 0;
        break;
    }
}

/* =========================================================================
 *  Internal: altitude callout tone
 * ========================================================================= */

/**
 * @brief Play a short altitude callout ping.
 * @param alt_ft  The altitude we're calling out.
 */
static void play_altitude_callout(AlertSystem* as, float alt_ft)
{
    /* Find a free tone slot (lower-priority or inactive) */
    int slot = -1;
    for (int i = 0; i < MAX_TONES; i++) {
        if (!as->tones[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return;  /* all slots busy */

    ToneSlot* t = &as->tones[slot];
    memset(t, 0, sizeof(*t));
    t->active  = 1;
    t->type    = ALERT_MINIMUMS;  /* reuse priority level */
    t->wave    = WAVE_SINE;
    t->volume  = 0.20f;
    t->samples_total = (SAMPLE_RATE * 150) / 1000; /* 150ms ping */
    t->pulse_on_samples  = t->samples_total;
    t->pulse_off_samples = 0;
    t->pulse_state = 1;

    /* Frequency: higher for lower altitudes */
    if (alt_ft >= 400.0f)      t->freq = 500.0f;
    else if (alt_ft >= 100.0f) t->freq = 600.0f;
    else if (alt_ft >= 50.0f)  t->freq = 800.0f;
    else                       t->freq = 1000.0f;
}

/* =========================================================================
 *  Audio callback — called from SDL audio thread
 * ========================================================================= */

static void audio_callback(void* userdata, Uint8* stream, int len)
{
    AlertSystem* as = (AlertSystem*)userdata;
    int16_t* buf = (int16_t*)stream;
    int total_samples = len / (int)sizeof(int16_t);

    /* Lock tone array briefly to snapshot active tones */
    SDL_LockMutex(as->tone_mutex);

    for (int s = 0; s < total_samples; s++) {
        float mixed = 0.0f;

        for (int i = 0; i < MAX_TONES; i++) {
            ToneSlot* t = &as->tones[i];
            if (!t->active) continue;

            /* Pulse on/off gating */
            if (t->pulse_on_samples > 0 || t->pulse_off_samples > 0) {
                t->pulse_counter++;
                int threshold = t->pulse_state ? t->pulse_on_samples
                                               : t->pulse_off_samples;
                if (t->pulse_counter >= threshold && threshold > 0) {
                    t->pulse_counter = 0;
                    t->pulse_state   = !t->pulse_state;
                }
            }

            float sample_val = 0.0f;

            if (t->pulse_state) {
                /* Frequency sweep */
                float progress = (float)t->samples_elapsed
                                 / (float)t->samples_total;
                if (t->samples_total <= 0) progress = 0.0f;
                if (progress > 1.0f) progress = 1.0f;

                float freq = t->freq_start
                             + (t->freq_end - t->freq_start) * progress;

                /* Phase increment */
                float phase_inc = freq / (float)SAMPLE_RATE;
                t->phase += phase_inc;
                if (t->phase >= 1.0f) t->phase -= 1.0f;

                /* Generate waveform */
                switch (t->wave) {
                case WAVE_SINE:
                    sample_val = sinf(t->phase * 2.0f * (float)M_PI);
                    break;
                case WAVE_SQUARE:
                    sample_val = (t->phase < 0.5f) ? 1.0f : -1.0f;
                    break;
                case WAVE_SAW:
                    sample_val = (t->phase * 2.0f) - 1.0f;
                    break;
                }
            }

            mixed += sample_val * t->volume;

            /* Advance elapsed counter, deactivate when done */
            t->samples_elapsed++;
            if (t->samples_elapsed >= t->samples_total && t->samples_total > 0) {
                t->active = 0;
            }
        }

        /* Clamp and quantize */
        if (mixed > 1.0f)  mixed = 1.0f;
        if (mixed < -1.0f) mixed = -1.0f;

        buf[s] = (int16_t)(mixed * 8000.0f);
    }

    SDL_UnlockMutex(as->tone_mutex);

    /* Fill silence after all tones complete */
    (void)userdata;
}

/* =========================================================================
 *  Lifecycle
 * ========================================================================= */

AlertSystem* alert_system_create(void)
{
    AlertSystem* as = calloc(1, sizeof(AlertSystem));
    if (!as) {
        LOG_ERROR("Out of memory allocating AlertSystem");
        return NULL;
    }

    as->enabled = 1;

    as->tone_mutex = SDL_CreateMutex();
    if (!as->tone_mutex) {
        LOG_ERROR("Failed to create tone mutex: %s", SDL_GetError());
        free(as);
        return NULL;
    }

    SDL_AudioSpec want, have;
    SDL_memset(&want, 0, sizeof(want));
    want.freq     = SAMPLE_RATE;
    want.format   = AUDIO_FORMAT;
    want.channels = CHANNELS;
    want.samples  = 512;
    want.callback = audio_callback;
    want.userdata = as;

    as->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                   SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (as->dev == 0) {
        LOG_WARN("Audio device not available — alerts disabled: %s", SDL_GetError());
        as->audio_ok = 0;
        SDL_DestroyMutex(as->tone_mutex);
        as->tone_mutex = NULL;
        /* Return the struct anyway — alert_system_update becomes a no-op */
        return as;
    }

    as->audio_ok = 1;
    LOG_INFO("Audio device opened: %d Hz, %s, %d channels",
             have.freq,
             (have.format == AUDIO_S16SYS) ? "S16" : "other",
             have.channels);

    /* Start playback (callback will be called even for silence) */
    SDL_PauseAudioDevice(as->dev, 0);

    return as;
}

void alert_system_destroy(AlertSystem* as)
{
    if (!as) return;
    if (as->audio_ok && as->dev > 0) {
        SDL_CloseAudioDevice(as->dev);
    }
    if (as->tone_mutex) {
        SDL_DestroyMutex(as->tone_mutex);
    }
    free(as);
    LOG_DEBUG("AlertSystem destroyed");
}

void alert_system_set_enabled(AlertSystem* as, int enabled)
{
    if (!as) return;
    as->enabled = enabled;
    if (!enabled && as->audio_ok) {
        /* Clear all active tones when muting */
        if (as->tone_mutex) SDL_LockMutex(as->tone_mutex);
        for (int i = 0; i < MAX_TONES; i++) {
            as->tones[i].active = 0;
        }
        if (as->tone_mutex) SDL_UnlockMutex(as->tone_mutex);
    }
    LOG_INFO("Alert system %s", enabled ? "enabled" : "muted");
}

int alert_system_audio_ok(const AlertSystem* as)
{
    return (as && as->audio_ok) ? 1 : 0;
}

/* =========================================================================
 *  Internal: start a tone
 * ========================================================================= */

static void tone_start(AlertSystem* as, AlertType type)
{
    if (!as || !as->audio_ok) return;

    int priority = alert_priority[type];

    SDL_LockMutex(as->tone_mutex);

    /* Find a free slot, or evict the lowest-priority active tone */
    int slot = -1;
    int lowest_prio = -1;
    int lowest_slot = -1;

    for (int i = 0; i < MAX_TONES; i++) {
        if (!as->tones[i].active) {
            slot = i;
            break;
        }
        int p = alert_priority[as->tones[i].type];
        if (p > lowest_prio) {
            lowest_prio = p;
            lowest_slot = i;
        }
    }

    if (slot < 0) {
        /* All busy — can we evict? */
        if (priority < lowest_prio) {
            slot = lowest_slot;
        } else {
            /* Our priority is lower than all active — skip */
            SDL_UnlockMutex(as->tone_mutex);
            return;
        }
    }

    configure_tone(&as->tones[slot], type);
    SDL_UnlockMutex(as->tone_mutex);
}

/* =========================================================================
 *  Alert condition evaluation
 * ========================================================================= */

static void evaluate_alerts(AlertSystem* as, const FlightDataValues* fd, float dt)
{
    if (!as || !as->enabled) return;

    float agl_ft     = fd->altitude_agl_ft;
    float vs_fpm     = fd->vs_fpm;
    float ias_kts    = fd->ias_kts;
    float roll_deg   = fd->roll_deg;
    float flaps      = fd->flap_ratio;
    int   gear_down  = fd->gear_deployed;
    float gs_kts     = fd->gs_kts;

    /* Update cooldown timers */
    for (int i = 0; i < ALERT_TYPE_COUNT; i++) {
        if (as->cooldown_timer[i] > 0.0f) {
            as->cooldown_timer[i] -= dt;
        }
    }

    /* --- 1. PULL UP (critical) --------------------------------------------
     * AGL < 100ft, sink rate > 2000 fpm, not in landing flare
     * OR: AGL < 50ft and still descending fast */
    if (((agl_ft < 100.0f && vs_fpm < -2000.0f) || fd->dref_gpws) && as->cooldown_timer[ALERT_PULL_UP] <= 0.0f) {
        tone_start(as, ALERT_PULL_UP);
        as->cooldown_timer[ALERT_PULL_UP] = alert_cooldown[ALERT_PULL_UP];
        LOG_DEBUG("ALERT: PULL UP (AGL=%.0f, VS=%.0f)", (double)agl_ft, (double)vs_fpm);
    }

    /* --- Stall Warning ----------------------------------------------------- */
    if (((ias_kts < 110.0f && agl_ft > 0.0f) || fd->dref_stall_warning) && as->cooldown_timer[ALERT_STALL] <= 0.0f) {
        tone_start(as, ALERT_STALL);
        as->cooldown_timer[ALERT_STALL] = alert_cooldown[ALERT_STALL];
        LOG_DEBUG("ALERT: STALL (IAS=%.0f)", (double)ias_kts);
    }

    /* --- Master Warning / Fire --------------------------------------------- */
    if (fd->master_warning && as->cooldown_timer[ALERT_MASTER_WARNING] <= 0.0f) {
        tone_start(as, ALERT_MASTER_WARNING);
        as->cooldown_timer[ALERT_MASTER_WARNING] = alert_cooldown[ALERT_MASTER_WARNING];
        LOG_DEBUG("ALERT: MASTER WARNING");
    }

    /* --- Master Caution ---------------------------------------------------- */
    if (fd->master_caution && as->cooldown_timer[ALERT_MASTER_CAUTION] <= 0.0f) {
        tone_start(as, ALERT_MASTER_CAUTION);
        as->cooldown_timer[ALERT_MASTER_CAUTION] = alert_cooldown[ALERT_MASTER_CAUTION];
        LOG_DEBUG("ALERT: MASTER CAUTION");
    }

    /* --- 2. WINDSHEAR ------------------------------------------------------
     * Sudden IAS change > 20 kts in 1 second */
    as->ias_delta = ias_kts - as->last_ias_kts;
    as->last_ias_kts = ias_kts;
    if (((fabsf(as->ias_delta) > 20.0f && agl_ft < 2000.0f) || fd->dref_windshear)
        && as->cooldown_timer[ALERT_WINDSHEAR] <= 0.0f) {
        tone_start(as, ALERT_WINDSHEAR);
        as->cooldown_timer[ALERT_WINDSHEAR] = alert_cooldown[ALERT_WINDSHEAR];
        LOG_DEBUG("ALERT: WINDSHEAR (dIAS=%.0f, AGL=%.0f)",
                  (double)as->ias_delta, (double)agl_ft);
    }

    /* --- 3. TERRAIN --------------------------------------------------------
     * AGL < 1000ft, descending > 1500 fpm */
    if (agl_ft < 1000.0f && vs_fpm < -1500.0f && agl_ft > 100.0f
        && as->cooldown_timer[ALERT_TERRAIN] <= 0.0f) {
        tone_start(as, ALERT_TERRAIN);
        as->cooldown_timer[ALERT_TERRAIN] = alert_cooldown[ALERT_TERRAIN];
        LOG_DEBUG("ALERT: TERRAIN (AGL=%.0f, VS=%.0f)", (double)agl_ft, (double)vs_fpm);
    }

    /* --- 4. SINK RATE ------------------------------------------------------
     * AGL < 2500ft, sink rate > 1500 fpm */
    if (agl_ft < 2500.0f && vs_fpm < -1500.0f && agl_ft > 500.0f
        && as->cooldown_timer[ALERT_SINK_RATE] <= 0.0f) {
        tone_start(as, ALERT_SINK_RATE);
        as->cooldown_timer[ALERT_SINK_RATE] = alert_cooldown[ALERT_SINK_RATE];
        LOG_DEBUG("ALERT: SINK RATE (AGL=%.0f, VS=%.0f)", (double)agl_ft, (double)vs_fpm);
    }

    /* --- 5. TOO LOW GEAR ---------------------------------------------------
     * AGL < 500ft, gear not down, descending */
    if (agl_ft < 500.0f && !gear_down && vs_fpm < -50.0f
        && as->cooldown_timer[ALERT_TOO_LOW_GEAR] <= 0.0f) {
        tone_start(as, ALERT_TOO_LOW_GEAR);
        as->cooldown_timer[ALERT_TOO_LOW_GEAR] = alert_cooldown[ALERT_TOO_LOW_GEAR];
        LOG_DEBUG("ALERT: TOO LOW GEAR (AGL=%.0f)", (double)agl_ft);
    }

    /* --- 6. TOO LOW FLAPS --------------------------------------------------
     * AGL < 500ft, flaps < approach config, descending */
    if (agl_ft < 500.0f && flaps < 0.25f && vs_fpm < -50.0f
        && as->cooldown_timer[ALERT_TOO_LOW_FLAPS] <= 0.0f) {
        tone_start(as, ALERT_TOO_LOW_FLAPS);
        as->cooldown_timer[ALERT_TOO_LOW_FLAPS] = alert_cooldown[ALERT_TOO_LOW_FLAPS];
        LOG_DEBUG("ALERT: TOO LOW FLAPS (AGL=%.0f, flaps=%.2f)",
                  (double)agl_ft, (double)flaps);
    }

    /* --- 7. GLIDESLOPE -----------------------------------------------------
     * CDI > 0.5 dots (0.5 * 0.5° = 0.25° deviation) while below 1000ft */
    if (agl_ft < 1000.0f && fabsf(fd->nav1_cdi) > 0.5f
        && as->cooldown_timer[ALERT_GLIDESLOPE] <= 0.0f) {
        tone_start(as, ALERT_GLIDESLOPE);
        as->cooldown_timer[ALERT_GLIDESLOPE] = alert_cooldown[ALERT_GLIDESLOPE];
        LOG_DEBUG("ALERT: GLIDESLOPE (CDI=%.2f)", (double)fd->nav1_cdi);
    }

    /* --- 8. BANK ANGLE -----------------------------------------------------
     * |roll| > 35° below 2000ft AGL */
    if (((agl_ft < 2000.0f && fabsf(roll_deg) > 35.0f) || fd->dref_bank_angle)
        && as->cooldown_timer[ALERT_BANK_ANGLE] <= 0.0f) {
        tone_start(as, ALERT_BANK_ANGLE);
        as->cooldown_timer[ALERT_BANK_ANGLE] = alert_cooldown[ALERT_BANK_ANGLE];
        LOG_DEBUG("ALERT: BANK ANGLE (roll=%.0f, AGL=%.0f)",
                  (double)roll_deg, (double)agl_ft);
    }

    /* --- 9. OVERSPEED ------------------------------------------------------
     * IAS > 340 kts (737-800 Vmo/Mmo equivalent) */
    if ((ias_kts > 340.0f || fd->dref_overspeed) && as->cooldown_timer[ALERT_OVERSPEED] <= 0.0f) {
        tone_start(as, ALERT_OVERSPEED);
        as->cooldown_timer[ALERT_OVERSPEED] = alert_cooldown[ALERT_OVERSPEED];
        LOG_DEBUG("ALERT: OVERSPEED (IAS=%.0f)", (double)ias_kts);
    }

    /* --- 10. MINIMUMS ------------------------------------------------------
     * At 200ft AGL decision height */
    {
        float decision_height = 200.0f;
        if (as->last_agl_ft > decision_height && agl_ft <= decision_height
            && !as->minim_triggered
            && as->cooldown_timer[ALERT_MINIMUMS] <= 0.0f) {
            tone_start(as, ALERT_MINIMUMS);
            as->cooldown_timer[ALERT_MINIMUMS] = alert_cooldown[ALERT_MINIMUMS];
            as->minim_triggered = 1;
            LOG_DEBUG("ALERT: MINIMUMS (%.0f ft)", (double)agl_ft);
        }
        /* Reset when climbing back above 500ft */
        if (agl_ft > 500.0f) {
            as->minim_triggered = 0;
        }
    }

    /* --- 11. Altitude callouts --------------------------------------------
     * Specific altitudes: 500, 400, 300, 200, 100, 50, 40, 30, 20, 10 ft */
    {
        static const int callout_alts[] = {500, 400, 300, 200, 100,
                                            50, 40, 30, 20, 10};
        int num_callouts = (int)(sizeof(callout_alts) / sizeof(callout_alts[0]));

        for (int i = 0; i < num_callouts; i++) {
            int alt = callout_alts[i];
            float alt_f = (float)alt;

            /* Detect crossing from above */
            if (as->last_agl_ft > alt_f && agl_ft <= alt_f
                && !as->callouts_done[i] && vs_fpm < -50.0f) {
                play_altitude_callout(as, alt_f);
                as->callouts_done[i] = 1;
            }
        }

        /* Reset callout flags when climbing above 600ft */
        if (agl_ft > 600.0f) {
            memset(as->callouts_done, 0, sizeof(as->callouts_done));
        }
    }

    /* Store AGL for next frame's crossing detection */
    as->last_agl_ft = agl_ft;
}

/* =========================================================================
 *  Public API
 * ========================================================================= */

void alert_system_update(AlertSystem* as, const FlightDataValues* fd, float dt)
{
    if (!as || !as->enabled) return;
    if (!fd) return;

    evaluate_alerts(as, fd, dt);

    /* Store for next frame */
    memcpy(&as->prev_fd, fd, sizeof(FlightDataValues));
}

/* =========================================================================
 *  Alert state query (for AI / external consumers)
 * ========================================================================= */

void alert_system_get_active_alerts(const AlertSystem* as,
                                    const FlightDataValues* fd,
                                    int* out_states)
{
    if (!fd || !out_states) return;
    memset(out_states, 0, sizeof(int) * ALERT_ID_COUNT);

    float agl_ft    = fd->altitude_agl_ft;
    float vs_fpm    = fd->vs_fpm;
    float ias_kts   = fd->ias_kts;
    float roll_deg  = fd->roll_deg;
    float flaps     = fd->flap_ratio;
    int   gear_down = fd->gear_deployed;

    /* === GPWS alerts (13) — same thresholds as evaluate_alerts() === */

    /* PULL_UP: AGL < 100ft, sink > 2000 fpm, not landing */
    if (agl_ft < 100.0f && vs_fpm < -2000.0f)
        out_states[ALERT_ID_PULL_UP] = 1;

    /* WINDSHEAR (approximate: check dref_windshear DREF first) */
    if (fd->dref_windshear)
        out_states[ALERT_ID_WINDSHEAR] = 1;
    else if (as) {
        /* PENDING: IAS delta check needs recent history.
         * For now, mark active if DREF says so. The audio path already handles
         * this correctly. Future: expose ias_delta from AlertSystem state. */
    }

    /* MASTER_WARNING / MASTER_CAUTION */
    if (fd->master_warning)
        out_states[ALERT_ID_MASTER_WARNING] = 1;
    if (fd->master_caution)
        out_states[ALERT_ID_MASTER_CAUTION] = 1;

    /* TERRAIN: 100 < AGL < 1000, sink > 1500 fpm */
    if (agl_ft < 1000.0f && vs_fpm < -1500.0f && agl_ft > 100.0f)
        out_states[ALERT_ID_TERRAIN] = 1;

    /* SINK_RATE: 500 < AGL < 2500, sink > 1500 fpm */
    if (agl_ft < 2500.0f && vs_fpm < -1500.0f && agl_ft > 500.0f)
        out_states[ALERT_ID_SINK_RATE] = 1;

    /* TOO_LOW_GEAR: AGL < 500ft, gear up, descending */
    if (agl_ft < 500.0f && !gear_down && vs_fpm < -50.0f)
        out_states[ALERT_ID_TOO_LOW_GEAR] = 1;

    /* TOO_LOW_FLAPS: AGL < 500ft, flaps < 25%, descending */
    if (agl_ft < 500.0f && flaps < 0.25f && vs_fpm < -50.0f)
        out_states[ALERT_ID_TOO_LOW_FLAPS] = 1;

    /* GLIDESLOPE: AGL < 1000ft, CDI > 0.5 dots */
    if (agl_ft < 1000.0f && fabsf(fd->nav1_cdi) > 0.5f)
        out_states[ALERT_ID_GLIDESLOPE] = 1;

    /* BANK_ANGLE: |roll| > 35° below 2000ft AGL */
    if (agl_ft < 2000.0f && fabsf(roll_deg) > 35.0f)
        out_states[ALERT_ID_BANK_ANGLE] = 1;

    /* OVERSPEED: IAS > 340 kts */
    if (ias_kts > 340.0f)
        out_states[ALERT_ID_OVERSPEED] = 1;

    /* STALL: IAS < 110 kts, airborne */
    if (ias_kts < 110.0f && agl_ft > 0.0f)
        out_states[ALERT_ID_STALL] = 1;

    /* MINIMUMS: AGL within ±100ft of 200ft, descending */
    if (agl_ft > 100.0f && agl_ft < 300.0f && vs_fpm < -50.0f)
        out_states[ALERT_ID_MINIMUMS] = 1;

    /* === System deviations (13) === */

    /* ENG_OVERHEAT: EGT > 850°C */
    {
        float egt_max = fd->egt_c[0];
        for (int i = 1; i < 4; i++)
            if (fd->egt_c[i] > egt_max) egt_max = fd->egt_c[i];
        if (egt_max > 850.0f)
            out_states[ALERT_ID_ENG_OVERHEAT] = 1;
    }

    /* ENG_ASYM: N1 diff > 5% */
    {
        float n1_diff = fabsf(fd->n1_pct[0] - fd->n1_pct[1]);
        if (n1_diff > 5.0f)
            out_states[ALERT_ID_ENG_ASYM] = 1;
    }

    /* FUEL_IMBALANCE: estimated diff > 1000 lbs (we approximate from N1 diff) */
    {
        float n1_diff_pct = fabsf(fd->n1_pct[0] - fd->n1_pct[1]);
        float estim_imb = n1_diff_pct * 200.0f;  /* rough heuristic */
        if (estim_imb > 1000.0f)
            out_states[ALERT_ID_FUEL_IMBALANCE] = 1;
    }

    /* OIL_PRESS_LOW: min oil press < 25 psi */
    {
        float oil_min = fd->oil_press_psi[0];
        for (int i = 1; i < 2; i++)
            if (fd->oil_press_psi[i] < oil_min) oil_min = fd->oil_press_psi[i];
        if (oil_min < 25.0f)
            out_states[ALERT_ID_OIL_PRESS_LOW] = 1;
    }

    /* CABIN_ALT_HIGH: cabin alt > 10000 ft */
    if (fd->cabin_alt_ft > 10000.0f)
        out_states[ALERT_ID_CABIN_ALT_HIGH] = 1;

    /* BUS_VOLT_ABNORM: voltage outside 24-32V */
    if (fd->elec_bus_volts < 24.0f || fd->elec_bus_volts > 32.0f)
        out_states[ALERT_ID_BUS_VOLT_ABNORM] = 1;

    /* TAKEOFF_CONFIG: flaps < 10% below 100ft (simplified — phase unknown) */
    if (agl_ft < 100.0f && flaps < 0.10f && vs_fpm > 50.0f)
        out_states[ALERT_ID_TAKEOFF_CONFIG] = 1;

    /* LOW_FUEL: total < 3000 lbs */
    if (fd->fuel_total_lbs < 3000.0f && fd->fuel_total_lbs > 0.0f)
        out_states[ALERT_ID_LOW_FUEL] = 1;

    /* ICING_CONDITION: OAT < 10°C, anti-ice off, airborne */
    if (fd->oat_c < 10.0f && !fd->anti_ice_wing && agl_ft > 100.0f)
        out_states[ALERT_ID_ICING_CONDITION] = 1;

    /* APU_FIRE: APU EGT > 760°C, APU running */
    if (fd->apu_egt_c > 760.0f && fd->apu_running)
        out_states[ALERT_ID_APU_FIRE] = 1;

    /* OIL_TEMP_HIGH: max oil temp > 120°C */
    {
        float oil_tmax = fd->oil_temp_c[0];
        for (int i = 1; i < 2; i++)
            if (fd->oil_temp_c[i] > oil_tmax) oil_tmax = fd->oil_temp_c[i];
        if (oil_tmax > 120.0f)
            out_states[ALERT_ID_OIL_TEMP_HIGH] = 1;
    }

    /* HYD_PRESS_LOW: use DREF numeric values if available, else UDP */
    {
        float hp0 = fd->dref_hyd_press_psi[0];
        float hp1 = fd->dref_hyd_press_psi[1];
        if (hp0 > 0.0f || hp1 > 0.0f) {
            /* DREF values available */
            if ((hp0 > 0.0f && hp0 < 2800.0f) || (hp1 > 0.0f && hp1 < 2800.0f))
                out_states[ALERT_ID_HYD_PRESS_LOW] = 1;
        } else {
            /* Fall back to UDP hyd_press_psi (may not be received) */
            float hmin = fd->hyd_press_psi[0];
            for (int i = 1; i < 2; i++)
                if (fd->hyd_press_psi[i] < hmin) hmin = fd->hyd_press_psi[i];
            if (hmin > 100.0f && hmin < 2800.0f)
                out_states[ALERT_ID_HYD_PRESS_LOW] = 1;
        }
    }

    /* HYD_QTY_LOW: DREF numeric values */
    {
        float hq0 = fd->dref_hyd_qty_pct[0];
        float hq1 = fd->dref_hyd_qty_pct[1];
        if (hq0 > 0.0f || hq1 > 0.0f) {
            if ((hq0 > 0.0f && hq0 < 0.20f) || (hq1 > 0.0f && hq1 < 0.20f))
                out_states[ALERT_ID_HYD_QTY_LOW] = 1;
        }
    }

    /* === DREF-only alerts (13) — read directly from RREF-populated fields === */

    if (fd->dref_engine_fire) {
        /* engine_fire DREF is an array; index 0=Eng1, 1=Eng2. We only get
         * the first value via RREF. For full coverage, subscribe to both indices.
         * For now: treat as "any engine fire". */
        out_states[ALERT_ID_FIRE_ENG1] = 1;  /* conservative: assume Eng1 */
    }
    if (fd->dref_fire_warning)
        out_states[ALERT_ID_FIRE_WHEEL_WELL] = 1;

    if (fd->dref_ap_disconnect)
        out_states[ALERT_ID_AP_DISENGAGE] = 1;

    /* AT disconnect: detected as AP_ATHR going 1→0 (edge). For now use DREF.
     * PENDING: add proper edge detection. */
    if (fd->dref_ap_disconnect)  /* share same DREF for now */
        out_states[ALERT_ID_AT_DISENGAGE] = 1;

    if (fd->dref_door)
        out_states[ALERT_ID_DOOR_OPEN] = 1;

    if (fd->dref_generator)
        out_states[ALERT_ID_ELEC_FAULT] = 1;

    if (fd->dref_anti_ice)
        out_states[ALERT_ID_ANTI_ICE_FAULT] = 1;

    if (fd->dref_stall_warning)
        out_states[ALERT_ID_STALL_WARNING] = 1;

    /* Fire alerts: use engine_fire + fire_warning + apu_fire (derived) */
    if (fd->dref_hyd_pressure)
        out_states[ALERT_ID_HYD_PRESS_LOW] = 1;
    if (fd->dref_hyd_quantity)
        out_states[ALERT_ID_HYD_QTY_LOW] = 1;

    if (fd->dref_cabin_altitude)
        out_states[ALERT_ID_CABIN_ALT_HIGH] = 1;

    if (fd->dref_fuel_quantity)
        out_states[ALERT_ID_LOW_FUEL] = 1;

    if (fd->dref_oil_pressure)
        out_states[ALERT_ID_OIL_PRESS_LOW] = 1;

    if (fd->dref_oil_temperature)
        out_states[ALERT_ID_OIL_TEMP_HIGH] = 1;

    if (fd->dref_voltage)
        out_states[ALERT_ID_BUS_VOLT_ABNORM] = 1;

    if (fd->dref_pressurization)
        out_states[ALERT_ID_CABIN_ALT_HIGH] = 1;

    if (fd->dref_ice)
        out_states[ALERT_ID_ICING_CONDITION] = 1;

    /* TCAS: not yet implemented (needs sim/cockpit2/tcas/indicators/* DREFs) */
    /* APU fire: covered by ALERT_ID_APU_FIRE (derived from EGT) */

    /* Mark as inactive until we have proper TCAS DREF subscriptions */
    /* (out_states[ALERT_ID_TCAS_TA] and ALERT_ID_TCAS_RA remain 0 for now) */
}

/* =========================================================================
 *  Test function — play distinctive tone for every active alert
 * ========================================================================= */

/**
 * @brief AlertID → internal AlertType mapping.
 * ALERT_ID_* (0..38) maps 1:1 to ALERT_* (0..38) since both enums are
 * defined in the same order.
 */
void alert_system_test_beeps(AlertSystem* as, const FlightDataValues* fd)
{
    if (!as || !fd) return;
    if (!as->audio_ok) return;

    /* Get all active alerts */
    int alert_states[ALERT_ID_COUNT];
    alert_system_get_active_alerts(as, fd, alert_states);

    /* Update cooldown timers */
    float dt = 0.05f;  /* rough: called at ~20 Hz */
    for (int i = 0; i < ALERT_TYPE_COUNT; i++) {
        if (as->cooldown_timer[i] > 0.0f) {
            as->cooldown_timer[i] -= dt;
        }
    }

    /* Trigger tone for each active alert (if not on cooldown) */
    for (int id = 0; id < ALERT_ID_COUNT; id++) {
        if (!alert_states[id]) continue;
        if (as->cooldown_timer[id] <= 0.0f) {
            /* Internal AlertType enum has same order as AlertID enum */
            tone_start(as, (AlertType)id);
            as->cooldown_timer[id] = alert_cooldown[id];
        }
    }

    /* Store copy for next frame */
    memcpy(&as->prev_fd, fd, sizeof(FlightDataValues));
}
