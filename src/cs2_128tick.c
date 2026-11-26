/*
 * CS2 128-Tick Patcher (EXPERIMENTAL) - Windows GUI, FACEIT-style theme
 * LAN / -insecure use only. Never on official Valve / VAC servers.
 *
 * CLI also works:  cs2_128tick.exe status|patch|undo [128|256] ["<CS2 folder>"]
 *                  cs2_128tick.exe play [128|256]   (opens the window and launches CS2)
 */
#include "core.inc"   /* shared helpers: file IO, SHA-256, patch state */
#include <commctrl.h>
#include <tlhelp32.h>
#include <shellapi.h>

/* ---- theme (FACEIT-like) ---- */
#define C_BG      RGB(0x14,0x14,0x16)
#define C_PANEL   RGB(0x1E,0x1E,0x21)
#define C_ORANGE  RGB(0xFF,0x55,0x00)   /* FACEIT orange - primary button only */
#define C_ORANGE2 RGB(0xD9,0x4A,0x00)
#define C_SOFT    RGB(0xFF,0x8A,0x50)   /* softer orange for labels / outlines */
#define C_TEXT    RGB(0xF2,0xF2,0xF2)
#define C_BODY    RGB(0xC8,0xC8,0xCC)
#define C_MUTED   RGB(0x8E,0x8E,0x96)
#define C_LINE    RGB(0x4A,0x4A,0x52)
#define C_GREEN   RGB(0x7F,0xD6,0xA0)
#define C_PATCHED RGB(0xFF,0xB2,0x7A)
#define C_RED     RGB(0xF0,0x8A,0x8A)
#define C_WARNBG  RGB(0x2A,0x24,0x1A)
#define C_AMBER   RGB(0xE8,0xB0,0x4A)
#define C_WARNTXT RGB(0xD9,0xD2,0xC5)

enum { ID_TITLE = 6, ID_SUB = 7, ID_UNDO = 102, ID_LOG = 104, ID_FINDFILE = 106, ID_R128 = 110, ID_R256, ID_PLAY, ID_STATUS, ID_WARN = 400, ID_WARNBODY = 401 };

static HWND g_wnd, g_log;
static int g_state[NFILES], g_rate[NFILES];
static int g_sel_rate = 128;   /* tick rate chosen in the UI */
static char g_root[MAX_PATH * 2];
static HFONT f_title, f_sub, f_btn, f_body, f_bold, f_mono;
static HBRUSH b_bg, b_panel, b_warn;
static int g_cli = 0;
static int g_status_kind = 0;   /* 0 original, 1 patched, 2 problem */

/* ---- output: console in CLI mode, log box in GUI mode ---- */
static void out(const char *fmt, ...) {
    char buf[1024], conv[2048]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (g_cli) { fputs(buf, stdout); return; }
    char *w = conv; for (char *r = buf; *r && w - conv < (int)sizeof conv - 2; r++) { if (*r == '\n') *w++ = '\r'; *w++ = *r; } *w = 0;
    int len = GetWindowTextLengthA(g_log);
    SendMessageA(g_log, EM_SETSEL, len, len);
    SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)conv);
}

static int rate_index(int rate) { for (int i = 0; i < NRATES; i++) if (RATES[i] == rate) return i; return 0; }

static int count_edits(const PatchFile *f, int maxtier) { int n = 0; for (int i = 0; i < f->n; i++) if (f->p[i].tier <= maxtier) n++; return n; }
static const PatchFile *g_bf[NFILES];   /* build each DLL currently matches (NULL = none) */
static int total_edits(int maxtier) { int n = 0; for (int i = 0; i < NFILES; i++) n += count_edits(g_bf[i] ? g_bf[i] : &FILES[i], maxtier); return n; }

static int backup_ok(int i);
static int g_build[NFILES] = { -1, -1, -1 };
static char g_logged[NFILES][65];   /* last auto-match attempt already written to the log */

static int is_static_build(int i, const char *hash) {
    for (int k = 0; k < NALL; k++) if (!strcmp(ALL_BUILDS[k].name, FILES[i].name) && !strcmp(ALL_BUILDS[k].sha, hash)) return 1;
    return 0;
}

/* DLL i isn't a build this app ships a table for: try to match the patch to it automatically.
   Prefer the .orig64 backup as the original (the file itself may already be patched). */
static int try_auto_match(int i, const unsigned char *b, DWORD sz, const PatchFile **bf) {
    char p[MAX_PATH * 2], bak[MAX_PATH * 2], h[65];
    snprintf(p, sizeof p, "%s\\%s", g_root, FILES[i].rel); snprintf(bak, sizeof bak, "%s" BACKUP_EXT, p);
    DWORD bs = 0; unsigned char *bb = read_file(bak, &bs);
    if (bb) {
        sha256_hex(bb, bs, h);
        if (!is_static_build(i, h)) auto_port(i, bb, bs, h);
        free(bb);
        int st = get_state_any(b, sz, i, bf);
        if (st != ST_UNKNOWN) return st;
    }
    sha256_hex(b, sz, h);
    auto_port(i, b, sz, h);
    return get_state_any(b, sz, i, bf);
}

static void log_auto_match(int i) {
    if (!g_dyn_tried[i][0] || !strcmp(g_logged[i], g_dyn_tried[i])) return;
    snprintf(g_logged[i], 65, "%s", g_dyn_tried[i]);
    const PortReport *r = &g_dyn_rep[i];
    if (g_dyn_valid[i] && !strcmp(g_dyn_sha[i], g_dyn_tried[i]))
        out("%-12s newer CS2 build - patch matched automatically: %d of %d tick sites (passed all safety checks)\n", FILES[i].name, r->matched, r->sites);
    else
        out("%-12s unknown CS2 build - couldn't match the patch safely (%s). Nothing will be changed.\n", FILES[i].name, r->reason);
}

