/*
 * tickpatch.h — CS2 tick-rate patch engine (buffer level, portable C).
 *
 * CS2 compiles 64 tick in: 1/64 and 64.0 (float and double) are constants
 * in engine2.dll, server.dll and client.dll. For each supported build this
 * engine has a table of the tick-related sites:
 *   type 0: RIP-relative SSE read of a constant -> disp32 is redirected to a
 *           new constant written into int3 padding inside .text
 *   type 1: mov dword [..], imm32 with 0x3C800000 / 0x42800000 -> imm32 rewritten
 *   type 2: raw 4-byte instruction edit (e.g. skip a warning that would print every tick)
 * The whole file must match the build's SHA-256 before anything is touched.
 *
 * LAN / -insecure use only.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>

typedef struct {
    uint32_t off;        /* file offset of the 4 bytes to edit */
    uint8_t  type;       /* 0 = redirect disp32, 1 = imm32, 2 = raw 4 bytes */
    uint8_t  kind;       /* 0 f64(64.0f) 1 finv(1/64f) 2 d64 3 dinv */
    uint8_t  orig[4];    /* original 4 bytes (checked) */
    int32_t  newdisp;    /* type 0: new disp32; type 2: the 4 new bytes */
} tp_site;

typedef struct {
    const wchar_t *rel;      /* path below the CS2 folder */
    uint32_t size;
    const char *sha256;      /* original file */
    uint32_t fpad_off;       /* 8 bytes: float rate, float 1/rate */
    uint32_t dpad_off;       /* 16 bytes: double rate, double 1/rate */
    const tp_site *sites;
    uint32_t nsites;
} tp_dll;

#include "sites_1420.h"

/* ------------------------------------------------------------ SHA-256 */
typedef struct { uint32_t s[8]; uint64_t n; uint8_t b[64]; uint32_t bl; } tp_sha;
static const uint32_t TP_K[64] = {
 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
 0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
 0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
 0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
 0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
 0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define TP_ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void tp_sha_blk(tp_sha *c, const uint8_t *p) {
    uint32_t w[64], a,b,d,e,f,g,h,cc,t1,t2; int i;
    for (i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i]<<24 | p[4*i+1]<<16 | p[4*i+2]<<8 | p[4*i+3];
    for (; i < 64; i++) {
        uint32_t s0 = TP_ROR(w[i-15],7)^TP_ROR(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1 = TP_ROR(w[i-2],17)^TP_ROR(w[i-2],19)^(w[i-2]>>10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=c->s[0];b=c->s[1];cc=c->s[2];d=c->s[3];e=c->s[4];f=c->s[5];g=c->s[6];h=c->s[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (TP_ROR(e,6)^TP_ROR(e,11)^TP_ROR(e,25)) + ((e&f)^(~e&g)) + TP_K[i] + w[i];
        t2 = (TP_ROR(a,2)^TP_ROR(a,13)^TP_ROR(a,22)) + ((a&b)^(a&cc)^(b&cc));
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->s[0]+=a;c->s[1]+=b;c->s[2]+=cc;c->s[3]+=d;c->s[4]+=e;c->s[5]+=f;c->s[6]+=g;c->s[7]+=h;
}
static void tp_sha_init(tp_sha *c) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->s, iv, sizeof iv); c->n = 0; c->bl = 0;
}
static void tp_sha_upd(tp_sha *c, const uint8_t *p, size_t n) {
    c->n += n;
    while (n) {
        if (c->bl == 0 && n >= 64) { tp_sha_blk(c, p); p += 64; n -= 64; continue; }
        uint32_t k = 64 - c->bl; if (k > n) k = (uint32_t)n;
        memcpy(c->b + c->bl, p, k); c->bl += k; p += k; n -= k;
        if (c->bl == 64) { tp_sha_blk(c, c->b); c->bl = 0; }
    }
}
static void tp_sha_hex(tp_sha *c, char out[65]) {
    uint64_t bits = c->n * 8; uint8_t pad = 0x80, z = 0, L[8]; int i;
    tp_sha_upd(c, &pad, 1);
    while (c->bl != 56) tp_sha_upd(c, &z, 1);
    for (i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - 8*i));
    tp_sha_upd(c, L, 8);
    for (i = 0; i < 8; i++) sprintf(out + 8*i, "%08x", c->s[i]);
    out[64] = 0;
}
static void tp_sha256(const uint8_t *p, size_t n, char out[65]) { tp_sha c; tp_sha_init(&c); tp_sha_upd(&c, p, n); tp_sha_hex(&c, out); }

/* ------------------------------------------------------------- engine */
static const tp_dll *const *tp_builds[] = { TP_BUILD_1420 };
static const char *tp_build_names[] = { TP_BUILD_1420_NAME };
#define TP_NBUILDS 1

/* Find the table entry for DLL index i (0 engine2, 1 server, 2 client) whose
 * original hash equals `sha`. Returns NULL if unknown. */
static const tp_dll *tp_find(int i, const char *sha, const char **build)
{
    for (int b = 0; b < TP_NBUILDS; b++)
        if (!strcmp(tp_builds[b][i]->sha256, sha)) { if (build) *build = tp_build_names[b]; return tp_builds[b][i]; }
    return NULL;
}

/* Apply `rate` (128 or 256, or any 65..1000) to an ORIGINAL buffer.
 * Returns number of edits, or -1 with err set. */
static int tp_patch_buffer(const tp_dll *t, uint8_t *buf, size_t len, int rate, char *err, size_t errsz)
{
    if (len != t->size) { snprintf(err, errsz, "size mismatch"); return -1; }
    /* every site must still hold its original bytes, padding must be int3 */
    for (uint32_t i = 0; i < t->nsites; i++) {
        const tp_site *s = &t->sites[i];
        if (s->off + 4 > len || memcmp(buf + s->off, s->orig, 4)) { snprintf(err, errsz, "site %u (0x%x) differs", i, s->off); return -1; }
    }
    for (int k = 0; k < 8; k++)  if (buf[t->fpad_off + k] != 0xCC) { snprintf(err, errsz, "float padding not free"); return -1; }
    for (int k = 0; k < 16; k++) if (buf[t->dpad_off + k] != 0xCC) { snprintf(err, errsz, "double padding not free"); return -1; }

    float  fr = (float)rate, fi = 1.0f / (float)rate;
    double dr = (double)rate, di = 1.0 / (double)rate;
    memcpy(buf + t->fpad_off, &fr, 4);
    memcpy(buf + t->fpad_off + 4, &fi, 4);
    memcpy(buf + t->dpad_off, &dr, 8);
    memcpy(buf + t->dpad_off + 8, &di, 8);
    int edits = 4;
    for (uint32_t i = 0; i < t->nsites; i++) {
        const tp_site *s = &t->sites[i];
        if (s->type == 0 || s->type == 2) memcpy(buf + s->off, &s->newdisp, 4);
        else                              memcpy(buf + s->off, s->kind == 1 ? (const void *)&fi : (const void *)&fr, 4);
        edits++;
    }
    return edits;
}
