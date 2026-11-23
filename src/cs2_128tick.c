/*
 * CS2 128-Tick Patcher (EXPERIMENTAL) - Windows GUI, FACEIT-style theme
 * LAN / -insecure use only. Never on official Valve / VAC servers.
 *
 * CLI also works:  cs2_128tick.exe status|patch|aggressive|undo ["<CS2 folder>"]
 */
#include "core.inc"   /* shared helpers: file IO, SHA-256, patch state */
#include <commctrl.h>

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

enum { ID_WARNBODY = 401, ID_EX1 = 3, ID_EX2 = 4, ID_EX3 = 5, ID_TITLE = 6, ID_SUB = 7, ID_SAFE = 100, ID_AGGR, ID_UNDO, ID_BROWSE, ID_LOG, ID_PATH, ID_FINDFILE, ID_AUTO, ID_APPLY, ID_KIND, ID_R128, ID_R256, ID_RATEHEAD, ID_RATEDESC, ID_STAT0 = 200, ID_DESC0 = 300, ID_WARN = 400 };

static HWND g_wnd, g_log, g_path, g_kind, g_stat[NFILES];
static int g_state[NFILES], g_rate[NFILES];
static int g_sel_rate = 128;   /* tick rate chosen in the UI */
static char g_root[MAX_PATH * 2];
static HFONT f_title, f_sub, f_btn, f_body, f_bold, f_mono;
static HBRUSH b_bg, b_panel, b_warn;
static int g_cli = 0;

/* ---- output: console in CLI mode, log box in GUI mode ---- */
static void out(const char *fmt, ...) {
    char buf[1024], conv[2048]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (g_cli) { fputs(buf, stdout); return; }
    char *w = conv; for (char *r = buf; *r && w - conv < (int)sizeof conv - 2; r++) { if (*r == '\n') *w++ = '\r'; *w++ = *r; } *w = 0;
    int len = GetWindowTextLengthA(g_log);
    SendMessageA(g_log, EM_SETSEL, len, len);
    SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)conv);
}

static const char *RATE_HEAD[NRATES] = {
    "128 tick  -  recommended",
    "256 tick  -  untested, read the downsides first",
};
static const char *RATE_DESC[NRATES] = {
    "Twice Valve's 64. Confirmed working with cl_ticktiming (FirstInput -> NetSend about 6.7 ms). "
    "Costs about 2x the server CPU and network traffic of stock. Very long sessions on one map "
    "(roughly 9+ hours without a map change) may start to lose timing precision.",

    "\x95  Server CPU and network traffic about 4x stock. Every tick has to finish within 3.9 ms; your "
    "server used about 1.1 ms per tick at 128 on LAN, so there's headroom, but less with 10 players and lots of utility.\n"
    "\x95  Your fps must stay above 256 the whole time. Dips below make movement jitter, like the old 64 fps server cap did.\n"
    "\x95  Any 64-tick maths the patch missed is now 4x off instead of 2x, so bugs are more likely.\n"
    "\x95  Lag compensation probably keeps a fixed number of ticks of history, so it covers half the time it does at "
    "128: worse hit registration for high-ping players (fine on LAN).\n"
    "\x95  Timing precision runs out about twice as fast: change map or restart every few hours (roughly 4-5 h).\n"
    "\x95  The real gain over 128 is small: about 3.9 ms less per tick.",
};
static int rate_index(int rate) { for (int i = 0; i < NRATES; i++) if (RATES[i] == rate) return i; return 0; }

static int count_edits(const PatchFile *f, int maxtier) { int n = 0; for (int i = 0; i < f->n; i++) if (f->p[i].tier <= maxtier) n++; return n; }
static int total_edits(int maxtier) { int n = 0; for (int i = 0; i < NFILES; i++) n += count_edits(&FILES[i], maxtier); return n; }

static int backup_ok(int i);
static void refresh_status(void);
static int cs2_running(void) { return FindWindowA(NULL, "Counter-Strike 2") != NULL; }