static int any_auto(void) { for (int i = 0; i < NFILES; i++) if (g_build[i] == BUILD_AUTO) return 1; return 0; }
static void refresh_status(void);
static int cs2_running(void) { return FindWindowA(NULL, "Counter-Strike 2") != NULL; }

/* ---- Play: patch, start CS2 through Steam with -insecure, undo as soon as CS2 closes ---- */
enum { PLAY_OFF, PLAY_STARTING, PLAY_RUNNING, PLAY_CLOSING };
static int g_play = PLAY_OFF;
static DWORD g_play_t0, g_play_gone;
static HANDLE g_play_proc;
static int g_autoplay = 0;            /* "play" on the command line */
#define PLAY_TIMER 1
#define START_TIMEOUT_MS (5 * 60 * 1000)   /* Steam may update CS2 or itself first */
#define RELAUNCH_GRACE_MS 8000             /* CS2 sometimes restarts itself once */

/* the game's own cs2.exe (not a dedicated server's cs2.exe from another folder) */
static DWORD find_game_pid(void) {
    char want[MAX_PATH * 2]; snprintf(want, sizeof want, "%s\\game\\bin\\win64\\cs2.exe", g_root);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe = { sizeof pe }; DWORD pid = 0;
    for (BOOL ok = Process32First(snap, &pe); ok && !pid; ok = Process32Next(snap, &pe)) {
        if (_stricmp(pe.szExeFile, "cs2.exe")) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        char path[MAX_PATH * 2]; DWORD n = sizeof path;
        if (QueryFullProcessImageNameA(h, 0, path, &n) && !_stricmp(path, want)) pid = pe.th32ProcessID;
        CloseHandle(h);
    }
    CloseHandle(snap);
    return pid;
}

/* a note in %LOCALAPPDATA% while a Play session is on, so the patch is undone next time if this window was closed */
static void session_path(char *out, size_t n) {
    char base[MAX_PATH] = "."; DWORD k = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base);
    if (!k || k >= sizeof base) strcpy(base, ".");
    snprintf(out, n, "%s\\CS2-128-tick", base); CreateDirectoryA(out, NULL);
    strncat(out, "\\play_session.txt", n - strlen(out) - 1);
}
static void session_write(void) { char p[MAX_PATH * 2]; session_path(p, sizeof p); write_file(p, (unsigned char *)g_root, (DWORD)strlen(g_root)); }
static void session_clear(void) { char p[MAX_PATH * 2]; session_path(p, sizeof p); DeleteFileA(p); }
static int session_read(char *root, size_t n) {
    char p[MAX_PATH * 2]; session_path(p, sizeof p);
    DWORD sz = 0; unsigned char *b = read_file(p, &sz); if (!b) return 0;
    snprintf(root, n, "%.*s", (int)sz, (char *)b); free(b); return root[0] != 0;
}

static int start_cs2(void) {
    char steam[MAX_PATH * 2] = {0}; DWORD sz = sizeof steam;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamExe", RRF_RT_REG_SZ, NULL, steam, &sz) != ERROR_SUCCESS || !steam[0]) {
        sz = sizeof steam;
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, NULL, steam, &sz) == ERROR_SUCCESS)
            strncat(steam, "\\steam.exe", sizeof steam - strlen(steam) - 1);
    }
    for (char *c = steam; *c; c++) if (*c == '/') *c = '\\';
    if (!steam[0] || GetFileAttributesA(steam) == INVALID_FILE_ATTRIBUTES) { out("Couldn't find steam.exe - start CS2 yourself with -insecure.\n"); return 0; }
    /* -applaunch passes -insecure without Steam's "launch with these options?" prompt, on top of your own launch options */
    HINSTANCE r = ShellExecuteA(g_wnd, "open", steam, "-applaunch 730 -insecure", NULL, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) { out("Couldn't start Steam (%s).\n", steam); return 0; }
    out("Starting CS2 through Steam with -insecure ...\n");
    return 1;
}

static void play_buttons(void) {
    if (!g_wnd || g_cli) return;
    HWND b = GetDlgItem(g_wnd, ID_PLAY);
    char t[64];
    if (g_play == PLAY_OFF) snprintf(t, sizeof t, "Launch CS2  -  %d tick", g_sel_rate);
    else snprintf(t, sizeof t, g_play == PLAY_STARTING ? "Starting CS2..." : "Playing at %d tick", g_sel_rate);
    SetWindowTextA(b, t);
    int patched = 0;
    for (int i = 0; i < NFILES; i++) if (g_state[i] == ST_SAFE || g_state[i] == ST_AGGR) patched = 1;
    EnableWindow(b, g_play == PLAY_OFF);
    EnableWindow(GetDlgItem(g_wnd, ID_UNDO), g_play == PLAY_OFF && patched);
    EnableWindow(GetDlgItem(g_wnd, ID_FINDFILE), g_play == PLAY_OFF);
    EnableWindow(GetDlgItem(g_wnd, ID_R128), g_play == PLAY_OFF);
    EnableWindow(GetDlgItem(g_wnd, ID_R256), g_play == PLAY_OFF);
    InvalidateRect(g_wnd, NULL, TRUE);
}

static int do_undo(void);

/* back to 64 tick after CS2 has closed. Retries for a while: Windows can hold the DLLs for a moment after exit. */
static void play_finish(void) {
    if (g_play_proc) { CloseHandle(g_play_proc); g_play_proc = NULL; }
    out("CS2 closed - restoring the original 64-tick files.\n");
    int ok = 0;
    for (int tries = 0; tries < 15 && !ok; tries++) {
        refresh_status();
        int all_orig = 1;
        for (int i = 0; i < NFILES; i++) if (g_state[i] != ST_ORIGINAL && g_state[i] != ST_MISSING) all_orig = 0;
        if (all_orig) {   /* e.g. Steam updated CS2 while it was starting: Valve's new files, nothing to undo */
            for (int i = 0; i < NFILES; i++) { char bak[MAX_PATH * 2]; snprintf(bak, sizeof bak, "%s\\%s" BACKUP_EXT, g_root, FILES[i].rel); DeleteFileA(bak); }
            out("Files are already Valve's originals - nothing to undo.\n\n"); ok = 1; break;
        }
        ok = do_undo();
        if (!ok) Sleep(2000);
    }
    g_play = PLAY_OFF; KillTimer(g_wnd, PLAY_TIMER);
    if (ok) session_clear();
    else out("Couldn't restore everything - click Undo, or use Steam > CS2 > Properties > Installed Files > Verify integrity.\n");
    refresh_status(); play_buttons();
    if (ok && g_autoplay) PostMessageA(g_wnd, WM_CLOSE, 0, 0);   /* started from a shortcut: nothing left to do */
}

