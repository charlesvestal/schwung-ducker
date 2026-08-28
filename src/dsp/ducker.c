/*
 * Ducker - MIDI-triggered sidechain ducking audio FX
 *
 * A MIDI note triggers an envelope that attenuates the audio signal,
 * producing classic sidechain pumping without needing an audio sidechain input.
 *
 * Chain host discovers MIDI capability via dlsym("move_audio_fx_on_midi").
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "audio_fx_api_v2.h"

#define SAMPLE_RATE 44100

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

    /* Envelope state */
    int phase;            /* PHASE_* */
    int phase_pos;        /* sample counter within phase */
    int phase_len;        /* total samples in current phase */
    float vel_depth;      /* computed depth for current trigger */
    float envelope;       /* current envelope value: 1.0=pass, 0.0=max duck */
    int active_notes;     /* count of held notes (for gate mode) */

} ducker_instance_t;

static const host_api_v1_t *g_host = NULL;

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
    if (x < lo) return lo;
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
    inst->phase = PHASE_IDLE;
    inst->envelope = 1.0f;
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

    for (int i = 0; i < frames; i++) {
        /* Advance envelope */
        switch (inst->phase) {
        case PHASE_ATTACK: {
            if (inst->phase_len > 0) {
                float t = (float)inst->phase_pos / (float)inst->phase_len;
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
                if (inst->phase_len <= 0 && inst->mode == MODE_TRIGGER) {
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

        /* Apply gain */
        float gain = inst->envelope;
        float l = (float)audio_inout[i * 2] * gain;
        float r = (float)audio_inout[i * 2 + 1] * gain;

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

        start_attack(inst);
    }
    else if (status == 0x80 || (status == 0x90 && vel == 0)) {
        /* Note off */
        if (inst->active_notes > 0) inst->active_notes--;

        if (inst->mode == MODE_GATE && inst->active_notes == 0) {
            /* Gate mode: release on last note-off */
            if (inst->phase == PHASE_HOLD || inst->phase == PHASE_ATTACK) {
                start_release(inst);
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
    if (strcmp(key, "name") == 0) return snprintf(buf, buf_len, "DUCKER");

    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"channel\":%d,\"trigger_note\":%d,\"mode\":%d,"
            "\"depth\":%.3f,\"attack\":%.3f,\"hold\":%.3f,\"release\":%.3f,"
            "\"curve\":%d,\"vel_sens\":%.3f}",
            inst->channel, inst->trigger_note, inst->mode,
            inst->depth, inst->attack, inst->hold, inst->release,
            inst->curve, inst->vel_sens);
    }

    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy = "{"
            "\"modes\":null,"
            "\"levels\":{"
                "\"root\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"channel\",\"trigger_note\",\"mode\",\"depth\",\"attack\",\"hold\",\"release\",\"curve\"],"
                    "\"params\":[\"channel\",\"trigger_note\",\"mode\",\"depth\",\"attack\",\"hold\",\"release\",\"curve\",\"vel_sens\"]"
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
        const char *params_json = "["
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
            "{\"key\":\"attack\",\"name\":\"Attack\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.1,\"step\":0.01},"
            "{\"key\":\"hold\",\"name\":\"Hold\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.2,\"step\":0.01},"
            "{\"key\":\"release\",\"name\":\"Release\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.3,\"step\":0.01},"
            "{\"key\":\"curve\",\"name\":\"Curve\",\"type\":\"enum\",\"options\":[\"Linear\",\"Expo\",\"S-Curve\",\"Pump\"],\"default\":\"Linear\"}"
        "]";
        int len = strlen(params_json);
        if (len < buf_len) {
            strcpy(buf, params_json);
            return len;
        }
        return -1;
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
