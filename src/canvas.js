/*
 * Ducker envelope: replaces Schwung's generic envelope graphic over the Attack,
 * Hold and Release cells. It draws the duck (down on attack, low through hold,
 * back up on release, shaped by Curve) and, at its right end, a meter showing
 * how much the signal is being ducked right now. Full bar = no ducking; the
 * lowest the bar goes is the hold level, which is empty only at Depth 100%.
 * Volume Inv shows the resulting volume the other way up: low at rest, full on
 * a trigger. The graph draws the nominal full-velocity shape; the meter follows
 * the velocity of the last hit, like the sound.
 *
 * The level comes from "duck_level", "<level>,<epoch ms>", which the DSP
 * declares live: a Schwung that reads live extra keys (charlesvestal/schwung#668)
 * refreshes it every tick. Schwung 1.7.3 only reads it on its slow value
 * rotation, so the reading is usually stale; the time stamp tells the two apart,
 * and a stale reading falls back to "duck_info", the last trigger (note-on
 * time, release start, depth, curve, attack/release times, where a soft
 * retrigger starts on the attack curve), replayed from the wall clock. Same
 * maths as shape_curve() in ducker.c.
 */

const FRESH_MS = 150;   /* a live reading is a tick or two old; a rotation read is not */

function shape(curve, t, isRelease) {
    t = Math.max(0, Math.min(1, t));
    if (curve === 1) return t * t;
    if (curve === 2) return t * t * (3 - 2 * t);
    if (curve === 3 && isRelease) {
        const inv = 1 - t;
        return 1 - inv * inv * inv;
    }
    return t;
}

/* 1 = no ducking, 1 - depth = full duck. */
function duckLevel(info, nowMs) {
    const p = String(info || "").split(",").map(Number);
    if (p.length < 6 || p.some((x) => !Number.isFinite(x))) return 1;
    const [t0, trel, depth, curve, attackMs, releaseMs] = p;
    /* Where on the attack curve a soft retrigger started; absent (0) = from open */
    const startFrac = p.length > 6 ? p[6] : 0;
    if (!(t0 > 0) || nowMs < t0) return 1;
    const releasing = trel > 0 && nowMs >= trel;
    if (!releasing) {
        const el = nowMs - t0;
        if (attackMs > 0 && el < attackMs) return 1 - depth * shape(curve, startFrac + el / attackMs, false);
        return 1 - depth;
    }
    const r = nowMs - trel;
    if (releaseMs <= 0 || r >= releaseMs) return 1;
    return 1 - depth + depth * shape(curve, r / releaseMs, true);
}

const CURVE_NAMES = ["linear", "expo", "s-curve", "pump"];

function curveIndex(raw) {
    const i = CURVE_NAMES.indexOf(String(raw).toLowerCase());
    if (i >= 0) return i;
    const n = Number(raw);
    return Number.isFinite(n) ? Math.max(0, Math.min(3, Math.round(n))) : 0;
}

function num(values, key, dflt) {
    const n = values ? Number(values[key]) : NaN;
    return Number.isFinite(n) ? n : dflt;
}

function drawDuckEnvelope(ctx, { values, nowMs }) {
    const w = ctx.width, h = ctx.height;
    if (w < 24 || h < 7) return;

    const attack = num(values, "attack", 0.1);
    const hold = num(values, "hold", 0.2);
    const release = num(values, "release", 0.3);
    const depth = Math.max(0, Math.min(1, num(values, "depth", 1)));
    const curve = curveIndex(values && values.curve);
    const now = Number.isFinite(nowMs) ? nowMs : Date.now();
    const live = String((values && values.duck_level) || "").split(",").map(Number);
    const fresh = live.length === 2 && Number.isFinite(live[0]) && Number.isFinite(live[1]) &&
        Math.abs(now - live[1]) <= FRESH_MS;
    const duck = Math.max(0, Math.min(1, fresh ? live[0] : duckLevel(values && values.duck_info, now)));
    /* Volume Inv rests at 1 - Depth and opens up on a trigger: the same envelope
     * read as how far it has ducked. The meter and graph show the resulting gain. */
    const inverted = String((values && values.target) || "") === "Volume Inv";
    const gainOf = (lvl) => (inverted ? Math.max(0, Math.min(1, 1 - depth + (1 - lvl))) : lvl);
    const level = gainOf(duck);

    /* Meter at the right end: a framed bar, filled from the bottom. */
    const barW = 8;
    const barX = w - barW;
    ctx.fillRect(barX, 0, barW, 1, 1);
    ctx.fillRect(barX, h - 1, barW, 1, 1);
    ctx.fillRect(barX, 0, 1, h, 1);
    ctx.fillRect(w - 1, 0, 1, h, 1);
    const innerH = h - 4;
    const fill = Math.round(innerH * level);
    ctx.fillRect(barX + 2, 2 + (innerH - fill), barW - 4, fill, 1);

    /* The duck on a true time axis: the full width is one beat (a quarter
     * note at the current tempo), so the right edge is the next kick. Longer
     * ducks run off the edge. Notches along the bottom mark 16th notes. */
    const plotW = barX - 3;
    const bpm = num(values, "tempo", 120);
    const beatMs = 60000 / Math.max(20, Math.min(300, bpm));
    const attackMs = attack * 50, holdMs = hold * 500, releaseMs = release * 1000;
    const plotH = h - 2;
    for (let x = 0; x < plotW; x++) {
        const t = (x + 0.5) / plotW * beatMs;
        let lvl;
        if (t < attackMs) lvl = 1 - depth * shape(curve, t / attackMs, false);
        else if (t < attackMs + holdMs) lvl = 1 - depth;
        else if (t < attackMs + holdMs + releaseMs) lvl = 1 - depth + depth * shape(curve, (t - attackMs - holdMs) / releaseMs, true);
        else lvl = 1;
        const y = 1 + Math.round((1 - gainOf(lvl)) * (plotH - 1));
        ctx.fillRect(x, y, 1, h - 1 - y, 1);                  /* solid fill under the line */
    }
    for (let i = 1; i < 4; i++) ctx.fillRect(Math.round(plotW * i / 4), h - 2, 1, 2, 0);
}

globalThis.canvas_overlay = {
    widgetKinds: {
        "custom:duckenv": drawDuckEnvelope,
    },
};