static void play_tick(void) {
    DWORD now = GetTickCount();
    if (g_play == PLAY_STARTING) {
        DWORD pid = find_game_pid();
        if (pid) {
            g_play_proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
            g_play = PLAY_RUNNING; play_buttons();
            out("CS2 is running at %d tick. It goes back to 64 tick when you close CS2 - keep this window open (minimised is fine).\n", g_sel_rate);
        } else if (now - g_play_t0 > START_TIMEOUT_MS) {
            out("CS2 didn't start within 5 minutes.\n"); play_finish();
        }
    } else if (g_play == PLAY_RUNNING) {
        if (!g_play_proc || WaitForSingleObject(g_play_proc, 0) == WAIT_OBJECT_0) { g_play = PLAY_CLOSING; g_play_gone = now; }
    } else if (g_play == PLAY_CLOSING) {
        DWORD pid = find_game_pid();
        if (pid) {   /* it restarted itself */
            if (g_play_proc) CloseHandle(g_play_proc);
            g_play_proc = OpenProcess(SYNCHRONIZE, FALSE, pid); g_play = PLAY_RUNNING;
        } else if (now - g_play_gone > RELAUNCH_GRACE_MS) play_finish();
    }
}

static int do_patch(void);
static int confirm_patch(void);

/* the VAC / downsides warning is shown the first time each tick rate is used */
static int confirm_once(void) {
    char p[MAX_PATH * 2], tag[32]; session_path(p, sizeof p);
    char *sl = strrchr(p, '\\'); if (sl) sl[1] = 0;
    snprintf(tag, sizeof tag, "accepted_%d.txt", g_sel_rate); strncat(p, tag, sizeof p - strlen(p) - 1);
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) return 1;
    if (!confirm_patch()) return 0;
    write_file(p, (unsigned char *)"yes", 3);
    return 1;
}

static void play_start(void) {
    if (g_play != PLAY_OFF) return;
    if (!g_root[0]) { out("Pick your CS2 folder first.\n"); return; }
    if (g_state[2] == ST_MISSING) { out("Play needs a game install (this folder is a dedicated server).\n"); return; }
    if (find_game_pid() || cs2_running()) { out("Close CS2 first - Play starts it for you.\n"); return; }
    if (!confirm_once()) return;
    out("--- Launch at %d tick ---\n", g_sel_rate);
    if (!do_patch()) { refresh_status(); return; }
    session_write();
    refresh_status();
    if (!start_cs2()) { out("--- Undo ---\n"); do_undo(); session_clear(); refresh_status(); return; }
    g_play = PLAY_STARTING; g_play_t0 = GetTickCount();
    SetTimer(g_wnd, PLAY_TIMER, 1000, NULL);
    play_buttons();
}

/* at startup: a Play session left over (window closed while playing, PC crashed ...) */
static void play_resume(void) {
    char root[MAX_PATH * 2];
    if (!session_read(root, sizeof root)) return;
    if (!valid_root(root)) { session_clear(); return; }
    snprintf(g_root, sizeof g_root, "%s", root);
    refresh_status();
    for (int i = 0; i < NFILES; i++) if (g_rate[i] == 128 || g_rate[i] == 256) { g_sel_rate = g_rate[i]; break; }
    if (find_game_pid()) {
        g_play = PLAY_CLOSING; g_play_gone = GetTickCount();   /* picks the running CS2 up on the first tick */
        SetTimer(g_wnd, PLAY_TIMER, 1000, NULL);
        out("CS2 is still running from your last Play - it goes back to 64 tick when CS2 closes.\n");
        play_buttons();
    } else {
        out("Your last Play didn't get to restore 64 tick - doing it now.\n");
        g_play = PLAY_RUNNING; g_play_proc = NULL; play_finish();
    }
}

