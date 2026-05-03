# Chart Plan For benchmark_run_1777321841.json

## Main Deck
1. `timing_01_wait_time_summary.png`
   Open with the queue-cost view: average wait, blocked-only wait, worst-case wait, and blocked fraction by scenario.
   Current highlights: The Thundering Herd has the largest all-op average wait (64 ms), The Thundering Herd has the largest blocked-only average wait (71 ms), and The Thundering Herd has the worst single wait (124 ms).
2. `timing_02_latency_component_breakdown.png`
   Use this to show where latency comes from. In this run, queue wait and hold/release tail dominate; Qdrant itself is comparatively small.
   Hotspot group average wait is 27.23 ms, while its average Qdrant window is only 2.69 ms.
3. `timing_03_hotspot_vs_cold_wait.png`
   This is the architectural comparison slide: hotspot vs mixed vs cold path.
   Cold-path traffic still sees tiny waits, but the magnitude stays low: avg wait 1.08 ms, blocked-only avg 4.33 ms, max wait 10 ms.
   Hotspot writes are the expensive path: blocked-only write wait averages 42.92 ms versus 1.00 ms on the cold path.
4. `timing_04_queue_depth_vs_wait.png`
   Use this to explain that queue depth and queue-hopping amplify timing cost under contention.
   Queue depth is most visible in Queue Hopping (max wait position 19) and queue hopping is most visible in Queue Hopping (max queue hops 18).
5. `timing_05_resource_heatmaps.png`
   Keep this as backup only if you want a resource snapshot after the timing story is clear.

## Timing Story
- Hotspot group: avg wait 27.23 ms, blocked-only avg 36.88 ms, max wait 124 ms, blocked fraction 73.8%.
- Mixed group: avg wait 17.75 ms, blocked-only avg 37.64 ms, max wait 74 ms, blocked fraction 47.2%.
- Cold group: avg wait 1.08 ms, blocked-only avg 4.33 ms, max wait 10 ms, blocked fraction 25.0%.

## Appendix Timelines
Use the per-case timing timelines to show exactly where time is spent: queue wait, active lock hold, and Qdrant completion.
Recommended appendix cases for this run:
- `timing_timelines/timing_timeline_case_01_the_thundering_herd.png`: The Thundering Herd | avg blocked wait 71 ms | max wait 124 ms | wait P95 124 ms
- `timing_timelines/timing_timeline_case_06_the_ghost_client.png`: The Ghost Client | avg blocked wait 36 ms | max wait 58 ms | wait P95 58 ms
- `timing_timelines/timing_timeline_case_08_queue_hopping.png`: Queue Hopping | avg blocked wait 33 ms | max wait 61 ms | wait P95 59 ms
- `timing_timelines/timing_timeline_case_05_the_strict_sieve.png`: The Strict Sieve | avg blocked wait 4 ms | max wait 10 ms | wait P95 10 ms
- `timing_timelines/timing_timeline_case_07_the_almost_collision.png`: The Almost Collision | avg blocked wait 0 ms | max wait 0 ms | wait P95 0 ms
- `timing_timelines/timing_timeline_case_03_the_read_starvation_trap.png`: The Read-Starvation Trap | avg blocked wait 0 ms | max wait 0 ms | wait P95 0 ms

## Notes
- The curated benchmark latency numbers exclude embedding generation because embeddings are precomputed before the per-case run begins.
- The main architectural cost here is queue wait plus any hold/release tail, not Qdrant execution time.
- Container charts are one-shot snapshots collected after each case, not continuous peak measurements.
- All generated outputs for this run are listed below.

- `02_correctness_parallelism.png`
- `06_wait_fairness.png`
- `07_queue_depth_fairness.png`
- `timing_01_wait_time_summary.png`
- `timing_02_latency_component_breakdown.png`
- `timing_03_hotspot_vs_cold_wait.png`
- `timing_04_queue_depth_vs_wait.png`
- `timing_05_resource_heatmaps.png`
- `timing_timelines/timing_timeline_case_01_the_thundering_herd.png`
- `timing_timelines/timing_timeline_case_02_the_semantic_interleaving.png`
- `timing_timelines/timing_timeline_case_03_the_read_starvation_trap.png`
- `timing_timelines/timing_timeline_case_04_the_permissive_sieve.png`
- `timing_timelines/timing_timeline_case_05_the_strict_sieve.png`
- `timing_timelines/timing_timeline_case_06_the_ghost_client.png`
- `timing_timelines/timing_timeline_case_07_the_almost_collision.png`
- `timing_timelines/timing_timeline_case_08_queue_hopping.png`
- `timing_timelines/timing_timeline_case_09_the_mixed_stagger.png`
- `timing_timelines/timing_timeline_case_10_the_100_read_stampede.png`
- `timing_timelines/timing_timeline_case_11_the_paraphrase_gauntlet.png`
- `timing_timelines/timing_timeline_case_12_the_cross_domain_flood.png`
- `timing_timelines/timing_timeline_case_13_the_write_pressure_ratchet.png`
