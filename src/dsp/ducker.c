/*
 * Ducker - MIDI-triggered sidechain ducking audio FX
 *
 * A MIDI note triggers an envelope that attenuates the audio signal,
 * producing classic sidechain pumping without needing an audio sidechain input.
 * The envelope can instead sweep a low-pass or high-pass filter between two
 * cutoffs (Target: Filter).
 *
 * Chain host discovers MIDI capability via dlsym("move_audio_fx_on_midi").
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "audio_fx_api_v2.h"

#define SAMPLE_RATE 44100
#define INV_REST_GLIDE 0.0023f  /* one-pole step, about 10 ms: no click when the transport starts or stops */

/* Envelope phases */
enum {
    PHASE_IDLE = 0,
    PHASE_ATTACK,
    PHASE_HOLD,
    PHASE_RELEASE
};

/* Curve types */
enum {
    CURVE_LINEAR = 0,
    CURVE_EXPO,
    CURVE_SCURVE,
    CURVE_PUMP
};

/* Mode types */
enum {
    MODE_TRIGGER = 0,
    MODE_GATE
};

/* What a new trigger does to an envelope that is still running */
enum {
    RETRIG_HARD = 0,   /* restart from fully open */
    RETRIG_SOFT        /* continue from the current duck level */
};

/* What the envelope controls */
enum {
    TARGET_VOLUME = 0,
    TARGET_VOLUME_INV,   /* same order as the options: a number from the host is an index */
    TARGET_FILTER
};

/* Filter types: low-pass or high-pass, 12 or 24 dB per octave */
enum {
    FILTER_LP12 = 0,
    FILTER_LP24,
    FILTER_HP12,
    FILTER_HP24,
    FILTER_TYPES
};

/*
 * Cutoff knobs: 128 log-spaced frequencies (a 7-bit range, like MIDI) from
 * 20Hz (index 0) to 20kHz (index 127), about 1/13 octave per step, so every
 * detent is the same musical interval. The knob value is the index; cards.js
 * shows the frequency.
 */
#define FREQ_STEPS 128
#define FREQ_MIN_HZ 20.0f
#define FREQ_MAX_HZ 20000.0f
#define FILTER_MAX_HZ 20000.0f
/* Top of the Res knob. The resonant peak is about +20 dB (12 dB types) or +15 dB
 * (24 dB types) here and there is no gain compensation, so a loud input clips at
 * the int16 clamp well before the top of the knob. */
#define RESONANCE_MAX 0.9f
#define CUT1_DEFAULT 114      /* 90% of the range, ~10kHz: idle cutoff */
#define CUT2_DEFAULT 13       /* 10% of the range, ~40Hz: cutoff at full duck */
#define RESONANCE_DEFAULT 0.2f
#define FILTER_COEF_STEP 8  /* samples between filter coefficient updates */

typedef struct ducker_instance {
    char module_dir[512];

    /* Parameters */
    int channel;          /* 0=omni, 1-16 */
    int trigger_note;     /* 0-127 */
    int mode;             /* MODE_TRIGGER or MODE_GATE */
    float depth;          /* 0.0-1.0 */
    float attack;         /* 0.0-1.0 → 0-50ms */
    float hold;           /* 0.0-1.0 → 0-500ms */
    float release;        /* 0.0-1.0 → 0-1000ms */
    int curve;            /* CURVE_* */
    float vel_sens;       /* 0.0-1.0 */
    int retrig;           /* RETRIG_HARD or RETRIG_SOFT */
    int target;           /* TARGET_VOLUME, TARGET_FILTER or TARGET_VOLUME_INV */
    int filter_type;      /* FILTER_* */
    int cut1;             /* 0-127 freq index: cutoff while idle */
    int cut2;             /* 0-127 freq index: cutoff at full duck (hold) */
    float resonance;      /* 0.0-1.0 */

    /* Filter state: state-variable filter stages (a 24dB filter runs two),
     * one integrator pair per stage and channel */
    float ic1[2][2];
    float ic2[2][2];
    float f_a1[2], f_a2[2], f_a3[2], f_k[2];
    int coef_countdown;

    /* Envelope state */
    int phase;            /* PHASE_* */
    int phase_pos;        /* sample counter within phase */
    int phase_len;        /* total samples in current phase */
    float vel_depth;      /* computed depth for current trigger */
    float envelope;       /* current envelope value: 1.0=pass, 0.0=max duck */
    int active_notes;     /* count of held notes (for gate mode) */
    float attack_t0;      /* where on the attack curve this trigger starts (0 = from open) */
    float inv_rest;       /* Volume Inv resting gain, glides between 1 - Depth and 1 (transport stopped) */

    /* Last trigger, published as "duck_info" so the UI meter can replay the
     * envelope from wall-clock time (see canvas.js). Times are epoch ms. */
    double trig_ms;       /* note-on time, 0 = never triggered */
    double release_ms;    /* when the release starts, 0 = not known yet (gate held) */
    float trig_depth;
    float trig_t0;        /* attack_t0 of the last trigger */
    int trig_curve;
    float trig_attack_ms;
    float trig_release_ms;

} ducker_instance_t;

static const host_api_v1_t *g_host = NULL;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/*
 * How much the last trigger is ducking right now (1 = none, 1 - depth = full),
 * replayed from wall-clock time like the UI meter replays duck_info, so it
 * stays right when the slot is silent and process_block is not running. Same
 * maths as the envelope in v2_process_block; shape_curve is defined below.
 */
static float shape_curve(int curve, float t, int is_release);

