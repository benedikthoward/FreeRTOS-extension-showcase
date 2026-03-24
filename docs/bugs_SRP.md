# Known Bugs — SRP Support

## Fixed Bugs

### 1. QueueHandle_t Forward Declaration (CRITICAL — fixed)

**`task.h` SRP API declarations**

`task.h` declared SRP APIs using `QueueHandle_t`, which is defined in `queue.h`.
Since `task.h` is included before `queue.h` in many translation units, this caused
compilation errors wherever SRP functions were referenced.

**Impact**: Build failure in any file that included `task.h` without a prior
`#include "queue.h"`.

**Fix**: Introduced `SrpQueueHandle_t` as a local forward declaration in `task.h`,
removing the dependency on `queue.h` being included first.

### 2. Queue_t Access from tasks.c (minor — fixed)

**`xTaskSrpDeclareUsage()` in `FreeRTOS-Kernel/tasks.c`**

The original implementation cast the semaphore handle to `Queue_t*` to access
`uxSrpResourceIndex`, but `Queue_t` is private to `queue.c` and not visible from
`tasks.c`.

**Fix**: Replaced the direct struct access with a scan of `xSrpResources[]` to find
the matching handle by comparison, avoiding any dependency on `Queue_t` internals.

---

## Current Bugs

### 1. Stack Sharing Is Analytical Only

`xTaskCreateEDFSharedStack()` tracks stack groups and reports savings, but each task
still receives its own physical stack allocation. True physical stack sharing would
require careful context-save/restore coordination that is beyond current scope.

**Severity**: Low — the analytical tracking is useful for sizing estimates, and the
system remains correct since every task has a dedicated stack. Physical sharing is a
future enhancement.

**Workaround**: Use the reported savings data to manually reduce individual stack
sizes where analysis confirms it is safe.

### 2. Resource Usage Not Tracked Per-Task

The SRP resource registry tracks ceilings but does not store which specific tasks use
each resource. The blocking time computation (B_i) in `xTaskSrpFinalizeAdmission()`
scans all tasks and resources, which is O(N*M) but does not precisely track per-task
resource sets. This is conservative but may over-estimate blocking.

**Severity**: Low — over-estimation makes admission stricter than necessary, which is
safe. It may reject task sets that would be feasible under a tighter analysis.

**Workaround**: None currently. Keep the number of resources and tasks reasonable to
limit the impact of the conservative estimate.

### 3. No Dynamic Resource Deregistration

Once an SRP resource is registered, it cannot be removed. The resource registry only
grows. If tasks are deleted, their resource associations are not cleaned up.

**Severity**: Medium — prevents long-running systems from reclaiming resource slots.
Acceptable for the current demo scenarios where resources are registered once at
startup.

**Workaround**: None currently. Pre-allocate only the resources that are needed for
the lifetime of the system.

### 4. Ceiling Stack Depth Is Static

`configSRP_CEILING_STACK_DEPTH` is fixed at compile time. If nesting exceeds this
depth, a `configASSERT` fires with no graceful degradation or recovery path.

**Severity**: Low — the nesting depth equals the maximum number of concurrently held
SRP resources, which is small in practice. Setting the config value to the total
number of registered resources guarantees safety.

**Workaround**: Set `configSRP_CEILING_STACK_DEPTH` to at least the maximum number of
SRP resources that any single task can hold simultaneously.

### 5. SRP Requires EDF

The preemption level scheme (pi_i = UINT32_MAX - D_i) is tied to EDF deadlines. SRP
cannot be used with fixed-priority tasks in this implementation.

**Severity**: Medium — limits SRP to EDF-scheduled task sets only. A fixed-priority
mapping would require a different preemption level assignment.

**Workaround**: None currently. All tasks using SRP resources must be created with
`xTaskCreateEDF()` or `xTaskCreateEDFSporadic()`.
