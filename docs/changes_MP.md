# Changes for Multiprocessor Support

This document records all changes and additions made to FreeRTOS to support
multiprocessor real-time scheduling (partitioned and global EDF) on the
dual-core RP2350.

All MP code is conditionally compiled behind `#if (configUSE_EDF == 1 && configUSE_MP == 1)`.

---

## Kernel Files Altered

### `FreeRTOS-Kernel/include/FreeRTOS.h`

| Location | Change |
|----------|--------|
| After existing trace hooks | Added `traceMP_TASK_MIGRATED` trace hook (empty default) |

### `FreeRTOS-Kernel/include/task.h`

Added the following MP public API declarations (guarded by `#if configUSE_EDF && configUSE_MP`):

| Function | Purpose |
|----------|---------|
| `xTaskCreateEDFPartitioned()` | Create an EDF task pinned to a specific core |
| `xTaskCreateEDFAutoPartition()` | Create an EDF task with automatic core assignment via Worst-Fit Decreasing |
| `xTaskMpMigrate()` | Migrate a task between cores (partitioned: full re-registration; global: affinity update) |
| `ulTaskMpGetCoreUtilization()` | Query per-core utilization (fixed-point, scale 10000) |

### `FreeRTOS-Kernel/tasks.c`

This is the core implementation file. All MP changes are guarded by `#if (configUSE_EDF == 1 && configUSE_MP == 1)`.

#### TCB Extension

| Field | Type | Purpose |
|-------|------|---------|
| `xMpAssignedCore` | `BaseType_t` | Core assignment: -1 for global, 0 or 1 for partitioned |

#### New Per-Core Data Structures (Partitioned Mode)

| Variable | Type | Purpose |
|----------|------|---------|
| `xEdfReadyLists[2]` | `List_t[2]` | Per-core EDF ready queues sorted by absolute deadline |
| `xEdfWaitingForPeriodLists[2]` | `List_t[2]` | Per-core waiting-for-period lists |
| `uxEdfTaskCounts[2]` | `UBaseType_t[2]` | Number of EDF tasks registered on each core |
| `pxEdfTaskRegistries[2][configEDF_MAX_TASKS]` | `TCB_t*[2][MAX]` | Per-core task registries for admission control |

#### New Helper Macros

| Macro | Purpose |
|-------|---------|
| `prvEdfReadyListFor(pxTCB)` | Route to correct ready list: global `xEdfReadyList` or `xEdfReadyLists[core]` based on `xMpAssignedCore` |
| `prvEdfWaitListFor(pxTCB)` | Route to correct waiting list: global `xEdfWaitingForPeriodList` or `xEdfWaitingForPeriodLists[core]` |

#### New Static (Internal) Functions

| Function | Purpose |
|----------|---------|
| `prvMpCoreUtilization()` | Compute per-core utilization from the core's task registry (fixed-point, scale 10000) |
| `prvMpAutoPartition()` | Worst-Fit Decreasing heuristic: assigns a new task to the core with the lowest current utilization |
| `prvEdfAdmissionCheckPartitioned()` | Per-core admission test: U <= 1.0 (utilization bound checked against the target core only) |
| `prvMpRegisterTask()` | Register a task in the per-core task registry for its assigned core |

#### New Public Functions

| Function | Purpose |
|----------|---------|
| `xTaskCreateEDFPartitioned()` | Creates an EDF task pinned to a specific core: runs per-core admission check, initializes EDF fields, sets `xMpAssignedCore`, registers in per-core registry, adds to per-core ready list |
| `xTaskCreateEDFAutoPartition()` | Creates an EDF task with automatic assignment: calls `prvMpAutoPartition()` for WFD placement, then delegates to `xTaskCreateEDFPartitioned()` |
| `xTaskMpMigrate()` | Migrates a task between cores. Partitioned mode: removes from source registry, re-registers on destination, updates `xMpAssignedCore`, moves between per-core lists. Global mode: updates `uxCoreAffinityMask`. Fires `traceMP_TASK_MIGRATED`. |
| `ulTaskMpGetCoreUtilization()` | Public wrapper around `prvMpCoreUtilization()` for querying per-core utilization |

#### Changes to Existing Functions

| Function | Change |
|----------|--------|
| `prvEdfInitialiseTask()` | **Extended**: initializes `xMpAssignedCore = -1` and `uxCoreAffinityMask = tskNO_AFFINITY` for all new EDF tasks |
| `prvAddEdfTaskToReadyList` | **Modified**: uses `prvEdfReadyListFor(pxTCB)` to route insertion to the correct per-core or global ready list |
| `prvSelectHighestPriorityTask()` | **Modified**: added EDF selection block at top that walks `xEdfReadyList` (global) or `xEdfReadyLists[coreID]` (partitioned) before the priority-based loop. Uses `goto` to skip the priority loop when an EDF task is found. |
| `xTaskIncrementTick()` | **Extended**: EDF period release now handles both global (single list) and partitioned (per-core lists). CBS budget tracking and deadline miss detection iterate ALL cores in SMP mode. |
| `prvEdfAdmissionTestLL()` | **Modified**: utilization bound changed from U <= 1.0 (10000) to U <= m (configNUMBER_OF_CORES * 10000) for global multiprocessor schedulability |
| `vTaskEdfWaitForNextPeriod()` | **Modified**: uses `prvEdfWaitListFor(pxTCB)` for waiting list insertion instead of the single global list |
| Initialization (`prvInitialiseTaskLists`) | **Extended**: initializes per-core lists (`xEdfReadyLists`, `xEdfWaitingForPeriodLists`) and per-core registries for partitioned mode |

