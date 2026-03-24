# Testing Document — Multiprocessor EDF Support

This document describes the testing methodology and all test cases for the
multiprocessor EDF scheduling extension. Tests are implemented as interactive
scenarios in `src/mp_demo.c`, selectable via a UART menu at boot. The menu
adapts to the compile-time mode (global or partitioned).

---

## General Testing Methodology

- **Platform**: Raspberry Pi Pico 2 W (RP2350, dual Cortex-M33), dual-core mode.
- **Observation**: Structured trace events (`#TS` with core IDs) emitted over
  USB serial, parsed by the PyQt6 host monitor (`host/monitor.py`) for live Gantt
  charts and per-core utilization statistics. Human-readable `printf` output
  accompanies all scenarios.
- **Busy-wait simulation**: Tasks simulate computation by busy-waiting for their WCET
  using `vBusyWait()`, which loops on `xTaskGetTickCount()`. This provides
  deterministic, tick-accurate execution times.
- **Compile-time mode selection**: Global EDF is enabled with `GLOBAL_EDF_ENABLE=1`;
  partitioned EDF with `PARTITIONED_EDF_ENABLE=1`. Scenarios 2-4 are only available
  in partitioned mode; scenario 1 is only available in global mode.

---

## Test Cases

### Scenario 1: Global EDF (4 Tasks, 2 Cores)

**Purpose**: Verify that global EDF correctly schedules a task set with total
utilization exceeding 1.0 across two cores, allowing task migration.

**Task set**:

| Task | Period (T) | Deadline (D) | WCET (C) | Utilization |
|------|-----------|-------------|---------|------------|
| tau1 | 100 ms | 100 ms | 30 ms | 0.30 |
| tau2 | 150 ms | 150 ms | 40 ms | 0.27 |
| tau3 | 200 ms | 200 ms | 50 ms | 0.25 |
| tau4 | 300 ms | 300 ms | 60 ms | 0.20 |

Total utilization: 1.02 (fits on 2 cores since U <= 2.0).

**Methodology**:
- All four tasks are created with `xTaskCreateEDF()` and admitted under the
  global bound (U <= m = 2.0).
- Tasks run for 3 seconds. A monitor task prints per-core utilization via
  `ulTaskMpGetCoreUtilization()` after the run.
- Trace events (`#TS`) include core IDs, allowing the host monitor to display
  which core each task executes on.

**What to verify**:
- All 4 tasks are admitted (total U = 1.02 <= 2.0).
- At every tick, the two tasks with the earliest absolute deadlines are running
  on the two cores. This can be confirmed by inspecting `#TS` trace events.
- Tasks may migrate between cores across jobs — this is expected and correct for
  global EDF.
- No deadline misses occur.

**Result**: PASS — all tasks admitted, dual-core EDF schedule correct, no misses.

---

### Scenario 2: Partitioned EDF — Automatic WFD Assignment

**Purpose**: Verify that the Worst-Fit Decreasing (WFD) heuristic automatically
assigns tasks to cores such that per-core utilization stays below 1.0.

**Task set**: Same as Scenario 1.

| Task | Period (T) | Deadline (D) | WCET (C) | Utilization |
|------|-----------|-------------|---------|------------|
| tau1 | 100 ms | 100 ms | 30 ms | 0.30 |
| tau2 | 150 ms | 150 ms | 40 ms | 0.27 |
| tau3 | 200 ms | 200 ms | 50 ms | 0.25 |
| tau4 | 300 ms | 300 ms | 60 ms | 0.20 |

Total utilization: 1.02.

**Methodology**:
- Tasks are created with `xTaskCreateEDFAutoPartition()`, which uses WFD to
  select the core with the most remaining capacity for each task.
- After creation, per-core utilization is printed via `ulTaskMpGetCoreUtilization()`.
- Tasks run for 3 seconds under the monitor.

**What to verify**:
- All 4 tasks are admitted (WFD finds a feasible partition).
- Per-core utilization is balanced — neither core exceeds 1.0.
- Tasks are pinned to their assigned cores and do not migrate.
- No deadline misses occur.

**Result**: PASS — WFD produces a balanced partition, all tasks meet deadlines.

---

### Scenario 3: Partitioned EDF — Manual Assignment (2+2)

**Purpose**: Verify that tasks can be manually assigned to specific cores and
that per-core admission control correctly enforces the utilization bound.

**Partition plan**:

| Core | Tasks | Per-Core Utilization |
|------|-------|---------------------|
| Core 0 | tau1 (T=100, C=30), tau2 (T=150, C=40) | 0.30 + 0.27 = 0.57 |
| Core 1 | tau3 (T=200, C=50), tau4 (T=300, C=60) | 0.25 + 0.20 = 0.45 |

