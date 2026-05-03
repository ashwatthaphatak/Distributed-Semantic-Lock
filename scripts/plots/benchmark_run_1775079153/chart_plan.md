# Chart Plan For benchmark_run_1775079153.json

## Main Deck
1. `01_overview_latency_vs_throughput.png`
   Open with this figure. It summarizes the benchmark envelope in one view.
   Current highlights: The Strict Sieve has the highest throughput (250.00 ops/sec) and The Almost Collision has the lowest P95 latency (11 ms).
2. `02_correctness_parallelism.png`
   Use this to separate semantic safety from useful concurrency.
   Cases with correctness violations in this run: C1 The Thundering Herd (2 violations), C2 The Semantic Interleaving (3 violations), C8 Queue Hopping (1 violations), C10 The 100% Read Stampede (3 violations)
3. `03_contention_fairness.png`
   Use this to explain queue depth, wakeups, and requeue pressure under load.
   Queue stress is most visible in Queue Hopping (max wait position 18) and Queue Hopping (max queue hops 17).
4. `04_resource_heatmaps.png`
   Keep this as an appendix or a backup slide for resource-footprint questions.

## Appendix Timelines
Use the per-case timeline plots to show exactly when waiting, granting, release, and any violation windows happened.
Recommended appendix cases for this run:
- `timelines/timeline_case_01_the_thundering_herd.png`: The Thundering Herd | P95 7661 ms | serialization 95.6% | distinct parallelism 0.0%
- `timelines/timeline_case_02_the_semantic_interleaving.png`: The Semantic Interleaving | P95 3815 ms | serialization 85.0% | distinct parallelism 32.0%
- `timelines/timeline_case_08_queue_hopping.png`: Queue Hopping | P95 105 ms | serialization 99.5% | distinct parallelism 0.0%
- `timelines/timeline_case_10_the_100_read_stampede.png`: The 100% Read Stampede | P95 47 ms | serialization 93.3% | distinct parallelism 0.0%
- `timelines/timeline_case_05_the_strict_sieve.png`: The Strict Sieve | P95 40 ms | serialization 100.0% | distinct parallelism 100.0%

## Notes
- The curated benchmark latency numbers exclude embedding generation because embeddings are precomputed before the per-case run begins.
- Container charts are one-shot snapshots collected after each case, not continuous peak measurements.
- All generated outputs for this run are listed below.

- `01_overview_latency_vs_throughput.png`
- `02_correctness_parallelism.png`
- `03_contention_fairness.png`
- `04_resource_heatmaps.png`
- `timelines/timeline_case_01_the_thundering_herd.png`
- `timelines/timeline_case_02_the_semantic_interleaving.png`
- `timelines/timeline_case_03_the_read_starvation_trap.png`
- `timelines/timeline_case_04_the_permissive_sieve.png`
- `timelines/timeline_case_05_the_strict_sieve.png`
- `timelines/timeline_case_06_the_ghost_client.png`
- `timelines/timeline_case_07_the_almost_collision.png`
- `timelines/timeline_case_08_queue_hopping.png`
- `timelines/timeline_case_09_the_mixed_stagger.png`
- `timelines/timeline_case_10_the_100_read_stampede.png`
