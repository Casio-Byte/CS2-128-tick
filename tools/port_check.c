/*
 * port_check: run the app's auto-port engine on a folder of DLLs and print what it finds.
 *   gcc -O2 -o port_check tools/port_check.c        (from the repo root)
 *   ./port_check <folder with engine2.dll server.dll client.dll> [entries.txt]
 * Use it on the reference build to get the --uncovered numbers for make_signatures.py,
 * and on a new build to see whether the app will accept it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/patches.h"
#include "../src/signatures.h"
#include "../src/port.inc"
static unsigned char *rd(const char *p, uint32_t *s) { FILE *f = fopen(p, "rb"); if (!f) return 0; fseek(f, 0, 2); *s = ftell(f); rewind(f); unsigned char *b = malloc(*s); if (fread(b, 1, *s, f) != *s) { fclose(f); return 0; } fclose(f); return b; }
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: port_check <dir> [entries.txt]\n"); return 2; }
    const char *names[3] = { "engine2.dll", "server.dll", "client.dll" }, *KN[] = { "fixed", "f_inv", "f_rate", "d_inv", "d_rate" };
    FILE *o = argc > 2 ? fopen(argv[2], "w") : NULL; int bad = 0;
    for (int fi = 0; fi < 3; fi++) {
        char p[1024]; uint32_t s; snprintf(p, sizeof p, "%s/%s", argv[1], names[fi]);
        unsigned char *b = rd(p, &s); if (!b) { printf("%-12s missing\n", names[fi]); continue; }
        PatchEntry *e; int n; PortReport r; int ok = port_file(fi, b, s, &e, &n, &r);
        printf("%-12s %s  sites %d/%d  extra copies %d  critical missing %d  conflicts %d  uncovered 1/64 reads %d  %s\n",
               names[fi], ok ? "ACCEPTED" : "REFUSED ", r.matched, r.sites, r.ambiguous, r.critical_missing, r.duplicates, r.uncovered, r.reason);
        bad |= !ok;
        if (ok && o) for (int i = 0; i < n; i++) {
            fprintf(o, "%s %u %d %s ", names[fi], e[i].off, e[i].tier, KN[e[i].kind]);
            for (int k = 0; k < e[i].len; k++) fprintf(o, "%02x", e[i].oldb[k]);
            fprintf(o, " "); if (e[i].kind == 0) for (int k = 0; k < 4; k++) fprintf(o, "%02x", e[i].newb[k]); else fprintf(o, "-"); fprintf(o, "\n");
        }
        if (ok) free(e);
        free(b);
    }
    if (o) fclose(o);
    return bad;
}
