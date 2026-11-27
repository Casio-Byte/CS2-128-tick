/*
 * CS2 128/256-tick Game Launcher (client) - self-contained.
 * Patch engine: ../engine/tickpatch.h (+ generated sites_1420.h), file side
 * in ../engine/tp_files.h. No external patcher is needed.
 *
 * Flow: pick 128/256 -> Launch CS2 -> wait for Steam (start it if needed)
 *       -> patch -> steam.exe -applaunch 730 -insecure -> watch the game's own
 *       cs2.exe by full path -> log its real command line -> on exit wait for a
 *       possible self-relaunch -> restore 64 tick (retry while DLLs are held).
 * A session file in %LOCALAPPDATA%\CS2-128-tick makes the next start restore
 * 64 tick if the launcher was closed mid-game.
 *
 * For LAN / -insecure servers only. Never use on official Valve servers.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -municode -mwindows cs2_tick_launcher.c \
 *        -o cs2_tick_launcher.exe -lshlwapi
 */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <wchar.h>
#include "../engine/tp_files.h"

#define APP_TITLE L"CS2 128/256 Tick Launcher"
#define APP_VER   L"2.3"

enum { ID_R128 = 101, ID_R256, ID_LAUNCH, ID_RESTORE, ID_LOG, ID_TAB, ID_INFO, ID_INFOCHECK };
#define WM_APP_LOG  (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)
#define WM_APP_INFO (WM_APP + 3)

static HWND g_wnd, g_log, g_r128, g_r256, g_launch, g_restore, g_tab, g_info, g_infoBtn, g_infoLbl, g_tickLbl;
static volatile LONG g_busy = 0;
static WCHAR g_dataDir[MAX_PATH], g_sessionFile[MAX_PATH], g_iniFile[MAX_PATH], g_logFile[MAX_PATH];


/* ------------------------------------------------------------------ log */
static void Log(const WCHAR *fmt, ...)
{
    WCHAR body[2048], line[2200];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(body, 2047, fmt, ap); body[2047] = 0;
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    _snwprintf(line, 2199, L"[%02d:%02d:%02d] %s\r\n", st.wHour, st.wMinute, st.wSecond, body);
    line[2199] = 0;
    /* file log */
    HANDLE f = CreateFileW(g_logFile, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        char u8[6600]; int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, u8, sizeof u8, NULL, NULL);
        DWORD w; if (n > 1) WriteFile(f, u8, n - 1, &w, NULL);
        CloseHandle(f);
    }
    WCHAR *copy = _wcsdup(line);
    if (copy && !PostMessageW(g_wnd, WM_APP_LOG, 0, (LPARAM)copy)) free(copy);
}

