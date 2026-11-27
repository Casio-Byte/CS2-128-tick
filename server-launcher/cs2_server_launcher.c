/*
 * CS2 Server Launcher - dedicated server with 64/128/256 tick, MatchZy, Retakes + InstaDefuse.
 * Same patch engine as the game launcher (../engine).
 *
 * - Install / Update: SteamCMD app 730 (restores 64 tick first; patch is re-applied on Start)
 * - Install plugins: Metamod, CounterStrikeSharp (latest), MatchZy 0.9.3, Retakes 3.1.2, InstaDefuse 2.0.0
 * - Modes: competitive / wingman / practice -> MatchZy only; retakes -> Retakes + InstaDefuse only
 * - Start: patch to the chosen rate, -insecure, +sv_fps_max <rate>, "[128 tick]" in the server name,
 *          restart after crashes. "Run without plugins" switches Metamod off in gameinfo.gi.
 *
 * LAN / -insecure use only.
 * Build: windres server.rc -O coff -o server_res.o
 *        x86_64-w64-mingw32-gcc -O2 -municode -mwindows -static cs2_server_launcher.c server_res.o \
 *          -o cs2_server_launcher.exe -lshlwapi -lole32 -lshell32 -lcomctl32
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <urlmon.h>
#include <winsock2.h>
#include <tlhelp32.h>
#include <time.h>
#include <iphlpapi.h>
#include <wininet.h>
#include <stdio.h>
#include <wchar.h>
#include "../engine/tp_files.h"

#define APP_TITLE L"CS2 Server Launcher"
#define APP_VER   L"2.6"
#define WM_APP_LOG  (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)
#define WM_APP_STATE (WM_APP + 3)
#define WM_APP_PROG  (WM_APP + 4)
#define WM_APP_CON   (WM_APP + 5)
#define WM_APP_PLAYERS (WM_APP + 6)
#define WM_APP_RESOLVED (WM_APP + 7)
#define TIMER_POLL 1
#define WM_APP_EXTIP (WM_APP + 8)
#define WM_APP_EMBED (WM_APP + 9)
#define WM_APP_INFO (WM_APP + 10)
#define WM_APP_ADMINRES (WM_APP + 11)
#define IDR_PS1 101

enum {
    ID_ROOT = 200, ID_BROWSE, ID_SCMD, ID_BROWSE2, ID_NAME, ID_PASS, ID_RCON, ID_PORT, ID_MAP, ID_MAXPL, ID_MODE, ID_TICK,
    ID_LAN, ID_NET, ID_GSLT, ID_NOPLUG, ID_INSTALL, ID_PLUGINS, ID_CHECK, ID_FIREWALL, ID_START, ID_STOP,
    ID_RESTORE, ID_LOG, ID_PAUSE, ID_CANCEL, ID_PROG, ID_PSTAT, ID_CMD, ID_SEND, ID_DEBUG, ID_OPENLOGS,
    ID_TAB, ID_CONSOLE, ID_PLIST, ID_PREFRESH, ID_PKICK, ID_PBAN, ID_PBANS, ID_PPROFILE, ID_PTEAM1, ID_PTEAM2, ID_PSTAT2,
    ID_T1NAME, ID_T1, ID_T2NAME, ID_T2, ID_PPT, ID_SERIES, ID_SIDES, ID_MAPS, ID_EXTRAMAPS, ID_RESOLVE, ID_LOADMATCH, ID_ENDMATCH, ID_MSTAT, ID_CONCLEAR,
    ID_CLOCAL, ID_CEXT, ID_CPORT, ID_CPASS, ID_CSHOWPW, ID_CCMDL, ID_CCMDE, ID_CLINKL, ID_CLINKE, ID_CCOPY1, ID_CCOPY2, ID_CCOPY3, ID_CCOPY4, ID_CEXTREF, ID_CNOTE, ID_CRCON, ID_CCOPYR, ID_CONHOST, ID_EMBED, ID_INFO, ID_INFOCHECK, ID_MAPMODE, ID_MS1, ID_MS2, ID_MS3, ID_MS4, ID_MS5, ID_CHMAP, ID_CHMAPBTN, ID_PLUGCHECK,
    ID_APROF, ID_ANAME, ID_AFLAGS, ID_AADD, ID_AREMOVE, ID_ALIST, ID_ASTAT, ID_AMZ, ID_ARELOAD, ID_PADMIN
};

static HWND g_wnd, g_log;
static HWND g_root, g_scmd, g_name, g_pass, g_rcon, g_port, g_map, g_maxpl, g_mode, g_tick, g_lan, g_net, g_gslt, g_noplug, g_debug;
static HWND g_btn[7];   /* install, plugins, check, firewall, start, stop, restore */
static HWND g_prog, g_pstat, g_pause, g_cancel, g_cmd, g_send;
static HANDLE g_srvIn = NULL;               /* pseudo console input of the running server */
static HANDLE g_job = NULL;                 /* job object of the running helper / SteamCMD */
static CRITICAL_SECTION g_jobLock;
static volatile LONG g_paused = 0, g_cancelReq = 0;

/* progress shown under the buttons */
typedef struct { int pct10; WCHAR text[256]; } Prog;   /* pct10: 0..1000, -1 = marquee/idle */
static void SetProgress(int pct10, const WCHAR *fmt, ...)
{
    Prog *p = (Prog *)malloc(sizeof *p); if (!p) return;
    p->pct10 = pct10;
    va_list ap; va_start(ap, fmt); _vsnwprintf(p->text, 255, fmt, ap); p->text[255] = 0; va_end(ap);
    if (!PostMessageW(g_wnd, WM_APP_PROG, 0, (LPARAM)p)) free(p);
}

/* ---- SteamCMD output parsing ----
 *  Update state (0x61) downloading, progress: 7.12 (2547896012 / 35776491327)
 *  Update state (0x81) verifying update, progress: 51.40 (...)
 *  [ 45%] Downloading update (12,345 of 40,000 KB)...      (SteamCMD self-update) */
static DWORD g_pLastTick; static unsigned long long g_pLastBytes; static double g_pSpeed; static int g_pLastLogged;
static void FmtBytes(WCHAR *o, int n, unsigned long long b)
{
    if (b >= 1000000000ULL) _snwprintf(o, n, L"%.2f GB", b / 1e9);
    else _snwprintf(o, n, L"%.0f MB", b / 1e6);
}
/* returns TRUE if the line was a progress line (then it is not logged every time) */
static BOOL ParseSteamcmd(const WCHAR *l)
{
    const WCHAR *us = wcsstr(l, L"Update state (");
    const WCHAR *pg = wcsstr(l, L"progress: ");
    if (us && pg) {
        const WCHAR *ph = wcschr(us, L')'); WCHAR phase[64] = L"";
        if (ph) { ph += 2; int k = 0; while (ph < pg && *ph && *ph != L',' && k < 63) phase[k++] = *ph++; phase[k] = 0; }
        double pct = _wtof(pg + 10);
        unsigned long long done = 0, total = 0;
        const WCHAR *br = wcschr(pg, L'(');
        if (br) { done = _wcstoui64(br + 1, NULL, 10); const WCHAR *sl = wcschr(br, L'/'); if (sl) total = _wcstoui64(sl + 1, NULL, 10); }
        DWORD now = GetTickCount();
        if (g_pLastTick && done >= g_pLastBytes && now > g_pLastTick && !wcsstr(phase, L"verif")) {
            double inst = (double)(done - g_pLastBytes) * 1000.0 / (now - g_pLastTick);
            g_pSpeed = g_pSpeed > 0 ? g_pSpeed * 0.7 + inst * 0.3 : inst;
        }
        g_pLastTick = now; g_pLastBytes = done;
        WCHAR d[32], t[32], sp[48] = L"", eta[48] = L"";
        FmtBytes(d, 32, done); FmtBytes(t, 32, total);
        if (g_pSpeed > 1 && !wcsstr(phase, L"verif")) {
            _snwprintf(sp, 48, L"  %.1f MB/s", g_pSpeed / 1e6);
            if (total > done) { double s = (total - done) / g_pSpeed; _snwprintf(eta, 48, s > 3600 ? L"  ~%.1f h left" : L"  ~%.0f min left", s > 3600 ? s / 3600 : (s < 60 ? 1 : s / 60)); }
        }
        SetProgress((int)(pct * 10), L"SteamCMD %s: %.1f%%  (%s / %s)%s%s", phase, pct, d, t, sp, eta);
        int bucket = (int)pct / 10;
        if (bucket != g_pLastLogged) { g_pLastLogged = bucket; return FALSE; }   /* log every 10% */
        return TRUE;
    }
    const WCHAR *b = wcsstr(l, L"[");
    if (b && wcsstr(l, L"%]")) {
        int pct = _wtoi(b + 1);
        if (wcsstr(l, L"Downloading") || wcsstr(l, L"Extracting") || wcsstr(l, L"Verifying")) {
            SetProgress(pct * 10, L"SteamCMD self-update: %s", l + (b - l));
            return pct != 0 && pct != 100;
        }
    }
    if (wcsstr(l, L"Success! App '730'")) SetProgress(1000, L"SteamCMD: CS2 is up to date.");
    else if (wcsstr(l, L"Logging in")) SetProgress(-1, L"SteamCMD: logging in (anonymous)...");
    else if (wcsstr(l, L"Checking for available update")) SetProgress(-1, L"SteamCMD: checking for its own update...");
    return FALSE;
}

typedef BOOL (*LineFilter)(const WCHAR *);

/* Suspend / resume / kill every process in the current job */
typedef LONG (NTAPI *NtProc_t)(HANDLE);
static void JobForEach(int what)   /* 0 suspend, 1 resume */
{
    static NtProc_t sus, res;
    if (!sus) { HMODULE nt = GetModuleHandleW(L"ntdll.dll"); sus = (NtProc_t)(void *)GetProcAddress(nt, "NtSuspendProcess"); res = (NtProc_t)(void *)GetProcAddress(nt, "NtResumeProcess"); }
    EnterCriticalSection(&g_jobLock);
    if (g_job && sus && res) {
        BYTE buf[sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 256 * sizeof(ULONG_PTR)];
        JOBOBJECT_BASIC_PROCESS_ID_LIST *l = (JOBOBJECT_BASIC_PROCESS_ID_LIST *)buf;
        if (QueryInformationJobObject(g_job, JobObjectBasicProcessIdList, l, sizeof buf, NULL)) {
            for (DWORD i = 0; i < l->NumberOfProcessIdsInList; i++) {
                HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, (DWORD)l->ProcessIdList[i]);
                if (h) { (what == 0 ? sus : res)(h); CloseHandle(h); }
            }
        }
    }
    LeaveCriticalSection(&g_jobLock);
}
static WCHAR g_dataDir[MAX_PATH], g_ini[MAX_PATH], g_logFile[MAX_PATH], g_ps1[MAX_PATH];
static volatile LONG g_busy = 0, g_running = 0, g_stopReq = 0;
static HANDLE g_proc = NULL;

static const WCHAR *MODES[4]   = { L"competitive", L"wingman", L"practice", L"retakes" };
static const WCHAR *MODEUI[4]  = { L"Competitive (MatchZy)", L"Wingman (MatchZy)", L"Practice (MatchZy)", L"Retakes + InstaDefuse" };
static const int    TICKS[3]   = { 64, 128, 256 };

/* ---------------------------------------------------------------- log */
static WCHAR g_sessLog[MAX_PATH] = L"", g_logDir[MAX_PATH];
static WCHAR g_autoRcon[40] = L"";
static void Log(const WCHAR *fmt, ...)
{
    WCHAR body[4096], line[4200];
    va_list ap; va_start(ap, fmt); _vsnwprintf(body, 4095, fmt, ap); body[4095] = 0; va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    _snwprintf(line, 4199, L"[%02d:%02d:%02d] %s\r\n", st.wHour, st.wMinute, st.wSecond, body); line[4199] = 0;
    {
        char u8[13000]; int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, u8, sizeof u8, NULL, NULL);
        const WCHAR *files[2] = { g_logFile, g_sessLog };
        for (int i = 0; i < 2; i++) {
            if (!files[i][0]) continue;
            HANDLE f = CreateFileW(files[i], FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
            if (f != INVALID_HANDLE_VALUE) { DWORD w; if (n > 1) WriteFile(f, u8, n - 1, &w, NULL); CloseHandle(f); }
        }
    }
    WCHAR *c = _wcsdup(line);
    if (c && !PostMessageW(g_wnd, WM_APP_LOG, 0, (LPARAM)c)) free(c);
}

/* ------------------------------------------------------------ settings */
typedef struct {
    WCHAR root[MAX_PATH], scmd[MAX_PATH], name[128], pass[128], rcon[128], map[64], gslt[128];
    int port, maxpl, mode, tick, lan, noplug, debug;
} Cfg;

static void LogConnInfo(const Cfg *c);
static void GetText(HWND h, WCHAR *o, int n) { GetWindowTextW(h, o, n); }
static void ReadUi(Cfg *c)
{
    GetText(g_root, c->root, MAX_PATH); GetText(g_scmd, c->scmd, MAX_PATH); GetText(g_name, c->name, 128); GetText(g_pass, c->pass, 128);
    GetText(g_rcon, c->rcon, 128); GetText(g_map, c->map, 64); GetText(g_gslt, c->gslt, 128);
    WCHAR b[32];
    GetText(g_port, b, 32); c->port = _wtoi(b); if (c->port <= 0) c->port = 27015;
    GetText(g_maxpl, b, 32); c->maxpl = _wtoi(b); if (c->maxpl <= 0) c->maxpl = 10;
    c->mode = (int)SendMessageW(g_mode, CB_GETCURSEL, 0, 0); if (c->mode < 0) c->mode = 0;
    int t = (int)SendMessageW(g_tick, CB_GETCURSEL, 0, 0); c->tick = TICKS[t < 0 ? 1 : t];
    c->lan = SendMessageW(g_lan, BM_GETCHECK, 0, 0) == BST_CHECKED;
    c->noplug = SendMessageW(g_noplug, BM_GETCHECK, 0, 0) == BST_CHECKED;
    c->debug = SendMessageW(g_debug, BM_GETCHECK, 0, 0) == BST_CHECKED;
    /* strip trailing backslash */
    size_t L = wcslen(c->root); while (L > 3 && c->root[L - 1] == L'\\') c->root[--L] = 0;
    L = wcslen(c->scmd); while (L > 3 && c->scmd[L - 1] == L'\\') c->scmd[--L] = 0;
    /* allow picking steamcmd.exe itself */
    if (L > 13 && !_wcsicmp(c->scmd + L - 13, L"\\steamcmd.exe")) c->scmd[L - 13] = 0;
    if (!c->scmd[0]) _snwprintf(c->scmd, MAX_PATH, L"%s\\steamcmd", c->root);
}
static void SaveCfg(const Cfg *c)
{
    WCHAR b[32];
#define WS(k, v) WritePrivateProfileStringW(L"server", k, v, g_ini)
#define WI(k, v) (_snwprintf(b, 32, L"%d", v), WS(k, b))
    WS(L"root", c->root); WS(L"steamcmd", c->scmd); WS(L"name", c->name); WS(L"password", c->pass); WS(L"rcon", c->rcon);
    WS(L"map", c->map); WS(L"gslt", c->gslt);
    WI(L"port", c->port); WI(L"maxplayers", c->maxpl); WI(L"mode", c->mode); WI(L"tick", c->tick);
    WI(L"lan", c->lan); WI(L"noplugins", c->noplug); WI(L"debug", c->debug);
}
static void LoadCfgToUi(void)
{
    WCHAR b[MAX_PATH];
#define RS(k, d, h) (GetPrivateProfileStringW(L"server", k, d, b, MAX_PATH, g_ini), SetWindowTextW(h, b))
    RS(L"root", L"C:\\CS2Server", g_root); RS(L"steamcmd", L"C:\\CS2Server\\steamcmd", g_scmd); RS(L"name", L"My CS2 Server", g_name); RS(L"password", L"", g_pass);
    RS(L"rcon", L"", g_rcon); RS(L"map", L"de_dust2", g_map); RS(L"gslt", L"", g_gslt);
    RS(L"port", L"27015", g_port); RS(L"maxplayers", L"10", g_maxpl);
    int mode = GetPrivateProfileIntW(L"server", L"mode", 0, g_ini); if (mode < 0 || mode > 3) mode = 0;
    SendMessageW(g_mode, CB_SETCURSEL, mode, 0);
    int tick = GetPrivateProfileIntW(L"server", L"tick", 128, g_ini);
    SendMessageW(g_tick, CB_SETCURSEL, tick == 64 ? 0 : tick == 256 ? 2 : 1, 0);
    int lan = GetPrivateProfileIntW(L"server", L"lan", 1, g_ini);
    SendMessageW(lan ? g_lan : g_net, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(g_noplug, BM_SETCHECK, GetPrivateProfileIntW(L"server", L"noplugins", 0, g_ini) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_debug, BM_SETCHECK, GetPrivateProfileIntW(L"server", L"debug", 0, g_ini) ? BST_CHECKED : BST_UNCHECKED, 0);
}

/* ----------------------------------------------------------- processes */
/* Runs a command, streams stdout/stderr into the log. Returns exit code (-1 = could not start). */
static int RunCaptureF(const WCHAR *exe, const WCHAR *args, const WCHAR *dir, const WCHAR *prefix, LineFilter filt)
{
    if (g_cancelReq) return -2;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    WCHAR cmd[8192]; _snwprintf(cmd, 8191, L"\"%s\" %s", exe, args); cmd[8191] = 0;
    STARTUPINFOW si = { 0 }; si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, dir, &si, &pi)) {
        DWORD e = GetLastError(); CloseHandle(rd); CloseHandle(wr);
        Log(L"ERROR: cannot run %s (%lu)", exe, e); return -1;
    }
    /* job object: Pause/Cancel reach SteamCMD's child processes too */
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = { 0 };
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
        AssignProcessToJobObject(job, pi.hProcess);
        EnterCriticalSection(&g_jobLock); g_job = job; LeaveCriticalSection(&g_jobLock);
    }
    ResumeThread(pi.hThread);
    if (g_paused) JobForEach(0);
    CloseHandle(wr);
    char buf[4096], line[8192]; int ln = 0; DWORD n;
    while (ReadFile(rd, buf, sizeof buf, &n, NULL) && n) {
        for (DWORD i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\n' || ch == '\r' || ln >= (int)sizeof line - 1) {
                if (ln) {
                    line[ln] = 0; WCHAR w[8192];
                    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line, -1, w, 8192))
                        MultiByteToWideChar(CP_OEMCP, 0, line, -1, w, 8192);
                    if (!filt || !filt(w)) Log(L"%s%s", prefix, w);
                }
                ln = 0;
            } else line[ln++] = ch;
        }
    }
    if (ln) { line[ln] = 0; WCHAR w[8192]; MultiByteToWideChar(CP_UTF8, 0, line, -1, w, 8192); Log(L"%s%s", prefix, w); }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 0; GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    EnterCriticalSection(&g_jobLock); g_job = NULL; LeaveCriticalSection(&g_jobLock);
    if (job) CloseHandle(job);              /* kills anything left over */
    if (g_cancelReq) return -2;
    return (int)ec;
}
static int RunCapture(const WCHAR *exe, const WCHAR *args, const WCHAR *dir, const WCHAR *prefix)
{ return RunCaptureF(exe, args, dir, prefix, NULL); }

static int RunPs(const WCHAR *action, const WCHAR *root, const WCHAR *arg)
{
    WCHAR ps[MAX_PATH]; GetSystemDirectoryW(ps, MAX_PATH); PathAppendW(ps, L"WindowsPowerShell\\v1.0\\powershell.exe");
    WCHAR args[2048];
    _snwprintf(args, 2047, L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%s\" %s \"%s\" %s",
               g_ps1, action, root ? root : L"", arg ? arg : L""); args[2047] = 0;
    return RunCapture(ps, args, NULL, L"  ");
}

static BOOL HasServer(const WCHAR *root)
{
    WCHAR p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%s\\game\\bin\\win64\\cs2.exe", root); p[MAX_PATH-1] = 0;
    return tp_exists(p) && tp_is_cs2(root);
}
static BOOL HasCss(const WCHAR *root)
{
    WCHAR p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%s\\game\\csgo\\addons\\counterstrikesharp", root); p[MAX_PATH-1] = 0;
    DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ------------------------------------------------------------- mode cfgs */
static void WriteModeCfg(const Cfg *c)
{
    WCHAR p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%s\\game\\csgo\\cfg\\cs2launcher_%s.cfg", c->root, MODES[c->mode]);
    const char *body;
    switch (c->mode) {
    case 0: body = "// CS2 Server Launcher - competitive (MatchZy)\nmatchzy_autostart_mode 1\nmp_autokick 0\nsv_allow_votes 0\n"; break;
    case 1: body = "// CS2 Server Launcher - wingman (MatchZy)\nmatchzy_autostart_mode 1\nmp_autokick 0\nsv_allow_votes 0\n"; break;
    case 2: body = "// CS2 Server Launcher - practice (MatchZy practice mode; players can also type .prac)\nmatchzy_autostart_mode 2\nmp_autokick 0\n"; break;
    default: body = "// CS2 Server Launcher - retakes (RetakesPlugin + InstaDefuse; bomb is auto-planted by Retakes)\n"
                    "mp_autokick 0\nsv_allow_votes 0\nbot_quota 0\nbot_kick\n"; break;
    }
    FILE *f = _wfopen(p, L"wb"); if (f) { fputs(body, f); fclose(f); }
}

/* ------------------------------------------------------------- jobs */
typedef enum { J_INSTALL, J_PLUGINS, J_CHECK, J_FIREWALL, J_START, J_RESTORE } JobKind;
typedef struct { JobKind k; Cfg c; } Job;

/* Makes sure steamcmd.exe exists in the SteamCMD folder; fills sc/dir. */
static BOOL GetSteamCmd(const Cfg *c, WCHAR *sc, WCHAR *dir)
{
    wcscpy(dir, c->scmd);
    _snwprintf(sc, MAX_PATH, L"%s\\steamcmd.exe", dir);
    if (tp_exists(sc)) Log(L"Using SteamCMD: %s", sc);
    else if (RunPs(L"steamcmd", dir, NULL) != 0 || !tp_exists(sc)) {
        if (g_cancelReq) { Log(L"Cancelled."); SetProgress(0, L"Cancelled."); return FALSE; }
        /* fallback without PowerShell: urlmon download + Windows tar.exe */
        Log(L"Helper could not get SteamCMD; downloading it directly...");
        SHCreateDirectoryExW(NULL, dir, NULL);
        WCHAR zip[MAX_PATH]; _snwprintf(zip, MAX_PATH, L"%s\\steamcmd.zip", dir);
        HRESULT hr = URLDownloadToFileW(NULL, L"https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip", zip, 0, NULL);
        if (FAILED(hr)) { Log(L"ERROR: download of steamcmd.zip failed (0x%08lx). Check the internet connection / antivirus.", hr); return FALSE; }
        WCHAR tar[MAX_PATH]; GetSystemDirectoryW(tar, MAX_PATH); PathAppendW(tar, L"tar.exe");
        WCHAR ta[1024]; _snwprintf(ta, 1023, L"-xf \"%s\" -C \"%s\"", zip, dir);
        RunCapture(tar, ta, dir, L"  tar> ");
        if (!tp_exists(sc)) { Log(L"ERROR: could not unpack SteamCMD into %s", dir); return FALSE; }
        Log(L"SteamCMD downloaded.");
    }
    return TRUE;
}

/* ---- steamclient64.dll: the dedicated server needs one with the interface CS2 asks for ---- */
#define STEAMCLIENT_IFACE "SteamClient023"
static BOOL DllHasString(const WCHAR *p, const char *s)
{
    size_t n; uint8_t *b = tp_read(p, &n, NULL); if (!b) return FALSE;
    size_t L = strlen(s); BOOL found = FALSE;
    for (size_t i = 0; i + L <= n && !found; i++) if (b[i] == (uint8_t)s[0] && !memcmp(b + i, s, L)) found = TRUE;
    free(b); return found;
}

/* ---- real test, in a child process: load steamclient64.dll and ask it for SteamClient023 ----
 * exit codes: 0 ok | 0x10000+err LoadLibrary failed | 3 no CreateInterface | 4 interface not provided */
static int SteamClientTestChild(const WCHAR *dll)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    HMODULE m = LoadLibraryExW(dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) return 0x10000 + (int)(GetLastError() & 0xFFFF);
    typedef void *(*CI_t)(const char *, int *);
    CI_t ci = (CI_t)(void *)GetProcAddress(m, "CreateInterface");
    if (!ci) return 3;
    int rc = 0; void *p = ci(STEAMCLIENT_IFACE, &rc);
    return p ? 0 : 4;
}

