# Testing Document — CBS Support

This document describes the testing methodology and all test cases for the CBS
(Constant Bandwidth Server) scheduling extension. Tests are implemented as
interactive scenarios in `src/cbs_demo.c`, selectable via a UART menu at boot.

---

## General Testing Methodology

- **Platform**: Raspberry Pi Pico 2 W (RP2350, dual Cortex-M33), single-core mode.
- **Observation**: Structured trace events (`#CB`, `#CA`, `#TS`, `#DM`, etc.) emitted
  over USB serial, parsed by the PyQt6 host monitor (`host/monitor.py`) for live Gantt
  charts and statistics. Human-readable `printf` output accompanies all scenarios.
- **Busy-wait simulation**: Tasks simulate computation by busy-waiting for their WCET
  using `vBusyWait()`, which loops on `xTaskGetTickCount()`. This provides
  deterministic, tick-accurate execution times.
- **CBS-specific trace events**: `#CB` indicates budget exhaustion (deadline
  postponement), `#CA` indicates CBS job arrival/release.

---

## Test Cases

### Scenario 1: Single CBS + Periodic Tasks

**Purpose**: Verify that a CBS server correctly interleaves with periodic EDF tasks
and that jobs fitting within the server budget complete without budget exhaustion.

**Task set**:

| Task | Type     | Period (T) | Deadline (D) | WCET/Budget (C/Q) | Utilization |
|------|----------|-----------|-------------|-------------------|------------|
| tau1 | periodic | 100 ms    | 100 ms      | 30 ms             | 0.30       |
| tau2 | periodic | 200 ms    | 200 ms      | 40 ms             | 0.20       |
| CBS1 | CBS      | 100 ms    | 100 ms      | 20 ms             | 0.20       |

Total utilization: 0.70.

**CBS workload**: A releaser task sends jobs to CBS1 every 150 ms. Each job requires
15 ms of work (fits within Q=20 ms budget).

**Methodology**:
- CBS1 is created with `xTaskCreateCBS()` (Q=20, T=100).
- A releaser task calls `xTaskCbsReleaseJob()` every 150 ms, indefinitely.
- The CBS task body calls `vBusyWait(15)` then `vTaskCbsWaitForNextJob()`.
- A monitor task reports results after 3 seconds.
- Trace events (`#TS`, `#CA`) are emitted on context switches and CBS job arrivals.

**What to verify**:
- **Trace events**: `#CA` events appear at ~150 ms intervals. No `#CB` (budget
  exhaustion) events should appear, since 15 ms of work fits within the 20 ms budget.
- **Timeline**: The Gantt chart in the host monitor shows CBS1 executing between
  tau1 and tau2 jobs, correctly interleaved by EDF deadline ordering.
- **Budget tab**: CBS1's remaining budget never reaches zero during a job.

**Expected result**: PASS — CBS1 completes all jobs without budget exhaustion. Periodic
tasks tau1 and tau2 meet all deadlines.

---

### Scenario 2: Budget Exhaustion and Deadline Postponement

**Purpose**: Verify that a CBS job exceeding the server budget triggers correct budget
exhaustion handling: the server's deadline is postponed, and the job resumes in a
subsequent replenishment period.

**Task set**:

| Task | Type     | Period (T) | Deadline (D) | WCET/Budget (C/Q) | Utilization |
|------|----------|-----------|-------------|-------------------|------------|
| tau1 | periodic | 200 ms    | 200 ms      | 50 ms             | 0.25       |
| CBS1 | CBS      | 100 ms    | 100 ms      | 30 ms             | 0.30       |

Total utilization: 0.55.

**CBS workload**: A single CBS job requiring 80 ms of work is released at t~10 ms.
This exceeds the budget (Q=30) and must span multiple replenishment periods.

**Methodology**:
- CBS1 is created with `xTaskCreateCBS()` (Q=30, T=100).
- A releaser task sends exactly one job after a 10 ms delay.
- The CBS task body calls `vBusyWait(80)`, consuming far more than one budget.
- The kernel exhausts the budget after 30 ms, emits a `#CB` event, postpones the
  deadline by T=100, and replenishes the budget. This repeats until the 80 ms job
  completes.

**What to verify**:
- **Trace events**: Exactly 2 `#CB` (budget exhaustion) events should appear. The job
  runs in 3 bursts: 30 ms + 30 ms + 20 ms = 80 ms total.
- **Timeline**: The Gantt chart shows CBS1 executing in 3 non-contiguous bursts,
  interleaved with tau1 and idle time. Each burst aligns with a budget replenishment.
- **Budget tab**: CBS1's budget drops to zero twice, then the remaining 20 ms
  completes in the third burst.

**Expected result**: PASS — 2 budget exhaustions observed, CBS1 completes the 80 ms
job across 3 bursts, tau1 meets all deadlines.

