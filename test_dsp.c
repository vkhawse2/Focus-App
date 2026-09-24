/* DSP test for Focus App — verifies the click-free audio engine.
 * Mirrors focus_app.c's fill_buffer + UI state changes exactly, then:
 *  A. binaural beat frequencies are still 10/20/40 Hz (L=200, R=200+beat)
 *  B. preset switch produces no sample discontinuity (the old click)
 *  C. rapid double-switch stays clean
 *  D. stop fades to true silence with no discontinuity and terminates
 *  E. restart after stop fades in cleanly
 */
#include <stdio.h>
#include <math.h>
#include <string.h>

#define SAMPLE_RATE 44100
#define PI 3.14159265358979323846
#define CARRIER_HZ 200.0
#define FADE_MS 100
#define FADE_STEP (1.0 / (SAMPLE_RATE * FADE_MS / 1000))
#define SWAP_THRESH 0.02
#define N 2048

/* ---- engine state (mirrors focus_app.c) ---- */
static double g_beat = 10.0;
static float  g_volF = 0.2f;
static double g_env = 0.0;
static double g_envTarget = 0.0;      /* volatile in app */
static int    g_swapPending = 0;
static double g_swapBeat = 10.0;
static double g_phase = 0.0;
static int    g_playing = 0;
static int    g_stopping = 0;
static int    g_activePreset = -1;
static double g_beats[3] = { 10.0, 20.0, 40.0 };

/* ---- exact copy of fill_buffer from focus_app.c ---- */
static int fill_buffer(short *out, int n)
{
    double beat = g_beat;
    float  vol  = g_volF;
    const double dt = 1.0 / SAMPLE_RATE;
    int audible = 0;

    for (int i = 0; i < n; i++) {
        double target = g_envTarget;
        if (g_env < target)      { g_env += FADE_STEP; if (g_env > target) g_env = target; }
        else if (g_env > target) { g_env -= FADE_STEP; if (g_env < target) g_env = target; }

        if (g_swapPending && target == 0.0 && g_env <= SWAP_THRESH) {
            beat = g_swapBeat;
            g_beat = g_swapBeat;
            g_swapPending = 0;
            g_envTarget = 1.0;
        }

        double fR = CARRIER_HZ + beat;
        double sL = sin(2.0 * PI * CARRIER_HZ * g_phase);
        double sR = sin(2.0 * PI * fR * g_phase);
        g_phase += dt;
        if (g_phase >= 1.0) g_phase -= 1.0;

        float a = vol * (float)g_env;
        if (g_env > 0.0) audible = 1;
        out[2 * i]     = (short)(sL * 32767.0 * a);
        out[2 * i + 1] = (short)(sR * 32767.0 * a);
    }
    return audible;
}

/* ---- UI-side state changes (mirror start_preset/stop_playback/callback) ---- */
static void ui_start(int i) {
    g_beat = g_beats[i]; g_swapPending = 0;
    g_env = 0.0; g_envTarget = 1.0; g_stopping = 0;
    g_activePreset = i; g_playing = 1;
}
static void ui_switch(int i) {
    if (g_activePreset == i && !g_swapPending) return;
    g_swapBeat = g_beats[i]; g_swapPending = 1;
    g_stopping = 0; g_envTarget = 0.0; g_activePreset = i;
}
static void ui_stop(void) {
    if (!g_playing) return;
    g_swapPending = 0; g_stopping = 1; g_envTarget = 0.0; g_activePreset = -1;
}
/* one callback invocation; returns 0 when stream ended */
static int cb_step(short *buf) {
    if (!g_playing) return 0;
    int audible = fill_buffer(buf, N);
    if (g_stopping && !audible) { g_stopping = 0; g_playing = 0; return 0; }
    return 1;
}

/* max |sample[n]-sample[n-1]| across a run, per channel */
static short buf[N * 2];
static double max_jump_r, max_jump_l;

static void run_buffers(int count, int do_cb) {
    short prev_l = 0, prev_r = 0;
    int first = 1;
    for (int b = 0; b < count; b++) {
        if (do_cb) cb_step(buf); else fill_buffer(buf, N);
        for (int i = 0; i < N; i++) {
            short l = buf[2*i], r = buf[2*i+1];
            if (!first) {
                double jl = fabs((double)l - prev_l), jr = fabs((double)r - prev_r);
                if (jl > max_jump_l) max_jump_l = jl;
                if (jr > max_jump_r) max_jump_r = jr;
            }
            first = 0; prev_l = l; prev_r = r;
        }
    }
}

static int failures = 0;
static void check(int cond, const char *name) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) failures++;
}