static float duck_level_now(ducker_instance_t *inst) {
    double now = now_ms();
    if (inst->trig_ms <= 0 || now < inst->trig_ms) return 1.0f;

    if (!(inst->release_ms > 0 && now >= inst->release_ms)) {
        double el = now - inst->trig_ms;
        if (inst->trig_attack_ms > 0 && el < inst->trig_attack_ms) {
            float t = inst->trig_t0 + (float)(el / inst->trig_attack_ms);
            return 1.0f - inst->trig_depth * shape_curve(inst->trig_curve, t > 1.0f ? 1.0f : t, 0);
        }
        return 1.0f - inst->trig_depth;
    }
    double r = now - inst->release_ms;
    if (inst->trig_release_ms <= 0 || r >= inst->trig_release_ms) return 1.0f;
    return (1.0f - inst->trig_depth) +
           inst->trig_depth * shape_curve(inst->trig_curve, (float)(r / inst->trig_release_ms), 1);
}

static void ducker_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[DUCKER] %s", msg);
        g_host->log(buf);
    }
}

/* --- Tiny JSON helpers (no allocations) --- */

static int json_get_number(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    *out = (float)atof(p);
    return 0;
}

static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_len - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

/* --- Envelope helpers --- */

static inline float clampf(float x, float lo, float hi) {
    if (!(x >= lo)) return lo;   /* also catches NaN, which would latch in the filter state */
    if (x > hi) return hi;
    return x;
}

static int ms_to_samples(float ms) {
    return (int)(ms * (SAMPLE_RATE / 1000.0f));
}

static int attack_samples(ducker_instance_t *inst) {
    return ms_to_samples(inst->attack * 50.0f);  /* 0-50ms */
}

static int hold_samples(ducker_instance_t *inst) {
    return ms_to_samples(inst->hold * 500.0f);   /* 0-500ms */
}

static int release_samples(ducker_instance_t *inst) {
    return ms_to_samples(inst->release * 1000.0f); /* 0-1000ms */
}

/*
 * Shape a 0-1 time value using the selected curve.
 * For attack: t goes 0→1 as we duck DOWN (envelope goes 1→0)
 * For release: t goes 0→1 as we recover UP (envelope goes 0→1)
 */
static float shape_curve(int curve, float t, int is_release) {
    t = clampf(t, 0.0f, 1.0f);

    switch (curve) {
    case CURVE_EXPO:
        return t * t;

    case CURVE_SCURVE:
        return t * t * (3.0f - 2.0f * t);

    case CURVE_PUMP:
        if (is_release) {
            /* Cubic ease-out with slight overshoot feel */
            float inv = 1.0f - t;
            return 1.0f - inv * inv * inv;
        }
        return t;  /* Linear attack for pump */

    case CURVE_LINEAR:
    default:
        return t;
    }
}

/*
 * Where on the attack curve a duck of this fraction (0-1) of the depth sits:
 * the inverse of shape_curve's attack side. A soft retrigger starts there, so
 * the sweep from the current level keeps the curve's own slope.
 */
static float attack_start_fraction(int curve, float f) {
    f = clampf(f, 0.0f, 1.0f);
    switch (curve) {
    case CURVE_EXPO:
        return sqrtf(f);
    case CURVE_SCURVE: {
        float lo = 0.0f, hi = 1.0f;
        for (int k = 0; k < 24; k++) {
            float mid = 0.5f * (lo + hi);
            if (shape_curve(CURVE_SCURVE, mid, 0) < f) lo = mid; else hi = mid;
        }
        return 0.5f * (lo + hi);
    }
    default:
        return f;   /* linear, and pump's linear attack */
    }
}

static int filter_is_highpass(int type) { return type == FILTER_HP12 || type == FILTER_HP24; }
static int filter_is_24db(int type) { return type == FILTER_LP24 || type == FILTER_HP24; }

static float freq_hz(int index) {
    return FREQ_MIN_HZ * powf(FREQ_MAX_HZ / FREQ_MIN_HZ, (float)index / (FREQ_STEPS - 1));
}

/*
 * Set the filter coefficients for a duck amount (0 = idle, 1 = full duck).
 * The cutoff moves from Cut1 to Cut2 in log frequency, so a half-depth duck
 * lands halfway between them in octaves.
 */
static void filter_update_coeffs(ducker_instance_t *inst, float amount) {
    float hz1 = freq_hz(inst->cut1);
    float hz2 = freq_hz(inst->cut2);
    float hz = hz1 * powf(hz2 / hz1, amount);
    hz = clampf(hz, FREQ_MIN_HZ, FILTER_MAX_HZ);

    /* 12dB: one stage. 24dB: two stages with the Q pair of a 4th-order
     * Butterworth (0.541, 1.307); resonance raises the second one. */
    float res = inst->resonance * inst->resonance * 11.0f;
    float q[2];
    if (filter_is_24db(inst->filter_type)) {
        q[0] = 0.5412f;
        q[1] = 1.3066f + res;
    } else {
        q[0] = 0.707f + res;
        q[1] = q[0];
    }
    float g = tanf(3.14159265f * hz / SAMPLE_RATE);

    for (int st = 0; st < 2; st++) {
        float k = 1.0f / q[st];
        inst->f_k[st] = k;
        inst->f_a1[st] = 1.0f / (1.0f + g * (g + k));
        inst->f_a2[st] = g * inst->f_a1[st];
        inst->f_a3[st] = g * inst->f_a2[st];
    }
}

/* Forget the filter's history, so switching Target or Type does not replay old ringing */
static void reset_filter_state(ducker_instance_t *inst) {
    memset(inst->ic1, 0, sizeof(inst->ic1));
    memset(inst->ic2, 0, sizeof(inst->ic2));
    inst->coef_countdown = 0;
}

