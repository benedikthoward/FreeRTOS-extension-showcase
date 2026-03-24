# Design Document — CBS Support

This document describes the design choices and implementation approach for adding
Constant Bandwidth Server (CBS) scheduling to FreeRTOS on the RP2350 (Cortex-M33),
building on the EDF extension.

---

## CBS Algorithm

CBS (Abeni & Buttazzo, 1998) provides a mechanism for servicing aperiodic soft
real-time requests alongside hard real-time periodic tasks under EDF without
violating the timing guarantees of periodic tasks. Each server is characterized
by a pair (Q_s, T_s):

- **Q_s** — maximum budget (execution time per server period).
- **T_s** — server period (replenishment interval).
- **U_s = Q_s / T_s** — server bandwidth, reserved on the processor.

The CBS rules are:

1. **Job arrival**: When a new aperiodic job arrives at time r, the server checks
   whether its remaining budget q_s can sustain the current deadline d_s. If the
   budget is too large relative to the remaining time (would cause the server to
   exceed its bandwidth), a new deadline d_s = r + T_s is assigned and the budget
   is replenished to Q_s. Otherwise the current deadline and budget are kept.
2. **Budget tracking**: While the server executes, q_s decrements by one each tick.
3. **Budget exhaustion**: When q_s reaches zero, the deadline is postponed
   (d_s += T_s) and the budget is replenished (q_s = Q_s). The server is re-sorted
   in the EDF ready list under its new, later deadline.
4. **Job completion**: When the aperiodic job finishes, the server becomes idle.
   Budget and deadline are preserved for the next arrival.

**Key property**: CBS guarantees that the server never consumes more than U_s of
the processor over any interval, so periodic tasks are not affected regardless of
the aperiodic workload pattern.

---

## Integration with EDF

CBS tasks **are** EDF tasks. They sit in `xEdfReadyList` and are sorted by their
server deadline d_s, competing for the processor under the same earliest-deadline-first
rule as periodic tasks. No separate scheduling mechanism is required.

Budget tracking is performed in `xTaskIncrementTick`, the same tick handler that
manages periodic releases and deadline miss detection. Each tick, if the current
task is a CBS task with an active job, its `xCbsBudgetRemaining` is decremented.
When the budget reaches zero, the deadline postponement and replenishment happen
inline, followed by a re-sort and a forced context switch.

**No blocking**: A CBS task with an active job is never moved to a waiting list.
When its budget is exhausted it stays in the ready list but with a postponed
(later) deadline, allowing higher-priority work to proceed.

---

## TCB Design

CBS reuses the existing EDF TCB extension fields wherever possible, adding only
one new field:

| TCB Field | CBS Meaning | Notes |
|-----------|-------------|-------|
| `xPeriod` | T_s (server period) | Existing EDF field |
| `xWCET` | Q_s (maximum budget) | Existing EDF field |
| `xAbsoluteDeadline` | d_s (current server deadline) | Existing EDF field, dynamically updated |
| `xReleaseTime` | Tick of last job arrival | Existing EDF field |
| `xCbsBudgetRemaining` | q_s (remaining budget this period) | **New field**, compiled behind `configUSE_CBS` |

Two new flag bits are defined in `uxEdfFlags`:

| Flag | Bit | Purpose |
|------|-----|---------|
| `tskEDF_FLAG_CBS` | 4 | Identifies the task as a CBS server |
| `tskEDF_FLAG_CBS_IDLE` | 5 | Server is idle (no active aperiodic job) |

When `configUSE_CBS == 0`, `tskEDF_FLAG_CBS` is defined as 0 so all CBS-related
conditionals compile away to nothing.

---

## Task Lifecycle

