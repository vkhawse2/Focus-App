/* ============================================================================
 * Focus App 1.0 — binaural beats player for focus sessions.
 * Pure Win32 C. No frameworks, no network, no installer.
 * Alpha 10 Hz / Beta 20 Hz / Gamma 40 Hz binaural beats over a 200 Hz carrier.
 * ========================================================================== */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <math.h>
#include <wchar.h>
#include <stdio.h>

#define PI 3.14159265358979323846

/* ---------------- configuration ---------------- */
#define SAMPLE_RATE   44100
#define BUF_SAMPLES   2048          /* ~46 ms per buffer */
#define NUM_BUFS      4             /* ~184 ms queued: responsive yet underrun-safe */
#define CARRIER_HZ    200.0

#define APP_TITLE     L"Focus App"
#define WND_CLASS     L"FocusAppWndClass"

#define WM_TRAYICON   (WM_APP + 1)
#define IDT_WAVE      1
#define ID_TRAY_OPEN  101
#define ID_TRAY_STOP  102
#define ID_TRAY_EXIT  103

/* ---------------- palette ---------------- */
#define COL_BG        RGB(10, 15, 26)
#define COL_CARD      RGB(17, 24, 40)
#define COL_BORDER    RGB(34, 48, 74)
#define COL_TILE      RGB(21, 31, 51)
#define COL_TILE_HOV  RGB(28, 39, 64)
#define COL_TEXT      RGB(241, 245, 249)
#define COL_GRAY      RGB(139, 152, 173)
#define COL_ALPHA     RGB(45, 212, 238)
#define COL_BETA      RGB(52, 211, 153)
#define COL_GAMMA     RGB(167, 139, 250)
#define COL_CYAN      RGB(53, 214, 232)
#define COL_RED       RGB(248, 113, 113)
#define COL_RED_BG    RGB(58, 22, 32)
#define COL_RED_BD    RGB(122, 42, 53)
#define COL_ICON_BG   RGB(16, 46, 58)

/* ---------------- presets ---------------- */
static const wchar_t *g_names[3]   = { L"Alpha", L"Beta", L"Gamma" };
static const wchar_t *g_hz[3]      = { L"10 Hz", L"20 Hz", L"40 Hz" };
static const wchar_t *g_hzshort[3]= { L"10Hz",  L"20Hz",  L"40Hz" };
static const double   g_beats[3]   = { 10.0, 20.0, 40.0 };
static const COLORREF g_cols[3]    = { COL_ALPHA, COL_BETA, COL_GAMMA };

/* ---------------- audio state ---------------- */
static HWAVEOUT       g_hwo = NULL;
static WAVEHDR        g_hdrs[NUM_BUFS];
static short          g_bufs[NUM_BUFS][BUF_SAMPLES * 2];
static volatile LONG  g_bufState[NUM_BUFS];   /* 0 = idle, 1 = queued */
static volatile LONG  g_playing = 0;
static volatile LONG  g_stopping = 0;      /* fade-out toward a full stop */
static volatile LONG  g_activePreset = -1;
static volatile double g_beat = 10.0;
static volatile float  g_volF = 0.2f;

/* Click-free engine: a single amplitude envelope glides toward g_envTarget.
 * Stops fade out instead of cutting. Preset switches dip the envelope to
 * near-silence, swap the beat frequency at the bottom of the dip, then
 * glide back up — the frequency change is never audible as a pop.
 * g_env / g_phase are owned by the waveOut callback thread; the UI thread
 * only writes the volatile targets. */
static double         g_env = 0.0;          /* 0..1 amplitude envelope */
static volatile double g_envTarget = 0.0;   /* 0.0 or 1.0 */
static volatile LONG  g_swapPending = 0;    /* preset change waiting for the dip */
static volatile double g_swapBeat = 10.0;
static double         g_phase = 0.0;        /* seconds; callback thread only */

#define FADE_MS       100
#define FADE_STEP     (1.0 / (SAMPLE_RATE * FADE_MS / 1000))
#define SWAP_THRESH   0.02