**Methodology**:
- Tasks are created with `xTaskCreateEDFPartitioned()`, specifying the target
  core explicitly.
- Per-core utilization is printed after creation.
- Tasks run for 3 seconds under the monitor.

**What to verify**:
- All 4 tasks are admitted to their designated cores.
- Each core runs an independent EDF schedule over its assigned tasks only.
- Per-core utilization matches the expected values (0.57 and 0.45).
- Tasks never migrate — `#TS` trace events consistently show the assigned core.
- No deadline misses occur.

**Result**: PASS — manual partition respected, independent per-core EDF correct.

---

### Scenario 4: Task Migration at t=2s

**Purpose**: Verify that a task can be migrated from one core to another at
runtime via `xTaskMpMigrate()`, with correct admission control on the
destination core.

**Initial partition**:

| Core | Tasks | Per-Core Utilization |
|------|-------|---------------------|
| Core 0 | tau1 (T=200, C=30), tau2 (T=300, C=40) | 0.15 + 0.13 = 0.28 |
| Core 1 | tau3 (T=200, C=50), tau4 (T=500, C=30) | 0.25 + 0.06 = 0.31 |

**Migration event**: At t=2s, a migration controller task calls
`xTaskMpMigrate(tau4, 0)` to move tau4 from core 1 to core 0.

**After migration**:

| Core | Tasks | Per-Core Utilization |
|------|-------|---------------------|
| Core 0 | tau1, tau2, tau4 | 0.15 + 0.13 + 0.06 = 0.34 |
| Core 1 | tau3 | 0.25 |

**Methodology**:
- Four tasks are created with `xTaskCreateEDFPartitioned()` in the initial layout.
- A `vMigrationController` task sleeps for 2 seconds, then calls
  `xTaskMpMigrate()` to move tau4 to core 0.
- The migration result is printed (OK or FAILED).
- The monitor reports per-core utilization after 3 seconds.

**What to verify**:
- Before t=2s, tau4 executes on core 1 (visible in `#TS` events).
- The migration call returns `pdPASS` (core 0 has capacity for tau4).
- After t=2s, tau4 executes on core 0.
- All tasks continue meeting deadlines after the migration.
- Per-core utilization shifts as expected.

**Result**: PASS — migration succeeds, utilization rebalanced, no misses.

---

### Scenario 5: Admission Control Comparison (Global vs Partitioned)

**Purpose**: Demonstrate the difference between global and partitioned admission
control on the same task set.

**Task set**:

| Task | Period (T) | Deadline (D) | WCET (C) | Utilization |
|------|-----------|-------------|---------|------------|
| tau1 | 100 ms | 100 ms | 30 ms | 0.30 |
| tau2 | 100 ms | 100 ms | 40 ms | 0.40 |
| tau3 | 100 ms | 100 ms | 50 ms | 0.50 |
| tau4 | 100 ms | 100 ms | 30 ms | 0.30 |

Total utilization: 1.50.

**Methodology**:
- The scenario is available in both compile-time modes.
- **Global mode**: Tasks are created with `xTaskCreateEDF()`. The global bound
  is U <= m = 2.0. All 4 tasks (U=1.50) should be accepted.
- **Partitioned mode**: Tasks are created with `xTaskCreateEDFAutoPartition()`
  using WFD. Per-core bound is U <= 1.0. WFD attempts to bin-pack tasks onto
  2 cores; per-core utilization is printed after each admission.
- Admission results (ACCEPTED / REJECTED / ERROR) are printed for each task.

**What to verify**:
- **Global EDF**: All 4 tasks accepted (1.50 <= 2.0).
- **Partitioned EDF (WFD)**: WFD may reject tau4, since the greedy assignment
  could leave insufficient capacity on both cores (e.g., core 0 gets tau3=0.50
  and tau1=0.30, core 1 gets tau2=0.40 — tau4=0.30 fits on core 1 at 0.70).
  The exact result depends on WFD ordering.
- The scenario highlights that global EDF has a more permissive admission bound
  than partitioned EDF.

**Result**: PASS — global admits all tasks; partitioned WFD prints per-core
assignments and accepts or rejects based on greedy bin-packing.

---

## Summary

| Scenario | Tests | Result |
|----------|-------|--------|
| 1. Global EDF | 4-task dual-core scheduling, migration allowed | PASS |
| 2. Partitioned WFD | Automatic worst-fit assignment, balance check | PASS |
| 3. Manual Partition | Explicit 2+2 core assignment, isolation | PASS |
| 4. Task Migration | Runtime migration via `xTaskMpMigrate()` | PASS |
| 5. Admission Comparison | Global vs partitioned admission bounds | PASS |
