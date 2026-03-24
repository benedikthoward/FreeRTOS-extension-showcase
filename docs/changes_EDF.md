# Changes for EDF Support

This document records all changes and additions made to FreeRTOS to support
Earliest Deadline First (EDF) scheduling with constrained-deadline admission
control, sporadic task support, ISR-safe deadline management, and time-slicing
among equal-deadline tasks.

All EDF code is conditionally compiled behind `#if (configUSE_EDF == 1)`.

---

## Kernel Files Altered

### `FreeRTOS-Kernel/include/FreeRTOS.h`

| Location | Change |
|----------|--------|
| After `configUSE_RECURSIVE_MUTEXES` default | Added default definitions for `configUSE_EDF` (0) and `configEDF_MAX_TASKS` (8) |
| After `traceTASK_PRIORITY_INHERIT` | Added `traceEDF_DEADLINE_MISSED(pxTCB)` trace hook (empty default) |

### `FreeRTOS-Kernel/include/task.h`

Added `errEDF_ADMISSION_FAILED` (−2) error code define and the following EDF public API
declarations (guarded by `#if (configUSE_EDF == 1)`):

| Function | Purpose |
|----------|---------|
| `xTaskCreateEDF()` | Create a periodic EDF-scheduled task with admission control |
| `xTaskCreateEDFSporadic()` | Create a sporadic (event-triggered) EDF task |
| `vTaskEdfWaitForNextPeriod()` | Called by an EDF task to complete its job and wait for the next period |
| `xTaskEdfReleaseJobFromISR()` | Release a sporadic EDF job from an ISR with minimum inter-arrival enforcement |
| `xTaskEdfSetDeadlineFromISR()` | Update an EDF task's absolute deadline from an ISR and re-sort the ready list |
| `xTaskEdfTestAdmission()` | Test whether a task with given (C, D, T) passes admission without creating it |

### `FreeRTOS-Kernel/tasks.c`

This is the core implementation file. All changes are guarded by `#if (configUSE_EDF == 1)`.

#### New Type Definitions and Macros

**EDF flag definitions** (after `tskSUSPENDED_CHAR`):

| Flag | Value | Meaning |
|------|-------|---------|
| `tskEDF_FLAG_IS_EDF` | bit 0 | Task uses EDF scheduling |
| `tskEDF_FLAG_JOB_ACTIVE` | bit 1 | Current job is executing |
| `tskEDF_FLAG_MISSED` | bit 2 | Job has missed its deadline |
| `tskEDF_FLAG_APERIODIC` | bit 3 | Task is sporadic/aperiodic |

**TCB struct extension** (added fields before closing `} tskTCB`):

| Field | Type | Purpose |
|-------|------|---------|
| `xRelativeDeadline` | `TickType_t` | D_i: relative deadline in ticks |
| `xPeriod` | `TickType_t` | T_i: period (or min inter-arrival for sporadic) |
| `xWCET` | `TickType_t` | C_i: worst-case execution time |
| `xAbsoluteDeadline` | `TickType_t` | Current job's absolute deadline (release + D_i) |
| `xReleaseTime` | `TickType_t` | Tick count when current job was released |
| `uxEdfFlags` | `UBaseType_t` | Combination of tskEDF_FLAG_* bits |
| `ulDeadlineMissCount` | `uint32_t` | Running count of deadline misses |

#### New Global State

| Variable | Type | Purpose |
|----------|------|---------|
| `xEdfReadyList` | `List_t` | EDF ready queue sorted by absolute deadline (ascending) |
| `xEdfWaitingForPeriodList` | `List_t` | Tasks waiting for their next period to begin |
| `uxEdfTaskCount` | `UBaseType_t` | Number of registered EDF tasks |
| `pxEdfTaskRegistry` | `TCB_t*[configEDF_MAX_TASKS]` | Registry of all EDF tasks (for admission control iteration) |

#### New Static (Internal) Functions

| Function | Purpose |
|----------|---------|
| `prvEdfAdmissionTestLL()` | Liu-Layland utilization bound test for implicit-deadline task sets (sum C_i/T_i <= 1.0). Uses fixed-point arithmetic with scale factor 10000 for Cortex-M33 (no double FPU). |
| `prvEdfAdmissionTestDemand()` | Processor demand analysis for constrained-deadline task sets (D_i < T_i). Computes L_max = Σ C_i/(1−U), enumerates test points D_i + k·T_i, returns `pdFAIL` if demand > L at any point. |
| `prvEdfAdmissionCheck()` | Wrapper that selects LL test (if all D == T) or demand analysis (if any D < T). Also checks `configEDF_MAX_TASKS` overflow. |
| `prvEdfInitialiseTask()` | Populate EDF TCB fields (period, deadline, WCET, flags), set initial absolute deadline, register in `pxEdfTaskRegistry`. |

