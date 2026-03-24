# Future Improvements — EDF Support

## High Priority

### 1. EDF Task Removal and Registry Cleanup

Currently, deleting an EDF task does not free its admission slot in
`pxEdfTaskRegistry` or decrement `uxEdfTaskCount`. Implement a
`vTaskEdfDelete()` or hook into `vTaskDelete()` to:
- Remove the task from the registry.
- Reclaim the utilization budget so new tasks can be admitted.
- Handle the case where the deleted task holds resources (relevant for SRP).

### 2. Overflow-Safe Absolute Deadline Comparison

Absolute deadlines are stored as `TickType_t` and compared directly. When the tick
counter wraps (after ~49 days at 1 kHz with 32-bit ticks), deadline comparisons
will produce incorrect results. Use the same wrap-safe subtraction technique that
FreeRTOS uses for delayed task lists (`xTickCount - xDeadline` as signed).

### 3. Incremental Demand Analysis

The current demand analysis recomputes from scratch for each admission attempt,
iterating all test points. For large task sets, this is O(n^2 * m) where m is the
number of test points. An incremental approach could cache partial demand sums and
only recompute the contribution of the new candidate task.

---

## Medium Priority

### 4. EDF-Aware `vTaskPrioritySet`

If a user calls `vTaskPrioritySet()` on an EDF task, it modifies `uxPriority` but
has no effect on EDF scheduling (which uses absolute deadlines). This could be
confusing. Either: (a) make `vTaskPrioritySet` a no-op for EDF tasks with a warning,
or (b) reinterpret it as a deadline adjustment.

### 5. Configurable Deadline Miss Policy

The current policy (late jobs run to completion) is hardcoded. Expose the policy as
a configuration option or callback:
- `configEDF_MISS_POLICY_CONTINUE` (current behavior)
- `configEDF_MISS_POLICY_DROP` (cancel the late job)
- `configEDF_MISS_POLICY_CALLBACK` (user-defined handler)

### 6. Reduce Demand Analysis Safety Cap Dependence

Replace the fixed 100,000-tick L_max cap with a dynamic bound based on the actual
hyperperiod (LCM of all periods). This would make the test exact regardless of
task parameters, at the cost of computing the LCM.

---

## Low Priority / Optimizations

### 7. Binary Heap for EDF Ready List

The current sorted linked list gives O(n) insertion. A binary min-heap would give
O(log n) insertion and O(1) minimum extraction, improving scalability for large
task sets (n > 50).

### 8. Batch Period Release in Tick Handler

The tick handler's period release loop iterates the entire waiting list each tick.
Since the list is sorted by release time, the loop already breaks early, but a
timestamp check before entering the loop would avoid the function call overhead
on ticks where no releases are due.

### 9. Utilization Tracking for Runtime Monitoring

Maintain a running sum of actual CPU utilization (execution ticks / elapsed ticks)
per task, updated at each job completion. This would enable runtime overload
detection independent of the WCET estimates used at admission time.

### 10. Power-Aware EDF

When no EDF task is ready, enter a low-power sleep mode (WFI on Cortex-M33) instead
of running the idle task. FreeRTOS supports tickless idle, but it needs to be made
aware of the next EDF release time to set the correct wakeup.