static void refresh_status(void) {
    static int busy = 0;
    if (busy) return;
    busy = 1;
    for (int i = 0; i < NFILES; i++) {
        char p[MAX_PATH * 2]; snprintf(p, sizeof p, "%s\\%s", g_root, FILES[i].rel);
        DWORD sz = 0; unsigned char *b = g_root[0] ? read_file(p, &sz) : NULL;
        g_state[i] = get_state(b, sz, &FILES[i]); g_rate[i] = g_detected_rate; free(b);
        if (!g_cli) {
            char t[256];
            if (g_state[i] == ST_SAFE || g_state[i] == ST_AGGR)
                snprintf(t, sizeof t, "%-12s  patched - %d tick (%s)", FILES[i].name, g_rate[i], g_state[i] == ST_AGGR ? "aggressive" : "safe");
            else
                snprintf(t, sizeof t, "%-12s  %s", FILES[i].name, (g_state[i] == ST_UNKNOWN && backup_ok(i)) ? "patched by an older version - re-patch or undo" : g_state[i] == ST_MISSING ? "not in this install (skipped)" : ST_NAME[g_state[i]]);
            SetWindowTextA(g_stat[i], t);
        } else if (g_state[i] == ST_SAFE || g_state[i] == ST_AGGR) printf("  %-12s patched - %d tick (%s)\n", FILES[i].name, g_rate[i], g_state[i] == ST_AGGR ? "aggressive" : "safe");
        else printf("  %-12s %s\n", FILES[i].name, ST_NAME[g_state[i]]);
    }
    if (!g_cli) {
        char cur[MAX_PATH * 2]; GetWindowTextA(g_path, cur, sizeof cur);
        if (strcmp(cur, g_root)) SetWindowTextA(g_path, g_root);
        const char *k = !g_root[0] ? "CS2 folder  -  not found yet: use Find file, Browse folder or paste a path"
                      : g_state[2] == ST_MISSING ? "CS2 folder  -  dedicated server install (no client.dll), so engine2 + server get patched"
                      : "CS2 folder  -  game install (client + local/listen server)";
        SetWindowTextA(g_kind, k);
        InvalidateRect(g_wnd, NULL, TRUE);
    }
    busy = 0;
}

static int backup_ok(int i) {
    char bak[MAX_PATH * 2]; snprintf(bak, sizeof bak, "%s\\%s" BACKUP_EXT, g_root, FILES[i].rel);
    DWORD bs = 0; unsigned char *bb = read_file(bak, &bs); char h[65] = "";
    int ok = 0;
    if (bb) { sha256_hex(bb, bs, h); ok = !strcmp(h, FILES[i].sha); free(bb); }
    return ok;
}

static int do_patch(int aggressive) {
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
        int st = get_state(b, sz, &FILES[i]);
        if (st == ST_UNKNOWN) { free(b); b = read_file(bak, &sz); st = ST_ORIGINAL; out("%-12s older patch found - rebuilding from backup\n", FILES[i].name); }
        if (st == ST_SAFE || st == ST_AGGR) set_entries(b, &FILES[i], 1, 0, 0);
        if (GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES && !write_file(bak, b, sz)) {
            out("%s: couldn't write backup - run as administrator\n", FILES[i].name); free(b); ok = 0; break;
        }
        set_entries(b, &FILES[i], aggressive, 1, g_sel_rate);
        if (!write_file(p, b, sz)) { out("%s: write FAILED (in use? run as admin?)\n", FILES[i].name); ok = 0; }
        else out("%-12s patched to %d tick (%d edits), backup %s%s\n", FILES[i].name, g_sel_rate, count_edits(&FILES[i], aggressive), FILES[i].name, BACKUP_EXT);
        free(b);
    }
    if (ok) out("Done - %s mode, %d tick.\nLaunch CS2 AND your server with  -insecure\nDo NOT connect to official Valve servers / matchmaking until you click UNDO.\n\n", aggressive ? "aggressive" : "safe", g_sel_rate);
    return ok;
}