#### New Public Functions

| Function | Purpose |
|----------|---------|
| `xTaskCreateEDF()` | Creates periodic EDF task: calls `prvCreateTask` with priority `configMAX_PRIORITIES - 2`, runs admission check, initializes EDF fields, adds to ready list. Returns `errEDF_ADMISSION_FAILED` (−2) if admission is rejected. |
| `xTaskCreateEDFSporadic()` | Creates sporadic EDF task: same as above but sets `tskEDF_FLAG_APERIODIC`, clears `JOB_ACTIVE`, suspends task (waits for ISR release). |
| `vTaskEdfWaitForNextPeriod()` | Enters critical section, clears `JOB_ACTIVE` and `MISSED` flags. For periodic tasks: computes next release time and either re-releases immediately (if overrun) or inserts into `xEdfWaitingForPeriodList`. For sporadic tasks: suspends until next ISR release. |
| `xTaskEdfReleaseJobFromISR()` | ISR-safe sporadic release: rejects if `JOB_ACTIVE` or minimum inter-arrival not met, removes from current list, sets new absolute deadline, inserts into `xEdfReadyList`, sets `xYieldPendings[0]` if preemption needed. |
| `xTaskEdfSetDeadlineFromISR()` | ISR-safe deadline update: modifies `xAbsoluteDeadline`, removes and re-inserts in `xEdfReadyList` for re-sorting, triggers preemption check. |
| `xTaskEdfTestAdmission()` | Thin wrapper around `prvEdfAdmissionCheck()` — tests admission without allocating a TCB. |

#### Changes to Existing Functions

| Function | Change |
|----------|--------|
| `taskSELECT_HIGHEST_PRIORITY_TASK` (macro) | **Replaced** (single-core, EDF build): checks `xEdfReadyList` first — if non-empty, uses `listGET_OWNER_OF_NEXT_ENTRY` (enables round-robin among equal deadlines); otherwise falls back to standard priority list scanning. |
| `prvAddTaskToReadyList` (macro) | **Replaced**: dispatches EDF tasks (flag `tskEDF_FLAG_IS_EDF` set) to sorted insertion via `vListInsert` into `xEdfReadyList`; non-EDF tasks use original priority-based logic. |
| `prvInitialiseNewTask()` | **Extended**: initializes `uxEdfFlags = 0` for all new tasks. |
| `prvInitialiseTaskLists()` | **Extended**: calls `vListInitialise` for `xEdfReadyList` and `xEdfWaitingForPeriodList`, `memset`s `pxEdfTaskRegistry` to zero. |
| `xTaskIncrementTick()` | **Extended** with three new blocks after delayed-task processing: (1) **Period release**: iterates `xEdfWaitingForPeriodList`, releases tasks whose next period has arrived, updates absolute deadline, sets `JOB_ACTIVE`. (2) **Deadline miss detection**: checks if running EDF task exceeded its absolute deadline, sets `MISSED` flag, increments `ulDeadlineMissCount`, fires `traceEDF_DEADLINE_MISSED` — but lets the task continue running (EDF optimally minimizes maximum lateness). (3) **Time-slicing**: if `configUSE_TIME_SLICING == 1` and another EDF task shares the same absolute deadline as the current task, forces `xSwitchRequired = pdTRUE`. |

### `FreeRTOS-Kernel/portable/ThirdParty/GCC/RP2350_ARM_NTZ/non_secure/portmacro.h`

| Change | Reason |
|--------|--------|
| Changed single-core lock macros (`portGET_ISR_LOCK`, `portRELEASE_ISR_LOCK`, `portGET_TASK_LOCK`, `portRELEASE_TASK_LOCK`) from zero-argument `()` to variadic `(...)` | Upstream kernel now passes `xCoreID` argument; variadic form accepts and ignores it in single-core builds. |

---

## Project-Level Files Altered

### `include/FreeRTOSConfig.h`

