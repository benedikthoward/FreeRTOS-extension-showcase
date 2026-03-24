# Known Bugs — Multiprocessor EDF Support

## Current Bugs

### 1. Port Compatibility Shims: Variadic Macro Issues

**Multiprocessor port shims in the kernel**

The multiprocessor extension uses variadic macros (e.g., `portYIELD_CORE(...)`,
`portGET_CORE_ID(...)`) to abstract per-core operations. Some older or non-GCC
compilers do not support C99 variadic macros correctly, leading to compilation
errors or silent macro expansion failures.

**Severity**: Medium — affects portability to non-standard toolchains.

**Workaround**: Update to a port that fully supports C99 variadic macros, or
manually expand the macros for the target compiler.

### 2. No Runtime Mode Switching (Global/Partitioned)

**Compile-time selection via `GLOBAL_EDF_ENABLE` / `PARTITIONED_EDF_ENABLE`**

The choice between global and partitioned EDF is made at compile time. There is
no API to switch modes at runtime. This means comparing the two modes requires
two separate firmware builds and flash cycles.

**Severity**: Low — acceptable for evaluation and benchmarking, but inconvenient
for interactive demonstrations.

**Workaround**: None. Build and flash separately for each mode.

### 3. WFD Is Greedy, Not Optimal

**`xTaskCreateEDFAutoPartition()` in the kernel**

The Worst-Fit Decreasing heuristic assigns each task to the core with the most
remaining capacity. This greedy strategy is not optimal: it may reject a task set
that is actually partitionable by a different assignment. For example, a set where
the only feasible partition requires placing two high-utilization tasks on the
same core will be rejected by WFD if it spreads them across cores first.

**Severity**: Medium — may cause false rejections for task sets near the
partitioning boundary.

**Workaround**: Use manual partitioning via `xTaskCreateEDFPartitioned()` when
WFD rejects a task set that is known to be feasible.

### 4. Global EDF Admission Bound Is Necessary but Not Sufficient

**Global EDF admission check (U <= m)**

The global admission test checks that total utilization does not exceed the
number of cores (U <= m). This is a necessary condition for global EDF
feasibility but not a sufficient one. Some task sets with U <= m are infeasible
under global EDF due to deadline ordering effects (the Dhall effect for
near-unit-utilization tasks). Such sets will be admitted but may experience
deadline misses at runtime.

**Severity**: Medium — the bound is standard in the literature, but users should
be aware that admission does not guarantee zero deadline misses for all task
parameter combinations.

**Workaround**: Keep individual task utilizations well below 1.0, or use
partitioned EDF where per-core admission is exact for implicit-deadline tasks.

### 5. SRP + Global EDF Not Supported

**Stack Resource Policy integration**

The SRP extension (single-core) is not compatible with global EDF mode. Enabling
both `configUSE_SRP` and `GLOBAL_EDF_ENABLE` results in undefined behavior: the
preemption level and ceiling calculations assume a single ready queue, which does
not hold when tasks migrate between cores.

**Severity**: High — combining these features silently produces incorrect
resource access behavior.

**Workaround**: Use partitioned EDF when SRP is needed. SRP operates correctly
within a single core's partition, since each core maintains its own ready queue
and preemption level ceiling.

### 6. Migration During Task Execution

**`xTaskMpMigrate()` yield timing**

When a running task is migrated via `xTaskMpMigrate()`, the migration sets the
new core affinity and requests a yield on the old core. However, the task may
continue executing on the old core for up to one tick before the yield takes
effect. During this window, the task briefly occupies a slot on both the old
core (still running) and the new core (registered in the affinity map).

**Severity**: Low — the window is at most one tick (1 ms at 1 kHz). The task
does not actually execute on both cores simultaneously; it simply has not yet
yielded on the old core. No data corruption occurs, but trace output may show
a brief overlap in core assignment.

**Workaround**: None currently. The overlap is benign and resolves within one
tick.