/* missing imports of a PE file (looked up next to it and in System32) */
static void LogMissingImports(const WCHAR *dll)
{
    size_t n; uint8_t *b = tp_read(dll, &n, NULL); if (!b || n < 0x400) { free(b); return; }
    uint32_t pe = *(uint32_t *)(b + 0x3C);
    if (pe + 0x108 > n || memcmp(b + pe, "PE\0\0", 4)) { free(b); return; }
    uint16_t nsec = *(uint16_t *)(b + pe + 6), opt = *(uint16_t *)(b + pe + 20);
    uint8_t *oh = b + pe + 24, *sh = oh + opt;
    uint32_t irva = *(uint32_t *)(oh + 120);       /* PE32+: data dir 1 */
    #define RVA2OFF(r) ({ uint32_t _o = 0; for (int _i = 0; _i < nsec; _i++) { uint8_t *_s = sh + 40 * _i; uint32_t va = *(uint32_t *)(_s + 12), vs = *(uint32_t *)(_s + 8), ro = *(uint32_t *)(_s + 20); if ((r) >= va && (r) < va + vs) { _o = (r) - va + ro; break; } } _o; })
    uint32_t io = RVA2OFF(irva);
    WCHAR dir[MAX_PATH]; wcscpy(dir, dll); PathRemoveFileSpecW(dir);
    WCHAR sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    int missing = 0;
    for (; io && io + 20 <= n; io += 20) {
        uint32_t nameRva = *(uint32_t *)(b + io + 12); if (!nameRva) break;
        uint32_t no = RVA2OFF(nameRva); if (!no || no >= n) break;
        const char *nm = (const char *)b + no;
        if (!_strnicmp(nm, "api-ms-", 7) || !_strnicmp(nm, "ext-ms-", 7)) continue;
        WCHAR w[128]; MultiByteToWideChar(CP_ACP, 0, nm, -1, w, 128);
        WCHAR p1[MAX_PATH], p2[MAX_PATH];
        _snwprintf(p1, MAX_PATH, L"%s\\%s", dir, w); _snwprintf(p2, MAX_PATH, L"%s\\%s", sys, w);
        if (!tp_exists(p1) && !tp_exists(p2)) { Log(L"    needs %s - not found next to it or in System32", w); missing++; }
    }
    #undef RVA2OFF
    if (!missing) Log(L"    all DLLs it imports are present");
    free(b);
}

static BOOL TestSteamClient(const WCHAR *dll, BOOL verbose)
{
    if (!tp_exists(dll)) { if (verbose) Log(L"    %s: not there", dll); return FALSE; }
    WCHAR self[MAX_PATH], cmd[MAX_PATH * 2 + 64]; GetModuleFileNameW(NULL, self, MAX_PATH);
    _snwprintf(cmd, MAX_PATH * 2 + 64, L"\"%s\" --steamclient-test \"%s\"", self, dll);
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(self, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return FALSE;
    CloseHandle(pi.hThread);
    DWORD ec = 99;
    if (WaitForSingleObject(pi.hProcess, 20000) == WAIT_TIMEOUT) { TerminateProcess(pi.hProcess, 98); ec = 98; }
    else GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess);
    if (ec == 0) { Log(L"    %s: loads and provides %hs - OK", dll, STEAMCLIENT_IFACE); return TRUE; }
    if (ec >= 0x10000 && ec < 0x20000) {
        DWORD e = ec - 0x10000; WCHAR msg[256] = L"";
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, msg, 255, NULL);
        for (WCHAR *q = msg; *q; q++) if (*q == L'\r' || *q == L'\n') *q = L' ';
        Log(L"    %s: does NOT load (error %lu: %s)", dll, e, msg);
        LogMissingImports(dll);
    }
    else if (ec == 3) Log(L"    %s: loads, but has no CreateInterface (not a steamclient64.dll?)", dll);
    else if (ec == 4) Log(L"    %s: loads, but does NOT provide %hs (too old)", dll, STEAMCLIENT_IFACE);
    else Log(L"    %s: test failed (code %lu - crashed or timed out)", dll, ec);
    return FALSE;
}

/* copies steamclient64.dll + tier0_s64.dll + vstdlib_s64.dll from `from` into bin as one set */
static BOOL CopySteamSet(const WCHAR *from, const WCHAR *bin)
{
    static const WCHAR *files[3] = { L"steamclient64.dll", L"tier0_s64.dll", L"vstdlib_s64.dll" };
    for (int i = 0; i < 3; i++) {
        WCHAR s[MAX_PATH], d[MAX_PATH];
        _snwprintf(s, MAX_PATH, L"%s\\%s", from, files[i]); _snwprintf(d, MAX_PATH, L"%s\\%s", bin, files[i]);
        if (!tp_exists(s)) { Log(L"    %s not in %s", files[i], from); if (i == 0) return FALSE; continue; }
        if (!CopyFileW(s, d, FALSE)) { Log(L"ERROR: cannot copy %s (error %lu)", files[i], GetLastError()); return FALSE; }
        Log(L"    copied %s from %s", files[i], from);
    }
    return TRUE;
}

/* Steamworks SDK loads HKCU\\Software\\Valve\\Steam\\ActiveProcess\\SteamClientDll64 if it is set,
 * even when Steam is closed. If that copy is too old, point it at the working server copy for the
 * few seconds the server needs to initialise, then put the original value back. */
static BOOL g_redirect = FALSE;
static WCHAR g_redirectTo[MAX_PATH];
#define AP_KEY L"Software\\Valve\\Steam\\ActiveProcess"

static void RestoreSteamClientReg(void)
{
    WCHAR saved[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"steamclient", L"saved", L"", saved, MAX_PATH, g_ini);
    if (!saved[0]) return;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, AP_KEY, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        WCHAR cur[MAX_PATH]; DWORD sz = sizeof cur, type;
        /* only put it back if it is still our value (Steam may have rewritten it meanwhile) */
        if (RegQueryValueExW(k, L"SteamClientDll64", NULL, &type, (BYTE *)cur, &sz) != ERROR_SUCCESS || !_wcsicmp(cur, g_redirectTo[0] ? g_redirectTo : cur))
            RegSetValueExW(k, L"SteamClientDll64", 0, REG_SZ, (const BYTE *)saved, (DWORD)((wcslen(saved) + 1) * sizeof(WCHAR)));
        RegCloseKey(k);
        Log(L"  Steam registry entry restored (SteamClientDll64 = %s)", saved);
    }
    WritePrivateProfileStringW(L"steamclient", L"saved", NULL, g_ini);
}

static void RedirectSteamClientReg(void)
{
    HKEY k; WCHAR cur[MAX_PATH]; DWORD sz = sizeof cur, type;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, AP_KEY, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return;
    if (RegQueryValueExW(k, L"SteamClientDll64", NULL, &type, (BYTE *)cur, &sz) == ERROR_SUCCESS) {
        WritePrivateProfileStringW(L"steamclient", L"saved", cur, g_ini);     /* survives a launcher crash */
        RegSetValueExW(k, L"SteamClientDll64", 0, REG_SZ, (const BYTE *)g_redirectTo, (DWORD)((wcslen(g_redirectTo) + 1) * sizeof(WCHAR)));
        Log(L"  Pointing SteamClientDll64 at %s for the server start (old Steam copy: %s)", g_redirectTo, cur);
    }
    RegCloseKey(k);
}

static BOOL EnsureSteamClient(const Cfg *c)
{
    g_redirect = FALSE;
    WCHAR bin[MAX_PATH], dst[MAX_PATH];
    _snwprintf(bin, MAX_PATH, L"%s\\game\\bin\\win64", c->root);
    _snwprintf(dst, MAX_PATH, L"%s\\steamclient64.dll", bin);
    Log(L"  Testing the server's copy:");
    BOOL binOk = TestSteamClient(dst, TRUE);
    /* With Steam running, the Steamworks SDK uses the Steam client's registered steamclient64.dll instead */
    {
        DWORD pid = 0, psz = sizeof pid; WCHAR act[MAX_PATH]; DWORD sz = sizeof act, type;
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"pid", RRF_RT_REG_DWORD, NULL, &pid, &psz);
        HANDLE sp = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : NULL;
        BOOL steamRunning = sp != NULL; if (sp) CloseHandle(sp);
        if (steamRunning && RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"SteamClientDll64", RRF_RT_REG_SZ, &type, act, &sz) == ERROR_SUCCESS) {
            Log(L"  Steam is running (pid %lu) - the server uses Steam's registered copy:", pid);
            if (!TestSteamClient(act, TRUE)) {
                Log(L"  >> Steam's own steamclient64.dll is too old. Update Steam (Steam > Check for Steam Client Updates, or reinstall Steam).");
                if (binOk) { g_redirect = TRUE; _snwprintf(g_redirectTo, MAX_PATH, L"%s", dst); Log(L"  >> Workaround: the launcher will point the server at the working copy while it starts."); }
            }
            else
                Log(L"  >> Both copies pass this test. If CS2 still says \"No SteamClient023\", Exit Steam completely (tray icon > Exit) and Start again to see if it is Steam's copy.");
        } else if (!steamRunning) {
            sz = sizeof act;
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"SteamClientDll64", RRF_RT_REG_SZ, &type, act, &sz) == ERROR_SUCCESS) {
                Log(L"  Steam is not running, but the registry still points the SDK at Steam's copy (the server uses it):");
                if (!TestSteamClient(act, TRUE)) {
                    Log(L"  >> Steam's own steamclient64.dll is too old. Update Steam (Steam > Check for Steam Client Updates, or reinstall Steam).");
                    if (binOk) { g_redirect = TRUE; _snwprintf(g_redirectTo, MAX_PATH, L"%s", dst); Log(L"  >> Workaround: the launcher will point the server at the working copy while it starts."); }
                }
            }
        }
    }
    if (binOk) return TRUE;

    /* 1. SteamCMD's set (update SteamCMD first so it is current) */
    WCHAR sc[MAX_PATH], dir[MAX_PATH];
    if (GetSteamCmd(c, sc, dir)) {
        Log(L"  Updating SteamCMD (gets the current Steam client files)...");
        SetProgress(-1, L"Updating SteamCMD...");
        g_pLastTick = 0; g_pLastBytes = 0; g_pSpeed = 0; g_pLastLogged = -1;
        int rc = RunCaptureF(sc, L"+quit", dir, L"  steamcmd> ", ParseSteamcmd);
        if (rc == -2) { Log(L"Cancelled."); return FALSE; }
        SetProgress(1000, L"SteamCMD updated.");
        WCHAR src[MAX_PATH]; _snwprintf(src, MAX_PATH, L"%s\\steamclient64.dll", dir);
        Log(L"  Testing SteamCMD's copy:");
        if (TestSteamClient(src, TRUE)) {
            if (CopySteamSet(dir, bin) && TestSteamClient(dst, TRUE)) return TRUE;
        }
    }
    /* 2. the Steam client's set */
    WCHAR sp[MAX_PATH]; DWORD sz = sizeof sp, type;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, &type, sp, &sz) == ERROR_SUCCESS) {
        for (WCHAR *q = sp; *q; q++) if (*q == L'/') *q = L'\\';
        WCHAR alt[MAX_PATH]; _snwprintf(alt, MAX_PATH, L"%s\\steamclient64.dll", sp);
        Log(L"  Testing the Steam client's copy:");
        if (TestSteamClient(alt, TRUE) && CopySteamSet(sp, bin) && TestSteamClient(dst, TRUE)) return TRUE;
    }
    Log(L"ERROR: no working steamclient64.dll with %hs found. See the lines above for why each copy failed.", STEAMCLIENT_IFACE);
    return FALSE;
}

static BOOL DoInstall(const Cfg *c)
{
    if (g_running) { Log(L"Stop the server first."); return FALSE; }
    CreateDirectoryW(c->root, NULL);
    if (HasServer(c->root)) {
        Log(L"Restoring 64 tick before the update...");
        if (!tp_restore(c->root, Log)) Log(L"WARNING: restore was not clean; SteamCMD validate will fix the files.");
    }
    WCHAR sc[MAX_PATH], dir[MAX_PATH], args[1024];
    if (!GetSteamCmd(c, sc, dir)) return FALSE;
    _snwprintf(args, 1023, L"+force_install_dir \"%s\" +login anonymous +app_update 730 validate +quit", c->root);
    for (int attempt = 1; attempt <= 3; attempt++) {
        Log(L"SteamCMD: installing / updating CS2 (app 730), attempt %d. This can take a while...", attempt);
        g_pLastTick = 0; g_pLastBytes = 0; g_pSpeed = 0; g_pLastLogged = -1;
        SetProgress(-1, L"SteamCMD: starting...");
        int rc = RunCaptureF(sc, args, dir, L"  steamcmd> ", ParseSteamcmd);
        if (rc == -2) { Log(L"Install / Update cancelled. Run it again to continue - SteamCMD keeps what it already downloaded."); SetProgress(0, L"Cancelled."); return FALSE; }
        Log(L"SteamCMD exit code %d", rc);
        if (HasServer(c->root) && rc == 0) break;
        if (attempt == 3) { Log(L"ERROR: SteamCMD did not finish the install."); return FALSE; }
    }
    SetProgress(1000, L"Install / Update finished.");
    Log(L"Steam server library:"); EnsureSteamClient(c);
    Log(L"Server installed. Patch state:");
    int st = tp_status(c->root, Log);
    if (st == 0) Log(L"WARNING: this CS2 build is not supported by the patch engine (only %hs). 64 tick still works.", TP_BUILD_1420_NAME);
    if (HasCss(c->root)) Log(L"Note: CS2 updates can break CounterStrikeSharp - click Check updates.");
    return TRUE;
}

/* ------------------------------------------------------------- diagnostics */
static const WCHAR *ExitMeaning(DWORD ec)
{
    switch (ec) {
    case 0: return L"normal exit";
    case 1: return L"CS2 quit by itself - a startup error (look for FATAL ERROR in the lines below)";
    case 0xC0000005: return L"access violation - a crash, usually a plugin/Metamod/CounterStrikeSharp or a bad DLL";
    case 0xC0000135: return L"a required DLL was not found (missing file, or missing Visual C++ runtime)";
    case 0xC000007B: return L"bad image - a DLL is corrupt or the wrong type (32/64-bit)";
    case 0xC0000139: return L"entry point not found - DLL version mismatch (e.g. Metamod/CSS built for another CS2 build)";
    case 0xC0000142: return L"DLL initialisation failed";
    case 0xC0000409: return L"fail-fast / stack buffer overrun - a crash";
    case 0xC0000374: return L"heap corruption - a crash";
    case 0xC00000FD: return L"stack overflow - a crash";
    case 0x80000003: return L"breakpoint / assertion - a crash";
    case 0xE06D7363: return L"unhandled C++ exception";
    case 0xC000013A: return L"stopped with Ctrl+C / window closed";
    case 1450:       return L"ERROR_NO_SYSTEM_RESOURCES (0x5AA)";
    }
    return L"unknown";
}

static BOOL FileIn(const WCHAR *root, const WCHAR *rel)
{
    WCHAR p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%s\\%s", root, rel); p[MAX_PATH-1] = 0; return tp_exists(p);
}

static BOOL PortFree(int port)
{
    WSADATA wd; if (WSAStartup(MAKEWORD(2, 2), &wd)) return TRUE;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); BOOL ok = TRUE;
    if (s != INVALID_SOCKET) {
        struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = INADDR_ANY;
        BOOL excl = TRUE; setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
        if (bind(s, (struct sockaddr *)&a, sizeof a) == SOCKET_ERROR) ok = FALSE;
        closesocket(s);
    }
    WSACleanup(); return ok;
}

static int OtherServers(const WCHAR *exe)
{
    int n = 0; HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof pe };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"cs2.exe")) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID); if (!h) continue;
        WCHAR img[MAX_PATH]; DWORD cch = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, img, &cch) && !_wcsicmp(img, exe)) { Log(L"  ERROR: cs2.exe from this folder is already running (pid %lu)", pe.th32ProcessID); n++; }
        CloseHandle(h);
    }
    CloseHandle(snap); return n;
}

/* Returns number of definite errors found. Everything is written to the log. */
static int Preflight(const Cfg *c)
{
    int err = 0;
    Log(L"--- Pre-start checks ---");
    WCHAR inf[MAX_PATH]; _snwprintf(inf, MAX_PATH, L"%s\\game\\csgo\\steam.inf", c->root);
    size_t n; uint8_t *b = tp_read(inf, &n, NULL);
    if (b) {
        b[n] = 0; char *pv = strstr((char *)b, "PatchVersion="), *vd = strstr((char *)b, "VersionDate=");
        char v1[32] = "?", v2[32] = "?";
        if (pv) sscanf(pv + 13, "%31[^\r\n]", v1);
        if (vd) sscanf(vd + 12, "%31[^\r\n]", v2);
        Log(L"  CS2 server build: %hs (%hs)", v1, v2); free(b);
    } else { Log(L"  WARNING: game\\csgo\\steam.inf missing - incomplete install?"); }
    static const WCHAR *need[] = { L"game\\bin\\win64\\cs2.exe", L"game\\bin\\win64\\engine2.dll", L"game\\bin\\win64\\tier0.dll",
        L"game\\csgo\\bin\\win64\\server.dll", L"game\\csgo\\gameinfo.gi", L"game\\csgo\\pak01_dir.vpk" };
    for (int i = 0; i < 6; i++) if (!FileIn(c->root, need[i])) { Log(L"  ERROR: missing %s - run Install / Update (validate)", need[i]); err++; }
    {
        WCHAR scd[MAX_PATH]; _snwprintf(scd, MAX_PATH, L"%s\\game\\bin\\win64\\steamclient64.dll", c->root);
        if (!tp_exists(scd)) { Log(L"  ERROR: game\\bin\\win64\\steamclient64.dll missing (server stops with \"No %hs\")", STEAMCLIENT_IFACE); err++; }
        /* which steamclient the running Steam client would hand out */
        DWORD sz = MAX_PATH * sizeof(WCHAR), type; WCHAR act[MAX_PATH];
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"SteamClientDll64", RRF_RT_REG_SZ, &type, act, &sz) == ERROR_SUCCESS)
            Log(L"  Steam client registered steamclient: %s", act);
    }
    /* map */
    WCHAR mp[MAX_PATH]; _snwprintf(mp, MAX_PATH, L"game\\csgo\\maps\\%s.vpk", c->map[0] ? c->map : L"de_dust2");
    if (!FileIn(c->root, mp)) Log(L"  WARNING: %s not found - the map name may be wrong", mp);
    /* gameinfo / metamod */
    WCHAR gi[MAX_PATH]; _snwprintf(gi, MAX_PATH, L"%s\\game\\csgo\\gameinfo.gi", c->root);
    b = tp_read(gi, &n, NULL);
    if (b) {
        b[n] = 0; BOOL mm = strstr((char *)b, "csgo/addons/metamod") != NULL; free(b);
        Log(L"  gameinfo.gi: Metamod %s", mm ? L"ON" : L"off");
        if (mm && !FileIn(c->root, L"game\\csgo\\addons\\metamod\\bin\\win64\\server.dll")) {
            Log(L"  ERROR: gameinfo.gi loads Metamod but addons\\metamod\\bin\\win64\\server.dll is missing"); err++;
        }
        if (mm && HasCss(c->root)) {
            if (!FileIn(c->root, L"game\\csgo\\addons\\counterstrikesharp\\bin\\win64\\counterstrikesharp.dll"))
                { Log(L"  ERROR: CounterStrikeSharp is incomplete (bin\\win64\\counterstrikesharp.dll missing)"); err++; }
            WCHAR dn[MAX_PATH]; _snwprintf(dn, MAX_PATH, L"%s\\game\\csgo\\addons\\counterstrikesharp\\dotnet", c->root);
            DWORD a = GetFileAttributesW(dn);
            if (a == INVALID_FILE_ATTRIBUTES) Log(L"  WARNING: CounterStrikeSharp has no dotnet folder (not the with-runtime build)");
        }
    }
    /* VC++ runtime (CounterStrikeSharp/Metamod need it) */
    WCHAR sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    static const WCHAR *vc[3] = { L"vcruntime140.dll", L"vcruntime140_1.dll", L"msvcp140.dll" };
    for (int i = 0; i < 3; i++) {
        WCHAR p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%s\\%s", sys, vc[i]);
        if (!tp_exists(p)) { Log(L"  WARNING: %s missing - install the Visual C++ 2015-2022 x64 redistributable", vc[i]); }
    }
    /* already running / port */
    WCHAR exe[MAX_PATH]; _snwprintf(exe, MAX_PATH, L"%s\\game\\bin\\win64\\cs2.exe", c->root);
    WCHAR full[MAX_PATH]; GetFullPathNameW(exe, MAX_PATH, full, NULL);
    err += OtherServers(full);
    if (!PortFree(c->port)) { Log(L"  ERROR: UDP port %d is already in use (another server or program)", c->port); err++; }
    else Log(L"  UDP port %d is free", c->port);
    /* write access + disk */
    WCHAR t[MAX_PATH]; _snwprintf(t, MAX_PATH, L"%s\\game\\csgo\\cs2launcher_write.tmp", c->root);
    HANDLE f = CreateFileW(t, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (f == INVALID_HANDLE_VALUE) { Log(L"  ERROR: cannot write in game\\csgo (error %lu) - folder permissions?", GetLastError()); err++; } else CloseHandle(f);
    ULARGE_INTEGER fr; if (GetDiskFreeSpaceExW(c->root, &fr, NULL, NULL) && fr.QuadPart < 2000000000ULL) Log(L"  WARNING: only %.1f GB free on the server drive", fr.QuadPart / 1e9);
    Log(L"  Patch state:"); tp_status(c->root, Log);
    Log(err ? L"--- %d problem(s) found ---" : L"--- checks OK ---", err);
    return err;
}

/* list the Steam-related modules a minidump says were loaded (definitive: which steamclient CS2 used) */
static void LogDumpModules(const WCHAR *path)
{
    size_t n; uint8_t *b = tp_read(path, &n, NULL);
    if (!b || n < 32 || memcmp(b, "MDMP", 4)) { free(b); return; }
    uint32_t ns = *(uint32_t *)(b + 8), dir = *(uint32_t *)(b + 12);
    int shown = 0, total = 0;
    for (uint32_t i = 0; i < ns && dir + 12 * (i + 1) <= n; i++) {
        uint32_t type = *(uint32_t *)(b + dir + 12 * i), rva = *(uint32_t *)(b + dir + 12 * i + 8);
        if (type != 4 || rva + 4 > n) continue;                  /* ModuleListStream */
        uint32_t cnt = *(uint32_t *)(b + rva);
        for (uint32_t m = 0; m < cnt; m++) {
            size_t mo = rva + 4 + (size_t)m * 108; if (mo + 108 > n) break;
            uint32_t nameRva = *(uint32_t *)(b + mo + 20); if (nameRva + 4 > n) continue;
            uint32_t len = *(uint32_t *)(b + nameRva); if (nameRva + 4 + len > n || len > 1000) continue;
            WCHAR name[520]; memcpy(name, b + nameRva + 4, len); name[len / 2] = 0;
            total++;
            WCHAR low[520]; wcscpy(low, name); _wcslwr(low);
            if (wcsstr(low, L"steam") || wcsstr(low, L"tier0_s") || wcsstr(low, L"vstdlib_s")) { Log(L"    loaded: %s", name); shown++; }
        }
    }
    if (total && !shown) Log(L"    no steamclient64.dll was loaded at all (%d modules in the dump) - CS2 could not load any copy", total);
    else if (total && shown) {
        BOOL sc = FALSE;
        /* re-scan quickly for steamclient specifically */
        for (size_t i = 0; i + 34 < n && !sc; i++) if (!memcmp(b + i, L"steamclient64.dll", 34)) sc = TRUE;
        if (!sc) Log(L"    (steam_api64 is loaded, but no steamclient64.dll - CS2 could not load any copy)");
    }
    free(b);
}

