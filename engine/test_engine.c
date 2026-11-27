/* Linux test: patch disposable copies; write <name>.<rate>.dll; check hashes. */
#include <stdlib.h>
#include "tickpatch.h"

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n); if (fread(b, 1, *n, f) != *n) { free(b); b = NULL; }
    fclose(f); return b;
}

int main(int argc, char **argv) {
    const char *names[3] = {"engine2", "server", "client"};
    int rates[2] = {128, 256}, total = 0, fail = 0;
    for (int r = 0; r < 2; r++) {
        total = 0;
        for (int i = 0; i < 3; i++) {
            char p[512], sha[65], err[128]; size_t n;
            snprintf(p, sizeof p, "%s/%s.dll", argc > 1 ? argv[1] : ".", names[i]);
            uint8_t *b = slurp(p, &n); if (!b) { printf("cannot read %s\n", p); return 1; }
            tp_sha256(b, n, sha);
            const char *build; const tp_dll *t = tp_find(i, sha, &build);
            if (!t) { printf("%s: unknown build %s\n", names[i], sha); fail++; continue; }
            int e = tp_patch_buffer(t, b, n, rates[r], err, sizeof err);
            if (e < 0) { printf("%s: %s\n", names[i], err); fail++; continue; }
            /* patching a patched buffer must be refused */
            uint8_t *c = malloc(n); memcpy(c, b, n);
            if (tp_patch_buffer(t, c, n, rates[r], err, sizeof err) >= 0) { printf("%s: double patch NOT refused\n", names[i]); fail++; }
            free(c);
            char psha[65]; tp_sha256(b, n, psha);
            printf("%s %d tick: %d edits, build %s, patched sha %.16s\n", names[i], rates[r], e, build, psha);
            total += e;
            snprintf(p, sizeof p, "%s/%s.%d.dll", argc > 1 ? argv[1] : ".", names[i], rates[r]);
            FILE *f = fopen(p, "wb"); fwrite(b, 1, n, f); fclose(f);
            free(b);
        }
        printf("total edits at %d: %d\n", rates[r], total);
    }
    return fail ? 1 : 0;
}