/* -------------------------------------------------------------- helpers */
static BOOL FileExists(const WCHAR *p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

static BOOL RegStr(HKEY root, const WCHAR *key, const WCHAR *val, WCHAR *out, DWORD cch)
{
    DWORD sz = cch * sizeof(WCHAR), type;
    if (RegGetValueW(root, key, val, RRF_RT_REG_SZ, &type, out, &sz) != ERROR_SUCCESS) return FALSE;
    for (WCHAR *c = out; *c; c++) if (*c == L'/') *c = L'\\';
    return TRUE;
}
static DWORD RegDword(HKEY root, const WCHAR *key, const WCHAR *val)
{
    DWORD v = 0, sz = sizeof v;
    if (RegGetValueW(root, key, val, RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS) return 0;
    return v;
}

/* ------------------------------------------------------- find CS2 install */
static BOOL CheckCs2Root(const WCHAR *root)
{
    WCHAR p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%s\\game\\bin\\win64\\engine2.dll", root); p[MAX_PATH-1] = 0;
    return FileExists(p);
}

/* Parses libraryfolders.vdf for "path" entries; checks each for CS2 */
static BOOL FindCs2(WCHAR *root)
{
    WCHAR steam[MAX_PATH];
    if (!RegStr(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", steam, MAX_PATH)) return FALSE;
    WCHAR libs[16][MAX_PATH]; int nlib = 0;
    wcscpy(libs[nlib++], steam);
    WCHAR vdf[MAX_PATH]; _snwprintf(vdf, MAX_PATH, L"%s\\steamapps\\libraryfolders.vdf", steam);
    FILE *f = _wfopen(vdf, L"rb");
    if (f) {
        char line[2048];
        while (fgets(line, sizeof line, f) && nlib < 16) {
            char *k = strstr(line, "\"path\"");
            if (!k) continue;
            char *q1 = strchr(k + 6, '"'); if (!q1) continue;
            char *q2 = strchr(q1 + 1, '"'); if (!q2) continue;
            *q2 = 0;
            char un[MAX_PATH * 2]; int j = 0;
            for (char *s = q1 + 1; *s && j < (int)sizeof un - 1; s++) {
                if (s[0] == '\\' && s[1] == '\\') s++;
                un[j++] = *s;
            }
            un[j] = 0;
            MultiByteToWideChar(CP_UTF8, 0, un, -1, libs[nlib], MAX_PATH);
            nlib++;
        }
        fclose(f);
    }
    for (int i = 0; i < nlib; i++) {
        WCHAR cand[MAX_PATH];
        _snwprintf(cand, MAX_PATH, L"%s\\steamapps\\common\\Counter-Strike Global Offensive", libs[i]);
        cand[MAX_PATH-1] = 0;
        if (CheckCs2Root(cand)) { GetFullPathNameW(cand, MAX_PATH, root, NULL); return TRUE; }
    }
    return FALSE;
}

/* ------------------------------------------------------------ processes */
static BOOL ImagePath(DWORD pid, WCHAR *out, DWORD cch)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return FALSE;
    BOOL ok = QueryFullProcessImageNameW(h, 0, out, &cch);
    CloseHandle(h);
    return ok;
}

/* Finds a running process whose full image path equals `path`. Returns pid or 0. */
static DWORD FindProcessByPath(const WCHAR *exeName, const WCHAR *path)
{
    DWORD found = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof pe };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exeName)) continue;
        WCHAR img[MAX_PATH];
        if (path == NULL) { found = pe.th32ProcessID; break; }
        if (ImagePath(pe.th32ProcessID, img, MAX_PATH) && !_wcsicmp(img, path)) { found = pe.th32ProcessID; break; }
    }
    CloseHandle(snap);
    return found;
}

typedef LONG (NTAPI *NtQIP_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } USTR;

