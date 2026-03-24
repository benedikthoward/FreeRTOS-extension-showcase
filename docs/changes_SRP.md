# Changes for SRP Support

> **Status**: Implemented.

This document records all changes and additions made to FreeRTOS to support
the Stack Resource Policy (SRP) for resource sharing under EDF scheduling.

SRP requires EDF to be enabled (`configUSE_EDF == 1` and `configUSE_SRP == 1`).

---

## Kernel Changes

### `FreeRTOS-Kernel/include/FreeRTOS.h`

| Change | Description |
|--------|-------------|
| `configUSE_SRP` default | Defaults to 0 (disabled) |
| `configSRP_MAX_RESOURCES` default | Defaults to 16 |
| `configSRP_CEILING_STACK_DEPTH` default | Defaults to 16 |
| `configSRP_MAX_STACK_GROUPS` default | Defaults to 32 |
| `traceSRP_CEILING_RAISED` | Empty default trace hook for resource lock |
| `traceSRP_CEILING_LOWERED` | Empty default trace hook for resource unlock |

### `FreeRTOS-Kernel/include/task.h`

| Change | Description |
|--------|-------------|
| `SrpQueueHandle_t` | Forward declaration of `struct QueueDefinition *` to avoid circular dependency with queue.h |
| `uxTaskSrpRegisterResource()` | Register an SRP resource in the kernel registry |
| `xTaskSrpDeclareUsage()` | Declare that a task uses an SRP resource (updates ceiling) |
| `xTaskSrpFinalizeAdmission()` | Compute blocking times and re-run admission with SRP |
| `vTaskSrpResourceTake()` | Internal: push system ceiling on resource acquire |
| `vTaskSrpResourceGive()` | Internal: pop system ceiling on resource release |
| `xTaskCreateEDFSharedStack()` | Create EDF task with stack sharing by preemption level |
| `vTaskSrpGetStackStats()` | Query stack sharing statistics |

### `FreeRTOS-Kernel/include/queue.h`

| Change | Description |
|--------|-------------|
| `queueQUEUE_TYPE_SRP_BINARY_SEMAPHORE` | New queue type constant (6U) for SRP binary semaphores |

### `FreeRTOS-Kernel/include/semphr.h`

| Change | Description |
|--------|-------------|
| `xQueueCreateSrpBinarySemaphore()` | Create an SRP binary semaphore with max CS length |
| `xSemaphoreCreateBinarySRP()` | Macro wrapper for the above |

### `FreeRTOS-Kernel/tasks.c`

| Change | Description |
|--------|-------------|
| TCB fields | `uxPreemptionLevel` (π = UINT32_MAX - D) and `xMaxBlockingTime` (B_i) |
| `SrpResource_t` registry | Static array of resources with ceiling, max CS, holder |
| `SrpCeilingEntry_t` stack | System ceiling stack (push on lock, pop on unlock) |
| `SrpStackGroup_t` array | Stack groups for tasks sharing preemption level |
| `prvSrpSystemCeiling()` | Macro returning current system ceiling (top of stack, or 0) |
| `prvSrpPushCeiling()` | Push a ceiling entry onto the system ceiling stack |
| `prvSrpPopCeiling()` | Pop from the system ceiling stack |
| `uxTaskSrpRegisterResource()` | Allocates a slot in the resource registry |
| `xTaskSrpDeclareUsage()` | Scans registry to find resource, updates ceiling to max(ceiling, π) |
| `vTaskSrpResourceTake()` | Asserts π > system ceiling, records holder, pushes ceiling, emits trace |
| `vTaskSrpResourceGive()` | Asserts caller is holder, pops ceiling, clears holder, emits trace |
| `xTaskSrpFinalizeAdmission()` | Computes B_i for all tasks, re-runs LL/demand test with blocking |
| `xTaskCreateEDFSharedStack()` | Finds/creates stack group by π, delegates to xTaskCreateEDF |
| `vTaskSrpGetStackStats()` | Returns group count, shared memory total, individual memory total |
| `taskSELECT_HIGHEST_PRIORITY_TASK` | SRP-aware variant: walks xEdfReadyList, picks first task with π > system ceiling |
| `prvEdfInitialiseTask()` | Sets uxPreemptionLevel and xMaxBlockingTime on task creation |
| `prvInitialiseTaskLists()` | Zeroes SRP globals (resources, ceiling stack, stack groups) |