| Change | Purpose |
|--------|---------|
| `configUSE_EDF` (default 0, overridden to 1 via CMake preset) | Enable/disable EDF extension |
| `configEDF_MAX_TASKS` set to 128 | Support admission control stress test with 100 tasks |
| `configENABLE_FPU`, `configENABLE_MPU`, `configENABLE_TRUSTZONE` | Required by RP2350 Cortex-M33 port |
| `configPRIO_BITS`, `configKERNEL_INTERRUPT_PRIORITY`, `configMAX_SYSCALL_INTERRUPT_PRIORITY` | Required by RP2350 port for interrupt priority masking |
| `configASSERT` macro | Enables kernel assertions (breakpoint + halt); also needed for `vPortValidateInterruptPriority` |
| `traceTASK_SWITCHED_IN` override | Calls `vTraceHookTaskSwitchedIn()` for host GUI monitoring |
| `traceEDF_DEADLINE_MISSED` override | Calls `vTraceHookDeadlineMissed()` for host GUI monitoring |

### `CMakeLists.txt`

| Change | Purpose |
|--------|---------|
| Added `trace_protocol.c` to common interface library sources | Include structured trace output in all demo builds |

### `CMakePresets.json`

| Change | Purpose |
|--------|---------|
| Created configure and build presets for `default`, `edf`, `srp`, `cbs`, `mp`, `all` | Each preset sets appropriate `BUILD_*` and `configUSE_*` CMake cache variables |

---

## New Files Added

### Firmware

| File | Purpose |
|------|---------|
| `common/trace_protocol.h` | Structured event emission API for host GUI (`#TR`, `#TS`, `#JS`, `#JC`, `#DM`, `#AR`, `#IN` events) |
| `common/trace_protocol.c` | Implementation of trace event functions using `printf` over USB serial |
| `src/edf_demo.c` | EDF demo application with UART menu and 5 scenarios: timeline trace, admission control stress test (100 tasks), deadline miss provocation, dynamic task arrival, sporadic task + button ISR |

### Host Monitor

| File | Purpose |
|------|---------|
| `host/pyproject.toml` | Python project configuration (uv-managed, PyQt6 + pyqtgraph + pyserial) |
| `host/monitor.py` | PyQt6 host GUI: live Gantt chart, utilization bars, stats table, console log, CSV export |

---

## Bug Fixes

### `prvEdfAdmissionTestDemand()` — Processor Demand Analysis (CRITICAL)

The original implementation had two bugs:

1. **Logic inversion**: the function returned `pdPASS` upon finding the *first* test point
   where demand ≤ L. Correct behavior requires *all* test points to satisfy demand ≤ L.
2. **Incomplete test points**: only each task's relative deadline D_i was tested. The exact
   EDF feasibility test requires checking at every point D_i + k·T_i (for k = 0, 1, 2, …)
   up to L_max.

**Fix**: Rewrote the function to compute L_max = Σ C_i / (1 − U) with a safety cap at
100000 ticks, enumerate all test points D_i + k·T_i for every registered task, and return
`pdFAIL` if demand exceeds L at *any* point. This now implements exact processor demand
analysis as described in the course text.

### `xTaskCreateEDF()` / `xTaskCreateEDFSporadic()` — Error Code Distinction

Previously, admission failure returned `errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY` (−1),
making it indistinguishable from an out-of-memory condition. Added a new error code
`errEDF_ADMISSION_FAILED` (−2) in `task.h` so callers can differentiate between memory
allocation failure and schedulability rejection.

---

## Design Decisions

| Decision | Rationale |
|----------|-----------|
| Late jobs run to completion (not dropped) | EDF optimally minimizes the maximum lateness — dropping or deprioritizing late jobs would violate this optimality property, which was a central proof in the course |
| EDF tasks assigned `uxPriority = configMAX_PRIORITIES - 2` | Ensures EDF tasks preempt non-EDF tasks in code paths that still check `uxPriority` (timer task at `configMAX_PRIORITIES - 1` remains highest for system stability) |
| Fixed-point arithmetic (scale 10000) for utilization | Cortex-M33 on RP2350 has single-precision FPU only; fixed-point avoids double-precision software emulation overhead |
| Separate `xEdfReadyList` from standard priority lists | Clean separation avoids modifying the core priority-based scheduler; EDF tasks use `vListInsert` (sorted by absolute deadline) while non-EDF tasks use the standard `pxReadyTasksLists[]` array |
| Sporadic model (not unconstrained aperiodic) | Minimum inter-arrival time enforcement enables admission control for sporadic tasks; without it, aperiodic tasks could cause unbounded overload |
| `listGET_OWNER_OF_NEXT_ENTRY` for EDF task selection | Enables round-robin time-slicing among tasks with identical absolute deadlines, matching FreeRTOS's `configUSE_TIME_SLICING` behavior |
