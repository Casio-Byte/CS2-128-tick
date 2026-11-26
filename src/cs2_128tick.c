/*
 * CS2 128 Tick - launcher (EXPERIMENTAL)
 * For your own LAN / -insecure server only. Never on official Valve / VAC servers.
 *
 * Window: tick 128 or 256, click Patch. Start CS2 yourself with -insecure. Click Restore 64 tick when done.
 *
 * Command line (servers, scripts, the CS2 Server Pack):
 *   cs2_128tick.exe status|patch|undo [128|256] ["<CS2 folder or any DLL in it>"]
 */
#include "core.inc"   /* patch tables, SHA-256, file IO, auto-match engine */
#include <tlhelp32.h>

/* ============================================================ state */

static char g_root[MAX_PATH * 2];   /* CS2 install folder */
static int g_rate = 128;            /* chosen tick rate */
static int g_cli;                   /* running from the command line */
static HWND g_wnd, g_log;

static int g_st[NFILES];            /* ST_* per DLL */
static int g_strate[NFILES];        /* rate a patched DLL is at */
static const PatchFile *g_bf[NFILES];

static void out(const char *fmt, ...) {
    char buf[1200], conv[2400];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (g_cli || !g_log) { fputs(buf, stdout); fflush(stdout); return; }
    char *w = conv;
    for (char *r = buf; *r && w - conv < (int)sizeof conv - 2; r++) { if (*r == '\n') *w++ = '\r'; *w++ = *r; }
    *w = 0;
    int len = GetWindowTextLengthA(g_log);
    SendMessageA(g_log, EM_SETSEL, len, len);
    SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)conv);
}

static void dll_path(int i, char *p, size_t n) { snprintf(p, n, "%s\\%s", g_root, FILES[i].rel); }
static void bak_path(int i, char *p, size_t n) { snprintf(p, n, "%s\\%s" BACKUP_EXT, g_root, FILES[i].rel); }

/* ============================================================ the patch (engine in core.inc / port.inc) */

static int known_build(int i, const char *hash) {
    for (int k = 0; k < NALL; k++) if (!strcmp(ALL_BUILDS[k].name, FILES[i].name) && !strcmp(ALL_BUILDS[k].sha, hash)) return 1;
    return 0;
}

/* state of DLL i; for a CS2 build newer than the tables, match the patch automatically (from the backup if there is one) */
static int dll_state(int i, const unsigned char *b, DWORD sz, const PatchFile **bf) {
    int st = get_state_any(b, sz, i, bf);
    if (st != ST_UNKNOWN || !b) return st;
    char bak[MAX_PATH * 2], h[65]; bak_path(i, bak, sizeof bak);
    DWORD bs = 0; unsigned char *bb = read_file(bak, &bs);
    if (bb) {
        sha256_hex(bb, bs, h);
        if (!known_build(i, h)) auto_port(i, bb, bs, h);
        free(bb);
        st = get_state_any(b, sz, i, bf);
        if (st != ST_UNKNOWN) return st;
    }
    sha256_hex(b, sz, h);
    if (!known_build(i, h)) auto_port(i, b, sz, h);
    return get_state_any(b, sz, i, bf);
}

/* a backup is only trusted if it is an original of a build we know (or matched) */
static const PatchFile *backup_build(int i) {
    char bak[MAX_PATH * 2], h[65]; bak_path(i, bak, sizeof bak);
    DWORD bs = 0; unsigned char *bb = read_file(bak, &bs);
    if (!bb) return NULL;
    sha256_hex(bb, bs, h); free(bb);
    return build_by_hash(i, h);
}

static void scan(void) {
    for (int i = 0; i < NFILES; i++) {
        char p[MAX_PATH * 2]; dll_path(i, p, sizeof p);
        DWORD sz = 0; unsigned char *b = g_root[0] ? read_file(p, &sz) : NULL;
        g_st[i] = dll_state(i, b, sz, &g_bf[i]);
        g_strate[i] = g_detected_rate;
        free(b);
    }
}

static int is_patched(int i) { return g_st[i] == ST_SAFE || g_st[i] == ST_AGGR; }
static int any_patched(void) { for (int i = 0; i < NFILES; i++) if (is_patched(i)) return 1; return 0; }
static int is_server(void) { return g_st[2] == ST_MISSING; }   /* SteamCMD install: no client.dll */

