# Known Bugs — EDF Support

## Fixed Bugs

### 1. Processor Demand Analysis Logic (CRITICAL — fixed)

**`prvEdfAdmissionTestDemand()` in `FreeRTOS-Kernel/tasks.c`**

The original implementation had two bugs:

1. **Logic inversion**: returned `pdPASS` upon finding the first test point where
   demand <= L. The correct algorithm requires ALL test points to satisfy demand <= L.
2. **Incomplete test points**: only checked at each task's D_i, not at the full set
   of points D_i + k*T_i (k = 0, 1, 2, ...) up to L_max.

**Impact**: Constrained-deadline task sets could be incorrectly admitted as feasible,
potentially leading to unexpected deadline misses at runtime.

**Fix**: Rewrote the function to compute L_max, enumerate all test points, and return
`pdFAIL` if demand exceeds L at any point.

### 2. Admission Failure Error Code (minor — fixed)

**`xTaskCreateEDF()` / `xTaskCreateEDFSporadic()` in `FreeRTOS-Kernel/tasks.c`**

Admission failure returned `errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY` (-1), making it
indistinguishable from an out-of-memory condition.

**Fix**: Added `errEDF_ADMISSION_FAILED` (-2) in `task.h`.

---

## Current Bugs

### 1. Demand Analysis Safety Cap May Be Too Aggressive

The L_max computation in `prvEdfAdmissionTestDemand()` is capped at 100,000 ticks
to prevent pathological iteration when utilization approaches 1.0. For task sets
with very long periods and high utilization, this cap could cause the test to
incorrectly pass (test points beyond the cap are not checked).

**Severity**: Low — unlikely to occur with typical task parameters (periods < 1s
at 1 kHz tick rate). The cap of 100,000 ticks covers 100 seconds, which exceeds
the hyperperiod of most practical task sets.

**Workaround**: Increase the cap if using very long periods.

### 2. Deadline Miss Detection Lag

Deadline miss detection occurs in `xTaskIncrementTick()`, which runs once per tick.
A deadline miss is only detected on the tick *after* the deadline passes. This means
the reported miss time may be up to 1 tick late compared to the actual deadline.

**Severity**: Low — 1-tick granularity is inherent to the tick-based architecture
and matches FreeRTOS's overall timing resolution.

### 3. No Runtime Task Removal

Once an EDF task is admitted and registered in `pxEdfTaskRegistry`, there is no API
to remove it and reclaim its admission slot. Deleting an EDF task via `vTaskDelete()`
frees the TCB but does not decrement `uxEdfTaskCount` or clear the registry entry.
This means the utilization budget is never recovered.

**Severity**: Medium — prevents long-running systems from replacing tasks. Acceptable
for the current demo scenarios where tasks are created once.

**Workaround**: None currently. See `future_EDF.md` for planned improvement.