/* ---------------- ui state ---------------- */
static HWND   g_hwnd = NULL;
static HICON  g_hIconBig = NULL, g_hIconSm = NULL;
static HFONT  g_fTitle, g_fSub, g_fBtn, g_fHz, g_fPct, g_fPill;
static NOTIFYICONDATAW g_nid;
static BOOL   g_inTray = FALSE;
static BOOL   g_balloonShown = FALSE;
static int    g_hover = -1;        /* 0..2 preset, 3 = stop, -1 none */
static BOOL   g_dragging = FALSE;
static int    g_volume = 20;       /* 0..100 */
static ULONGLONG g_playStartTick = 0;
static double g_waveT = 0.0;

static RECT g_rcPreset[3];
static RECT g_rcTrack;             /* slider track */
static RECT g_rcStop;
static RECT g_rcPill;

static const int TRACK_X_IDLE = 66,  TRACK_W_IDLE = 294;
static const int TRACK_X_PLAY = 66,  TRACK_W_PLAY = 264;

/* ============================================================================
 * Audio engine — waveOut streaming, buffers refilled in WOM_DONE callback.
 * ========================================================================== */
/* Fill one buffer. The envelope glides toward g_envTarget every sample; a
 * pending preset swap happens only once the envelope is near zero, so the
 * frequency change can never produce a discontinuity. Returns TRUE while
 * any sample in the buffer is still audible. */
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
        /* All our frequencies are integers, so wrapping by whole seconds
           keeps every sine continuous — no clicks, no float drift. */
        if (g_phase >= 1.0) g_phase -= 1.0;

        float a = vol * (float)g_env;
        if (g_env > 0.0) audible = 1;
        out[2 * i]     = (short)(sL * 32767.0 * a);
        out[2 * i + 1] = (short)(sR * 32767.0 * a);
    }
    return audible;
}

static void CALLBACK wave_out_proc(HWAVEOUT hwo, UINT msg,
                                   DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2)
{
    (void)inst; (void)p2;
    if (msg != WOM_DONE) return;
    WAVEHDR *hdr = (WAVEHDR *)p1;
    int idx = (int)(hdr - g_hdrs);   /* hdr points into g_hdrs[] */

    if (idx < 0 || idx >= NUM_BUFS) return;

    if (InterlockedCompareExchange(&g_playing, 0, 0)) {
        int audible = fill_buffer((short *)hdr->lpData, BUF_SAMPLES);
        if (g_stopping && !audible) {
            /* Fade-out finished: let the device run dry instead of cutting. */
            InterlockedExchange(&g_bufState[idx], 0);
            InterlockedExchange(&g_stopping, 0);
            InterlockedExchange(&g_playing, 0);
            return;
        }
        InterlockedExchange(&g_bufState[idx], 1);
        waveOutWrite(hwo, hdr, sizeof(WAVEHDR));
    } else {
        InterlockedExchange(&g_bufState[idx], 0);
    }
}

