# Design Document — SRP (Stack Resource Policy) Support

This document describes the design choices and implementation approach for adding
the Stack Resource Policy (SRP) to FreeRTOS on the RP2350 (Cortex-M33), building
on top of the EDF scheduling extension.

---

## High-Level Architecture

SRP is implemented as an **opt-in extension** compiled behind `configUSE_SRP == 1`.
It requires `configUSE_EDF == 1`; when disabled, the kernel behaves as stock
FreeRTOS with EDF only. The extension adds:

1. A **preemption level** for each EDF task: pi_i = UINT32_MAX - D_i (shorter
   deadline yields higher preemption level).
2. A **resource registry** (`SrpResource_t` array) tracking ceiling, maximum
   critical section length, and current holder for each SRP resource.
3. A **system ceiling stack** (push on lock, pop on unlock) implementing Baker's
   SRP protocol.
4. **SRP-aware task selection** — tasks can only preempt if their preemption
   level exceeds the current system ceiling.
5. **SRP fast paths** in `queue.c` — binary semaphore take/give bypass the
   standard blocking logic entirely.
6. **Stack sharing** — tasks with the same preemption level share runtime stacks,
   reducing memory usage.
7. **Extended admission control** — blocking times B_i are included in
   feasibility tests.

```
                     ┌──────────────────────────┐
                     │  xSemaphoreCreateBinary  │
                     │    SRP(maxCS)            │
                     └─────────┬────────────────┘
                               │
                     ┌─────────▼────────────────┐
                     │ xTaskSrpDeclareUsage      │
                     │  (task, sem)              │
                     └─────────┬────────────────┘
                               │
                     ┌─────────▼────────────────┐
                     │ xTaskSrpFinalizeAdmission │
                     │  ┌──────────────────────┐ │
                     │  │ Compute B_i for all  │ │
                     │  │ Re-run LL / Demand   │ │
                     │  └──────────────────────┘ │
                     └─────────┬────────────────┘
                               │ pdPASS
                     ┌─────────▼────────────────┐
                     │   xEdfReadyList           │
                     │  (SRP-gated selection)    │
                     └──────────────────────────┘
```

---

## Scheduler Design

### Task Selection (`taskSELECT_HIGHEST_PRIORITY_TASK`)

The macro is extended in SRP builds to enforce the system ceiling constraint.
The new logic:

1. If `uxSysCeil == 0` (no resource locked), use normal EDF ordering — pick the
   head of `xEdfReadyList` (earliest deadline). This fast path avoids any scan
   overhead in the common case.
2. If `uxSysCeil > 0`, walk `xEdfReadyList`; the first task whose preemption
   level pi_i > system ceiling wins.
3. If no EDF task qualifies, fall back to the standard priority-based scan of
   `pxReadyTasksLists[]`.

**Rationale**: The fast path check for `uxSysCeil == 0` ensures that, when no
resource is held, the overhead of SRP is a single integer comparison — zero cost
in the dominant case.

### Preemption Level Assignment

Each EDF task receives a preemption level computed as:

```
pi_i = UINT32_MAX - D_i
```

Tasks with shorter relative deadlines have higher preemption levels. Because EDF
orders by absolute deadline and SRP gates preemption by preemption level, the two
mechanisms compose correctly: a task can only preempt if it has both the earliest
deadline *and* sufficient preemption level relative to the system ceiling.

---

## Resource Access Protocol

### Ceiling Definitions

Each resource R_k has a ceiling defined as the maximum preemption level among all
tasks that use R_k:

```
C(R_k) = max{ pi_i : task i uses R_k }
```

The system ceiling is the maximum of all currently locked resources' ceilings:

```
Pi = max{ C(R_k) : R_k is currently locked }    (or 0 if none locked)
```

### Lock (Take)

On `vTaskSrpResourceTake`:

1. Assert that the calling task's preemption level pi > current system ceiling Pi.
   Under correct SRP usage this assertion always holds — SRP guarantees that no
   task will attempt to lock a resource it cannot immediately acquire.