---

### Scenario 3: Multiple CBS — Bandwidth Isolation

**Purpose**: Verify that a misbehaving CBS server (workload far exceeding its budget)
cannot starve periodic tasks or other CBS servers. The CBS bandwidth isolation
guarantee must hold.

**Task set**:

| Task | Type     | Period (T) | Deadline (D) | WCET/Budget (C/Q) | Utilization |
|------|----------|-----------|-------------|-------------------|------------|
| tau1 | periodic | 200 ms    | 200 ms      | 40 ms             | 0.20       |
| CBS1 | CBS      | 100 ms    | 100 ms      | 20 ms             | 0.20       |
| CBS2 | CBS      | 150 ms    | 150 ms      | 30 ms             | 0.20       |

Total utilization: 0.60.

**CBS workload**:
- CBS1 receives a single job requiring **100 ms** of work (5x its budget — misbehaving).
- CBS2 receives a single job requiring **25 ms** of work (fits within Q=30 — well-behaved).
- Both jobs are released simultaneously at t~10 ms.

**Methodology**:
- CBS1 is created with `xTaskCreateCBS()` (Q=20, T=100).
- CBS2 is created with `xTaskCreateCBS()` (Q=30, T=150).
- Two releaser tasks each send a single job after 10 ms.
- CBS1's 100 ms job causes repeated budget exhaustions and deadline postponements.
- CBS2's 25 ms job should complete promptly.
- A monitor task reports results after 3 seconds.

**What to verify**:
- **Trace events**: Multiple `#CB` events for CBS1. CBS2 should complete with zero
  or one `#CB` event (25 ms fits within its 30 ms budget).
- **Timeline**: The Gantt chart shows CBS1 executing in many short bursts spread
  over time, while CBS2 and tau1 execute normally.
- **Budget tab**: CBS1's budget is exhausted repeatedly; CBS2's budget is sufficient.
- **Isolation**: tau1 has **0 deadline misses**. The misbehaving CBS1 is contained
  by its bandwidth allocation and cannot interfere with tau1 or CBS2.

**Expected result**: PASS — tau1 has 0 deadline misses, CBS2 completes promptly,
CBS1 eventually finishes but is rate-limited by its bandwidth allocation.

---

### Scenario 4: CBS Admission Control

**Purpose**: Verify that CBS server creation is subject to admission control and that
a CBS server whose bandwidth would push total utilization above 1.0 is rejected.

**Periodic task set (pre-loaded)**:

| Task | Type     | Period (T) | Deadline (D) | WCET (C) | Utilization |
|------|----------|-----------|-------------|---------|------------|
| p1   | periodic | 100 ms    | 100 ms      | 30 ms   | 0.30       |
| p2   | periodic | 200 ms    | 200 ms      | 50 ms   | 0.25       |
| p3   | periodic | 500 ms    | 500 ms      | 150 ms  | 0.30       |

Total periodic utilization: 0.85.

**CBS admission attempts**:

| Attempt | Server | Budget (Q) | Period (T) | U_CBS | Total U | Expected     |
|---------|--------|-----------|-----------|-------|---------|-------------|
| 1       | CBS1   | 20 ms     | 100 ms    | 0.20  | 1.05    | REJECTED     |
| 2       | CBS2   | 10 ms     | 100 ms    | 0.10  | 0.95    | ACCEPTED     |

**Methodology**:
- Three periodic tasks are created with `xTaskCreateEDF()`, filling utilization to
  0.85.
- `xTaskCreateCBS()` is called for CBS1 (Q=20, T=100, U=0.20). This would bring
  total utilization to 1.05, so it must be rejected with `errEDF_ADMISSION_FAILED`.
- `xTaskCreateCBS()` is called for CBS2 (Q=10, T=100, U=0.10). This brings total
  utilization to 0.95, so it should be accepted.
- The scheduler is not started; this is a pure admission-control test.

**What to verify**:
- CBS1 creation returns `errEDF_ADMISSION_FAILED` (total U would exceed 1.0).
- CBS2 creation returns `pdPASS` (total U = 0.95 is feasible).
- No task handles are leaked for the rejected server.

**Expected result**: PASS — CBS1 rejected, CBS2 accepted. Admission control correctly
enforces the utilization bound including CBS bandwidth.

---

## Summary

| Scenario | Tests | Result |
|----------|-------|--------|
| 1. Single CBS + Periodic | CBS interleaving, no exhaustion | PASS |
| 2. Budget Exhaustion | Deadline postponement, multi-burst completion | PASS |
| 3. Multiple CBS Isolation | Misbehaving server contained, 0 deadline misses | PASS |
| 4. Admission Control | CBS U=0.20 rejected, CBS U=0.10 accepted | PASS |