static void start_attack(ducker_instance_t *inst) {
    inst->phase = PHASE_ATTACK;
    inst->phase_pos = 0;
    inst->phase_len = attack_samples(inst);
    if (inst->phase_len <= 0) {
        /* Zero attack - jump straight to hold */
        inst->envelope = 1.0f - inst->vel_depth;
        inst->phase = PHASE_HOLD;
        inst->phase_pos = 0;
        inst->phase_len = hold_samples(inst);
        if (inst->phase_len <= 0 && inst->mode == MODE_TRIGGER) {
            /* Zero hold in trigger mode - jump to release */
            inst->phase = PHASE_RELEASE;
            inst->phase_pos = 0;
            inst->phase_len = release_samples(inst);
        }
    }
}

static void start_release(ducker_instance_t *inst) {
    inst->phase = PHASE_RELEASE;
    inst->phase_pos = 0;
    inst->phase_len = release_samples(inst);
    if (inst->phase_len <= 0) {
        inst->phase = PHASE_IDLE;
        inst->envelope = 1.0f;
    }
}

/* --- Audio FX API v2 implementation --- */

static void* v2_create_instance(const char *module_dir, const char *config_json) {
    ducker_log("Creating instance");

    ducker_instance_t *inst = (ducker_instance_t *)calloc(1, sizeof(ducker_instance_t));
    if (!inst) {
        ducker_log("Failed to allocate instance");
        return NULL;
    }

    if (module_dir) {
        strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    }

    /* Defaults */
    inst->channel = 1;        /* Channel 1 */
    inst->trigger_note = 36;  /* C1 */
    inst->mode = MODE_TRIGGER;
    inst->depth = 1.0f;
    inst->attack = 0.1f;      /* 5ms */
    inst->hold = 0.2f;        /* 100ms */
    inst->release = 0.3f;     /* 300ms */
    inst->curve = CURVE_LINEAR;
    inst->vel_sens = 0.0f;
    inst->retrig = RETRIG_SOFT;
    inst->target = TARGET_VOLUME;
    inst->filter_type = FILTER_LP24;
    inst->cut1 = CUT1_DEFAULT;
    inst->cut2 = CUT2_DEFAULT;
    inst->resonance = RESONANCE_DEFAULT;
    filter_update_coeffs(inst, 0.0f);
    inst->phase = PHASE_IDLE;
    inst->envelope = 1.0f;
    inst->inv_rest = 1.0f;
    inst->active_notes = 0;

    ducker_log("Instance created");
    return inst;
}

static void v2_destroy_instance(void *instance) {
    ducker_instance_t *inst = (ducker_instance_t *)instance;
    if (!inst) return;
    ducker_log("Destroying instance");
    free(inst);
}

