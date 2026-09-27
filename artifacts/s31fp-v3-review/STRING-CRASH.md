# Detached cancellation test race: cause and correction

The first revised-policy host run printed zero mismatches, followed by SIGSEGV.
Successful reruns did not explain it. Reproduction retained both an untraced
core (failure on round 6) and a syscall-traced core (round 4). These are host
QEMU core containers, but the backtrace shows QEMU deliberately propagating a
**guest** SIGSEGV via dump_core_and_abort; this is not evidence of a QEMU bug.

Resolved guest state:

| Evidence | Traced | Untraced |
| --- | --- | --- |
| Guest PC | 0x2b306808 | 0x2b306782 |
| libc base | 0x2b2ac000 | 0x2b2ac000 |
| libc PC offset | __lock+0x9a (0x5a808) | __lock+0x14 (0x5a782) |
| Caller RA | 0x2b30896e (pthread_kill+0x1c) | same |
| Target exit lock | 0x2b3f9de0 | 0x2b387de0 |
| Saved return into test | 0x55557696 | same |

The test load base is 0x55556000 (gp 0x5555c804 minus linked gp 0x6804).
Return offset 0x1696 follows main's call to pthread_cancel at 0x1692 in the
preserved pre-fix strtest. The traced target munmaps [0x2b3d7000, +143360)
and exits, then the parent faults reading its exit lock at 0x2b3f9de0.

The test assumed a detached target was safe until the cancellation request
started. It must remain alive through the complete request. pthread_cancel
publishes the cancel flag before pthread_kill acquires the target's exit lock;
a target polling pthread_testcancel can act on that flag and unmap its own
thread storage while the parent still needs it. That is exactly the recorded
stack, address and syscall ordering. No string comparison is on this path.

Correction: cancellation targets disable cancellation before publishing
readiness. They execute their string checks, then wait on a condition variable.
The parent waits for readiness, completes pthread_cancel, and only then
releases them to re-enable cancellation and run cleanup. Completion remains
condition-variable based. No guessed delay or application changes.

Validation so far: 30 scalar + 30 dispatch host runs, each 75001 cases /
975013 calls, zero mismatches and zero process failures (cancel-fix-host.txt).
This exceeds the two short failing reproductions. Real-board validation is
recorded separately. Preserve the disassembly/core-derived guest-state text;
the large cores and binaries are local diagnostic artifacts, not repository
payload. The production library was not changed to mask a harness fault.
