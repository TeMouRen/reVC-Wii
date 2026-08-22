# Wii Streaming Baseline Report

`report_streaming_baseline.py` creates a fail-closed JSON report from one or
more user-operated, run-specific Dolphin logs.

It starts each run at the first complete `[WII-STREAM-DIAG]` window with a
nonzero victim count. Multiple runs are aligned by complete summary-window
count, or by `--active-duration-ms` when an explicit duration is supplied.
Rates use each run's actual selected duration, so normal 5-second diagnostic
jitter does not bias comparisons.

The command rejects duplicate logs, mixed profiles, mixed builds, invalid
sidecar data, missing trim windows, and existing output files.

```powershell
python tools/wii/report_streaming_baseline.py `
  --log "C:\Users\20493\AppData\Roaming\Dolphin Emulator\Logs\dolphin-REVC02-20260809-172818-562.log" `
  --output reports/streaming-baselines/P4-noaudio-balanced-washington-beach-v1.json `
  --route-id washington-beach-v1 `
  --checkpoint second-splash `
  --checkpoint washington-beach `
  --expected-profile P4-noaudio-balanced `
  --expected-build 8a0122cab4a2-dirty-20260809T091551Z `
  --dol build/src/main.dol
```

Repeat `--log` for the three matched runs of one profile. Do not combine P4 and
P5 in one report; generate one report per profile and compare their aggregate
median, range, and worst values. `comparison_readiness.ready` remains false
until the report contains at least three matched runs, and manual visual
confirmation remains mandatory.

Streaming rates and load latency use the normalized active interval. Memory
minima, allocator counters, failure markers, and archive-ceiling changes cover
the full run; active-interval copies are included where correlation is useful.

`frame_samples` is explicitly diagnostic-log sampling, not an all-frame
histogram. It is useful for triage only. Lifecycle-v2 detailed builds emit an
all-frame histogram for correlation, while final performance acceptance still
requires the same histogram with per-resource event logging disabled.

## Detailed lifecycle diagnostic

Build the same P4 policy with per-resource evidence enabled. This changes only
diagnostic state and logging; it does not change the memory profile, archive
ceiling, victim policy, or per-frame removal limit.

```powershell
$env:WII_MEMORY_PROFILE_ID = "P4-noaudio-balanced"
$env:WII_STREAM_MEMORY_DIAGNOSTICS = "ON"
$env:WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS = "ON"
bash ./build.sh
```

Run one user-operated cold boot over the fixed route. Detailed logging is for
root-cause attribution, so do not use that run for frame-performance
acceptance. Generate its report with:

```powershell
python tools/wii/report_streaming_diagnostics.py `
  --log "C:\Users\20493\AppData\Roaming\Dolphin Emulator\Logs\dolphin-REVC02-YYYYMMDD-HHMMSS-NNN.log" `
  --output reports/streaming-diagnostics/P4-lifecycle-YYYYMMDD.json `
  --route-id washington-beach-v1 `
  --checkpoint second-splash `
  --checkpoint washington-beach `
  --expected-profile P4-noaudio-balanced `
  --expected-build BUILD_ID_FROM_RUN_START `
  --dol build/src/main.dol
```

The lifecycle-v2 report separates request-to-dispatch queue time from
dispatch-to-load service time, aggregates repeated trim/reload resources,
records actual type-priority victims against the globally oldest eligible
counterfactual, and correlates all-frame 40 ms events with `MakeSpaceFor` work.
The report intentionally sets `performance_acceptance_allowed` to false.

## P5 global-LRU experiment

P5 keeps the complete P4 memory tuple and archive limits. Its only streaming
policy change is a single global loaded-list pass for healthy-pool archive
trims; real generic, newlib, and GX pressure retain their pool-aware
type-priority passes.

```powershell
$env:WII_MEMORY_PROFILE_ID = "P5-noaudio-global-lru"
$env:WII_STREAM_MEMORY_DIAGNOSTICS = "ON"
$env:WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS = "ON"
bash ./build.sh
```

Use one detailed cold-boot route first to confirm that queue and service
latencies use the same clock and that no-pressure victim counterfactual
bypasses fall to zero. If that run is memory-safe and reduces reload churn,
build with `WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS=OFF` and collect three matched
runs for performance acceptance. P5 remains an audio-off lifecycle experiment;
it does not validate the final audio partition.
