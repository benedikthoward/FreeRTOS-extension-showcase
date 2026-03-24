# Testing Document — SRP Extension

This document describes the testing methodology and all test cases for the Stack
Resource Policy (SRP) extension. Tests are implemented as interactive scenarios
in `src/srp_demo.c`, selectable via a UART menu at boot.

---

## General Testing Methodology

- **Platform**: Raspberry Pi Pico 2 W (RP2350, dual Cortex-M33), single-core mode.
- **Observation**: Structured trace events (`#RL`, `#RU`, `#DM`, etc.) emitted over
  USB serial, parsed by the PyQt6 host monitor (`host/monitor.py`) for live Gantt
  charts and statistics. Human-readable `printf` output accompanies all scenarios.
- **Busy-wait simulation**: Tasks simulate computation by busy-waiting for their WCET
  using `vBusyWait()`, which loops on `xTaskGetTickCount()`. This provides
  deterministic, tick-accurate execution times.
- **SRP resources**: Created with `xSemaphoreCreateBinarySRP()`, which accepts the
  maximum critical section length. Resource ceilings are computed automatically by
  `xTaskSrpDeclareUsage()` and blocking terms are incorporated into admission
  control by `xTaskSrpFinalizeAdmission()`.
- **Reproducibility**: Scenarios using random task parameters use a fixed PRNG seed,
  producing identical results across runs.

---

## Test Cases

### Scenario 1: SRP Correctness (3 Tasks, 2 Resources)

**Purpose**: Verify that the SRP ceiling protocol prevents resource blocking,
that ceiling values rise and fall correctly on lock/unlock, and that all
deadlines are met under a feasible task set.

**Task set**:

| Task | Period (T) | Deadline (D) | WCET (C) | Uses | CS length |
|------|-----------|-------------|---------|------|-----------|
| tau1 | 100 ms | 100 ms | 20 ms | R1 | 5 ms |
| tau2 | 200 ms | 200 ms | 30 ms | R1, R2 | 8 ms, 10 ms |
| tau3 | 500 ms | 500 ms | 40 ms | R2 | 15 ms |

Preemption levels: pi1 > pi2 > pi3 (tau1 has shortest deadline, highest level).
Resource ceilings: C(R1) = pi1 (used by tau1 and tau2), C(R2) = pi2 (used by
tau2 and tau3).

**Methodology**:
- Create two SRP binary semaphores (R1 with max CS = 8 ms, R2 with max CS = 15 ms).
- Create three EDF tasks and declare resource usage via `xTaskSrpDeclareUsage()`.
- Call `xTaskSrpFinalizeAdmission()` to verify feasibility with SRP blocking terms.
- Start the scheduler and let the system run for 3 seconds.
- A monitor task reports results after the observation period.

**What to verify**:
- `xTaskSrpFinalizeAdmission()` returns `pdPASS` (task set is feasible with
  blocking).
- No task ever blocks when attempting to acquire a resource. Under SRP, a task
  either acquires the resource immediately or is prevented from starting its job
  altogether — there is no mid-execution blocking.
- `#RL` (Resource Lock) trace events appear when a task acquires a semaphore,
  showing the system ceiling rising to the resource's ceiling value.
- `#RU` (Resource Unlock) trace events appear when a task releases a semaphore,
  showing the system ceiling falling back to its previous value.
- No `#DM` (Deadline Missed) trace events appear during the 3-second run.
- Console output shows correct LOCK/UNLOCK sequences for each task and all jobs
  complete before their deadlines.

**Result**: PASS — ceiling rises/falls correctly, no blocking, no deadline misses.

---

### Scenario 2: Admission Control with Blocking

**Purpose**: Demonstrate that SRP blocking terms are correctly incorporated into
admission control, causing a task set that is feasible without blocking to be
rejected when blocking pushes effective utilization above 1.0.

**Task set**:

| Task | Period (T) | Deadline (D) | WCET (C) | Utilization | Uses | CS length |
|------|-----------|-------------|---------|------------|------|-----------|
| tau1 | 100 ms | 100 ms | 30 ms | 0.30 | R | 5 ms |
| tau2 | 200 ms | 200 ms | 50 ms | 0.25 | (none) | — |
| tau3 | 500 ms | 500 ms | 150 ms | 0.30 | R | 80 ms |

Without blocking: total U = 0.85 (feasible).

With SRP blocking: C(R) = pi1 (both tau1 and tau3 use R). The blocking term for
tau2 is B2 = 80 ms (the maximum critical section length among tasks with lower
preemption level whose resource ceiling >= pi2). This gives tau2 an effective
utilization of (50 + 80) / 200 = 0.65. Total effective utilization becomes
0.30 + 0.65 + 0.30 = 1.25, which exceeds 1.0.