### `FreeRTOS-Kernel/queue.c`

| Change | Description |
|--------|-------------|
| `Queue_t.uxSrpResourceIndex` | New field: index into SRP resource registry (sentinel = configSRP_MAX_RESOURCES) |
| Queue init | Initialises uxSrpResourceIndex to sentinel value |
| `xQueueCreateSrpBinarySemaphore()` | Creates queue, registers SRP resource, sets index, gives semaphore |
| `xQueueSemaphoreTake()` SRP path | Fast path: if SRP resource, assert available, decrement, call vTaskSrpResourceTake |
| `xQueueGenericSend()` SRP path | Fast path: if SRP resource, increment, call vTaskSrpResourceGive, yield |

---

## Project-Level Changes

### `include/FreeRTOSConfig.h`

| Change | Description |
|--------|-------------|
| `configSRP_MAX_RESOURCES` | Set to 8 |
| `configSRP_CEILING_STACK_DEPTH` | Set to 16 |
| `configSRP_MAX_STACK_GROUPS` | Set to 32 |
| `traceSRP_CEILING_RAISED` | Override: calls `vTraceHookResourceLock()` |
| `traceSRP_CEILING_LOWERED` | Override: calls `vTraceHookResourceUnlock()` |

### `common/trace_protocol.h` / `trace_protocol.c`

| Change | Description |
|--------|-------------|
| `vTraceHookResourceLock()` | Emits `#RL:tick,task,res_idx,ceiling` |
| `vTraceHookResourceUnlock()` | Emits `#RU:tick,task,res_idx,ceiling` |

### `src/srp_demo.c`

Complete rewrite from stub. Four demo scenarios:

1. **SRP Correctness** — 3 tasks, 2 resources, verifies ceiling protocol and no blocking
2. **Admission with Blocking** — task set feasible without blocking, rejected with SRP blocking
3. **Stack Sharing Study** — 100 tasks across 15 deadline classes, reports memory savings
4. **Nested Resource Locking** — demonstrates LIFO ceiling stack with nested lock/unlock

---

## Bug Fixes

### 1. QueueHandle_t Forward Declaration

**Problem**: task.h declared SRP APIs using `QueueHandle_t`, which is defined in queue.h.
Since task.h is often included before queue.h, this caused "unknown type" compilation errors.

**Fix**: Introduced `SrpQueueHandle_t` as a local forward declaration
(`typedef struct QueueDefinition * SrpQueueHandle_t`) in task.h, used for all SRP API
signatures. Both types are `struct QueueDefinition *` and are assignment-compatible.

### 2. Queue_t Private Access from tasks.c

**Problem**: `xTaskSrpDeclareUsage()` originally cast the semaphore handle to `Queue_t *`
to read `uxSrpResourceIndex`. But `Queue_t` is a private struct defined inside queue.c,
not accessible from tasks.c.

**Fix**: Changed to scan `xSrpResources[]` for the matching semaphore handle instead of
accessing Queue_t internals. This keeps the abstraction boundary clean.

---

## Internal Functions

| Function | File | Description |
|----------|------|-------------|
| `prvSrpPushCeiling` | tasks.c | Push resource ceiling onto system ceiling stack |
| `prvSrpPopCeiling` | tasks.c | Pop from system ceiling stack (asserts LIFO match) |
| `prvSrpSystemCeiling` | tasks.c | Macro: returns top of ceiling stack or 0 |