/* new crash dumps since `since` */
static void FindDumps(const Cfg *c, FILETIME since)
{
    static const WCHAR *dirs[3] = { L"game\\bin\\win64", L"game\\csgo", L"game" };
    for (int i = 0; i < 3; i++) {
        WCHAR pat[MAX_PATH]; _snwprintf(pat, MAX_PATH, L"%s\\%s\\*.mdmp", c->root, dirs[i]);
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        int shownMods = 0;
        do if (CompareFileTime(&fd.ftLastWriteTime, &since) >= 0) {
            Log(L"  Crash dump written: %s\\%s\\%s", c->root, dirs[i], fd.cFileName);
            if (!shownMods++) {
                WCHAR dp[MAX_PATH]; _snwprintf(dp, MAX_PATH, L"%s\\%s\\%s", c->root, dirs[i], fd.cFileName);
                Log(L"  Steam modules CS2 had loaded (from the dump):");
                LogDumpModules(dp);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

static void PostExitDiagnostics(const Cfg *c, DWORD ec, FILETIME started, DWORD ranMs);

static BOOL PrepareStart(const Cfg *c)
{
    if (!HasServer(c->root)) { Log(L"ERROR: no CS2 server in %s - click Install / Update.", c->root); return FALSE; }
    /* plugins + mode */
    if (c->noplug || !HasCss(c->root)) {
        /* Metamod without CounterStrikeSharp is useless and can stop the server */
        Log(c->noplug ? L"Run without plugins: Metamod off." : L"CounterStrikeSharp not installed: Metamod off.");
        RunPs(L"metamod", c->root, L"off");
    }
    if (HasCss(c->root)) {
        if (c->noplug) { }
        else {
            if (RunPs(L"metamod", c->root, L"on") != 0) return FALSE;
            Log(L"Mode %s:", MODES[c->mode]);
            if (RunPs(L"set-mode", c->root, MODES[c->mode]) != 0) return FALSE;
        }
    } else {
        if (!c->noplug) Log(L"Plugins are not installed (click Install plugins). Starting stock CS2.");
        if (c->mode == 3) Log(L"WARNING: retakes mode needs the plugins.");
    }
    WriteModeCfg(c);
    Log(L"Steam server library:");
    if (!EnsureSteamClient(c)) { Log(L"ERROR: the server cannot start without it (\"No %hs\")", STEAMCLIENT_IFACE); return FALSE; }
    if (Preflight(c) && !c->debug) Log(L"Starting anyway - the problems above are the likely cause if it fails.");
    /* tick */
    if (c->tick == 64) {
        Log(L"64 tick: restoring original DLLs...");
        if (!tp_restore(c->root, Log)) { Log(L"ERROR: could not restore 64 tick."); return FALSE; }
    } else {
        Log(L"Patching server to %d tick:", c->tick);
        if (!tp_apply(c->root, c->tick, Log) || tp_status(c->root, NULL) != c->tick) {
            Log(L"ERROR: patch failed; server not started.");
            tp_restore(c->root, Log);
            return FALSE;
        }
    }
    return TRUE;
}

static void BuildArgs(const Cfg *c, WCHAR *a, size_t n)
{
    static const int gt[4] = { 0, 0, 0, 0 }, gm[4] = { 1, 2, 1, 1 };
    WCHAR host[200];
    if (c->tick > 64) _snwprintf(host, 200, L"%s [%d tick]", c->name, c->tick); else _snwprintf(host, 200, L"%s", c->name);
    for (WCHAR *q = host; *q; q++) if (*q == L'"') *q = L'\'';
    int k = _snwprintf(a, n, L"-dedicated -insecure -console -usercon -condebug -port %d -maxplayers %d "
                             L"+game_type %d +game_mode %d +map %s +hostname \"%s\" +sv_fps_max %d",
                       c->port, c->maxpl, gt[c->mode], gm[c->mode], c->map[0] ? c->map : L"de_dust2", host,
                       c->tick == 64 ? 0 : c->tick);
    if (c->pass[0]) k += _snwprintf(a + k, n - k, L" +sv_password \"%s\"", c->pass);
    k += _snwprintf(a + k, n - k, L" +rcon_password \"%s\"", c->rcon[0] ? c->rcon : g_autoRcon);   /* RCON = how the launcher talks to the server */
    if (c->lan) k += _snwprintf(a + k, n - k, L" +sv_lan 1");
    else {
        k += _snwprintf(a + k, n - k, L" +sv_lan 0");
        if (c->gslt[0]) k += _snwprintf(a + k, n - k, L" +sv_setsteamaccount %s", c->gslt);
    }
    _snwprintf(a + k, n - k, L" +exec cs2launcher_%s.cfg", MODES[c->mode]);
}

/* -condebug writes console.log; show its tail after an unexpected exit */
static void ConLogPath(const Cfg *c, int i, WCHAR *o)
{
    static const WCHAR *rel[3] = { L"game\\csgo\\console.log", L"game\\bin\\win64\\console.log", L"game\\console.log" };
    _snwprintf(o, MAX_PATH, L"%s\\%s", c->root, rel[i]); o[MAX_PATH-1] = 0;
}
static void ClearConLog(const Cfg *c)
{
    for (int i = 0; i < 3; i++) { WCHAR p[MAX_PATH]; ConLogPath(c, i, p); DeleteFileW(p); }
}
static void DumpConLog(const Cfg *c, int lines)
{
    for (int i = 0; i < 3; i++) {
        WCHAR p[MAX_PATH]; ConLogPath(c, i, p);
        size_t n; uint8_t *b = tp_read(p, &n, NULL);
        if (!b) continue;
        b[n] = 0;
        int seen = 0; size_t s = n;
        while (s > 0 && seen <= lines) { s--; if (b[s] == '\n') seen++; }
        if (b[s] == '\n') s++;
        Log(L"Last lines of CS2's console (%s):", p);
        char *q = (char *)b + s;
        while (*q) {
            char *e = strchr(q, '\n'); if (e) *e = 0;
            size_t L = strlen(q); if (L && q[L - 1] == '\r') q[L - 1] = 0;
            if (*q) { WCHAR w[2048]; MultiByteToWideChar(CP_UTF8, 0, q, -1, w, 2048); w[2047] = 0; Log(L"  cs2> %s", w); }
            if (!e) break; q = e + 1;
        }
        free(b);
        return;
    }
    Log(L"(CS2 wrote no console.log - it exited before the console started.)");
}

static int RunPs(const WCHAR *action, const WCHAR *root, const WCHAR *arg);
static void PostExitDiagnostics(const Cfg *c, DWORD ec, FILETIME started, DWORD ranMs)
{
    (void)ec;
    Log(L"--- Diagnostics ---");
    DumpConLog(c, 40);
    {   /* name known causes */
        WCHAR p[MAX_PATH]; ConLogPath(c, 0, p);
        if (DllHasString(p, "No SteamClient")) Log(L"CAUSE: CS2 could not use steamclient64.dll (see the \"Steam server library\" test at the top of this start).");
        else if (DllHasString(p, "Address already in use") || DllHasString(p, "Couldn't allocate any server IP port")) Log(L"CAUSE: the port is in use - change Port or stop the other server.");
        else if (DllHasString(p, "FATAL ERROR")) Log(L"CAUSE: CS2 reported a FATAL ERROR - see the cs2> lines above.");
    }
    FindDumps(c, started);
    WCHAR mins[16]; _snwprintf(mins, 16, L"%lu", ranMs / 60000 + 2);
    Log(L"Windows error events:");
    RunPs(L"events", c->root, mins);
    Log(L"--- end of diagnostics (saved in %s) ---", g_sessLog);
}

/* ---- server under a pseudo console: its output goes to the log, commands go in ---- */
typedef HRESULT (WINAPI *CreatePC_t)(COORD, HANDLE, HANDLE, DWORD, HANDLE *);
typedef void    (WINAPI *ClosePC_t)(HANDLE);
typedef struct { HANDLE pipe; } VtReader;

/* ---- server console: lines go to the Console tab, or to a capture buffer while we query the server ---- */
static CRITICAL_SECTION g_capLock;
static volatile LONG g_capOn = 0;          /* 1 = json (status_json), 2 = text (status) */
static char *g_cap = NULL; static size_t g_capLen = 0, g_capCap = 0; static int g_capDepth = 0, g_capSeen = 0;
static HANDLE g_capDone = NULL;

static void SessLogOnly(const WCHAR *line)
{
    if (!g_sessLog[0]) return;
    char u8[9000]; int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, u8, sizeof u8, NULL, NULL);
    HANDLE f = CreateFileW(g_sessLog, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) { DWORD w; if (n > 1) WriteFile(f, u8, n - 1, &w, NULL); CloseHandle(f); }
}

static void ConsoleLine(const char *line)
{
    if (g_capOn) {
        EnterCriticalSection(&g_capLock);
        size_t L = strlen(line);
        if (g_capLen + L + 2 > g_capCap) { size_t nc = (g_capCap ? g_capCap * 2 : 65536); while (nc < g_capLen + L + 2) nc *= 2; char *nb = (char *)realloc(g_cap, nc); if (nb) { g_cap = nb; g_capCap = nc; } }
        if (g_capLen + L + 2 <= g_capCap) { memcpy(g_cap + g_capLen, line, L); g_capLen += L; g_cap[g_capLen++] = '\n'; g_cap[g_capLen] = 0; }
        if (g_capOn == 1) {
            for (const char *q = line; *q; q++) { if (*q == '{') { g_capDepth++; g_capSeen = 1; } else if (*q == '}') g_capDepth--; }
            if (g_capSeen && g_capDepth <= 0) SetEvent(g_capDone);
            if (strstr(line, "Unknown command")) SetEvent(g_capDone);
        } else if (strstr(line, "#end")) SetEvent(g_capDone);
        LeaveCriticalSection(&g_capLock);
        return;
    }
    WCHAR w[4096]; MultiByteToWideChar(CP_UTF8, 0, line, -1, w, 4096); w[4095] = 0;
    SYSTEMTIME st; GetLocalTime(&st);
    WCHAR full[4200]; _snwprintf(full, 4200, L"[%02d:%02d:%02d] %s\r\n", st.wHour, st.wMinute, st.wSecond, w); full[4199] = 0;
    SessLogOnly(full);
    WCHAR *cp = _wcsdup(full);
    if (cp && !PostMessageW(g_wnd, WM_APP_CON, 0, (LPARAM)cp)) free(cp);
}

static DWORD WINAPI VtReadThread(LPVOID p)
{
    HANDLE pipe = ((VtReader *)p)->pipe; free(p);
    char buf[4096], line[4096], last[4096] = ""; int ln = 0, st = 0; DWORD n;
#define FLUSH() do { while (ln && line[ln-1] == ' ') ln--; line[ln] = 0; \
        if (ln && strcmp(line, last)) { ConsoleLine(line); strcpy(last, line); } \
        ln = 0; } while (0)
    while (ReadFile(pipe, buf, sizeof buf, &n, NULL) && n) {
        for (DWORD i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)buf[i];
            switch (st) {
            case 0:
                if (ch == 0x1B) st = 1;
                else if (ch == '\n') FLUSH();
                else if (ch >= 0x20 || ch == '\t') { if (ln < (int)sizeof line - 1) line[ln++] = (char)ch; else FLUSH(); }
                break;
            case 1: st = ch == '[' ? 2 : ch == ']' ? 3 : 0; break;
            case 2: if (ch >= 0x40 && ch <= 0x7E) { if (ch == 'H' || ch == 'f' || ch == 'E') FLUSH(); st = 0; } break;
            case 3: if (ch == 0x07) st = 0; else if (ch == 0x1B) st = 4; break;
            case 4: st = ch == '\\' ? 0 : 3; break;
            }
        }
    }
    FLUSH();
#undef FLUSH
    return 0;
}

/* Starts cs2.exe under a pseudo console. Returns process handle, or NULL if ConPTY is unavailable. */
static HANDLE StartUnderConPty(const WCHAR *exe, WCHAR *cmd, const WCHAR *dir, HANDLE *hpcOut, HANDLE *readerOut, DWORD *pidOut)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    CreatePC_t createPC = (CreatePC_t)(void *)GetProcAddress(k32, "CreatePseudoConsole");
    if (!createPC) return NULL;
    HANDLE inR, inW, outR, outW;
    if (!CreatePipe(&inR, &inW, NULL, 0)) return NULL;
    if (!CreatePipe(&outR, &outW, NULL, 0)) { CloseHandle(inR); CloseHandle(inW); return NULL; }
    HANDLE hpc; COORD size = { 500, 100 };   /* wide: fewer wrapped lines */
    if (FAILED(createPC(size, inR, outW, 0, &hpc))) { CloseHandle(inR); CloseHandle(inW); CloseHandle(outR); CloseHandle(outW); return NULL; }
    CloseHandle(inR); CloseHandle(outW);
    SIZE_T asz = 0; InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    LPPROC_THREAD_ATTRIBUTE_LIST al = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(asz);
    HANDLE proc = NULL;
    if (al && InitializeProcThreadAttributeList(al, 1, 0, &asz) &&
        UpdateProcThreadAttribute(al, 0, 0x00020016 /* PSEUDOCONSOLE */, hpc, sizeof hpc, NULL, NULL)) {
        STARTUPINFOEXW si; ZeroMemory(&si, sizeof si); si.StartupInfo.cb = sizeof si; si.lpAttributeList = al;
        PROCESS_INFORMATION pi;
        if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL, dir, &si.StartupInfo, &pi)) {
            CloseHandle(pi.hThread); proc = pi.hProcess; *pidOut = pi.dwProcessId;
        }
        DeleteProcThreadAttributeList(al);
    }
    free(al);
    if (!proc) { ((ClosePC_t)(void *)GetProcAddress(k32, "ClosePseudoConsole"))(hpc); CloseHandle(inW); CloseHandle(outR); return NULL; }
    VtReader *vr = (VtReader *)malloc(sizeof *vr); vr->pipe = outR;
    *readerOut = CreateThread(NULL, 0, VtReadThread, vr, 0, NULL);
    *hpcOut = hpc;
    g_srvIn = inW;
    return proc;
}

static void EndConPty(HANDLE hpc, HANDLE reader)
{
    ClosePC_t closePC = (ClosePC_t)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ClosePseudoConsole");
    if (hpc && closePC) closePC(hpc);
    if (reader) { WaitForSingleObject(reader, 5000); CloseHandle(reader); }
    if (g_srvIn) { CloseHandle(g_srvIn); g_srvIn = NULL; }
}

/* ---- Source RCON over TCP on localhost: how the launcher sends commands and reads replies ---- */
static CRITICAL_SECTION g_rconLock;
static SOCKET g_rs = INVALID_SOCKET;
static int g_rconPort = 27015, g_rconId = 10;
static char g_rconPwA[160];
static volatile LONG g_rconOk = 0;

static BOOL WaitReadable(SOCKET s, DWORD ms)
{ fd_set f; FD_ZERO(&f); FD_SET(s, &f); struct timeval tv = { (long)(ms / 1000), (long)((ms % 1000) * 1000) }; return select(0, &f, NULL, NULL, &tv) > 0; }
static BOOL RecvAll(SOCKET s, char *b, int n)
{ while (n > 0) { if (!WaitReadable(s, 3000)) return FALSE; int r = recv(s, b, n, 0); if (r <= 0) return FALSE; b += r; n -= r; } return TRUE; }
static BOOL RconSendPkt(SOCKET s, int id, int type, const char *body)
{
    int L = (int)strlen(body), size = 10 + L; char *p = (char *)malloc(size + 4); if (!p) return FALSE;
    memcpy(p, &size, 4); memcpy(p + 4, &id, 4); memcpy(p + 8, &type, 4); memcpy(p + 12, body, L); p[12 + L] = 0; p[13 + L] = 0;
    BOOL ok = send(s, p, size + 4, 0) == size + 4; free(p); return ok;
}
static int RconRecvPkt(SOCKET s, int *id, int *type, char **body)
{
    int size; if (!RecvAll(s, (char *)&size, 4)) return -1;
    if (size < 10 || size > (1 << 22)) return -1;
    char *p = (char *)malloc(size + 1); if (!p || !RecvAll(s, p, size)) { free(p); return -1; }
    p[size] = 0; memcpy(id, p, 4); memcpy(type, p + 4, 4); *body = _strdup(p + 8); free(p); return 0;
}
static void RconClose(void) { if (g_rs != INVALID_SOCKET) { closesocket(g_rs); g_rs = INVALID_SOCKET; } }
static BOOL RconConnect(void)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); if (s == INVALID_SOCKET) return FALSE;
    struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons((u_short)g_rconPort); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { closesocket(s); return FALSE; }
    RconSendPkt(s, 1, 3, g_rconPwA);
    for (int i = 0; i < 4; i++) {
        int id, type; char *b;
        if (!WaitReadable(s, 3000) || RconRecvPkt(s, &id, &type, &b)) break;
        free(b);
        if (type == 2) {
            if (id == -1) { closesocket(s); Log(L"RCON: the server rejected the password."); return FALSE; }
            g_rs = s; if (!InterlockedExchange(&g_rconOk, 1)) Log(L"RCON connected (the launcher sends commands this way)."); return TRUE;
        }
    }
    closesocket(s); return FALSE;
}
/* runs one command; returns its output (malloc'd, may be "") or NULL if RCON is not reachable */
static char *RconExec(const char *cmd)
{
    if (!g_running) return NULL;
    EnterCriticalSection(&g_rconLock);
    char *out = NULL;
    for (int attempt = 0; attempt < 2 && !out; attempt++) {
        if (g_rs == INVALID_SOCKET && !RconConnect()) break;
        int id = ++g_rconId, endId = ++g_rconId;
        if (!RconSendPkt(g_rs, id, 2, cmd) || !RconSendPkt(g_rs, endId, 0, "")) { RconClose(); continue; }
        size_t cap = 8192, len = 0; char *buf = (char *)calloc(cap, 1); BOOL any = FALSE, dead = FALSE;
        DWORD t0 = GetTickCount();
        for (;;) {
            if (!WaitReadable(g_rs, any ? 400 : 3000)) break;             /* idle after the reply = done */
            int rid, type; char *b;
            if (RconRecvPkt(g_rs, &rid, &type, &b)) { dead = TRUE; break; }
            any = TRUE;
            if (rid == endId) { free(b); break; }                           /* server mirrored our end marker */
            if (rid == id || rid == 0) { size_t L = strlen(b); if (len + L + 1 > cap) { cap = (len + L + 1) * 2; buf = (char *)realloc(buf, cap); } memcpy(buf + len, b, L); len += L; buf[len] = 0; }
            free(b);
            if (GetTickCount() - t0 > 8000) break;
        }
        if (dead && !len) { free(buf); RconClose(); continue; }
        out = buf;
    }
    LeaveCriticalSection(&g_rconLock);
    return out;
}

/* output text (several lines) into the Console tab */
static void ConsoleText(const char *s)
{
    char *t = _strdup(s); if (!t) return; char *ctx = NULL;
    for (char *ln = strtok_s(t, "\r\n", &ctx); ln; ln = strtok_s(NULL, "\r\n", &ctx)) if (*ln) ConsoleLine(ln);
    free(t);
}

static DWORD WINAPI CmdThread(LPVOID p)
{
    WCHAR *w = (WCHAR *)p;
    char u[1600]; WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL);
    char *o = RconExec(u);
    if (o) { if (*o) ConsoleText(o); free(o); }
    else if (g_srvIn) { int n = (int)strlen(u); u[n] = '\r'; DWORD wr; WriteFile(g_srvIn, u, n + 1, &wr, NULL); }
    else { WCHAR e[200]; _snwprintf(e, 200, L"(could not send - RCON not reachable yet)\r\n"); WCHAR *cp = _wcsdup(e); if (cp && !PostMessageW(g_wnd, WM_APP_CON, 0, (LPARAM)cp)) free(cp); }
    free(w);
    return 0;
}

static void SendServerCommand(void)
{
    WCHAR w[512]; GetWindowTextW(g_cmd, w, 512);
    if (!w[0]) return;
    if (!g_running) { Log(L"Server is not running."); return; }
    { WCHAR e[600]; _snwprintf(e, 600, L"> %s\r\n", w); WCHAR *cp = _wcsdup(e); if (cp && !PostMessageW(g_wnd, WM_APP_CON, 0, (LPARAM)cp)) free(cp); SessLogOnly(e); }
    HANDLE t = CreateThread(NULL, 0, CmdThread, _wcsdup(w), 0, NULL); if (t) CloseHandle(t);
    SetWindowTextW(g_cmd, L"");
}

/* follow game\csgo\console.log (-condebug) into the Console tab */
static volatile LONG g_tailRun = 0;
static DWORD WINAPI TailThread(LPVOID p)
{
    WCHAR path[MAX_PATH]; wcscpy(path, (WCHAR *)p); free(p);
    LONGLONG pos = 0; char part[8192]; int pl = 0;
    while (g_tailRun) {
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
            if (sz.QuadPart < pos) pos = 0;                       /* file was recreated */
            if (sz.QuadPart > pos) {
                LARGE_INTEGER o; o.QuadPart = pos; SetFilePointerEx(f, o, NULL, FILE_BEGIN);
                char buf[16384]; DWORD n;
                while (ReadFile(f, buf, sizeof buf, &n, NULL) && n) {
                    pos += n;
                    for (DWORD i = 0; i < n; i++) {
                        if (buf[i] == '\n' || pl >= (int)sizeof part - 1) { part[pl] = 0; if (pl && part[pl - 1] == '\r') part[--pl] = 0; if (pl) ConsoleLine(part); pl = 0; }
                        else part[pl++] = buf[i];
                    }
                }
            }
            CloseHandle(f);
        }
        Sleep(250);
    }
    return 0;
}

