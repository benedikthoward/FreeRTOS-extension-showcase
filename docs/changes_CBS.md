# Changes for CBS Support

This document records all changes and additions made to FreeRTOS to support
Constant Bandwidth Server (CBS) scheduling with admission control, per-tick
budget tracking, deadline postponement on exhaustion, and ISR-safe job release.

All CBS code is conditionally compiled behind `#if (configUSE_CBS == 1)` and
requires `configUSE_EDF == 1` as CBS tasks are scheduled on the EDF ready list.

---

## Kernel Files Altered

### `FreeRTOS-Kernel/include/FreeRTOS.h`

| Location | Change |
|----------|--------|
| After existing trace hook defaults | Added default-empty definitions for `traceCBS_BUDGET_EXHAUSTED(pxTCB)` and `traceCBS_JOB_ARRIVAL(pxTCB)` trace hooks |

### `FreeRTOS-Kernel/include/task.h`

Added the following CBS public API declarations (guarded by `#if (configUSE_CBS == 1)`):

| Function | Purpose |
|----------|---------|
| `xTaskCreateCBS()` | Create a CBS task with admission control; starts suspended |
| `xTaskCbsReleaseJob()` | Release an aperiodic job from task context |
| `xTaskCbsReleaseJobFromISR()` | Release an aperiodic job from ISR context |
| `vTaskCbsWaitForNextJob()` | CBS task goes idle, preserving budget and deadline |

### `FreeRTOS-Kernel/tasks.c`

This is the core implementation file. All changes are guarded by `#if (configUSE_CBS == 1)`.

#### New Flag Definitions

Added after existing EDF flag definitions:

| Flag | Value | Meaning |
|------|-------|---------|
| `tskEDF_FLAG_CBS` | bit 4 | Task is a CBS server |
| `tskEDF_FLAG_CBS_IDLE` | bit 5 | CBS server is idle (no active job) |

When CBS is disabled (`configUSE_CBS == 0`), a fallback `tskEDF_FLAG_CBS = 0` is
defined so that CBS-aware checks in shared code paths compile to no-ops.

#### New TCB Field

| Field | Type | Purpose |
|-------|------|---------|
| `xCbsBudgetRemaining` | `TickType_t` | Remaining execution budget for the current CBS server period |

Added under `#if configUSE_CBS` inside the TCB struct.

#### Modified Macros

| Macro | Change |
|-------|--------|
| `prvAddEdfTaskToReadyList` | CBS tasks get their sort value decremented by 1, giving them tie-breaking priority over periodic EDF tasks with the same absolute deadline |

#### New Static (Internal) Functions

| Function | Purpose |
|----------|---------|
| `prvCbsActivateServer()` | Internal helper that applies the CBS arrival rule: if the remaining budget is insufficient relative to the current deadline, the deadline is postponed and the budget is replenished. Uses integer-safe arithmetic to avoid overflow. |

#### New Public Functions

| Function | Purpose |
|----------|---------|
| `xTaskCreateCBS()` | Creates a CBS task: runs admission control (budget/period added to total utilization), initializes CBS fields, sets `tskEDF_FLAG_CBS` and `tskEDF_FLAG_CBS_IDLE`, starts the task in suspended state. Returns `errEDF_ADMISSION_FAILED` if admission is rejected. |
| `xTaskCbsReleaseJob()` | Releases an aperiodic job from task context: calls `prvCbsActivateServer()` to apply the arrival rule, clears `CBS_IDLE` flag, sets `JOB_ACTIVE`, inserts into `xEdfReadyList`. |
| `xTaskCbsReleaseJobFromISR()` | ISR-safe variant of `xTaskCbsReleaseJob()`: applies the CBS arrival rule, activates the server, triggers preemption check via `xYieldPendings[0]`. |
| `vTaskCbsWaitForNextJob()` | Called by a CBS task when it finishes its current job: sets `CBS_IDLE` flag, clears `JOB_ACTIVE`, preserves remaining budget and current deadline (no replenishment). |

#### Changes to Existing Functions

