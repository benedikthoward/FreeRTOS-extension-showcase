# Design Document — Multiprocessor EDF Support

This document describes the design choices and implementation approach for adding
multiprocessor EDF scheduling (global and partitioned) to FreeRTOS on the RP2350
dual-core Cortex-M33.

---

## Overview

The RP2350 contains two symmetric Cortex-M33 cores. FreeRTOS SMP provides the
infrastructure required for multiprocessor scheduling:

- **`pxCurrentTCBs[]`** — an array of TCB pointers, one per core, tracking which
  task is currently executing on each core.
- **`prvSelectHighestPriorityTask(xCoreID)`** — called on each context switch to
  select the next task for a given core. This is the function extended by the EDF
  block.
- **Inter-Processor Interrupt (IPI) via doorbell** — the RP2350 port implements
  `vYieldCore()` by asserting the hardware doorbell
  (`multicore_doorbell_set_other_core`). When a core needs the other core to
  reschedule (e.g., a higher-priority EDF task becomes ready), it sets
  `xYieldPendings[otherCore] = pdTRUE` and fires the doorbell. The receiving core
  handles the interrupt in `prvDoorbellInterruptHandler`, which triggers
  `portYIELD_FROM_ISR`.
- **Per-core idle tasks** — FreeRTOS SMP creates one idle task per core. Each
  idle task is pinned to its core via `uxCoreAffinityMask`.

The MP extension is compiled behind `configUSE_MP == 1` and requires
`configUSE_EDF == 1`. It adds two mutually exclusive scheduling modes: global EDF
and partitioned EDF.

```
              configUSE_MP == 1
                     │
        ┌────────────┴────────────┐
        │                         │
  GLOBAL_EDF_ENABLE         PARTITIONED_EDF_ENABLE
        │                         │
  Single shared              Per-core
  xEdfReadyList            xEdfReadyLists[2]
  U ≤ 2.0                 per-core U ≤ 1.0
```

---

## Global EDF

When `GLOBAL_EDF_ENABLE == 1`, a single shared `xEdfReadyList` is used. All EDF
tasks are inserted into this list sorted by absolute deadline. There is also a
single shared `xEdfWaitingForPeriodList` for tasks awaiting their next period.

**Task migration is implicit.** Tasks have no core affinity constraint (the
default `uxCoreAffinityMask` allows both cores). When `prvSelectHighestPriorityTask`
runs on either core, it walks the shared EDF list and picks the earliest-deadline
task that is not already running on the other core. A task that ran on core 0 in
one period may run on core 1 in the next.

**Admission control** uses the multiprocessor utilization bound: **U <= m** (where
m = `configNUMBER_OF_CORES` = 2). This is the Liu-Layland bound extended to m
processors. The standard single-core admission path (`prvEdfAdmissionCheck`)
handles this by comparing total utilization against 10000 * m (i.e., 20000 in
fixed-point scale 10000).

**Rationale**: Global EDF is optimal among work-conserving algorithms for
identical multiprocessors. It maximizes schedulability but allows unbounded task
migration, which precludes per-core resource ceiling protocols.

---

## Partitioned EDF

When `PARTITIONED_EDF_ENABLE == 1`, each core has independent EDF data structures:

- `xEdfReadyLists[configNUMBER_OF_CORES]` — per-core ready lists sorted by
  absolute deadline.
- `xEdfWaitingForPeriodLists[configNUMBER_OF_CORES]` — per-core waiting lists
  sorted by next release time.
- `pxEdfTaskRegistries[configNUMBER_OF_CORES][configEDF_MAX_TASKS]` — per-core
  registries for admission control iteration.
- `uxEdfTaskCounts[configNUMBER_OF_CORES]` — per-core task counts.

Tasks are pinned to a core at creation time via `uxCoreAffinityMask` (set to
`1 << xCoreID`). The TCB field `xMpAssignedCore` records the partition assignment.
Helper macros `prvEdfReadyListFor(pxTCB)` and `prvEdfWaitListFor(pxTCB)` resolve
the correct per-core list based on the task's assigned core.

**Admission control** is per-core: each core's utilization must satisfy
**U <= 1.0** (10000 in fixed-point). The function `prvEdfAdmissionCheckPartitioned`
computes the target core's current utilization via `prvMpCoreUtilization` and
checks whether adding the new task exceeds the bound.

**Auto-assignment** uses the Worst-Fit Decreasing heuristic (see below).

**Rationale**: Partitioned EDF eliminates runtime migration overhead and enables
per-core resource ceiling protocols (SRP). The trade-off is reduced schedulability
compared to global EDF — a task set feasible under global EDF may be infeasible
under any partition.

---

## Task Selection

The EDF selection block is inserted at the top of `prvSelectHighestPriorityTask`,
before the standard priority-based `while` loop. The logic:

1. Resolve the EDF list for this core: `xEdfReadyLists[xCoreID]` (partitioned) or
   the single `xEdfReadyList` (global).