static DWORD WINAPI Monitor(LPVOID p)
{
    Cfg c = *(Cfg *)p; free(p);
    WCHAR exe[MAX_PATH], dir[MAX_PATH], args[4096], shown[4096];
    _snwprintf(dir, MAX_PATH, L"%s\\game\\bin\\win64", c.root);
    _snwprintf(exe, MAX_PATH, L"%s\\cs2.exe", dir);
    BuildArgs(&c, args, 4096);
    wcscpy(shown, args);
    /* hide secrets in the log */
    WCHAR *s;
    if ((s = wcsstr(shown, L"+sv_setsteamaccount "))) { s += 20; while (*s && *s != L' ') *s++ = L'*'; }
    if ((s = wcsstr(shown, L"+rcon_password \""))) { s += 16; while (*s && *s != L'"') *s++ = L'*'; }
    if ((s = wcsstr(shown, L"+sv_password \""))) { s += 14; while (*s && *s != L'"') *s++ = L'*'; }
    DWORD crashes[5] = { 0 }; int nc = 0;
    for (;;) {
        WCHAR cmd[4600]; _snwprintf(cmd, 4600, L"\"%s\" %s", exe, args);
        STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
        ClearConLog(&c);
        Log(L"Starting: cs2.exe %s", shown);
        FILETIME started; GetSystemTimeAsFileTime(&started); DWORD t0 = GetTickCount();
        HANDLE hpc = NULL, reader = NULL; DWORD pid = 0;
        if (c.debug) {
            /* visible console that stays open after CS2 exits */
            WCHAR com[MAX_PATH]; GetEnvironmentVariableW(L"ComSpec", com, MAX_PATH);
            static WCHAR dcmd[6000];
            _snwprintf(dcmd, 6000, L"\"%s\" /S /K \"title CS2 server (debug) & \"%s\" %s & call echo. & call echo === CS2 exited with code %%^errorlevel%% - close this window when done ===\"",
                       com, exe, args);
            if (g_redirect) RedirectSteamClientReg();
            if (!CreateProcessW(com, dcmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, dir, &si, &pi)) { if (g_redirect) RestoreSteamClientReg(); Log(L"ERROR: cannot open debug window (%lu)", GetLastError()); break; }
            if (g_redirect) { Sleep(30000); RestoreSteamClientReg(); }
            CloseHandle(pi.hThread);
            g_proc = pi.hProcess;
            InterlockedExchange(&g_running, 1); PostMessageW(g_wnd, WM_APP_STATE, 0, 0);
            Log(L"Debug window open: the server runs there and the window stays open after CS2 exits. Close it to finish.");
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess); g_proc = NULL;
            Log(L"Debug window closed.");
            PostExitDiagnostics(&c, 0xFFFFFFFF, started, GetTickCount() - t0);
            break;
        }
        if (g_redirect) RedirectSteamClientReg();
        HANDLE proc = StartUnderConPty(exe, cmd, dir, &hpc, &reader, &pid);
        if (proc) { pi.hProcess = proc; pi.dwProcessId = pid; Log(L"Server console is shown here; type commands in the box above."); }
        else {
            if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, dir, &si, &pi)) {
                Log(L"ERROR: cannot start cs2.exe (%lu)", GetLastError()); break;
            }
            CloseHandle(pi.hThread);
        }
        g_proc = pi.hProcess;
        InterlockedExchange(&g_running, 1); PostMessageW(g_wnd, WM_APP_STATE, 0, 0);
        Log(L"Server running (pid %lu), %d tick, mode %s.", pi.dwProcessId, c.tick, MODES[c.mode]);
        LogConnInfo(&c);
        {   /* RCON target, console.log follower, window embedding */
            EnterCriticalSection(&g_rconLock); RconClose(); g_rconPort = c.port;
            WideCharToMultiByte(CP_UTF8, 0, c.rcon[0] ? c.rcon : g_autoRcon, -1, g_rconPwA, sizeof g_rconPwA, NULL, NULL);
            InterlockedExchange(&g_rconOk, 0); LeaveCriticalSection(&g_rconLock);
            WCHAR *lp = (WCHAR *)malloc(MAX_PATH * sizeof(WCHAR)); ConLogPath(&c, 0, lp);
            InterlockedExchange(&g_tailRun, 1);
            HANDLE tt = CreateThread(NULL, 0, TailThread, lp, 0, NULL); if (tt) CloseHandle(tt); else free(lp);
            PostMessageW(g_wnd, WM_APP_EMBED, pi.dwProcessId, 0);
        }
        if (g_redirect) {
            WaitForSingleObject(pi.hProcess, 30000);      /* Steam is initialised within a few seconds */
            RestoreSteamClientReg();
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD ec = 0; GetExitCodeProcess(pi.hProcess, &ec);
        CloseHandle(pi.hProcess); g_proc = NULL;
        InterlockedExchange(&g_tailRun, 0);
        EnterCriticalSection(&g_rconLock); RconClose(); LeaveCriticalSection(&g_rconLock);
        Sleep(300);
        EndConPty(hpc, reader);
        if (g_stopReq) { Log(L"Server stopped."); break; }
        Log(L"Server exited unexpectedly after %.1f s: code 0x%08lx (%lu) = %s.", (GetTickCount() - t0) / 1000.0, ec, ec, ExitMeaning(ec));
        PostExitDiagnostics(&c, ec, started, GetTickCount() - t0);
        DWORD now = GetTickCount();
        crashes[nc++ % 5] = now;
        int recent = 0; for (int i = 0; i < 5; i++) if (crashes[i] && now - crashes[i] < 180000) recent++;
        if (recent >= 3) {
            Log(L"Exited %d times in 3 minutes - not restarting.", recent);
            Log(L"Full log of this start: %s", g_sessLog);
            Log(L"To find the cause: tick \"Debug window\" and Start - CS2's own message stays on screen. Or try:");
            Log(L"  1. Tick 64 + \"Run without plugins\"  -> tests the plain server");
            if (c.tick > 64) Log(L"  2. Tick %d + \"Run without plugins\" -> tests the tick patch", c.tick);
            if (!c.noplug && HasCss(c.root)) Log(L"  3. Tick 64 with plugins            -> tests CounterStrikeSharp (v1.0.380 is known to crash CS2 1.42.0.1)");
            break;
        }
        Log(L"Restarting in 5 s...");
        Sleep(5000);
        if (g_stopReq) break;
    }
    InterlockedExchange(&g_running, 0); InterlockedExchange(&g_stopReq, 0);
    PostMessageW(g_wnd, WM_APP_STATE, 0, 0);
    return 0;
}