| Function | Change |
|----------|--------|
| `vTaskEdfWaitForNextPeriod()` | **CBS routing**: when called by a CBS task (flag `tskEDF_FLAG_CBS` set), routes to idle state via `vTaskCbsWaitForNextJob()` instead of performing periodic wait logic. |
| `xTaskIncrementTick()` | **CBS budget tracking**: for the currently running CBS task, decrements `xCbsBudgetRemaining` each tick. When budget reaches zero, postpones the absolute deadline by one server period, replenishes the budget, re-sorts the task in `xEdfReadyList`, and fires `traceCBS_BUDGET_EXHAUSTED`. |
| `xTaskIncrementTick()` (deadline miss detection) | **CBS exclusion**: CBS tasks are excluded from deadline miss detection because CBS deadlines are dynamically assigned (postponed on budget exhaustion) and do not represent hard deadlines. |

---

## Project-Level Files Altered

### `include/FreeRTOSConfig.h`

| Change | Purpose |
|--------|---------|
| `traceCBS_BUDGET_EXHAUSTED` override | Calls `vTraceHookCbsBudgetExhausted()` for host GUI monitoring |
| `traceCBS_JOB_ARRIVAL` override | Calls `vTraceHookCbsJobArrival()` for host GUI monitoring |
| `configUSE_CBS` (already existed as 0) | Enable/disable CBS extension (set to 1 via CMake preset) |

### `common/trace_protocol.h`

| Change | Purpose |
|--------|---------|
| Added `vTraceHookCbsBudgetExhausted()` declaration | Emit `#CB` event when a CBS server exhausts its budget |
| Added `vTraceHookCbsJobArrival()` declaration | Emit `#CA` event when a CBS job is released |

### `common/trace_protocol.c`

| Change | Purpose |
|--------|---------|
| Added `vTraceHookCbsBudgetExhausted()` emitter | Emits `#CB` structured trace event (budget exhaustion) |
| Added `vTraceHookCbsJobArrival()` emitter | Emits `#CA` structured trace event (job arrival) |

---

## New / Rewritten Files

### Firmware

| File | Purpose |
|------|---------|
| `src/cbs_demo.c` | Full rewrite: CBS demo application with 4 scenarios — single CBS server, budget exhaustion demonstration, isolation between CBS and periodic tasks, and admission control rejection |

### Host Monitor

Changes to `host/monitor.py`:

| Change | Purpose |
|--------|---------|
| Added `CbsBudgetEvent` dataclass | Data model for CBS budget/arrival events |
| Added CBS fields in `ScheduleModel` | Storage for CBS event history |
| Added `#CB` / `#CA` parsing in `SerialReaderThread` | Parse CBS trace events from serial stream and emit signals |
| Added CBS exhaustion markers on timeline | Orange triangles drawn on the Gantt chart at budget exhaustion points |
| Added `CBSBudgetWidget` tab | New tab with budget step plot and exhaustion vertical lines |
| Added CBS task tree highlighting | Orange tint on the Type column for CBS tasks in the task tree view |
| Added CBS CSV export | Include CBS budget and arrival events in exported CSV data |

---

## Design Decisions

| Decision | Rationale |
|----------|-----------|
| CBS tasks start suspended | Unlike periodic EDF tasks, CBS servers have no work at creation time; they must be explicitly activated via `xTaskCbsReleaseJob()` or `xTaskCbsReleaseJobFromISR()` |
| CBS tie-breaking (sort value - 1) | When a CBS task and a periodic task share the same absolute deadline, the CBS task runs first. This ensures the bandwidth-limited server makes progress before its deadline is postponed. |
| Integer-safe arrival rule | The CBS arrival rule check (`budget / period <= (deadline - now) / period`) is reformulated as a cross-multiplication to avoid integer division truncation on Cortex-M33 |
| CBS excluded from deadline miss detection | CBS deadlines are soft — they are dynamically postponed when budget is exhausted. Flagging them as misses would generate spurious alerts that do not indicate a real-time violation. |
| Budget preserved on idle | When a CBS task calls `vTaskCbsWaitForNextJob()`, the remaining budget and current deadline are kept intact. The arrival rule at the next job release decides whether replenishment is needed. |
| Admission control reuses EDF utilization framework | CBS servers contribute `budget / period` to the total utilization, checked against the same bound as periodic EDF tasks. This guarantees temporal isolation — a misbehaving CBS server cannot steal bandwidth from periodic tasks. |