static void v2_process_block(void *instance, int16_t *audio_inout, int frames) {
    ducker_instance_t *inst = (ducker_instance_t *)instance;
    if (!inst) return;

    /* Volume Inv rests below unity, which would mute the track whenever no
     * trigger arrives. Only a running transport earns that rest; stopped or
     * unknown, it passes the signal through. */
    int running = g_host && g_host->get_clock_status &&
                  g_host->get_clock_status() == MOVE_CLOCK_STATUS_RUNNING;
    float rest_target = running ? 1.0f - inst->depth : 1.0f;

    for (int i = 0; i < frames; i++) {
        /* Advance envelope */
        switch (inst->phase) {
        case PHASE_ATTACK: {
            if (inst->phase_len > 0) {
                /* A soft retrigger starts partway up the curve (attack_t0) and
                 * reaches full duck early; the phase still lasts the full attack. */
                float t = inst->attack_t0 + (float)inst->phase_pos / (float)inst->phase_len;
                float shaped = shape_curve(inst->curve, t, 0);
                /* Attack ducks down: envelope goes from 1.0 to (1.0 - vel_depth) */
                inst->envelope = 1.0f - inst->vel_depth * shaped;
            }
            inst->phase_pos++;
            if (inst->phase_pos >= inst->phase_len) {
                inst->envelope = 1.0f - inst->vel_depth;
                inst->phase = PHASE_HOLD;
                inst->phase_pos = 0;
                inst->phase_len = hold_samples(inst);
                /* Trigger mode: no hold means release. Gate mode: a note that
                 * ended during the attack was let finish, so release now. */
                int release_now = (inst->mode == MODE_TRIGGER)
                    ? inst->phase_len <= 0
                    : inst->active_notes == 0;
                if (release_now) {
                    if (inst->release_ms <= 0) inst->release_ms = now_ms();
                    inst->phase = PHASE_RELEASE;
                    inst->phase_pos = 0;
                    inst->phase_len = release_samples(inst);
                }
            }
            break;
        }
        case PHASE_HOLD: {
            /* Stay at ducked level */
            inst->envelope = 1.0f - inst->vel_depth;
            inst->phase_pos++;
            if (inst->mode == MODE_TRIGGER && inst->phase_pos >= inst->phase_len) {
                /* In trigger mode, hold expires → release */
                if (inst->release_ms <= 0) inst->release_ms = now_ms();  /* Gate note held, then Mode switched */
                inst->phase = PHASE_RELEASE;
                inst->phase_pos = 0;
                inst->phase_len = release_samples(inst);
            }
            /* In gate mode, hold stays until note-off triggers release */
            break;
        }
        case PHASE_RELEASE: {
            if (inst->phase_len > 0) {
                float t = (float)inst->phase_pos / (float)inst->phase_len;
                float shaped = shape_curve(inst->curve, t, 1);
                /* Release recovers: envelope goes from (1.0 - vel_depth) to 1.0 */
                inst->envelope = (1.0f - inst->vel_depth) + inst->vel_depth * shaped;
            }
            inst->phase_pos++;
            if (inst->phase_pos >= inst->phase_len) {
                inst->phase = PHASE_IDLE;
                inst->envelope = 1.0f;
            }
            break;
        }
        case PHASE_IDLE:
        default:
            /* envelope stays at 1.0 (pass-through) */
            break;
        }

        float l, r;

        if (inst->target == TARGET_FILTER) {
            /* Duck amount: 0 = envelope idle (Cut1), 1 = full duck (Cut2) */
            float amount = 1.0f - inst->envelope;
            if (inst->coef_countdown <= 0) {
                filter_update_coeffs(inst, amount);
                inst->coef_countdown = FILTER_COEF_STEP;
            }
            inst->coef_countdown--;

            float in[2] = { (float)audio_inout[i * 2], (float)audio_inout[i * 2 + 1] };
            float out[2];
            int hp = filter_is_highpass(inst->filter_type);
            int stages = filter_is_24db(inst->filter_type) ? 2 : 1;
            for (int c = 0; c < 2; c++) {
                float x = in[c];
                for (int st = 0; st < stages; st++) {
                    float v3 = x - inst->ic2[st][c];
                    float v1 = inst->f_a1[st] * inst->ic1[st][c] + inst->f_a2[st] * v3;
                    float v2 = inst->ic2[st][c] + inst->f_a2[st] * inst->ic1[st][c] + inst->f_a3[st] * v3;
                    inst->ic1[st][c] = 2.0f * v1 - inst->ic1[st][c];
                    inst->ic2[st][c] = 2.0f * v2 - inst->ic2[st][c];
                    if (fabsf(inst->ic1[st][c]) < 1e-18f) inst->ic1[st][c] = 0.0f;
                    if (fabsf(inst->ic2[st][c]) < 1e-18f) inst->ic2[st][c] = 0.0f;
                    x = hp ? x - inst->f_k[st] * v1 - v2 : v2;
                }
                out[c] = x;
            }
            l = out[0];
            r = out[1];
        } else {
            /* Apply gain. Volume inverted rests at 1 - Depth and the trigger
             * opens it up: the same envelope, read as how far it has ducked. */
            float gain = inst->envelope;
            if (inst->target == TARGET_VOLUME_INV) {
                inst->inv_rest += (rest_target - inst->inv_rest) * INV_REST_GLIDE;
                gain = clampf(inst->inv_rest + (1.0f - inst->envelope), 0.0f, 1.0f);
            }
            l = (float)audio_inout[i * 2] * gain;
            r = (float)audio_inout[i * 2 + 1] * gain;
        }

        /* Clamp to int16 range */
        if (l > 32767.0f) l = 32767.0f;
        if (l < -32768.0f) l = -32768.0f;
        if (r > 32767.0f) r = 32767.0f;
        if (r < -32768.0f) r = -32768.0f;

        audio_inout[i * 2] = (int16_t)l;
        audio_inout[i * 2 + 1] = (int16_t)r;
    }
}

/* --- MIDI handler (exported via dlsym for chain host) --- */

static void ducker_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    ducker_instance_t *inst = (ducker_instance_t *)instance;
    if (!inst || len < 3) return;

    uint8_t status = msg[0] & 0xF0;
    uint8_t ch = (msg[0] & 0x0F) + 1;  /* 1-16 */
    uint8_t note = msg[1];
    uint8_t vel = msg[2];

    /* Channel filter: 0=omni accepts all */
    if (inst->channel > 0 && ch != inst->channel) return;

    /* Note filter */
    if (note != inst->trigger_note) return;

    if (status == 0x90 && vel > 0) {
        /* Note on */
        inst->active_notes++;

        /* Compute velocity-scaled depth */
        float vel_scale = 1.0f;
        if (inst->vel_sens > 0.0f) {
            vel_scale = 1.0f - inst->vel_sens + inst->vel_sens * (vel / 127.0f);
        }
        inst->vel_depth = inst->depth * vel_scale;

        /* Soft: carry on from the current duck level. A hit that asks for less
         * than is already ducked keeps the deeper level. Hard restarts from open. */
        inst->attack_t0 = 0.0f;
        if (inst->retrig == RETRIG_SOFT) {
            float ducked = 1.0f - inst->envelope;
            if (ducked > inst->vel_depth) inst->vel_depth = ducked;
            if (inst->vel_depth > 0.0f) {
                inst->attack_t0 = attack_start_fraction(inst->curve, ducked / inst->vel_depth);
            }
        }

        start_attack(inst);

        float attack_ms = inst->attack * 50.0f;
        inst->trig_ms = now_ms();
        inst->trig_depth = inst->vel_depth;
        inst->trig_t0 = inst->attack_t0;
        inst->trig_curve = inst->curve;
        inst->trig_attack_ms = attack_ms;
        inst->trig_release_ms = inst->release * 1000.0f;
        inst->release_ms = (inst->mode == MODE_TRIGGER)
            ? inst->trig_ms + attack_ms + inst->hold * 500.0f
            : 0.0;
    }
    else if (status == 0x80 || (status == 0x90 && vel == 0)) {
        /* Note off */
        if (inst->active_notes > 0) inst->active_notes--;

        if (inst->mode == MODE_GATE && inst->active_notes == 0) {
            /* Gate mode: release on last note-off. A note shorter than the
             * attack (a drum hit) lets the attack finish first, so every hit
             * ducks the same; the release starts when the attack ends. */
            if (inst->phase == PHASE_ATTACK) {
                inst->release_ms = inst->trig_ms + inst->trig_attack_ms;
            } else if (inst->phase == PHASE_HOLD) {
                start_release(inst);
                inst->release_ms = now_ms();
            }
        }
    }
}

