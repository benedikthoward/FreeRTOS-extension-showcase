# Known Bugs — CBS Support

## Fixed Bugs

None yet — fresh implementation.

---

## Current Bugs

### 1. Budget Overflow for Large Periods

The admission test and budget replenishment logic compute products such as
`q_s * T_s` and `(d_s - r) * Q_s`, both stored as `uint32_t`. When periods
exceed 65535 ticks, these intermediate products can overflow 32-bit arithmetic,
producing silently incorrect admission decisions or premature budget exhaustion.

**Severity**: High — incorrect admission or budget tracking undermines CBS
guarantees entirely. The overflow is silent, so the failure mode is subtle.

**Workaround**: Keep server periods and budgets reasonable (periods below 65535
ticks). At a 1 kHz tick rate this still allows periods up to ~65 seconds, which
is sufficient for most embedded workloads. See `future_CBS.md` for the planned
64-bit safe arithmetic fix.

### 2. No Runtime CBS Parameter Modification

Once a CBS server is created, its budget (Q_s) and period (T_s) cannot be
changed. There is no API to adjust CBS parameters at runtime. If workload
characteristics change, the only option is to delete and recreate the task.

**Severity**: Medium — prevents adaptive systems from tuning bandwidth
allocation in response to runtime conditions. Acceptable for static workloads
but limiting for dynamic ones.

**Workaround**: None currently. See `future_CBS.md` for planned improvement.

### 3. CBS + SRP Interaction Untested

When both CBS and SRP are enabled, CBS tasks receive preemption levels as
expected, but budget exhaustion does not interact with the SRP ceiling protocol.
A CBS task whose budget is exhausted will have its deadline postponed and
priority lowered, but this transition does not account for any resources the
task may hold under SRP. This could lead to unexpected priority inversions or
ceiling violations.

**Severity**: Medium — only affects configurations where both CBS and SRP are
active simultaneously. The two mechanisms have not been integration-tested.

**Workaround**: Avoid combining CBS and SRP on the same task until integration
testing is complete.

### 4. Single-Job Simplification

The current CBS assignment assumes no job overlap within a single server: each
aperiodic job completes before the next arrives. Real aperiodic workloads may
exhibit bursty arrivals where multiple jobs queue up. In this case, budget
accounting and deadline assignment may not correctly isolate the server's
bandwidth consumption.

**Severity**: Medium — affects correctness under bursty aperiodic loads. For
well-spaced aperiodic events (inter-arrival time >= T_s), the current
implementation is correct.

**Workaround**: Ensure aperiodic job arrivals are spaced at least one server
period apart, or provision extra bandwidth headroom.

### 5. Budget Tracking Resolution

Budget is decremented once per tick in the tick handler. Sub-tick execution
time is not tracked, meaning a CBS task that runs for a fraction of a tick
still consumes one full tick of budget. This can cause slight over-accounting
of budget consumption.

**Severity**: Low — 1-tick granularity is inherent to the tick-based
architecture and matches FreeRTOS's overall timing resolution. The effect is
a small conservatism in budget tracking.

**Workaround**: None — this is a fundamental limitation of tick-based
scheduling.