### `FreeRTOS-Kernel/portable/.../portmacro.h`

| Change | Reason |
|--------|--------|
| Modified 8 port macros to accept variadic arguments: `portGET_CRITICAL_NESTING_COUNT`, `portSET_CRITICAL_NESTING_COUNT`, `portINCREMENT_CRITICAL_NESTING_COUNT`, `portDECREMENT_CRITICAL_NESTING_COUNT`, `portGET_ISR_LOCK`, `portRELEASE_ISR_LOCK`, `portGET_TASK_LOCK`, `portRELEASE_TASK_LOCK` | Upstream SMP kernel passes additional arguments (e.g., `xCoreID`); variadic form `(...)` accepts and ignores them in single-core port builds, maintaining compatibility |

---

## Project-Level Files Altered

### `include/FreeRTOSConfig.h`

| Change | Purpose |
|--------|---------|
| Conditional SMP block (when `configUSE_MP == 1`): `configNUMBER_OF_CORES = 2`, `configRUN_MULTIPLE_PRIORITIES = 1`, `configUSE_CORE_AFFINITY = 1`, `configTICK_CORE = 0` | Enable FreeRTOS SMP kernel for dual-core RP2350 |
| `GLOBAL_EDF_ENABLE` / `PARTITIONED_EDF_ENABLE` flags | Select scheduling mode with mutual exclusion (compile-time error if both set); defaults to global when neither is defined |
| SRP + global EDF compile-time error | Stack Resource Policy is incompatible with global EDF; enforced via `#error` |
| `configUSE_PASSIVE_IDLE_HOOK = 0` | Required by FreeRTOS SMP kernel |
| SMP-aware `traceTASK_SWITCHED_IN` override | Calls `vTraceHookTaskSwitchedInCore()` with core ID for host monitoring |
| `traceMP_TASK_MIGRATED` override | Calls `vTraceHookMigration()` for host monitoring |

### `CMakeLists.txt`

| Change | Purpose |
|--------|---------|
| Forward `GLOBAL_EDF_ENABLE` and `PARTITIONED_EDF_ENABLE` as compile definitions | Allow CMake presets to control scheduling mode at build time |

### `CMakePresets.json`

| Change | Purpose |
|--------|---------|
| Added `mp-partitioned` configure and build presets | Build with partitioned EDF mode enabled |
| Renamed existing `mp` preset to default to global EDF | `mp` preset now implies global multiprocessor scheduling |
| Downgraded preset version from 6 to 3 | Avoids CMake attempting to scan subproject (FreeRTOS-Kernel) for preset files |

---

## New / Rewritten Files

### Firmware

| File | Purpose |
|------|---------|
| `src/mp_demo.c` | Full rewrite: 5 scenarios — global EDF, partitioned WFD, manual partition, migration, admission comparison (global U <= m vs partitioned U <= 1.0) |

### Trace Protocol

| File | Change |
|------|--------|
| `common/trace_protocol.h` | Added `vTraceHookTaskSwitchedInCore()` declaration (3-field `#TS` with core ID) and `vTraceHookMigration()` declaration (`#MG` event) |
| `common/trace_protocol.c` | Implementation of `vTraceHookTaskSwitchedInCore()` emitting `#TS:<tick>,<task_name>,<core_id>` and `vTraceHookMigration()` emitting `#MG:<tick>,<task_name>,<from_core>,<to_core>` |

### Host Monitor

| File | Change |
|------|--------|
| `host/monitor.py` | `SwitchEvent` dataclass extended with `core_id` field; new `MigrationEvent` dataclass added; `#TS` parser updated to handle optional 3rd field (core ID); `#MG` parser added; migration signal and handler wired into GUI |

---

## Design Decisions

| Decision | Rationale |
|----------|-----------|
| Per-core data structures for partitioned mode (separate ready lists, wait lists, registries) | Clean separation between cores avoids lock contention; each core operates on its own EDF queue independently |
| `xMpAssignedCore = -1` for global tasks | Distinguishes global tasks (migratable across all cores) from partitioned tasks (pinned to core 0 or 1) using a single field |
| Worst-Fit Decreasing for auto-partitioning | WFD is a well-known bin-packing heuristic that balances load across cores; assigns each new task to the core with the lowest current utilization |
| Global admission bound U <= m instead of U <= 1.0 | Global EDF on m identical processors is feasible if total utilization does not exceed m; this is the Dhall-Liu bound relaxation for global scheduling |
| Partitioned admission bound U <= 1.0 per core | Each core runs independent uniprocessor EDF; the standard Liu-Layland bound applies per partition |
| Variadic port macros for SMP compatibility | The FreeRTOS SMP kernel passes core ID arguments to lock macros; variadic form avoids modifying every port while maintaining single-core compatibility |
| Preset version downgrade (6 to 3) | CMake version 6 presets require all referenced subprojects to also have preset files; version 3 avoids this constraint for the FreeRTOS-Kernel submodule |
| Mutual exclusion of global and partitioned modes | Mixing partitioned and global scheduling within a single build would require complex hybrid data structures; compile-time selection keeps the implementation clean |
| SRP + global EDF compile-time error | Stack Resource Policy assumes non-preemptive sections on a single processor; global EDF with migration violates SRP's assumptions |