/* problem that stops patching, or NULL */
static const char *scan_problem(void) {
    if (!g_root[0]) return "CS2 not found";
    if (g_st[0] == ST_MISSING) return "engine2.dll is missing";
    for (int i = 0; i < NFILES; i++)
        if (g_st[i] == ST_UNKNOWN && !backup_build(i)) return "this CS2 version couldn't be matched safely - wait for an update of this app";
    return NULL;
}

/* patch every DLL to `rate`; the originals are kept as *.orig64 */
static int patch_all(int rate) {
    scan();
    const char *pr = scan_problem();
    if (pr) { out("Not patched: %s.\n", pr); return 0; }
    for (int i = 0; i < NFILES; i++) {
        if (g_st[i] == ST_MISSING) continue;
        char p[MAX_PATH * 2], bak[MAX_PATH * 2]; dll_path(i, p, sizeof p); bak_path(i, bak, sizeof bak);
        DWORD sz = 0; unsigned char *b = read_file(p, &sz);
        if (!b) { out("%s: can't read it.\n", FILES[i].name); return 0; }
        const PatchFile *bf = g_bf[i];
        if (g_st[i] == ST_UNKNOWN) {   /* patched by an older version: start again from the backup */
            free(b); b = read_file(bak, &sz); bf = backup_build(i);
            if (!b || !bf) { out("%s: unknown file and no usable backup.\n", FILES[i].name); free(b); return 0; }
        } else if (is_patched(i)) {
            set_entries(b, bf, 1, 0, 0);   /* back to the original bytes first */
        }
        /* keep exactly this build's original as the backup */
        const PatchFile *old = backup_build(i);
        if (old && strcmp(old->sha, bf->sha)) DeleteFileA(bak);
        if (GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES && !write_file(bak, b, sz)) {
            out("%s: couldn't write the backup - run as administrator.\n", FILES[i].name); free(b); return 0;
        }
        set_entries(b, bf, 1, 1, rate);
        int ok = write_file(p, b, sz);
        free(b);
        if (!ok) { out("%s: couldn't write it (CS2 or the server still open?).\n", FILES[i].name); return 0; }
        int n = 0; for (int k = 0; k < bf->n; k++) n++;
        out("%-12s patched to %d tick (%d edits)\n", FILES[i].name, rate, n);
    }
    scan();
    return 1;
}

/* put every DLL back to the original 64-tick file and remove the backups */
static int restore_all(void) {
    scan();
    int ok = 1;
    for (int i = 0; i < NFILES; i++) {
        char p[MAX_PATH * 2], bak[MAX_PATH * 2]; dll_path(i, p, sizeof p); bak_path(i, bak, sizeof bak);
        if (g_st[i] == ST_MISSING) continue;
        if (g_st[i] == ST_ORIGINAL) { DeleteFileA(bak); continue; }
        DWORD sz = 0; unsigned char *b = read_file(p, &sz);
        if (is_patched(i) && b) {
            set_entries(b, g_bf[i], 1, 0, 0);
            if (write_file(p, b, sz)) { out("%-12s back to 64 tick\n", FILES[i].name); DeleteFileA(bak); }
            else { out("%-12s couldn't write it (still open?)\n", FILES[i].name); ok = 0; }
        } else {
            DWORD bs = 0; unsigned char *bb = backup_build(i) ? read_file(bak, &bs) : NULL;
            if (bb && write_file(p, bb, bs)) { out("%-12s restored from the backup\n", FILES[i].name); DeleteFileA(bak); }
            else { out("%-12s unknown file - use Steam > CS2 > Properties > Installed Files > Verify integrity\n", FILES[i].name); ok = 0; }
            free(bb);
        }
        free(b);
    }
    scan();
    return ok;
}

/* accept the install folder, a folder inside it, or one of its DLLs */
static int set_root(const char *in) {
    char p[MAX_PATH * 2]; snprintf(p, sizeof p, "%s", in);
    char *s = p; while (*s == '"' || *s == ' ') s++; memmove(p, s, strlen(s) + 1);
    size_t L = strlen(p); while (L && strchr("\" \\/", p[L - 1])) p[--L] = 0;
    for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
    DWORD a = GetFileAttributesA(p);
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) { char *sl = strrchr(p, '\\'); if (sl) *sl = 0; }
    for (int up = 0; up < 8 && p[0]; up++) {
        if (valid_root(p)) { snprintf(g_root, sizeof g_root, "%s", p); return 1; }
        char *sl = strrchr(p, '\\'); if (!sl) break; *sl = 0;
    }
    return 0;
}

