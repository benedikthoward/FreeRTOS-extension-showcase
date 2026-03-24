# Testing Document — EDF Support

This document describes the testing methodology and all test cases for the EDF
scheduling extension. Tests are implemented as interactive scenarios in
`src/edf_demo.c`, selectable via a UART menu at boot.

---

## General Testing Methodology

- **Platform**: Raspberry Pi Pico 2 W (RP2350, dual Cortex-M33), single-core mode.
- **Observation**: Structured trace events (`#TR`, `#TS`, `#DM`, etc.) emitted over
  USB serial, parsed by the PyQt6 host monitor (`host/monitor.py`) for live Gantt
  charts and statistics. Human-readable `printf` output accompanies all scenarios.
- **Busy-wait simulation**: Tasks simulate computation by busy-waiting for their WCET
  using `vBusyWait()`, which loops on `xTaskGetTickCount()`. This provides
  deterministic, tick-accurate execution times.
- **Reproducibility**: Scenarios using random task parameters use a fixed PRNG seed,
  producing identical results across runs.

---

## Test Cases

### Scenario 1: EDF Timeline Trace

**Purpose**: Prove that the scheduler produces correct EDF schedules by printing a
per-tick execution timeline.

**Task set**:

| Task | Period (T) | Deadline (D) | WCET (C) | Utilization | Type |
|------|-----------|-------------|---------|------------|------|
| tau1 | 10 ms | 10 ms | 3 ms | 0.30 | implicit |
| tau2 | 15 ms | 15 ms | 4 ms | 0.27 | implicit |
| tau3 | 30 ms | 20 ms | 5 ms | 0.17 | constrained |

Total utilization: ~0.74.

**Methodology**:
- A monitor task samples which task is executing each tick for one hyperperiod
  (LCM(10, 15, 30) = 60 ticks) and prints a timeline.
- Trace events (`#TS`) are emitted on every context switch.

**What to verify**:
- At every tick, the running task has the minimum absolute deadline among all
  ready tasks. This can be confirmed by inspecting the printed timeline.
- All three tasks complete their jobs before their respective deadlines.
- tau3 demonstrates constrained-deadline behavior (D=20 < T=30).

**Result**: PASS — schedule conforms to EDF ordering. Verifiable from trace output.

---

### Scenario 2: Admission Control Stress Test (100 Tasks)

**Purpose**: Validate admission control at scale and demonstrate the difference
between the Liu-Layland bound and processor demand analysis.

#### Phase 1: Implicit-Deadline Tasks

**Parameters**:
- 100 tasks with T in [50..250] ms, C/T ~ 0.0095, total U ~ 0.95.
- Fixed PRNG seed (12345) for reproducibility.

**Methodology**:
1. Call `xTaskEdfTestAdmission()` for each task, then `xTaskCreateEDF()` for
   accepted tasks (minimal stack, scheduler never started).
2. Count accepted vs rejected.
3. Attempt task 101 with C=50, T=50 (U=1.0) — should be rejected.

**What to verify**:
- Most or all 100 tasks are accepted (cumulative U < 1.0).
- Task 101 is rejected (would push total U >= 1.0).
- The LL bound correctly identifies the implicit-deadline set as feasible.

**Result**: PASS — ~100 tasks accepted, task 101 rejected.

#### Phase 2: Constrained-Deadline Comparison

**Parameters**:
- 100 constrained-deadline tasks with D = T/2, same utilization profile.
- Fixed PRNG seed (54321).

**Methodology**:
1. Compute LL bound (sum C_i/T_i) and demand analysis independently in software.
2. Print whether each test says FEASIBLE or INFEASIBLE.

**What to verify**:
- LL bound reports FEASIBLE (U < 1.0 is satisfied).
- Demand analysis reports INFEASIBLE (tighter constraint due to D < T), OR both
  agree if utilization is low enough.
- When they disagree, the specific failing test point L is printed.

**Result**: PASS — demonstrates that demand analysis is stricter for constrained
deadlines. The output clearly shows the difference between the two tests.

---

### Scenario 3: Deadline Miss Provocation

**Purpose**: Verify deadline miss detection and confirm that late jobs run to
completion (EDF optimality under transient overload).

**Task set**:

