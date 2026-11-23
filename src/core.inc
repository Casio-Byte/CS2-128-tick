/*
 * CS2 128-Tick Patcher (EXPERIMENTAL)  -  LAN / -insecure use only
 * Patches engine2.dll, server.dll, client.dll for one exact CS2 build.
 * Backs up originals as *.orig64 and can undo at any time.
 *
 * Usage: double-click for the menu, or:
 *   cs2_128tick.exe status|patch|aggressive|undo ["<CS2 folder>"]
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>
#include <commdlg.h>
#include <stdarg.h>
#include "patches.h"

#define BACKUP_EXT ".orig64"

enum { ST_MISSING, ST_ORIGINAL, ST_SAFE, ST_AGGR, ST_UNKNOWN };
static const char *ST_NAME[] = { "missing", "original - 64 tick", "patched - 128 tick (safe)", "patched - 128 tick (aggressive)", "unknown build - game updated?" };

static void color(WORD c) { SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), c); }
static void reset(void) { color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE); }

static unsigned char *read_file(const char *path, DWORD *size) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL), got = 0;
    unsigned char *buf = (unsigned char *)malloc(sz ? sz : 1);
    if (!buf || !ReadFile(h, buf, sz, &got, NULL) || got != sz) { free(buf); CloseHandle(h); return NULL; }
    CloseHandle(h); *size = sz; return buf;
}

static int write_file(const char *path, const unsigned char *buf, DWORD size) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0; BOOL ok = WriteFile(h, buf, size, &w, NULL);
    CloseHandle(h); return ok && w == size;
}

static void sha256_hex(const unsigned char *buf, DWORD size, char out[65]) {
    BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE hh; unsigned char d[32];
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0);
    BCryptHashData(hh, (PUCHAR)buf, size, 0);
    BCryptFinishHash(hh, d, 32, 0);
    BCryptDestroyHash(hh); BCryptCloseAlgorithmProvider(alg, 0);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
}

/* does every selected entry currently hold `want` bytes (0=old,1=new)? */
static int entries_are(const unsigned char *buf, DWORD size, const PatchFile *f, int tier, int want) {
    for (int i = 0; i < f->n; i++) {
        const PatchEntry *e = &f->p[i];
        if (e->tier != tier) continue;
        if (e->off + e->len > size) return 0;
        if (memcmp(buf + e->off, want ? e->newb : e->oldb, e->len) != 0) return 0;
    }
    return 1;
}

static int get_state(const unsigned char *buf, DWORD size, const PatchFile *f) {
    char h[65];
    if (!buf) return ST_MISSING;
    sha256_hex(buf, size, h);
    if (strcmp(h, f->sha) == 0) return ST_ORIGINAL;
    if (entries_are(buf, size, f, 0, 1) && entries_are(buf, size, f, 1, 1)) return ST_AGGR;
    if (entries_are(buf, size, f, 0, 1) && entries_are(buf, size, f, 1, 0)) return ST_SAFE;
    return ST_UNKNOWN;
}

/* set entries of tier<=maxtier to new (dir=1) or old (dir=0) */
static void set_entries(unsigned char *buf, const PatchFile *f, int maxtier, int dir) {
    for (int i = 0; i < f->n; i++) {
        const PatchEntry *e = &f->p[i];
        if (e->tier <= maxtier) memcpy(buf + e->off, dir ? e->newb : e->oldb, e->len);
    }
}

static int valid_root(const char *root) {
    char p[MAX_PATH * 2];
    snprintf(p, sizeof p, "%s\\game\\bin\\win64\\engine2.dll", root);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

static int find_cs2(char *out, size_t outsz) {
    char steam[MAX_PATH] = {0}; DWORD sz = sizeof steam;
    char libs[16][MAX_PATH]; int nl = 0;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, NULL, steam, &sz) == ERROR_SUCCESS) {
        for (char *c = steam; *c; c++) if (*c == '/') *c = '\\';
        strcpy(libs[nl++], steam);
    }
    strcpy(libs[nl++], "C:\\Program Files (x86)\\Steam");
    for (int s = 0, base = nl; s < base && nl < 16; s++) {
        char vdf[MAX_PATH * 2]; snprintf(vdf, sizeof vdf, "%s\\steamapps\\libraryfolders.vdf", libs[s]);
        FILE *fp = fopen(vdf, "r"); if (!fp) continue;
        char line[1024];
        while (fgets(line, sizeof line, fp) && nl < 16) {
            char *k = strstr(line, "\"path\""); if (!k) continue;
            char *a = strchr(k + 6, '"'); if (!a) continue; a++;
            char *b = strchr(a, '"'); if (!b) continue; *b = 0;
            char *w = libs[nl]; for (char *r = a; *r && w - libs[nl] < MAX_PATH - 1; r++) { if (r[0] == '\\' && r[1] == '\\') r++; *w++ = *r; }
            *w = 0; nl++;
        }
        fclose(fp);
    }
    for (int i = 0; i < nl; i++) {
        char p[MAX_PATH * 2]; snprintf(p, sizeof p, "%s\\steamapps\\common\\Counter-Strike Global Offensive", libs[i]);
        if (valid_root(p)) { snprintf(out, outsz, "%s", p); return 1; }
    }
    return 0;
}