2. Push C(R_k) onto the system ceiling stack.
3. Update `uxSysCeil` to the new stack top.
4. Set the resource's holder to the calling task.

No blocking ever occurs. This is the fundamental SRP guarantee: either a task can
immediately acquire the resource, or the task would have been prevented from
preempting in the first place.

### Unlock (Give)

On `vTaskSrpResourceGive`:

1. Assert that the caller is the current holder of the resource.
2. Pop the top of the system ceiling stack.
3. Update `uxSysCeil` to the new stack top (or 0 if the stack is empty).
4. Clear the resource's holder.
5. Yield to allow any newly-eligible higher-preemption-level task to run.

### Nested Locking

The ceiling stack supports LIFO nesting. Resources must be released in the
reverse order they were acquired. The stack depth is bounded by
`configSRP_CEILING_STACK_DEPTH`.

**Rationale**: Baker's SRP requires strict LIFO nesting of resource locks. The
stack-based implementation enforces this structurally and detects violations
via assertions in debug builds.

### Fast Paths in queue.c

SRP binary semaphores are tagged with `queueQUEUE_TYPE_SRP_BINARY_SEMAPHORE`
and carry an `uxSrpResourceIndex` field linking back to the resource registry.
When the queue type is SRP:

- `xQueueSemaphoreTake` bypasses the standard blocking path and calls
  `vTaskSrpResourceTake` directly.
- `xQueueSemaphoreGive` bypasses the standard wake-and-reschedule path and calls
  `vTaskSrpResourceGive` directly.

**Rationale**: The standard FreeRTOS queue machinery (block, timeout, wake waiters)
is unnecessary under SRP because resources are never contended. Bypassing it
removes overhead and simplifies the code path.

---

## Admission Control with Blocking

### Blocking Time Computation

When `xTaskSrpFinalizeAdmission` is called, blocking times are computed for each
task. For task i:

```
B_i = max{ maxCS(R_k) : there exists task j with pi_j < pi_i
                         that uses R_k, and C(R_k) >= pi_i }
```

In words: B_i is the longest critical section of any resource that could cause
task i to be blocked under SRP. A resource R_k can block task i only if some
lower-preemption-level task j uses R_k and R_k's ceiling is at least pi_i.

### Liu-Layland Bound with Blocking

For implicit-deadline task sets (all D_i == T_i):

```
sum( (C_i + B_i) / T_i ) <= 1.0
```

Fixed-point arithmetic with scale factor 10000 is used, consistent with the EDF
admission implementation.

### Processor Demand Analysis with Blocking

For constrained-deadline task sets (any D_i < T_i), the demand-bound function
test is extended: the blocking term B_i is added to each task's demand
contribution. The same safety cap of 100,000 ticks on L_max applies.

**Rationale**: The blocking time B_i represents the worst-case delay a task can
experience due to SRP ceiling enforcement. Including it in admission control
ensures that the system remains feasible even in the worst-case blocking scenario.

---

## Stack Sharing

### Theoretical Basis

Under SRP, two tasks with the same preemption level can never both be active
simultaneously. This is a direct consequence of the protocol: if task A is
running and holds no resource, another task B with the same preemption level
cannot preempt (pi_B is not strictly greater than the system ceiling of 0 when
pi_B == pi_A is false — in practice, equal preemption levels block preemption
because the condition is strict inequality).

### Stack Groups

Tasks that share a preemption level are assigned to the same **stack group**.
Within a group, a single stack allocation is made, sized to the maximum stack
depth among all member tasks:

```
group_stack_size = max{ stack_depth_i : task i in group }
```

Tasks in a group are created via `xTaskCreateEDFSharedStack(...)`, which assigns
the shared stack buffer.

### Memory Reporting

`vTaskSrpGetStackStats(...)` reports:

- Number of stack groups
- Per-group: number of member tasks, allocated size, high-water mark
- Total memory saved compared to individual allocations

**Rationale**: Stack sharing is one of the key practical benefits of SRP. On
memory-constrained targets like the RP2350, reducing stack allocations can free
significant RAM for application buffers or additional tasks.

---

## Configuration

All SRP behavior is controlled via `FreeRTOSConfig.h`:

| Macro | Default | Purpose |
|-------|---------|---------|
| `configUSE_SRP` | 0 | Master enable for SRP extension (requires `configUSE_EDF == 1`) |
| `configSRP_MAX_RESOURCES` | 16 | Maximum number of SRP-managed resources |
| `configSRP_CEILING_STACK_DEPTH` | 16 | Maximum nesting depth for resource locks |
| `configSRP_MAX_STACK_GROUPS` | 32 | Maximum number of stack sharing groups |

When `configUSE_SRP == 0`, all SRP code is compiled out and the kernel behaves
as stock FreeRTOS with EDF only.

---

## Trace Protocol

SRP extends the trace protocol with two new event types:

| Tag | Format | Meaning |
|-----|--------|---------|
| `#RL` | `#RL:tick,task,res_idx,ceiling` | Resource locked — task acquired resource |
| `#RU` | `#RU:tick,task,res_idx,ceiling` | Resource unlocked — task released resource |

These events are emitted via the `traceRESOURCE_LOCK` and `traceRESOURCE_UNLOCK`
hooks defined in `FreeRTOS.h` and implemented in `common/trace_protocol.h/.c`.

---

## API Summary

| Function | Purpose |
|----------|---------|
| `xSemaphoreCreateBinarySRP(maxCS)` | Create an SRP binary semaphore with declared maximum critical section length |
| `xTaskSrpDeclareUsage(task, sem)` | Declare that a task uses an SRP resource (must be called before finalization) |
| `xTaskSrpFinalizeAdmission()` | Compute blocking times B_i and re-run admission control for all EDF tasks |
| `xTaskCreateEDFSharedStack(...)` | Create an EDF task using a shared stack group |
| `vTaskSrpGetStackStats(...)` | Query stack sharing statistics (group count, sizes, savings) |
| `vTaskSrpResourceTake` | Internal kernel function — lock an SRP resource (called via semaphore fast path) |
| `vTaskSrpResourceGive` | Internal kernel function — unlock an SRP resource (called via semaphore fast path) |

---

## Files Modified

| File | Changes |
|------|---------|
| `FreeRTOS-Kernel/include/FreeRTOS.h` | Config defaults for SRP macros, trace hook definitions (`traceRESOURCE_LOCK`, `traceRESOURCE_UNLOCK`) |
| `FreeRTOS-Kernel/include/task.h` | SRP API declarations (`xTaskSrpDeclareUsage`, `xTaskSrpFinalizeAdmission`, `xTaskCreateEDFSharedStack`, `vTaskSrpGetStackStats`) |
| `FreeRTOS-Kernel/include/queue.h` | `queueQUEUE_TYPE_SRP_BINARY_SEMAPHORE` constant |
| `FreeRTOS-Kernel/include/semphr.h` | `xSemaphoreCreateBinarySRP` macro |
| `FreeRTOS-Kernel/tasks.c` | TCB fields (preemption level, stack group), resource registry (`SrpResource_t` array), system ceiling stack, SRP-aware task selection, admission control with blocking, stack sharing logic |
| `FreeRTOS-Kernel/queue.c` | `uxSrpResourceIndex` field in queue struct, SRP fast paths in take/give, `xQueueCreateSrpBinarySemaphore` |
| `include/FreeRTOSConfig.h` | Config overrides (`configUSE_SRP`, resource/stack limits), trace macro overrides |
| `common/trace_protocol.h/.c` | `#RL` / `#RU` trace event emitters |
| `src/srp_demo.c` | Four demo scenarios exercising SRP resource access, nesting, stack sharing, and admission |