static DWORD WINAPI Worker(LPVOID p)
{
    Job j = *(Job *)p; free(p);
    switch (j.k) {
    case J_INSTALL:  DoInstall(&j.c); break;
    case J_PLUGINS:
        if (g_running) { Log(L"Stop the server first."); break; }
        if (!HasServer(j.c.root)) { Log(L"Install the server first."); break; }
        Log(L"Installing plugins (Metamod, CounterStrikeSharp, MatchZy 0.9.3, Retakes 3.1.2, InstaDefuse 2.0.0)...");
        SetProgress(-1, L"Installing plugins...");
        if (RunPs(L"install-plugins", j.c.root, NULL) == 0) { SetProgress(1000, L"Plugins installed.");  Log(L"Plugins installed. Checking versions:"); RunPs(L"check", j.c.root, NULL); }
        else if (g_cancelReq) { Log(L"Plugin install cancelled."); SetProgress(0, L"Cancelled."); }
        else { Log(L"ERROR: plugin install failed (see above)."); SetProgress(0, L"Plugin install failed."); }
        break;
    case J_CHECK:
        Log(L"Checking versions...");
        RunPs(L"check", j.c.root, NULL);
        if (HasServer(j.c.root)) { Log(L"Patch state:"); tp_status(j.c.root, Log); }
        break;
    case J_FIREWALL: {
        WCHAR ps[MAX_PATH]; GetSystemDirectoryW(ps, MAX_PATH); PathAppendW(ps, L"WindowsPowerShell\\v1.0\\powershell.exe");
        WCHAR args[2048]; _snwprintf(args, 2047, L"-NoProfile -ExecutionPolicy Bypass -File \"%s\" firewall \"%s\"", g_ps1, j.c.root);
        SHELLEXECUTEINFOW se = { sizeof se }; se.fMask = SEE_MASK_NOCLOSEPROCESS; se.lpVerb = L"runas";
        se.lpFile = ps; se.lpParameters = args; se.nShow = SW_SHOWNORMAL;
        Log(L"Fixing firewall (asks for administrator)...");
        if (ShellExecuteExW(&se) && se.hProcess) {
            WaitForSingleObject(se.hProcess, INFINITE);
            DWORD ec = 1; GetExitCodeProcess(se.hProcess, &ec); CloseHandle(se.hProcess);
            Log(ec == 0 ? L"Firewall fixed: Block rules for cs2.exe removed, UDP/TCP 27015 allowed." : L"Firewall fix failed (see its window).");
        } else Log(L"Firewall fix cancelled.");
        break;
    }
    case J_START: {
        if (g_running) { Log(L"Server already running."); break; }
        {
            SYSTEMTIME st; GetLocalTime(&st);
            _snwprintf(g_sessLog, MAX_PATH, L"%s\\server_%04d%02d%02d_%02d%02d%02d.log", g_logDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            Log(L"Session log: %s", g_sessLog);
            Log(L"Settings: folder %s | mode %s | %d tick | port %d | %s | plugins %s%s", j.c.root, MODES[j.c.mode], j.c.tick, j.c.port,
                j.c.lan ? L"LAN" : L"Internet", j.c.noplug ? L"off" : L"on", j.c.debug ? L" | debug window" : L"");
        }
        if (!PrepareStart(&j.c)) break;
        Cfg *c = (Cfg *)malloc(sizeof *c); *c = j.c;
        InterlockedExchange(&g_stopReq, 0);
        HANDLE t = CreateThread(NULL, 0, Monitor, c, 0, NULL); if (t) CloseHandle(t); else free(c);
        break;
    }
    case J_RESTORE:
        if (g_running) { Log(L"Stop the server first."); break; }
        Log(L"Restoring 64 tick:");
        if (tp_restore(j.c.root, Log)) Log(L"Server DLLs are 64 tick.");
        break;
    }
    PostMessageW(g_wnd, WM_APP_DONE, 0, 0);
    return 0;
}

static void UpdateButtons(void)
{
    BOOL busy = g_busy, run = g_running;
    EnableWindow(g_btn[0], !busy && !run); EnableWindow(g_btn[1], !busy && !run); EnableWindow(g_btn[2], !busy);
    EnableWindow(g_btn[3], !busy); EnableWindow(g_btn[4], !busy && !run); EnableWindow(g_btn[5], run);
    EnableWindow(g_btn[6], !busy && !run);
    if (g_pause) { EnableWindow(g_pause, busy); EnableWindow(g_cancel, busy); }
    if (!busy && g_pause) {
        g_paused = 0; SetWindowTextW(g_pause, L"Pause"); SendMessageW(g_prog, PBM_SETSTATE, PBST_NORMAL, 0);
    }
}

static void StartJob(JobKind k)
{
    if (g_busy) return;
    Job *j = (Job *)malloc(sizeof *j); j->k = k; ReadUi(&j->c); SaveCfg(&j->c);
    InterlockedExchange(&g_cancelReq, 0); InterlockedExchange(&g_paused, 0);
    if (k == J_START && !j->c.lan && !j->c.gslt[0])
        Log(L"Note: Internet mode without a GSLT - the server will not show up on the internet list.");
    InterlockedExchange(&g_busy, 1); UpdateButtons();
    HANDLE t = CreateThread(NULL, 0, Worker, j, 0, NULL); if (t) CloseHandle(t); else { free(j); g_busy = 0; UpdateButtons(); }
}

static void StopServer(void)
{
    if (!g_running) return;
    InterlockedExchange(&g_stopReq, 1);
    HANDLE p = g_proc;
    if (p) { Log(L"Stopping server..."); TerminateProcess(p, 0); }
}

static void Browse(HWND target, const WCHAR *title)
{
    BROWSEINFOW bi = { 0 }; bi.hwndOwner = g_wnd; bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pid = SHBrowseForFolderW(&bi);
    if (pid) { WCHAR p[MAX_PATH]; if (SHGetPathFromIDListW(pid, p)) SetWindowTextW(target, p); CoTaskMemFree(pid); }
}


/* ================================================================ players / bans / match */
typedef struct { int userid; WCHAR name[64], steamid[24], addr[48], time[16]; int ping, bot; } Player;
static Player g_pl[128]; static int g_npl = 0;
static volatile LONG g_polling = 0;
static WCHAR g_bansFile[MAX_PATH];
static HWND g_con, g_plist, g_pstat2, g_t1name, g_t1, g_t2name, g_t2, g_ppt, g_series, g_sides, g_maps, g_extra, g_mstat, g_mapmode, g_ms[5], g_chmap;
static const WCHAR *POOL[] = { L"de_ancient", L"de_anubis", L"de_dust2", L"de_inferno", L"de_mirage", L"de_nuke", L"de_overpass", L"de_train", L"de_vertigo" };
#define NPOOL 9

static BOOL SendRaw(const char *cmd)
{
    char *o = RconExec(cmd);
    if (o) { if (*o) ConsoleText(o); free(o); return TRUE; }
    if (!g_running || !g_srvIn) return FALSE;
    char u[1100]; int n = snprintf(u, sizeof u - 1, "%s\r", cmd);
    DWORD wr; return WriteFile(g_srvIn, u, (DWORD)n, &wr, NULL);
}
static BOOL SendRawW(const WCHAR *cmd)
{
    char u[1100]; WideCharToMultiByte(CP_UTF8, 0, cmd, -1, u, sizeof u, NULL, NULL);
    WCHAR shown[1200]; _snwprintf(shown, 1200, L"> %s", cmd); Log(L"%s", shown);
    return SendRaw(u);
}

/* run a console command and collect its output (mode 1 = until JSON closes, 2 = until #end) */
static char *Capture(const char *cmd, int mode, DWORD ms)
{
    char *o = RconExec(cmd);
    if (o) { if (*o) return o; free(o); return NULL; }
    if (!g_srvIn) return NULL;
    EnterCriticalSection(&g_capLock);
    g_capLen = 0; if (g_cap) g_cap[0] = 0; g_capDepth = 0; g_capSeen = 0;
    ResetEvent(g_capDone); InterlockedExchange(&g_capOn, mode);
    LeaveCriticalSection(&g_capLock);
    SendRaw(cmd);
    WaitForSingleObject(g_capDone, ms);
    Sleep(100);
    EnterCriticalSection(&g_capLock);
    InterlockedExchange(&g_capOn, 0);
    char *r = g_cap && g_capLen ? _strdup(g_cap) : NULL;
    LeaveCriticalSection(&g_capLock);
    return r;
}

/* tiny JSON helpers - tolerant lookups inside one object's text */
static BOOL JGet(const char *obj, const char *end, const char *key, char *out, size_t n)
{
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *q = obj;
    for (;;) {
        q = strstr(q, pat); if (!q || q >= end) return FALSE;
        const char *v = q + strlen(pat); while (v < end && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
        if (v < end && *v == ':') { v++; while (v < end && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
            size_t k = 0;
            if (*v == '"') { v++; while (v < end && *v != '"' && k + 1 < n) { if (*v == '\\' && v + 1 < end) v++; if (*v != '\n') out[k++] = *v; v++; } }
            else { while (v < end && *v != ',' && *v != '}' && *v != ']' && *v != '\n' && k + 1 < n) out[k++] = *v++; while (k && out[k-1] == ' ') k--; }
            out[k] = 0; return TRUE; }
        q++;
    }
}
static BOOL JAny(const char *o, const char *e, const char *const *keys, char *out, size_t n)
{ for (int i = 0; keys[i]; i++) if (JGet(o, e, keys[i], out, n) && out[0]) return TRUE; out[0] = 0; return FALSE; }

static int ParseStatusJson(const char *s, Player *pl, int max)
{
    const char *k = strstr(s, "\"clients\""); if (!k) k = strstr(s, "\"players\""); if (!k) return -1;
    const char *a = strchr(k, '['); if (!a) return -1;
    int n = 0; const char *q = a + 1;
    static const char *NAME[] = { "name", "player_name", NULL }, *SID[] = { "steamid64", "steamid", "xuid", "SteamID64", "steam_id", NULL },
        *UID[] = { "userid", "user_id", "id", "slot", NULL }, *PING[] = { "ping", NULL }, *TIME[] = { "time", "connected", "duration", NULL },
        *ADDR[] = { "addr", "address", "ip", NULL }, *BOT[] = { "bot", "fakeplayer", "is_bot", NULL };
    while (n < max) {
        while (*q && *q != '{' && *q != ']') q++;
        if (*q != '{') break;
        int d = 0; const char *e = q; BOOL str = FALSE;
        for (; *e; e++) { if (*e == '"' && e[-1] != '\\') str = !str; if (str) continue; if (*e == '{') d++; else if (*e == '}' && --d == 0) break; }
        if (!*e) break;
        char v[128]; Player *p = &pl[n]; ZeroMemory(p, sizeof *p); p->userid = -1;
        if (JAny(q, e, NAME, v, sizeof v)) MultiByteToWideChar(CP_UTF8, 0, v, -1, p->name, 64);
        if (JAny(q, e, SID, v, sizeof v)) MultiByteToWideChar(CP_UTF8, 0, v, -1, p->steamid, 24);
        if (JAny(q, e, UID, v, sizeof v)) p->userid = atoi(v);
        if (JAny(q, e, PING, v, sizeof v)) p->ping = atoi(v);
        if (JAny(q, e, TIME, v, sizeof v)) MultiByteToWideChar(CP_UTF8, 0, v, -1, p->time, 16);
        if (JAny(q, e, ADDR, v, sizeof v)) MultiByteToWideChar(CP_UTF8, 0, v, -1, p->addr, 48);
        if (JAny(q, e, BOT, v, sizeof v)) p->bot = (v[0] == 't' || v[0] == '1');
        if (!wcscmp(p->steamid, L"0")) p->steamid[0] = 0;
        if (p->name[0] || p->steamid[0]) n++;
        q = e + 1;
    }
    return n;
}

/* text status:  "     2    00:15   15    0     active 786432 192.168.1.5:27005 'Casio'" */
static int ParseStatusText(const char *s, Player *pl, int max)
{
    const char *q = strstr(s, "---------players--------"); if (!q) return -1;
    int n = 0;
    for (q = strchr(q, '\n'); q && *q && n < max; q = strchr(q + 1, '\n')) {
        const char *ln = q + 1; if (!strncmp(ln, "#end", 4)) break;
        int id, ping, loss, rate; char tm[16], state[32], adr[64];
        if (sscanf(ln, "%d %15s %d %d %31s %d %63s", &id, tm, &ping, &loss, state, &rate, adr) < 7) continue;
        if (id == 65535) continue;
        const char *a = strchr(ln, '\''), *b = a ? strchr(a + 1, '\'') : NULL;
        const char *eol = strchr(ln, '\n'); if (b && eol && b > eol) b = NULL;
        Player *p = &pl[n]; ZeroMemory(p, sizeof *p);
        p->userid = id; p->ping = ping;
        MultiByteToWideChar(CP_UTF8, 0, tm, -1, p->time, 16);
        MultiByteToWideChar(CP_UTF8, 0, adr, -1, p->addr, 48);
        if (a && b) { char nm[128]; size_t L = b - a - 1; if (L > 127) L = 127; memcpy(nm, a + 1, L); nm[L] = 0; MultiByteToWideChar(CP_UTF8, 0, nm, -1, p->name, 64); }
        p->bot = !strcmp(adr, "BOT") || !strncmp(adr, "loopback", 8) ? (strstr(ln, "BOT") != NULL) : 0;
        n++;
    }
    return n;
}

static BOOL IsBanned(const Player *p, WCHAR *why, int n)
{
    FILE *f = _wfopen(g_bansFile, L"r, ccs=UTF-8"); if (!f) return FALSE;
    WCHAR line[512]; BOOL hit = FALSE;
    WCHAR ip[48]; wcscpy(ip, p->addr); WCHAR *c = wcschr(ip, L':'); if (c) *c = 0;
    while (!hit && fgetws(line, 512, f)) {
        line[wcscspn(line, L"\r\n")] = 0; if (!line[0] || line[0] == L'#') continue;
        WCHAR *sid = line, *nm = wcschr(line, L'|'); WCHAR *bip = NULL;
        if (nm) { *nm++ = 0; bip = wcschr(nm, L'|'); if (bip) *bip++ = 0; }
        if ((sid[0] && p->steamid[0] && !wcscmp(sid, p->steamid)) || (bip && bip[0] && ip[0] && !wcscmp(bip, ip))) { hit = TRUE; _snwprintf(why, n, L"%s", nm ? nm : sid); }
    }
    fclose(f); return hit;
}

static void KickPlayer(const Player *p, const WCHAR *reason)
{
    WCHAR cmd[300];
    if (p->userid >= 0) _snwprintf(cmd, 300, L"kickid %d \"%s\"", p->userid, reason);
    else _snwprintf(cmd, 300, L"kick \"%s\"", p->name);
    SendRawW(cmd);
}

typedef struct { int n; Player p[128]; WCHAR how[64]; } PlayerMsg;
static DWORD WINAPI PollThread(LPVOID unused)
{
    (void)unused;
    PlayerMsg *m = (PlayerMsg *)calloc(1, sizeof *m);
    if (m && g_running) {
        char *s = Capture("status_json", 1, 2500);
        int n = s ? ParseStatusJson(s, m->p, 128) : -1;
        if (n >= 0) wcscpy(m->how, L"status_json");
        free(s);
        if (n < 0) { s = Capture("status", 2, 2500); n = s ? ParseStatusText(s, m->p, 128) : -1; if (n >= 0) wcscpy(m->how, L"status (no SteamIDs)"); free(s); }
        m->n = n;
        for (int i = 0; i < n; i++) { WCHAR why[128];
            if (!m->p[i].bot && IsBanned(&m->p[i], why, 128)) { Log(L"Banned player %s on the server - kicking.", m->p[i].name); KickPlayer(&m->p[i], L"You are banned from this server"); } }
        if (!PostMessageW(g_wnd, WM_APP_PLAYERS, 0, (LPARAM)m)) free(m);
    } else free(m);
    InterlockedExchange(&g_polling, 0);
    return 0;
}
static void PollPlayers(void)
{
    if (!g_running) { SetWindowTextW(g_pstat2, L"Server is not running."); return; }
    if (InterlockedExchange(&g_polling, 1)) return;
    HANDLE t = CreateThread(NULL, 0, PollThread, NULL, 0, NULL); if (t) CloseHandle(t); else g_polling = 0;
}

static int SelectedPlayer(void)
{
    int i = (int)SendMessageW(g_plist, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (i < 0 || i >= g_npl) { MessageBoxW(g_wnd, L"Select a player in the list first.", APP_TITLE, MB_ICONINFORMATION); return -1; }
    return i;
}

static void FillPlayers(PlayerMsg *m)
{
    SendMessageW(g_plist, LVM_DELETEALLITEMS, 0, 0);
    if (m->n < 0) { SetWindowTextW(g_pstat2, L"Could not read the player list (no status output) - see the Console tab."); g_npl = 0; return; }
    g_npl = m->n; memcpy(g_pl, m->p, sizeof(Player) * m->n);
    for (int i = 0; i < g_npl; i++) {
        Player *p = &g_pl[i]; WCHAR b[32];
        LVITEMW it = { 0 }; it.mask = LVIF_TEXT; it.iItem = i;
        _snwprintf(b, 32, p->userid >= 0 ? L"%d" : L"-", p->userid); it.pszText = b;
        SendMessageW(g_plist, LVM_INSERTITEMW, 0, (LPARAM)&it);
        WCHAR ping[16]; _snwprintf(ping, 16, L"%d", p->ping);
        const WCHAR *cols[5] = { p->name, p->bot ? L"BOT" : (p->steamid[0] ? p->steamid : L"?"), ping, p->time, p->addr };
        for (int k = 0; k < 5; k++) { it.iSubItem = k + 1; it.pszText = (WCHAR *)cols[k]; SendMessageW(g_plist, LVM_SETITEMTEXTW, i, (LPARAM)&it); }
    }
    SYSTEMTIME st; GetLocalTime(&st); WCHAR t[160];
    _snwprintf(t, 160, L"%d player(s) - updated %02d:%02d:%02d via %s (refreshes every 15 s)", g_npl, st.wHour, st.wMinute, st.wSecond, m->how);
    SetWindowTextW(g_pstat2, t);
}

static void BanSelected(void)
{
    int i = SelectedPlayer(); if (i < 0) return;
    Player *p = &g_pl[i];
    WCHAR q[300]; _snwprintf(q, 300, L"Ban %s%s%s and kick them now?\n\nBans are kept in bans.txt and enforced by the launcher.", p->name, p->steamid[0] ? L"  " : L"", p->steamid);
    if (MessageBoxW(g_wnd, q, APP_TITLE, MB_YESNO | MB_ICONWARNING) != IDYES) return;
    WCHAR ip[48]; wcscpy(ip, p->addr); WCHAR *c = wcschr(ip, L':'); if (c) *c = 0;
    if (!wcsncmp(ip, L"loopback", 8)) ip[0] = 0;
    FILE *f = _wfopen(g_bansFile, L"a, ccs=UTF-8");
    if (f) { fwprintf(f, L"%s|%s|%s\n", p->steamid, p->name, ip); fclose(f); }
    Log(L"Banned %s (%s, %s).", p->name, p->steamid[0] ? p->steamid : L"no SteamID", ip[0] ? ip : L"no IP");
    KickPlayer(p, L"You are banned from this server");
    Sleep(300); PollPlayers();
}

static void OpenBans(void)
{
    if (GetFileAttributesW(g_bansFile) == INVALID_FILE_ATTRIBUTES) {
        FILE *f = _wfopen(g_bansFile, L"w, ccs=UTF-8");
        if (f) { fwprintf(f, L"# CS2 Server Launcher bans: steamid64|name|ip   (delete a line to unban)\n"); fclose(f); }
    }
    ShellExecuteW(g_wnd, L"open", L"notepad.exe", g_bansFile, NULL, SW_SHOWNORMAL);
}

static void AppendLine(HWND e, const WCHAR *s)
{
    int n = GetWindowTextLengthW(e); SendMessageW(e, EM_SETSEL, n, n);
    if (n) { WCHAR last[2]; WCHAR *all = (WCHAR *)malloc((n + 1) * sizeof(WCHAR)); GetWindowTextW(e, all, n + 1);
        BOOL nl = all[n - 1] == L'\n'; free(all); if (!nl) SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n"); (void)last; }
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)s);
}

static void PlayerToTeam(HWND team)
{
    int i = SelectedPlayer(); if (i < 0) return;
    Player *p = &g_pl[i];
    if (!p->steamid[0]) { MessageBoxW(g_wnd, L"No SteamID for this player (the server's status output had none). Paste their Steam profile link instead.", APP_TITLE, MB_ICONINFORMATION); return; }
    WCHAR line[128]; _snwprintf(line, 128, L"%s %s", p->steamid, p->name);
    AppendLine(team, line);
}

/* ---- match ---- */
static void JsonStr(FILE *f, const WCHAR *s)
{
    char u[512]; WideCharToMultiByte(CP_UTF8, 0, s, -1, u, sizeof u, NULL, NULL);
    fputc('"', f);
    for (char *q = u; *q; q++) { if (*q == '"' || *q == '\\') { fputc('\\', f); fputc(*q, f); } else if ((unsigned char)*q < 0x20) fputc(' ', f); else fputc(*q, f); }
    fputc('"', f);
}

/* parses a team box: each line "7656xxxxxxxxxxxxx Name". Returns count, -1 on an unresolved line. */
static int TeamPlayers(HWND e, WCHAR ids[][24], WCHAR names[][64], int max, WCHAR *bad, int badn)
{
    int L = GetWindowTextLengthW(e); WCHAR *t = (WCHAR *)calloc(L + 2, sizeof(WCHAR)); GetWindowTextW(e, t, L + 1);
    int n = 0; WCHAR *ctx = NULL;
    for (WCHAR *ln = wcstok_s(t, L"\r\n", &ctx); ln; ln = wcstok_s(NULL, L"\r\n", &ctx)) {
        while (*ln == L' ' || *ln == L'\t') ln++; if (!*ln) continue;
        WCHAR *sp = ln; while (*sp && *sp != L' ' && *sp != L'\t') sp++;
        int idl = (int)(sp - ln);
        BOOL ok = idl == 17 && !wcsncmp(ln, L"7656", 4);
        for (int k = 0; ok && k < 17; k++) if (ln[k] < L'0' || ln[k] > L'9') ok = FALSE;
        if (!ok) { _snwprintf(bad, badn, L"%s", ln); free(t); return -1; }
        if (n >= max) break;
        wcsncpy(ids[n], ln, 17); ids[n][17] = 0;
        while (*sp == L' ' || *sp == L'\t') sp++;
        _snwprintf(names[n], 64, L"%s", *sp ? sp : ids[n]);
        n++;
    }
    free(t); return n;
}

static void LoadMatch(void)
{
    Cfg c; ReadUi(&c);
    if (!g_running) { MessageBoxW(g_wnd, L"Start the server first.", APP_TITLE, MB_ICONINFORMATION); return; }
    if (c.mode > 2 || c.noplug || !HasCss(c.root)) { MessageBoxW(g_wnd, L"Matches need MatchZy: Game mode Competitive or Wingman, plugins installed and \"Run without plugins\" unticked. Change it and restart the server.", APP_TITLE, MB_ICONWARNING); return; }
    WCHAR id1[16][24], nm1[16][64], id2[16][24], nm2[16][64], bad[200];
    int n1 = TeamPlayers(g_t1, id1, nm1, 16, bad, 200);
    int n2 = n1 < 0 ? 0 : TeamPlayers(g_t2, id2, nm2, 16, bad, 200);
    if (n1 < 0 || n2 < 0) { WCHAR m[400]; _snwprintf(m, 400, L"Not a resolved player line:\n\n%s\n\nClick \"Resolve Steam profiles\" first (each line becomes SteamID64 + name).", bad); MessageBoxW(g_wnd, m, APP_TITLE, MB_ICONWARNING); return; }
    WCHAR b[16]; GetWindowTextW(g_ppt, b, 16); int ppt = _wtoi(b); if (ppt <= 0) ppt = c.mode == 1 ? 2 : 5;
    if (n1 > ppt || n2 > ppt) { MessageBoxW(g_wnd, L"A team has more players than \"Players per team\".", APP_TITLE, MB_ICONWARNING); return; }
    static const int SER[4] = { 1, 2, 3, 5 }; int nm = SER[max(0, (int)SendMessageW(g_series, CB_GETCURSEL, 0, 0))];
    static const WCHAR *SIDE[3] = { L"knife", L"team1_ct", L"team2_ct" }; int sd = max(0, (int)SendMessageW(g_sides, CB_GETCURSEL, 0, 0));
    WCHAR maps[32][64]; int nmaps = 0;
    if (SendMessageW(g_mapmode, CB_GETCURSEL, 0, 0) == 0) {
        for (int i = 0; i < nm; i++) {
            GetWindowTextW(g_ms[i], maps[nmaps], 64);
            if (!maps[nmaps][0]) { WCHAR m[100]; _snwprintf(m, 100, L"Choose Map %d.", i + 1); MessageBoxW(g_wnd, m, APP_TITLE, MB_ICONWARNING); return; }
            for (int k = 0; k < nmaps; k++) if (!_wcsicmp(maps[k], maps[nmaps])) { MessageBoxW(g_wnd, L"The same map is picked twice.", APP_TITLE, MB_ICONWARNING); return; }
            nmaps++;
        }
    } else {
        for (int i = 0; i < NPOOL; i++) if (SendMessageW(g_maps, LVM_GETITEMSTATE, i, LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2)) wcscpy(maps[nmaps++], POOL[i]);
        { WCHAR ex[512]; GetWindowTextW(g_extra, ex, 512); WCHAR *ctx = NULL;
          for (WCHAR *t = wcstok_s(ex, L", ;", &ctx); t && nmaps < 32; t = wcstok_s(NULL, L", ;", &ctx)) _snwprintf(maps[nmaps++], 64, L"%s", t); }
        if (nmaps == nm) Log(L"Veto pool has exactly %d map(s) - they are played in that order.", nm);
    }
    if (nmaps < nm) { WCHAR m[200]; _snwprintf(m, 200, L"Pick at least %d map(s) for a Bo%d.", nm, nm); MessageBoxW(g_wnd, m, APP_TITLE, MB_ICONWARNING); return; }
    WCHAR tn1[64], tn2[64]; GetWindowTextW(g_t1name, tn1, 64); GetWindowTextW(g_t2name, tn2, 64);
    if (!tn1[0]) wcscpy(tn1, L"Team 1"); if (!tn2[0]) wcscpy(tn2, L"Team 2");
    WCHAR path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%s\\game\\csgo\\cs2launcher_match.json", c.root);
    FILE *f = _wfopen(path, L"wb"); if (!f) { Log(L"ERROR: cannot write %s", path); return; }
    fprintf(f, "{\n  \"matchid\": %lld,\n  \"num_maps\": %d,\n  \"players_per_team\": %d,\n  \"min_players_to_ready\": %d,\n  \"clinch_series\": true,\n  \"skip_veto\": %s,\n",
            (long long)time(NULL), nm, ppt, ppt, nmaps == nm ? "true" : "false");
    fprintf(f, "  \"maplist\": ["); for (int i = 0; i < nmaps; i++) { if (i) fputs(", ", f); JsonStr(f, maps[i]); } fputs("],\n", f);
    fprintf(f, "  \"map_sides\": ["); for (int i = 0; i < nm; i++) { if (i) fputs(", ", f); JsonStr(f, SIDE[sd]); } fputs("],\n", f);
    for (int t = 0; t < 2; t++) {
        fprintf(f, "  \"team%d\": {\n    \"name\": ", t + 1); JsonStr(f, t ? tn2 : tn1); fputs(",\n    \"players\": {", f);
        int n = t ? n2 : n1;
        for (int i = 0; i < n; i++) { if (i) fputs(",", f); fputs("\n      ", f); JsonStr(f, t ? id2[i] : id1[i]); fputs(": ", f); JsonStr(f, t ? nm2[i] : nm1[i]); }
        fputs(n ? "\n    }\n  },\n" : "}\n  },\n", f);
    }
    fputs("  \"spectators\": { \"players\": {} }\n}\n", f);
    fclose(f);
    Log(L"Match file written: %s (%s vs %s, Bo%d, %d map(s) in pool%s)", path, tn1, tn2, nm, nmaps, nmaps > nm ? L", in-game veto" : L"");
    SendRawW(L"matchzy_loadmatch cs2launcher_match.json");
    WCHAR st[200]; _snwprintf(st, 200, L"Match sent to MatchZy: %s vs %s, Bo%d%s. Watch the Console tab.", tn1, tn2, nm, nmaps > nm ? L" with veto" : L"");
    SetWindowTextW(g_mstat, st);
}

/* resolve Steam profiles in both team boxes (worker thread via the helper script) */
typedef struct { WCHAR *t1, *t2; } Resolved;
static WCHAR *g_resIn[2];
static DWORD WINAPI ResolveThread(LPVOID unused)
{
    (void)unused;
    WCHAR in[MAX_PATH], out[MAX_PATH]; _snwprintf(in, MAX_PATH, L"%s\\resolve_in.txt", g_dataDir); _snwprintf(out, MAX_PATH, L"%s\\resolve_out.txt", g_dataDir);
    FILE *f = _wfopen(in, L"wb");
    if (f) {
        for (int t = 0; t < 2; t++) {
            WCHAR *ctx = NULL;
            for (WCHAR *ln = wcstok_s(g_resIn[t], L"\r\n", &ctx); ln; ln = wcstok_s(NULL, L"\r\n", &ctx)) {
                char u[600]; WideCharToMultiByte(CP_UTF8, 0, ln, -1, u, sizeof u, NULL, NULL); fprintf(f, "%d\t%s\n", t + 1, u);
            }
        }
        fclose(f);
    }
    DeleteFileW(out);
    Log(L"Resolving Steam profiles...");
    RunPs(L"resolve", in, out);
    Resolved *r = (Resolved *)calloc(1, sizeof *r);
    size_t cap = 16384; r->t1 = (WCHAR *)calloc(cap, sizeof(WCHAR)); r->t2 = (WCHAR *)calloc(cap, sizeof(WCHAR));
    size_t n; uint8_t *b = tp_read(out, &n, NULL);
    if (b) {
        b[n] = 0; char *ctx = NULL;
        for (char *ln = strtok_s((char *)b, "\r\n", &ctx); ln; ln = strtok_s(NULL, "\r\n", &ctx)) {
            WCHAR w[700]; MultiByteToWideChar(CP_UTF8, 0, ln, -1, w, 700);
            WCHAR *team = w, *a = wcschr(w, L'\t'); if (!a) continue; *a++ = 0;
            WCHAR *bb = wcschr(a, L'\t'); if (!bb) continue; *bb++ = 0;
            WCHAR *dst = team[0] == L'2' ? r->t2 : r->t1, line[400];
            if (!wcscmp(a, L"ERR")) { WCHAR *msg = wcschr(bb, L'\t'); if (msg) *msg++ = 0; _snwprintf(line, 400, L"%s   <-- %s\r\n", bb, msg ? msg : L"failed"); }
            else _snwprintf(line, 400, L"%s %s\r\n", a, bb);
            if (wcslen(dst) + wcslen(line) < cap - 1) wcscat(dst, line);
        }
        free(b);
    } else Log(L"ERROR: profile lookup produced no result.");
    free(g_resIn[0]); free(g_resIn[1]); g_resIn[0] = g_resIn[1] = NULL;
    if (!PostMessageW(g_wnd, WM_APP_RESOLVED, 0, (LPARAM)r)) { free(r->t1); free(r->t2); free(r); }
    return 0;
}
static void ResolveProfiles(void)
{
    if (g_resIn[0]) return;
    for (int t = 0; t < 2; t++) { HWND e = t ? g_t2 : g_t1; int L = GetWindowTextLengthW(e); g_resIn[t] = (WCHAR *)calloc(L + 2, sizeof(WCHAR)); GetWindowTextW(e, g_resIn[t], L + 1);
        WCHAR *mark = wcsstr(g_resIn[t], L"   <--"); while (mark) { WCHAR *eol = wcspbrk(mark, L"\r\n"); if (eol) memmove(mark, eol, (wcslen(eol) + 1) * sizeof(WCHAR)); else *mark = 0; mark = wcsstr(g_resIn[t], L"   <--"); } }
    SetWindowTextW(g_mstat, L"Resolving Steam profiles...");
    HANDLE th = CreateThread(NULL, 0, ResolveThread, NULL, 0, NULL); if (th) CloseHandle(th);
}

/* ---------------------------------------------------------------- UI */
static HFONT g_font, g_mono;
static HWND g_tab;
static HWND g_page[7][80]; static int g_npage[7], g_curPage = 0;
static HWND Mk(HWND h, const WCHAR *cls, const WCHAR *txt, DWORD st, DWORD ex, int x, int y, int w, int hh, int id)
{
    HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | st, x, y, w, hh, h, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (g_npage[g_curPage] < 80) g_page[g_curPage][g_npage[g_curPage]++] = c;
    return c;
}
#define LBL(t, x, y, w) Mk(h, L"STATIC", t, 0, 0, x, y + 3, w, 18, 0)
#define EDT(id, x, y, w, st) Mk(h, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | (st), WS_EX_CLIENTEDGE, x, y, w, 22, id)

/* ---------------------------------------------------------------- connection info */
static HWND g_cLocal, g_cExt, g_cPort, g_cPass, g_cShowPw, g_cCmdL, g_cCmdE, g_cLinkL, g_cLinkE, g_cNote, g_cRcon;
static WCHAR g_extIp[64] = L"";
static volatile LONG g_extBusy = 0;

static int LocalIPv4(WCHAR out[][48], int max)
{
    ULONG sz = 32768; IP_ADAPTER_ADDRESSES *a = (IP_ADAPTER_ADDRESSES *)malloc(sz); int n = 0;
    if (a && GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, a, &sz) == ERROR_BUFFER_OVERFLOW) {
        free(a); a = (IP_ADAPTER_ADDRESSES *)malloc(sz);
        if (a && GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, a, &sz) != NO_ERROR) { free(a); a = NULL; }
    } else if (a && GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, a, &sz) != NO_ERROR) { free(a); a = NULL; }
    for (IP_ADAPTER_ADDRESSES *p = a; p && n < max; p = p->Next) {
        if (p->OperStatus != IfOperStatusUp || p->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        /* adapters with a default gateway first: those are the real LAN */
        for (IP_ADAPTER_UNICAST_ADDRESS *u = p->FirstUnicastAddress; u && n < max; u = u->Next) {
            struct sockaddr_in *s = (struct sockaddr_in *)u->Address.lpSockaddr;
            BYTE *b = (BYTE *)&s->sin_addr;
            if (b[0] == 169 && b[1] == 254) continue;               /* link-local */
            _snwprintf(out[n], 48, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            if (p->FirstGatewayAddress && n > 0) { WCHAR t[48]; wcscpy(t, out[n]); memmove(out[1], out[0], sizeof(out[0]) * n); wcscpy(out[0], t); }
            n++;
        }
    }
    free(a); return n;
}

static DWORD WINAPI ExtIpThread(LPVOID unused)
{
    (void)unused;
    static const WCHAR *urls[3] = { L"https://api.ipify.org", L"https://icanhazip.com", L"https://checkip.amazonaws.com" };
    WCHAR tmp[MAX_PATH]; _snwprintf(tmp, MAX_PATH, L"%s\\extip.txt", g_dataDir);
    WCHAR *res = (WCHAR *)calloc(64, sizeof(WCHAR));
    for (int i = 0; i < 3 && res && !res[0]; i++) {
        DeleteUrlCacheEntryW(urls[i]);
        if (FAILED(URLDownloadToFileW(NULL, urls[i], tmp, 0, NULL))) continue;
        size_t n; uint8_t *b = tp_read(tmp, &n, NULL); if (!b) continue; b[n] = 0;
        int a, bb, cc, d; char ip[64];
        if (sscanf((char *)b, " %63s", ip) == 1 && sscanf(ip, "%d.%d.%d.%d", &a, &bb, &cc, &d) == 4) MultiByteToWideChar(CP_ACP, 0, ip, -1, res, 64);
        free(b);
    }
    DeleteFileW(tmp);
    if (res && !res[0]) wcscpy(res, L"(could not get it - no internet?)");
    if (!res || !PostMessageW(g_wnd, WM_APP_EXTIP, 0, (LPARAM)res)) free(res);
    InterlockedExchange(&g_extBusy, 0);
    return 0;
}

static void ConnStrings(const Cfg *c, const WCHAR *ip, WCHAR *cmd, WCHAR *link)
{
    if (c->pass[0]) { _snwprintf(cmd, 300, L"connect %s:%d; password \"%s\"", ip, c->port, c->pass); _snwprintf(link, 300, L"steam://connect/%s:%d/%s", ip, c->port, c->pass); }
    else { _snwprintf(cmd, 300, L"connect %s:%d", ip, c->port); _snwprintf(link, 300, L"steam://connect/%s:%d", ip, c->port); }
}

static void UpdateConnInfo(BOOL refreshExt)
{
    Cfg c; ReadUi(&c);
    WCHAR ips[8][48]; int n = LocalIPv4(ips, 8);
    WCHAR all[400] = L"";
    for (int i = 0; i < n; i++) { if (i) wcscat(all, L"   "); wcscat(all, ips[i]); }
    SetWindowTextW(g_cLocal, n ? all : L"(no network)");
    WCHAR b[64]; _snwprintf(b, 64, L"%d (UDP)", c.port); SetWindowTextW(g_cPort, b);
    BOOL show = SendMessageW(g_cShowPw, BM_GETCHECK, 0, 0) == BST_CHECKED;
    SetWindowTextW(g_cPass, c.pass[0] ? (show ? c.pass : L"********  (tick Show)") : L"(none)");
    { WCHAR rc[96]; _snwprintf(rc, 96, L"%s%s", show ? (c.rcon[0] ? c.rcon : g_autoRcon) : L"********", c.rcon[0] ? L"" : L"   (automatic - set one on the Server tab to choose your own)"); SetWindowTextW(g_cRcon, rc); }
    WCHAR cmd[300], link[300];
    ConnStrings(&c, n ? ips[0] : L"127.0.0.1", cmd, link); SetWindowTextW(g_cCmdL, cmd); SetWindowTextW(g_cLinkL, link);
    if (g_extIp[0] && g_extIp[0] != L'(') { ConnStrings(&c, g_extIp, cmd, link); SetWindowTextW(g_cCmdE, cmd); SetWindowTextW(g_cLinkE, link); }
    else { SetWindowTextW(g_cCmdE, g_extIp[0] ? g_extIp : L"(getting external IP...)"); SetWindowTextW(g_cLinkE, L""); }
    SetWindowTextW(g_cExt, g_extIp[0] ? g_extIp : L"(getting...)");
    WCHAR note[700];
    _snwprintf(note, 700,
        L"Server: %s   |   %d tick (players must use the same rate)   |   %s   |   %s\r\n"
        L"%s",
        g_running ? L"RUNNING" : L"stopped", c.tick, c.lan ? L"LAN only (sv_lan 1)" : L"Internet (sv_lan 0)", MODEUI[c.mode],
        c.lan ? L"LAN mode: only players on your local network can join - use the local address. For friends over the internet choose Internet and forward the port."
              : L"Internet: friends use the external address. Forward UDP port on your router to the first local IP, and allow it in the firewall (Fix firewall). "
                L"Players on your own network should still use the local address.");
    SetWindowTextW(g_cNote, note);
    if ((refreshExt || !g_extIp[0]) && !InterlockedExchange(&g_extBusy, 1)) {
        HANDLE t = CreateThread(NULL, 0, ExtIpThread, NULL, 0, NULL); if (t) CloseHandle(t); else g_extBusy = 0;
    }
}

static void CopyText(HWND src)
{
    int n = GetWindowTextLengthW(src); if (!n) return;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof(WCHAR)); if (!g) return;
    GetWindowTextW(src, (WCHAR *)GlobalLock(g), n + 1); GlobalUnlock(g);
    if (OpenClipboard(g_wnd)) { EmptyClipboard(); SetClipboardData(CF_UNICODETEXT, g); CloseClipboard(); } else GlobalFree(g);
}

static void LogConnInfo(const Cfg *c)
{
    WCHAR ips[8][48]; int n = LocalIPv4(ips, 8);
    Log(L"Connect (LAN):      connect %s:%d%s", n ? ips[0] : L"127.0.0.1", c->port, c->pass[0] ? L"   (+ password)" : L"");
    if (g_extIp[0] && g_extIp[0] != L'(' && !c->lan) Log(L"Connect (internet): connect %s:%d%s", g_extIp, c->port, c->pass[0] ? L"   (+ password)" : L"");
}

/* ---------------------------------------------------------------- embed the cs2.exe window */
static HWND g_conHost, g_embedChk, g_emb = NULL;
static LONG_PTR g_embStyle, g_embEx;
typedef struct { DWORD pid; HWND found; } FindWin;
static BOOL CALLBACK FindWinProc(HWND w, LPARAM l)
{
    FindWin *f = (FindWin *)l; DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    if (pid != f->pid || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    WCHAR cls[64]; GetClassNameW(w, cls, 64);
    if (!wcscmp(cls, L"ConsoleWindowClass") || !wcscmp(cls, L"PseudoConsoleWindow") || !wcscmp(cls, L"IME")) return TRUE;
    RECT r; GetWindowRect(w, &r); if (r.right - r.left < 100 || r.bottom - r.top < 60) return TRUE;
    f->found = w; return FALSE;
}
static void FitEmbedded(void)
{
    if (!g_emb) return;
    RECT r; GetClientRect(g_conHost, &r);
    SetWindowPos(g_emb, NULL, 0, 0, r.right, r.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
static void UpdateConsoleView(void)
{
    BOOL emb = g_emb && IsWindow(g_emb);
    if (!emb) g_emb = NULL;
    if (g_curPage == 1) { ShowWindow(g_conHost, emb ? SW_SHOW : SW_HIDE); ShowWindow(g_con, emb ? SW_HIDE : SW_SHOW); }
}
static void Embed(HWND w)
{
    if (!w || !IsWindow(w) || g_emb == w) return;
    g_emb = w;
    g_embStyle = GetWindowLongPtrW(w, GWL_STYLE); g_embEx = GetWindowLongPtrW(w, GWL_EXSTYLE);
    LONG_PTR st = (g_embStyle & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME)) | WS_CHILD | WS_VISIBLE;
    SetWindowLongPtrW(w, GWL_STYLE, st);
    SetWindowLongPtrW(w, GWL_EXSTYLE, g_embEx & ~(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE));
    SetParent(w, g_conHost);
    FitEmbedded();
    UpdateConsoleView();
    Log(L"Server window is now shown in the Console tab (untick \"Embed server window\" to pop it out).");
}
static void Unembed(void)
{
    if (!g_emb) return;
    HWND w = g_emb; g_emb = NULL;
    if (IsWindow(w)) {
        SetParent(w, NULL);
        SetWindowLongPtrW(w, GWL_STYLE, g_embStyle); SetWindowLongPtrW(w, GWL_EXSTYLE, g_embEx);
        SetWindowPos(w, HWND_TOP, 80, 80, 900, 600, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    UpdateConsoleView();
}
static DWORD WINAPI EmbedWatch(LPVOID p)
{
    DWORD pid = (DWORD)(UINT_PTR)p;
    for (int i = 0; i < 200 && g_running; i++) {           /* up to ~60 s for the window to appear */
        FindWin f = { pid, NULL }; EnumWindows(FindWinProc, (LPARAM)&f);
        if (f.found) { Sleep(500); PostMessageW(g_wnd, WM_APP_EMBED, 1, (LPARAM)f.found); return 0; }
        Sleep(300);
    }
    return 0;
}

/* ---------------------------------------------------------------- Info tab: tick-rate check */
static HWND g_info;
static WCHAR *g_infoBuf; static size_t g_infoLen, g_infoCap;
static void InfoAdd(const wchar_t *fmt, ...)
{
    WCHAR line[2048]; va_list ap; va_start(ap, fmt); _vsnwprintf(line, 2040, fmt, ap); line[2040] = 0; va_end(ap);
    wcscat(line, L"\r\n"); size_t L = wcslen(line);
    if (g_infoLen + L + 1 > g_infoCap) { g_infoCap = (g_infoLen + L + 1) * 2 + 4096; g_infoBuf = (WCHAR *)realloc(g_infoBuf, g_infoCap * sizeof(WCHAR)); }
    wcscpy(g_infoBuf + g_infoLen, line); g_infoLen += L;
}
static void InfoRcon(const char *cmd)
{
    char *o = RconExec(cmd);
    WCHAR wc[128]; MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wc, 128);
    if (!o) { InfoAdd(L"  %-14s (no answer over RCON)", wc); return; }
    char *ctx = NULL; int k = 0;
    for (char *ln = strtok_s(o, "\r\n", &ctx); ln && k < 12; ln = strtok_s(NULL, "\r\n", &ctx), k++) {
        WCHAR w[1024]; MultiByteToWideChar(CP_UTF8, 0, ln, -1, w, 1024); w[1023] = 0; InfoAdd(L"  %-14s %s", k ? L"" : wc, w); }
    if (!k) InfoAdd(L"  %-14s (empty reply)", wc);
    free(o);
}
/* ---- plugin check: files on disk + what the running server really loaded ---- */
static char *ReadFrom(const WCHAR *p, LONGLONG from, LONGLONG *size)
{
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { if (size) *size = 0; return NULL; }
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz); if (size) *size = sz.QuadPart;
    if (from < 0 || from > sz.QuadPart) from = 0;
    LONGLONG n = sz.QuadPart - from; if (n > (8 << 20)) { from = sz.QuadPart - (8 << 20); n = 8 << 20; }
    char *b = (char *)malloc((size_t)n + 1); DWORD got = 0;
    LARGE_INTEGER o; o.QuadPart = from; SetFilePointerEx(f, o, NULL, FILE_BEGIN);
    if (b) { ReadFile(f, b, (DWORD)n, &got, NULL); b[got] = 0; }
    CloseHandle(f); return b;
}
static BOOL HasI(const char *s, const char *w)
{
    if (!s) return FALSE; size_t L = strlen(w);
    for (; *s; s++) if (!_strnicmp(s, w, L)) return TRUE;
    return FALSE;
}
static void PluginCheck(const Cfg *c)
{
    InfoAdd(L"3. Plugins:");
    if (c->noplug) InfoAdd(L"  \"Run without plugins\" is ticked - Metamod is switched off on Start, so no plugins load.");
    if (!HasCss(c->root)) { InfoAdd(L"  CounterStrikeSharp is not installed - click Install plugins on the Server tab."); InfoAdd(L""); return; }
    /* files */
    WCHAR gi[MAX_PATH]; _snwprintf(gi, MAX_PATH, L"%s\\game\\csgo\\gameinfo.gi", c->root);
    char *g = ReadFrom(gi, 0, NULL); BOOL mmLine = HasI(g, "csgo/addons/metamod"); free(g);
    InfoAdd(L"  gameinfo.gi Metamod line:   %s", mmLine ? L"present" : L"missing (Start adds it unless \"Run without plugins\")");
    InfoAdd(L"  Metamod files:              %s", FileIn(c->root, L"game\\csgo\\addons\\metamod\\bin\\win64\\server.dll") ? L"OK" : L"MISSING - Install plugins");
    InfoAdd(L"  CounterStrikeSharp files:   %s%s", FileIn(c->root, L"game\\csgo\\addons\\counterstrikesharp\\bin\\win64\\counterstrikesharp.dll") ? L"OK" : L"MISSING",
            FileIn(c->root, L"game\\csgo\\addons\\metamod\\counterstrikesharp.vdf") ? L"" : L"  (addons\\metamod\\counterstrikesharp.vdf missing - Metamod will not load it)");
    WCHAR st[MAX_PATH]; _snwprintf(st, MAX_PATH, L"%s\\cs2launcher_components.txt", c->root);
    char *s = ReadFrom(st, 0, NULL);
    static const char *K[3] = { "MatchZy", "Retakes", "InstaDefuse" };
    BOOL want[3] = { c->mode != 3, c->mode == 3, c->mode == 3 };
    for (int i = 0; i < 3; i++) {
        char key[40]; snprintf(key, sizeof key, "%s.folder=", K[i]);
        char *q = s ? strstr(s, key) : NULL; WCHAR folder[80] = L"";
        if (q) { q += strlen(key); char f[80]; int n = 0; while (*q && *q != '\r' && *q != '\n' && n < 79) f[n++] = *q++; f[n] = 0; MultiByteToWideChar(CP_UTF8, 0, f, -1, folder, 80); }
        WCHAR on[MAX_PATH], off[MAX_PATH];
        _snwprintf(on, MAX_PATH, L"%s\\game\\csgo\\addons\\counterstrikesharp\\plugins\\%s", c->root, folder);
        _snwprintf(off, MAX_PATH, L"%s\\game\\csgo\\addons\\counterstrikesharp\\plugins\\disabled\\%s", c->root, folder);
        DWORD a1 = folder[0] ? GetFileAttributesW(on) : INVALID_FILE_ATTRIBUTES, a2 = folder[0] ? GetFileAttributesW(off) : INVALID_FILE_ATTRIBUTES;
        const WCHAR *where = !folder[0] ? L"not installed" : a1 != INVALID_FILE_ATTRIBUTES ? L"enabled (plugins\\)" : a2 != INVALID_FILE_ATTRIBUTES ? L"disabled (plugins\\disabled\\)" : L"folder missing";
        InfoAdd(L"  %-12hs %-30s %s", K[i], where, want[i] ? L"<- needed for this mode" : L"");
    }
    free(s);
    /* live */
    InfoAdd(L"");
    if (!g_running) { InfoAdd(L"  Server not running - Start it, then press Check again to see what actually loaded."); InfoAdd(L""); return; }
    WCHAR cl[MAX_PATH]; ConLogPath(c, 0, cl);
    LONGLONG before = 0; free(ReadFrom(cl, 0, &before));
    InfoAdd(L"  Asking the server:");
    char *mv = RconExec("meta list"), *cv = RconExec("css_plugins list");
    if (mv) { InfoAdd(L"    > meta list"); char *t = _strdup(mv), *ctx = NULL; int k = 0; for (char *ln = strtok_s(t, "\r\n", &ctx); ln && k < 10; ln = strtok_s(NULL, "\r\n", &ctx), k++) { WCHAR w[600]; MultiByteToWideChar(CP_UTF8, 0, ln, -1, w, 600); w[599] = 0; InfoAdd(L"      %s", w); } free(t); }
    if (cv) { InfoAdd(L"    > css_plugins list"); char *t = _strdup(cv), *ctx = NULL; int k = 0; for (char *ln = strtok_s(t, "\r\n", &ctx); ln && k < 14; ln = strtok_s(NULL, "\r\n", &ctx), k++) { WCHAR w[600]; MultiByteToWideChar(CP_UTF8, 0, ln, -1, w, 600); w[599] = 0; InfoAdd(L"      %s", w); } free(t); }
    Sleep(900);
    char *tail = ReadFrom(cl, before, NULL);         /* replies that only went to the console */
    size_t L = (mv ? strlen(mv) : 0) + (cv ? strlen(cv) : 0) + (tail ? strlen(tail) : 0) + 4;
    char *all = (char *)calloc(L, 1);
    if (all) { if (mv) strcat(all, mv); strcat(all, "\n"); if (cv) strcat(all, cv); strcat(all, "\n"); if (tail) strcat(all, tail); }
    BOOL unknownMeta = HasI(all, "Unknown command \"meta\"") || HasI(all, "Unknown command 'meta'");
    BOOL mmOk = !unknownMeta && (HasI(all, "Listing") || HasI(all, "Metamod") || HasI(all, "plugin"));
    BOOL cssOk = HasI(all, "CounterStrikeSharp") && !HasI(all, "Unknown command \"css_plugins\"");
    BOOL ld[3] = { HasI(all, "MatchZy"), HasI(all, "Retakes"), HasI(all, "InstaDefuse") || HasI(all, "Instadefuse") || HasI(all, "Insta Defuse") };
    if (!mv && !cv) InfoAdd(L"  (RCON did not answer - wait a few seconds after Start and press Check again)");
    else {
        InfoAdd(L"  RESULT:");
        InfoAdd(L"    Metamod             %s", mmOk ? L"LOADED" : L"NOT LOADED");
        InfoAdd(L"    CounterStrikeSharp  %s", cssOk ? L"LOADED" : L"NOT LOADED");
        for (int i = 0; i < 3; i++) InfoAdd(L"    %-19hs %s%s", K[i], ld[i] ? L"LOADED" : L"not loaded", want[i] && !ld[i] ? L"   <-- PROBLEM: this mode needs it" : (!want[i] && ld[i] ? L"   <-- should be off in this mode" : L""));
        if (!mmOk) InfoAdd(L"    Metamod did not load: check the gameinfo.gi line, \"Run without plugins\", or that this Metamod build supports CS2 %hs.", TP_BUILD_1420_NAME);
        else if (!cssOk) InfoAdd(L"    CounterStrikeSharp did not load: usually a version that does not support this CS2 build yet, or a missing Visual C++ runtime. See errors below.");
    }
    /* errors in the whole console log */
    char *full = ReadFrom(cl, 0, NULL); int shown = 0;
    if (full) {
        char *ctx = NULL;
        for (char *ln = strtok_s(full, "\r\n", &ctx); ln; ln = strtok_s(NULL, "\r\n", &ctx)) {
            BOOL plug = HasI(ln, "[META]") || HasI(ln, "CounterStrikeSharp") || HasI(ln, "MatchZy") || HasI(ln, "Retakes") || HasI(ln, "InstaDefuse") || HasI(ln, "plugin");
            BOOL bad = HasI(ln, "error") || HasI(ln, "fail") || HasI(ln, "exception") || HasI(ln, "unable") || HasI(ln, "could not") || HasI(ln, "not found");
            if (plug && bad) {
                if (!shown) InfoAdd(L"  Plugin errors in console.log:");
                if (shown < 15) { WCHAR w[700]; MultiByteToWideChar(CP_UTF8, 0, ln, -1, w, 700); w[699] = 0; InfoAdd(L"    %s", w); }
                shown++;
            }
        }
        free(full);
    }
    if (!shown) InfoAdd(L"  No plugin errors in console.log.");
    else if (shown > 15) InfoAdd(L"    ... %d more (Open logs / console.log)", shown - 15);
    free(all); free(tail); free(mv); free(cv);
    InfoAdd(L"");
}

static volatile LONG g_infoBusy = 0;
static DWORD WINAPI InfoThread(LPVOID unused)
{
    (void)unused;
    Cfg c; ReadUi(&c);
    g_infoLen = 0; if (g_infoBuf) g_infoBuf[0] = 0;
    SYSTEMTIME st; GetLocalTime(&st);
    InfoAdd(L"TICK RATE + PLUGIN CHECK  (%02d:%02d:%02d)", st.wHour, st.wMinute, st.wSecond);
    InfoAdd(L"");
    InfoAdd(L"Chosen on the Server tab: %d tick", c.tick);
    InfoAdd(L"");
    InfoAdd(L"1. Server DLLs (what the patch engine finds on disk):");
    int rate = HasServer(c.root) ? tp_status(c.root, InfoAdd) : -1;
    if (rate < 0) InfoAdd(L"  no server in %s", c.root);
    else if (rate == 64) InfoAdd(L"  => 64 tick (original files)%s", c.tick > 64 ? L" - the patch is applied when you click Start" : L"");
    else if (rate > 0) InfoAdd(L"  => all three patched to %d tick%s", rate, rate != c.tick ? L" - differs from the chosen rate; Start re-patches" : L"");
    else InfoAdd(L"  => mixed or unknown - Start will re-patch or refuse unsupported files");
    InfoAdd(L"");
    InfoAdd(L"2. Running server:");
    if (!g_running) InfoAdd(L"  not running");
    else {
        InfoAdd(L"  launched with -insecure and +sv_fps_max %d (frame cap = tick rate)", c.tick == 64 ? 0 : c.tick);
        InfoRcon("sv_fps_max");
        InfoRcon("hostname");
        InfoRcon("sv_cheats");
        if (!g_rconOk) InfoAdd(L"  (RCON not connected yet - wait a few seconds after Start and press Check again)");
    }
    InfoAdd(L"");
    PluginCheck(&c);
    InfoAdd(L"4. Players:");
    InfoAdd(L"  Players must run CS2 through the 128/256 tick game launcher at %d tick.", rate > 64 ? rate : c.tick);
    InfoAdd(L"  The game launcher has the same check on its Info tab (patched rate of their DLLs, -insecure).");
    InfoAdd(L"  The server name carries the rate, e.g. \"%s [%d tick]\", so players can see it in the server browser / scoreboard.", c.name, rate > 64 ? rate : c.tick);
    InfoAdd(L"  A player on a different rate gets prediction errors / rubber-banding: compare both Info tabs.");
    InfoAdd(L"  In game, players check with   cl_ticktiming print   (console): the line FirstInput -> NetSend is");
    InfoAdd(L"  about one tick minus half a frame - ~6-7 ms at 128 tick, ~3 ms at 256, ~14-15 ms at stock 64.");
    WCHAR *cp = _wcsdup(g_infoBuf ? g_infoBuf : L"");
    if (!cp || !PostMessageW(g_wnd, WM_APP_INFO, 0, (LPARAM)cp)) free(cp);
    InterlockedExchange(&g_infoBusy, 0);
    return 0;
}
static void CheckTickRate(void)
{
    if (InterlockedExchange(&g_infoBusy, 1)) return;
    SetWindowTextW(g_info, L"Checking...");
    HANDLE t = CreateThread(NULL, 0, InfoThread, NULL, 0, NULL); if (t) CloseHandle(t); else g_infoBusy = 0;
}

/* ---------------------------------------------------------------- map dropdowns */
static int ListMaps(const WCHAR *root, WCHAR names[][64], int max)
{
    int n = 0;
    WCHAR pat[MAX_PATH]; _snwprintf(pat, MAX_PATH, L"%s\\game\\csgo\\maps\\*.vpk", root);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            WCHAR nm[64]; wcsncpy(nm, fd.cFileName, 63); nm[63] = 0;
            WCHAR *dot = wcsrchr(nm, L'.'); if (dot) *dot = 0;
            WCHAR low[64]; wcscpy(low, nm); _wcslwr(low);
            if (wcsstr(low, L"_vanity") || wcsstr(low, L"lobby_") || wcsstr(low, L"graphics_settings") || wcsstr(low, L"_prefab") ||
                wcsstr(low, L"_skybox") || wcsstr(low, L"_dir") == low + wcslen(low) - 4 || !wcschr(low, L'_')) continue;
            /* split vpks: de_dust2_000.vpk etc. */
            size_t L = wcslen(low); if (L > 4 && low[L - 4] == L'_' && iswdigit(low[L - 3]) && iswdigit(low[L - 2]) && iswdigit(low[L - 1])) continue;
            BOOL dup = FALSE; for (int i = 0; i < n; i++) if (!_wcsicmp(names[i], nm)) dup = TRUE;
            if (!dup && n < max) wcscpy(names[n++], nm);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!n) for (int i = 0; i < NPOOL && n < max; i++) wcscpy(names[n++], POOL[i]);   /* no server yet: the usual pool */
    return n;
}

static void FillCombo(HWND cb, WCHAR names[][64], int n, BOOL keep)
{
    WCHAR cur[64] = L""; if (keep) GetWindowTextW(cb, cur, 64);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < n; i++) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)names[i]);
    if (keep) SetWindowTextW(cb, cur);
}