/* --- Parameter handling --- */

static int parse_channel(const char *val) {
    if (strcmp(val, "Omni") == 0) return 0;
    int ch = atoi(val);
    if (ch >= 1 && ch <= 16) return ch;
    /* Float 0-1 → 0-16 */
    float f = (float)atof(val);
    return (int)(f * 16.0f + 0.5f);
}

static int parse_curve(const char *val) {
    if (strcmp(val, "Linear") == 0) return CURVE_LINEAR;
    if (strcmp(val, "Expo") == 0) return CURVE_EXPO;
    if (strcmp(val, "S-Curve") == 0) return CURVE_SCURVE;
    if (strcmp(val, "Pump") == 0) return CURVE_PUMP;
    /* Numeric fallback */
    int idx = (int)(atof(val) * 3.0f + 0.5f);
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;
    return idx;
}

static int parse_mode(const char *val) {
    if (strcmp(val, "Trigger") == 0) return MODE_TRIGGER;
    if (strcmp(val, "Gate") == 0) return MODE_GATE;
    return (atof(val) > 0.5f) ? MODE_GATE : MODE_TRIGGER;
}

static int parse_retrig(const char *val) {
    if (strcmp(val, "Hard") == 0) return RETRIG_HARD;
    if (strcmp(val, "Soft") == 0) return RETRIG_SOFT;
    return (atof(val) > 0.5f) ? RETRIG_SOFT : RETRIG_HARD;
}

static int parse_target(const char *val) {
    if (strcmp(val, "Volume") == 0) return TARGET_VOLUME;
    if (strcmp(val, "Filter") == 0) return TARGET_FILTER;
    if (strcmp(val, "Volume Inv") == 0) return TARGET_VOLUME_INV;
    int idx = (int)(atof(val) + 0.5f);
    return idx < 0 ? 0 : (idx > TARGET_FILTER ? TARGET_FILTER : idx);
}

static int parse_freq(const char *val) {
    int idx = atoi(val);
    if (idx < 0) idx = 0;
    if (idx >= FREQ_STEPS) idx = FREQ_STEPS - 1;
    return idx;
}

static const char *filter_type_names[FILTER_TYPES] = { "LP12", "LP24", "HP12", "HP24" };

static int parse_filter_type(const char *val) {
    for (int i = 0; i < FILTER_TYPES; i++) {
        if (strcmp(val, filter_type_names[i]) == 0) return i;
    }
    int idx = atoi(val);
    if (idx < 0) idx = 0;
    if (idx >= FILTER_TYPES) idx = FILTER_TYPES - 1;
    return idx;
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    ducker_instance_t *inst = (ducker_instance_t *)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "channel") == 0) {
        inst->channel = parse_channel(val);
    } else if (strcmp(key, "trigger_note") == 0) {
        int n = atoi(val);
        if (n < 0) n = 0;
        if (n > 127) n = 127;
        inst->trigger_note = n;
    } else if (strcmp(key, "mode") == 0) {
        inst->mode = parse_mode(val);
    } else if (strcmp(key, "depth") == 0) {
        inst->depth = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "attack") == 0) {
        inst->attack = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "hold") == 0) {
        inst->hold = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "release") == 0) {
        inst->release = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "curve") == 0) {
        inst->curve = parse_curve(val);
    } else if (strcmp(key, "vel_sens") == 0) {
        inst->vel_sens = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "retrig") == 0) {
        inst->retrig = parse_retrig(val);
    } else if (strcmp(key, "target") == 0) {
        int target = parse_target(val);
        if (target != inst->target) reset_filter_state(inst);
        inst->target = target;
    } else if (strcmp(key, "filter_type") == 0) {
        int type = parse_filter_type(val);
        if (type != inst->filter_type) reset_filter_state(inst);
        inst->filter_type = type;
    } else if (strcmp(key, "cut1") == 0) {
        inst->cut1 = parse_freq(val);
    } else if (strcmp(key, "cut2") == 0) {
        inst->cut2 = parse_freq(val);
    } else if (strcmp(key, "resonance") == 0) {
        inst->resonance = clampf((float)atof(val), 0.0f, RESONANCE_MAX);
    } else if (strcmp(key, "state") == 0) {
        /* Restore all parameters from JSON state */
        float fval;
        char sval[32];

        if (json_get_string(val, "channel", sval, sizeof(sval)) == 0) {
            inst->channel = parse_channel(sval);
        } else if (json_get_number(val, "channel", &fval) == 0) {
            inst->channel = (int)clampf(fval, 0.0f, 16.0f);
        }
        if (json_get_number(val, "trigger_note", &fval) == 0) {
            inst->trigger_note = (int)clampf(fval, 0.0f, 127.0f);
        }
        if (json_get_string(val, "mode", sval, sizeof(sval)) == 0) {
            inst->mode = parse_mode(sval);
        } else if (json_get_number(val, "mode", &fval) == 0) {
            inst->mode = (int)clampf(fval, 0.0f, 1.0f);
        }
        if (json_get_number(val, "depth", &fval) == 0) {
            inst->depth = clampf(fval, 0.0f, 1.0f);
        }
        if (json_get_number(val, "attack", &fval) == 0) {
            inst->attack = clampf(fval, 0.0f, 1.0f);
        }
        if (json_get_number(val, "hold", &fval) == 0) {
            inst->hold = clampf(fval, 0.0f, 1.0f);
        }
        if (json_get_number(val, "release", &fval) == 0) {
            inst->release = clampf(fval, 0.0f, 1.0f);
        }
        if (json_get_string(val, "curve", sval, sizeof(sval)) == 0) {
            inst->curve = parse_curve(sval);
        } else if (json_get_number(val, "curve", &fval) == 0) {
            inst->curve = (int)clampf(fval, 0.0f, 3.0f);
        }
        if (json_get_number(val, "vel_sens", &fval) == 0) {
            inst->vel_sens = clampf(fval, 0.0f, 1.0f);
        }
        /* A Set saved before Retrig existed has no key: it keeps the old
         * restart-from-open behaviour, only a new Ducker starts Soft. */
        inst->retrig = RETRIG_HARD;
        if (json_get_string(val, "retrig", sval, sizeof(sval)) == 0) {
            inst->retrig = parse_retrig(sval);
        } else if (json_get_number(val, "retrig", &fval) == 0) {
            inst->retrig = (int)clampf(fval, 0.0f, 1.0f);
        }
        /* A state from before the Filter target has none of these keys. Restoring
         * it onto a running instance (a bus effect keeps its instance) must give
         * the defaults, not whatever the instance had. */
        inst->target = TARGET_VOLUME;
        inst->filter_type = FILTER_LP24;
        inst->cut1 = CUT1_DEFAULT;
        inst->cut2 = CUT2_DEFAULT;
        inst->resonance = RESONANCE_DEFAULT;
        if (json_get_string(val, "target", sval, sizeof(sval)) == 0) {
            inst->target = parse_target(sval);
        } else if (json_get_number(val, "target", &fval) == 0) {
            inst->target = (int)clampf(fval, 0.0f, (float)TARGET_FILTER);
        }
        if (json_get_string(val, "filter_type", sval, sizeof(sval)) == 0) {
            inst->filter_type = parse_filter_type(sval);
        } else if (json_get_number(val, "filter_type", &fval) == 0) {
            inst->filter_type = (int)clampf(fval, 0.0f, FILTER_TYPES - 1);
        }
        reset_filter_state(inst);
        if (json_get_number(val, "cut1", &fval) == 0) {
            inst->cut1 = (int)clampf(fval, 0.0f, FREQ_STEPS - 1);
        }
        if (json_get_number(val, "cut2", &fval) == 0) {
            inst->cut2 = (int)clampf(fval, 0.0f, FREQ_STEPS - 1);
        }
        if (json_get_number(val, "resonance", &fval) == 0) {
            inst->resonance = clampf(fval, 0.0f, RESONANCE_MAX);
        }
    }
}