static void refresh_status(void) {
    static int busy = 0;
    if (busy) return;
    busy = 1;
    for (int i = 0; i < NFILES; i++) {   /* pass 1: detect state + build of every DLL */
        char p[MAX_PATH * 2]; snprintf(p, sizeof p, "%s\\%s", g_root, FILES[i].rel);
        DWORD sz = 0; unsigned char *b = g_root[0] ? read_file(p, &sz) : NULL;
        const PatchFile *bf; g_state[i] = get_state_any(b, sz, i, &bf);
        if (g_state[i] == ST_UNKNOWN) { g_state[i] = try_auto_match(i, b, sz, &bf); log_auto_match(i); }
        g_rate[i] = g_detected_rate; g_build[i] = bf ? bf->build : -1; g_bf[i] = bf; free(b);
    }
    for (int i = 0; i < NFILES; i++) {   /* pass 2: show it */
        if (!g_cli) continue;
        if (g_state[i] == ST_SAFE || g_state[i] == ST_AGGR) printf("  %-12s patched - %d tick%s%s\n", FILES[i].name, g_rate[i], g_state[i] == ST_AGGR ? "" : " (partial, patch again)", g_build[i] == BUILD_AUTO ? " (auto-matched build)" : "");
        else printf("  %-12s %s\n", FILES[i].name, ST_NAME[g_state[i]]);
    }
    if (!g_cli) {   /* one status line */
        char t[MAX_PATH * 3]; int patched = 0, unknown = 0, rate = 0;
        for (int i = 0; i < NFILES; i++) {
            if (g_state[i] == ST_SAFE || g_state[i] == ST_AGGR) { patched = 1; rate = g_rate[i]; }
            if (g_state[i] == ST_UNKNOWN && !backup_ok(i)) unknown = 1;
        }
        if (!g_root[0]) snprintf(t, sizeof t, "CS2 not found - click Change folder and pick engine2.dll");
        else if (g_state[2] == ST_MISSING) snprintf(t, sizeof t, "%s\nThat's a dedicated server - use the server pack, or Change folder to your game", g_root);
        else if (unknown) snprintf(t, sizeof t, "%s\nThis CS2 version couldn't be matched safely - wait for an update of this app", g_root);
        else if (patched) snprintf(t, sizeof t, "%s\nPatched - %d tick%s", g_root, rate, g_play == PLAY_OFF ? "  (Restore 64 tick puts it back)" : "  - back to 64 when CS2 closes");
        else snprintf(t, sizeof t, "%s\nOriginal - 64 tick%s", g_root, any_auto() ? "  (newer CS2 build, matched automatically)" : "");
        g_status_kind = !g_root[0] || unknown || g_state[2] == ST_MISSING ? 2 : patched ? 1 : 0;
        SetWindowTextA(GetDlgItem(g_wnd, ID_STATUS), t);
        play_buttons();
        InvalidateRect(g_wnd, NULL, TRUE);
    }
    busy = 0;
}

static int backup_ok(int i) {
    char bak[MAX_PATH * 2]; snprintf(bak, sizeof bak, "%s\\%s" BACKUP_EXT, g_root, FILES[i].rel);
    DWORD bs = 0; unsigned char *bb = read_file(bak, &bs); char h[65] = "";
    int ok = 0;
    if (bb) {
        sha256_hex(bb, bs, h); free(bb);
        const PatchFile *bp = build_by_hash(i, h);
        /* only trust a backup from the same CS2 build as the rest of this install */
        int other = -1;
        for (int j = 0; j < NFILES; j++) if (j != i && g_build[j] >= 0) { other = g_build[j]; break; }
        ok = bp && (other < 0 || bp->build == other);
    }
    return ok;
}

static int do_patch(void) {
    const int aggressive = 1;   /* always apply the full patch set (tier 0 + tier 1) */
    if (cs2_running()) { out("Close CS2 and your server first.\n"); return 0; }
    for (int i = 0; i < NFILES; i++)
        if (g_state[i] == ST_UNKNOWN && backup_ok(i)) continue;   /* e.g. patched by v1: rebuild from .orig64 */
        else if (g_state[i] == ST_MISSING && i != 0) continue;    /* e.g. no client.dll on a dedicated server */
        else if (g_state[i] == ST_MISSING || g_state[i] == ST_UNKNOWN) {
            out("%s is %s.\nThese offsets only match the build they were made from. Nothing was changed.\n", FILES[i].name, ST_NAME[g_state[i]]);
            return 0;
        }
    int ok = 1;
    for (int i = 0; i < NFILES && ok; i++) {
        char p[MAX_PATH * 2], bak[MAX_PATH * 2];
        snprintf(p, sizeof p, "%s\\%s", g_root, FILES[i].rel); snprintf(bak, sizeof bak, "%s" BACKUP_EXT, p);
        if (g_state[i] == ST_MISSING) { out("%-12s not in this install - skipped\n", FILES[i].name); continue; }
        DWORD sz; unsigned char *b = read_file(p, &sz);
        if (!b) { out("%s: can't read\n", FILES[i].name); ok = 0; break; }
        const PatchFile *bf; int st = get_state_any(b, sz, i, &bf);
        if (st == ST_UNKNOWN) {
            free(b); b = read_file(bak, &sz); char hh[65]; sha256_hex(b, sz, hh); bf = build_by_hash(i, hh); st = ST_ORIGINAL;
            out("%-12s older patch found - rebuilding from backup\n", FILES[i].name);
        }
        if (st == ST_SAFE || st == ST_AGGR) set_entries(b, bf, 1, 0, 0);
        {   /* a backup left from an older CS2 build is stale: replace it with this build's original */
            DWORD os2 = 0; unsigned char *ob = read_file(bak, &os2); char oh[65] = "";
            if (ob) { sha256_hex(ob, os2, oh); free(ob); if (strcmp(oh, bf->sha)) DeleteFileA(bak); }
        }
        if (GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES && !write_file(bak, b, sz)) {
            out("%s: couldn't write backup - run as administrator\n", FILES[i].name); free(b); ok = 0; break;
        }
        set_entries(b, bf, aggressive, 1, g_sel_rate);
        if (!write_file(p, b, sz)) { out("%s: write FAILED (in use? run as admin?)\n", FILES[i].name); ok = 0; }
        else out("%-12s patched to %d tick (%d edits), backup %s%s\n", FILES[i].name, g_sel_rate, count_edits(bf, aggressive), FILES[i].name, BACKUP_EXT);
        free(b);
    }
    if (ok) {
        out("Done - %d tick.\nLaunch CS2 AND your server with  -insecure\nDo NOT connect to official Valve servers / matchmaking until you click UNDO.\n", g_sel_rate);
        out("Check it's working: join the server, open the console (~) and run   cl_ticktiming print\n"
            "  'FirstInput -> NetSend' should be a bit under %s ms (stock 64 tick: about 15 ms).\n\n", g_sel_rate > 128 ? "3.9" : "7.8");
    }
    return ok;
}