static BOOL audio_init(void)
{
    WAVEFORMATEX wfx;
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = SAMPLE_RATE;
    wfx.nAvgBytesPerSec = SAMPLE_RATE * 4;
    wfx.nBlockAlign     = 4;
    wfx.wBitsPerSample  = 16;
    wfx.cbSize          = 0;

    if (waveOutOpen(&g_hwo, WAVE_MAPPER, &wfx,
                    (DWORD_PTR)wave_out_proc, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        g_hwo = NULL;
        return FALSE;
    }
    for (int i = 0; i < NUM_BUFS; i++) {
        g_hdrs[i].lpData         = (LPSTR)g_bufs[i];
        g_hdrs[i].dwBufferLength = sizeof(g_bufs[i]);
        g_hdrs[i].dwFlags        = 0;
        g_hdrs[i].dwLoops        = 0;
        waveOutPrepareHeader(g_hwo, &g_hdrs[i], sizeof(WAVEHDR));
        g_bufState[i] = 0;
    }
    return TRUE;
}

static void audio_shutdown(void)
{
    if (!g_hwo) return;
    InterlockedExchange(&g_playing, 0);
    waveOutReset(g_hwo);
    for (int i = 0; i < NUM_BUFS; i++)
        waveOutUnprepareHeader(g_hwo, &g_hdrs[i], sizeof(WAVEHDR));
    waveOutClose(g_hwo);
    g_hwo = NULL;
}

/* ============================================================================
 * Playback control (UI thread)
 * ========================================================================== */
static void update_tray_tip(void);

static void start_preset(int i)
{
    if (i < 0 || i > 2) return;
    if (!g_hwo) {
        MessageBoxW(g_hwnd, L"Could not open the audio device.\nPlayback will not work.",
                    APP_TITLE, MB_ICONWARNING | MB_OK);
        return;
    }

    if (InterlockedCompareExchange(&g_playing, 0, 0)) {
        /* Already streaming: dip the envelope, swap the beat at the bottom
           of the dip, glide back up. No clicks, even if tapped rapidly —
           a newer tap simply overwrites the pending beat. */
        if (InterlockedCompareExchange(&g_activePreset, -1, -1) == i && !g_swapPending)
            return;   /* same tile, nothing to do */
        g_swapBeat = g_beats[i];
        g_swapPending = 1;
        InterlockedExchange(&g_stopping, 0);
        g_envTarget = 0.0;
        InterlockedExchange(&g_activePreset, i);
        /* session clock keeps running across switches */
    } else {
        /* Fresh start: begin silent, glide up. */
        g_beat = g_beats[i];
        g_swapPending = 0;
        g_env = 0.0;
        g_envTarget = 1.0;
        InterlockedExchange(&g_stopping, 0);
        InterlockedExchange(&g_activePreset, i);
        g_playStartTick = GetTickCount64();
        InterlockedExchange(&g_playing, 1);
        for (int k = 0; k < NUM_BUFS; k++) {
            if (InterlockedCompareExchange(&g_bufState[k], 1, 0) == 0) {
                fill_buffer((short *)g_hdrs[k].lpData, BUF_SAMPLES);
                waveOutWrite(g_hwo, &g_hdrs[k], sizeof(WAVEHDR));
            }
        }
        SetTimer(g_hwnd, IDT_WAVE, 50, NULL);
    }
    update_tray_tip();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void stop_playback(void)
{
    if (!InterlockedCompareExchange(&g_playing, 0, 0)) return;
    /* Never cut the audio: glide the envelope to zero. The callback ends
       the stream once it's actually silent, so no pop on stop or exit. */
    g_swapPending = 0;
    InterlockedExchange(&g_stopping, 1);
    g_envTarget = 0.0;
    InterlockedExchange(&g_activePreset, -1);
    KillTimer(g_hwnd, IDT_WAVE);
    update_tray_tip();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ============================================================================
 * Tray icon
 * ========================================================================== */
static void update_tray_tip(void)
{
    wchar_t tip[128];
    LONG p = InterlockedCompareExchange(&g_activePreset, -1, -1);
    if (p >= 0)
        swprintf(tip, 128, L"Focus App \u2014 Playing %s (%s)", g_names[p], g_hzshort[p]);
    else
        wcscpy(tip, L"Focus App");
    wcsncpy(g_nid.szTip, tip, 127);
    g_nid.szTip[127] = 0;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void tray_add(HWND hwnd)
{
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = g_hIconSm;
    wcscpy(g_nid.szTip, L"Focus App");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void tray_balloon_once(void)
{
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags |= NIF_INFO;
    wcscpy(nid.szInfoTitle, L"Focus App");
    wcscpy(nid.szInfo, L"Still running here. Double-click the tray icon to reopen.");
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void tray_menu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"Open Focus App");
    AppendMenuW(m, MF_STRING | (InterlockedCompareExchange(&g_playing, 0, 0) ? 0 : MF_GRAYED),
                ID_TRAY_STOP, L"Stop playback");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"Exit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

static void window_show(void)
{
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    g_inTray = FALSE;
}

static void window_hide_to_tray(void)
{
    ShowWindow(g_hwnd, SW_HIDE);
    g_inTray = TRUE;
    if (!g_balloonShown) {
        g_balloonShown = TRUE;
        tray_balloon_once();
    }
}

/* ============================================================================
 * Drawing helpers
 * ========================================================================== */
static void fill_round_rect(HDC hdc, RECT r, int rad,
                            COLORREF fill, COLORREF border, int borderW)
{
    HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right, r.bottom, rad, rad);
    HBRUSH br = CreateSolidBrush(fill);
    FillRgn(hdc, rgn, br);
    DeleteObject(br);
    if (borderW > 0) {
        HPEN pen = CreatePen(PS_SOLID, borderW, border);
        HGDIOBJ op = SelectObject(hdc, pen);
        HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, r.left, r.top, r.right, r.bottom, rad, rad);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(pen);
    }
    DeleteObject(rgn);
}

static void draw_text(HDC hdc, HFONT f, COLORREF col, RECT r,
                      const wchar_t *s, UINT fmt)
{
    HGDIOBJ of = SelectObject(hdc, f);
    SetTextColor(hdc, col);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, s, -1, &r, fmt);
    SelectObject(hdc, of);
}

static void draw_lightning(HDC hdc, int x, int y, int s, COLORREF col)
{
    /* bolt polygon inside an s-by-s box at (x,y) */
    POINT p[6] = {
        { x + s*56/100, y + s* 6/100 },
        { x + s*34/100, y + s*48/100 },
        { x + s*47/100, y + s*48/100 },
        { x + s*42/100, y + s*94/100 },
        { x + s*70/100, y + s*40/100 },
        { x + s*55/100, y + s*40/100 },
    };
    HBRUSH br = CreateSolidBrush(col);
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);
    Polygon(hdc, p, 6);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

static void draw_speaker(HDC hdc, int x, int y)
{
    HBRUSH br = CreateSolidBrush(COL_GRAY);
    HPEN pen = CreatePen(PS_SOLID, 1, COL_GRAY);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);
    POINT cone[4] = { {x+7,y+4}, {x+15,y+0}, {x+15,y+16}, {x+7,y+12} };
    Polygon(hdc, cone, 4);
    Rectangle(hdc, x, y+5, x+7, y+11);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
    DeleteObject(pen);
    /* sound waves */
    HPEN wp = CreatePen(PS_SOLID, 2, COL_GRAY);
    op = SelectObject(hdc, wp);
    ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Arc(hdc, x+16, y+2, x+26, y+14, x+19, y+5, x+19, y+11);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(wp);
}

/* ============================================================================
 * Main paint
 * ========================================================================== */
static void paint_ui(HDC hdc, int W, int H)
{
    RECT rc = { 0, 0, W, H };
    HBRUSH bg = CreateSolidBrush(COL_BG);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    LONG active = InterlockedCompareExchange(&g_activePreset, -1, -1);
    BOOL playing = active >= 0;

    /* card */
    RECT card = { 14, 14, W - 14, H - 14 };
    fill_round_rect(hdc, card, 36, COL_CARD, COL_BORDER, 1);

    /* header icon */
    RECT ir = { 32, 32, 80, 80 };
    fill_round_rect(hdc, ir, 28, COL_ICON_BG, COL_BORDER, 1);
    draw_lightning(hdc, 40, 40, 32, COL_CYAN);

    /* title + subtitle */
    RECT tr = { 94, 32, 340, 58 };
    draw_text(hdc, g_fTitle, COL_TEXT, tr, APP_TITLE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    wchar_t sub[128];
    if (playing) {
        ULONGLONG s = (GetTickCount64() - g_playStartTick) / 1000;
        swprintf(sub, 128, L"Playing %s (%s) \u00B7 %02llu:%02llu",
                 g_names[active], g_hzshort[active], s / 60, s % 60);
        RECT sr = { 94, 58, 340, 80 };
        draw_text(hdc, g_fSub, g_cols[active], sr, sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        RECT sr = { 94, 58, 340, 80 };
        draw_text(hdc, g_fSub, COL_GRAY, sr, L"Ready (Headphones on)",
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    /* status pill */
    g_rcPill.left = 348; g_rcPill.top = 34; g_rcPill.right = 452; g_rcPill.bottom = 70;
    fill_round_rect(hdc, g_rcPill, 24, COL_BG, COL_BORDER, 1);
    if (playing) {
        HPEN pen = CreatePen(PS_SOLID, 2, COL_CYAN);
        HGDIOBJ op = SelectObject(hdc, pen);
        HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        POINT pts[48];
        int n = 0, midY = (g_rcPill.top + g_rcPill.bottom) / 2;
        for (int x = g_rcPill.left + 10; x <= g_rcPill.right - 10 && n < 48; x += 3) {
            pts[n].x = x;
            pts[n].y = (LONG)(midY + sin(x * 0.28 + g_waveT) * 8.0);
            n++;
        }
        Polyline(hdc, pts, n);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(pen);
    } else {
        draw_text(hdc, g_fPill, COL_GRAY, g_rcPill, L"IDLE",
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    /* preset tiles */
    int px[3] = { 30, 176, 322 };
    for (int i = 0; i < 3; i++) {
        g_rcPreset[i].left = px[i]; g_rcPreset[i].top = 104;
        g_rcPreset[i].right = px[i] + 134; g_rcPreset[i].bottom = 168;
        BOOL isActive = (active == i);
        BOOL isHov = (g_hover == i);
        COLORREF fill = isHov ? COL_TILE_HOV : COL_TILE;
        COLORREF bd = isActive ? g_cols[i] : COL_BORDER;
        fill_round_rect(hdc, g_rcPreset[i], 24, fill, bd, isActive ? 2 : 1);

        RECT nr = g_rcPreset[i]; nr.top += 8; nr.bottom = nr.top + 24;
        draw_text(hdc, g_fBtn, g_cols[i], nr, g_names[i],
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT hr = g_rcPreset[i]; hr.top += 32; hr.bottom = hr.top + 22;
        draw_text(hdc, g_fHz, COL_GRAY, hr, g_hz[i],
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    /* volume row */
    draw_speaker(hdc, 34, 200);
    int tx = playing ? TRACK_X_PLAY : TRACK_X_IDLE;
    int tw = playing ? TRACK_W_PLAY : TRACK_W_IDLE;
    g_rcTrack.left = tx; g_rcTrack.top = 205;
    g_rcTrack.right = tx + tw; g_rcTrack.bottom = 211;
    fill_round_rect(hdc, g_rcTrack, 6, COL_BORDER, 0, 0);

    int kx = tx + (g_volume * tw) / 100;
    RECT fillr = { tx, 205, kx, 211 };
    if (kx > tx) fill_round_rect(hdc, fillr, 6, COL_CYAN, 0, 0);

    HBRUSH kb = CreateSolidBrush(COL_CYAN);
    HGDIOBJ ob2 = SelectObject(hdc, kb);
    HPEN kp = CreatePen(PS_SOLID, 1, RGB(14, 116, 144));
    HGDIOBJ op2 = SelectObject(hdc, kp);
    Ellipse(hdc, kx - 9, 199, kx + 9, 217);
    SelectObject(hdc, ob2);
    SelectObject(hdc, op2);
    DeleteObject(kb);
    DeleteObject(kp);

    wchar_t pct[16];
    swprintf(pct, 16, L"%d%%", g_volume);
    if (playing) {
        RECT pr = { tx + tw + 8, 196, tx + tw + 58, 220 };
        draw_text(hdc, g_fPct, COL_CYAN, pr, pct, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        /* stop button */
        g_rcStop.left = 400; g_rcStop.top = 192;
        g_rcStop.right = 440; g_rcStop.bottom = 224;
        BOOL hov = (g_hover == 3);
        fill_round_rect(hdc, g_rcStop, 20, hov ? RGB(74, 28, 40) : COL_RED_BG,
                        COL_RED_BD, 1);
        HPEN xp = CreatePen(PS_SOLID, 2, COL_RED);
        HGDIOBJ oxp = SelectObject(hdc, xp);
        MoveToEx(hdc, g_rcStop.left + 14, g_rcStop.top + 10, NULL);
        LineTo(hdc, g_rcStop.right - 14, g_rcStop.bottom - 10);
        MoveToEx(hdc, g_rcStop.right - 14, g_rcStop.top + 10, NULL);
        LineTo(hdc, g_rcStop.left + 14, g_rcStop.bottom - 10);
        SelectObject(hdc, oxp);
        DeleteObject(xp);
    } else {
        RECT pr = { tx + tw + 8, 196, 452, 220 };
        draw_text(hdc, g_fPct, COL_CYAN, pr, pct, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        g_rcStop.left = g_rcStop.top = g_rcStop.right = g_rcStop.bottom = 0;
    }
}

/* ============================================================================
 * Input
 * ========================================================================== */
static int hit_test(int x, int y)
{
    LONG active = InterlockedCompareExchange(&g_activePreset, -1, -1);
    for (int i = 0; i < 3; i++)
        if (PtInRect(&g_rcPreset[i], (POINT){ x, y })) return i;
    if (active >= 0 && PtInRect(&g_rcStop, (POINT){ x, y })) return 3;
    return -1;
}

static BOOL in_slider(int x, int y)
{
    RECT r = g_rcTrack;
    r.left -= 12; r.right += 12; r.top -= 12; r.bottom += 12;
    return PtInRect(&r, (POINT){ x, y });
}

static void set_volume_from_x(int x)
{
    int tw = g_rcTrack.right - g_rcTrack.left;
    int v = (x - g_rcTrack.left) * 100 / (tw ? tw : 1);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_volume = v;
    g_volF = v / 100.0f;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void ensure_hover_tracking(HWND hwnd)
{
    TRACKMOUSEEVENT tme;
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hwnd;
    tme.dwHoverTime = 0;
    TrackMouseEvent(&tme);
}

/* ============================================================================
 * Window procedure
 * ========================================================================== */
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE: {
        g_hwnd = hwnd;
        HINSTANCE hi = ((LPCREATESTRUCTW)lp)->hInstance;
        g_hIconBig = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, 48, 48, 0);
        g_hIconSm  = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
        if (!g_hIconBig) g_hIconBig = LoadIconW(NULL, IDI_APPLICATION);
        if (!g_hIconSm)  g_hIconSm  = LoadIconW(NULL, IDI_APPLICATION);
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_hIconBig);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_hIconSm);

        g_fTitle = CreateFontW(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_fSub   = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_fBtn   = CreateFontW(17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_fHz    = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_fPct   = CreateFontW(17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_fPill  = CreateFontW(13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        if (!audio_init()) {
            MessageBoxW(hwnd, L"Could not open the audio device.\nPlayback will not work.",
                        APP_TITLE, MB_ICONWARNING | MB_OK);
        }
        tray_add(hwnd);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint_ui(mem, rc.right, rc.bottom);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int h = hit_test(x, y);
        if (h >= 0 && h <= 2) { start_preset(h); return 0; }
        if (h == 3) { stop_playback(); return 0; }
        if (in_slider(x, y)) {
            g_dragging = TRUE;
            SetCapture(hwnd);
            set_volume_from_x(x);
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (g_dragging) { set_volume_from_x(x); return 0; }
        int h = hit_test(x, y);
        if (h != g_hover) {
            g_hover = h;
            ensure_hover_tracking(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (g_dragging) { g_dragging = FALSE; ReleaseCapture(); }
        return 0;

    case WM_MOUSELEAVE:
        if (g_hover != -1) { g_hover = -1; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;

    case WM_TIMER:
        if (wp == IDT_WAVE) {
            g_waveT += 0.45;
            InvalidateRect(hwnd, NULL, FALSE);   /* waveform + session clock */
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) { window_hide_to_tray(); return 0; }
        break;

    case WM_CLOSE:
        window_hide_to_tray();
        return 0;

    case WM_TRAYICON:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == WM_LBUTTONUP) {
            window_show();
        } else if (LOWORD(lp) == WM_RBUTTONUP) {
            tray_menu(hwnd);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TRAY_OPEN: window_show(); break;
        case ID_TRAY_STOP: stop_playback(); break;
        case ID_TRAY_EXIT: DestroyWindow(hwnd); break;
        }
        return 0;

    case WM_DESTROY:
        stop_playback();
        /* Let the fade-out finish, then let the queued fade tail drain out
           of the driver, before tearing anything down — waveOutReset would
           otherwise amputate the tail mid-waveform and pop. (~0.3 s max) */
        for (int w = 0; w < 150; w++) {
            if (!InterlockedCompareExchange(&g_playing, 0, 0)) {
                int busy = 0;
                for (int k = 0; k < NUM_BUFS; k++)
                    if (InterlockedCompareExchange(&g_bufState[k], 0, 0)) { busy = 1; break; }
                if (!busy) break;
            }
            Sleep(10);
        }
        audio_shutdown();
        g_nid.uFlags = 0;
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        DeleteObject(g_fTitle); DeleteObject(g_fSub); DeleteObject(g_fBtn);
        DeleteObject(g_fHz);    DeleteObject(g_fPct); DeleteObject(g_fPill);
        if (g_hIconBig) DestroyIcon(g_hIconBig);
        if (g_hIconSm)  DestroyIcon(g_hIconSm);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ============================================================================
 * Entry point
 * ========================================================================== */
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmd, int show)
{
    (void)hp; (void)cmd;
    SetProcessDPIAware();

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hi;
    wc.lpszClassName = WND_CLASS;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    HICON appIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                      32, 32, LR_DEFAULTCOLOR);
    wc.hIcon = appIcon ? appIcon : LoadIconW(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);

    RECT wr = { 0, 0, 480, 300 };
    AdjustWindowRect(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(0, WND_CLASS, APP_TITLE,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sw - (wr.right - wr.left)) / 2, (sh - (wr.bottom - wr.top)) / 2,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, hi, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}
