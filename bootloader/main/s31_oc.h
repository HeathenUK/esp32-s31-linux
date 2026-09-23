/*
 * CPU clock override for the overclock arms (docs/perf-plan-2026-09-23.md,
 * Phase 5). 0 = leave the sdkconfig frequency alone (the shipped value).
 * scripts/board/build-oc.sh rewrites this for one build and restores it.
 * Anything other than 0 also needs the vendor clock code patched for that
 * frequency (the same script does it, in the container, for that build only)
 * and the kernel DTS + OpenSBI timebases set to match.
 */
#define S31_CPU_OC_MHZ 0
