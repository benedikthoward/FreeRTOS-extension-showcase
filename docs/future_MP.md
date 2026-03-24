# Future Improvements — Multiprocessor EDF Support

## High Priority

### 1. Optimal Partitioning Algorithms (FFD, BFD)

The current WFD heuristic is simple but can reject feasible task sets. Implement
First-Fit Decreasing (FFD) and Best-Fit Decreasing (BFD) as alternative
partitioning strategies. FFD assigns each task to the first core that fits; BFD
assigns to the tightest-fitting core. Both achieve better bin-packing in practice
than WFD. Expose the strategy as a configuration option
(`configMP_PARTITION_ALGORITHM`).

### 2. Global SRP (MSRP)

The current SRP implementation is single-core only. Implement the Multiprocessor
Stack Resource Policy (MSRP), which extends SRP to shared resources across cores.
MSRP uses non-preemptive spinning for global resources and local ceilings for
per-core resources. This would enable safe resource sharing under both global and
partitioned EDF.

### 3. Runtime Mode Switching

Allow switching between global and partitioned EDF at runtime without
reflashing. This requires:
- Draining all active EDF tasks (wait for idle or force-stop).
- Reconfiguring the ready queue structure (single global queue vs per-core queues).
- Re-admitting all tasks under the new mode's admission test.
Expose via an API such as `vTaskMpSetMode( mpMODE_GLOBAL | mpMODE_PARTITIONED )`.

---

## Medium Priority

### 4. Per-Core Timeline in GUI

The host monitor (`host/monitor.py`) currently shows a single Gantt chart. For
multiprocessor scenarios, display a separate timeline lane per core so that
parallel execution is clearly visible. Core ID is already included in `#TS`
trace events; the GUI needs to parse and separate the lanes.

### 5. Load Balancing Daemon

Implement a low-priority background task that periodically evaluates per-core
utilization and migrates tasks to reduce imbalance. The daemon would use
`ulTaskMpGetCoreUtilization()` to detect skew and `xTaskMpMigrate()` to
rebalance. Configurable via `configMP_LOAD_BALANCE_PERIOD_MS` and a threshold
parameter.

### 6. Support for More Than 2 Cores

The current implementation assumes `configNUMBER_OF_CORES == 2` (RP2350
dual-core). Generalize the core affinity bitmask, per-core utilization tracking,
and WFD partitioning to support N cores. This would enable porting to platforms
like RP2350-based boards with external core expanders or future multi-core MCUs.

---

## Low Priority / Optimizations

### 7. Heterogeneous Core Support

Allow cores to have different speeds (processing capacities). The admission test
and partitioning heuristics would need to account for per-core speed factors
(e.g., core 0 at 150 MHz, core 1 at 48 MHz). WCET would be scaled by the
speed ratio when computing per-core utilization.

### 8. Power-Aware Multiprocessor Scheduling

When a core has no ready EDF tasks, enter a deep sleep mode (WFI on Cortex-M33)
instead of running the idle task. In partitioned mode, a lightly loaded core
could be powered down entirely if its next task release is far in the future.
Integrate with FreeRTOS tickless idle, computing the next wakeup time from the
per-core EDF release queue.

### 9. Cache-Aware Partitioning

On processors with per-core caches, task migration invalidates cache lines and
degrades performance. Implement a cache-affinity heuristic that biases the
partitioning algorithm toward keeping tasks on the core where they have warm
cache state. Track per-task migration counts and penalize frequent movers in the
load balancing daemon.

### 10. Migration Cost Modeling

Currently, `xTaskMpMigrate()` does not account for the overhead of migration
(cache flush, TLB invalidation on applicable platforms, context switch). Model
the migration cost as a configurable parameter (`configMP_MIGRATION_COST_TICKS`)
and subtract it from the destination core's available capacity during admission
checks for migrated tasks.