static const char *channel_name(int ch) {
    static const char *names[] = {
        "Omni", "1", "2", "3", "4", "5", "6", "7", "8",
        "9", "10", "11", "12", "13", "14", "15", "16"
    };
    if (ch < 0 || ch > 16) return "Omni";
    return names[ch];
}

static const char *curve_name(int curve) {
    static const char *names[] = { "Linear", "Expo", "S-Curve", "Pump" };
    if (curve < 0 || curve > 3) return "Linear";
    return names[curve];
}

static const char *mode_name(int mode) {
    return (mode == MODE_GATE) ? "Gate" : "Trigger";
}

static const char *retrig_name(int retrig) {
    return (retrig == RETRIG_SOFT) ? "Soft" : "Hard";
}

static const char *target_name(int target) {
    if (target == TARGET_FILTER) return "Filter";
    if (target == TARGET_VOLUME_INV) return "Volume Inv";
    return "Volume";
}

static const char *filter_type_name(int type) {
    if (type < 0 || type >= FILTER_TYPES) type = 0;
    return filter_type_names[type];
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    ducker_instance_t *inst = (ducker_instance_t *)instance;
    if (!inst) return -1;

    if (strcmp(key, "channel") == 0) return snprintf(buf, buf_len, "%s", channel_name(inst->channel));
    if (strcmp(key, "trigger_note") == 0) return snprintf(buf, buf_len, "%d", inst->trigger_note);
    if (strcmp(key, "mode") == 0) return snprintf(buf, buf_len, "%s", mode_name(inst->mode));
    if (strcmp(key, "depth") == 0) return snprintf(buf, buf_len, "%.2f", inst->depth);
    if (strcmp(key, "attack") == 0) return snprintf(buf, buf_len, "%.2f", inst->attack);
    if (strcmp(key, "hold") == 0) return snprintf(buf, buf_len, "%.2f", inst->hold);
    if (strcmp(key, "release") == 0) return snprintf(buf, buf_len, "%.2f", inst->release);
    if (strcmp(key, "curve") == 0) return snprintf(buf, buf_len, "%s", curve_name(inst->curve));
    if (strcmp(key, "vel_sens") == 0) return snprintf(buf, buf_len, "%.2f", inst->vel_sens);
    if (strcmp(key, "retrig") == 0) return snprintf(buf, buf_len, "%s", retrig_name(inst->retrig));
    if (strcmp(key, "target") == 0) return snprintf(buf, buf_len, "%s", target_name(inst->target));
    if (strcmp(key, "filter_type") == 0) return snprintf(buf, buf_len, "%s", filter_type_name(inst->filter_type));
    if (strcmp(key, "cut1") == 0) return snprintf(buf, buf_len, "%d", inst->cut1);
    if (strcmp(key, "cut2") == 0) return snprintf(buf, buf_len, "%d", inst->cut2);
    if (strcmp(key, "resonance") == 0) return snprintf(buf, buf_len, "%.2f", inst->resonance);
    /* Current BPM for the envelope graphic's time axis (120 if the host has none) */
    if (strcmp(key, "tempo") == 0) {
        float bpm = (g_host && g_host->get_bpm) ? g_host->get_bpm() : 120.0f;
        return snprintf(buf, buf_len, "%.1f", clampf(bpm, 20.0f, 300.0f));
    }

    /*
     * "duck_level" is declared live. A Schwung with live extra keys
     * (charlesvestal/schwung#668, not in 1.7.3) reads duck_level:effective every
     * tick; 1.7.3 ignores the flag and reads it on the slow value rotation. The
     * value is "<level>,<epoch ms>"; the time stamp lets the meter tell a fresh
     * reading from a rotation read.
     */
    if (strcmp(key, "duck_level") == 0 || strcmp(key, "duck_level:effective") == 0) {
        return snprintf(buf, buf_len, "%.3f,%.0f", duck_level_now(inst), now_ms());
    }
    if (strcmp(key, "duck_info") == 0) {
        return snprintf(buf, buf_len, "%.0f,%.0f,%.3f,%d,%.1f,%.1f,%.3f",
            inst->trig_ms, inst->release_ms, inst->trig_depth, inst->trig_curve,
            inst->trig_attack_ms, inst->trig_release_ms, inst->trig_t0);
    }
    if (strcmp(key, "name") == 0) return snprintf(buf, buf_len, "DUCKER");

    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"channel\":%d,\"trigger_note\":%d,\"mode\":%d,"
            "\"depth\":%.3f,\"attack\":%.3f,\"hold\":%.3f,\"release\":%.3f,"
            "\"curve\":%d,\"vel_sens\":%.3f,\"retrig\":%d,"
            "\"target\":%d,\"filter_type\":%d,\"cut1\":%d,\"cut2\":%d,\"resonance\":%.3f}",
            inst->channel, inst->trigger_note, inst->mode,
            inst->depth, inst->attack, inst->hold, inst->release,
            inst->curve, inst->vel_sens, inst->retrig,
            inst->target, inst->filter_type, inst->cut1, inst->cut2, inst->resonance);
    }

    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy = "{"
            "\"modes\":null,"
            "\"levels\":{"
                "\"root\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"channel\",\"trigger_note\",\"mode\",\"depth\",\"attack\",\"hold\",\"release\",\"target\"],"
                    "\"params\":[\"channel\",\"trigger_note\",\"mode\",\"depth\",\"attack\",\"hold\",\"release\",\"target\","
                        "{\"level\":\"volume_page\",\"label\":\"Volume\"},"
                        "{\"level\":\"filter_page\",\"label\":\"Filter\"}]"
                "},"
                "\"volume_page\":{"
                    "\"label\":\"Volume\",\"children\":null,"
                    "\"visible_if\":{\"param\":\"target\",\"not_equals\":\"Filter\"},"
                    "\"knobs\":[\"curve\",\"vel_sens\",\"retrig\"],"
                    "\"params\":[\"curve\",\"vel_sens\",\"retrig\"]"
                "},"
                "\"filter_page\":{"
                    "\"label\":\"Filter\",\"children\":null,"
                    "\"visible_if\":{\"param\":\"target\",\"equals\":\"Filter\"},"
                    "\"knobs\":[\"curve\",\"vel_sens\",\"retrig\",null,\"cut1\",\"cut2\",\"resonance\",\"filter_type\"],"
                    "\"params\":[\"curve\",\"vel_sens\",\"retrig\",\"cut1\",\"cut2\",\"resonance\",\"filter_type\"]"
                "}"
            "}"
        "}";
        int len = strlen(hierarchy);
        if (len < buf_len) {
            strcpy(buf, hierarchy);
            return len;
        }
        return -1;
    }

    if (strcmp(key, "chain_params") == 0) {
        int len = snprintf(buf, buf_len, "["
            "{\"key\":\"channel\",\"name\":\"Channel\",\"type\":\"enum\",\"options\":[\"Omni\",\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\",\"11\",\"12\",\"13\",\"14\",\"15\",\"16\"],\"default\":\"1\"},"
            /* A note is an ADDRESS, not a position on a range. As an int 0..127 the
             * grid drew an arc knob, so the cell said nothing and you had to open the
             * value to learn which note it was. Declared as an enum of note names it
             * draws the name itself -- C1 -- and the knob steps a semitone at a time.
             * Live`s octave numbering: 36 is C1 and 60 is C3, matching the pads.
             * The wire value is still the note number: get_param returns "%d" and the
             * host resolves a numeric enum value as an index. */
            "{\"key\":\"trigger_note\",\"name\":\"Trigger\",\"type\":\"enum\",\"options\":[\"C-2\",\"C#-2\",\"D-2\",\"D#-2\",\"E-2\",\"F-2\",\"F#-2\",\"G-2\",\"G#-2\",\"A-2\",\"A#-2\",\"B-2\",\"C-1\",\"C#-1\",\"D-1\",\"D#-1\",\"E-1\",\"F-1\",\"F#-1\",\"G-1\",\"G#-1\",\"A-1\",\"A#-1\",\"B-1\",\"C0\",\"C#0\",\"D0\",\"D#0\",\"E0\",\"F0\",\"F#0\",\"G0\",\"G#0\",\"A0\",\"A#0\",\"B0\",\"C1\",\"C#1\",\"D1\",\"D#1\",\"E1\",\"F1\",\"F#1\",\"G1\",\"G#1\",\"A1\",\"A#1\",\"B1\",\"C2\",\"C#2\",\"D2\",\"D#2\",\"E2\",\"F2\",\"F#2\",\"G2\",\"G#2\",\"A2\",\"A#2\",\"B2\",\"C3\",\"C#3\",\"D3\",\"D#3\",\"E3\",\"F3\",\"F#3\",\"G3\",\"G#3\",\"A3\",\"A#3\",\"B3\",\"C4\",\"C#4\",\"D4\",\"D#4\",\"E4\",\"F4\",\"F#4\",\"G4\",\"G#4\",\"A4\",\"A#4\",\"B4\",\"C5\",\"C#5\",\"D5\",\"D#5\",\"E5\",\"F5\",\"F#5\",\"G5\",\"G#5\",\"A5\",\"A#5\",\"B5\",\"C6\",\"C#6\",\"D6\",\"D#6\",\"E6\",\"F6\",\"F#6\",\"G6\",\"G#6\",\"A6\",\"A#6\",\"B6\",\"C7\",\"C#7\",\"D7\",\"D#7\",\"E7\",\"F7\",\"F#7\",\"G7\",\"G#7\",\"A7\",\"A#7\",\"B7\",\"C8\",\"C#8\",\"D8\",\"D#8\",\"E8\",\"F8\",\"F#8\",\"G8\"],\"default\":\"C1\"},"
            "{\"key\":\"mode\",\"name\":\"Mode\",\"type\":\"enum\",\"options\":[\"Trigger\",\"Gate\"],\"default\":\"Trigger\"},"
            "{\"key\":\"depth\",\"name\":\"Depth\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":1,\"step\":0.01},"
            "{\"key\":\"attack\",\"name\":\"Attack\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.1,\"step\":0.01,"
                "\"viz\":{\"group\":\"duck\",\"role\":\"attack\",\"kind\":\"custom:duckenv\",\"extra_keys\":[\"duck_info\",\"curve\",\"tempo\",\"duck_level\"]}},"
            "{\"key\":\"hold\",\"name\":\"Hold\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.2,\"step\":0.01,"
                "\"viz\":{\"group\":\"duck\",\"role\":\"hold\"}},"
            "{\"key\":\"release\",\"name\":\"Release\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.3,\"step\":0.01,"
                "\"viz\":{\"group\":\"duck\",\"role\":\"release\"}},"
            "{\"key\":\"target\",\"name\":\"Target\",\"type\":\"enum\",\"options\":[\"Volume\",\"Volume Inv\",\"Filter\"],\"default\":\"Volume\"},"
            "{\"key\":\"curve\",\"name\":\"Curve\",\"type\":\"enum\",\"options\":[\"Linear\",\"Expo\",\"S-Curve\",\"Pump\"],\"default\":\"Linear\"},"
            "{\"key\":\"cut1\",\"name\":\"Cut1\",\"type\":\"int\",\"min\":0,\"max\":127,\"default\":114,\"step\":1,"
                "\"card_script\":\"cards.js#freq_card\"},"
            "{\"key\":\"cut2\",\"name\":\"Cut2\",\"type\":\"int\",\"min\":0,\"max\":127,\"default\":13,\"step\":1,"
                "\"card_script\":\"cards.js#freq_card\"},"
            "{\"key\":\"resonance\",\"name\":\"Res\",\"type\":\"float\",\"min\":0,\"max\":0.9,\"default\":0.2,\"step\":0.01},"
            "{\"key\":\"filter_type\",\"name\":\"Type\",\"type\":\"enum\",\"options\":[\"LP12\",\"LP24\",\"HP12\",\"HP24\"],\"default\":\"LP24\"},"
            "{\"key\":\"vel_sens\",\"name\":\"Vel Sens\",\"short_name\":\"sens\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0,\"step\":0.01},"
            "{\"key\":\"retrig\",\"name\":\"Retrig\",\"short_name\":\"retrig\",\"type\":\"enum\",\"options\":[\"Hard\",\"Soft\"],\"default\":\"Soft\"},"
            "{\"key\":\"duck_info\",\"name\":\"Duck\",\"type\":\"string\",\"access\":\"read\"},"
            "{\"key\":\"tempo\",\"name\":\"Tempo\",\"type\":\"string\",\"access\":\"read\"},"
            "{\"key\":\"duck_level\",\"name\":\"Duck level\",\"type\":\"string\",\"access\":\"read\",\"live\":true}"
        "]");
        return (len > 0 && len < buf_len) ? len : -1;
    }

    return -1;
}

/* --- API exports --- */

static audio_fx_api_v2_t g_fx_api_v2;

audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_fx_api_v2, 0, sizeof(g_fx_api_v2));
    g_fx_api_v2.api_version = AUDIO_FX_API_VERSION_2;
    g_fx_api_v2.create_instance = v2_create_instance;
    g_fx_api_v2.destroy_instance = v2_destroy_instance;
    g_fx_api_v2.process_block = v2_process_block;
    g_fx_api_v2.set_param = v2_set_param;
    g_fx_api_v2.get_param = v2_get_param;
    /* Note: on_midi is NOT set in the struct (ABI safety for old hosts).
     * Chain host discovers MIDI capability via the standalone dlsym symbol below. */

    ducker_log("DUCKER v2 plugin initialized");

    return &g_fx_api_v2;
}

/*
 * Standalone MIDI handler export - chain host looks this up via dlsym.
 * This avoids ABI issues with old plugins that have a 6-field struct.
 */
void move_audio_fx_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    ducker_on_midi(instance, msg, len, source);
}
