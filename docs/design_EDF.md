# Design Document — EDF Support

This document describes the design choices and implementation approach for adding
Earliest Deadline First (EDF) scheduling to FreeRTOS on the RP2350 (Cortex-M33).

---

## High-Level Architecture

EDF is implemented as an **opt-in extension** compiled behind `configUSE_EDF == 1`.
When disabled, the kernel behaves exactly as stock FreeRTOS. The extension adds:

1. A **separate EDF ready list** (`xEdfReadyList`) sorted by absolute deadline.
2. **TCB extensions** carrying per-task timing parameters (C, D, T) and runtime state.
3. **Admission control** selecting the appropriate feasibility test automatically.
4. **Tick-level logic** for period release, deadline miss detection, and time-slicing.
5. **ISR-safe APIs** for sporadic task release and deadline updates.

```
                     ┌──────────────────────┐
                     │  xTaskCreateEDF()    │
                     │  xTaskCreateEDF-     │
                     │    Sporadic()        │
                     └─────────┬────────────┘
                               │
                     ┌─────────▼────────────┐
                     │ prvEdfAdmissionCheck  │
                     │  ┌─────────────────┐ │
                     │  │ All D==T?       │ │
                     │  │  YES → LL bound │ │
                     │  │  NO  → Demand   │ │
                     │  └─────────────────┘ │
                     └─────────┬────────────┘
                               │ pdPASS
                     ┌─────────▼────────────┐
                     │ prvEdfInitialiseTask  │
                     │ (populate TCB, reg.)  │
                     └─────────┬────────────┘
                               │
                     ┌─────────▼────────────┐
                     │   xEdfReadyList      │
                     │  (sorted by abs. D)  │
                     └──────────────────────┘
```

---

## Scheduler Design

### Task Selection (`taskSELECT_HIGHEST_PRIORITY_TASK`)

The macro is replaced in EDF builds. The new logic:

1. If `xEdfReadyList` is non-empty, pick the head (earliest deadline).
   Uses `listGET_OWNER_OF_NEXT_ENTRY` to enable round-robin among tasks with
   identical absolute deadlines when `configUSE_TIME_SLICING == 1`.
2. Otherwise, fall back to the standard priority-based scan of `pxReadyTasksLists[]`.

**Rationale**: EDF tasks always preempt non-EDF tasks. This is enforced structurally
(EDF list is checked first) rather than by manipulating priority values.

### Ready List Insertion (`prvAddTaskToReadyList`)