static int do_undo(void) {
    if (cs2_running()) { out("Close CS2 and your server first.\n"); return 0; }
    int ok = 1;
    for (int i = 0; i < NFILES; i++) {
        char p[MAX_PATH * 2], bak[MAX_PATH * 2];
        snprintf(p, sizeof p, "%s\\%s", g_root, FILES[i].rel); snprintf(bak, sizeof bak, "%s" BACKUP_EXT, p);
        DWORD sz = 0; unsigned char *b = read_file(p, &sz);
        const PatchFile *bf; int st = get_state_any(b, sz, i, &bf);
        if (st == ST_MISSING) { continue; }
        if (st == ST_ORIGINAL) { out("%-12s already original\n", FILES[i].name); DeleteFileA(bak); }
        else if (st == ST_SAFE || st == ST_AGGR) {
            set_entries(b, bf, 1, 0, 0);
            if (write_file(p, b, sz)) { out("%-12s restored to original 64 tick\n", FILES[i].name); DeleteFileA(bak); }
            else { out("%-12s restore FAILED (in use? run as admin?)\n", FILES[i].name); ok = 0; }
        } else {
            DWORD bs = 0; unsigned char *bb = read_file(bak, &bs); char h[65] = "";
            if (bb) sha256_hex(bb, bs, h);
            if (bb && backup_ok(i) && write_file(p, bb, bs)) { out("%-12s restored from backup\n", FILES[i].name); DeleteFileA(bak); }
            else { out("%-12s %s - use Steam > CS2 > Properties > Installed Files > Verify integrity\n", FILES[i].name, ST_NAME[st]); ok = 0; }
            free(bb);
        }
        free(b);
    }
    if (ok) out("Undo complete. Files are back to stock - safe to play online again.\n\n");
    return ok;
}

static int confirm_patch(void) {
    char msg[4000];
    const char *extra = g_sel_rate > 128 ?
        "256 TICK DOWNSIDES:\n"
        "- about 4x server CPU and network traffic vs stock\n"
        "- your fps must stay above 256 or movement will jitter\n"
        "- any 64-tick maths the patch missed is 4x off, so bugs are more likely\n"
        "- lag compensation likely covers less time: worse for high-ping players\n"
        "- change map / restart every few hours to keep timing precise\n"
        "- only about 3.9 ms gained per tick over 128\n\n" : "";
    char autonote[600] = "";
    if (any_auto()) {
        char parts[300] = "";
        for (int i = 0; i < NFILES; i++) if (g_build[i] == BUILD_AUTO) {
            char one[80]; snprintf(one, sizeof one, "%s%s %d/%d", parts[0] ? ", " : "", FILES[i].name, g_dyn_rep[i].matched, g_dyn_rep[i].sites);
            strncat(parts, one, sizeof parts - strlen(parts) - 1);
        }
        snprintf(autonote, sizeof autonote,
            "NEWER CS2 BUILD: this game version is newer than the patcher. The patch was matched to it automatically "
            "(%s tick sites found) and passed all safety checks, but this build hasn't been reviewed by a person. "
            "If anything behaves oddly, click Undo.\n\n", parts);
    }
    snprintf(msg, sizeof msg,
        "Patch to %d tick (%d edits)?\n\n"
        "%s%s"
        "BEFORE YOU CONTINUE\n"
        "- Do NOT connect to official Valve servers, matchmaking, Premier or any VAC-secured server while patched. "
        "Modified game files can get your Steam account VAC banned, and Valve does not reverse VAC bans.\n"
        "- Only use this on your own LAN / private server, and launch CS2 and the server with -insecure.\n"
        "- Patch your game AND your server to the SAME tick rate (run this on the server install too if it's separate).\n"
        "- Click Undo before you play online again. A Steam update also undoes the patch.\n\n"
        "This is experimental: some game timing may still behave like 64 tick.",
        g_sel_rate, total_edits(1), autonote, extra);
    return MessageBoxA(g_wnd, msg, g_sel_rate > 128 ? "256 tick - downsides and VAC ban risk" : "Official Valve servers - VAC ban risk",
                       MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

/* accept the install folder, any folder inside it, or one of its DLLs */
static int set_root_from(const char *in) {
    char p[MAX_PATH * 2]; snprintf(p, sizeof p, "%s", in);
    char *s = p; while (*s == '"' || *s == ' ') s++; memmove(p, s, strlen(s) + 1);
    size_t L = strlen(p); while (L && (p[L-1] == '"' || p[L-1] == ' ' || p[L-1] == '\\' || p[L-1] == '/')) p[--L] = 0;
    for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
    DWORD a = GetFileAttributesA(p);
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) { char *sl = strrchr(p, '\\'); if (sl) *sl = 0; }
    for (int up = 0; up < 8 && p[0]; up++) {
        if (valid_root(p)) { snprintf(g_root, sizeof g_root, "%s", p); return 1; }
        char *sl = strrchr(p, '\\'); if (!sl) break; *sl = 0;
    }
    return 0;
}

static const char *EX_FOLDER = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive";

static void not_found_box(const char *what) {
    char m[1024];
    snprintf(m, sizeof m, "Couldn't find a CS2 install from:\n%s\n\nPick the install folder or any DLL inside it, for example:\n\n"
        "Game:   %s\n\nDLL:    ...\\Counter-Strike Global Offensive\\game\\bin\\win64\\engine2.dll\n\n"
        "SteamCMD server:   the folder you installed it to, e.g.  C:\\cs2-server", what, EX_FOLDER);
    MessageBoxA(g_wnd, m, "Not a CS2 folder", MB_ICONERROR);
}

static void find_file(void) {
    char file[MAX_PATH * 2] = "engine2.dll";
    char init[MAX_PATH * 2]; snprintf(init, sizeof init, "%s\\game\\bin\\win64", g_root[0] ? g_root : EX_FOLDER);
    OPENFILENAMEA o = {0};
    o.lStructSize = sizeof o; o.hwndOwner = g_wnd;
    o.lpstrFilter = "CS2 DLLs (engine2.dll, server.dll, client.dll)\0engine2.dll;server.dll;client.dll\0All files (*.*)\0*.*\0";
    o.lpstrFile = file; o.nMaxFile = sizeof file; o.lpstrInitialDir = init;
    o.lpstrTitle = "Find engine2.dll  -  e.g. ...\\Counter-Strike Global Offensive\\game\\bin\\win64\\engine2.dll";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&o)) return;
    if (set_root_from(file)) { refresh_status(); out("CS2 folder found from %s:\n  %s\n", file, g_root); }
    else not_found_box(file);
}

