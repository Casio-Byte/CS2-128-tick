/*
 * Auto-port: carry the patch over to a CS2 build this app has never seen.
 *
 * For every patched instruction of the reference build, signatures.h holds the bytes around it plus a mask of the
 * bytes that move between builds (RIP-relative displacements, call/jmp targets, the site's own operand).
 * In a new DLL we only look at places that read a 1/64 or 64.0 constant (or hold a 1/64 immediate), compare the
 * masked window there, and accept a site only if exactly one place matches. Then strict checks decide whether the
 * result is trustworthy; if not, nothing is patched.
 *
 * Pure C (no Windows calls) so it can be tested on any platform. Needs patches.h (PatchEntry, K_*) and signatures.h.
 */
#include <stdint.h>
#include <time.h>

typedef struct { uint32_t va, vsize, raw, rawsize, ch; char name[9]; } PortSec;
typedef struct { const unsigned char *buf; uint32_t size; PortSec sec[96]; int nsec; int text; } PortPE;

static int port_pe(PortPE *pe, const unsigned char *buf, uint32_t size) {
    memset(pe, 0, sizeof *pe); pe->buf = buf; pe->size = size; pe->text = -1;
    if (size < 0x400 || buf[0] != 'M' || buf[1] != 'Z') return 0;
    uint32_t nt; memcpy(&nt, buf + 0x3C, 4);
    if (nt + 0x108 > size || memcmp(buf + nt, "PE\0\0", 4)) return 0;
    uint16_t nsec, optsz; memcpy(&nsec, buf + nt + 6, 2); memcpy(&optsz, buf + nt + 20, 2);
    uint32_t sh = nt + 24 + optsz;
    if (nsec > 96 || sh + nsec * 40u > size) return 0;
    for (int i = 0; i < nsec; i++) {
        const unsigned char *s = buf + sh + i * 40; PortSec *o = &pe->sec[i];
        memcpy(o->name, s, 8); o->name[8] = 0;
        memcpy(&o->vsize, s + 8, 4); memcpy(&o->va, s + 12, 4); memcpy(&o->rawsize, s + 16, 4);
        memcpy(&o->raw, s + 20, 4); memcpy(&o->ch, s + 36, 4);
        if (o->raw + o->rawsize > size) return 0;
        if (!strcmp(o->name, ".text")) pe->text = i;
    }
    pe->nsec = nsec;
    return pe->text >= 0;
}

static int64_t port_rva2off(const PortPE *pe, uint32_t rva) {
    for (int i = 0; i < pe->nsec; i++) {
        const PortSec *s = &pe->sec[i];
        if (rva >= s->va && rva < s->va + s->rawsize) return (int64_t)s->raw + (rva - s->va);
    }
    return -1;
}

/* constant locations: readable, non-writable, non-text sections, naturally aligned */
typedef struct { uint32_t rva; unsigned char kind; } PortConst;
static int port_cmp_const(const void *a, const void *b) {
    uint32_t x = ((const PortConst *)a)->rva, y = ((const PortConst *)b)->rva; return x < y ? -1 : x > y;
}
static int port_find_const(const PortConst *c, int n, uint32_t rva) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (c[m].rva == rva) return c[m].kind; if (c[m].rva < rva) lo = m + 1; else hi = m - 1; }
    return 0;
}

typedef struct {
    int sites, matched, ambiguous /* extra identical copies */, critical_missing, duplicates, uncovered, slots_ok, accepted;
    char reason[160];
} PortReport;

/* thresholds: deliberately strict */
#define PORT_MIN_MATCH_PERMILLE 985   /* >= 98.5% of a DLL's sites must be found uniquely */
#define PORT_MAX_NEW_UNCOVERED  8     /* new 1/64 uses (not in the reference build) tolerated */
#define PORT_MAX_EXTRA_COPIES   8     /* extra identical copies of a patched helper tolerated */
#define PORT_MAX_MISMATCH_PCT   15    /* outer window may differ in up to 15% of its fixed bytes */
#define PORT_MIN_MARGIN         6     /* an imperfect match must beat the runner-up by this many bytes */
#define PORT_MAX_MISMATCH_PCT2  25    /* second pass (instruction-only core) */
#define PORT_MIN_MARGIN2        12