static void FillMapCombos(void)
{
    WCHAR root[MAX_PATH]; GetWindowTextW(g_root, root, MAX_PATH);
    static WCHAR names[256][64]; int n = ListMaps(root, names, 256);
    FillCombo(g_map, names, n, TRUE);
    FillCombo(g_chmap, names, n, TRUE);
    for (int i = 0; i < 5; i++) FillCombo(g_ms[i], names, n, TRUE);
    /* sensible defaults for the pick dropdowns */
    static const WCHAR *def[5] = { L"de_mirage", L"de_inferno", L"de_ancient", L"de_nuke", L"de_dust2" };
    for (int i = 0; i < 5; i++) if (!GetWindowTextLengthW(g_ms[i])) SetWindowTextW(g_ms[i], def[i]);
}

static void UpdateMapPick(void)
{
    static const int SER[4] = { 1, 2, 3, 5 };
    int nm = SER[max(0, (int)SendMessageW(g_series, CB_GETCURSEL, 0, 0))];
    BOOL pick = SendMessageW(g_mapmode, CB_GETCURSEL, 0, 0) == 0;
    for (int i = 0; i < 5; i++) EnableWindow(g_ms[i], pick && i < nm);
    EnableWindow(g_maps, !pick); EnableWindow(g_extra, !pick);
}

static void ChangeMap(void)
{
    WCHAR m[64]; GetWindowTextW(g_chmap, m, 64);
    if (!m[0]) return;
    if (!g_running) { MessageBoxW(g_wnd, L"Start the server first.", APP_TITLE, MB_ICONINFORMATION); return; }
    for (WCHAR *q = m; *q; q++) if (*q == L' ' || *q == L';' || *q == L'"') { MessageBoxW(g_wnd, L"Not a valid map name.", APP_TITLE, MB_ICONWARNING); return; }
    WCHAR cmd[100]; _snwprintf(cmd, 100, L"changelevel %s", m);
    { WCHAR e[200]; _snwprintf(e, 200, L"> %s\r\n", cmd); WCHAR *cp = _wcsdup(e); if (cp && !PostMessageW(g_wnd, WM_APP_CON, 0, (LPARAM)cp)) free(cp); }
    HANDLE t = CreateThread(NULL, 0, CmdThread, _wcsdup(cmd), 0, NULL); if (t) CloseHandle(t);
}

/* ---------------------------------------------------------------- admins */
static HWND g_aprof, g_aname, g_aflags, g_alist, g_astat, g_amz;
static const WCHAR *AFLAGS[] = { L"@css/root", L"@css/generic", L"@css/kick", L"@css/ban", L"@css/unban", L"@css/slay", L"@css/changemap",
    L"@css/cvar", L"@css/config", L"@css/chat", L"@css/vote", L"@css/password", L"@css/rcon", L"@css/cheats", L"@css/reservation" };
static const WCHAR *AFDESC[] = { L"everything", L"basic admin", L"kick", L"ban", L"unban", L"slay / slap", L"change map",
    L"change cvars", L"configs (MatchZy admin)", L"admin chat", L"start votes", L"server password", L"rcon", L"cheat commands", L"reserved slot" };
#define NAFLAGS 15
typedef struct { WCHAR name[64], sid[24], flags[400]; BOOL mz; } Admin;
static Admin g_adm[128]; static int g_nadm = 0;

static void AdminPaths(WCHAR *css, WCHAR *mz)
{
    WCHAR root[MAX_PATH]; GetWindowTextW(g_root, root, MAX_PATH);
    _snwprintf(css, MAX_PATH, L"%s\\game\\csgo\\addons\\counterstrikesharp\\configs\\admins.json", root);
    _snwprintf(mz, MAX_PATH, L"%s\\game\\csgo\\cfg\\MatchZy\\admins.json", root);
}

/* read CounterStrikeSharp admins.json:  { "Name": { "identity": "7656...", "flags": ["@css/root"] }, ... } */
static void ParseCssAdmins(const char *s)
{
    for (const char *q = s; (q = strstr(q, "\"identity\"")); q++) {
        if (g_nadm >= 128) break;
        /* the object that holds it, and the key before that object */
        const char *ob = q; int d = 0; while (ob > s) { ob--; if (*ob == '}') d++; else if (*ob == '{') { if (d == 0) break; d--; } }
        const char *k2 = ob; while (k2 > s && *k2 != ':') k2--;
        const char *ke = k2; while (ke > s && *ke != '"') ke--;
        const char *ks = ke > s ? ke - 1 : s; while (ks > s && *ks != '"') ks--;
        const char *oe = strchr(ob, '}'); if (!oe) break;
        char v[400];
        Admin *a = &g_adm[g_nadm]; ZeroMemory(a, sizeof *a);
        if (ke > ks) { char nm[64]; size_t L = ke - ks - 1; if (L > 63) L = 63; memcpy(nm, ks + 1, L); nm[L] = 0; MultiByteToWideChar(CP_UTF8, 0, nm, -1, a->name, 64); }
        if (JGet(ob, oe, "identity", v, sizeof v)) MultiByteToWideChar(CP_UTF8, 0, v, -1, a->sid, 24);
        const char *fl = strstr(ob, "\"flags\"");
        if (fl && fl < oe) {
            const char *lb = strchr(fl, '['), *rb = lb ? strchr(lb, ']') : NULL;
            for (const char *f = lb; f && rb && (f = strchr(f + 1, '"')) && f < rb; ) {
                const char *fe = strchr(f + 1, '"'); if (!fe || fe > rb) break;
                char one[64]; size_t L = fe - f - 1; if (L > 63) L = 63; memcpy(one, f + 1, L); one[L] = 0;
                WCHAR w[64]; MultiByteToWideChar(CP_UTF8, 0, one, -1, w, 64);
                if (a->flags[0]) wcscat(a->flags, L" "); if (wcslen(a->flags) + wcslen(w) < 390) wcscat(a->flags, w);
                f = fe;
            }
        }
        if (a->sid[0]) g_nadm++;
        q = oe;
    }
}