/* ProcessCommandLineInformation (class 60), Windows 8.1+ */
static BOOL GetCommandLineOf(DWORD pid, WCHAR *out, DWORD cch)
{
    static NtQIP_t q;
    if (!q) q = (NtQIP_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (!q) return FALSE;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return FALSE;
    ULONG need = 0; BOOL ok = FALSE;
    q(h, 60, NULL, 0, &need);
    if (need == 0) need = 65536;
    BYTE *buf = (BYTE *)malloc(need);
    if (buf && q(h, 60, buf, need, &need) >= 0) {
        USTR *u = (USTR *)buf;
        DWORD n = u->Length / sizeof(WCHAR);
        if (n >= cch) n = cch - 1;
        memcpy(out, u->Buffer, n * sizeof(WCHAR)); out[n] = 0;
        ok = TRUE;
    }
    free(buf); CloseHandle(h);
    return ok;
}

static BOOL ProcessAlive(DWORD pid, const WCHAR *exeName)
{
    if (!pid) return FALSE;
    WCHAR img[MAX_PATH];
    if (!ImagePath(pid, img, MAX_PATH)) return FALSE;
    return !_wcsicmp(PathFindFileNameW(img), exeName);
}

/* --------------------------------------------------------------- Steam */
static BOOL SteamLoggedIn(void)
{
    DWORD pid  = RegDword(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"pid");
    DWORD user = RegDword(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser");
    return user != 0 && ProcessAlive(pid, L"steam.exe");
}

static BOOL SteamExe(WCHAR *out)
{
    if (RegStr(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamExe", out, MAX_PATH) && FileExists(out)) return TRUE;
    WCHAR p[MAX_PATH];
    if (RegStr(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", p, MAX_PATH)) {
        _snwprintf(out, MAX_PATH, L"%s\\steam.exe", p);
        if (FileExists(out)) return TRUE;
    }
    return FALSE;
}

static BOOL RunDetached(const WCHAR *exe, const WCHAR *args)
{
    WCHAR cmd[2048]; _snwprintf(cmd, 2047, L"\"%s\" %s", exe, args); cmd[2047] = 0;
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return FALSE;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return TRUE;
}

static BOOL WaitForSteam(void)
{
    if (SteamLoggedIn()) { Log(L"Steam is running and logged in."); return TRUE; }
    WCHAR exe[MAX_PATH];
    if (!FindProcessByPath(L"steam.exe", NULL)) {
        if (!SteamExe(exe)) { Log(L"ERROR: Steam is not installed (no SteamExe in registry)."); return FALSE; }
        Log(L"Starting Steam: %s", exe);
        if (!RunDetached(exe, L"")) { Log(L"ERROR: could not start Steam (%lu).", GetLastError()); return FALSE; }
    }
    Log(L"Waiting for Steam to log in...");
    for (int i = 0; i < 300; i++) {           /* up to 5 minutes */
        if (SteamLoggedIn()) {
            Log(L"Steam is logged in.");
            Sleep(3000);                       /* give Steam a moment to settle */
            return TRUE;
        }
        Sleep(1000);
    }
    Log(L"ERROR: Steam did not log in within 5 minutes.");
    return FALSE;
}

static BOOL DllsLocked(const WCHAR *root)
{
    for (int i = 0; i < 3; i++) {
        WCHAR d[MAX_PATH]; _snwprintf(d, MAX_PATH, L"%s\\%s", root, TP_REL[i]);
        if (!FileExists(d)) continue;
        HANDLE h = CreateFileW(d, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { if (GetLastError() == ERROR_SHARING_VIOLATION) return TRUE; }
        else CloseHandle(h);
    }
    return FALSE;
}

/* ------------------------------------------------------------- session */
static void WriteSession(int tick, const WCHAR *root)
{
    FILE *f = _wfopen(g_sessionFile, L"w, ccs=UTF-8");
    if (!f) { Log(L"WARNING: could not write session file %s", g_sessionFile); return; }
    fwprintf(f, L"tick=%d\nroot=%s\n", tick, root);
    fclose(f);
}

static BOOL ReadSession(int *tick, WCHAR *root)
{
    FILE *f = _wfopen(g_sessionFile, L"r, ccs=UTF-8");
    if (!f) return FALSE;
    WCHAR line[1024]; *tick = 0; root[0] = 0;
    while (fgetws(line, 1024, f)) {
        line[wcscspn(line, L"\r\n")] = 0;
        if (!wcsncmp(line, L"tick=", 5)) *tick = _wtoi(line + 5);
        else if (!wcsncmp(line, L"root=", 5)) { wcsncpy(root, line + 5, MAX_PATH - 1); root[MAX_PATH-1] = 0; }
    }
    fclose(f);
    return root[0] != 0;
}

/* --------------------------------------------------------------- core */
static void GameExePath(const WCHAR *root, WCHAR *out)
{
    WCHAR t[MAX_PATH]; _snwprintf(t, MAX_PATH, L"%s\\game\\bin\\win64\\cs2.exe", root); t[MAX_PATH-1] = 0;
    GetFullPathNameW(t, MAX_PATH, out, NULL);
}

static BOOL Restore64(const WCHAR *root)
{
    for (int attempt = 1; attempt <= 20; attempt++) {
        if (DllsLocked(root)) {
            Log(L"DLLs are still held by Windows, waiting... (attempt %d/20)", attempt);
            Sleep(3000);
            continue;
        }
        if (tp_restore(root, Log)) { Log(L"Restored 64 tick."); return TRUE; }
        Log(L"Restore not complete yet, retrying in 3 s (attempt %d/20)", attempt);
        Sleep(3000);
    }
    Log(L"ERROR: could not restore 64 tick. Close anything using CS2 and click \"Restore 64 tick now\".");
    return FALSE;
}

/* Waits for the game's own cs2.exe; returns its pid or 0 on timeout. */
static DWORD WaitForGame(const WCHAR *exe, DWORD ms)
{
    for (DWORD t = 0; t < ms; t += 500) {
        DWORD pid = FindProcessByPath(L"cs2.exe", exe);
        if (pid) return pid;
        Sleep(500);
    }
    return 0;
}

static void LogCmdLine(DWORD pid)
{
    WCHAR cl[8192];
    Sleep(1500);
    if (GetCommandLineOf(pid, cl, 8192)) {
        Log(L"CS2 command line: %s", cl);
        /* case-insensitive search for -insecure */
        WCHAR low[8192]; wcscpy(low, cl); _wcslwr(low);
        if (!wcsstr(low, L"-insecure"))
            Log(L"WARNING: -insecure is MISSING from CS2's command line! Do not join any server; close CS2 now.");
        else
            Log(L"-insecure is present.");
    } else {
        Log(L"WARNING: could not read CS2's command line (%lu).", GetLastError());
    }
}

typedef struct { int tick; int restoreOnly; } Job;

static DWORD WINAPI Worker(LPVOID p)
{
    Job job = *(Job *)p; free(p);
    WCHAR root[MAX_PATH], exe[MAX_PATH];

    if (job.restoreOnly) {
        int t; if (!ReadSession(&t, root) && !FindCs2(root)) { Log(L"ERROR: CS2 install not found."); goto done; }
        GameExePath(root, exe);
        if (FindProcessByPath(L"cs2.exe", exe)) { Log(L"CS2 is running; close it first."); goto done; }
        Log(L"Restoring 64 tick in %s", root);
        if (Restore64(root)) DeleteFileW(g_sessionFile);
        goto done;
    }

    if (!FindCs2(root)) { Log(L"ERROR: could not find CS2 (Steam library folders checked)."); goto done; }
    GameExePath(root, exe);
    Log(L"CS2 folder: %s", root);

    if (FindProcessByPath(L"cs2.exe", exe)) { Log(L"CS2 is already running. Close it, then click Launch again."); goto done; }

    if (!WaitForSteam()) goto done;

    /* patch right before launching */
    WriteSession(job.tick, root);              /* written first: a crash mid-patch still restores */
    Log(L"Patching to %d tick:", job.tick);
    if (!tp_apply(root, job.tick, Log) || tp_status(root, NULL) != job.tick) {
        Log(L"ERROR: patch did not apply to all DLLs. Restoring 64 tick and NOT launching.");
        if (Restore64(root)) DeleteFileW(g_sessionFile);
        goto done;
    }
    Log(L"Patched to %d tick.", job.tick);

    WCHAR steam[MAX_PATH];
    if (!SteamExe(steam)) { Log(L"ERROR: steam.exe not found."); Restore64(root); DeleteFileW(g_sessionFile); goto done; }
    Log(L"Launching: steam.exe -applaunch 730 -insecure");
    if (!RunDetached(steam, L"-applaunch 730 -insecure")) {
        Log(L"ERROR: could not run steam.exe (%lu).", GetLastError());
        if (Restore64(root)) DeleteFileW(g_sessionFile);
        goto done;
    }

    Log(L"Waiting for CS2 to start (%s)...", exe);
    DWORD pid = WaitForGame(exe, 180000);
    if (!pid) {
        Log(L"CS2 did not start within 3 minutes. Restoring 64 tick.");
        if (Restore64(root)) DeleteFileW(g_sessionFile);
        goto done;
    }

    for (;;) {
        Log(L"CS2 running (pid %lu).", pid);
        LogCmdLine(pid);
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
        else while (FindProcessByPath(L"cs2.exe", exe) == pid) Sleep(1000);
        Log(L"CS2 exited. Waiting 10 s in case it relaunches itself...");
        pid = WaitForGame(exe, 10000);
        if (!pid) break;
        Log(L"CS2 relaunched itself.");
    }

    Log(L"Restoring 64 tick...");
    if (Restore64(root)) DeleteFileW(g_sessionFile);

done:
    PostMessageW(g_wnd, WM_APP_DONE, 0, 0);
    return 0;
}

/* Recovery on start: if a previous session was left patched. */
static DWORD WINAPI RecoveryWorker(LPVOID unused)
{
    (void)unused;
    int tick; WCHAR root[MAX_PATH], exe[MAX_PATH];
    if (!ReadSession(&tick, root)) { PostMessageW(g_wnd, WM_APP_DONE, 0, 0); return 0; }
    Log(L"Previous session (%d tick) was not restored. Restoring 64 tick in %s", tick, root);
    GameExePath(root, exe);
    DWORD pid = FindProcessByPath(L"cs2.exe", exe);
    if (pid) {
        Log(L"CS2 is still running from that session; will restore when it exits.");
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
        while (WaitForGame(exe, 10000)) Sleep(1000);
    }
    if (Restore64(root)) DeleteFileW(g_sessionFile);
    PostMessageW(g_wnd, WM_APP_DONE, 0, 0);
    return 0;
}

/* --------------------------------------------------------------- settings */
static int LoadTick(void) { int t = GetPrivateProfileIntW(L"launcher", L"tick", 128, g_iniFile); return t == 256 ? 256 : 128; }
static void SaveSettings(int tick)
{
    WCHAR b[16]; _snwprintf(b, 16, L"%d", tick);
    WritePrivateProfileStringW(L"launcher", L"tick", b, g_iniFile);
}

static void SetBusy(BOOL b)
{
    InterlockedExchange(&g_busy, b);
    EnableWindow(g_launch, !b); EnableWindow(g_restore, !b);
    EnableWindow(g_r128, !b); EnableWindow(g_r256, !b);
}

static void StartJob(int tick, int restoreOnly)
{
    if (g_busy) return;
    SaveSettings(tick);
    Job *j = (Job *)malloc(sizeof *j); j->tick = tick; j->restoreOnly = restoreOnly;
    SetBusy(TRUE);
    HANDLE t = CreateThread(NULL, 0, Worker, j, 0, NULL);
    if (t) CloseHandle(t); else { free(j); SetBusy(FALSE); }
}

/* --------------------------------------------------------------- Info tab: tick-rate check */
static WCHAR *g_ib; static size_t g_il, g_ic;
static void InfoAdd(const wchar_t *fmt, ...)
{
    WCHAR line[2048]; va_list ap; va_start(ap, fmt); _vsnwprintf(line, 2040, fmt, ap); line[2040] = 0; va_end(ap);
    wcscat(line, L"\r\n"); size_t L = wcslen(line);
    if (g_il + L + 1 > g_ic) { g_ic = (g_il + L + 1) * 2 + 4096; g_ib = (WCHAR *)realloc(g_ib, g_ic * sizeof(WCHAR)); }
    wcscpy(g_ib + g_il, line); g_il += L;
}
static volatile LONG g_infoBusy = 0;
static DWORD WINAPI InfoThread(LPVOID p)
{
    int chosen = (int)(INT_PTR)p;
    g_il = 0; if (g_ib) g_ib[0] = 0;
    SYSTEMTIME st; GetLocalTime(&st);
    InfoAdd(L"TICK RATE CHECK  (%02d:%02d:%02d)", st.wHour, st.wMinute, st.wSecond);
    InfoAdd(L"");
    InfoAdd(L"Chosen in this launcher: %d tick", chosen);
    WCHAR root[MAX_PATH], exe[MAX_PATH];
    if (!FindCs2(root)) { InfoAdd(L"CS2 not found in the Steam library folders."); goto out; }
    InfoAdd(L"CS2 folder: %s", root);
    InfoAdd(L"");
    InfoAdd(L"1. Game DLLs (what the patch engine finds on disk):");
    int rate = tp_status(root, InfoAdd);
    if (rate == 64) InfoAdd(L"  => 64 tick (original). The launcher patches right before CS2 starts and restores after it exits.");
    else if (rate > 0) InfoAdd(L"  => patched to %d tick%s", rate, rate != chosen ? L"  (differs from the chosen rate - Launch re-patches)" : L"");
    else InfoAdd(L"  => mixed or unknown build - Launch refuses unsupported files");
    int t; WCHAR sr[MAX_PATH];
    if (ReadSession(&t, sr)) InfoAdd(L"  Session file present: patched session (%d tick) is active or was not restored yet.", t);
    { WCHAR st[MAX_PATH]; _snwprintf(st, MAX_PATH, L"%s\\backup", g_dataDir);
      InfoAdd(L"  Backups of the original DLLs (only while patched): %s", st);
      InfoAdd(L"  Nothing is stored in the CS2 folder; restore puts the exact original files back (bytes, timestamps, attributes)."); }
    InfoAdd(L"");
    InfoAdd(L"2. Running game:");
    GameExePath(root, exe);
    DWORD pid = FindProcessByPath(L"cs2.exe", exe);
    if (!pid) InfoAdd(L"  CS2 is not running.");
    else {
        WCHAR cl[8192];
        InfoAdd(L"  CS2 running (pid %lu).", pid);
        if (GetCommandLineOf(pid, cl, 8192)) {
            WCHAR low[8192]; wcscpy(low, cl); _wcslwr(low);
            InfoAdd(L"  -insecure: %s", wcsstr(low, L"-insecure") ? L"YES" : L"NO - do not join any server; close CS2 and use Launch");
            InfoAdd(L"  Command line: %s", cl);
        } else InfoAdd(L"  (could not read its command line)");
        if (rate > 64) InfoAdd(L"  => this game session runs at %d tick.", rate);
        else if (rate == 64) InfoAdd(L"  => this game session runs at stock 64 tick (it was not started with this launcher).");
    }
    InfoAdd(L"");
    InfoAdd(L"3. Server:");
    InfoAdd(L"  The server must run the same rate. Servers from the CS2 Server Launcher put it in their name,");
    InfoAdd(L"  e.g. \"My Server [%d tick]\", and show it on their Info tab.", rate > 64 ? rate : chosen);
    InfoAdd(L"  Different rates on client and server = prediction errors / rubber-banding.");
    InfoAdd(L"");
    InfoAdd(L"4. Check in game (on the server):");
    InfoAdd(L"  Enable the console (Settings > Game > Enable Developer Console), join the server, press ~ and run:");
    InfoAdd(L"      cl_ticktiming print");
    InfoAdd(L"  Look at the line  FirstInput -> NetSend  - it is about one tick minus half a frame:");
    InfoAdd(L"      64 tick (stock) : a bit under 15.6 ms  (e.g. ~14-15 ms)");
    InfoAdd(L"      128 tick        : a bit under 7.8 ms   (e.g. ~6-7 ms)");
    InfoAdd(L"      256 tick        : a bit under 3.9 ms   (e.g. ~3 ms)");
    InfoAdd(L"  About 15 ms on a 128/256 server means your game is not patched to the same rate.");
out:;
    WCHAR *cp = _wcsdup(g_ib ? g_ib : L"");
    if (!cp || !PostMessageW(g_wnd, WM_APP_INFO, 0, (LPARAM)cp)) free(cp);
    InterlockedExchange(&g_infoBusy, 0);
    return 0;
}
static void CheckTickRate(void)
{
    if (InterlockedExchange(&g_infoBusy, 1)) return;
    SetWindowTextW(g_info, L"Checking...");
    int chosen = SendMessageW(g_r256, BM_GETCHECK, 0, 0) == BST_CHECKED ? 256 : 128;
    HANDLE t = CreateThread(NULL, 0, InfoThread, (LPVOID)(INT_PTR)chosen, 0, NULL); if (t) CloseHandle(t); else g_infoBusy = 0;
}
static void ShowTab(int p)
{
    HWND a[6] = { g_tickLbl, g_r128, g_r256, g_launch, g_restore, g_log }, b[3] = { g_info, g_infoBtn, g_infoLbl };
    for (int i = 0; i < 6; i++) ShowWindow(a[i], p == 0 ? SW_SHOW : SW_HIDE);
    for (int i = 0; i < 3; i++) ShowWindow(b[i], p == 1 ? SW_SHOW : SW_HIDE);
    if (p == 1) CheckTickRate();
}

/* --------------------------------------------------------------- UI */
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HINSTANCE hi = ((LPCREATESTRUCT)l)->hInstance;
#define MK(cls, txt, st, x, y, cw, ch, id) CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | (st), x, y, cw, ch, h, (HMENU)(INT_PTR)(id), hi, NULL)
        g_tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_BUTTONS | TCS_FLATBUTTONS, 8, 6, 400, 26, h, (HMENU)ID_TAB, hi, NULL);
        SendMessageW(g_tab, WM_SETFONT, (WPARAM)font, TRUE);
        { TCITEMW ti = { 0 }; ti.mask = TCIF_TEXT; ti.pszText = L"  Launch  "; SendMessageW(g_tab, TCM_INSERTITEMW, 0, (LPARAM)&ti);
          ti.pszText = L"  Info  "; SendMessageW(g_tab, TCM_INSERTITEMW, 1, (LPARAM)&ti); }
        g_tickLbl = MK(L"STATIC", L"Tick rate:", 0, 12, 46, 60, 18, 0);
        g_r128 = MK(L"BUTTON", L"128 tick", BS_AUTORADIOBUTTON | WS_GROUP, 80, 44, 80, 20, ID_R128);
        g_r256 = MK(L"BUTTON", L"256 tick", BS_AUTORADIOBUTTON, 165, 44, 80, 20, ID_R256);
        g_launch = MK(L"BUTTON", L"Launch CS2", BS_DEFPUSHBUTTON, 270, 40, 120, 28, ID_LAUNCH);
        g_restore = MK(L"BUTTON", L"Restore 64 tick now", 0, 400, 40, 150, 28, ID_RESTORE);
        g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 12, 78, 538, 330, h, (HMENU)ID_LOG, hi, NULL);
        g_infoBtn = MK(L"BUTTON", L"Check tick rate", 0, 12, 40, 120, 28, ID_INFOCHECK);
        g_infoLbl = MK(L"STATIC", L"Patched rate of your CS2 files and the running game's -insecure.", 0, 142, 46, 420, 18, 0);
        g_info = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 12, 78, 538, 330, h, (HMENU)ID_INFO, hi, NULL);
        HWND all[] = { g_tickLbl, g_r128, g_r256, g_launch, g_restore, g_log, g_infoBtn, g_infoLbl, g_info };
        for (int i = 0; i < 9; i++) SendMessageW(all[i], WM_SETFONT, (WPARAM)font, TRUE);
        HFONT mono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, L"Consolas");
        SendMessageW(g_log, WM_SETFONT, (WPARAM)mono, TRUE);
        SendMessageW(g_info, WM_SETFONT, (WPARAM)mono, TRUE);
        SendMessageW(g_log, EM_SETLIMITTEXT, 0, 0);
        SendMessageW(LoadTick() == 256 ? g_r256 : g_r128, BM_SETCHECK, BST_CHECKED, 0);
        ShowWindow(g_infoBtn, SW_HIDE); ShowWindow(g_infoLbl, SW_HIDE);
        SetWindowPos(g_tab, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        return 0;
    }
    case WM_SIZE: {
        int cw = LOWORD(l), ch = HIWORD(l);
        MoveWindow(g_tab, 8, 6, cw - 16, 26, TRUE);
        MoveWindow(g_log, 12, 78, cw - 24, ch - 90, TRUE);
        MoveWindow(g_info, 12, 78, cw - 24, ch - 90, TRUE);
        return 0;
    }
    case WM_GETMINMAXINFO: ((MINMAXINFO *)l)->ptMinTrackSize.x = 580; ((MINMAXINFO *)l)->ptMinTrackSize.y = 340; return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_LAUNCH:  StartJob(SendMessageW(g_r256, BM_GETCHECK, 0, 0) == BST_CHECKED ? 256 : 128, 0); break;
        case ID_RESTORE: StartJob(SendMessageW(g_r256, BM_GETCHECK, 0, 0) == BST_CHECKED ? 256 : 128, 1); break;
        case ID_INFOCHECK: CheckTickRate(); break;
        }
        return 0;
    case WM_NOTIFY:
        if (((NMHDR *)l)->hwndFrom == g_tab && ((NMHDR *)l)->code == TCN_SELCHANGE) ShowTab((int)SendMessageW(g_tab, TCM_GETCURSEL, 0, 0));
        return 0;
    case WM_APP_INFO: { WCHAR *s = (WCHAR *)l; SetWindowTextW(g_info, s); free(s); return 0; }
    case WM_APP_LOG: {
        WCHAR *s = (WCHAR *)l;
        int n = GetWindowTextLengthW(g_log);
        SendMessageW(g_log, EM_SETSEL, n, n);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)s);
        free(s);
        return 0;
    }
    case WM_APP_DONE: SetBusy(FALSE); return 0;
    case WM_CLOSE:
        if (g_busy && MessageBoxW(h,
                L"The launcher is still working (CS2 may be patched).\n\n"
                L"If you close now, 64 tick will be restored the next time you open the launcher.\n\nClose anyway?",
                APP_TITLE, MB_YESNO | MB_ICONWARNING) != IDYES) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd;
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\CS2_128tick_launcher");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"CS2TickLauncher", NULL);
        if (other) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }

    WCHAR la[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) wcscpy(la, L".");
    _snwprintf(g_dataDir, MAX_PATH, L"%s\\CS2-128-tick", la);
    CreateDirectoryW(g_dataDir, NULL);
    _snwprintf(g_sessionFile, MAX_PATH, L"%s\\session.txt", g_dataDir);
    _snwprintf(g_iniFile, MAX_PATH, L"%s\\launcher.ini", g_dataDir);
    _snwprintf(g_logFile, MAX_PATH, L"%s\\launcher.log", g_dataDir);
    { WCHAR st[MAX_PATH]; _snwprintf(st, MAX_PATH, L"%s\\backup", g_dataDir); tp_set_store(st); }

    { INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES | ICC_TAB_CLASSES }; InitCommonControlsEx(&icc); }
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"CS2TickLauncher";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, APP_TITLE L" " APP_VER, WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 600, 500, NULL, NULL, hi, NULL);
    ShowWindow(g_wnd, show); UpdateWindow(g_wnd);

    Log(L"%s %s - LAN / -insecure servers only. Server must run the same tick rate.", APP_TITLE, APP_VER);
    Log(L"Log file: %s", g_logFile);
    Log(L"Built-in patch engine for CS2 %hs.", TP_BUILD_1420_NAME);
    {
        WCHAR root[MAX_PATH];
        if (FindCs2(root)) {
            Log(L"CS2 folder: %s", root);
            int st = tp_status(root, Log);
            if (st == 64) Log(L"Current state: 64 tick (original).");
            else if (st) Log(L"Current state: patched %d tick.", st);
            else Log(L"Current state: unknown or mixed - Launch will refuse unsupported files.");
        } else Log(L"CS2 not found in the Steam library folders.");
    }

    int t; WCHAR r[MAX_PATH];
    if (ReadSession(&t, r)) {
        SetBusy(TRUE);
        HANDLE th = CreateThread(NULL, 0, RecoveryWorker, NULL, 0, NULL);
        if (th) CloseHandle(th); else SetBusy(FALSE);
    } else if (cmd) {
        /* optional: --launch 128|256 starts straight away (desktop shortcuts) */
        WCHAR *a = wcsstr(cmd, L"--launch");
        if (a) {
            int tk = _wtoi(a + 8) == 256 ? 256 : 128;
            SendMessageW(g_r128, BM_SETCHECK, tk == 128 ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(g_r256, BM_SETCHECK, tk == 256 ? BST_CHECKED : BST_UNCHECKED, 0);
            StartJob(tk, 0);
        }
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return 0;
}
