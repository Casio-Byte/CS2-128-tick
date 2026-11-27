/*
 * tp_files.h - file side of the tick patch (Windows).
 *
 * Nothing is kept in the game folder:
 *   - the original DLL, its SHA-256, timestamps and attributes are stored in a
 *     backup folder outside the game (tp_set_store, e.g. %LOCALAPPDATA%\CS2-128-tick\backup)
 *   - the patched DLL replaces the original (written to <dll>.tptmp, then MoveFileEx)
 *     and gets the original timestamps + attributes back
 *   - restore writes the original bytes back, puts the original timestamps and
 *     attributes back, checks the SHA-256 against the known build, then deletes
 *     the backup: the folder is exactly as before, as if nothing happened
 *   - all three DLLs are checked before anything is written
 * Older versions kept <dll>.orig64 / <dll>.tickpatch next to the DLL; those are
 * still understood, used for restore and then removed.
 */
#pragma once
#include <windows.h>
#include <shlobj.h>
#include <stdlib.h>
#include "tickpatch.h"

typedef void (*tp_log_fn)(const wchar_t *fmt, ...);

static const wchar_t *TP_REL[3] = {
    L"game\\bin\\win64\\engine2.dll",
    L"game\\csgo\\bin\\win64\\server.dll",
    L"game\\csgo\\bin\\win64\\client.dll",
};
static const wchar_t *TP_NAME[3] = { L"engine2.dll", L"server.dll", L"client.dll" };

static wchar_t tp_store_dir[MAX_PATH] = L"";
static void tp_set_store(const wchar_t *dir) { wcsncpy(tp_store_dir, dir, MAX_PATH - 1); tp_store_dir[MAX_PATH - 1] = 0; }

/* ------------------------------------------------------------ file helpers */
static BOOL tp_exists(const wchar_t *p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

static uint8_t *tp_read(const wchar_t *p, size_t *n, DWORD *gle)
{
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) { if (gle) *gle = GetLastError(); return NULL; }
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    uint8_t *b = (uint8_t *)malloc((size_t)sz.QuadPart + 1); DWORD got = 0; size_t tot = 0;
    while (b && tot < (size_t)sz.QuadPart) {
        DWORD want = (DWORD)min((size_t)sz.QuadPart - tot, (size_t)(64u << 20));
        if (!ReadFile(f, b + tot, want, &got, NULL) || !got) { free(b); b = NULL; break; }
        tot += got;
    }
    if (gle) *gle = GetLastError();
    CloseHandle(f);
    *n = tot;
    return b;
}

static BOOL tp_write_file(const wchar_t *p, const uint8_t *b, size_t n, DWORD *gle)
{
    HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { if (gle) *gle = GetLastError(); return FALSE; }
    size_t tot = 0; DWORD w;
    while (tot < n) {
        DWORD c = (DWORD)min(n - tot, (size_t)(64u << 20));
        if (!WriteFile(f, b + tot, c, &w, NULL) || w != c) { if (gle) *gle = GetLastError(); CloseHandle(f); DeleteFileW(p); return FALSE; }
        tot += w;
    }
    FlushFileBuffers(f); CloseHandle(f);
    return TRUE;
}