```
 ┌──────────────┐   admission   ┌───────────────────┐
 │xTaskCreateCBS│──────OK──────>│ Suspended (idle)   │
 └──────────────┘               └────────┬──────────┘
                                         │ xTaskCbsReleaseJob[FromISR]()
                                         │ (CBS arrival rule applied)
                                ┌────────▼──────────┐
                                │   xEdfReadyList    │
                                │ (sorted by d_s)    │
                                └────────┬──────────┘
                                         │ scheduled
                                ┌────────▼──────────┐
                                │   EXECUTING        │
                                │ (q_s-- each tick)  │
                                └──┬─────────────┬──┘
                     q_s == 0      │             │  vTaskCbsWaitForNextJob()
                  ┌────────────────┘             └──────────────┐
                  │                                             │
         ┌────────▼───────────┐                     ┌──────────▼──────────┐
         │ Deadline postponed  │                     │ Suspended (idle)    │
         │ d_s += T_s, q_s=Q_s│                     │ budget/deadline     │
         │ re-sort ready list  │                     │ preserved           │
         └────────┬───────────┘                     └─────────────────────┘
                  │
                  └──────> back to xEdfReadyList (new position)
```

### Creation

`xTaskCreateCBS` creates the task, runs EDF admission control with implicit-deadline
parameters (D_s = T_s), populates the TCB, and then **suspends** the task. The server
starts idle with `CBS_IDLE` set and `JOB_ACTIVE` clear. No job runs until explicitly
released.

### Job Arrival

When `xTaskCbsReleaseJob` or `xTaskCbsReleaseJobFromISR` is called, the internal
helper `prvCbsActivateServer` applies the CBS arrival rule:

**Integer-safe arrival condition**: Instead of the textbook floating-point test
`q_s / Q_s >= (d_s - r) / T_s`, the implementation cross-multiplies to avoid
division:

```
q_s * T_s >= (d_s - r) * Q_s
```

All operands are `uint32_t`. If the condition holds (remaining budget is relatively
too large), a fresh deadline and full budget are assigned. If `d_s <= r` (deadline
already in the past), the condition is trivially true and the short-circuit path
fires immediately.

After the arrival rule, `CBS_IDLE` is cleared, `JOB_ACTIVE` is set, and the task
is moved from the suspended list into `xEdfReadyList`.

A release is **rejected** (returns `pdFAIL`) if the server already has an active job
(`CBS_IDLE` not set). This is a simplification: the implementation handles one
aperiodic job at a time per server.

### Execution

While the CBS task runs, `xTaskIncrementTick` decrements `xCbsBudgetRemaining`
each tick. This is the same tick handler that manages periodic releases.

### Budget Exhaustion

When `xCbsBudgetRemaining` reaches zero:

1. `xAbsoluteDeadline += xPeriod` (deadline postponement).
2. `xCbsBudgetRemaining = xWCET` (budget replenishment to Q_s).
3. The task is removed from and re-inserted into `xEdfReadyList` so it is sorted
   under its new, later deadline.
4. A context switch is forced (`xSwitchRequired = pdTRUE`).

### Job Completion

When the aperiodic job finishes, the task calls `vTaskCbsWaitForNextJob()`:

1. `JOB_ACTIVE` is cleared, `CBS_IDLE` is set.
2. The task is removed from `xEdfReadyList` and placed on `xSuspendedTaskList`.
3. Budget and deadline are **preserved** — they carry forward to the next arrival,
   where the arrival rule will decide whether to keep or refresh them.
4. The scheduler yields to pick the next task.

---

## Priority Tie-Breaking

CBS tasks must win deadline ties against periodic tasks. This is because a CBS
server that shares a deadline with a periodic task should execute first to consume
its budget before the deadline passes.

The implementation achieves this in `prvAddEdfTaskToReadyList`:

```c
if( ( pxTCB->uxEdfFlags & tskEDF_FLAG_CBS ) && ( xSortVal_ > 0U ) )
{
    xSortVal_--;
}
```

The sort value used for list insertion is `deadline - 1`. Because `vListInsert`
places new items **after** existing items with the same value, a CBS task with
sort value `d - 1` is placed before a periodic task with sort value `d`.

When `configUSE_CBS == 0`, `tskEDF_FLAG_CBS` is 0 and the branch is optimized away.

---

## Admission Control

CBS servers participate in the existing EDF admission control. A CBS server is
**implicit-deadline** by construction (D_s = T_s), so its bandwidth U_s = Q_s / T_s
is treated identically to a periodic task's utilization C_i / T_i.