**Methodology**:
- Create one SRP binary semaphore (R with max CS = 80 ms).
- Create three EDF tasks. Only tau1 and tau3 declare usage of R.
- Call `xTaskSrpFinalizeAdmission()` and check the return value.
- The scheduler is not started — this is a static analysis test.

**What to verify**:
- `xTaskSrpFinalizeAdmission()` returns `pdFAIL`, indicating the task set is
  infeasible when SRP blocking is accounted for.
- Console output confirms the rejection and explains that tau3's 80-tick critical
  section causes the blocking term that pushes effective utilization above 1.0.

**Result**: PASS — admission correctly rejects the infeasible task set.

---

### Scenario 3: Stack Sharing Study (100 Tasks)

**Purpose**: Quantify memory savings from SRP stack sharing, where tasks at the
same preemption level (i.e., same deadline) share a single stack because SRP
guarantees they cannot preempt each other.

**Task set**:
- 100 tasks with implicit deadlines (D = T).
- Deadlines drawn from 15 distinct values: 50, 75, 100, 125, 150, 200, 250,
  300, 400, 500, 600, 750, 1000, 1500, 2000 ms.
- WCET values are small (1-3 ms) to ensure admission passes.
- Stack depths vary from 256 to 768 words per task.
- Fixed PRNG seed (42) for reproducibility.

**Methodology**:
- Create 100 tasks using `xTaskCreateEDFSharedStack()`, which groups tasks by
  preemption level and allocates one stack per group (sized to the maximum depth
  in that group).
- Call `vTaskSrpGetStackStats()` to retrieve: number of stack groups, total
  memory with sharing, and total memory that would be required without sharing.
- Compute and print the percentage savings.
- The scheduler is not started — this is a memory analysis test.

**What to verify**:
- All 100 tasks are created successfully (admission passes for each).
- The number of stack groups is at most 15 (the number of distinct deadline
  values). In practice it may be fewer if some deadline classes have no tasks
  assigned by the PRNG.
- Memory with sharing is significantly less than memory without sharing.
  Typical savings are 50-80%, depending on the PRNG distribution of tasks
  across deadline classes.
- The reported statistics are consistent: shared memory <= individual memory,
  and groups <= 15.

**Result**: PASS — stack groups < 15, significant memory savings reported.

---

### Scenario 4: Nested Resource Locking

**Purpose**: Verify that SRP correctly handles nested resource acquisition, where
a task holds one resource and then acquires a second. The ceiling stack must
maintain LIFO ordering: push R1's ceiling, push R2's ceiling, pop R2's ceiling,
pop R1's ceiling.

**Task set**:

| Task | Period (T) | Deadline (D) | WCET (C) | Uses | Nesting |
|------|-----------|-------------|---------|------|---------|
| nest | 200 ms | 200 ms | 20 ms | R1, R2 | R1 outer, R2 inner |
| other | 300 ms | 300 ms | 10 ms | R2 | flat |

Resource ceilings: C(R1) = pi_nest (only nest uses R1), C(R2) = pi_nest (both
nest and other use R2, nest has higher preemption level).

**Methodology**:
- Create two SRP binary semaphores (R1 with max CS = 10 ms, R2 with max CS = 5 ms).
- Create two EDF tasks. The nest task acquires R1, busy-waits 3 ms, acquires R2
  (nested), busy-waits 5 ms, releases R2, busy-waits 2 ms, releases R1.
- Declare resource usage and finalize admission.
- Start the scheduler and let the system run for 2 seconds.
- A monitor task reports results after the observation period.

**What to verify**:
- `xTaskSrpFinalizeAdmission()` returns `pdPASS`.
- The `#RL`/`#RU` trace events show the correct ceiling stack sequence for each
  job of the nest task:
  1. `#RL` for R1 — system ceiling rises to C(R1).
  2. `#RL` for R2 — system ceiling rises further to C(R2) (stack depth = 2).
  3. `#RU` for R2 — system ceiling drops back to C(R1).
  4. `#RU` for R1 — system ceiling returns to its base value (stack empty).
- LIFO ordering is strictly maintained: R2 is always released before R1.
- Console output shows the LOCK/UNLOCK sequence in the correct nested order
  for every job of the nest task.
- No `#DM` trace events appear during the 2-second run.

**Result**: PASS — ceiling stack operates correctly with nested locks, LIFO
ordering maintained.

---

## Summary

| Scenario | Tests | Result |
|----------|-------|--------|
| 1. SRP Correctness | Ceiling protocol, no blocking, deadlines met | PASS |
| 2. Admission Blocking | Blocking-aware admission rejects infeasible set | PASS |
| 3. Stack Sharing | 100-task memory savings quantification | PASS |
| 4. Nested Locking | Ceiling stack LIFO ordering under nesting | PASS |