int main(void)
{
    printf("Focus App DSP test — click-free engine\n");

    /* A: frequencies */
    printf("A: binaural frequencies\n");
    for (int p = 0; p < 3; p++) {
        ui_start(p);
        run_buffers(25, 0);              /* settle past fade-in */
        int zc = 0; short pr = 0; int f = 1;
        for (int b = 0; b < 43; b++) {  /* ~2 s */
            fill_buffer(buf, N);
            for (int i = 0; i < N; i++) {
                short r = buf[2*i+1];
                if (!f && ((pr <= 0 && r > 0) || (pr >= 0 && r < 0))) zc++;
                f = 0; pr = r;
            }
        }
        double hz = zc / 2.0 / 2.0;     /* 2 s window */
        char nm[64]; snprintf(nm, sizeof nm, "preset %d right = %.0f Hz (expect %.0f)",
                             p, hz, CARRIER_HZ + g_beats[p]);
        check(fabs(hz - (CARRIER_HZ + g_beats[p])) < 1.5, nm);
        g_playing = 0;
    }

    /* B: switch alpha -> beta, no discontinuity */
    printf("B: preset switch alpha->beta\n");
    max_jump_l = max_jump_r = 0;
    ui_start(0); run_buffers(12, 0);
    ui_switch(1); run_buffers(30, 0);
    check(g_beat == 20.0 && g_envTarget == 1.0 && !g_swapPending, "switch completed, beat=20Hz, envelope back to 1");
    char jb[128]; snprintf(jb, sizeof jb, "max sample jump L=%.0f R=%.0f (< 2000 = no click)", max_jump_l, max_jump_r);
    check(max_jump_r < 2000 && max_jump_l < 2000, jb);

    /* C: rapid double switch alpha -> beta -> gamma */
    printf("C: rapid double switch\n");
    max_jump_l = max_jump_r = 0;
    ui_start(0); run_buffers(12, 0);
    ui_switch(1); run_buffers(2, 0);    /* mid-dip */
    ui_switch(2); run_buffers(30, 0);
    check(g_beat == 40.0 && !g_swapPending, "double switch landed on gamma, no stuck pending swap");
    snprintf(jb, sizeof jb, "max sample jump L=%.0f R=%.0f (< 2000 = no click)", max_jump_l, max_jump_r);
    check(max_jump_r < 2000 && max_jump_l < 2000, jb);

    /* D: stop fades to silence, terminates, no click */
    printf("D: stop fade-out\n");
    max_jump_l = max_jump_r = 0;
    ui_start(2); run_buffers(12, 0);
    ui_stop();
    int steps = 0, alive = 1;
    short last_l = 0, last_r = 0, last = 1;
    int have_last = 0;
    while (alive && steps < 200) {
        alive = cb_step(buf); steps++;
        for (int i = 0; i < N; i++) {
            short l = buf[2*i], r = buf[2*i+1];
            if (have_last) {
                double jl = fabs((double)l - last_l), jr = fabs((double)r - last_r);
                if (jl > max_jump_l) max_jump_l = jl;
                if (jr > max_jump_r) max_jump_r = jr;
            }
            have_last = 1; last_l = l; last_r = r;
            last = r;
        }
    }
    check(!g_playing && !g_stopping, "stream terminated after fade");
    check(steps < 200, "fade completed promptly");
    check(last == 0, "final buffer is true silence");
    snprintf(jb, sizeof jb, "max sample jump during fade < 2000 (no click)");
    check(max_jump_r < 2000, jb);

    /* E: restart after full stop fades in cleanly */
    printf("E: restart after stop\n");
    max_jump_l = max_jump_r = 0;
    ui_start(1); run_buffers(12, 0);
    check(g_env > 0.5, "audio resumed after restart");
    snprintf(jb, sizeof jb, "max sample jump on restart L=%.0f R=%.0f (< 2000 = no click)", max_jump_l, max_jump_r);
    check(max_jump_r < 2000 && max_jump_l < 2000, jb);

    /* F: exit drain — mirrors the WM_DESTROY wait loop: after the fade the
       callback has stopped rewriting, but fade-tail buffers may still be
       queued in the driver; the loop must drain them before teardown. */
    printf("F: exit drain\n");
    int t_state[4] = {1,1,1,1};
    int t_playing = 0, w;
    for (w = 0; w < 150; w++) {
        if (!t_playing) {
            int busy = 0;
            for (int k = 0; k < 4; k++) if (t_state[k]) { busy = 1; break; }
            if (!busy) break;
            /* driver completes the oldest queued buffer -> WOM_DONE ->
               callback (!playing) marks it idle */
            for (int k = 0; k < 4; k++) if (t_state[k]) { t_state[k] = 0; break; }
        }
    }
    check(w < 150, "drain loop terminates (no hang on exit)");
    int all0 = 1; for (int k = 0; k < 4; k++) if (t_state[k]) all0 = 0;
    check(all0, "queued tail drained before teardown (no reset-amputate pop)");

    printf(failures ? "RESULT: %d FAILURES\n" : "RESULT: ALL PASS\n", failures);
    return failures != 0;
}