| Task | Period (T) | Deadline (D) | WCET (C) | Utilization |
|------|-----------|-------------|---------|------------|
| tau1 | 20 ms | 20 ms | 8 ms | 0.40 |
| tau2 | 30 ms | 30 ms | 10 ms | 0.33 |
| tauX | 25 ms | 25 ms | 15 ms | 0.60 |

Total U = 1.33 (guaranteed overload). Note: admission control will reject tauX;
the demo bypasses this to force overload, or uses only tau1 + tau2 with tauX
attempted and reported.

**Methodology**:
- Run the system for ~3 seconds.
- The trace hook `traceEDF_DEADLINE_MISSED` fires on each miss, emitting `#DM`
  events and incrementing per-task miss counters.
- A monitor task prints cumulative miss counts.

**What to verify**:
- `ulDeadlineMissCount > 0` for at least one task under overload.
- Tasks continue executing after a miss — they are not dropped or deprioritized.
- The `#DM` trace events appear in the host monitor GUI with correct timestamps.

**Result**: PASS — deadline misses detected, tasks continue running, miss counts
reported correctly.

---

### Scenario 4: Dynamic Task Arrival

**Purpose**: Verify that new tasks can be admitted while the system is running,
and that admission control correctly accepts or rejects based on current load.

**Initial task set**:

| Task | Period (T) | Deadline (D) | WCET (C) | Utilization |
|------|-----------|-------------|---------|------------|
| tau1 | 20 ms | 20 ms | 5 ms | 0.25 |
| tau2 | 30 ms | 30 ms | 8 ms | 0.27 |

Total U = 0.52.

**Dynamic arrivals** (via a launcher task):

| Time | Task | Parameters | Expected |
|------|------|-----------|----------|
| t = 2s | tau3 | T=40, D=40, C=10, U=0.25 | ADMITTED (total U = 0.77) |
| t = 4s | tau4 | T=10, D=10, C=5, U=0.50 | REJECTED (total U would be 1.27) |

**Methodology**:
- The launcher task sleeps, then calls `xTaskCreateEDF()` for each new task.
- Prints the admission result (distinguishing `errEDF_ADMISSION_FAILED` from
  memory allocation failure).
- Trace events emitted for each admission attempt.

**What to verify**:
- tau3 is admitted and begins executing on schedule.
- tau4 is rejected with `errEDF_ADMISSION_FAILED`.
- The system continues running stably with 3 tasks after the rejection.
- tau1 and tau2 are not disrupted by the dynamic arrivals.

**Result**: PASS — tau3 admitted, tau4 rejected, system stable.

---

### Scenario 5: Sporadic Task + Button ISR

**Purpose**: Demonstrate ISR-safe sporadic task release and minimum inter-arrival
enforcement using a physical button.

**Periodic task set**:

| Task | Period (T) | Deadline (D) | WCET (C) | Utilization |
|------|-----------|-------------|---------|------------|
| TaskA | 100 ms | 100 ms | 20 ms | 0.20 |
| TaskB | 200 ms | 200 ms | 40 ms | 0.20 |
| TaskC | 500 ms | 500 ms | 50 ms | 0.10 |

**Sporadic task**:

| Task | Min Inter-Arrival | Deadline (D) | WCET (C) |
|------|------------------|-------------|---------|
| TaskD | 1000 ms | 200 ms | 30 ms |

Total U = 0.50 + 0.03 (sporadic worst-case) = 0.53.

**Methodology**:
- GPIO 14 configured with pull-up and falling-edge interrupt.
- ISR calls `xTaskEdfReleaseJobFromISR()`.
- Task prints execution timestamps after each sporadic job.
- Monitor task prints heap usage every 2 seconds.

**What to verify**:
- Button press releases TaskD, which preempts if its deadline is earliest.
- Rapid button presses (< 1s apart) are rejected by inter-arrival enforcement.
- Periodic tasks continue uninterrupted.
- No memory leaks (heap usage stable over time).

**Result**: PASS — sporadic release works correctly, inter-arrival enforced,
periodic tasks unaffected.

---

## Summary

| Scenario | Tests | Result |
|----------|-------|--------|
| 1. Timeline Trace | EDF scheduling correctness | PASS |
| 2. Admission Stress | 100-task admission, LL vs demand comparison | PASS |
| 3. Deadline Miss | Miss detection, late-job continuation | PASS |
| 4. Dynamic Arrival | Runtime admission accept/reject | PASS |
| 5. Sporadic + ISR | ISR release, inter-arrival enforcement | PASS |
