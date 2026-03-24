"""
FreeRTOS Extension Monitor — PyQt6 host GUI.

Connects to a Pico 2 W over USB serial, parses structured trace events
(#TR, #TS, #JS, #JC, #DM, #AR, #IN, #RL, #RU, #CB, #CA), and renders
live visualizations:
  Tab 1 — Gantt-style timeline (with SRP/CBS overlays)
  Tab 2 — Per-task utilization bars
  Tab 3 — Stats table
  Tab 4 — Raw console log
  Tab 5 — SRP Resources (system ceiling + resource lock timeline)
  Tab 6 — CBS Budget (per-server budget over time)

Usage:
    uv run monitor.py [--port /dev/tty.usbmodem*] [--baud 115200]
"""

from __future__ import annotations

import argparse
import csv
import os
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

import pyqtgraph as pg
import serial
import serial.tools.list_ports
from PyQt6.QtCore import QThread, pyqtSignal, Qt, QTimer
from PyQt6.QtGui import QColor, QFont, QAction
from PyQt6.QtWidgets import (
    QApplication,
    QComboBox,
    QFileDialog,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSlider,
    QSplitter,
    QStackedWidget,
    QStatusBar,
    QTableWidget,
    QTableWidgetItem,
    QTabWidget,
    QToolBar,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

# ── Colour palette for tasks ────────────────────────────────────────────

TASK_COLOURS = [
    "#e6194b", "#3cb44b", "#4363d8", "#f58231", "#911eb4",
    "#42d4f4", "#f032e6", "#bfef45", "#fabed4", "#469990",
    "#dcbeff", "#9A6324", "#800000", "#aaffc3", "#808000",
    "#000075", "#a9a9a9",
]

# System / infrastructure tasks — tracked for utilization math but hidden
# from the timeline Gantt chart, sidebar tree, and stats table.
SYSTEM_TASK_PREFIXES = ("IDLE", "Tmr Svc", "USB", "Menu", "TLMon", "DMon",
                        "Launch")

# Colours for resource overlays (per resource index)
RESOURCE_COLOURS = [
    "#ffff00", "#00ffff", "#ff00ff", "#ff8800", "#88ff00",
    "#0088ff", "#ff0088", "#00ff88",
]

# Scenario metadata: (preset_name, demo_binary, short_description, detailed_help)
SCENARIOS = {
    "edf-s1": ("edf-s1", "edf_demo", "EDF S1: Timeline Trace",
        "3 EDF tasks (500/750/1500ms periods). Watch the Timeline tab for a Gantt chart "
        "showing earliest-deadline-first scheduling. Shorter-deadline tasks preempt longer ones."),
    "edf-s2": ("edf-s2", "edf_demo", "EDF S2: Admission Control",
        "100 implicit-deadline tasks tested against LL bound, then 100 constrained-deadline "
        "tasks comparing LL bound vs processor demand analysis. Check Console for results."),
    "edf-s3": ("edf-s3", "edf_demo", "EDF S3: Deadline Miss",
        "Overloaded task set causes deadline misses. Watch Timeline for red diamond markers "
        "and check Stats tab for miss counts."),
    "edf-s4": ("edf-s4", "edf_demo", "EDF S4: Dynamic Arrival",
        "Tasks arrive dynamically at runtime with admission control. Console shows accept/reject."),
    "edf-s5": ("edf-s5", "edf_demo", "EDF S5: Sporadic + ISR",
        "Button press triggers sporadic task release via ISR. Press GPIO14 button to trigger."),
    "srp-s1": ("srp-s1", "srp_demo", "SRP S1: Correctness",
        "3 tasks sharing 2 SRP resources. SRP Resources tab shows ceiling evolution. "
        "Timeline shows critical section overlays (yellow borders)."),
    "srp-s2": ("srp-s2", "srp_demo", "SRP S2: Admission + Blocking",
        "Admission control including worst-case blocking times from SRP. Console output."),
    "srp-s3": ("srp-s3", "srp_demo", "SRP S3: Stack Sharing",
        "100 tasks quantitative study: stack sharing vs individual stacks. Console output."),
    "srp-s4": ("srp-s4", "srp_demo", "SRP S4: Nested Locking",
        "Tasks lock multiple resources in nested order. SRP Resources tab shows two-step "
        "ceiling rise/fall."),
    "cbs-s1": ("cbs-s1", "cbs_demo", "CBS S1: Single CBS + Periodic",
        "One CBS server alongside periodic tasks. CBS Budget tab shows budget sawtooth. "
        "CBS wins tie-breaks over periodic tasks."),
    "cbs-s2": ("cbs-s2", "cbs_demo", "CBS S2: Budget Exhaustion",
        "CBS server exhausts budget, deadline postponed. Orange triangle markers on Timeline."),
    "cbs-s3": ("cbs-s3", "cbs_demo", "CBS S3: Bandwidth Isolation",
        "Multiple CBS servers demonstrate isolation — overloaded server doesn't affect others."),
    "cbs-s4": ("cbs-s4", "cbs_demo", "CBS S4: Admission Control",
        "CBS admission: total bandwidth (periodic U + server bandwidth) must stay ≤ 1.0."),
    "mp-s1": ("mp-s1", "mp_demo", "MP S1: Global EDF",
        "4 tasks on 2 cores with global EDF. Tasks migrate between cores. "
        "Timeline shows tasks appearing on both cores."),
    "mp-s2": ("mp-s2", "mp_demo", "MP S2: Partitioned WFD",
        "Worst-Fit Decreasing auto-assigns tasks to cores. Console shows assignments."),
    "mp-s3": ("mp-s3", "mp_demo", "MP S3: Manual Partition",
        "Tasks manually pinned to specific cores. Each core runs independent EDF."),
    "mp-s4": ("mp-s4", "mp_demo", "MP S4: Migration",
        "Task migrates from one core to another at runtime. Watch for #MG events."),
    "mp-s5": ("mp-s5", "mp_demo", "MP S5: Admission Comparison",
        "Global (U ≤ 2.0) vs partitioned (per-core U ≤ 1.0) admission comparison."),
}

# Map from (#ID task_label, scenario_number) to preset key
SCENARIO_ID_MAP = {}
for key, (preset, demo, label, desc) in SCENARIOS.items():
    parts = key.split("-s")
    if len(parts) == 2:
        SCENARIO_ID_MAP[(parts[0], int(parts[1]))] = key


# ── Data model ──────────────────────────────────────────────────────────

@dataclass
class TaskInfo:
    name: str
    period: int = 0
    deadline: int = 0
    wcet: int = 0
    task_type: str = "periodic"
    colour: str = "#888888"


@dataclass
class TaskStats:
    jobs_completed: int = 0
    deadline_misses: int = 0
    execution_ticks: int = 0
    last_job_start: int | None = None


@dataclass
class SwitchEvent:
    tick: int
    task_name: str
    core_id: int = 0  # core ID from SMP trace (0 for single-core)


@dataclass
class MigrationEvent:
    tick: int
    task_name: str
    from_core: int
    to_core: int


@dataclass
class MissEvent:
    tick: int
    task_name: str
    abs_deadline: int
    miss_count: int


@dataclass
class ResourceLockEvent:
    tick: int
    task_name: str
    res_idx: int
    ceiling: int


@dataclass
class ResourceUnlockEvent:
    tick: int
    task_name: str
    res_idx: int
    ceiling: int


@dataclass
class CbsBudgetEvent:
    tick: int
    task_name: str
    new_budget: int
    new_deadline: int


class ScheduleModel:
    """Central data store for all trace events."""

    def __init__(self) -> None:
        self.tasks: dict[str, TaskInfo] = {}
        self.stats: dict[str, TaskStats] = {}
        self.switches: list[SwitchEvent] = []
        self.misses: list[MissEvent] = []
        self.admissions: list[tuple[str, str, int, int]] = []  # name, result, num, den
        self.info_messages: list[str] = []
        self._colour_idx = 0

        # Tick normalisation: subtract the first #TS tick so the
        # timeline starts near zero instead of at thousands.
        self._tick_offset: int | None = None

        # SRP resource tracking
        self.locks: list[ResourceLockEvent] = []
        self.unlocks: list[ResourceUnlockEvent] = []
        self.ceiling_history: list[tuple[int, int]] = []  # (tick, ceiling_value)
        self.resource_names: dict[int, str] = {}  # res_idx -> "R0", "R1", ...
        self.task_resources: dict[str, set[int]] = {}  # task_name -> set of res_idx

        # MP tracking
        self.migrations: list[MigrationEvent] = []

        # CBS tracking
        self.cbs_exhaustions: list[CbsBudgetEvent] = []
        self.cbs_arrivals: list[CbsBudgetEvent] = []
        self.cbs_servers: set[str] = set()  # names of CBS tasks (type="cbs")

    def norm_tick(self, tick: int) -> int:
        """Normalise a raw tick to start from zero."""
        if self._tick_offset is None:
            self._tick_offset = tick
        return tick - self._tick_offset

    def register_task(self, name: str, period: int, deadline: int,
                      wcet: int, task_type: str) -> None:
        if name not in self.tasks:
            colour = TASK_COLOURS[self._colour_idx % len(TASK_COLOURS)]
            self._colour_idx += 1
            self.tasks[name] = TaskInfo(name, period, deadline, wcet,
                                        task_type, colour)
            self.stats[name] = TaskStats()
            if task_type == "cbs":
                self.cbs_servers.add(name)

    def add_switch(self, tick: int, task_name: str,
                   core_id: int = 0) -> None:
        self.switches.append(SwitchEvent(tick, task_name, core_id))

    def add_migration(self, tick: int, task_name: str,
                      from_core: int, to_core: int) -> None:
        self.migrations.append(MigrationEvent(tick, task_name, from_core, to_core))

    def add_miss(self, tick: int, task_name: str, abs_deadline: int,
                 miss_count: int) -> None:
        self.misses.append(MissEvent(tick, task_name, abs_deadline, miss_count))
        if task_name in self.stats:
            self.stats[task_name].deadline_misses = miss_count

    def add_job_start(self, task_name: str, tick: int) -> None:
        if task_name in self.stats:
            self.stats[task_name].last_job_start = tick

    def add_job_complete(self, task_name: str, tick: int) -> None:
        if task_name in self.stats:
            self.stats[task_name].jobs_completed += 1

    def add_admission(self, name: str, result: str, num: int, den: int) -> None:
        self.admissions.append((name, result, num, den))

    def add_lock(self, tick: int, task_name: str, res_idx: int,
                 ceiling: int) -> None:
        self.locks.append(ResourceLockEvent(tick, task_name, res_idx, ceiling))
        self.ceiling_history.append((tick, ceiling))
        if res_idx not in self.resource_names:
            self.resource_names[res_idx] = f"R{res_idx}"
        self.task_resources.setdefault(task_name, set()).add(res_idx)

    def add_unlock(self, tick: int, task_name: str, res_idx: int,
                   ceiling: int) -> None:
        self.unlocks.append(ResourceUnlockEvent(tick, task_name, res_idx, ceiling))
        self.ceiling_history.append((tick, ceiling))

    def add_cbs_exhaustion(self, tick: int, task_name: str,
                           budget: int, deadline: int) -> None:
        self.cbs_exhaustions.append(CbsBudgetEvent(tick, task_name, budget, deadline))

    def add_cbs_arrival(self, tick: int, task_name: str,
                        budget: int, deadline: int) -> None:
        self.cbs_arrivals.append(CbsBudgetEvent(tick, task_name, budget, deadline))

    def clear(self) -> None:
        self.__init__()


# ── Serial reader thread ───────────────────────────────────────────────

class SerialReaderThread(QThread):
    line_received = pyqtSignal(str)  # raw line (for console)
    task_registered = pyqtSignal(str, int, int, int, str)
    task_switch = pyqtSignal(int, str, int)  # tick, name, core_id
    job_start = pyqtSignal(int, str, int)
    job_complete = pyqtSignal(int, str)
    deadline_miss = pyqtSignal(int, str, int, int)
    admission = pyqtSignal(str, str, int, int)
    info_msg = pyqtSignal(str)
    connection_status = pyqtSignal(str)
    resource_lock = pyqtSignal(int, str, int, int)    # tick, task, res_idx, ceiling
    resource_unlock = pyqtSignal(int, str, int, int)   # tick, task, res_idx, ceiling
    cbs_budget_exhausted = pyqtSignal(int, str, int, int)  # tick, name, budget, deadline
    cbs_job_arrival = pyqtSignal(int, str, int, int)       # tick, name, budget, deadline
    migration = pyqtSignal(int, str, int, int)              # tick, name, from_core, to_core
    scenario_id = pyqtSignal(str, int)  # task_label, scenario_number

    def __init__(self, port: str, baud: int = 115200) -> None:
        super().__init__()
        self.port = port
        self.baud = baud
        self._running = False
        self._ser: serial.Serial | None = None

    def run(self) -> None:
        self._running = True
        try:
            self._ser = serial.Serial(self.port, self.baud, timeout=0.1)
            self.connection_status.emit(f"Connected to {self.port}")
        except serial.SerialException as e:
            self.connection_status.emit(f"Error: {e}")
            return

        try:
            while self._running:
                try:
                    raw = self._ser.readline()
                except serial.SerialException:
                    self.connection_status.emit("Serial disconnected")
                    break

                if not raw:
                    continue

                try:
                    line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                except Exception:
                    continue

                if not line:
                    continue

                self.line_received.emit(line)
                self._parse(line)
        finally:
            if self._ser:
                self._ser.close()
                self._ser = None
            self.connection_status.emit("Disconnected")

    def send(self, data: bytes) -> None:
        """Send bytes to the serial port (thread-safe)."""
        if self._ser and self._ser.is_open:
            try:
                self._ser.write(data)
            except serial.SerialException:
                pass

    def _parse(self, line: str) -> None:
        if not line.startswith("#"):
            return

        try:
            tag, payload = line[1:].split(":", 1)
        except ValueError:
            return

        parts = payload.split(",")

        if tag == "TR" and len(parts) >= 5:
            name = parts[0]
            period = int(parts[1])
            deadline = int(parts[2])
            wcet = int(parts[3])
            ttype = parts[4]
            self.task_registered.emit(name, period, deadline, wcet, ttype)

        elif tag == "TS" and len(parts) >= 2:
            tick = int(parts[0])
            name = parts[1]
            # Optional 3rd field: core ID (SMP mode)
            core_id = int(parts[2]) if len(parts) >= 3 else 0
            self.task_switch.emit(tick, name, core_id)

        elif tag == "JS" and len(parts) >= 2:
            name = parts[0]
            abs_d = int(parts[1])
            # Use current latest tick as approximation
            self.job_start.emit(0, name, abs_d)

        elif tag == "JC" and len(parts) >= 1:
            name = parts[0]
            self.job_complete.emit(0, name)

        elif tag == "DM" and len(parts) >= 4:
            tick = int(parts[0])
            name = parts[1]
            abs_d = int(parts[2])
            count = int(parts[3])
            self.deadline_miss.emit(tick, name, abs_d, count)

        elif tag == "AR" and len(parts) >= 4:
            name = parts[0]
            result = parts[1]
            num = int(parts[2])
            den = int(parts[3])
            self.admission.emit(name, result, num, den)

        elif tag == "RL" and len(parts) >= 4:
            tick = int(parts[0])
            name = parts[1]
            res_idx = int(parts[2])
            ceiling = int(parts[3])
            self.resource_lock.emit(tick, name, res_idx, ceiling)

        elif tag == "RU" and len(parts) >= 4:
            tick = int(parts[0])
            name = parts[1]
            res_idx = int(parts[2])
            ceiling = int(parts[3])
            self.resource_unlock.emit(tick, name, res_idx, ceiling)

        elif tag == "MG" and len(parts) >= 4:
            tick = int(parts[0])
            name = parts[1]
            from_core = int(parts[2])
            to_core = int(parts[3])
            self.migration.emit(tick, name, from_core, to_core)

        elif tag == "CB" and len(parts) >= 4:
            tick = int(parts[0])
            name = parts[1]
            budget = int(parts[2])
            deadline = int(parts[3])
            self.cbs_budget_exhausted.emit(tick, name, budget, deadline)

        elif tag == "CA" and len(parts) >= 4:
            tick = int(parts[0])
            name = parts[1]
            budget = int(parts[2])
            deadline = int(parts[3])
            self.cbs_job_arrival.emit(tick, name, budget, deadline)

        elif tag == "ID" and len(parts) >= 2:
            task_label = parts[0]
            scenario_num = int(parts[1])
            self.scenario_id.emit(task_label, scenario_num)

        elif tag == "IN":
            self.info_msg.emit(payload)

    def stop(self) -> None:
        self._running = False
        self.wait(2000)


# ── Timeline widget (Gantt chart) ──────────────────────────────────────

class TimelineWidget(pg.PlotWidget):
    """Horizontal Gantt chart: one row per task, coloured bars.
    Supports SRP critical section overlays."""

    def __init__(self) -> None:
        super().__init__()
        self.setLabel("bottom", "Tick")
        self.setLabel("left", "Task")
        self.showGrid(x=True, y=False, alpha=0.3)
        self.setMouseEnabled(y=False)

        self._task_rows: dict[str, int] = {}
        self._bars: list[pg.BarGraphItem] = []
        self._miss_scatter = pg.ScatterPlotItem(
            symbol="d", size=12,
            brush=pg.mkBrush("#ff0000"),
            pen=pg.mkPen(None),
        )
        self.addItem(self._miss_scatter)

        self._segments: list[tuple[str, int, int]] = []  # (task, start, end)
        self._current_task: str | None = None
        self._current_start: int = 0
        self._miss_points: list[dict] = []

        # SRP critical section tracking
        self._lock_segments: list[tuple[str, int, int, int]] = []  # (task, start, end, res_idx)
        self._open_locks: dict[tuple[str, int], int] = {}  # (task, res_idx) -> start_tick
        self._lock_bars: list[pg.BarGraphItem] = []

        # CBS budget exhaustion markers (orange triangles)
        self._cbs_scatter = pg.ScatterPlotItem(
            symbol="t", size=10,
            brush=pg.mkBrush("#ff8800"),
            pen=pg.mkPen("#ff8800"),
        )
        self.addItem(self._cbs_scatter)
        self._cbs_exhaust_points: list[dict] = []

        self._window_size = 500

    def set_tasks(self, task_names: list[str]) -> None:
        self._task_rows.clear()
        for i, name in enumerate(task_names):
            self._task_rows[name] = i

        # Set y-axis ticks
        ticks = [(i, name) for name, i in self._task_rows.items()]
        ay = self.getAxis("left")
        ay.setTicks([ticks])
        self.setYRange(-0.5, len(task_names) - 0.5)

    def add_switch(self, tick: int, task_name: str,
                   colours: dict[str, str]) -> None:
        # Close previous segment
        if self._current_task is not None and self._current_task in self._task_rows:
            self._segments.append(
                (self._current_task, self._current_start, tick)
            )

        self._current_task = task_name
        self._current_start = tick

    def add_miss(self, tick: int, task_name: str) -> None:
        if task_name in self._task_rows:
            row = self._task_rows[task_name]
            self._miss_points.append({"pos": (tick, row)})

    def add_cbs_exhaustion(self, tick: int, task_name: str) -> None:
        if task_name in self._task_rows:
            row = self._task_rows[task_name]
            self._cbs_exhaust_points.append({"pos": (tick, row)})

    def add_resource_lock(self, tick: int, task_name: str,
                          res_idx: int) -> None:
        self._open_locks[(task_name, res_idx)] = tick

    def add_resource_unlock(self, tick: int, task_name: str,
                            res_idx: int) -> None:
        key = (task_name, res_idx)
        if key in self._open_locks:
            start = self._open_locks.pop(key)
            self._lock_segments.append((task_name, start, tick, res_idx))

    def refresh(self, colours: dict[str, str]) -> None:
        """Redraw all bars, lock overlays, and miss markers."""
        # Remove old bars
        for b in self._bars:
            self.removeItem(b)
        self._bars.clear()

        for b in self._lock_bars:
            self.removeItem(b)
        self._lock_bars.clear()

        # Group segments by task for batch rendering
        by_task: dict[str, list[tuple[int, int]]] = {}
        for task, start, end in self._segments:
            by_task.setdefault(task, []).append((start, end))

        for task, segs in by_task.items():
            if task not in self._task_rows:
                continue
            row = self._task_rows[task]
            colour = colours.get(task, "#888888")

            xs = [s for s, e in segs]
            widths = [e - s for s, e in segs]
            ys = [row - 0.35] * len(segs)
            heights = [0.7] * len(segs)

            bar = pg.BarGraphItem(
                x0=xs, width=widths, y0=ys, height=heights,
                brush=pg.mkBrush(colour),
                pen=pg.mkPen(colour),
            )
            self.addItem(bar)
            self._bars.append(bar)

        # Draw SRP critical section overlays
        by_res: dict[int, list[tuple[str, int, int]]] = {}
        for task, start, end, res_idx in self._lock_segments:
            by_res.setdefault(res_idx, []).append((task, start, end))

        for res_idx, segs in by_res.items():
            res_colour = RESOURCE_COLOURS[res_idx % len(RESOURCE_COLOURS)]
            overlay_brush = pg.mkBrush(QColor(res_colour).red(),
                                       QColor(res_colour).green(),
                                       QColor(res_colour).blue(), 80)
            overlay_pen = pg.mkPen(res_colour, width=2)

            for task, start, end in segs:
                if task not in self._task_rows:
                    continue
                row = self._task_rows[task]

                bar = pg.BarGraphItem(
                    x0=[start], width=[end - start],
                    y0=[row - 0.25], height=[0.5],
                    brush=overlay_brush,
                    pen=overlay_pen,
                )
                self.addItem(bar)
                self._lock_bars.append(bar)

        # Miss markers
        if self._miss_points:
            self._miss_scatter.setData(self._miss_points)

        # CBS budget exhaustion markers
        if self._cbs_exhaust_points:
            self._cbs_scatter.setData(self._cbs_exhaust_points)

    def get_max_tick(self) -> int:
        """Return the highest tick seen across all segments."""
        if self._segments:
            return max(e for _, _, e in self._segments)
        return 0

    def set_view_range(self, x_min: int, x_max: int) -> None:
        """Set the visible X-axis range (called by slider controls)."""
        self.setXRange(x_min, x_max, padding=0)


# ── Utilization widget ─────────────────────────────────────────────────

class UtilizationWidget(pg.PlotWidget):
    """Bar chart of per-task utilization."""

    def __init__(self) -> None:
        super().__init__()
        self.setLabel("bottom", "Task")
        self.setLabel("left", "Utilization")
        self.setYRange(0, 1.2)
        self.showGrid(x=False, y=True, alpha=0.3)
        self._bars: pg.BarGraphItem | None = None

    def update_utilization(self, task_names: list[str],
                           configured: list[float],
                           actual: list[float],
                           colours: list[str]) -> None:
        if self._bars is not None:
            self.removeItem(self._bars)

        if not task_names:
            return

        n = len(task_names)
        xs = list(range(n))

        # Configured utilization (lighter)
        brushes_cfg = [pg.mkBrush(QColor(c).lighter(160)) for c in colours]
        bar_cfg = pg.BarGraphItem(
            x0=[x - 0.35 for x in xs], width=[0.35] * n,
            height=configured, y0=[0] * n,
            brushes=brushes_cfg,
        )
        self.addItem(bar_cfg)

        # Actual utilization
        brushes_act = [pg.mkBrush(c) for c in colours]
        bar_act = pg.BarGraphItem(
            x0=[x + 0.0 for x in xs], width=[0.35] * n,
            height=actual, y0=[0] * n,
            brushes=brushes_act,
        )
        self.addItem(bar_act)

        self._bars = bar_act  # track for removal

        ticks = [(i, name) for i, name in enumerate(task_names)]
        ax = self.getAxis("bottom")
        ax.setTicks([ticks])


# ── SRP Resources widget ──────────────────────────────────────────────

class SRPResourceWidget(QWidget):
    """Tab showing system ceiling timeline and per-resource lock Gantt."""

    def __init__(self) -> None:
        super().__init__()

        self._stack = QStackedWidget()
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self._stack)

        # Page 0: empty state
        empty_label = QLabel("No SRP resource data received")
        empty_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        empty_label.setFont(QFont("Menlo", 14))
        empty_label.setStyleSheet("color: #888888;")
        self._stack.addWidget(empty_label)

        # Page 1: plots
        plot_container = QWidget()
        plot_layout = QVBoxLayout(plot_container)
        plot_layout.setContentsMargins(0, 0, 0, 0)

        # Top: system ceiling step plot
        self._ceiling_plot = pg.PlotWidget()
        self._ceiling_plot.setLabel("bottom", "Tick")
        self._ceiling_plot.setLabel("left", "System Ceiling")
        self._ceiling_plot.showGrid(x=True, y=True, alpha=0.3)
        self._ceiling_curve: pg.PlotDataItem | None = None
        self._ceiling_fill: pg.FillBetweenItem | None = None
        plot_layout.addWidget(self._ceiling_plot, stretch=1)

        # Bottom: resource lock timeline (mini Gantt)
        self._res_plot = pg.PlotWidget()
        self._res_plot.setLabel("bottom", "Tick")
        self._res_plot.setLabel("left", "Resource")
        self._res_plot.showGrid(x=True, y=False, alpha=0.3)
        self._res_plot.setMouseEnabled(y=False)
        self._res_bars: list[pg.BarGraphItem] = []
        plot_layout.addWidget(self._res_plot, stretch=1)

        self._stack.addWidget(plot_container)

        self._last_lock_count = 0
        self._last_unlock_count = 0

    def refresh(self, model: ScheduleModel) -> None:
        """Update SRP plots from model data."""
        total_events = len(model.locks) + len(model.unlocks)
        if total_events == 0:
            self._stack.setCurrentIndex(0)
            return

        self._stack.setCurrentIndex(1)

        # Only redraw if new events arrived
        if (len(model.locks) == self._last_lock_count and
                len(model.unlocks) == self._last_unlock_count):
            return
        self._last_lock_count = len(model.locks)
        self._last_unlock_count = len(model.unlocks)

        self._refresh_ceiling(model)
        self._refresh_resource_gantt(model)

    def _refresh_ceiling(self, model: ScheduleModel) -> None:
        """Draw the system ceiling step plot."""
        if self._ceiling_curve is not None:
            self._ceiling_plot.removeItem(self._ceiling_curve)
            self._ceiling_curve = None
        if self._ceiling_fill is not None:
            self._ceiling_plot.removeItem(self._ceiling_fill)
            self._ceiling_fill = None

        if not model.ceiling_history:
            return

        # Build step data: ceiling starts at 0, steps at each event
        xs = [0]
        ys = [0]
        for tick, ceiling in model.ceiling_history:
            xs.append(tick)
            ys.append(ys[-1])  # hold previous value until this tick
            xs.append(tick)
            ys.append(ceiling)

        # Extend to latest tick
        if model.switches:
            last_tick = model.switches[-1].tick
            xs.append(last_tick)
            ys.append(ys[-1])

        self._ceiling_curve = self._ceiling_plot.plot(
            xs, ys,
            pen=pg.mkPen("#ffaa00", width=2),
            fillLevel=0,
            fillBrush=pg.mkBrush(255, 170, 0, 40),
        )

        # Auto-scroll to match timeline
        if xs:
            max_x = max(xs)
            x_min = max(0, max_x - 500)
            self._ceiling_plot.setXRange(x_min, max_x + 10)

    def _refresh_resource_gantt(self, model: ScheduleModel) -> None:
        """Draw per-resource lock bars."""
        for b in self._res_bars:
            self._res_plot.removeItem(b)
        self._res_bars.clear()

        if not model.locks:
            return

        # Build completed lock segments from matching lock/unlock pairs
        open_locks: dict[tuple[str, int], int] = {}  # (task, res_idx) -> tick
        segments: list[tuple[int, str, int, int]] = []  # (res_idx, task, start, end)

        for ev in sorted(model.locks + model.unlocks,
                         key=lambda e: e.tick):
            if isinstance(ev, ResourceLockEvent):
                open_locks[(ev.task_name, ev.res_idx)] = ev.tick
            elif isinstance(ev, ResourceUnlockEvent):
                key = (ev.task_name, ev.res_idx)
                if key in open_locks:
                    segments.append(
                        (ev.res_idx, ev.task_name, open_locks.pop(key), ev.tick))

        # Set up resource rows
        res_indices = sorted(model.resource_names.keys())
        res_rows = {idx: i for i, idx in enumerate(res_indices)}
        ticks = [(i, model.resource_names.get(idx, f"R{idx}"))
                 for idx, i in res_rows.items()]
        ay = self._res_plot.getAxis("left")
        ay.setTicks([ticks])
        if res_indices:
            self._res_plot.setYRange(-0.5, len(res_indices) - 0.5)

        # Draw bars grouped by resource
        task_colours = {n: t.colour for n, t in model.tasks.items()}

        for res_idx, task, start, end in segments:
            if res_idx not in res_rows:
                continue
            row = res_rows[res_idx]
            colour = task_colours.get(task, "#888888")

            bar = pg.BarGraphItem(
                x0=[start], width=[end - start],
                y0=[row - 0.3], height=[0.6],
                brush=pg.mkBrush(colour),
                pen=pg.mkPen(colour),
            )
            self._res_plot.addItem(bar)
            self._res_bars.append(bar)

        # Auto-scroll
        if segments:
            max_x = max(end for _, _, _, end in segments)
            x_min = max(0, max_x - 500)
            self._res_plot.setXRange(x_min, max_x + 10)

    def clear(self) -> None:
        self._last_lock_count = 0
        self._last_unlock_count = 0
        self._stack.setCurrentIndex(0)
        if self._ceiling_curve is not None:
            self._ceiling_plot.removeItem(self._ceiling_curve)
            self._ceiling_curve = None
        if self._ceiling_fill is not None:
            self._ceiling_plot.removeItem(self._ceiling_fill)
            self._ceiling_fill = None
        for b in self._res_bars:
            self._res_plot.removeItem(b)
        self._res_bars.clear()