EDF tasks are dispatched to `xEdfReadyList` via `vListInsert`, which performs sorted
insertion by `xItemValue` (set to the task's absolute deadline). Non-EDF tasks use the
original priority-indexed array.

**Rationale**: Keeping a separate sorted list avoids modifying the core priority-based
scheduler. Sorted insertion is O(n) per insert, but n is bounded by `configEDF_MAX_TASKS`
and insertions are infrequent relative to tick interrupts.

### EDF Priority Assignment

All EDF tasks are assigned `uxPriority = configMAX_PRIORITIES - 2`. This ensures:
- EDF tasks preempt all standard tasks in code paths that still check `uxPriority`.
- The timer daemon task (priority `configMAX_PRIORITIES - 1`) remains highest, preserving
  system stability for software timer callbacks.

---

## Admission Control

### Selecting the Feasibility Test

`prvEdfAdmissionCheck` inspects the task set to decide which test to apply:

- **Implicit-deadline** (all D_i == T_i): Liu-Layland utilization bound (U <= 1.0).
  This is both sufficient and necessary for EDF with implicit deadlines.
- **Constrained-deadline** (any D_i < T_i): Processor demand analysis.
  This is the exact (necessary and sufficient) test for constrained-deadline EDF.

The decision is made dynamically: if the new task has D < T, or any existing registered
task has D < T, demand analysis is used. Otherwise the cheaper LL test suffices.

### Liu-Layland Bound (`prvEdfAdmissionTestLL`)

Computes U = sum(C_i / T_i) for all registered tasks plus the candidate, using
**fixed-point arithmetic** with scale factor 10000 (0.01% granularity).

**Rationale**: The RP2350's Cortex-M33 has a single-precision FPU only.
Double-precision floating-point would require software emulation, adding latency
in a critical admission path. Fixed-point with scale 10000 provides sufficient
precision for utilization calculations.

### Processor Demand Analysis (`prvEdfAdmissionTestDemand`)

Implements the exact demand-bound function test:

1. Compute the synchronous busy period upper bound:
   L_max = (sum C_i) / (1 - U), capped at 100,000 ticks.
2. Enumerate all test points: {D_i + k * T_i} for each task i, k = 0, 1, 2, ...
   up to L_max.
3. At each test point L, compute the demand:
   h(0, L) = sum over all tasks j where D_j <= L of: (floor((L - D_j) / T_j) + 1) * C_j
4. If h(0, L) > L at **any** point, the task set is infeasible.

```
For each test point L:
  demand = 0
  for each task j:
    if D_j <= L:
      jobs_j = floor((L - D_j) / T_j) + 1
      demand += jobs_j * C_j
  if demand > L → FAIL
```

**Safety cap**: L_max is capped at 100,000 ticks to prevent pathological iteration
when utilization is very close to 1.0.

---

## Periodic Task Lifecycle

```
 ┌────────────────┐    admission    ┌────────────────┐
 │ xTaskCreateEDF │───────OK───────>│ xEdfReadyList  │
 └────────────────┘                 └───────┬────────┘
                                            │ scheduled
                                    ┌───────▼────────┐
                                    │   EXECUTING     │
                                    │  (busy-wait C)  │
                                    └───────┬────────┘
                                            │ vTaskEdfWaitForNextPeriod()
                                    ┌───────▼────────────────┐
                                    │ xEdfWaitingForPeriodList│
                                    │  (sorted by release t) │
                                    └───────┬────────────────┘
                                            │ tick >= next release
                                            └──────> back to xEdfReadyList
```

When a periodic task calls `vTaskEdfWaitForNextPeriod()`:
1. Clears JOB_ACTIVE and MISSED flags.
2. Computes next release time = current release + T_i.
3. If next release is already in the past (overrun), re-releases immediately.
4. Otherwise, inserts into `xEdfWaitingForPeriodList` sorted by release time.

The tick handler (`xTaskIncrementTick`) scans the waiting list each tick and moves
tasks whose release time has arrived back to the EDF ready list.

---

## Sporadic Task Model

Sporadic tasks use a minimum inter-arrival time (T_min) instead of a fixed period.
For admission control, T_min is treated as the period.

Lifecycle:
1. Created via `xTaskCreateEDFSporadic()` — starts **suspended** (no active job).
2. Released by `xTaskEdfReleaseJobFromISR()` from an ISR (e.g., button press).
3. Release is rejected if:
   - A previous job is still active (JOB_ACTIVE set), or
   - Time since last release < T_min (minimum inter-arrival enforcement).
4. On release: absolute deadline = now + D_i, task inserted into EDF ready list.
5. After execution, `vTaskEdfWaitForNextPeriod()` suspends the task again.

**Rationale**: The sporadic model (not unconstrained aperiodic) was chosen because
minimum inter-arrival enforcement is required for admission control to be meaningful.
Without it, aperiodic tasks could cause unbounded overload.

---

## Deadline Miss Handling

**Design choice: late jobs run to completion.**

When the tick handler detects that an EDF task's absolute deadline has passed
(current tick >= xAbsoluteDeadline) while the job is still active:
1. Sets the MISSED flag.
2. Increments `ulDeadlineMissCount`.
3. Fires the `traceEDF_DEADLINE_MISSED` hook (used by the host monitor GUI).
4. The task **continues executing** — it is not dropped, deprioritized, or restarted.

**Rationale**: EDF optimally minimizes the maximum lateness. Dropping or deprioritizing
a late job would violate this optimality property, which is a central result from the
course. The task continues executing under EDF ordering (its deadline is still the
earliest, so it keeps running until complete or preempted by a task with an even
earlier deadline).

---

## Time-Slicing Among Equal Deadlines

When `configUSE_TIME_SLICING == 1` and multiple EDF tasks share the same absolute
deadline, the tick handler forces a context switch each tick, enabling round-robin
among those tasks. This mirrors FreeRTOS's standard behavior for equal-priority tasks.

When `configUSE_TIME_SLICING == 0`, the first task selected with a given deadline
runs to completion (or until it blocks) without being preempted by equal-deadline peers.

---

## Configuration

All EDF behavior is controlled via `FreeRTOSConfig.h`:

| Macro | Default | Purpose |
|-------|---------|---------|
| `configUSE_EDF` | 0 | Master enable for EDF extension |
| `configEDF_MAX_TASKS` | 8 | Maximum number of EDF tasks (set to 128 for stress test) |
| `configUSE_TIME_SLICING` | 1 | Round-robin among equal-deadline EDF tasks |

When `configUSE_EDF == 0`, the kernel compiles to stock FreeRTOS with no overhead.
