# Wii Phase 0 Memory Sidecar Schema

Schema version: `1`

The Wii runtime emits one JSON object per line after the `[WII-P0] ` marker.
`extract_memory_sidecar.py` validates all runtime records before creating a
JSON Lines sidecar. The output path is opened exclusively and is never
overwritten.

## Runtime events

The first record must be one `run_start` event with `sequence=0` and
`elapsed_ms=0`. At least one `snapshot` event must follow. Sequence numbers
must increase strictly and elapsed time must not decrease.

Every event contains the complete version 1 field set. Unknown or missing
fields are rejected. `profile_id`, `build_id`, fixed boundaries, allocator
mode, texture policy, and candidate state must remain unchanged for the run.

## Address provenance

- `runtime_arena2_lo/hi`: application-owned Arena2 reported by libogc at boot.
- `linker_arena2_lo/hi`: linker fallback symbols. They are recorded for
  provenance and may differ from the runtime range.
- `generic_base/end`: fixed generic MEM2 pool.
- `process_heap_claim_base/end`: Arena2 interval claimed through the process
  heap cursor. The interval may be empty.
- `raw2_lo/hi`: current unclaimed Arena2 interval, which may be empty.
- `gx_base/end`: fixed GX pool.
- `shared_reserve_base/end/state`: zero/zero/`disabled` in Phase 0.

`process_heap_claim_base` equals `generic_end`,
`process_heap_claim_end` equals `raw2_lo`, and `raw2_hi` equals `gx_base`.
`raw2_lo` may move as `sbrk` claims or returns Arena2; the fixed boundaries may
not move. All active regions must remain ordered and inside the runtime Arena2
range.

## Accounting

Generic and GX records include capacity, used, free, and largest-free bytes.
Process heap fields come from `mallinfo()` and are labelled
`process_heap_*`; they are not a separate physical MEM2 pool and must not be
summed with the physical region sizes. `raw2_remaining` must equal
`raw2_hi - raw2_lo`.

Owner and unknown bytes currently cover texture-pool entries only. System
ownership is not yet separated and is emitted as `null`. `txd_failure_count`
is also `null` until a non-`gxread.cpp` observation point is added. A null gate
blocks candidate acceptance; it is not interpreted as zero.

`texture_candidate_state` is fixed to `blocked` in Phase 0. No sidecar record
can enable or select a texture candidate.

## Extraction

```text
python tools/wii/extract_memory_sidecar.py DOLPHIN_LOG OUTPUT_JSONL
python tools/wii/extract_memory_sidecar.py DOLPHIN_LOG OUTPUT_JSONL --run-id RUN_ID
```

Without `--run-id`, the input log filename stem is used. The sidecar adds
`run_id`, absolute `source_log`, and `source_line` to each validated record.
