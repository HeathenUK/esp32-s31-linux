/* Read-only ELF inventory, using the shipping sigs3 full-body/masked signatures.
 * Unlike the old scan2 prefix benchmark, matches mean byte-level eligibility.
 * They do NOT establish runtime call frequency, safe patch timing or a speedup.
 * Build via make s31fp-scanbench; host timings are not board scan timings. */
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sigs3.h"

static uint32_t filter[128];
static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec * 1e-6;
}
static int span(size_t size, size_t off, size_t len)
{ return off <= size && len <= size - off; }
/* Same mask semantics as preload3.c body_ok; never write the input. */
static int matches(const unsigned char *p, size_t left, const struct s31_sig *s)
{
    if (s->len > left) return 0;
    unsigned pos = 0;
    for (unsigned m = 0; m <= s->nmask; m++) {
        unsigned stop = m < s->nmask ? s->mask[m] : s->len;
        if (stop < pos || stop > s->len || memcmp(p + pos, s->body + pos, stop - pos)) return 0;
        pos = stop + 4;
    }
    return 1;
}
static int inventory(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    if (fseek(f, 0, SEEK_END)) { fclose(f); return 1; }
    long length = ftell(f);
    if (length < (long)sizeof(Elf32_Ehdr) || fseek(f, 0, SEEK_SET)) {
        fprintf(stderr, "%s: truncated ELF\n", path); fclose(f); return 1;
    }
    size_t size = (size_t)length;
    unsigned char *buf = malloc(size);
    if (!buf) { fclose(f); return 1; }
    double t0 = now();
    size_t got = fread(buf, 1, size, f);
    fclose(f);
    double t1 = now();
    Elf32_Ehdr eh;
    memcpy(&eh, buf, sizeof(eh));
    if (got != size || memcmp(eh.e_ident, ELFMAG, SELFMAG) ||
        eh.e_ident[EI_CLASS] != ELFCLASS32 || eh.e_ident[EI_DATA] != ELFDATA2LSB ||
        eh.e_machine != EM_RISCV || eh.e_phentsize != sizeof(Elf32_Phdr) ||
        !span(size, eh.e_phoff, (size_t)eh.e_phnum * sizeof(Elf32_Phdr))) {
        fprintf(stderr, "%s: invalid/unsupported RV32 ELF\n", path); free(buf); return 1;
    }
    /* Validate every segment before reporting partial results. */
    for (unsigned i = 0; i < eh.e_phnum; i++) {
        Elf32_Phdr ph;
        memcpy(&ph, buf + eh.e_phoff + i * sizeof(ph), sizeof(ph));
        if (ph.p_type == PT_LOAD && (!span(size, ph.p_offset, ph.p_filesz) || ph.p_filesz > ph.p_memsz)) {
            fprintf(stderr, "%s: invalid load segment\n", path); free(buf); return 1;
        }
    }
    unsigned hits[S31_NSIG] = {0}, total = 0, segments = 0;
    size_t text = 0;
    double scan_ms = 0;
    for (unsigned i = 0; i < eh.e_phnum; i++) {
        Elf32_Phdr ph;
        memcpy(&ph, buf + eh.e_phoff + i * sizeof(ph), sizeof(ph));
        if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X)) continue;
        segments++; text += ph.p_filesz;
        double start = now();
        for (size_t off = ph.p_vaddr & 1; off < ph.p_filesz; off += 2) {
            const unsigned char *p = buf + ph.p_offset + off;
            if (ph.p_filesz - off < 2) break;
            unsigned h = (p[0] | p[1] << 8) & 0xfff;
            if (!(filter[h >> 5] & (1u << (h & 31)))) continue;
            for (unsigned s = 0; s < S31_NSIG; s++) {
                const struct s31_sig *g = &s31_sigs[s];
                if (p[0] != g->body[0] || p[1] != g->body[1] ||
                    !matches(p, ph.p_filesz - off, g)) continue;
                hits[s]++; total++;
                printf("MATCH %s segment=%u vaddr=0x%08lx file=0x%08lx %s bytes=%u\n",
                    path, i, (unsigned long)ph.p_vaddr + off,
                    (unsigned long)ph.p_offset + off, g->name, g->len);
                off += g->len - 2;
                break;
            }
        }
        scan_ms += now() - start;
    }
    printf("SUMMARY %s type=%u xsegments=%u text=%zu read_ms=%.3f scan_report_ms=%.3f matches=%u",
        path, eh.e_type, segments, text, t1-t0, scan_ms, total);
    for (unsigned s = 0; s < S31_NSIG; s++) if (hits[s]) printf(" %s=%u", s31_sigs[s].name, hits[s]);
    puts("");
    free(buf);
    return 0;
}
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: scanbench ELF...\n"); return 2; }
    for (unsigned s = 0; s < S31_NSIG; s++) {
        unsigned h = (s31_sigs[s].body[0] | s31_sigs[s].body[1] << 8) & 0xfff;
        filter[h >> 5] |= 1u << (h & 31);
    }
    int bad = 0;
    for (int i = 1; i < argc; i++) bad |= inventory(argv[i]);
    return bad;
}
