# Future Improvements — CBS Support

## High Priority

### 1. 64-Bit Overflow-Safe Arithmetic

The admission test and budget replenishment compute products like `q_s * T_s`
and `(d_s - r) * Q_s` in 32-bit arithmetic, which overflows when periods exceed
65535 ticks. Implement 64-bit intermediate computations (using `uint64_t`) for
all CBS arithmetic, or use a cross-multiplication comparison technique that
avoids computing the full product (compare `a/b > c/d` as `a*d > c*b` with
overflow-safe helpers).

### 2. Runtime Budget and Period Modification

Add an API (e.g., `vTaskCBSSetParams(TaskHandle_t, TickType_t xNewBudget,
TickType_t xNewPeriod)`) to modify a CBS server's bandwidth at runtime. The
new parameters should take effect at the next replenishment boundary to avoid
mid-period inconsistencies. This requires re-running the admission test to
verify that the updated total utilization remains feasible.

### 3. Multi-Job Queuing

The current CBS implementation assumes at most one active job per server. Add
a per-server job queue so that bursty aperiodic arrivals are buffered and
serviced in FIFO order. Each queued job inherits the server's current deadline;
when a job completes and the queue is non-empty, the next job begins execution
under the remaining budget.

---

## Medium Priority

### 4. CBS + SRP Integration Testing

Create a dedicated test suite that exercises CBS tasks holding SRP resources
at the moment of budget exhaustion. Verify that deadline postponement correctly
interacts with the SRP ceiling — specifically, that a task whose deadline is
pushed does not violate the ceiling protocol and that resource holders are not
preempted in a way that causes unbounded priority inversion.

### 5. Budget Consumption Reporting API

Expose per-server budget statistics through a query API (e.g.,
`xTaskCBSGetStats(TaskHandle_t, CBS_Stats_t *)`), reporting:
- Remaining budget in the current period.
- Number of replenishments since creation.
- Number of deadline postponements (budget exhaustion events).
- Cumulative ticks consumed across all periods.

This enables runtime monitoring dashboards and overload detection.

### 6. GRUB Reclaiming Algorithm

Implement the Greedy Reclamation of Unused Bandwidth (GRUB) algorithm on top
of CBS. When a server completes its job before exhausting its budget, the
leftover bandwidth is redistributed to other active servers proportionally.
This improves system-wide utilization without violating per-server isolation
guarantees.

---

## Low Priority / Optimizations

### 7. Hard CBS Variant

The standard CBS is a soft-reservation mechanism — a server that exhausts its
budget gets a postponed deadline but continues to be schedulable. Implement a
hard CBS variant where budget exhaustion suspends the server until its next
replenishment. This provides strict temporal isolation at the cost of potential
underutilization.

### 8. Hierarchical CBS

Support nested CBS servers where a parent server allocates a fraction of its
bandwidth to child servers. This enables compositional scheduling: subsystems
are developed independently with their own CBS parameters, then composed under
a parent server that caps their aggregate bandwidth. Useful for mixed-criticality
systems where different subsystems have different assurance levels.

### 9. CBS Task Migration for Multiprocessor

Extend CBS to partitioned or global multiprocessor scheduling. In a partitioned
scheme, each core runs its own CBS+EDF scheduler and tasks are statically
assigned. In a global scheme, CBS servers migrate across cores, requiring
cross-core budget tracking and atomic replenishment. Start with partitioned
CBS as it requires minimal changes to the uniprocessor implementation.
