# Future Improvements — SRP Extension

## High Priority

### 1. Physical Stack Sharing

Currently `xTaskCreateEDFSharedStack` only tracks shared-stack groups
analytically. Implement true physical stack sharing where tasks with the same
preemption level reuse the same stack memory. This requires careful
context-save coordination on task switches: the outgoing task's context must be
fully saved before the incoming task's frame is laid down in the shared region.

### 2. Per-Task Resource Usage Tracking

Store which specific tasks use each resource in a bitmap or linked list
attached to the resource handle. This enables exact B_i computation (maximum
ceiling among only the resources that lower-priority tasks actually use)
instead of the current conservative approximation that considers all registered
resources.

### 3. Dynamic Resource Deregistration

Allow SRP resources to be unregistered when they are no longer needed via a
`vSRPResourceDeregister()` call. Clean up ceiling associations and recalculate
preemption levels when tasks are deleted. This is especially important in
systems where task sets change at runtime.

---

## Medium Priority

### 4. SRP with Fixed-Priority Tasks

Extend preemption levels to work with FreeRTOS priority-based scheduling, not
just EDF. Map the preemption level pi directly to the FreeRTOS `uxPriority`
value, and maintain the system ceiling stack against that priority space. This
would allow SRP's stack-sharing and blocking-time guarantees in systems that do
not use EDF.

### 5. Priority Ceiling Protocol (PCP) Alternative

Offer PCP as a compile-time alternative to SRP. PCP uses per-mutex ceilings
without the system ceiling stack, raising a task's priority to the ceiling of
each mutex it locks. A configuration flag (e.g. `configUSE_PCP`) would select
between the two protocols, sharing the same resource registration API.

### 6. Configurable Blocking Time Bound

Allow users to specify a maximum acceptable blocking time per task at admission
time. The admission test would reject a task if its computed B_i exceeds the
bound:
- `xTaskCreateEDF( ..., xMaxBlockingTicks )` parameter.
- Returns `errBLOCKING_BOUND_EXCEEDED` on violation.

### 7. ISR-Safe Resource Operations

Current SRP take/give assume task context (they manipulate the system ceiling
stack and may call scheduler functions). Add ISR-safe variants
(`xSRPResourceTakeFromISR` / `xSRPResourceGiveFromISR`) for use in interrupt
handlers, following the same `FromISR` convention used throughout FreeRTOS.

---

## Low Priority / Optimizations

### 8. Resource Contention Profiling

Track per-resource lock/unlock counts and cumulative hold times at runtime.
Expose the data via a query API (`vSRPResourceGetStats`) so developers can
identify contention hot spots and tune resource granularity without external
instrumentation.

### 9. Multiprocessor SRP (MSRP)

Extend SRP for SMP configurations (`configNUMBER_OF_CORES > 1`). MSRP
partitions resources into local (stack-based, per-core SRP) and global
(spin-based, cross-core). Global resource access uses non-preemptive spinning,
preserving the single-blocking-factor property per core.

### 10. Formal Verification of Ceiling Protocol

Add compile-time or runtime checks that resource release ordering is strictly
LIFO. On violation, produce diagnostic output (task name, resource pair, call
site) rather than relying solely on `configASSERT`. A
`configSRP_RELEASE_ORDER_CHECK` flag would enable this at the cost of a small
per-lock bookkeeping overhead.
