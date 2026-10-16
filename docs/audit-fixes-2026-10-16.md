# CQ scratch ownership (2026-10-16)

The worker now owns its fixed 256-entry RIORESULT scratch array for its entire lifetime.
`cec_runtime_drain` reuses this storage instead of value-initializing a local array on every call.
The provider writes the returned prefix; the existing corrupt-CQ and extent checks run before
processing that prefix. Each worker retains its own storage until the coordinator joins it.

This does not change Batch v1, native outstanding accounting, CLI or statistics. The retained
Release/Debug gates cover state machines, transport boundaries, process/quota and reset recovery.
This source change makes no measured throughput or CPU improvement claim.

Validation ran in order: `format.ps1`, `build_release.ps1`, `build_debug.ps1`; all exited 0.
Both modes passed the full MSBuild, unit/contract, runtime, pure engine, fail-fast, source policy,
process, quota and reset storm gates, with zero MSBuild warnings. The source file SHA-256 is
`5b05f1d3f053c1581fc81444c215bbc9f70abd5bb0d1a4ac5a852fe96f106c13`.

| Product | SHA-256 |
| --- | --- |
| x64 Release | `796dc547ff810796603e945ede6c0724aac9a081948f0c9a841859ef745b1c74` |
| x64 Debug | `e7a1c2142a5a33b76c44086ac619ae9e8bd5aa571402fe01b4b878eb34495bad` |

Complete retained coordinator logs:
`C:\Users\Administrator\echo-project\.superpowers\audit-fixes-20261016\cpp-echo-client-20261016T151539034Z`.