# ── CBS Budget widget ──────────────────────────────────────────────────

class CBSBudgetWidget(QWidget):
    """Tab showing CBS server budget over time."""

    def __init__(self) -> None:
        super().__init__()

        self._stack = QStackedWidget()
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self._stack)

        # Page 0: empty state
        empty_label = QLabel("No CBS data received")
        empty_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        empty_label.setFont(QFont("Menlo", 14))
        empty_label.setStyleSheet("color: #888888;")
        self._stack.addWidget(empty_label)

        # Page 1: budget plot
        self._budget_plot = pg.PlotWidget()
        self._budget_plot.setLabel("bottom", "Tick")
        self._budget_plot.setLabel("left", "Budget Remaining")
        self._budget_plot.showGrid(x=True, y=True, alpha=0.3)
        self._budget_plot.addLegend()
        self._budget_curves: list[pg.PlotDataItem] = []
        self._exhaust_lines: list[pg.InfiniteLine] = []
        self._stack.addWidget(self._budget_plot)

        self._last_event_count = 0

    def refresh(self, model: ScheduleModel) -> None:
        total = len(model.cbs_exhaustions) + len(model.cbs_arrivals)
        if not model.cbs_servers:
            self._stack.setCurrentIndex(0)
            return

        self._stack.setCurrentIndex(1)

        if total == self._last_event_count:
            return
        self._last_event_count = total

        # Clear old curves and lines
        for c in self._budget_curves:
            self._budget_plot.removeItem(c)
        self._budget_curves.clear()
        for ln in self._exhaust_lines:
            self._budget_plot.removeItem(ln)
        self._exhaust_lines.clear()

        # Reconstruct budget timeline per CBS server
        task_colours = {n: t.colour for n, t in model.tasks.items()}

        for name in sorted(model.cbs_servers):
            info = model.tasks.get(name)
            if info is None:
                continue

            Q_s = info.wcet  # budget capacity
            colour = task_colours.get(name, "#888888")

            # Collect all CBS events for this server, sorted by tick
            events: list[tuple[int, str, int]] = []
            for ev in model.cbs_arrivals:
                if ev.task_name == name:
                    events.append((ev.tick, "arrival", ev.new_budget))
            for ev in model.cbs_exhaustions:
                if ev.task_name == name:
                    events.append((ev.tick, "exhaust", ev.new_budget))
            events.sort(key=lambda x: x[0])

            # Build step points: budget decreases when CBS runs, jumps on events
            # We use switch events to infer execution periods
            xs: list[float] = []
            ys: list[float] = []

            # Start at 0 budget (idle)
            budget = 0
            last_tick = 0

            for tick, etype, val in events:
                # Add the point just before this event
                if xs:
                    xs.append(tick)
                    ys.append(ys[-1])  # hold previous value
                xs.append(tick)
                ys.append(val)
                budget = val
                last_tick = tick

            # Extend to latest switch tick
            if model.switches and xs:
                last_sw = model.switches[-1].tick
                if last_sw > last_tick:
                    xs.append(last_sw)
                    ys.append(ys[-1])

            if xs:
                curve = self._budget_plot.plot(
                    xs, ys,
                    pen=pg.mkPen(colour, width=2),
                    name=name,
                )
                self._budget_curves.append(curve)

            # Add vertical dashed lines for exhaustion events
            for ev in model.cbs_exhaustions:
                if ev.task_name == name:
                    line = pg.InfiniteLine(
                        pos=ev.tick,
                        angle=90,
                        pen=pg.mkPen(colour, width=1,
                                     style=Qt.PenStyle.DashLine),
                    )
                    self._budget_plot.addItem(line)
                    self._exhaust_lines.append(line)

        # Auto-scroll
        if model.switches:
            max_x = model.switches[-1].tick
            x_min = max(0, max_x - 500)
            self._budget_plot.setXRange(x_min, max_x + 10)

    def clear(self) -> None:
        self._last_event_count = 0
        self._stack.setCurrentIndex(0)
        for c in self._budget_curves:
            self._budget_plot.removeItem(c)
        self._budget_curves.clear()
        for ln in self._exhaust_lines:
            self._budget_plot.removeItem(ln)
        self._exhaust_lines.clear()