static int do_undo(void) {
    if (cs2_running()) { out("Close CS2 and your server first.\n"); return 0; }
    int ok = 1;
    for (int i = 0; i < NFILES; i++) {
        char p[MAX_PATH * 2], bak[MAX_PATH * 2];
        snprintf(p, sizeof p, "%s\\%s", g_root, FILES[i].rel); snprintf(bak, sizeof bak, "%s" BACKUP_EXT, p);
        DWORD sz = 0; unsigned char *b = read_file(p, &sz);
        int st = get_state(b, sz, &FILES[i]);
        if (st == ST_MISSING) { continue; }
        if (st == ST_ORIGINAL) { out("%-12s already original\n", FILES[i].name); DeleteFileA(bak); }
        else if (st == ST_SAFE || st == ST_AGGR) {
            set_entries(b, &FILES[i], 1, 0, 0);
            if (write_file(p, b, sz)) { out("%-12s restored to original 64 tick\n", FILES[i].name); DeleteFileA(bak); }
            else { out("%-12s restore FAILED (in use? run as admin?)\n", FILES[i].name); ok = 0; }
        } else {
            DWORD bs = 0; unsigned char *bb = read_file(bak, &bs); char h[65] = "";
            if (bb) sha256_hex(bb, bs, h);
            if (bb && !strcmp(h, FILES[i].sha) && write_file(p, bb, bs)) { out("%-12s restored from backup\n", FILES[i].name); DeleteFileA(bak); }
            else { out("%-12s %s - use Steam > CS2 > Properties > Installed Files > Verify integrity\n", FILES[i].name, ST_NAME[st]); ok = 0; }
            free(bb);
        }
        free(b);
    }
    if (ok) out("Undo complete. Files are back to stock - safe to play online again.\n\n");
    return ok;
}