`xTaskCreateCBS` calls `prvEdfAdmissionCheck(xBudget, xServerPeriod, xServerPeriod)`,
passing Q_s as the WCET and T_s as both the relative deadline and the period. This
means CBS bandwidth is accounted for in the Liu-Layland utilization bound alongside
periodic tasks, ensuring total utilization does not exceed 1.0.

No separate admission path is needed for CBS.

---

## Deadline Miss Exclusion

CBS tasks are **excluded** from the standard EDF deadline miss detection in
`xTaskIncrementTick`. The tick handler's miss detection condition explicitly
checks `( pxTCB->uxEdfFlags & tskEDF_FLAG_CBS ) == 0U` before firing.

**Rationale**: CBS server deadlines are dynamically assigned and postponed by
design. A server "missing" its current deadline is the normal operating mode when
its budget is exhausted — the deadline is simply pushed forward. Treating this as
a miss would produce spurious alarms and is semantically incorrect.

---

## Trace Events

Two trace events are emitted for CBS activity, following the existing trace protocol
format:

| Event | Format | Fields | Trigger |
|-------|--------|--------|---------|
| Budget exhaustion | `#CB:<tick>,<name>,<budget>,<deadline>` | tick, server name, new budget (Q_s), new deadline | `xCbsBudgetRemaining` reaches 0 |
| Job arrival | `#CA:<tick>,<name>,<budget>,<deadline>` | tick, server name, current budget, current deadline | `prvCbsActivateServer` completes |

Both are emitted via `printf` through the trace protocol helpers
`vTraceHookCbsBudgetExhausted` and `vTraceHookCbsJobArrival`, gated by
`xTraceEnabled`. The host monitor GUI parses these alongside `#EDF` and other
trace lines.

---

## Configuration

CBS is controlled via `FreeRTOSConfig.h` and requires EDF to be enabled:

| Macro | Default | Purpose |
|-------|---------|---------|
| `configUSE_CBS` | 0 | Master enable for CBS extension |
| `configUSE_EDF` | — | **Must be 1** for CBS to compile |

When `configUSE_CBS == 0`, all CBS code is compiled out. The single extra TCB field
(`xCbsBudgetRemaining`) and the two flag bits are removed, and the CBS-related
branches in the tick handler and ready-list insertion macro are eliminated.

---

## API Summary

All CBS APIs are available when `configUSE_EDF == 1 && configUSE_CBS == 1`.

### `xTaskCreateCBS`

```c
BaseType_t xTaskCreateCBS( TaskFunction_t pxTaskCode,
                            const char * const pcName,
                            const configSTACK_DEPTH_TYPE uxStackDepth,
                            void * const pvParameters,
                            TickType_t xBudget,        /* Q_s */
                            TickType_t xServerPeriod,   /* T_s */
                            TaskHandle_t * const pxCreatedTask );
```

Creates a CBS server task. Runs admission control (Q_s / T_s added to total
utilization). The task starts suspended and idle. Returns `pdPASS`,
`errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY`, or `errEDF_ADMISSION_FAILED`.

### `xTaskCbsReleaseJob`

```c
BaseType_t xTaskCbsReleaseJob( TaskHandle_t xTask );
```

Releases a new aperiodic job to the CBS server from task context. Applies the CBS
arrival rule and moves the task to `xEdfReadyList`. Returns `pdFAIL` if the server
already has an active job.

### `xTaskCbsReleaseJobFromISR`

```c
BaseType_t xTaskCbsReleaseJobFromISR( TaskHandle_t xTask,
                                       BaseType_t * pxHigherPriorityTaskWoken );
```

ISR-safe variant of `xTaskCbsReleaseJob`. Sets `*pxHigherPriorityTaskWoken` to
`pdTRUE` if the released CBS task has an earlier deadline than the currently
running task, enabling the caller to request a context switch on ISR exit.

### `vTaskCbsWaitForNextJob`

```c
void vTaskCbsWaitForNextJob( void );
```

Called by the CBS task when its aperiodic job is complete. Marks the server idle,
preserves budget and deadline state, moves the task to the suspended list, and
yields.