# ── Main window ────────────────────────────────────────────────────────

class MainWindow(QMainWindow):
    def __init__(self, initial_port: str | None = None,
                 baud: int = 115200) -> None:
        super().__init__()
        self.setWindowTitle("FreeRTOS Extension Monitor")
        self.resize(1200, 700)

        self.model = ScheduleModel()
        self._serial_thread: SerialReaderThread | None = None
        self._baud = baud

        self._build_toolbar()
        self._build_ui()
        self._build_statusbar()

        # Refresh timer (update charts at ~10 Hz)
        self._timer = QTimer()
        self._timer.timeout.connect(self._refresh_views)
        self._timer.start(100)

        if initial_port:
            self._port_combo.setCurrentText(initial_port)
            self._connect()

    # ── Toolbar ──

    def _build_toolbar(self) -> None:
        tb = QToolBar("Main")
        tb.setMovable(False)
        self.addToolBar(tb)

        tb.addWidget(QLabel("  Port: "))
        self._port_combo = QComboBox()
        self._port_combo.setMinimumWidth(200)
        self._refresh_ports()
        tb.addWidget(self._port_combo)

        self._refresh_btn = QPushButton("Refresh")
        self._refresh_btn.clicked.connect(self._refresh_ports)
        tb.addWidget(self._refresh_btn)

        tb.addSeparator()

        self._connect_btn = QPushButton("Connect")
        self._connect_btn.clicked.connect(self._toggle_connection)
        tb.addWidget(self._connect_btn)

        tb.addSeparator()

        tb.addWidget(QLabel("  Scenario: "))
        self._scenario_combo = QComboBox()
        self._scenario_combo.setMinimumWidth(250)
        # Populate with all scenarios
        self._scenario_combo.addItem("(auto-detect)", None)
        for key, (preset, demo, label, desc) in SCENARIOS.items():
            self._scenario_combo.addItem(label, key)
        self._scenario_combo.currentIndexChanged.connect(self._on_scenario_changed)
        tb.addWidget(self._scenario_combo)

        tb.addSeparator()

        clear_btn = QPushButton("Clear")
        clear_btn.clicked.connect(self._clear_all)
        tb.addWidget(clear_btn)

        export_btn = QPushButton("Export CSV")
        export_btn.clicked.connect(self._export_csv)
        tb.addWidget(export_btn)

        flash_btn = QPushButton("Flash && Run")
        flash_btn.setToolTip("Build the selected scenario, flash via pyocd, and reset")
        flash_btn.clicked.connect(self._flash_and_run)
        tb.addWidget(flash_btn)

        tb.addSeparator()

        reset_btn = QPushButton("Reset Target")
        reset_btn.setToolTip("Reset the Pico 2 W via the debug probe (pyocd)")
        reset_btn.clicked.connect(self._reset_target)
        tb.addWidget(reset_btn)

        tb.addSeparator()

        info_btn = QPushButton("?")
        info_btn.setFixedWidth(30)
        info_btn.setToolTip("Show help / what each tab displays")
        info_btn.clicked.connect(self._show_info)
        tb.addWidget(info_btn)

    def _refresh_ports(self) -> None:
        self._port_combo.clear()
        ports = serial.tools.list_ports.comports()
        for p in ports:
            label = f"{p.device}"
            if p.vid and p.pid:
                label += f" [{p.vid:04X}:{p.pid:04X}]"
            if p.description and p.description != "n/a":
                label += f" — {p.description}"
            self._port_combo.addItem(label, p.device)

        # Auto-select Pico (RP2350 VID:PID = 2E8A:000A)
        for i, p in enumerate(ports):
            if p.vid == 0x2E8A:
                self._port_combo.setCurrentIndex(i)
                break

    # ── UI layout ──

    def _build_ui(self) -> None:
        central = QWidget()
        self.setCentralWidget(central)
        layout = QHBoxLayout(central)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        layout.addWidget(splitter)

        # Left: task list
        self._task_tree = QTreeWidget()
        self._task_tree.setHeaderLabels(["Task", "Period", "Deadline",
                                         "WCET", "Type", "Misses",
                                         "Resources"])
        self._task_tree.setMaximumWidth(400)
        splitter.addWidget(self._task_tree)

        # Right side: description + tabs
        right_container = QWidget()
        right_layout = QVBoxLayout(right_container)
        right_layout.setContentsMargins(0, 0, 0, 0)

        self._scenario_desc = QLabel("")
        self._scenario_desc.setWordWrap(True)
        self._scenario_desc.setStyleSheet("padding: 6px; background: #2a2a2a; color: #cccccc; border-radius: 4px;")
        self._scenario_desc.setFont(QFont("Menlo", 10))
        self._scenario_desc.setMinimumHeight(40)
        self._scenario_desc.hide()  # Hidden until a scenario is detected
        right_layout.addWidget(self._scenario_desc)

        self._tabs = QTabWidget()
        right_layout.addWidget(self._tabs)

        splitter.addWidget(right_container)

        # Tab 1: Timeline (with zoom/scroll controls)
        timeline_container = QWidget()
        tl_layout = QVBoxLayout(timeline_container)
        tl_layout.setContentsMargins(0, 0, 0, 0)

        self._timeline = TimelineWidget()
        tl_layout.addWidget(self._timeline, stretch=1)

        # Scroll slider (position)
        scroll_row = QHBoxLayout()
        scroll_row.addWidget(QLabel("Scroll:"))
        self._tl_scroll = QSlider(Qt.Orientation.Horizontal)
        self._tl_scroll.setMinimum(0)
        self._tl_scroll.setMaximum(1000)
        self._tl_scroll.setValue(1000)  # start at the right (latest)
        self._tl_scroll.valueChanged.connect(self._on_timeline_slider)
        scroll_row.addWidget(self._tl_scroll, stretch=1)

        # Zoom slider (window width)
        scroll_row.addWidget(QLabel("  Zoom:"))
        self._tl_zoom = QSlider(Qt.Orientation.Horizontal)
        self._tl_zoom.setMinimum(50)     # min 50 ticks visible
        self._tl_zoom.setMaximum(5000)   # max 5000 ticks visible
        self._tl_zoom.setValue(500)       # default 500 ticks
        self._tl_zoom.valueChanged.connect(self._on_timeline_slider)
        scroll_row.addWidget(self._tl_zoom, stretch=1)

        self._tl_auto_scroll = True  # track whether to auto-scroll
        tl_layout.addLayout(scroll_row)

        self._tabs.addTab(timeline_container, "Timeline")

        # Tab 2: Utilization
        self._util_widget = UtilizationWidget()
        self._tabs.addTab(self._util_widget, "Utilization")

        # Tab 3: Stats table
        self._stats_table = QTableWidget()
        self._stats_table.setColumnCount(6)
        self._stats_table.setHorizontalHeaderLabels([
            "Task", "Period", "Jobs Done", "Misses",
            "Configured U", "Actual U",
        ])
        self._stats_table.horizontalHeader().setSectionResizeMode(
            QHeaderView.ResizeMode.Stretch)
        self._tabs.addTab(self._stats_table, "Stats")

        # Tab 4: Console (with serial input)
        console_container = QWidget()
        console_layout = QVBoxLayout(console_container)
        console_layout.setContentsMargins(0, 0, 0, 0)

        self._console = QPlainTextEdit()
        self._console.setReadOnly(True)
        self._console.setFont(QFont("Menlo", 11))
        self._console.setMaximumBlockCount(5000)
        console_layout.addWidget(self._console)

        input_row = QHBoxLayout()
        self._serial_input = QLineEdit()
        self._serial_input.setFont(QFont("Menlo", 11))
        self._serial_input.setPlaceholderText("Type here and press Enter to send...")
        self._serial_input.returnPressed.connect(self._send_serial_input)
        input_row.addWidget(self._serial_input)

        send_btn = QPushButton("Send")
        send_btn.clicked.connect(self._send_serial_input)
        input_row.addWidget(send_btn)
        console_layout.addLayout(input_row)

        self._tabs.addTab(console_container, "Console")

        # Tab 5: SRP Resources
        self._srp_widget = SRPResourceWidget()
        self._tabs.addTab(self._srp_widget, "SRP Resources")

        # Tab 6: CBS Budget
        self._cbs_widget = CBSBudgetWidget()
        self._tabs.addTab(self._cbs_widget, "CBS Budget")

        splitter.setSizes([280, 920])

    def _build_statusbar(self) -> None:
        self._status_label = QLabel("Disconnected")
        self.statusBar().addWidget(self._status_label)

    # ── Connection ──

    def _toggle_connection(self) -> None:
        if self._serial_thread and self._serial_thread.isRunning():
            self._disconnect()
        else:
            self._connect()

    def _connect(self) -> None:
        port = self._port_combo.currentData()
        if not port:
            self._status_label.setText("No port selected")
            return

        self._serial_thread = SerialReaderThread(port, self._baud)
        self._serial_thread.line_received.connect(self._on_line)
        self._serial_thread.task_registered.connect(self._on_task_registered)
        self._serial_thread.task_switch.connect(self._on_task_switch)
        self._serial_thread.job_start.connect(self._on_job_start)
        self._serial_thread.job_complete.connect(self._on_job_complete)
        self._serial_thread.deadline_miss.connect(self._on_deadline_miss)
        self._serial_thread.admission.connect(self._on_admission)
        self._serial_thread.info_msg.connect(self._on_info)
        self._serial_thread.connection_status.connect(self._on_conn_status)
        self._serial_thread.resource_lock.connect(self._on_resource_lock)
        self._serial_thread.resource_unlock.connect(self._on_resource_unlock)
        self._serial_thread.cbs_budget_exhausted.connect(self._on_cbs_exhaustion)
        self._serial_thread.cbs_job_arrival.connect(self._on_cbs_arrival)
        self._serial_thread.migration.connect(self._on_migration)
        self._serial_thread.scenario_id.connect(self._on_scenario_id)
        self._serial_thread.start()
        self._connect_btn.setText("Disconnect")

    def _disconnect(self) -> None:
        if self._serial_thread:
            self._serial_thread.stop()
            self._serial_thread = None
        self._connect_btn.setText("Connect")

    # ── Signal handlers ──

    def _on_line(self, line: str) -> None:
        self._console.appendPlainText(line)

    def _on_conn_status(self, status: str) -> None:
        self._status_label.setText(status)

    def _on_task_registered(self, name: str, period: int, deadline: int,
                            wcet: int, ttype: str) -> None:
        self.model.register_task(name, period, deadline, wcet, ttype)
        self._update_task_tree()

        # Update timeline rows
        task_names = list(self.model.tasks.keys())
        self._timeline.set_tasks(task_names)

    def _on_task_switch(self, tick: int, name: str,
                        core_id: int = 0) -> None:
        tick = self.model.norm_tick(tick)
        self.model.add_switch(tick, name, core_id)
        colours = {n: t.colour for n, t in self.model.tasks.items()}
        self._timeline.add_switch(tick, name, colours)

    def _on_job_start(self, tick: int, name: str, abs_d: int) -> None:
        # Use the latest switch tick as approximation
        t = self.model.switches[-1].tick if self.model.switches else 0
        self.model.add_job_start(name, t)

    def _on_job_complete(self, tick: int, name: str) -> None:
        t = self.model.switches[-1].tick if self.model.switches else 0
        self.model.add_job_complete(name, t)

    def _on_deadline_miss(self, tick: int, name: str, abs_d: int,
                          count: int) -> None:
        tick = self.model.norm_tick(tick)
        self.model.add_miss(tick, name, abs_d, count)
        self._timeline.add_miss(tick, name)
        self._update_task_tree()

    def _on_admission(self, name: str, result: str, num: int,
                      den: int) -> None:
        self.model.add_admission(name, result, num, den)

    def _on_info(self, msg: str) -> None:
        self.model.info_messages.append(msg)

    def _on_resource_lock(self, tick: int, name: str, res_idx: int,
                          ceiling: int) -> None:
        tick = self.model.norm_tick(tick)
        self.model.add_lock(tick, name, res_idx, ceiling)
        self._timeline.add_resource_lock(tick, name, res_idx)
        self._update_task_tree()

    def _on_resource_unlock(self, tick: int, name: str, res_idx: int,
                            ceiling: int) -> None:
        tick = self.model.norm_tick(tick)
        self.model.add_unlock(tick, name, res_idx, ceiling)
        self._timeline.add_resource_unlock(tick, name, res_idx)

    def _on_cbs_exhaustion(self, tick: int, name: str, budget: int,
                           deadline: int) -> None:
        tick = self.model.norm_tick(tick)
        self.model.add_cbs_exhaustion(tick, name, budget, deadline)
        self._timeline.add_cbs_exhaustion(tick, name)

    def _on_cbs_arrival(self, tick: int, name: str, budget: int,
                        deadline: int) -> None:
        tick = self.model.norm_tick(tick)
        self.model.add_cbs_arrival(tick, name, budget, deadline)

    def _on_migration(self, tick: int, name: str, from_core: int,
                      to_core: int) -> None:
        tick = self.model.norm_tick(tick)
        self.model.add_migration(tick, name, from_core, to_core)

    def _send_serial_input(self) -> None:
        text = self._serial_input.text()
        if text and self._serial_thread:
            self._serial_thread.send(text.encode("utf-8"))
            self._console.appendPlainText(f"> {text}")
            self._serial_input.clear()

    def _on_timeline_slider(self) -> None:
        """Handle scroll/zoom slider changes."""
        self._tl_auto_scroll = (self._tl_scroll.value()
                                >= self._tl_scroll.maximum() - 10)
        self._apply_timeline_range()

    def _apply_timeline_range(self) -> None:
        """Set timeline X range from current slider values."""
        max_tick = self._timeline.get_max_tick()
        if max_tick <= 0:
            return
        window = self._tl_zoom.value()
        # Scroll slider maps [0..1000] to [0..max_tick-window]
        scrollable = max(0, max_tick - window)
        pos = int(self._tl_scroll.value() / 1000.0 * scrollable)
        self._timeline.set_view_range(pos, pos + window)

    def _reset_target(self) -> None:
        """Reset the Pico via pyocd."""
        pyocd = os.path.expanduser("~/.local/bin/pyocd")
        try:
            subprocess.run([pyocd, "reset", "-t", "rp2350"],
                           capture_output=True, timeout=5)
            self._status_label.setText("Target reset")
        except Exception as e:
            self._status_label.setText(f"Reset failed: {e}")

    def _show_info(self) -> None:
        """Show help dialog explaining the monitor's tabs."""
        QMessageBox.information(self, "FreeRTOS Extension Monitor — Help",
            "<h3>Tabs</h3>"
            "<p><b>Timeline</b> — Gantt chart of task execution over time. "
            "Each horizontal bar shows when a task was running on the CPU. "
            "Coloured bars = normal execution. "
            "Yellow overlays = SRP critical sections (resource held). "
            "Red diamonds = deadline misses. "
            "Orange triangles = CBS budget exhaustion.<br>"
            "Use <i>Scroll</i> to move through time and <i>Zoom</i> to "
            "change the visible window width (in ticks, 1 tick = 1 ms).</p>"
            "<p><b>Utilization</b> — Bar chart comparing each task's "
            "<i>configured</i> utilization (C/T, lighter bar) vs "
            "<i>actual</i> utilization (measured from trace, darker bar).</p>"
            "<p><b>Stats</b> — Table of per-task statistics: period, "
            "jobs completed, deadline misses, configured and actual "
            "utilization.</p>"
            "<p><b>Console</b> — Raw serial output from the Pico. "
            "Use the input field at the bottom to send characters "
            "(e.g. type <b>1</b> to select scenario 1).</p>"
            "<p><b>SRP Resources</b> — (SRP demos only) System ceiling "
            "step plot showing how the ceiling rises/falls as resources "
            "are locked/unlocked. Below: per-resource lock timeline.</p>"
            "<p><b>CBS Budget</b> — (CBS demos only) Per-server budget "
            "sawtooth showing budget depletion over time.</p>"
            "<h3>Sidebar</h3>"
            "<p>Shows registered tasks with their real-time parameters "
            "(Period, Deadline, WCET), scheduling type, deadline miss "
            "count, and SRP resource usage.</p>"
            "<h3>Toolbar</h3>"
            "<p><b>Reset Target</b> resets the Pico via the debug probe "
            "(pyocd). <b>Clear</b> clears all data. <b>Export CSV</b> "
            "saves the timeline, stats, and resource events to files.</p>"
        )

    # ── Scenario handling ──

    def _on_scenario_changed(self) -> None:
        key = self._scenario_combo.currentData()
        if key and key in SCENARIOS:
            _, _, label, desc = SCENARIOS[key]
            self._scenario_desc.setText(desc)
            self._scenario_desc.show()
        else:
            self._scenario_desc.hide()

    def _on_scenario_id(self, task_label: str, scenario_num: int) -> None:
        """Auto-select scenario when #ID event received."""
        key = SCENARIO_ID_MAP.get((task_label, scenario_num))
        if key:
            # Find and select in combo
            for i in range(self._scenario_combo.count()):
                if self._scenario_combo.itemData(i) == key:
                    self._scenario_combo.setCurrentIndex(i)
                    break

    def _flash_and_run(self) -> None:
        key = self._scenario_combo.currentData()
        if not key or key not in SCENARIOS:
            self._status_label.setText("Select a scenario first")
            return

        preset, demo, label, _ = SCENARIOS[key]
        self._status_label.setText(f"Building {preset}...")
        QApplication.processEvents()

        # Disconnect serial first (pyocd needs exclusive access to probe)
        self._disconnect()

        pyocd = os.path.expanduser("~/.local/bin/pyocd")

        # Build
        result = subprocess.run(
            ["cmake", "--preset", preset],
            capture_output=True, text=True, cwd=os.path.dirname(os.path.dirname(__file__))
        )
        if result.returncode != 0:
            self._status_label.setText(f"Configure failed: {result.stderr[-100:]}")
            return

        result = subprocess.run(
            ["cmake", "--build", "--preset", preset],
            capture_output=True, text=True, cwd=os.path.dirname(os.path.dirname(__file__))
        )
        if result.returncode != 0:
            self._status_label.setText(f"Build failed: {result.stderr[-100:]}")
            return

        # Flash
        elf_path = os.path.join(
            os.path.dirname(os.path.dirname(__file__)),
            "build", preset, f"{demo}.elf"
        )
        self._status_label.setText(f"Flashing {preset}...")
        QApplication.processEvents()

        result = subprocess.run(
            [pyocd, "flash", "-t", "rp2350", elf_path],
            capture_output=True, text=True
        )
        if result.returncode != 0:
            self._status_label.setText(f"Flash failed: {result.stderr[-100:]}")
            return

        # Reset
        subprocess.run([pyocd, "reset", "-t", "rp2350"], capture_output=True)

        self._status_label.setText(f"Flashed {label}. Reconnecting in 3s...")

        # Auto-reconnect after 3 seconds
        QTimer.singleShot(3000, self._connect)

    # ── Periodic refresh ──

    def _refresh_views(self) -> None:
        if not self.model.tasks:
            return

        colours = {n: t.colour for n, t in self.model.tasks.items()}
        self._timeline.refresh(colours)
        self._refresh_stats()
        self._srp_widget.refresh(self.model)
        self._cbs_widget.refresh(self.model)

        # Auto-scroll timeline if slider is at the right edge
        if self._tl_auto_scroll:
            self._tl_scroll.setValue(self._tl_scroll.maximum())
        self._apply_timeline_range()

        # Status bar
        nsw = len(self.model.switches)
        nseg = len(self._timeline._segments)
        ntasks = len(self.model.tasks)
        max_t = self._timeline.get_max_tick()
        self._status_label.setText(
            f"Tasks: {ntasks}  Switches: {nsw}  Segments: {nseg}  "
            f"Max tick: {max_t}"
        )

    def _update_task_tree(self) -> None:
        self._task_tree.clear()
        for name, info in self.model.tasks.items():
            stats = self.model.stats.get(name, TaskStats())
            res_set = self.model.task_resources.get(name, set())
            res_str = ", ".join(f"R{i}" for i in sorted(res_set)) if res_set else "-"

            item = QTreeWidgetItem([
                name,
                str(info.period),
                str(info.deadline),
                str(info.wcet),
                info.task_type,
                str(stats.deadline_misses),
                res_str,
            ])

            # Colour the name column
            item.setForeground(0, QColor(info.colour))

            if stats.deadline_misses > 0:
                item.setBackground(5, QColor("#ffcccc"))

            # CBS tasks get orange tint on Type column
            if name in self.model.cbs_servers:
                item.setBackground(4, QColor("#fff0d0"))

            self._task_tree.addTopLevelItem(item)

    def _refresh_stats(self) -> None:
        task_names = list(self.model.tasks.keys())
        self._stats_table.setRowCount(len(task_names))

        configured_u: list[float] = []
        actual_u: list[float] = []
        colours: list[str] = []

        # Compute actual utilization from switch events
        exec_ticks: dict[str, int] = {}
        max_tick = 0

        for i in range(len(self.model.switches)):
            sw = self.model.switches[i]
            end_tick = (self.model.switches[i + 1].tick
                        if i + 1 < len(self.model.switches)
                        else sw.tick)
            duration = end_tick - sw.tick
            exec_ticks[sw.task_name] = exec_ticks.get(sw.task_name, 0) + duration
            if end_tick > max_tick:
                max_tick = end_tick

        for row, name in enumerate(task_names):
            info = self.model.tasks[name]
            stats = self.model.stats.get(name, TaskStats())

            cfg_u = info.wcet / info.period if info.period > 0 else 0
            act_u = exec_ticks.get(name, 0) / max_tick if max_tick > 0 else 0

            configured_u.append(cfg_u)
            actual_u.append(act_u)
            colours.append(info.colour)

            self._stats_table.setItem(row, 0, QTableWidgetItem(name))
            self._stats_table.setItem(row, 1, QTableWidgetItem(str(info.period)))
            self._stats_table.setItem(row, 2, QTableWidgetItem(
                str(stats.jobs_completed)))
            miss_item = QTableWidgetItem(str(stats.deadline_misses))
            if stats.deadline_misses > 0:
                miss_item.setBackground(QColor("#ffcccc"))
            self._stats_table.setItem(row, 3, miss_item)
            self._stats_table.setItem(row, 4, QTableWidgetItem(f"{cfg_u:.3f}"))
            self._stats_table.setItem(row, 5, QTableWidgetItem(f"{act_u:.3f}"))

        # Update utilization chart
        self._util_widget.update_utilization(
            task_names, configured_u, actual_u, colours)

    # ── Actions ──

    def _clear_all(self) -> None:
        self.model.clear()
        self._task_tree.clear()
        self._console.clear()
        self._timeline = TimelineWidget()
        # Replace the timeline tab
        self._tabs.removeTab(0)
        self._tabs.insertTab(0, self._timeline, "Timeline")
        self._tabs.setCurrentIndex(0)
        self._srp_widget.clear()
        self._cbs_widget.clear()

    def _export_csv(self) -> None:
        path, _ = QFileDialog.getSaveFileName(
            self, "Export Timeline CSV", "timeline.csv",
            "CSV files (*.csv)")
        if not path:
            return

        with open(path, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["tick", "task_name"])
            for sw in self.model.switches:
                writer.writerow([sw.tick, sw.task_name])

        # Also export stats
        stats_path = Path(path).with_name(
            Path(path).stem + "_stats.csv")
        with open(stats_path, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["task", "period", "deadline", "wcet", "type",
                             "jobs_completed", "deadline_misses"])
            for name, info in self.model.tasks.items():
                stats = self.model.stats.get(name, TaskStats())
                writer.writerow([
                    name, info.period, info.deadline, info.wcet,
                    info.task_type, stats.jobs_completed,
                    stats.deadline_misses,
                ])

        # Export SRP resource events
        if self.model.locks or self.model.unlocks:
            res_path = Path(path).with_name(
                Path(path).stem + "_resources.csv")
            with open(res_path, "w", newline="") as f:
                writer = csv.writer(f)
                writer.writerow(["tick", "event", "task_name", "res_idx", "ceiling"])
                all_events: list[tuple[int, str, str, int, int]] = []
                for ev in self.model.locks:
                    all_events.append(
                        (ev.tick, "LOCK", ev.task_name, ev.res_idx, ev.ceiling))
                for ev in self.model.unlocks:
                    all_events.append(
                        (ev.tick, "UNLOCK", ev.task_name, ev.res_idx, ev.ceiling))
                all_events.sort(key=lambda x: x[0])
                for row in all_events:
                    writer.writerow(row)

        # Export CBS events
        if self.model.cbs_exhaustions or self.model.cbs_arrivals:
            cbs_path = Path(path).with_name(
                Path(path).stem + "_cbs.csv")
            with open(cbs_path, "w", newline="") as f:
                writer = csv.writer(f)
                writer.writerow(["tick", "event", "task_name", "budget", "deadline"])
                all_cbs: list[tuple[int, str, str, int, int]] = []
                for ev in self.model.cbs_exhaustions:
                    all_cbs.append(
                        (ev.tick, "EXHAUST", ev.task_name, ev.new_budget, ev.new_deadline))
                for ev in self.model.cbs_arrivals:
                    all_cbs.append(
                        (ev.tick, "ARRIVAL", ev.task_name, ev.new_budget, ev.new_deadline))
                all_cbs.sort(key=lambda x: x[0])
                for row in all_cbs:
                    writer.writerow(row)

        self._status_label.setText(f"Exported to {path}")

    def closeEvent(self, event) -> None:
        self._disconnect()
        super().closeEvent(event)


# ── Entry point ────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="FreeRTOS Extension Monitor")
    parser.add_argument("--port", "-p", default=None,
                        help="Serial port (auto-detected if omitted)")
    parser.add_argument("--baud", "-b", type=int, default=115200,
                        help="Baud rate (default: 115200)")
    args = parser.parse_args()

    app = QApplication(sys.argv)
    app.setStyle("Fusion")

    window = MainWindow(initial_port=args.port, baud=args.baud)
    window.show()

    sys.exit(app.exec())


if __name__ == "__main__":
    main()