2. Walk the list from head (earliest deadline) to tail:
   - If the task's `xTaskRunState == taskTASK_NOT_RUNNING` and its
     `uxCoreAffinityMask` includes this core, select it.
   - If the task is already `pxCurrentTCBs[xCoreID]` (the currently running task
     for this core), keep it selected.
   - Otherwise (task running on another core), skip.
3. If an EDF task is selected, set `xTaskScheduled = pdTRUE` and **`goto
   _edf_task_selected`** to skip the priority-based `while` loop entirely.
4. If no EDF task qualifies, fall through to the standard priority scan.

```c
if( xTaskScheduled != pdFALSE )
{
    goto _edf_task_selected;
}
/* ... priority-based while loop ... */
_edf_task_selected: ;  /* goto target */
```

**Rationale**: The `goto` avoids deeply nested conditionals or flag-based loop
skipping. EDF tasks structurally preempt all priority-based tasks because the EDF
block executes first.

---

## Tick Handler

The tick interrupt fires on core 0 only (`configTICK_CORE == 0`). The EDF tick
logic in `xTaskIncrementTick` handles all cores from this single entry point.

### Period Release

A helper macro `prvEdfReleaseFromWaitList(pxWaitList)` scans a waiting list and
moves tasks whose release time has arrived into the corresponding EDF ready list.

- **Partitioned**: iterates each core's waiting list in a loop:
  ```c
  for( xWlCore_ = 0; xWlCore_ < configNUMBER_OF_CORES; xWlCore_++ )
      prvEdfReleaseFromWaitList( &xEdfWaitingForPeriodLists[ xWlCore_ ] );
  ```
- **Global**: processes the single shared `xEdfWaitingForPeriodList`.

### CBS Budget and Deadline Miss Detection

Both CBS budget decrement and deadline miss detection must iterate **all cores**,
since the tick runs on core 0 only. The tick handler loops over
`pxCurrentTCBs[xTkCore_]` for each core:

```c
for( xTkCore_ = 0; xTkCore_ < configNUMBER_OF_CORES; xTkCore_++ )
{
    TCB_t * pxCoreTCB_ = pxCurrentTCBs[ xTkCore_ ];
    /* CBS budget tracking for pxCoreTCB_ ... */
    /* Deadline miss detection for pxCoreTCB_ ... */
}
```

When a CBS budget exhaustion triggers a deadline postponement, the tick handler
sets `xYieldPendings[xTkCore_] = pdTRUE` for the affected core. The doorbell IPI
mechanism then causes the remote core to reschedule.

---

## Partitioning Heuristic

`prvMpAutoPartition` implements **Worst-Fit Decreasing (WFD)**:

1. Compute the new task's utilization: `ulNewUtil = (C * 10000) / T`.
2. For each core, compute current utilization via `prvMpCoreUtilization`.
3. Select the core with the **lowest** current utilization that can still admit
   the new task (`ulCoreUtil + ulNewUtil <= 10000`).
4. Return the selected core ID, or -1 if no core can accommodate the task.

**Complexity**: O(n * m) where n = `configEDF_MAX_TASKS` and m =
`configNUMBER_OF_CORES` (2). The per-core utilization scan iterates the registry
array.

**Rationale**: WFD is a simple greedy heuristic that balances load across cores.
It is not optimal (no polynomial-time algorithm exists for the bin-packing problem
underlying partitioned scheduling), but it performs well in practice for small
task sets.

---

## Task Migration

### Partitioned Mode

`xTaskMpMigrate` performs a full re-registration:

1. **Admission check** on the target core via `prvEdfAdmissionCheckPartitioned`.
   If the target core cannot accommodate the task, return `pdFAIL`.
2. **Remove from source**: clear the task's slot in
   `pxEdfTaskRegistries[xOldCore]`, decrement `uxEdfTaskCounts[xOldCore]`, and
   remove the task from whichever list it currently occupies (ready or waiting).
3. **Update assignment**: set `xMpAssignedCore = xTargetCore` and
   `uxCoreAffinityMask = 1 << xTargetCore`.
4. **Register on target**: insert into `pxEdfTaskRegistries[xTargetCore]` via
   `prvMpRegisterTask`.
5. **Re-insert into target list**: if the task has an active job
   (`tskEDF_FLAG_JOB_ACTIVE`), insert into the target core's EDF ready list.
   Otherwise, insert into the target core's waiting-for-period list.
6. **Yield old core**: if the task was running (`xTaskRunState >= 0`), set
   `xYieldPendings[xTaskRunState] = pdTRUE` to force a reschedule on the old core.
7. Fire `traceMP_TASK_MIGRATED` hook.

All steps execute inside a single `taskENTER_CRITICAL` / `taskEXIT_CRITICAL`
block to ensure atomicity.

### Global Mode

Migration in global EDF simply updates the affinity mask:

```c
pxTCB->uxCoreAffinityMask = ( UBaseType_t ) 1U << xTargetCore;
pxTCB->xMpAssignedCore = xTargetCore;
```

If the task was running on a different core, a yield is pended for that core.
No list manipulation is required because the shared EDF list is common to both
cores.

---

## SRP Compatibility

SRP (Stack Resource Policy) requires per-core ceiling stacks. This is only
meaningful under partitioned scheduling where each core has a well-defined set of
tasks and resources.

**Global EDF + SRP is a compile-time error:**

```c
#if ( configUSE_SRP == 1 ) && ( GLOBAL_EDF_ENABLE == 1 )
    #error "SRP requires partitioned EDF in multiprocessor mode"
#endif
```

Under partitioned EDF, SRP operates independently on each core using the same
per-core ceiling stack mechanism as the single-core case. Each core's system
ceiling is derived from the resources locked by tasks assigned to that core.

---

## CBS Compatibility

CBS (Constant Bandwidth Server) works naturally in both global and partitioned
modes. Each task carries its own budget (`xCbsBudgetRemaining`) and deadline,
which are per-task state independent of the scheduling mode.

The tick handler's CBS budget decrement loop covers all cores (see Tick Handler
section above), ensuring that a CBS task running on core 1 has its budget
decremented even though the tick interrupt fires on core 0 only.

---

## Configuration

All MP behavior is controlled via `FreeRTOSConfig.h`:

| Macro | Default | Purpose |
|-------|---------|---------|
| `configUSE_MP` | 0 | Master enable for multiprocessor EDF extension |
| `GLOBAL_EDF_ENABLE` | 1 (when MP on) | Enable global EDF scheduling |
| `PARTITIONED_EDF_ENABLE` | 0 (when MP on) | Enable partitioned EDF scheduling |
| `configNUMBER_OF_CORES` | 2 (when MP on) | Number of cores (set automatically) |
| `configRUN_MULTIPLE_PRIORITIES` | 1 (when MP on) | Allow different priorities on different cores |
| `configUSE_CORE_AFFINITY` | 1 (when MP on) | Enable per-task core affinity masks |
| `configTICK_CORE` | 0 | Core responsible for the tick interrupt |

Only one of `GLOBAL_EDF_ENABLE` or `PARTITIONED_EDF_ENABLE` may be set to 1.
If both are 0, `GLOBAL_EDF_ENABLE` is forced to 1 as the default.
If both are 1, a compile-time error is raised.

When `configUSE_MP == 0`, the kernel compiles for single-core operation
(`configNUMBER_OF_CORES == 1`) with no MP overhead.

---

## Port Compatibility

The FreeRTOS SMP kernel expects certain port macros to accept a `xCoreID`
parameter (e.g., `portGET_CRITICAL_NESTING_COUNT(xCoreID)`). The RP2350 port
predates this convention and uses `portGET_CORE_ID()` internally instead.

Variadic macro shims bridge the mismatch:

```c
#define portGET_CRITICAL_NESTING_COUNT( ... )        ( ulCriticalNestings[ portGET_CORE_ID() ] )
#define portSET_CRITICAL_NESTING_COUNT( xCoreIgnored, x ) ( ulCriticalNestings[ portGET_CORE_ID() ] = ( x ) )
#define portINCREMENT_CRITICAL_NESTING_COUNT( ... )  ( ulCriticalNestings[ portGET_CORE_ID() ]++ )
#define portDECREMENT_CRITICAL_NESTING_COUNT( ... )  ( ulCriticalNestings[ portGET_CORE_ID() ]-- )
```

The `...` (variadic) parameter silently absorbs the core ID argument the kernel
passes, while the macro body always uses the hardware `portGET_CORE_ID()` to
index into the per-core nesting count array. This avoids modifying the upstream
port code.

---

## API Summary

| Function | Mode | Purpose |
|----------|------|---------|
| `xTaskCreateEDFPartitioned(pxTaskCode, pcName, uxStackDepth, pvParameters, xPeriod, xRelativeDeadline, xWCET, xCoreID, pxCreatedTask)` | Partitioned | Create an EDF task pinned to a specific core. Performs per-core admission control. |
| `xTaskCreateEDFAutoPartition(pxTaskCode, pcName, uxStackDepth, pvParameters, xPeriod, xRelativeDeadline, xWCET, pxCreatedTask)` | Partitioned (falls back to `xTaskCreateEDF` in global mode) | Create an EDF task with automatic core assignment via WFD heuristic. |
| `xTaskMpMigrate(xTask, xTargetCore)` | Both | Migrate a task to a different core. Partitioned: full re-registration with admission check. Global: affinity mask update. |
| `ulTaskMpGetCoreUtilization(xCoreID)` | Both | Return the total utilization for a core (fixed-point scale 10000). Partitioned: per-core registry sum. Global: total system utilization. |
