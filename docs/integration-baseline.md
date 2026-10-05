# standard-v1 C++ integration baseline

Content revision: **
v1-cpp-2026-10-05-r2
**.

| Side | Frozen HEAD |
|---|---|
| client | 
6500e34a31660db03aeec3e90ef6eca8c618d154
 |
| server | 
fd8e8e9440299f8a9f64a050cd4de49efda566f5
 |

The baseline equals the frozen content, so a baseline-relative patch is empty by definition: each
repository already carries the standard-v1 implementation for its side.
docs/integration-baseline.json holds the SHA256 of every tracked file of the frozen client commit, so
any later edit is detectable. The frozen commits are the code revisions; the record commits that
follow them in history merely document them.

## Verification at the freeze

- Release and Debug gates green on both sides, including the 400-reset accept storm
- v1 conformance -Mode All: 104/104 (CLI 86, payload 8, interop 10)
- notify diagnostics gate: 1/2/8/32 worker TCP and k=1/64/1024 UDP, per-worker gap in {0,1}, zero starvation wakeups
- 60 s soak: 4.2M echo/s, zero corrupted, zero lost, zero network errors, flat working set
- clean-room clone of the previous freeze rebuilt and passed every gate

## Revision history

- v1-cpp-2026-10-05: client c67ede9, server 8a6a87b - v1 command line, metrics line and final statistics
- v1-cpp-2026-10-05-r2: RIONotify provider seam, opt-in diagnostics, hardened gate, baseline record

## What this record does not claim

- The earlier workspace baselines (client 10d1d7d, server 30de201) no longer describe the current sources; they are historical evidence only.
- Only the frozen commits above are covered; any later commit is a new revision that must be re-frozen.