static void browse(void) {
    BROWSEINFOA bi = {0}; char name[MAX_PATH];
    bi.hwndOwner = g_wnd; bi.pszDisplayName = name;
    bi.lpszTitle = "Select the \"Counter-Strike Global Offensive\" folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pl = SHBrowseForFolderA(&bi);
    if (!pl) return;
    char path[MAX_PATH]; SHGetPathFromIDListA(pl, path); CoTaskMemFree(pl);
    if (set_root_from(path)) { refresh_status(); out("CS2 folder set: %s\n", g_root); }
    else not_found_box(path);
}

/* ---- drawing ---- */
static void fill(HDC dc, RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }

static void round_box(HDC dc, RECT r, COLORREF f, COLORREF e, int rad) {
    HBRUSH br = CreateSolidBrush(f); HPEN pen = CreatePen(PS_SOLID, 1, e);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br); DeleteObject(pen);
}

static void draw_button(DRAWITEMSTRUCT *d) {
    int id = d->CtlID, pressed = d->itemState & ODS_SELECTED, disabled = d->itemState & ODS_DISABLED;
    char t[96]; GetWindowTextA(d->hwndItem, t, sizeof t);
    SetBkMode(d->hDC, TRANSPARENT);
    if (id == ID_R128 || id == ID_R256) {   /* tick boxes: one of the two is always ticked */
        int rate = id == ID_R128 ? 128 : 256, sel = rate == g_sel_rate;
        FillRect(d->hDC, &d->rcItem, b_panel);
        int cy = (d->rcItem.top + d->rcItem.bottom) / 2;
        RECT box = { d->rcItem.left + 2, cy - 11, d->rcItem.left + 24, cy + 11 };
        COLORREF on = rate == 128 ? C_ORANGE : C_AMBER;
        round_box(d->hDC, box, sel ? on : C_BG, disabled ? C_LINE : sel ? on : C_MUTED, 6);
        if (sel) {
            HPEN pen = CreatePen(PS_SOLID, 3, rate == 128 ? RGB(0xFF,0xFF,0xFF) : RGB(0x1E,0x1A,0x12));
            HGDIOBJ op = SelectObject(d->hDC, pen);
            POINT pt[3] = { { box.left + 5, cy }, { box.left + 9, cy + 5 }, { box.left + 17, cy - 5 } };
            Polyline(d->hDC, pt, 3);
            SelectObject(d->hDC, op); DeleteObject(pen);
        }
        RECT tr = d->rcItem; tr.left = box.right + 10;
        SelectObject(d->hDC, f_btn); SetTextColor(d->hDC, disabled ? C_MUTED : sel ? C_TEXT : C_BODY);
        DrawTextA(d->hDC, t, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (id == ID_PLAY) {   /* the launch button */
        COLORREF f = disabled ? RGB(0x6A,0x3A,0x22) : pressed ? C_ORANGE2 : C_ORANGE;
        FillRect(d->hDC, &d->rcItem, b_bg);
        round_box(d->hDC, d->rcItem, f, f, 12);
        SelectObject(d->hDC, f_title); SetTextColor(d->hDC, disabled ? RGB(0xE0,0xC0,0xB0) : RGB(0xFF,0xFF,0xFF));
        DrawTextA(d->hDC, t, -1, &d->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    /* small text buttons: Change folder / Restore 64 tick */
    FillRect(d->hDC, &d->rcItem, b_bg);
    RECT r = d->rcItem;
    round_box(d->hDC, r, pressed ? RGB(0x2A,0x2A,0x2E) : C_BG, disabled ? C_PANEL : C_LINE, 8);
    SelectObject(d->hDC, f_body); SetTextColor(d->hDC, disabled ? C_LINE : C_BODY);
    DrawTextA(d->hDC, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

#define W 560
#define PAD 22
static RECT r_warn, r_rate;
static int g_clientH, g_scroll, g_viewH;

static int measure(HFONT f, const char *txt, int w) {
    HDC dc = GetDC(NULL); HGDIOBJ o = SelectObject(dc, f);
    RECT r = { 0, 0, w, 0 };
    DrawTextA(dc, txt, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, o); ReleaseDC(NULL, dc);
    return r.bottom + 2;
}

static void place(int id, HFONT f, int x, int y, int w, int hgt) {
    HWND c = GetDlgItem(g_wnd, id);
    SendMessage(c, WM_SETFONT, (WPARAM)f, 0);
    SetWindowPos(c, NULL, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
}

static int place_text(int id, HFONT f, int x, int y, int w) {
    char t[1024]; GetWindowTextA(GetDlgItem(g_wnd, id), t, sizeof t);
    int hh = measure(f, t, w); place(id, f, x, y, w, hh); return hh;
}

static void layout(void) {
    int x = PAD, inner = W - 2 * PAD, y = 16;
    y += place_text(ID_TITLE, f_title, x, y, inner);
    y += place_text(ID_SUB, f_sub, x, y, inner) + 12;

    int wy = y; y += 10;   /* warning */
    y += place_text(ID_WARN, f_bold, x + 16, y, inner - 26) + 2;
    y += place_text(ID_WARNBODY, f_body, x + 16, y, inner - 26) + 10;
    SetRect(&r_warn, x, wy, x + inner, y); y += 12;

    int ry = y; y += 10;   /* tick boxes */
    place(ID_R128, f_btn, x + 16, y, 150, 34);
    place(ID_R256, f_btn, x + 16 + 170, y, inner - 16 - 170 - 10, 34);
    y += 34 + 10; SetRect(&r_rate, x, ry, x + inner, y); y += 14;

    place(ID_PLAY, f_title, x, y, inner, 58); y += 58 + 12;

    place(ID_STATUS, f_sub, x, y, inner, measure(f_sub, "X\nX", inner)); y += measure(f_sub, "X\nX", inner) + 8;
    place(ID_FINDFILE, f_body, x, y, 150, 30);
    place(ID_UNDO, f_body, x + 160, y, 150, 30);
    y += 30 + 12;
    place(ID_LOG, f_mono, x, y, inner, 78); y += 78 + 14;
    g_clientH = y;
}

static void set_rate(int r) { g_sel_rate = r; play_buttons(); }

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        HINSTANCE hi = ((CREATESTRUCT *)lp)->hInstance;
        g_wnd = h;
        #define MK(cls, txt, st, id) CreateWindowA(cls, txt, WS_CHILD | WS_VISIBLE | (st), 0, 0, 10, 10, h, (HMENU)(INT_PTR)(id), hi, NULL)
        MK("STATIC", "CS2 128 Tick", SS_NOPREFIX, ID_TITLE);
        MK("STATIC", "v2.6  \x95  Experimental  \x95  For your own LAN / -insecure server", SS_NOPREFIX, ID_SUB);
        MK("STATIC", "Only join your own -insecure server", SS_NOPREFIX, ID_WARN);
        MK("STATIC", "CS2 is patched while it's open, so never join matchmaking, Premier or VAC servers with it - "
                     "modified game files can get your account VAC banned. The original 64-tick files are put back "
                     "as soon as CS2 closes.", SS_NOPREFIX, ID_WARNBODY);
        MK("BUTTON", "128 tick", BS_OWNERDRAW, ID_R128);
        MK("BUTTON", "256 tick  (experimental)", BS_OWNERDRAW, ID_R256);
        MK("BUTTON", "Launch CS2", BS_OWNERDRAW, ID_PLAY);
        MK("STATIC", "", SS_NOPREFIX, ID_STATUS);
        MK("BUTTON", "Change folder...", BS_OWNERDRAW, ID_FINDFILE);
        MK("BUTTON", "Restore 64 tick", BS_OWNERDRAW, ID_UNDO);
        g_log = MK("EDIT", "", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, ID_LOG);
        layout();
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC dc = (HDC)wp; RECT r; GetClientRect(h, &r); FillRect(dc, &r, b_bg);
        SetViewportOrgEx(dc, 0, -g_scroll, NULL);
        fill(dc, r_warn, C_WARNBG);
        RECT a = r_warn; a.right = a.left + 3; fill(dc, a, C_AMBER);
        fill(dc, r_rate, C_PANEL);
        RECT c = r_rate; c.right = c.left + 3; fill(dc, c, g_sel_rate > 128 ? C_AMBER : C_ORANGE);
        SetViewportOrgEx(dc, 0, 0, NULL);
        return 1;
    }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp; int id = GetDlgCtrlID((HWND)lp);
        SetBkMode(dc, TRANSPARENT);
        if (id == ID_LOG) { SetBkColor(dc, C_BG); SetTextColor(dc, C_MUTED); SetBkMode(dc, OPAQUE); return (LRESULT)b_bg; }
        if (id == ID_TITLE) { SetTextColor(dc, C_TEXT); return (LRESULT)b_bg; }
        if (id == ID_SUB) { SetTextColor(dc, C_SOFT); return (LRESULT)b_bg; }
        if (id == ID_WARN) { SetTextColor(dc, C_AMBER); return (LRESULT)b_warn; }
        if (id == ID_WARNBODY) { SetTextColor(dc, C_WARNTXT); return (LRESULT)b_warn; }
        if (id == ID_STATUS) { SetTextColor(dc, g_status_kind == 2 ? C_RED : g_status_kind == 1 ? C_PATCHED : C_GREEN); return (LRESULT)b_bg; }
        SetTextColor(dc, C_TEXT); return (LRESULT)b_bg;
    }
    case WM_DRAWITEM: draw_button((DRAWITEMSTRUCT *)lp); return TRUE;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (HIWORD(wp) != BN_CLICKED) return 0;
        if (id == ID_R128 || id == ID_R256) { set_rate(id == ID_R128 ? 128 : 256); return 0; }
        if (id == ID_FINDFILE) { find_file(); return 0; }
        if (id == ID_PLAY) { if (!g_root[0]) find_file(); if (g_root[0]) play_start(); return 0; }
        if (id == ID_UNDO) {
            HCURSOR oc = SetCursor(LoadCursor(NULL, IDC_WAIT));
            out("--- Restore 64 tick ---\n"); if (do_undo()) session_clear();
            SetCursor(oc); refresh_status(); return 0;
        }
        return 0;
    }
    case WM_TIMER: if (wp == PLAY_TIMER) play_tick(); return 0;
    case WM_VSCROLL: case WM_MOUSEWHEEL: {   /* only when the window is shorter than the content (small screens) */
        if (g_viewH <= 0 || g_viewH >= g_clientH) return 0;
        int ny = g_scroll, maxs = g_clientH - g_viewH;
        if (m == WM_MOUSEWHEEL) ny -= GET_WHEEL_DELTA_WPARAM(wp) / 2;
        else switch (LOWORD(wp)) {
            case SB_LINEUP: ny -= 40; break;  case SB_LINEDOWN: ny += 40; break;
            case SB_PAGEUP: ny -= g_viewH; break; case SB_PAGEDOWN: ny += g_viewH; break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: ny = HIWORD(wp); break;
            case SB_TOP: ny = 0; break; case SB_BOTTOM: ny = maxs; break;
        }
        if (ny > maxs) ny = maxs;
        if (ny < 0) ny = 0;
        if (ny != g_scroll) {
            ScrollWindowEx(h, 0, g_scroll - ny, NULL, NULL, NULL, NULL, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
            g_scroll = ny; SetScrollPos(h, SB_VERT, g_scroll, TRUE);
            UpdateWindow(h);
        }
        return 0;
    }
    case WM_APP + 1: play_start(); return 0;   /* "play" on the command line */
    case WM_CLOSE:
        if (g_play != PLAY_OFF && MessageBoxA(h,
                "CS2 is still patched and running.\n\nIf you close this window it can't put the 64-tick files back when CS2 closes "
                "(it will do it the next time you open it). Don't join official / VAC servers until then.\n\nClose anyway?",
                "CS2 is running", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return 0;
        DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    char m[512];
    snprintf(m, sizeof m, "CS2 128-Tick Patcher crashed.\n\nCode: 0x%08lX\nAddress: %p\n\nNo game files were left half-written (each file is written in one go).\nSend this message to get it fixed.",
             (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    MessageBoxA(NULL, m, "CS2 128-Tick Patcher - error", MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

static HFONT mkfont(int sz, int weight, const char *face) {
    return CreateFontA(-sz, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}

static int set_root_from(const char *in);
static void root_from_arg(const char *a) {
    if (set_root_from(a)) return;
    snprintf(g_root, sizeof g_root, "%s", a);
    size_t L = strlen(g_root); while (L && (g_root[L-1] == '\\' || g_root[L-1] == '"')) g_root[--L] = 0;
    if (!valid_root(g_root)) g_root[0] = 0;
}

int main(int argc, char **argv) {
    SetUnhandledExceptionFilter(crash_handler);
    const char *cmd = NULL;
    for (int i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "status") || !_stricmp(argv[i], "patch") || !_stricmp(argv[i], "undo")) cmd = argv[i];
        else if (!_stricmp(argv[i], "play")) g_autoplay = 1;
        else if (!strcmp(argv[i], "128") || !strcmp(argv[i], "256")) g_sel_rate = atoi(argv[i]);
        else root_from_arg(argv[i]);
    }
    if (!g_root[0]) find_cs2(g_root, sizeof g_root);

    if (cmd) {   /* command-line mode */
        g_cli = 1;
        if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) { freopen("CONOUT$", "w", stdout); freopen("CONOUT$", "w", stderr); }
        printf("\n");
        if (!g_root[0]) { printf("CS2 not found - pass the \"Counter-Strike Global Offensive\" folder path.\n"); return 1; }
        printf("CS2: %s\n", g_root); refresh_status();
        if (!_stricmp(cmd, "status")) return 0;
        if (!_stricmp(cmd, "undo")) return do_undo() ? 0 : 1;
        printf("\nPatching to %d tick. WARNING: never connect to official Valve servers / VAC servers while patched. Use -insecure.\n", g_sel_rate);
        if (g_sel_rate > 128) printf("256 tick downsides: ~4x CPU/network, needs 256+ fps, more bugs likely, change map every few hours.\n");
        return do_patch() ? 0 : 1;
    }

    SetProcessDPIAware();
    HINSTANCE hi = GetModuleHandle(NULL);
    CoInitialize(NULL);
    f_title = mkfont(28, FW_BOLD, "Segoe UI Semibold"); f_sub = mkfont(13, FW_SEMIBOLD, "Segoe UI");
    f_btn = mkfont(15, FW_BOLD, "Segoe UI"); f_body = mkfont(14, FW_NORMAL, "Segoe UI");
    f_bold = mkfont(14, FW_BOLD, "Segoe UI"); f_mono = mkfont(14, FW_NORMAL, "Consolas");
    b_bg = CreateSolidBrush(C_BG); b_panel = CreateSolidBrush(C_PANEL); b_warn = CreateSolidBrush(C_WARNBG);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = hi; wc.lpszClassName = "CS2Tick128";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = b_bg;
    wc.hIcon = LoadIcon(hi, MAKEINTRESOURCE(1));
    RegisterClassA(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    g_wnd = CreateWindowA("CS2Tick128", "CS2 128 Tick", style, CW_USEDEFAULT, CW_USEDEFAULT, 400, 300, NULL, NULL, hi, NULL);
    {   /* size the window to fit the measured layout */
        RECT r = { 0, 0, W, g_clientH }; AdjustWindowRect(&r, style, FALSE);
        RECT wa; SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        int ww = r.right - r.left, wh = r.bottom - r.top;
        if (wh > wa.bottom - wa.top) {   /* small screen: fit it and scroll */
            int chrome = wh - g_clientH;
            wh = wa.bottom - wa.top; g_viewH = wh - chrome;
            ww += GetSystemMetrics(SM_CXVSCROLL);
            SetWindowLongA(g_wnd, GWL_STYLE, style | WS_VSCROLL);
            SCROLLINFO si = { sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, g_clientH - 1, (UINT)g_viewH, 0, 0 };
            SetScrollInfo(g_wnd, SB_VERT, &si, TRUE);
        }
        int x = wa.left + ((wa.right - wa.left) - ww) / 2, y = wa.top + ((wa.bottom - wa.top) - wh) / 2;
        if (y < wa.top) y = wa.top;
        SetWindowPos(g_wnd, NULL, x, y, ww, wh, SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    /* dark title bar on Windows 10/11 */
    { typedef HRESULT (WINAPI *DSWA)(HWND, DWORD, LPCVOID, DWORD);
      HMODULE dw = LoadLibraryA("dwmapi.dll");
      if (dw) { DSWA f = (DSWA)GetProcAddress(dw, "DwmSetWindowAttribute"); BOOL on = TRUE; if (f) f(g_wnd, 20, &on, sizeof on); } }
    refresh_status();
    if (!g_root[0]) out("CS2 not found automatically - click Change folder and pick engine2.dll.\n");
    ShowWindow(g_wnd, SW_SHOW);
    play_resume();
    play_buttons();
    if (g_autoplay && g_play == PLAY_OFF) PostMessageA(g_wnd, WM_APP + 1, 0, 0);
    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}