static int confirm_patch(int aggressive) {
    char msg[3000];
    const char *extra = g_sel_rate > 128 ?
        "256 TICK DOWNSIDES (untested):\n"
        "- about 4x server CPU and network traffic vs stock\n"
        "- your fps must stay above 256 or movement will jitter\n"
        "- any 64-tick maths the patch missed is 4x off, so bugs are more likely\n"
        "- lag compensation likely covers less time: worse for high-ping players\n"
        "- change map / restart every few hours to keep timing precise\n"
        "- only about 3.9 ms gained per tick over 128\n\n" : "";
    snprintf(msg, sizeof msg,
        "Patch to %d tick (%s mode, %d edits)?\n\n"
        "%s"
        "BEFORE YOU CONTINUE\n"
        "- Do NOT connect to official Valve servers, matchmaking, Premier or any VAC-secured server while patched. "
        "Modified game files can get your Steam account VAC banned, and Valve does not reverse VAC bans.\n"
        "- Only use this on your own LAN / private server, and launch CS2 and the server with -insecure.\n"
        "- Patch your game AND your server to the SAME tick rate (run this on the server install too if it's separate).\n"
        "- Click Undo before you play online again. A Steam update also undoes the patch.\n\n"
        "This is experimental: some game timing may still behave like 64 tick.",
        g_sel_rate, aggressive ? "aggressive" : "safe", total_edits(aggressive), extra);
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
static void draw_button(DRAWITEMSTRUCT *d) {
    int pressed = d->itemState & ODS_SELECTED;
    int id = d->CtlID;
    int primary = id == ID_SAFE;
    if (id == ID_R128 || id == ID_R256) {
        int rate = id == ID_R128 ? 128 : 256, sel = rate == g_sel_rate;
        COLORREF f = sel ? (rate == 128 ? C_ORANGE : C_AMBER) : C_PANEL;
        COLORREF e = sel ? f : C_LINE;
        COLORREF t = sel ? (rate == 128 ? RGB(0xFF,0xFF,0xFF) : RGB(0x1E,0x1A,0x12)) : C_BODY;
        HBRUSH br = CreateSolidBrush(f); HPEN pen = CreatePen(PS_SOLID, 1, e);
        FillRect(d->hDC, &d->rcItem, b_panel);
        HGDIOBJ ob = SelectObject(d->hDC, br), op = SelectObject(d->hDC, pen);
        RoundRect(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right, d->rcItem.bottom, 10, 10);
        SelectObject(d->hDC, ob); SelectObject(d->hDC, op); DeleteObject(br); DeleteObject(pen);
        char tx[32]; GetWindowTextA(d->hwndItem, tx, sizeof tx);
        SetBkMode(d->hDC, TRANSPARENT); SetTextColor(d->hDC, t); SelectObject(d->hDC, f_btn);
        DrawTextA(d->hDC, tx, -1, &d->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    int small = id == ID_BROWSE || id == ID_FINDFILE || id == ID_AUTO || id == ID_APPLY;
    COLORREF fill = primary ? (pressed ? C_ORANGE2 : C_ORANGE) : (pressed ? RGB(0x2A,0x2A,0x2E) : C_PANEL);
    COLORREF edge = primary ? fill : id == ID_AGGR ? C_SOFT : C_LINE;
    COLORREF txt  = primary ? RGB(0xFF,0xFF,0xFF) : id == ID_AGGR ? C_SOFT : small ? C_BODY : C_TEXT;
    HBRUSH br = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, 1, edge);
    FillRect(d->hDC, &d->rcItem, b_panel);
    HGDIOBJ ob = SelectObject(d->hDC, br), op = SelectObject(d->hDC, pen);
    RoundRect(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right, d->rcItem.bottom, 10, 10);
    SelectObject(d->hDC, ob); SelectObject(d->hDC, op); DeleteObject(br); DeleteObject(pen);
    char t[64]; GetWindowTextA(d->hwndItem, t, sizeof t);
    SetBkMode(d->hDC, TRANSPARENT); SetTextColor(d->hDC, txt);
    SelectObject(d->hDC, small ? f_body : f_btn);
    DrawTextA(d->hDC, t, -1, &d->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void fill(HDC dc, RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }

#define W 860
#define PAD 24
static const char *DESC[3] = {
    "Recommended. Changes every use of the 1/64 tick interval, all time-to-tick conversions "
    "(including the clock used for movement, prediction and weapons), the dedicated server's "
    "frame-rate cap, and the tick interval sent to clients. 64 used as a size or distance is left "
    "alone. Your original files are backed up as *.orig64.",

    "Everything in Safe, plus a few remaining 64.0 values inside tick code that couldn't be "
    "confirmed as tick maths. Only try this if Safe still feels jittery or something runs at "
    "half or double speed - it has a slightly higher chance of affecting unrelated things.",

    "Puts the DLLs back to the original 64-tick files (checked byte-for-byte with SHA-256) and "
    "removes the backups. Works from either patch mode. Do this before playing online. "
    "If CS2 has updated since you patched, use Steam's \"Verify integrity of game files\" instead."
};

/* panel rectangles computed by layout(), painted in WM_ERASEBKGND */
static RECT r_warn, r_folder, r_rate, r_card[3];
static int g_clientH;

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
    int x = PAD, inner = W - 2 * PAD, y = 18;
    y += place_text(ID_TITLE, f_title, x, y, inner);
    y += place_text(ID_SUB, f_sub, x, y, inner) + 14;

    /* warning */
    int wy = y; y += 12;
    y += place_text(ID_WARN, f_bold, x + 18, y, inner - 30) + 2;
    y += place_text(ID_WARNBODY, f_body, x + 18, y, inner - 30) + 12;
    SetRect(&r_warn, x, wy, x + inner, y); y += 12;

    /* folder */
    int fy = y; y += 12;
    int fx = x + 14, fw = inner - 28;
    y += place_text(ID_KIND, f_sub, fx, y, fw) + 6;
    place(ID_PATH, f_body, fx, y, fw, 26); y += 26 + 10;
    int bx = fx;
    const int bid[4] = { ID_FINDFILE, ID_BROWSE, ID_AUTO, ID_APPLY }, bw[4] = { 120, 140, 120, 140 };
    for (int i = 0; i < 4; i++) { place(bid[i], f_body, bx, y, bw[i], 30); bx += bw[i] + 8; }
    y += 30 + 12;
    y += place_text(ID_EX1, f_sub, fx, y, fw);
    y += place_text(ID_EX2, f_sub, fx, y, fw);
    y += place_text(ID_EX3, f_sub, fx, y, fw) + 8;
    for (int i = 0; i < NFILES; i++) { place(ID_STAT0 + i, f_mono, fx, y, fw, 20); y += 20; }
    y += 10; SetRect(&r_folder, x, fy, x + inner, y); y += 12;

    /* tick rate: description area sized for the longest text so the window never jumps */
    int ry = y; y += 14;
    place(ID_R128, f_btn, x + 18, y, 110, 36);
    place(ID_R256, f_btn, x + 18 + 118, y, 110, 36);
    int tx = x + 18 + 236 + 20, tw = x + inner - 16 - tx;
    int hh = measure(f_bold, RATE_HEAD[1], tw), dh = 0;
    for (int i = 0; i < NRATES; i++) { int m2 = measure(f_body, RATE_DESC[i], tw); if (m2 > dh) dh = m2; }
    place(ID_RATEHEAD, f_bold, tx, y, tw, hh);
    place(ID_RATEDESC, f_body, tx, y + hh + 4, tw, dh);
    int rh = hh + 4 + dh; if (rh < 36) rh = 36;
    y += rh + 14; SetRect(&r_rate, x, ry, x + inner, y); y += 12;

    /* option cards */
    int bwid = 230, dx = x + 18 + bwid + 20, dw = x + inner - 16 - dx;
    for (int i = 0; i < 3; i++) {
        int cy = y;
        int dh = place_text(ID_DESC0 + i, f_body, dx, cy + 14, dw);
        place(ID_SAFE + i, f_btn, x + 18, cy + 14, bwid, 44);
        place_text(ID_DESC0 + 10 + i, f_sub, x + 18, cy + 64, bwid);
        int ch = dh + 28; if (ch < 100) ch = 100;
        SetRect(&r_card[i], x, cy, x + inner, cy + ch);
        y += ch + 10;
    }
    y += 2;
    place(ID_LOG, f_mono, x, y, inner, 104); y += 104 + 16;
    g_clientH = y;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        HINSTANCE hi = ((CREATESTRUCT *)lp)->hInstance;
        g_wnd = h;
        #define MK(cls, txt, st, id) CreateWindowA(cls, txt, WS_CHILD | WS_VISIBLE | (st), 0, 0, 10, 10, h, (HMENU)(INT_PTR)(id), hi, NULL)
        MK("STATIC", "CS2 128-Tick Patcher", SS_NOPREFIX, ID_TITLE);
        MK("STATIC", "v2.2  \x95  Experimental  \x95  For your own LAN / -insecure server", SS_NOPREFIX, ID_SUB);
        MK("STATIC", "Only use this on your own server", SS_NOPREFIX, ID_WARN);
        MK("STATIC", "Don't join official Valve servers (matchmaking, Premier or VAC community servers) while the "
                     "files are patched - modified game files can get your account VAC banned. Launch CS2 and your "
                     "server with -insecure, and click Undo before you play online again.", SS_NOPREFIX, ID_WARNBODY);
        g_kind = MK("STATIC", "CS2 folder", SS_NOPREFIX, ID_KIND);
        g_path = CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_BORDER, 0, 0, 10, 10, h, (HMENU)(INT_PTR)ID_PATH, hi, NULL);
        SendMessageW(g_path, EM_SETCUEBANNER, TRUE, (LPARAM)L"Paste a path here, or use the buttons below");
        MK("BUTTON", "Find file...",     BS_OWNERDRAW, ID_FINDFILE);
        MK("BUTTON", "Browse folder...", BS_OWNERDRAW, ID_BROWSE);
        MK("BUTTON", "Auto-detect",      BS_OWNERDRAW, ID_AUTO);
        MK("BUTTON", "Use typed path",   BS_OWNERDRAW, ID_APPLY);
        MK("STATIC", "Example game folder:   C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive", SS_NOPREFIX, ID_EX1);
        MK("STATIC", "Example file:              ...\\Counter-Strike Global Offensive\\game\\bin\\win64\\engine2.dll", SS_NOPREFIX, ID_EX2);
        MK("STATIC", "Example server:          C:\\cs2-server   (wherever you installed it with SteamCMD)", SS_NOPREFIX, ID_EX3);
        for (int i = 0; i < NFILES; i++) g_stat[i] = MK("STATIC", "", SS_NOPREFIX, ID_STAT0 + i);
        MK("BUTTON", "128 tick", BS_OWNERDRAW, ID_R128);
        MK("BUTTON", "256 tick", BS_OWNERDRAW, ID_R256);
        MK("STATIC", RATE_HEAD[0], SS_NOPREFIX, ID_RATEHEAD);
        MK("STATIC", RATE_DESC[0], SS_NOPREFIX, ID_RATEDESC);
        const char *lbl[3] = { "Patch  -  Safe", "Patch  -  Aggressive", "Undo  -  restore 64 tick" };
        for (int i = 0; i < 3; i++) {
            MK("BUTTON", lbl[i], BS_OWNERDRAW, ID_SAFE + i);
            char cnt[64];
            if (i < 2) snprintf(cnt, sizeof cnt, "%d edits%s", total_edits(i), i == 0 ? "  \x95  recommended" : "");
            else snprintf(cnt, sizeof cnt, "back to stock files");
            MK("STATIC", cnt, SS_CENTER | SS_NOPREFIX, ID_DESC0 + 10 + i);
            MK("STATIC", DESC[i], SS_NOPREFIX, ID_DESC0 + i);
        }
        g_log = MK("EDIT", "", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, ID_LOG);
        layout();
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC dc = (HDC)wp; RECT r; GetClientRect(h, &r); FillRect(dc, &r, b_bg);
        fill(dc, r_warn, C_WARNBG);
        RECT a = r_warn; a.right = a.left + 3; fill(dc, a, C_AMBER);
        fill(dc, r_folder, C_PANEL);
        fill(dc, r_rate, C_PANEL);
        { RECT c = r_rate; c.right = c.left + 3; fill(dc, c, g_sel_rate > 128 ? C_AMBER : C_ORANGE); }
        const COLORREF acc[3] = { C_ORANGE, C_SOFT, C_LINE };
        for (int i = 0; i < 3; i++) { fill(dc, r_card[i], C_PANEL); RECT c = r_card[i]; c.right = c.left + 3; fill(dc, c, acc[i]); }
        return 1;
    }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp; int id = GetDlgCtrlID((HWND)lp);
        SetBkMode(dc, TRANSPARENT);
        if (id == ID_PATH) { SetBkColor(dc, C_BG); SetTextColor(dc, C_TEXT); SetBkMode(dc, OPAQUE); return (LRESULT)b_bg; }
        if (id == ID_LOG)  { SetBkColor(dc, C_BG); SetTextColor(dc, C_MUTED); SetBkMode(dc, OPAQUE); return (LRESULT)b_bg; }
        if (id == ID_TITLE) { SetTextColor(dc, C_TEXT); return (LRESULT)b_bg; }
        if (id == ID_SUB)   { SetTextColor(dc, C_SOFT); return (LRESULT)b_bg; }
        if (id == ID_WARN)  { SetTextColor(dc, C_AMBER); return (LRESULT)b_warn; }
        if (id == ID_WARNBODY) { SetTextColor(dc, C_WARNTXT); return (LRESULT)b_warn; }
        if (id == ID_RATEHEAD) { SetTextColor(dc, g_sel_rate > 128 ? C_AMBER : C_SOFT); return (LRESULT)b_panel; }
        if (id == ID_RATEDESC) { SetTextColor(dc, C_BODY); return (LRESULT)b_panel; }
        if (id == ID_KIND)  { SetTextColor(dc, C_SOFT); return (LRESULT)b_panel; }
        if (id == ID_EX1 || id == ID_EX2 || id == ID_EX3) { SetTextColor(dc, C_MUTED); return (LRESULT)b_panel; }
        if (id >= ID_STAT0 && id < ID_STAT0 + NFILES) {
            int st = g_state[id - ID_STAT0];
            SetTextColor(dc, st == ST_ORIGINAL ? C_GREEN : (st == ST_SAFE || st == ST_AGGR) ? C_PATCHED : st == ST_MISSING ? C_MUTED : C_RED);
            return (LRESULT)b_panel;
        }
        if (id >= ID_DESC0 + 10) { SetTextColor(dc, C_MUTED); return (LRESULT)b_panel; }
        if (id >= ID_DESC0 && id < ID_DESC0 + 3) { SetTextColor(dc, C_BODY); return (LRESULT)b_panel; }
        SetTextColor(dc, C_TEXT); return (LRESULT)b_panel;
    }
    case WM_DRAWITEM: draw_button((DRAWITEMSTRUCT *)lp); return TRUE;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_PATH) return 0;                       /* ignore all edit-box notifications */
        if (HIWORD(wp) != BN_CLICKED) return 0;            /* only react to real button clicks */
        if (id == ID_BROWSE) { browse(); return 0; }
        if (id == ID_FINDFILE) { find_file(); return 0; }
        if (id == ID_R128 || id == ID_R256) {
            g_sel_rate = id == ID_R128 ? 128 : 256;
            int ri = rate_index(g_sel_rate);
            SetWindowTextA(GetDlgItem(h, ID_RATEHEAD), RATE_HEAD[ri]);
            SetWindowTextA(GetDlgItem(h, ID_RATEDESC), RATE_DESC[ri]);
            InvalidateRect(h, NULL, TRUE);
            return 0;
        }
        if (id == ID_AUTO) {
            char r[MAX_PATH * 2];
            if (find_cs2(r, sizeof r)) { snprintf(g_root, sizeof g_root, "%s", r); refresh_status(); out("Auto-detected: %s\n", g_root); }
            else MessageBoxA(h, "Couldn't find CS2 through Steam.\nUse Find file... and pick engine2.dll, e.g.\n\n...\\Counter-Strike Global Offensive\\game\\bin\\win64\\engine2.dll", "Auto-detect", MB_ICONINFORMATION);
            return 0;
        }
        if (id == ID_APPLY) {
            char typed[MAX_PATH * 2]; GetWindowTextA(g_path, typed, sizeof typed);
            if (set_root_from(typed)) { refresh_status(); out("CS2 folder set: %s\n", g_root); } else not_found_box(typed);
            return 0;
        }
        if ((id == ID_SAFE || id == ID_AGGR || id == ID_UNDO) && !g_root[0]) { find_file(); if (!g_root[0]) return 0; }
        HCURSOR oc = SetCursor(LoadCursor(NULL, IDC_WAIT));
        if (id == ID_SAFE && confirm_patch(0)) { out("--- Patch (safe) ---\n"); do_patch(0); }
        else if (id == ID_AGGR && confirm_patch(1)) { out("--- Patch (aggressive) ---\n"); do_patch(1); }
        else if (id == ID_UNDO) { out("--- Undo ---\n"); do_undo(); }
        SetCursor(oc); refresh_status();
        return 0;
    }
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
        if (!_stricmp(argv[i], "status") || !_stricmp(argv[i], "patch") || !_stricmp(argv[i], "aggressive") || !_stricmp(argv[i], "undo")) cmd = argv[i];
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
        if (g_sel_rate > 128) printf("256 tick is untested: ~4x CPU/network, needs 256+ fps, more bugs likely, change map every few hours.\n");
        return do_patch(!_stricmp(cmd, "aggressive")) ? 0 : 1;
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
    g_wnd = CreateWindowA("CS2Tick128", "CS2 128-Tick Patcher", style, CW_USEDEFAULT, CW_USEDEFAULT, 400, 300, NULL, NULL, hi, NULL);
    {   /* size the window to fit the measured layout */
        RECT r = { 0, 0, W, g_clientH }; AdjustWindowRect(&r, style, FALSE);
        RECT wa; SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        int ww = r.right - r.left, wh = r.bottom - r.top;
        int x = wa.left + ((wa.right - wa.left) - ww) / 2, y = wa.top + ((wa.bottom - wa.top) - wh) / 2;
        if (y < wa.top) y = wa.top;
        SetWindowPos(g_wnd, NULL, x, y, ww, wh, SWP_NOZORDER);
    }
    /* dark title bar on Windows 10/11 */
    { typedef HRESULT (WINAPI *DSWA)(HWND, DWORD, LPCVOID, DWORD);
      HMODULE dw = LoadLibraryA("dwmapi.dll");
      if (dw) { DSWA f = (DSWA)GetProcAddress(dw, "DwmSetWindowAttribute"); BOOL on = TRUE; if (f) f(g_wnd, 20, &on, sizeof on); } }
    refresh_status();
    if (g_root[0]) out("Found CS2: %s\n", g_root); else out("CS2 not found automatically - use Find file... (pick engine2.dll) or paste the folder path.\n");
    out("Build-locked: patches only apply to the exact files they were made from (SHA-256 checked).\n\n");
    ShowWindow(g_wnd, SW_SHOW);
    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}