/* write next to dst, then atomically replace dst */
static BOOL tp_write_replace(const wchar_t *dst, const uint8_t *b, size_t n, DWORD *gle)
{
    wchar_t tmp[MAX_PATH]; _snwprintf(tmp, MAX_PATH, L"%s.tptmp", dst); tmp[MAX_PATH-1] = 0;
    if (!tp_write_file(tmp, b, n, gle)) return FALSE;
    SetFileAttributesW(dst, FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { *gle = GetLastError(); DeleteFileW(tmp); return FALSE; }
    return TRUE;
}

typedef struct { FILETIME c, a, w; DWORD attr; BOOL ok; } tp_stamp;
static tp_stamp tp_get_stamp(const wchar_t *p)
{
    tp_stamp s = { 0 };
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExW(p, GetFileExInfoStandard, &d)) { s.c = d.ftCreationTime; s.a = d.ftLastAccessTime; s.w = d.ftLastWriteTime; s.attr = d.dwFileAttributes; s.ok = TRUE; }
    return s;
}
static void tp_put_stamp(const wchar_t *p, const tp_stamp *s)
{
    if (!s->ok) return;
    SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
    HANDLE f = CreateFileW(p, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (f != INVALID_HANDLE_VALUE) { SetFileTime(f, &s->c, &s->a, &s->w); CloseHandle(f); }
    SetFileAttributesW(p, s->attr ? s->attr : FILE_ATTRIBUTE_NORMAL);
}

static BOOL tp_is_cs2(const wchar_t *root)
{
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%s\\%s", root, TP_REL[0]); p[MAX_PATH-1] = 0;
    return tp_exists(p);
}

/* ------------------------------------------------------------ backup store */
/* one folder per CS2 install: <store>\<hash of the install path> */
static void tp_store_paths(const wchar_t *root, int i, wchar_t *bak, wchar_t *meta, wchar_t *dir)
{
    wchar_t low[MAX_PATH]; wcsncpy(low, root, MAX_PATH - 1); low[MAX_PATH - 1] = 0; _wcslwr(low);
    size_t L = wcslen(low); while (L && (low[L - 1] == L'\\' || low[L - 1] == L'/')) low[--L] = 0;
    uint64_t h = 1469598103934665603ULL; for (size_t k = 0; k < L; k++) { h ^= (uint16_t)low[k]; h *= 1099511628211ULL; }
    wchar_t d[MAX_PATH];
    if (tp_store_dir[0]) _snwprintf(d, MAX_PATH, L"%s\\%016llx", tp_store_dir, (unsigned long long)h);
    else { wchar_t la[MAX_PATH]; if (!GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) wcscpy(la, L"."); _snwprintf(d, MAX_PATH, L"%s\\CS2-128-tick\\backup\\%016llx", la, (unsigned long long)h); }
    d[MAX_PATH-1] = 0;
    if (dir) wcscpy(dir, d);
    if (bak) { _snwprintf(bak, MAX_PATH, L"%s\\%s.orig", d, TP_NAME[i]); bak[MAX_PATH-1] = 0; }
    if (meta) { _snwprintf(meta, MAX_PATH, L"%s\\%s.meta", d, TP_NAME[i]); meta[MAX_PATH-1] = 0; }
}

typedef struct { int rate; char patched[65], orig[65]; tp_stamp st; } tp_meta;

static BOOL tp_meta_read(const wchar_t *p, tp_meta *m)
{
    ZeroMemory(m, sizeof *m);
    size_t n; uint8_t *b = tp_read(p, &n, NULL); if (!b) return FALSE;
    b[n] = 0; char *s = (char *)b; unsigned long long v;
    char *q;
    if ((q = strstr(s, "rate="))) m->rate = atoi(q + 5);
    if ((q = strstr(s, "patched=")) && strlen(q + 8) >= 64) { memcpy(m->patched, q + 8, 64); m->patched[64] = 0; }
    if ((q = strstr(s, "orig=")) && strlen(q + 5) >= 64) { memcpy(m->orig, q + 5, 64); m->orig[64] = 0; }
    if ((q = strstr(s, "ctime=")) && sscanf(q + 6, "%llu", &v) == 1) { m->st.c.dwLowDateTime = (DWORD)v; m->st.c.dwHighDateTime = (DWORD)(v >> 32); m->st.ok = TRUE; }
    if ((q = strstr(s, "atime=")) && sscanf(q + 6, "%llu", &v) == 1) { m->st.a.dwLowDateTime = (DWORD)v; m->st.a.dwHighDateTime = (DWORD)(v >> 32); }
    if ((q = strstr(s, "mtime=")) && sscanf(q + 6, "%llu", &v) == 1) { m->st.w.dwLowDateTime = (DWORD)v; m->st.w.dwHighDateTime = (DWORD)(v >> 32); }
    if ((q = strstr(s, "attr=")) && sscanf(q + 5, "%llu", &v) == 1) m->st.attr = (DWORD)v;
    free(b);
    return m->rate > 0 && m->patched[0] && m->orig[0];
}
static BOOL tp_meta_write(const wchar_t *p, const tp_meta *m)
{
    char s[512];
#define FT(x) ((unsigned long long)(x).dwHighDateTime << 32 | (x).dwLowDateTime)
    int n = snprintf(s, sizeof s, "rate=%d\npatched=%s\norig=%s\nctime=%llu\natime=%llu\nmtime=%llu\nattr=%lu\n",
                     m->rate, m->patched, m->orig, FT(m->st.c), FT(m->st.a), FT(m->st.w), (unsigned long)m->st.attr);
#undef FT
    return tp_write_file(p, (const uint8_t *)s, (size_t)n, NULL);
}

static void tp_store_clean(const wchar_t *root, int i)
{
    wchar_t bak[MAX_PATH], meta[MAX_PATH], dir[MAX_PATH];
    tp_store_paths(root, i, bak, meta, dir);
    DeleteFileW(bak); DeleteFileW(meta);
    RemoveDirectoryW(dir);                       /* only succeeds when empty */
}

/* legacy (older versions): <dll>.orig64 + <dll>.tickpatch ("rate=..\nsha=..") next to the DLL */
static BOOL tp_legacy_marker(const wchar_t *dll, int *rate, char sha[65])
{
    wchar_t m[MAX_PATH]; _snwprintf(m, MAX_PATH, L"%s.tickpatch", dll); m[MAX_PATH-1] = 0;
    size_t n; uint8_t *b = tp_read(m, &n, NULL); if (!b) return FALSE;
    b[n] = 0; *rate = 0; sha[0] = 0;
    char *r = strstr((char *)b, "rate="), *s = strstr((char *)b, "sha=");
    if (r) *rate = atoi(r + 5);
    if (s && strlen(s + 4) >= 64) { memcpy(sha, s + 4, 64); sha[64] = 0; }
    free(b);
    return *rate > 0 && sha[0];
}
static void tp_legacy_clean(const wchar_t *dll)
{
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%s.orig64", dll); p[MAX_PATH-1] = 0; SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL); DeleteFileW(p);
    _snwprintf(p, MAX_PATH, L"%s.tickpatch", dll); p[MAX_PATH-1] = 0; DeleteFileW(p);
    _snwprintf(p, MAX_PATH, L"%s.tptmp", dll); p[MAX_PATH-1] = 0; DeleteFileW(p);
}

/* What is this DLL right now?  0 = original (known build), >64 = patched by us at that rate, -1 = unknown */
static int tp_state_of(const wchar_t *root, int i, const char *sha, tp_meta *m, BOOL *legacy)
{
    wchar_t dll[MAX_PATH], meta[MAX_PATH];
    _snwprintf(dll, MAX_PATH, L"%s\\%s", root, TP_REL[i]); dll[MAX_PATH-1] = 0;
    tp_store_paths(root, i, NULL, meta, NULL);
    if (legacy) *legacy = FALSE;
    if (tp_find(i, sha, NULL)) return 0;
    if (tp_meta_read(meta, m) && !strcmp(m->patched, sha)) return m->rate;
    int r; char ps[65];
    if (tp_legacy_marker(dll, &r, ps) && !strcmp(ps, sha)) { if (legacy) *legacy = TRUE; ZeroMemory(m, sizeof *m); m->rate = r; strcpy(m->patched, ps); return r; }
    return -1;
}

/* the original bytes for DLL i: from the store, or the legacy .orig64. Checked against the known build. */
static uint8_t *tp_load_original(const wchar_t *root, int i, BOOL legacy, size_t *n, const tp_dll **tab, tp_log_fn log)
{
    wchar_t dll[MAX_PATH], src[MAX_PATH];
    _snwprintf(dll, MAX_PATH, L"%s\\%s", root, TP_REL[i]); dll[MAX_PATH-1] = 0;
    if (legacy) _snwprintf(src, MAX_PATH, L"%s.orig64", dll); else tp_store_paths(root, i, src, NULL, NULL);
    src[MAX_PATH-1] = 0;
    DWORD gle = 0; uint8_t *b = tp_read(src, n, &gle);
    if (!b) { log(L"ERROR: backup of %s is missing (%s, error %lu). Use Steam > Verify integrity of game files.", TP_NAME[i], src, gle); return NULL; }
    char sha[65]; tp_sha256(b, *n, sha);
    *tab = tp_find(i, sha, NULL);
    if (!*tab) { free(b); log(L"ERROR: backup of %s is not a known original (sha %.16hs...).", TP_NAME[i], sha); return NULL; }
    return b;
}

/* ------------------------------------------------------------ patch */
static BOOL tp_apply(const wchar_t *root, int rate, tp_log_fn log)
{
    if (!tp_is_cs2(root)) { log(L"ERROR: not a CS2 folder (no game\\bin\\win64\\engine2.dll): %s", root); return FALSE; }
    if (rate < 65 || rate > 1000) { log(L"ERROR: bad tick rate %d", rate); return FALSE; }
    uint8_t *src[3] = {0}; size_t len[3] = {0}; const tp_dll *tab[3] = {0};
    BOOL fresh[3] = {0}, legacy[3] = {0}; tp_meta meta[3]; tp_stamp st[3];
    wchar_t dll[3][MAX_PATH];
    BOOL ok = TRUE;
    /* 1. check all three, collect the original bytes */
    for (int i = 0; i < 3 && ok; i++) {
        _snwprintf(dll[i], MAX_PATH, L"%s\\%s", root, TP_REL[i]); dll[i][MAX_PATH-1] = 0;
        DWORD gle = 0; char sha[65]; const char *build = NULL;
        src[i] = tp_read(dll[i], &len[i], &gle);
        if (!src[i]) { log(L"ERROR: cannot read %s (error %lu)", dll[i], gle); ok = FALSE; break; }
        tp_sha256(src[i], len[i], sha);
        int s = tp_state_of(root, i, sha, &meta[i], &legacy[i]);
        if (s == 0) {
            tab[i] = tp_find(i, sha, &build); fresh[i] = TRUE; st[i] = tp_get_stamp(dll[i]);
            ZeroMemory(&meta[i], sizeof meta[i]); strcpy(meta[i].orig, sha); meta[i].st = st[i];
            log(L"  %s: original 64 tick, CS2 %hs", TP_NAME[i], build);
            continue;
        }
        if (s > 0) {
            free(src[i]);
            src[i] = tp_load_original(root, i, legacy[i], &len[i], &tab[i], log);
            if (!src[i]) { ok = FALSE; break; }
            if (legacy[i]) { /* migrate: keep the original's identity from the legacy backup */
                wchar_t o64[MAX_PATH]; _snwprintf(o64, MAX_PATH, L"%s.orig64", dll[i]); o64[MAX_PATH-1] = 0;
                tp_sha256(src[i], len[i], meta[i].orig); meta[i].st = tp_get_stamp(o64); fresh[i] = TRUE;
            }
            st[i] = meta[i].st;
            log(L"  %s: currently %d tick, re-patching from the backup", TP_NAME[i], s);
            continue;
        }
        log(L"ERROR: %s is not a supported CS2 build (sha256 %hs).", TP_NAME[i], sha);
        log(L"       Supported: CS2 %hs. If CS2 just updated, this launcher needs a new table. Nothing was changed.", TP_BUILD_1420_NAME);
        ok = FALSE;
    }
    /* 2. patch in memory */
    for (int i = 0; i < 3 && ok; i++) {
        char err[128];
        if (tp_patch_buffer(tab[i], src[i], len[i], rate, err, sizeof err) < 0) { log(L"ERROR: %s: %hs", TP_NAME[i], err); ok = FALSE; }
    }
    /* 3. back up (outside the game folder) and replace */
    for (int i = 0; i < 3 && ok; i++) {
        wchar_t bak[MAX_PATH], mp[MAX_PATH], dir[MAX_PATH]; DWORD gle = 0;
        tp_store_paths(root, i, bak, mp, dir);
        if (fresh[i]) {
            SHCreateDirectoryExW(NULL, dir, NULL);
            const wchar_t *from = dll[i]; wchar_t o64[MAX_PATH];
            if (legacy[i]) { _snwprintf(o64, MAX_PATH, L"%s.orig64", dll[i]); o64[MAX_PATH-1] = 0; from = o64; }
            if (!CopyFileW(from, bak, FALSE)) { log(L"ERROR: cannot write backup %s (error %lu)", bak, GetLastError()); ok = FALSE; break; }
            size_t bn; uint8_t *bb = tp_read(bak, &bn, &gle); char bs[65] = "";
            if (bb) { tp_sha256(bb, bn, bs); free(bb); }
            if (strcmp(bs, tab[i]->sha256)) { log(L"ERROR: backup of %s did not verify - nothing replaced", TP_NAME[i]); ok = FALSE; break; }
        }
        if (!tp_write_replace(dll[i], src[i], len[i], &gle)) {
            log(L"ERROR: cannot write %s (error %lu)%s", dll[i], gle, gle == ERROR_SHARING_VIOLATION || gle == ERROR_ACCESS_DENIED ? L" - is CS2 running?" : L"");
            ok = FALSE; break;
        }
        tp_put_stamp(dll[i], &st[i]);                       /* keep the original times + attributes */
        meta[i].rate = rate; tp_sha256(src[i], len[i], meta[i].patched);
        if (!meta[i].orig[0]) strcpy(meta[i].orig, tab[i]->sha256);
        meta[i].st = st[i];
        tp_meta_write(mp, &meta[i]);
        tp_legacy_clean(dll[i]);                            /* no old-style files left next to the DLL */
        log(L"  %s: patched to %d tick (%u sites + 4 constants)", TP_REL[i], rate, tab[i]->nsites);
    }
    if (!ok) log(L"Patch failed part-way; run Restore 64 tick.");
    for (int i = 0; i < 3; i++) free(src[i]);
    return ok;
}

/* ------------------------------------------------------------ restore */
/* Puts every DLL back exactly as it was (bytes, times, attributes) and removes all traces.
 * TRUE if everything is original afterwards. */
static BOOL tp_restore(const wchar_t *root, tp_log_fn log)
{
    if (!tp_is_cs2(root)) { log(L"ERROR: not a CS2 folder: %s", root); return FALSE; }
    BOOL all = TRUE;
    for (int i = 0; i < 3; i++) {
        wchar_t dll[MAX_PATH]; _snwprintf(dll, MAX_PATH, L"%s\\%s", root, TP_REL[i]); dll[MAX_PATH-1] = 0;
        DWORD gle = 0; size_t n; char sha[65];
        uint8_t *cur = tp_read(dll, &n, &gle);
        if (!cur) { log(L"ERROR: cannot read %s (error %lu)", dll, gle); all = FALSE; continue; }
        tp_sha256(cur, n, sha); free(cur);
        tp_meta m; BOOL legacy;
        int s = tp_state_of(root, i, sha, &m, &legacy);
        if (s == 0) {                                           /* already original: just clean up */
            wchar_t meta[MAX_PATH]; tp_store_paths(root, i, NULL, meta, NULL);
            tp_meta om; if (tp_meta_read(meta, &om) && !strcmp(om.orig, sha)) tp_put_stamp(dll, &om.st);
            tp_store_clean(root, i); tp_legacy_clean(dll);
            log(L"  %s: 64 tick (original)", TP_REL[i]);
            continue;
        }
        if (s < 0) {
            /* not ours: e.g. Steam updated CS2 while patched - the old backup is stale */
            wchar_t bak[MAX_PATH]; tp_store_paths(root, i, bak, NULL, NULL);
            wchar_t o64[MAX_PATH]; _snwprintf(o64, MAX_PATH, L"%s.orig64", dll); o64[MAX_PATH-1] = 0;
            if (tp_exists(bak) || tp_exists(o64)) {
                tp_store_clean(root, i); tp_legacy_clean(dll);
                log(L"  %s: changed by something else (CS2 update?) - left alone, old backup discarded", TP_REL[i]);
            } else log(L"  %s: unknown build, not patched by this launcher (left alone)", TP_REL[i]);
            continue;
        }
        const tp_dll *tab = NULL; size_t on;
        uint8_t *ob = tp_load_original(root, i, legacy, &on, &tab, log);
        if (!ob) { all = FALSE; continue; }
        tp_stamp stamp = m.st;
        if (legacy) { wchar_t o64[MAX_PATH]; _snwprintf(o64, MAX_PATH, L"%s.orig64", dll); o64[MAX_PATH-1] = 0; stamp = tp_get_stamp(o64); }
        if (!tp_write_replace(dll, ob, on, &gle)) { free(ob); log(L"  %s: cannot write yet (error %lu)", TP_REL[i], gle); all = FALSE; continue; }
        free(ob);
        tp_put_stamp(dll, &stamp);
        uint8_t *chk = tp_read(dll, &n, &gle); char cs[65] = "";
        if (chk) { tp_sha256(chk, n, cs); free(chk); }
        if (!tp_find(i, cs, NULL)) { log(L"ERROR: %s did not verify after restore - backup kept", TP_REL[i]); all = FALSE; continue; }
        tp_store_clean(root, i); tp_legacy_clean(dll);
        log(L"  %s: restored 64 tick (SHA-256, timestamps and attributes as before; backup removed)", TP_REL[i]);
    }
    return all;
}

/* Returns the common patched rate, 64 if all original, 0 if mixed/unknown. */
static int tp_status(const wchar_t *root, tp_log_fn log)
{
    int common = -1;
    for (int i = 0; i < 3; i++) {
        wchar_t dll[MAX_PATH]; _snwprintf(dll, MAX_PATH, L"%s\\%s", root, TP_REL[i]); dll[MAX_PATH-1] = 0;
        size_t n; char sha[65]; int r = 0; const char *build = NULL;
        uint8_t *b = tp_read(dll, &n, NULL);
        if (!b) { if (log) log(L"  %s: missing", TP_REL[i]); common = 0; continue; }
        tp_sha256(b, n, sha); free(b);
        tp_meta m; BOOL legacy;
        int s = tp_state_of(root, i, sha, &m, &legacy);
        if (s == 0) { tp_find(i, sha, &build); r = 64; if (log) log(L"  %s: 64 tick (CS2 %hs)", TP_REL[i], build); }
        else if (s > 0) { r = s; if (log) log(L"  %s: patched %d tick%s", TP_REL[i], s, legacy ? L" (old-style backup next to the DLL)" : L""); }
        else if (log) log(L"  %s: unknown build (sha %.16hs...)", TP_REL[i], sha);
        if (common == -1) common = r; else if (common != r) common = 0;
    }
    return common < 0 ? 0 : common;
}