/* ============================================================ small helpers */

static void app_dir(char *out_, size_t n) {
    char base[MAX_PATH] = "."; DWORD k = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base);
    if (!k || k >= sizeof base) strcpy(base, ".");
    snprintf(out_, n, "%s\\CS2-128-tick", base); CreateDirectoryA(out_, NULL);
}
static void app_file(const char *name, char *out_, size_t n) { char d[MAX_PATH * 2]; app_dir(d, sizeof d); snprintf(out_, n, "%s\\%s", d, name); }
static int read_text(const char *name, char *buf, size_t n) {
    char p[MAX_PATH * 2]; app_file(name, p, sizeof p);
    DWORD sz = 0; unsigned char *b = read_file(p, &sz); if (!b) return 0;
    snprintf(buf, n, "%.*s", (int)sz, (char *)b); free(b); return 1;
}
static void write_text(const char *name, const char *t) { char p[MAX_PATH * 2]; app_file(name, p, sizeof p); write_file(p, (const unsigned char *)t, (DWORD)strlen(t)); }

/* the game's own cs2.exe (by path, so a dedicated server's cs2.exe elsewhere doesn't count) */
static DWORD game_pid(void) {
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

/* ============================================================ window */

#define C_BG      RGB(0x14,0x14,0x16)
#define C_PANEL   RGB(0x1E,0x1E,0x21)
#define C_ORANGE  RGB(0xFF,0x55,0x00)
#define C_ORANGE2 RGB(0xD9,0x4A,0x00)
#define C_SOFT    RGB(0xFF,0x8A,0x50)
#define C_TEXT    RGB(0xF2,0xF2,0xF2)
#define C_BODY    RGB(0xC8,0xC8,0xCC)
#define C_MUTED   RGB(0x8E,0x8E,0x96)
#define C_LINE    RGB(0x4A,0x4A,0x52)
#define C_GREEN   RGB(0x7F,0xD6,0xA0)
#define C_PATCHED RGB(0xFF,0xB2,0x7A)
#define C_RED     RGB(0xF0,0x8A,0x8A)
#define C_WARNBG  RGB(0x2A,0x24,0x1A)
#define C_AMBER   RGB(0xE8,0xB0,0x4A)

enum { ID_TITLE = 10, ID_SUB, ID_WARN, ID_R128, ID_R256, ID_LAUNCH, ID_STATUS, ID_FOLDER, ID_RESTORE, ID_LOG };

static HFONT f_title, f_big, f_btn, f_body, f_small, f_mono;
static HBRUSH b_bg, b_panel, b_warn;
static int g_status_col;   /* 0 green, 1 orange, 2 red */
static RECT r_warn, r_rate;
#define WIN_W 520
#define PAD 20
static int g_client_h;

static void round_rect(HDC dc, RECT r, COLORREF fill, COLORREF edge, int rad) {
    HBRUSH b = CreateSolidBrush(fill); HPEN p = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}

static void update_ui(void) {
    if (!g_wnd) return;
    char t[MAX_PATH * 3];
    const char *pr = scan_problem();
    if (pr) { snprintf(t, sizeof t, "%s", !g_root[0] ? "CS2 not found - click Change folder and pick engine2.dll" : pr); g_status_col = 2; }
    else if (is_server()) { snprintf(t, sizeof t, "%s\nThat's a dedicated server - pick your game with Change folder", g_root); g_status_col = 2; }
    else if (any_patched()) {
        int r = 0; for (int i = 0; i < NFILES; i++) if (is_patched(i)) r = g_strate[i];
        snprintf(t, sizeof t, "%s\nPatched - %d tick.  Start CS2 with -insecure, and Restore 64 tick before playing online.", g_root, r);
        g_status_col = 1;
    } else { snprintf(t, sizeof t, "%s\nOriginal - 64 tick", g_root); g_status_col = 0; }
    SetDlgItemTextA(g_wnd, ID_STATUS, t);

    char lb[40]; snprintf(lb, sizeof lb, "Patch  -  %d tick", g_rate);
    SetDlgItemTextA(g_wnd, ID_LAUNCH, lb);
    EnableWindow(GetDlgItem(g_wnd, ID_LAUNCH), g_root[0] && !scan_problem() && !is_server());
    EnableWindow(GetDlgItem(g_wnd, ID_RESTORE), any_patched());
    InvalidateRect(g_wnd, NULL, TRUE);
}

static void draw_item(DRAWITEMSTRUCT *d) {
    int id = d->CtlID, down = d->itemState & ODS_SELECTED, off = d->itemState & ODS_DISABLED;
    char t[96]; GetWindowTextA(d->hwndItem, t, sizeof t);
    HDC dc = d->hDC; RECT r = d->rcItem;
    SetBkMode(dc, TRANSPARENT);
    if (id == ID_R128 || id == ID_R256) {   /* tick box */
        int rate = id == ID_R128 ? 128 : 256, on = rate == g_rate;
        COLORREF acc = rate == 128 ? C_ORANGE : C_AMBER;
        FillRect(dc, &r, b_panel);
        int cy = (r.top + r.bottom) / 2;
        RECT box = { r.left + 2, cy - 11, r.left + 24, cy + 11 };
        round_rect(dc, box, on ? acc : C_BG, on ? acc : (off ? C_LINE : C_MUTED), 6);
        if (on) {
            HPEN p = CreatePen(PS_SOLID, 3, rate == 128 ? RGB(255,255,255) : RGB(0x1E,0x1A,0x12));
            HGDIOBJ op = SelectObject(dc, p);
            POINT pt[3] = { { box.left + 5, cy }, { box.left + 9, cy + 5 }, { box.left + 17, cy - 5 } };
            Polyline(dc, pt, 3); SelectObject(dc, op); DeleteObject(p);
        }
        RECT tr = r; tr.left = box.right + 10;
        SelectObject(dc, f_btn); SetTextColor(dc, off ? C_MUTED : on ? C_TEXT : C_BODY);
        DrawTextA(dc, t, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else if (id == ID_LAUNCH) {
        FillRect(dc, &r, b_bg);
        COLORREF f = off ? RGB(0x6A,0x3A,0x22) : down ? C_ORANGE2 : C_ORANGE;
        round_rect(dc, r, f, f, 12);
        SelectObject(dc, f_big); SetTextColor(dc, off ? RGB(0xE8,0xC8,0xB8) : RGB(255,255,255));
        DrawTextA(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else if (id == ID_RESTORE) {
        FillRect(dc, &r, b_bg);
        round_rect(dc, r, down ? RGB(0x2A,0x2A,0x2E) : C_PANEL, off ? C_PANEL : C_LINE, 12);
        SelectObject(dc, f_big); SetTextColor(dc, off ? C_LINE : C_TEXT);
        DrawTextA(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        FillRect(dc, &r, b_bg);
        round_rect(dc, r, down ? RGB(0x2A,0x2A,0x2E) : C_BG, off ? C_PANEL : C_LINE, 8);
        SelectObject(dc, f_body); SetTextColor(dc, off ? C_LINE : C_BODY);
        DrawTextA(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static int text_h(HFONT f, const char *t, int w) {
    HDC dc = GetDC(NULL); HGDIOBJ o = SelectObject(dc, f);
    RECT r = { 0, 0, w, 0 }; DrawTextA(dc, t, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, o); ReleaseDC(NULL, dc); return r.bottom + 2;
}
static void put(int id, HFONT f, int x, int y, int w, int h) {
    HWND c = GetDlgItem(g_wnd, id); SendMessageA(c, WM_SETFONT, (WPARAM)f, 0);
    SetWindowPos(c, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}
static int put_text(int id, HFONT f, int x, int y, int w) {
    char t[1024]; GetDlgItemTextA(g_wnd, id, t, sizeof t);
    int h = text_h(f, t, w); put(id, f, x, y, w, h); return h;
}

static void layout(void) {
    int x = PAD, w = WIN_W - 2 * PAD, y = 14;
    y += put_text(ID_TITLE, f_title, x, y, w);
    y += put_text(ID_SUB, f_small, x, y, w) + 10;
    int wy = y; y += 10;
    y += put_text(ID_WARN, f_body, x + 14, y, w - 24) + 10;
    SetRect(&r_warn, x, wy, x + w, y); y += 12;
    int ry = y; y += 10;
    put(ID_R128, f_btn, x + 14, y, 140, 32);
    put(ID_R256, f_btn, x + 14 + 160, y, w - 14 - 160 - 10, 32);
    y += 32 + 10; SetRect(&r_rate, x, ry, x + w, y); y += 14;
    int half = (w - 10) / 2;
    put(ID_LAUNCH, f_big, x, y, half, 52);
    put(ID_RESTORE, f_big, x + half + 10, y, w - half - 10, 52); y += 52 + 12;
    int sh = text_h(f_small, "X\nX\nX", w);
    put(ID_STATUS, f_small, x, y, w, sh); y += sh + 8;
    put(ID_FOLDER, f_body, x, y, 140, 28); y += 28 + 10;
    put(ID_LOG, f_mono, x, y, w, 92); y += 92 + 14;
    g_client_h = y;
}

static void pick_folder(void) {
    char file[MAX_PATH * 2] = "engine2.dll", init[MAX_PATH * 2];
    snprintf(init, sizeof init, "%s\\game\\bin\\win64", g_root[0] ? g_root : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive");
    OPENFILENAMEA o = {0};
    o.lStructSize = sizeof o; o.hwndOwner = g_wnd;
    o.lpstrFilter = "engine2.dll\0engine2.dll\0All files\0*.*\0";
    o.lpstrFile = file; o.nMaxFile = sizeof file; o.lpstrInitialDir = init;
    o.lpstrTitle = "Pick engine2.dll in ...\\Counter-Strike Global Offensive\\game\\bin\\win64";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&o)) return;
    if (set_root(file)) { write_text("folder.txt", g_root); scan(); out("CS2 folder: %s\n", g_root); update_ui(); }
    else MessageBoxA(g_wnd, "That isn't inside a CS2 install.\nPick ...\\Counter-Strike Global Offensive\\game\\bin\\win64\\engine2.dll", "Change folder", MB_ICONERROR);
}

static void do_patch_click(void) {
    if (!g_root[0]) { pick_folder(); if (!g_root[0]) return; }
    if (game_pid()) { out("Close CS2 first.\n"); return; }
    char tag[32], seen[8]; snprintf(tag, sizeof tag, "accepted_%d.txt", g_rate);
    if (!read_text(tag, seen, sizeof seen)) {
        char msg[1600];
        snprintf(msg, sizeof msg,
            "Patch CS2 to %d tick?\n\n"
            "- Only play on your own LAN / -insecure server. Do NOT join matchmaking, Premier or any VAC-secured "
            "server while CS2 is patched: modified game files can get your Steam account VAC banned.\n"
            "- Start CS2 (and your server) with -insecure, and patch the server to the same tick rate.\n"
            "- Click Restore 64 tick before playing online again. A Steam update also undoes the patch.\n%s\n"
            "This is experimental: some game timing may still behave like 64 tick.",
            g_rate, g_rate > 128 ?
            "\n256 tick: about 4x server CPU and network, your fps must stay above 256, more timing bugs are likely, "
            "and lag compensation covers less time. The gain over 128 is only about 3.9 ms per tick.\n" : "");
        if (MessageBoxA(g_wnd, msg, "CS2 128 Tick", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
        write_text(tag, "yes");
    }
    char rt[8]; snprintf(rt, sizeof rt, "%d", g_rate); write_text("rate.txt", rt);
    out("--- Patch to %d tick ---\n", g_rate);
    HCURSOR oc = SetCursor(LoadCursor(NULL, IDC_WAIT));
    if (patch_all(g_rate)) out("Done. Start CS2 with -insecure. Check it: join your server and run  cl_ticktiming print  "
                               "('FirstInput -> NetSend' a bit under %s ms).\n\n", g_rate > 128 ? "3.9" : "7.8");
    else restore_all();
    SetCursor(oc);
    update_ui();
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        HINSTANCE hi = ((CREATESTRUCT *)lp)->hInstance; g_wnd = h;
        #define MK(cls, txt, st, id) CreateWindowA(cls, txt, WS_CHILD | WS_VISIBLE | (st), 0, 0, 10, 10, h, (HMENU)(INT_PTR)(id), hi, NULL)
        MK("STATIC", "CS2 128 Tick", SS_NOPREFIX, ID_TITLE);
        MK("STATIC", "v3.1  \x95  Experimental  \x95  For your own LAN / -insecure server", SS_NOPREFIX, ID_SUB);
        MK("STATIC", "Close CS2, pick a tick rate and click Patch. Then start CS2 with -insecure in its Steam launch options. "
                     "While patched, never join matchmaking, Premier or VAC servers - modified game files can get your "
                     "account VAC banned. Click Restore 64 tick when you're done.", SS_NOPREFIX, ID_WARN);
        MK("BUTTON", "128 tick", BS_OWNERDRAW, ID_R128);
        MK("BUTTON", "256 tick  (experimental)", BS_OWNERDRAW, ID_R256);
        MK("BUTTON", "Patch", BS_OWNERDRAW, ID_LAUNCH);
        MK("STATIC", "", SS_NOPREFIX, ID_STATUS);
        MK("BUTTON", "Change folder...", BS_OWNERDRAW, ID_FOLDER);
        MK("BUTTON", "Restore 64 tick", BS_OWNERDRAW, ID_RESTORE);
        g_log = MK("EDIT", "", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, ID_LOG);
        layout();
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC dc = (HDC)wp; RECT r; GetClientRect(h, &r); FillRect(dc, &r, b_bg);
        FillRect(dc, &r_warn, b_warn);
        RECT a = r_warn; a.right = a.left + 3; HBRUSH am = CreateSolidBrush(C_AMBER); FillRect(dc, &a, am); DeleteObject(am);
        FillRect(dc, &r_rate, b_panel);
        RECT c = r_rate; c.right = c.left + 3; HBRUSH ac = CreateSolidBrush(g_rate > 128 ? C_AMBER : C_ORANGE); FillRect(dc, &c, ac); DeleteObject(ac);
        return 1;
    }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp; int id = GetDlgCtrlID((HWND)lp);
        SetBkMode(dc, TRANSPARENT);
        switch (id) {
        case ID_LOG: SetBkMode(dc, OPAQUE); SetBkColor(dc, C_BG); SetTextColor(dc, C_MUTED); return (LRESULT)b_bg;
        case ID_SUB: SetTextColor(dc, C_SOFT); return (LRESULT)b_bg;
        case ID_WARN: SetTextColor(dc, RGB(0xD9,0xD2,0xC5)); return (LRESULT)b_warn;
        case ID_STATUS: SetTextColor(dc, g_status_col == 2 ? C_RED : g_status_col == 1 ? C_PATCHED : C_GREEN); return (LRESULT)b_bg;
        default: SetTextColor(dc, C_TEXT); return (LRESULT)b_bg;
        }
    }
    case WM_DRAWITEM: draw_item((DRAWITEMSTRUCT *)lp); return TRUE;
    case WM_COMMAND:
        if (HIWORD(wp) != BN_CLICKED) return 0;
        switch (LOWORD(wp)) {
        case ID_R128: g_rate = 128; update_ui(); break;
        case ID_R256: g_rate = 256; update_ui(); break;
        case ID_LAUNCH: do_patch_click(); break;
        case ID_FOLDER: pick_folder(); break;
        case ID_RESTORE:
            if (game_pid()) { out("Close CS2 first.\n"); break; }
            out("--- Restore 64 tick ---\n");
            if (restore_all()) out("Back to the original 64-tick files - safe to play online again.\n\n");
            update_ui(); break;
        }
        return 0;
    case WM_CLOSE:
        if (any_patched() && MessageBoxA(h,
                "CS2 is still patched.\n\nThat's fine for your own -insecure server, but click Restore 64 tick before you play "
                "matchmaking, Premier or VAC servers.\n\nClose anyway?",
                "CS2 128 Tick", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return 0;
        DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

static HFONT font(int px, int weight, const char *face) {
    return CreateFontA(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}

static int run_window(void) {
    SetProcessDPIAware();
    HINSTANCE hi = GetModuleHandleA(NULL);
    f_title = font(26, FW_BOLD, "Segoe UI Semibold"); f_big = font(20, FW_BOLD, "Segoe UI");
    f_btn = font(15, FW_BOLD, "Segoe UI"); f_body = font(14, FW_NORMAL, "Segoe UI");
    f_small = font(13, FW_SEMIBOLD, "Segoe UI"); f_mono = font(13, FW_NORMAL, "Consolas");
    b_bg = CreateSolidBrush(C_BG); b_panel = CreateSolidBrush(C_PANEL); b_warn = CreateSolidBrush(C_WARNBG);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = hi; wc.lpszClassName = "CS2Tick128";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = b_bg;
    RegisterClassA(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    CreateWindowA("CS2Tick128", "CS2 128 Tick", style, CW_USEDEFAULT, CW_USEDEFAULT, 300, 200, NULL, NULL, hi, NULL);
    RECT r = { 0, 0, WIN_W, g_client_h }; AdjustWindowRect(&r, style, FALSE);
    RECT wa; SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = wa.left + (wa.right - wa.left - ww) / 2, y = wa.top + (wa.bottom - wa.top - wh) / 2;
    SetWindowPos(g_wnd, NULL, x, y < wa.top ? wa.top : y, ww, wh, SWP_NOZORDER);
    {   /* dark title bar on Windows 10/11 */
        typedef HRESULT (WINAPI *DSWA)(HWND, DWORD, LPCVOID, DWORD);
        HMODULE dw = LoadLibraryA("dwmapi.dll");
        DSWA f = dw ? (DSWA)GetProcAddress(dw, "DwmSetWindowAttribute") : NULL; BOOL on = TRUE;
        if (f) f(g_wnd, 20, &on, sizeof on);
    }
    scan();
    update_ui();
    ShowWindow(g_wnd, SW_SHOW);
    MSG msg; while (GetMessageA(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}

/* ============================================================ command line */

static int run_cli(const char *cmd) {
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) { freopen("CONOUT$", "w", stdout); freopen("CONOUT$", "w", stderr); }
    printf("\n");
    if (!g_root[0]) { printf("CS2 not found - pass the CS2 folder (or a SteamCMD server folder).\n"); return 1; }
    printf("CS2: %s\n", g_root);
    scan();
    for (int i = 0; i < NFILES; i++)
        printf("  %-12s %s\n", FILES[i].name, is_patched(i) ? (g_strate[i] == 256 ? "patched - 256 tick" : "patched - 128 tick") :
               g_st[i] == ST_ORIGINAL ? "original - 64 tick" : g_st[i] == ST_MISSING ? "not in this install" :
               backup_build(i) ? "patched by an older version" : "unknown CS2 build");
    if (!_stricmp(cmd, "status")) return 0;
    if (!_stricmp(cmd, "undo")) return restore_all() ? 0 : 1;
    printf("Patching to %d tick. Never connect to official Valve / VAC servers while patched; use -insecure.\n", g_rate);
    if (game_pid()) { printf("Close CS2 first.\n"); return 1; }
    return patch_all(g_rate) ? 0 : 1;
}

int main(int argc, char **argv) {
    (void)ST_NAME;
    const char *cmd = NULL; int folder_given = 0;
    for (int i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "status") || !_stricmp(argv[i], "patch") || !_stricmp(argv[i], "undo")) cmd = argv[i];
        else if (!strcmp(argv[i], "128") || !strcmp(argv[i], "256")) g_rate = atoi(argv[i]);
        else { folder_given = 1; set_root(argv[i]); }
    }
    if (cmd) {
        g_cli = 1;
        /* a folder that was given but isn't CS2 is an error - never fall back to some other install */
        if (!g_root[0] && !folder_given) find_cs2(g_root, sizeof g_root);
        return run_cli(cmd);
    }
    if (!g_root[0]) {
        char saved[MAX_PATH * 2];
        if (read_text("folder.txt", saved, sizeof saved) && valid_root(saved)) snprintf(g_root, sizeof g_root, "%s", saved);
        else find_cs2(g_root, sizeof g_root);
    }
    { char rt[8]; if (read_text("rate.txt", rt, sizeof rt) && (atoi(rt) == 128 || atoi(rt) == 256)) g_rate = atoi(rt); }
    return run_window();
}