static void FillAdminList(void)
{
    SendMessageW(g_alist, LVM_DELETEALLITEMS, 0, 0);
    for (int i = 0; i < g_nadm; i++) {
        LVITEMW it = { 0 }; it.mask = LVIF_TEXT; it.iItem = i; it.pszText = g_adm[i].name;
        SendMessageW(g_alist, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iSubItem = 1; it.pszText = g_adm[i].sid; SendMessageW(g_alist, LVM_SETITEMTEXTW, i, (LPARAM)&it);
        it.iSubItem = 2; it.pszText = g_adm[i].mz ? L"yes" : L"no"; SendMessageW(g_alist, LVM_SETITEMTEXTW, i, (LPARAM)&it);
        it.iSubItem = 3; it.pszText = g_adm[i].flags; SendMessageW(g_alist, LVM_SETITEMTEXTW, i, (LPARAM)&it);
    }
}

static void LoadAdmins(void)
{
    WCHAR css[MAX_PATH], mz[MAX_PATH]; AdminPaths(css, mz);
    g_nadm = 0;
    size_t n; uint8_t *b = tp_read(css, &n, NULL);
    if (b) { b[n] = 0; ParseCssAdmins((char *)b); free(b); }
    /* MatchZy admins: { "7656...": "", ... } */
    b = tp_read(mz, &n, NULL);
    if (b) {
        b[n] = 0;
        for (char *q = (char *)b; (q = strchr(q, '"')); ) {
            char *e = strchr(q + 1, '"'); if (!e) break;
            if (e - q - 1 == 17 && !strncmp(q + 1, "7656", 4)) {
                WCHAR w[24]; char id[18]; memcpy(id, q + 1, 17); id[17] = 0; MultiByteToWideChar(CP_UTF8, 0, id, -1, w, 24);
                int k = -1; for (int i = 0; i < g_nadm; i++) if (!wcscmp(g_adm[i].sid, w)) k = i;
                if (k < 0 && g_nadm < 128) { k = g_nadm++; ZeroMemory(&g_adm[k], sizeof g_adm[k]); wcscpy(g_adm[k].sid, w); wcscpy(g_adm[k].name, w); }
                if (k >= 0) g_adm[k].mz = TRUE;
            }
            q = e + 1;
        }
        free(b);
    }
    FillAdminList();
    WCHAR t[300];
    WCHAR root[MAX_PATH]; GetWindowTextW(g_root, root, MAX_PATH);
    _snwprintf(t, 300, HasCss(root) ? L"%d admin(s). Files: counterstrikesharp\\configs\\admins.json and cfg\\MatchZy\\admins.json."
                                    : L"CounterStrikeSharp is not installed yet - admins are saved anyway and used once plugins are installed (%d).", g_nadm);
    SetWindowTextW(g_astat, t);
}

static void JsonStrW(FILE *f, const WCHAR *s) { JsonStr(f, s); }

static BOOL SaveAdmins(void)
{
    WCHAR css[MAX_PATH], mz[MAX_PATH]; AdminPaths(css, mz);
    WCHAR dir[MAX_PATH];
    wcscpy(dir, css); PathRemoveFileSpecW(dir); SHCreateDirectoryExW(NULL, dir, NULL);
    wcscpy(dir, mz); PathRemoveFileSpecW(dir); SHCreateDirectoryExW(NULL, dir, NULL);
    FILE *f = _wfopen(css, L"wb"); if (!f) { Log(L"ERROR: cannot write %s", css); return FALSE; }
    fputs("{\n", f);
    for (int i = 0; i < g_nadm; i++) {
        WCHAR key[80]; _snwprintf(key, 80, L"%s", g_adm[i].name[0] ? g_adm[i].name : g_adm[i].sid);
        for (int k = 0; k < i; k++) if (!wcscmp(g_adm[k].name, g_adm[i].name)) { _snwprintf(key, 80, L"%s (%s)", g_adm[i].name, g_adm[i].sid); break; }
        fputs("  ", f); JsonStrW(f, key); fputs(": {\n    \"identity\": ", f); JsonStrW(f, g_adm[i].sid); fputs(",\n    \"flags\": [", f);
        WCHAR fl[400]; wcscpy(fl, g_adm[i].flags); WCHAR *ctx = NULL; int n = 0;
        for (WCHAR *t = wcstok_s(fl, L" ,", &ctx); t; t = wcstok_s(NULL, L" ,", &ctx)) { if (n++) fputs(", ", f); JsonStrW(f, t); }
        fputs(i + 1 < g_nadm ? "]\n  },\n" : "]\n  }\n", f);
    }
    fputs("}\n", f); fclose(f);
    f = _wfopen(mz, L"wb"); if (!f) { Log(L"ERROR: cannot write %s", mz); return FALSE; }
    fputs("{\n", f); int n = 0;
    for (int i = 0; i < g_nadm; i++) if (g_adm[i].mz) { if (n++) fputs(",\n", f); fputs("  ", f); JsonStrW(f, g_adm[i].sid); fputs(": \"\"", f); }
    fputs("\n}\n", f); fclose(f);
    Log(L"Admins saved (%d): %s", g_nadm, css);
    if (g_running) {
        HANDLE t = CreateThread(NULL, 0, CmdThread, _wcsdup(L"css_reloadadmins"), 0, NULL); if (t) CloseHandle(t);
        SetWindowTextW(g_astat, L"Saved. Sent css_reloadadmins; MatchZy reads its admin list on map change / restart.");
    } else SetWindowTextW(g_astat, L"Saved. Used when the server starts.");
    return TRUE;
}

static void AdminFlagsFromUi(WCHAR *out, int n)
{
    out[0] = 0;
    for (int i = 0; i < NAFLAGS; i++)
        if (SendMessageW(g_aflags, LVM_GETITEMSTATE, i, LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2)) {
            if (out[0]) wcsncat(out, L" ", n - wcslen(out) - 1); wcsncat(out, AFLAGS[i], n - wcslen(out) - 1); }
}
static void SetFlagChecks(const WCHAR *flags)
{
    for (int i = 0; i < NAFLAGS; i++) {
        BOOL on = FALSE; const WCHAR *p = flags; size_t L = wcslen(AFLAGS[i]);
        while ((p = wcsstr(p, AFLAGS[i]))) { if (p[L] == 0 || p[L] == L' ' || p[L] == L',') { on = TRUE; break; } p += L; }
        LVITEMW it = { 0 }; it.stateMask = LVIS_STATEIMAGEMASK; it.state = INDEXTOSTATEIMAGEMASK(on ? 2 : 1);
        SendMessageW(g_aflags, LVM_SETITEMSTATE, i, (LPARAM)&it);
    }
}

static void AddAdminResolved(const WCHAR *sid, const WCHAR *name)
{
    WCHAR fl[400]; AdminFlagsFromUi(fl, 400);
    if (!fl[0]) { MessageBoxW(g_wnd, L"Tick at least one permission (e.g. @css/root for full admin).", APP_TITLE, MB_ICONINFORMATION); return; }
    int k = -1; for (int i = 0; i < g_nadm; i++) if (!wcscmp(g_adm[i].sid, sid)) k = i;
    if (k < 0) { if (g_nadm >= 128) return; k = g_nadm++; ZeroMemory(&g_adm[k], sizeof g_adm[k]); }
    wcsncpy(g_adm[k].sid, sid, 23);
    WCHAR nm[64]; GetWindowTextW(g_aname, nm, 64);
    _snwprintf(g_adm[k].name, 64, L"%s", nm[0] ? nm : (name && name[0] ? name : sid));
    wcscpy(g_adm[k].flags, fl);
    g_adm[k].mz = SendMessageW(g_amz, BM_GETCHECK, 0, 0) == BST_CHECKED;
    FillAdminList();
    SaveAdmins();
    SetWindowTextW(g_aprof, L""); SetWindowTextW(g_aname, L"");
}

typedef struct { WCHAR sid[24], name[64], err[200]; } AdminRes;
static DWORD WINAPI AdminResolveThread(LPVOID p)
{
    WCHAR *txt = (WCHAR *)p;
    WCHAR in[MAX_PATH], out[MAX_PATH]; _snwprintf(in, MAX_PATH, L"%s\\admin_in.txt", g_dataDir); _snwprintf(out, MAX_PATH, L"%s\\admin_out.txt", g_dataDir);
    FILE *f = _wfopen(in, L"wb"); if (f) { char u[600]; WideCharToMultiByte(CP_UTF8, 0, txt, -1, u, sizeof u, NULL, NULL); fprintf(f, "1\t%s\n", u); fclose(f); }
    free(txt); DeleteFileW(out);
    RunPs(L"resolve", in, out);
    AdminRes *r = (AdminRes *)calloc(1, sizeof *r);
    size_t n; uint8_t *b = tp_read(out, &n, NULL);
    if (b) {
        b[n] = 0; WCHAR w[700]; MultiByteToWideChar(CP_UTF8, 0, (char *)b, -1, w, 700);
        WCHAR *ctx = NULL, *t0 = wcstok_s(w, L"\t\r\n", &ctx), *t1 = wcstok_s(NULL, L"\t\r\n", &ctx), *t2 = wcstok_s(NULL, L"\t\r\n", &ctx), *t3 = wcstok_s(NULL, L"\t\r\n", &ctx);
        (void)t0;
        if (t1 && !wcscmp(t1, L"ERR")) _snwprintf(r->err, 200, L"%s", t3 ? t3 : L"could not resolve");
        else if (t1) { wcsncpy(r->sid, t1, 23); if (t2) wcsncpy(r->name, t2, 63); }
        free(b);
    } else wcscpy(r->err, L"profile lookup failed");
    if (!PostMessageW(g_wnd, WM_APP_ADMINRES, 0, (LPARAM)r)) free(r);
    return 0;
}

static void AddAdmin(void)
{
    WCHAR p[300]; GetWindowTextW(g_aprof, p, 300);
    WCHAR *s = p; while (*s == L' ') s++;
    if (!*s) { MessageBoxW(g_wnd, L"Enter a Steam profile (link, SteamID64, STEAM_0:x:y or custom URL name).", APP_TITLE, MB_ICONINFORMATION); return; }
    BOOL id64 = wcslen(s) >= 17 && !wcsncmp(s, L"7656", 4); for (int i = 0; id64 && i < 17; i++) if (!iswdigit(s[i])) id64 = FALSE;
    if (id64 && (s[17] == 0 || s[17] == L' ')) { WCHAR sid[24]; wcsncpy(sid, s, 17); sid[17] = 0; AddAdminResolved(sid, NULL); return; }
    SetWindowTextW(g_astat, L"Looking up the Steam profile...");
    HANDLE t = CreateThread(NULL, 0, AdminResolveThread, _wcsdup(s), 0, NULL); if (t) CloseHandle(t);
}

static void RemoveAdmin(void)
{
    int i = (int)SendMessageW(g_alist, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (i < 0 || i >= g_nadm) { MessageBoxW(g_wnd, L"Select an admin in the list first.", APP_TITLE, MB_ICONINFORMATION); return; }
    WCHAR q[200]; _snwprintf(q, 200, L"Remove admin %s (%s)?", g_adm[i].name, g_adm[i].sid);
    if (MessageBoxW(g_wnd, q, APP_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    memmove(&g_adm[i], &g_adm[i + 1], sizeof(Admin) * (g_nadm - i - 1)); g_nadm--;
    FillAdminList(); SaveAdmins();
}

static void SelectAdmin(void)
{
    int i = (int)SendMessageW(g_alist, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (i < 0 || i >= g_nadm) return;
    SetWindowTextW(g_aprof, g_adm[i].sid); SetWindowTextW(g_aname, g_adm[i].name);
    SetFlagChecks(g_adm[i].flags); SendMessageW(g_amz, BM_SETCHECK, g_adm[i].mz ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void ShowPage(int p)
{
    for (int k = 0; k < 7; k++) for (int i = 0; i < g_npage[k]; i++) ShowWindow(g_page[k][i], k == p ? SW_SHOW : SW_HIDE);
    g_curPage = p;
    if (p == 2) PollPlayers();
    if (p == 4) UpdateConnInfo(FALSE);
    if (p == 1) UpdateConsoleView();
    if (p == 0 || p == 1 || p == 3) FillMapCombos();
    if (p == 3) UpdateMapPick();
    if (p == 5) CheckTickRate();
    if (p == 6) LoadAdmins();
}

static void AddCol(HWND lv, int i, const WCHAR *t, int w)
{ LVCOLUMNW col = { 0 }; col.mask = LVCF_TEXT | LVCF_WIDTH; col.pszText = (WCHAR *)t; col.cx = w; SendMessageW(lv, LVM_INSERTCOLUMNW, i, (LPARAM)&col); }

#define TOP 36
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        g_mono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, L"Consolas");
        g_tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_BUTTONS | TCS_FLATBUTTONS, 8, 6, 600, 26, h, (HMENU)ID_TAB, GetModuleHandleW(NULL), NULL);
        SendMessageW(g_tab, WM_SETFONT, (WPARAM)g_font, TRUE);
        const WCHAR *tabs[7] = { L"  Server  ", L"  Console  ", L"  Players  ", L"  Match  ", L"  Connect  ", L"  Info  ", L"  Admins  " };
        for (int i = 0; i < 7; i++) { TCITEMW ti = { 0 }; ti.mask = TCIF_TEXT; ti.pszText = (WCHAR *)tabs[i]; SendMessageW(g_tab, TCM_INSERTITEMW, i, (LPARAM)&ti); }

        /* ---------- page 0: server ---------- */
        g_curPage = 0;
        int y = TOP + 4;
        LBL(L"Server folder:", 10, y, 90); g_root = EDT(ID_ROOT, 105, y, 470, 0);
        Mk(h, L"BUTTON", L"...", WS_TABSTOP, 0, 580, y, 30, 22, ID_BROWSE); y += 30;
        LBL(L"SteamCMD folder:", 10, y, 92); g_scmd = EDT(ID_SCMD, 105, y, 470, 0);
        Mk(h, L"BUTTON", L"...", WS_TABSTOP, 0, 580, y, 30, 22, ID_BROWSE2); y += 30;
        LBL(L"Server name:", 10, y, 90); g_name = EDT(ID_NAME, 105, y, 220, 0);
        LBL(L"Map:", 340, y, 40); g_map = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_SORT | WS_VSCROLL | WS_TABSTOP, 0, 385, y, 120, 320, ID_MAP);
        LBL(L"Port:", 515, y, 30); g_port = EDT(ID_PORT, 550, y, 60, ES_NUMBER); y += 30;
        LBL(L"Password:", 10, y, 90); g_pass = EDT(ID_PASS, 105, y, 220, ES_PASSWORD);
        LBL(L"RCON:", 340, y, 40); g_rcon = EDT(ID_RCON, 385, y, 120, ES_PASSWORD);
        LBL(L"Slots:", 515, y, 32); g_maxpl = EDT(ID_MAXPL, 550, y, 60, ES_NUMBER); y += 30;
        LBL(L"Game mode:", 10, y, 90);
        g_mode = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 0, 105, y, 220, 200, ID_MODE);
        for (int i = 0; i < 4; i++) SendMessageW(g_mode, CB_ADDSTRING, 0, (LPARAM)MODEUI[i]);
        LBL(L"Tick:", 340, y, 40);
        g_tick = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 0, 385, y, 120, 200, ID_TICK);
        SendMessageW(g_tick, CB_ADDSTRING, 0, (LPARAM)L"64 (stock)");
        SendMessageW(g_tick, CB_ADDSTRING, 0, (LPARAM)L"128 tick");
        SendMessageW(g_tick, CB_ADDSTRING, 0, (LPARAM)L"256 tick");
        y += 30;
        g_lan = Mk(h, L"BUTTON", L"LAN", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 0, 105, y, 60, 22, ID_LAN);
        g_net = Mk(h, L"BUTTON", L"Internet", BS_AUTORADIOBUTTON, 0, 170, y, 70, 22, ID_NET);
        LBL(L"GSLT:", 250, y, 35); g_gslt = EDT(ID_GSLT, 290, y, 215, ES_PASSWORD);
        g_noplug = Mk(h, L"BUTTON", L"Run without plugins", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 515, y, 130, 22, ID_NOPLUG);
        y += 26;
        g_debug = Mk(h, L"BUTTON", L"Debug window (separate console that stays open after it exits)", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 105, y, 380, 22, ID_DEBUG);
        Mk(h, L"BUTTON", L"Open logs", WS_TABSTOP, 0, 535, y, 75, 22, ID_OPENLOGS);
        y += 32;
        const WCHAR *bt[7] = { L"Install / Update", L"Install plugins", L"Check updates", L"Fix firewall", L"Start", L"Stop", L"Restore 64 tick" };
        int bid[7] = { ID_INSTALL, ID_PLUGINS, ID_CHECK, ID_FIREWALL, ID_START, ID_STOP, ID_RESTORE };
        int bx = 10, bw[7] = { 100, 95, 90, 80, 70, 60, 100 };
        for (int i = 0; i < 7; i++) { g_btn[i] = Mk(h, L"BUTTON", bt[i], WS_TABSTOP | (i == 4 ? BS_DEFPUSHBUTTON : 0), 0, bx, y, bw[i], 28, bid[i]); bx += bw[i] + 5; }
        y += 34;
        g_prog = Mk(h, PROGRESS_CLASSW, L"", PBS_SMOOTH, 0, 10, y + 3, 250, 18, ID_PROG);
        SendMessageW(g_prog, PBM_SETRANGE32, 0, 1000);
        g_pause = Mk(h, L"BUTTON", L"Pause", WS_TABSTOP, 0, 268, y, 60, 24, ID_PAUSE);
        g_cancel = Mk(h, L"BUTTON", L"Cancel", WS_TABSTOP, 0, 333, y, 60, 24, ID_CANCEL);
        g_pstat = Mk(h, L"STATIC", L"Idle", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0, 400, y + 4, 300, 18, ID_PSTAT);
        y += 30;
        g_log = Mk(h, L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, WS_EX_CLIENTEDGE, 10, y, 640, 300, ID_LOG);
        SendMessageW(g_log, WM_SETFONT, (WPARAM)g_mono, TRUE); SendMessageW(g_log, EM_SETLIMITTEXT, 0, 0);

        /* ---------- page 1: console ---------- */
        g_curPage = 1; y = TOP + 4;
        LBL(L"Command:", 10, y, 60); g_cmd = EDT(ID_CMD, 75, y, 440, 0);
        g_send = Mk(h, L"BUTTON", L"Send", WS_TABSTOP, 0, 520, y, 55, 22, ID_SEND);
        Mk(h, L"BUTTON", L"Clear", WS_TABSTOP, 0, 580, y, 55, 22, ID_CONCLEAR);
        y += 28;
        LBL(L"Change map:", 10, y, 70);
        g_chmap = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_SORT | WS_VSCROLL | WS_TABSTOP, 0, 85, y, 200, 320, ID_CHMAP);
        Mk(h, L"BUTTON", L"Change", WS_TABSTOP, 0, 290, y, 65, 22, ID_CHMAPBTN);
        g_embedChk = Mk(h, L"BUTTON", L"Embed server window", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 380, y, 150, 22, ID_EMBED);
        SendMessageW(g_embedChk, BM_SETCHECK, GetPrivateProfileIntW(L"server", L"embed", 1, g_ini) ? BST_CHECKED : BST_UNCHECKED, 0);
        y += 30;
        g_conHost = Mk(h, L"STATIC", L"", WS_CLIPCHILDREN | SS_NOTIFY | SS_GRAYFRAME, 0, 10, y, 640, 500, ID_CONHOST);
        g_con = Mk(h, L"EDIT", L"", WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 10, y, 640, 500, ID_CONSOLE);
        SendMessageW(g_con, WM_SETFONT, (WPARAM)g_mono, TRUE); SendMessageW(g_con, EM_SETLIMITTEXT, 0, 0);
        SetWindowTextW(g_con, L"The server's console appears here while it runs.\r\n");

        /* ---------- page 2: players ---------- */
        g_curPage = 2; y = TOP + 4; bx = 10;
        const WCHAR *pb[8] = { L"Refresh", L"Kick", L"Ban", L"Bans list...", L"Steam profile", L"Add to Team 1", L"Add to Team 2", L"Make admin" };
        int pid[8] = { ID_PREFRESH, ID_PKICK, ID_PBAN, ID_PBANS, ID_PPROFILE, ID_PTEAM1, ID_PTEAM2, ID_PADMIN }, pw[8] = { 60, 45, 45, 75, 85, 90, 90, 80 };
        for (int i = 0; i < 8; i++) { Mk(h, L"BUTTON", pb[i], WS_TABSTOP, 0, bx, y, pw[i], 26, pid[i]); bx += pw[i] + 5; }
        y += 32;
        g_pstat2 = Mk(h, L"STATIC", L"Server is not running.", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0, 10, y, 640, 18, ID_PSTAT2);
        y += 22;
        g_plist = Mk(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, WS_EX_CLIENTEDGE, 10, y, 640, 400, ID_PLIST);
        SendMessageW(g_plist, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        AddCol(g_plist, 0, L"User ID", 60); AddCol(g_plist, 1, L"Name", 170); AddCol(g_plist, 2, L"SteamID64", 140);
        AddCol(g_plist, 3, L"Ping", 50); AddCol(g_plist, 4, L"Time", 70); AddCol(g_plist, 5, L"Address", 140);

        /* ---------- page 3: match ---------- */
        g_curPage = 3; y = TOP + 4;
        LBL(L"Team 1 name:", 10, y, 75); g_t1name = EDT(ID_T1NAME, 90, y, 230, 0); SetWindowTextW(g_t1name, L"Team 1");
        LBL(L"Team 2 name:", 340, y, 75); g_t2name = EDT(ID_T2NAME, 420, y, 230, 0); SetWindowTextW(g_t2name, L"Team 2");
        y += 28;
        LBL(L"Players - one Steam profile per line (profile link, SteamID64, STEAM_0:x:y or custom URL name):", 10, y, 640);
        y += 22;
        g_t1 = Mk(h, L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_TABSTOP, WS_EX_CLIENTEDGE, 10, y, 310, 150, ID_T1);
        g_t2 = Mk(h, L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_TABSTOP, WS_EX_CLIENTEDGE, 340, y, 310, 150, ID_T2);
        SendMessageW(g_t1, WM_SETFONT, (WPARAM)g_mono, TRUE); SendMessageW(g_t2, WM_SETFONT, (WPARAM)g_mono, TRUE);
        y += 158;
        LBL(L"Players per team:", 10, y, 95); g_ppt = EDT(ID_PPT, 108, y, 40, ES_NUMBER); SetWindowTextW(g_ppt, L"5");
        LBL(L"Series:", 165, y, 40);
        g_series = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 0, 210, y, 70, 200, ID_SERIES);
        SendMessageW(g_series, CB_ADDSTRING, 0, (LPARAM)L"Bo1"); SendMessageW(g_series, CB_ADDSTRING, 0, (LPARAM)L"Bo2");
        SendMessageW(g_series, CB_ADDSTRING, 0, (LPARAM)L"Bo3"); SendMessageW(g_series, CB_ADDSTRING, 0, (LPARAM)L"Bo5");
        SendMessageW(g_series, CB_SETCURSEL, 0, 0);
        LBL(L"Sides:", 295, y, 40);
        g_sides = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 0, 340, y, 150, 200, ID_SIDES);
        SendMessageW(g_sides, CB_ADDSTRING, 0, (LPARAM)L"Knife round"); SendMessageW(g_sides, CB_ADDSTRING, 0, (LPARAM)L"Team 1 starts CT");
        SendMessageW(g_sides, CB_ADDSTRING, 0, (LPARAM)L"Team 2 starts CT"); SendMessageW(g_sides, CB_SETCURSEL, 0, 0);
        y += 30;
        LBL(L"Maps:", 10, y, 40);
        g_mapmode = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 0, 55, y, 265, 200, ID_MAPMODE);
        SendMessageW(g_mapmode, CB_ADDSTRING, 0, (LPARAM)L"Pick the maps in order (dropdowns)");
        SendMessageW(g_mapmode, CB_ADDSTRING, 0, (LPARAM)L"Veto from the ticked map pool (.ban / .pick)");
        SendMessageW(g_mapmode, CB_SETCURSEL, 0, 0);
        y += 30;
        LBL(L"Map pool for veto (tick the maps):", 10, y, 300);
        LBL(L"Maps in order:", 340, y, 200);
        y += 20;
        g_maps = Mk(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_NOCOLUMNHEADER | WS_TABSTOP, WS_EX_CLIENTEDGE, 10, y, 310, 148, ID_MAPS);
        SendMessageW(g_maps, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        AddCol(g_maps, 0, L"Map", 280);
        for (int i = 0; i < NPOOL; i++) { LVITEMW it = { 0 }; it.mask = LVIF_TEXT; it.iItem = i; it.pszText = (WCHAR *)POOL[i]; SendMessageW(g_maps, LVM_INSERTITEMW, 0, (LPARAM)&it); }
        for (int i = 0; i < 5; i++) {
            WCHAR lb[16]; _snwprintf(lb, 16, L"Map %d:", i + 1);
            LBL(lb, 340, y + i * 30, 45);
            g_ms[i] = Mk(h, L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_SORT | WS_VSCROLL | WS_TABSTOP, 0, 390, y + i * 30, 260, 320, ID_MS1 + i);
        }
        LBL(L"Extra pool maps:", 10, y + 155, 95); g_extra = EDT(ID_EXTRAMAPS, 110, y + 152, 210, 0);
        y += 184;
        Mk(h, L"BUTTON", L"Resolve Steam profiles", WS_TABSTOP, 0, 10, y, 150, 28, ID_RESOLVE);
        Mk(h, L"BUTTON", L"Load match on server", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 165, y, 150, 28, ID_LOADMATCH);
        Mk(h, L"BUTTON", L"End match", WS_TABSTOP, 0, 320, y, 90, 28, ID_ENDMATCH);
        y += 34;
        g_mstat = Mk(h, L"STATIC", L"Load match writes game\\csgo\\cs2launcher_match.json and runs matchzy_loadmatch (Competitive or Wingman mode, plugins on).", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0, 10, y, 640, 18, ID_MSTAT);

        /* ---------- page 4: connect ---------- */
        g_curPage = 4; y = TOP + 6;
#define RO(id, x, yy, w) Mk(h, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP, WS_EX_CLIENTEDGE, x, yy, w, 22, id)
        LBL(L"Local IP:", 10, y, 100); g_cLocal = RO(ID_CLOCAL, 115, y, 420); y += 28;
        LBL(L"External IP:", 10, y, 100); g_cExt = RO(ID_CEXT, 115, y, 300);
        Mk(h, L"BUTTON", L"Refresh", WS_TABSTOP, 0, 420, y, 70, 22, ID_CEXTREF); y += 28;
        LBL(L"Port:", 10, y, 100); g_cPort = RO(ID_CPORT, 115, y, 120); y += 28;
        LBL(L"Server password:", 10, y, 100); g_cPass = RO(ID_CPASS, 115, y, 300);
        g_cShowPw = Mk(h, L"BUTTON", L"Show", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 420, y, 60, 22, ID_CSHOWPW); y += 28;
        LBL(L"RCON password:", 10, y, 100); g_cRcon = RO(ID_CRCON, 115, y, 300);
        Mk(h, L"BUTTON", L"Copy", WS_TABSTOP, 0, 420, y, 60, 22, ID_CCOPYR); y += 40;
        LBL(L"Players on your network - paste into the CS2 console (~):", 10, y, 500); y += 20;
        g_cCmdL = RO(ID_CCMDL, 10, y, 560); Mk(h, L"BUTTON", L"Copy", WS_TABSTOP, 0, 575, y, 60, 22, ID_CCOPY1); y += 26;
        g_cLinkL = RO(ID_CLINKL, 10, y, 560); Mk(h, L"BUTTON", L"Copy", WS_TABSTOP, 0, 575, y, 60, 22, ID_CCOPY2); y += 36;
        LBL(L"Players over the internet (Internet mode + port forwarded):", 10, y, 500); y += 20;
        g_cCmdE = RO(ID_CCMDE, 10, y, 560); Mk(h, L"BUTTON", L"Copy", WS_TABSTOP, 0, 575, y, 60, 22, ID_CCOPY3); y += 26;
        g_cLinkE = RO(ID_CLINKE, 10, y, 560); Mk(h, L"BUTTON", L"Copy", WS_TABSTOP, 0, 575, y, 60, 22, ID_CCOPY4); y += 36;
        g_cNote = Mk(h, L"STATIC", L"", 0, 0, 10, y, 640, 90, ID_CNOTE);
        LBL(L"Players launch CS2 with the 128/256 tick game launcher at the same rate. steam:// links open CS2 and join directly.", 10, y + 96, 640);

        /* ---------- page 5: info ---------- */
        g_curPage = 5; y = TOP + 4;
        Mk(h, L"BUTTON", L"Check tick rate + plugins", WS_TABSTOP, 0, 10, y, 170, 26, ID_INFOCHECK);
        LBL(L"Checks the patched DLLs and plugin files on disk, and asks the running server over RCON.", 190, y + 2, 470);
        y += 32;
        g_info = Mk(h, L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, WS_EX_CLIENTEDGE, 10, y, 640, 450, ID_INFO);
        SendMessageW(g_info, WM_SETFONT, (WPARAM)g_mono, TRUE);

        /* ---------- page 6: admins ---------- */
        g_curPage = 6; y = TOP + 4;
        LBL(L"Steam profile:", 10, y, 80); g_aprof = EDT(ID_APROF, 95, y, 300, 0);
        LBL(L"Name:", 405, y, 40); g_aname = EDT(ID_ANAME, 445, y, 205, 0);
        y += 30;
        LBL(L"Permissions (CounterStrikeSharp flags):", 10, y, 260);
        g_amz = Mk(h, L"BUTTON", L"MatchZy admin too (match commands)", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 405, y, 245, 20, ID_AMZ);
        SendMessageW(g_amz, BM_SETCHECK, BST_CHECKED, 0);
        y += 20;
        g_aflags = Mk(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_NOCOLUMNHEADER | WS_TABSTOP, WS_EX_CLIENTEDGE, 10, y, 385, 150, ID_AFLAGS);
        SendMessageW(g_aflags, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        AddCol(g_aflags, 0, L"Flag", 160); AddCol(g_aflags, 1, L"Allows", 190);
        for (int i = 0; i < NAFLAGS; i++) { LVITEMW it = { 0 }; it.mask = LVIF_TEXT; it.iItem = i; it.pszText = (WCHAR *)AFLAGS[i]; SendMessageW(g_aflags, LVM_INSERTITEMW, 0, (LPARAM)&it);
            it.iSubItem = 1; it.pszText = (WCHAR *)AFDESC[i]; SendMessageW(g_aflags, LVM_SETITEMTEXTW, i, (LPARAM)&it); }
        { LVITEMW it = { 0 }; it.stateMask = LVIS_STATEIMAGEMASK; it.state = INDEXTOSTATEIMAGEMASK(2); SendMessageW(g_aflags, LVM_SETITEMSTATE, 0, (LPARAM)&it); }
        Mk(h, L"BUTTON", L"Add / update admin", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 405, y, 150, 28, ID_AADD);
        Mk(h, L"BUTTON", L"Remove selected", WS_TABSTOP, 0, 405, y + 34, 150, 28, ID_AREMOVE);
        Mk(h, L"BUTTON", L"Reload from server", WS_TABSTOP, 0, 405, y + 68, 150, 28, ID_ARELOAD);
        Mk(h, L"STATIC", L"Click an admin below to edit them.\r\n@css/root = full admin.", 0, 0, 405, y + 104, 245, 40, 0);
        y += 158;
        g_astat = Mk(h, L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0, 10, y, 640, 18, ID_ASTAT);
        y += 22;
        g_alist = Mk(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, WS_EX_CLIENTEDGE, 10, y, 640, 200, ID_ALIST);
        SendMessageW(g_alist, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        AddCol(g_alist, 0, L"Name", 150); AddCol(g_alist, 1, L"SteamID64", 140); AddCol(g_alist, 2, L"MatchZy", 60); AddCol(g_alist, 3, L"Flags", 280);

        SetWindowPos(g_tab, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        LoadCfgToUi();
        FillMapCombos();
        UpdateMapPick();
        ShowPage(0);
        SetTimer(h, TIMER_POLL, 15000, NULL);
        return 0;
    }
    case WM_SIZE: {
        int cw = LOWORD(l), ch = HIWORD(l);
        MoveWindow(g_tab, 8, 6, cw - 16, 26, TRUE);
        RECT r;
        GetWindowRect(g_log, &r); MapWindowPoints(NULL, h, (POINT *)&r, 2); MoveWindow(g_log, 10, r.top, cw - 20, ch - r.top - 10, TRUE);
        GetWindowRect(g_con, &r); MapWindowPoints(NULL, h, (POINT *)&r, 2); MoveWindow(g_con, 10, r.top, cw - 20, ch - r.top - 10, TRUE);
        MoveWindow(g_conHost, 10, r.top, cw - 20, ch - r.top - 10, TRUE); FitEmbedded();
        GetWindowRect(g_info, &r); MapWindowPoints(NULL, h, (POINT *)&r, 2); MoveWindow(g_info, 10, r.top, cw - 20, ch - r.top - 10, TRUE);
        GetWindowRect(g_alist, &r); MapWindowPoints(NULL, h, (POINT *)&r, 2); MoveWindow(g_alist, 10, r.top, cw - 20, ch - r.top - 10, TRUE);
        GetWindowRect(g_plist, &r); MapWindowPoints(NULL, h, (POINT *)&r, 2); MoveWindow(g_plist, 10, r.top, cw - 20, ch - r.top - 10, TRUE);
        return 0;
    }
    case WM_GETMINMAXINFO: ((MINMAXINFO *)l)->ptMinTrackSize.x = 690; ((MINMAXINFO *)l)->ptMinTrackSize.y = 640; return 0;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)l;
        if (nh->hwndFrom == g_tab && nh->code == TCN_SELCHANGE) ShowPage((int)SendMessageW(g_tab, TCM_GETCURSEL, 0, 0));
        if (nh->hwndFrom == g_plist && nh->code == NM_DBLCLK) PostMessageW(h, WM_COMMAND, ID_PPROFILE, 0);
        if (nh->hwndFrom == g_alist && (nh->code == NM_CLICK || nh->code == LVN_ITEMCHANGED)) SelectAdmin();
        return 0;
    }
    case WM_TIMER:
        if (w == TIMER_POLL && g_running) PollPlayers();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_BROWSE: Browse(g_root, L"Server folder (the folder that contains \"game\")"); break;
        case ID_BROWSE2: Browse(g_scmd, L"SteamCMD folder (steamcmd.exe is used from here, or downloaded here)"); break;
        case ID_INSTALL: StartJob(J_INSTALL); break;
        case ID_PLUGINS: StartJob(J_PLUGINS); break;
        case ID_CHECK: StartJob(J_CHECK); break;
        case ID_FIREWALL: StartJob(J_FIREWALL); break;
        case ID_START: StartJob(J_START); break;
        case ID_STOP: StopServer(); break;
        case ID_RESTORE: StartJob(J_RESTORE); break;
        case ID_SEND: SendServerCommand(); break;
        case ID_CONCLEAR: SetWindowTextW(g_con, L""); break;
        case ID_INFOCHECK: CheckTickRate(); break;
        case ID_CHMAPBTN: ChangeMap(); break;
        case ID_MAPMODE: case ID_SERIES: if (HIWORD(w) == CBN_SELCHANGE) UpdateMapPick(); break;
        case ID_ROOT: if (HIWORD(w) == EN_KILLFOCUS) FillMapCombos(); break;
        case ID_EMBED: {
            BOOL on = SendMessageW(g_embedChk, BM_GETCHECK, 0, 0) == BST_CHECKED;
            WritePrivateProfileStringW(L"server", L"embed", on ? L"1" : L"0", g_ini);
            if (!on) Unembed();
            else if (g_running && g_proc) { HANDLE t = CreateThread(NULL, 0, EmbedWatch, (LPVOID)(UINT_PTR)GetProcessId(g_proc), 0, NULL); if (t) CloseHandle(t); }
            break; }
        case ID_CEXTREF: g_extIp[0] = 0; UpdateConnInfo(TRUE); break;
        case ID_CSHOWPW: UpdateConnInfo(FALSE); break;
        case ID_CCOPY1: CopyText(g_cCmdL); break;
        case ID_CCOPY2: CopyText(g_cLinkL); break;
        case ID_CCOPY3: CopyText(g_cCmdE); break;
        case ID_CCOPY4: CopyText(g_cLinkE); break;
        case ID_CCOPYR: { Cfg cc; ReadUi(&cc); HWND t = g_cRcon; SetWindowTextW(t, cc.rcon[0] ? cc.rcon : g_autoRcon); CopyText(t); UpdateConnInfo(FALSE); break; }
        case ID_OPENLOGS: ShellExecuteW(h, L"open", g_logDir, NULL, NULL, SW_SHOWNORMAL); break;
        case ID_PREFRESH: PollPlayers(); break;
        case ID_PKICK: { int i = SelectedPlayer(); if (i < 0) break;
            WCHAR q[200]; _snwprintf(q, 200, L"Kick %s?", g_pl[i].name);
            if (MessageBoxW(h, q, APP_TITLE, MB_YESNO | MB_ICONQUESTION) == IDYES) { KickPlayer(&g_pl[i], L"Kicked by the server admin"); Sleep(300); PollPlayers(); }
            break; }
        case ID_PBAN: BanSelected(); break;
        case ID_PBANS: OpenBans(); break;
        case ID_PPROFILE: { int i = SelectedPlayer(); if (i < 0) break;
            if (!g_pl[i].steamid[0]) { MessageBoxW(h, L"No SteamID for this player.", APP_TITLE, MB_ICONINFORMATION); break; }
            WCHAR u[128]; _snwprintf(u, 128, L"https://steamcommunity.com/profiles/%s", g_pl[i].steamid); ShellExecuteW(h, L"open", u, NULL, NULL, SW_SHOWNORMAL); break; }
        case ID_PTEAM1: PlayerToTeam(g_t1); break;
        case ID_PTEAM2: PlayerToTeam(g_t2); break;
        case ID_PADMIN: { int i = SelectedPlayer(); if (i < 0) break;
            if (!g_pl[i].steamid[0]) { MessageBoxW(h, L"No SteamID for this player - add them on the Admins tab with their profile link.", APP_TITLE, MB_ICONINFORMATION); break; }
            SendMessageW(g_tab, TCM_SETCURSEL, 6, 0); ShowPage(6);
            SetWindowTextW(g_aprof, g_pl[i].steamid); SetWindowTextW(g_aname, g_pl[i].name); SetFocus(g_aflags); break; }
        case ID_AADD: AddAdmin(); break;
        case ID_AREMOVE: RemoveAdmin(); break;
        case ID_ARELOAD: LoadAdmins(); break;
        case ID_RESOLVE: ResolveProfiles(); break;
        case ID_LOADMATCH: LoadMatch(); break;
        case ID_ENDMATCH:
            if (MessageBoxW(h, L"End the current MatchZy match?", APP_TITLE, MB_YESNO | MB_ICONQUESTION) == IDYES) { SendRawW(L"css_endmatch"); SetWindowTextW(g_mstat, L"Sent css_endmatch."); }
            break;
        case ID_PAUSE:
            if (!g_busy) break;
            if (!g_paused) { InterlockedExchange(&g_paused, 1); JobForEach(0); SetWindowTextW(g_pause, L"Resume");
                             SendMessageW(g_prog, PBM_SETSTATE, PBST_PAUSED, 0); SetWindowTextW(g_pstat, L"Paused"); Log(L"Paused."); }
            else { InterlockedExchange(&g_paused, 0); JobForEach(1); SetWindowTextW(g_pause, L"Pause");
                   SendMessageW(g_prog, PBM_SETSTATE, PBST_NORMAL, 0); SetWindowTextW(g_pstat, L"Resuming..."); Log(L"Resumed."); }
            break;
        case ID_CANCEL:
            if (!g_busy) break;
            if (MessageBoxW(h, L"Cancel the running task?", APP_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) break;
            InterlockedExchange(&g_cancelReq, 1);
            if (g_paused) { InterlockedExchange(&g_paused, 0); JobForEach(1); SetWindowTextW(g_pause, L"Pause"); SendMessageW(g_prog, PBM_SETSTATE, PBST_NORMAL, 0); }
            EnterCriticalSection(&g_jobLock); if (g_job) TerminateJobObject(g_job, 1); LeaveCriticalSection(&g_jobLock);
            Log(L"Cancelling...");
            break;
        }
        return 0;
    case WM_APP_LOG: {
        WCHAR *s = (WCHAR *)l; int n = GetWindowTextLengthW(g_log);
        if (n > 2000000) { SendMessageW(g_log, EM_SETSEL, 0, n / 2); SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L""); n = GetWindowTextLengthW(g_log); }
        SendMessageW(g_log, EM_SETSEL, n, n); SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)s); free(s); return 0;
    }
    case WM_APP_CON: {
        WCHAR *s = (WCHAR *)l; int n = GetWindowTextLengthW(g_con);
        if (n > 3000000) { SendMessageW(g_con, EM_SETSEL, 0, n / 2); SendMessageW(g_con, EM_REPLACESEL, FALSE, (LPARAM)L""); n = GetWindowTextLengthW(g_con); }
        SendMessageW(g_con, EM_SETSEL, n, n); SendMessageW(g_con, EM_REPLACESEL, FALSE, (LPARAM)s); free(s); return 0;
    }
    case WM_APP_EMBED:
        if (w == 1) { if (SendMessageW(g_embedChk, BM_GETCHECK, 0, 0) == BST_CHECKED && g_running) Embed((HWND)l); }
        else if (SendMessageW(g_embedChk, BM_GETCHECK, 0, 0) == BST_CHECKED) { HANDLE t = CreateThread(NULL, 0, EmbedWatch, (LPVOID)(UINT_PTR)w, 0, NULL); if (t) CloseHandle(t); }
        return 0;
    case WM_APP_ADMINRES: {
        AdminRes *r = (AdminRes *)l;
        if (r->err[0]) { WCHAR m[300]; _snwprintf(m, 300, L"Could not resolve that profile: %s", r->err); SetWindowTextW(g_astat, m); }
        else AddAdminResolved(r->sid, r->name);
        free(r); return 0;
    }
    case WM_APP_INFO: { WCHAR *s = (WCHAR *)l; SetWindowTextW(g_info, s); free(s); return 0; }
    case WM_APP_EXTIP: { WCHAR *s = (WCHAR *)l; wcsncpy(g_extIp, s, 63); free(s); if (g_curPage == 4) UpdateConnInfo(FALSE); return 0; }
    case WM_APP_PLAYERS: { PlayerMsg *pm = (PlayerMsg *)l; FillPlayers(pm); free(pm); return 0; }
    case WM_APP_RESOLVED: {
        Resolved *r = (Resolved *)l;
        SetWindowTextW(g_t1, r->t1); SetWindowTextW(g_t2, r->t2);
        BOOL bad = wcsstr(r->t1, L"<--") || wcsstr(r->t2, L"<--");
        SetWindowTextW(g_mstat, bad ? L"Some profiles could not be resolved (marked <--). Fix those lines and resolve again." : L"Profiles resolved. Check the names, then Load match on server.");
        free(r->t1); free(r->t2); free(r); return 0;
    }
    case WM_APP_DONE: InterlockedExchange(&g_busy, 0); UpdateButtons(); return 0;
    case WM_APP_STATE:
        UpdateButtons();
        if (g_curPage == 4) UpdateConnInfo(FALSE);
        if (g_running && g_curPage == 0) { SendMessageW(g_tab, TCM_SETCURSEL, 1, 0); ShowPage(1); }
        if (!g_running) { SendMessageW(g_plist, LVM_DELETEALLITEMS, 0, 0); g_npl = 0; SetWindowTextW(g_pstat2, L"Server is not running."); g_emb = NULL; UpdateConsoleView(); }
        return 0;
    case WM_APP_PROG: {
        Prog *p = (Prog *)l;
        LONG_PTR st = GetWindowLongPtrW(g_prog, GWL_STYLE);
        if (p->pct10 < 0) { if (!(st & PBS_MARQUEE)) { SetWindowLongPtrW(g_prog, GWL_STYLE, st | PBS_MARQUEE); SendMessageW(g_prog, PBM_SETMARQUEE, TRUE, 40); } }
        else { if (st & PBS_MARQUEE) { SendMessageW(g_prog, PBM_SETMARQUEE, FALSE, 0); SetWindowLongPtrW(g_prog, GWL_STYLE, st & ~PBS_MARQUEE); }
               SendMessageW(g_prog, PBM_SETPOS, p->pct10, 0); }
        SetWindowTextW(g_pstat, g_paused ? L"Paused" : p->text);
        free(p); return 0;
    }
    case WM_CLOSE:
        if (g_running && MessageBoxW(h, L"The server is running. Stop it and exit?", APP_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        { Cfg c; ReadUi(&c); SaveCfg(&c); }
        StopServer();
        g_emb = NULL;
        DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static BOOL ExtractPs1(void)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_PS1), (LPCWSTR)RT_RCDATA);
    if (!r) return FALSE;
    HGLOBAL g = LoadResource(NULL, r); DWORD n = SizeofResource(NULL, r); const void *p = LockResource(g);
    HANDLE f = CreateFileW(g_ps1, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    DWORD w; static const BYTE bom[3] = { 0xEF, 0xBB, 0xBF };
    WriteFile(f, bom, 3, &w, NULL); WriteFile(f, p, n, &w, NULL); CloseHandle(f);
    return TRUE;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev;
    if (cmd && wcsstr(cmd, L"--steamclient-test")) {
        WCHAR *q = wcschr(wcsstr(cmd, L"--steamclient-test"), L'"');
        WCHAR dll[MAX_PATH] = L"";
        if (q) { wcsncpy(dll, q + 1, MAX_PATH - 1); WCHAR *e = wcschr(dll, L'"'); if (e) *e = 0; }
        ExitProcess((UINT)SteamClientTestChild(dll));
    }
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\CS2_server_launcher");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND o = FindWindowW(L"CS2ServerLauncher", NULL); if (o) { ShowWindow(o, SW_RESTORE); SetForegroundWindow(o); }
        return 0;
    }
    CoInitialize(NULL);
    InitializeCriticalSection(&g_jobLock);
    InitializeCriticalSection(&g_capLock);
    InitializeCriticalSection(&g_rconLock);
    { WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd); }
    g_capDone = CreateEventW(NULL, TRUE, FALSE, NULL);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES }; InitCommonControlsEx(&icc);
    WCHAR la[MAX_PATH]; if (!GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) wcscpy(la, L".");
    _snwprintf(g_dataDir, MAX_PATH, L"%s\\CS2-128-tick", la); CreateDirectoryW(g_dataDir, NULL);
    _snwprintf(g_ini, MAX_PATH, L"%s\\server_launcher.ini", g_dataDir);
    _snwprintf(g_logFile, MAX_PATH, L"%s\\server_launcher.log", g_dataDir);
    _snwprintf(g_logDir, MAX_PATH, L"%s\\logs", g_dataDir); CreateDirectoryW(g_logDir, NULL);
    { WCHAR st[MAX_PATH]; _snwprintf(st, MAX_PATH, L"%s\\backup", g_dataDir); tp_set_store(st); }
    _snwprintf(g_ps1, MAX_PATH, L"%s\\cs2server.ps1", g_dataDir);
    _snwprintf(g_bansFile, MAX_PATH, L"%s\\bans.txt", g_dataDir);
    /* RCON password the launcher uses when none is set (random, kept in the ini) */
    GetPrivateProfileStringW(L"server", L"autorcon", L"", g_autoRcon, 40, g_ini);
    if (wcslen(g_autoRcon) < 16) {
        typedef BOOLEAN (WINAPI *RtlGenRandom_t)(PVOID, ULONG);
        RtlGenRandom_t gen = (RtlGenRandom_t)(void *)GetProcAddress(LoadLibraryW(L"advapi32.dll"), "SystemFunction036");
        BYTE rb[24] = { 0 }; if (gen) gen(rb, sizeof rb); else for (int i = 0; i < 24; i++) rb[i] = (BYTE)(GetTickCount() * (i + 7) ^ GetCurrentProcessId() >> i);
        static const WCHAR al[] = L"abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        for (int i = 0; i < 24; i++) g_autoRcon[i] = al[rb[i] % (sizeof al / sizeof al[0] - 1)];
        g_autoRcon[24] = 0;
        WritePrivateProfileStringW(L"server", L"autorcon", g_autoRcon, g_ini);
    }

    WNDCLASSW wc = { 0 }; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"CS2ServerLauncher";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, APP_TITLE L" " APP_VER, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 700, 720, NULL, NULL, hi, NULL);
    ShowWindow(g_wnd, show); UpdateWindow(g_wnd); UpdateButtons();

    Log(L"%s %s - LAN / -insecure servers only. Players must use the same tick rate.", APP_TITLE, APP_VER);
    Log(L"Built-in patch engine for CS2 %hs. Log: %s", TP_BUILD_1420_NAME, g_logFile);
    if (!ExtractPs1()) Log(L"ERROR: could not write helper script %s", g_ps1);
    { WCHAR sv[MAX_PATH]; GetPrivateProfileStringW(L"steamclient", L"saved", L"", sv, MAX_PATH, g_ini);
      if (sv[0]) { Log(L"Putting back the Steam registry entry left over from last time:"); g_redirectTo[0] = 0; RestoreSteamClientReg(); } }
    if (!InterlockedExchange(&g_extBusy, 1)) { HANDLE t = CreateThread(NULL, 0, ExtIpThread, NULL, 0, NULL); if (t) CloseHandle(t); else g_extBusy = 0; }
    Cfg c; ReadUi(&c);
    if (HasServer(c.root)) { Log(L"Server folder: %s", c.root); tp_status(c.root, Log); }
    else Log(L"No server in %s yet - pick a folder and click Install / Update.", c.root);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && msg.hwnd == g_cmd) { SendServerCommand(); continue; }
        if ((msg.hwnd == g_t1 || msg.hwnd == g_t2) && msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) { TranslateMessage(&msg); DispatchMessageW(&msg); continue; }
        if (!IsDialogMessageW(g_wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return 0;
}
