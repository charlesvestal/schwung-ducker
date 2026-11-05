/*
 * Ducker cutoff card: floats over the page while Cut1 or Cut2 is held or
 * turned. The knobs hold a 0-127 index on a log scale from 20Hz to 20kHz, so
 * the card shows the frequency that index means, and where it sits on the scale.
 * Same mapping as freq_hz() in ducker.c.
 */

const STEPS = 128;
const MIN_HZ = 20;
const MAX_HZ = 20000;

function indexToHz(index) {
    return MIN_HZ * Math.pow(MAX_HZ / MIN_HZ, index / (STEPS - 1));
}

function formatHz(hz) {
    if (hz < 1000) return Math.round(hz) + " Hz";
    const k = hz / 1000;
    return (k < 10 ? k.toFixed(2) : String(Math.round(k * 10) / 10)) + " kHz";
}

globalThis.freq_card = function (ctx, { name, raw }) {
    const w = ctx.width, h = ctx.height;
    if (w < 8 || h < 8) return;

    ctx.print(0, 0, String(name || ""), 1);

    /* No answer, no picture. */
    const n = raw === null || raw === undefined ? NaN : Number(raw);
    if (!Number.isFinite(n)) {
        ctx.print(0, h - 7, "--", 1);
        return;
    }
    const index = Math.max(0, Math.min(STEPS - 1, Math.round(n)));

    const text = formatHz(indexToHz(index));
    const tw = ctx.textWidth(text);
    if (tw <= w) ctx.print(w - tw, 0, text, 1);

    /* Position on the log scale, with a tick at each decade (20, 200, 2k, 20k). */
    const barY = h - 8;
    ctx.fillRect(0, barY + 4, w, 1, 1);
    for (const hz of [20, 200, 2000, 20000]) {
        const x = Math.round((w - 1) * Math.log(hz / MIN_HZ) / Math.log(MAX_HZ / MIN_HZ));
        ctx.fillRect(x, barY + 2, 1, 5, 1);
    }
    const px = Math.round((w - 1) * index / (STEPS - 1));
    ctx.fillRect(Math.max(0, Math.min(w - 3, px - 1)), barY, 3, 9, 1);
};