/*
 * Port the patch for logical file `fi` (0 engine2, 1 server, 2 client) onto `buf` (an ORIGINAL, unpatched DLL).
 * On success fills *out / *nout (malloc'd, caller frees) and returns 1. Always fills *rep.
 */
static int port_file(int fi, const unsigned char *buf, uint32_t size, PatchEntry **out, int *nout, PortReport *rep) {
    memset(rep, 0, sizeof *rep); *out = NULL; *nout = 0;
    PortPE pe;
    if (!port_pe(&pe, buf, size)) { snprintf(rep->reason, sizeof rep->reason, "not a valid 64-bit DLL"); return 0; }
    const PortSec *tx = &pe.sec[pe.text];
    const unsigned char *code = buf + tx->raw; uint32_t clen = tx->rawsize < tx->vsize ? tx->rawsize : tx->vsize;

    /* 1. constants 1/64, 64 (float, double) */
    int cap = 4096, nc = 0; PortConst *cs = malloc(cap * sizeof *cs);
    const float f_inv = 1.0f / 64, f_rate = 64.0f; const double d_inv = 1.0 / 64, d_rate = 64.0;
    for (int i = 0; i < pe.nsec; i++) {
        const PortSec *s = &pe.sec[i];
        if (i == pe.text || !(s->ch & 0x40000000u) || (s->ch & 0x80000000u) || (s->ch & 0x20000000u)) continue;
        const unsigned char *d = buf + s->raw;
        for (uint32_t o = 0; o + 8 <= s->rawsize; o += 4) {
            int k = 0;
            if (!memcmp(d + o, &f_inv, 4)) k = S_F_INV64; else if (!memcmp(d + o, &f_rate, 4)) k = S_F_RATE64;
            else if (!(o & 7) && !memcmp(d + o, &d_inv, 8)) k = S_D_INV64; else if (!(o & 7) && !memcmp(d + o, &d_rate, 8)) k = S_D_RATE64;
            if (!k) continue;
            if (nc == cap) { cap *= 2; cs = realloc(cs, cap * sizeof *cs); }
            cs[nc].rva = s->va + o; cs[nc].kind = (unsigned char)k; nc++;
        }
    }
    qsort(cs, nc, sizeof *cs, port_cmp_const);

    /* 2. candidate operand positions: disp32 whose target (+0/+1/+4 trailing bytes) is one of those constants,
          and 1/64 immediates. Stored as RVA of the disp/imm plus extra and kind. */
    typedef struct { uint32_t rva; unsigned char kind, extra; } Cand;
    int ccap = 8192, ncand = 0; Cand *cand = malloc(ccap * sizeof *cand);
    for (uint32_t p = 0; p + 4 <= clen; p++) {
        int32_t d; memcpy(&d, code + p, 4);
        uint32_t base = tx->va + p + 4 + (uint32_t)d;
        static const unsigned char EX[3] = { 0, 1, 4 };
        for (int e = 0; e < 3; e++) {
            int k = port_find_const(cs, nc, base + EX[e]);
            if (!k) continue;
            if (ncand == ccap) { ccap *= 2; cand = realloc(cand, ccap * sizeof *cand); }
            cand[ncand].rva = tx->va + p; cand[ncand].kind = (unsigned char)k; cand[ncand].extra = EX[e]; ncand++;
        }
        if (!memcmp(code + p, &f_inv, 4)) {
            if (ncand == ccap) { ccap *= 2; cand = realloc(cand, ccap * sizeof *cand); }
            cand[ncand].rva = tx->va + p; cand[ncand].kind = S_IMM_F_INV64; cand[ncand].extra = 0; ncand++;
        }
    }
    free(cs);

    /* 3. constant slots in int3 padding (same rule as tools/build_patches.py) */
    uint32_t slot[4]; int ns = 0;
    {
        uint32_t run = 0;
        for (uint32_t p = 0; p < clen && ns == 0; p++) {           /* one big run >= 64 */
            run = code[p] == 0xCC ? run + 1 : 0;
            if (run == 64) { uint32_t st = p + 1 - 64; uint32_t a = (st + 8 + 15) & ~15u; for (int i = 0; i < 4; i++) slot[i] = tx->va + a + 8 * i; ns = 4; }
        }
        if (!ns) {                                                  /* else four small runs >= 12 */
            uint32_t p = 0;
            while (p < clen && ns < 4) {
                if (code[p] != 0xCC) { p++; continue; }
                uint32_t st = p; while (p < clen && code[p] == 0xCC) p++;
                if (p - st >= 12) { uint32_t a = (st + 2 + 7) & ~7u; if (a + 8 <= p - 2) slot[ns++] = tx->va + a; }
            }
        }
    }
    rep->slots_ok = ns == 4;

    /* 4. match every reference site */
    int nsites = 0; for (int i = 0; i < NSIGS; i++) if (SIGS[i].file == fi) nsites += SIGS[i].count;
    rep->sites = nsites;
    int ocap = nsites + 64, ucap = nsites + 64;
    PatchEntry *pe_out = calloc(ocap, sizeof *pe_out); int n = 0;
    uint32_t *used = calloc(ucap, sizeof *used); int nused = 0;
    static const unsigned char SLOTKIND[4] = { K_F_INV, K_F_RATE, K_D_INV, K_D_RATE };
    if (rep->slots_ok)
        for (int i = 0; i < 4; i++) {
            int64_t fo = port_rva2off(&pe, slot[i]); int len = i < 2 ? 4 : 8;
            PatchEntry *e = &pe_out[n++]; e->tier = 0; e->kind = SLOTKIND[i]; e->off = (unsigned)fo; e->len = (unsigned char)len;
            memcpy(e->oldb, buf + fo, len); memset(e->newb, 0, 8);
        }
    int uncovered_inv = 0;
    int extra_copies = 0;
    typedef struct { uint32_t ins; int mis; } Hit;
    Hit *hits = malloc(4096 * sizeof *hits);
    for (int s = 0; s < NSIGS; s++) {
        const SigSite *g = &SIGS[s]; if (g->file != fi) continue;
        const unsigned char *wb = SIG_BYTES + g->at, *wm = SIG_MASK + g->at;
        /* core = the instruction +-8 bytes: must match exactly. The rest of the window may differ a little
           (register allocation, struct offsets, nearby jumps change between builds). */
        int unmasked = 0; for (int k = 0; k < g->len; k++) unmasked += wm[k];
        int nh = 0, perfect = 0, take = 0;
      for (int pass = 0; pass < 2; pass++) {
        /* pass 0: instruction +-8 bytes exact, rest within 15%.
           pass 1 (only for sites pass 0 couldn't place): instruction exact, rest within 25%, bigger winning margin */
        int ctx = pass ? 0 : 8;
        int core_lo = g->pre > ctx ? g->pre - ctx : 0, core_hi = g->pre + g->size + ctx; if (core_hi > g->len) core_hi = g->len;
        int maxmis = unmasked * (pass ? PORT_MAX_MISMATCH_PCT2 : PORT_MAX_MISMATCH_PCT) / 100;
        int margin = pass ? PORT_MIN_MARGIN2 : PORT_MIN_MARGIN;
        nh = 0;
        for (int c = 0; c < ncand && nh < 4096; c++) {
            if (cand[c].kind != g->src || (g->src != S_IMM_F_INV64 && cand[c].extra != g->extra)) continue;
            uint32_t ins = cand[c].rva - g->off;
            if (ins < tx->va + g->pre) continue;
            uint32_t lo = ins - g->pre;
            if (lo + g->len > tx->va + clen) continue;
            const unsigned char *w = code + (lo - tx->va);
            int ok = 1, mis = 0;
            for (int k = core_lo; k < core_hi; k++) if (wm[k] && w[k] != wb[k]) { ok = 0; break; }
            if (!ok) continue;
            for (int k = 0; k < g->len; k++) if (wm[k] && w[k] != wb[k]) mis++;
            if (mis > maxmis) continue;
            hits[nh].ins = ins; hits[nh].mis = mis; nh++;
        }
        /* sort by mismatches (insertion sort; lists are short) */
        for (int i = 1; i < nh; i++) { Hit t = hits[i]; int j = i - 1; while (j >= 0 && hits[j].mis > t.mis) { hits[j + 1] = hits[j]; j--; } hits[j + 1] = t; }
        perfect = 0; while (perfect < nh && hits[perfect].mis == 0) perfect++;
        if (perfect >= g->count) { take = perfect; }   /* identical copies: same code, same patch */
        else {
            take = perfect;
            while (take < g->count && take < nh) {
                /* an imperfect match must clearly beat the next candidate, or we can't tell which one it is */
                if (take + 1 < nh && hits[take + 1].mis < hits[take].mis + margin) break;
                take++;
            }
        }
        if (take >= g->count) break;
      }
        if (perfect > g->count) extra_copies += perfect - g->count;
        if (take < g->count && g->critical) rep->critical_missing++;
#ifdef PORT_DEBUG
        if (take != g->count) fprintf(stderr, "  site ref %#x (count %d, critical %d): %d core matches, %d perfect, took %d\n", g->ref, g->count, g->critical, nh, perfect, take);
#endif
        for (int h = 0; h < take; h++) {
            uint32_t at = hits[h].ins;
            PatchEntry ne; memset(&ne, 0, sizeof ne);
            int64_t fo = port_rva2off(&pe, at + g->off);
            ne.tier = g->tier; ne.off = (unsigned)fo; ne.len = 4; memcpy(ne.oldb, buf + fo, 4);
            if (g->src == S_IMM_F_INV64) ne.kind = K_F_INV;
            else {
                int si = g->dst == K_F_INV ? 0 : g->dst == K_F_RATE ? 1 : g->dst == K_D_INV ? 2 : 3;
                int32_t nd = (int32_t)(slot[si] - (at + g->size));
                ne.kind = K_FIXED; memcpy(ne.newb, &nd, 4);
            }
            int dup = -1;
            for (int u = 0; u < nused; u++) if (used[u] == at) dup = u;
            if (dup >= 0) {
                /* two groups landed on the same instruction: fine if they want the identical patch */
                const PatchEntry *pe0 = NULL;
                for (int q = 0; q < n; q++) if (pe_out[q].off == ne.off) pe0 = &pe_out[q];
                if (!pe0 || pe0->kind != ne.kind || memcmp(pe0->newb, ne.newb, 4)) rep->duplicates++;
                else if (h < g->count) rep->matched++;
                continue;
            }
            if (nused >= ucap) { ucap *= 2; used = realloc(used, ucap * sizeof *used); }
            used[nused++] = at;
            if (h < g->count) rep->matched++;
            if (n >= ocap) { ocap *= 2; pe_out = realloc(pe_out, ocap * sizeof *pe_out); }
            pe_out[n++] = ne;
        }
    }
    free(hits);
    rep->ambiguous = extra_copies;
    /* 5. 1/64 reads in the new DLL that no reference site accounts for (possible new tick code) */
    for (int c = 0; c < ncand; c++) {
        if (cand[c].kind != S_F_INV64 && cand[c].kind != S_D_INV64) continue;
        int hit = 0;
        for (int u = 0; u < nused && !hit; u++) { uint32_t d = cand[c].rva - used[u]; if (d < 12) hit = 1; }
        if (!hit) uncovered_inv++;
    }
    rep->uncovered = uncovered_inv;
    free(cand); free(used);

    /* 6. decide */
    int base_unc = fi == 0 ? PORT_BASE_UNCOVERED0 : fi == 1 ? PORT_BASE_UNCOVERED1 : PORT_BASE_UNCOVERED2;
    if (!rep->slots_ok) snprintf(rep->reason, sizeof rep->reason, "no free padding for the tick constants");
    else if (rep->critical_missing) snprintf(rep->reason, sizeof rep->reason, "%d critical tick site(s) not found", rep->critical_missing);
    else if (rep->duplicates) snprintf(rep->reason, sizeof rep->reason, "%d site(s) matched the same place", rep->duplicates);
    else if (rep->matched * 1000 < nsites * PORT_MIN_MATCH_PERMILLE) snprintf(rep->reason, sizeof rep->reason, "only %d of %d tick sites found", rep->matched, nsites);
    else if (rep->ambiguous > PORT_MAX_EXTRA_COPIES) snprintf(rep->reason, sizeof rep->reason, "%d unexpected extra copies of tick code", rep->ambiguous);
    else if (rep->uncovered > base_unc + PORT_MAX_NEW_UNCOVERED) snprintf(rep->reason, sizeof rep->reason, "%d new uses of the tick interval this patch doesn't know", rep->uncovered - base_unc);
    else { rep->accepted = 1; *out = pe_out; *nout = n; return 1; }
    free(pe_out);
    return 0;
}
