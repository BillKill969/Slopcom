#!/usr/bin/env python3
"""
benchmarks/run.py - Test harness and benchmark runner for slopcom.

Supports:
  - verify: 100+ start/stop cycles, selection collision, real-X fixture scenarios,
            and 10-minute scripted churn with i3 workspace switching.
  - bench:  (v0.3/v0.4) 6 standard workloads comparing backends and measuring idle.
  - soak:   (v0.5) Multi-hour stability and resource leak soak test.
"""

import sys
import os
import time
import json
import signal
import subprocess
import argparse
import ctypes
import threading
import queue
import shutil
from pathlib import Path

# --- X11 CTypes Helpers ---

_cached_display = {}
_cached_atom = {}

def get_x11_library():
    try:
        x11 = ctypes.cdll.LoadLibrary("libX11.so.6")
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XDefaultScreen.argtypes = [ctypes.c_void_p]
        x11.XDefaultScreen.restype = ctypes.c_int
        x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        x11.XInternAtom.restype = ctypes.c_ulong
        x11.XGetSelectionOwner.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        x11.XGetSelectionOwner.restype = ctypes.c_ulong
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        x11.XCloseDisplay.restype = ctypes.c_int
        x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        x11.XSync.restype = ctypes.c_int
        return x11
    except Exception as e:
        print(f"Warning: Failed to load libX11.so.6: {e}", file=sys.stderr)
        return None

def get_selection_owner(display_name: str, screen: int = 0) -> int:
    x11 = get_x11_library()
    if not x11:
        return 0
    key = (display_name, screen)
    dpy = _cached_display.get(display_name)
    if not dpy:
        dpy = x11.XOpenDisplay(display_name.encode("utf-8") if display_name else None)
        if not dpy:
            raise RuntimeError(f"Cannot open X display '{display_name}'")
        _cached_display[display_name] = dpy
    atom = _cached_atom.get(key)
    if not atom:
        atom_name = f"_NET_WM_CM_S{screen}".encode("ascii")
        atom = x11.XInternAtom(dpy, atom_name, 0)
        _cached_atom[key] = atom
    x11.XSync(dpy, 0)
    return int(x11.XGetSelectionOwner(dpy, atom))

# --- Process Controller ---

class SlopcomProcess:
    def __init__(self, binary_path: str, display: str, extra_args: list = None):
        self.binary_path = binary_path
        self.display = display
        self.extra_args = extra_args or []
        self.proc = None
        self.snapshots = queue.Queue()
        self.stdout_lines = []
        self.stderr_lines = []
        self._reader_thread = None
        self._err_thread = None

    def start(self, timeout: float = 5.0):
        cmd = [self.binary_path, "--stats"] + self.extra_args + [self.display]
        env = os.environ.copy()
        env["DISPLAY"] = self.display

        self.proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            env=env
        )

        def read_stdout():
            for line in iter(self.proc.stdout.readline, ''):
                self.stdout_lines.append(line)
                line_str = line.strip()
                if line_str.startswith("{") and line_str.endswith("}"):
                    try:
                        data = json.loads(line_str)
                        self.snapshots.put(data)
                    except json.JSONDecodeError:
                        pass
            self.proc.stdout.close()

        def read_stderr():
            for line in iter(self.proc.stderr.readline, ''):
                self.stderr_lines.append(line)
            self.proc.stderr.close()

        self._reader_thread = threading.Thread(target=read_stdout, daemon=True)
        self._reader_thread.start()
        self._err_thread = threading.Thread(target=read_stderr, daemon=True)
        self._err_thread.start()

        # Wait until selection owner becomes non-zero
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.proc.poll() is not None:
                raise RuntimeError(
                    f"slopcom exited prematurely with code {self.proc.returncode}:\n"
                    f"{''.join(self.stderr_lines)}"
                )
            owner = get_selection_owner(self.display)
            if owner != 0:
                return owner
            time.sleep(0.005)
        self.stop()
        raise TimeoutError("slopcom timed out acquiring selection")

    def query_snapshot(self, timeout: float = 2.0) -> dict:
        if self.proc.poll() is not None:
            raise RuntimeError(f"Process already terminated with code {self.proc.returncode}:\n{''.join(self.stderr_lines)}")
        # Clear any old unread snapshots
        while not self.snapshots.empty():
            try:
                self.snapshots.get_nowait()
            except queue.Empty:
                break

        self.proc.send_signal(signal.SIGUSR1)
        try:
            return self.snapshots.get(timeout=timeout)
        except queue.Empty:
            raise TimeoutError("Timed out waiting for SIGUSR1 snapshot response")

    def stop(self, timeout: float = 5.0) -> tuple[int, dict]:
        if not self.proc:
            return 0, {}
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()

        if self._reader_thread:
            self._reader_thread.join(timeout=1.0)
        if self._err_thread:
            self._err_thread.join(timeout=1.0)

        # Ensure selection ownership is cleared on X server before returning
        t0 = time.time()
        while time.time() - t0 < 2.0:
            if get_selection_owner(self.display) == 0:
                break
            time.sleep(0.005)

        # Look for the exit snapshot
        exit_snapshot = None
        while not self.snapshots.empty():
            try:
                snap = self.snapshots.get_nowait()
                if snap.get("kind") == "exit":
                    exit_snapshot = snap
            except queue.Empty:
                break

        return self.proc.returncode, exit_snapshot

# --- Memory Diagnostics ---

def positive_pid(value: str) -> int:
    try:
        val = int(value)
        if val <= 0:
            raise ValueError()
        return val
    except ValueError:
        raise argparse.ArgumentTypeError(f"PID must be a positive integer, got {value!r}")

def sum_if_known(values: dict, *keys: str):
    parts = [values.get(key) for key in keys]
    if any(value is None for value in parts):
        return None
    return sum(parts)

def parse_size_fields(text: str, wanted: tuple | list) -> tuple[dict, list[str]]:
    values = dict.fromkeys(wanted, None)
    reasons = []
    for line in text.splitlines():
        key, separator, raw = line.partition(":")
        key = key.strip()
        if not separator or key not in values:
            continue
        parts = raw.split()
        try:
            if len(parts) != 2 or parts[1] != "kB":
                raise ValueError("expected integer kB value")
            value = int(parts[0])
            if value < 0:
                raise ValueError("negative size")
        except ValueError as error:
            values[key] = None
            reasons.append(f"{key}: {error}")
        else:
            values[key] = value
    for key, value in values.items():
        if value is None and not any(reason.startswith(f"{key}:") for reason in reasons):
            reasons.append(f"{key}: unavailable")
    return values, reasons

def read_process_start_ticks(process_dir: Path) -> int:
    text = (process_dir / "stat").read_text()
    _, separator, tail = text.rpartition(") ")
    if not separator:
        raise ValueError("invalid process stat header")
    fields = tail.split()  # Starts at stat field 3, the process state.
    if len(fields) <= 19:
        raise ValueError("truncated stat fields")
    return int(fields[19])  # Field 22 is the process start time.

def parse_smaps_mappings(text: str) -> tuple[list[dict], list[str]]:
    vmas = []
    current_label = None
    current_lines = []
    reasons = []

    for line in text.splitlines():
        line_stripped = line.strip()
        if not line_stripped:
            continue
        parts = line.split(None, 5)
        # Check if line is a VMA header
        if len(parts) >= 5 and "-" in parts[0] and ":" not in parts[0]:
            addr_range = parts[0].split("-")
            if len(addr_range) == 2 and all(c in "0123456789abcdefABCDEF" for c in addr_range[0]) and all(c in "0123456789abcdefABCDEF" for c in addr_range[1]):
                if current_label is not None:
                    vmas.append((current_label, "\n".join(current_lines)))
                if len(parts) >= 6:
                    label = parts[5].strip()
                    current_label = label if label else "[anon]"
                else:
                    current_label = "[anon]"
                current_lines = []
                continue
        if current_label is not None:
            current_lines.append(line)

    if current_label is not None:
        vmas.append((current_label, "\n".join(current_lines)))

    mapping_fields = ("Rss", "Pss", "Private_Clean", "Private_Dirty")
    groups_data = {}

    for label, body in vmas:
        parsed, vma_reasons = parse_size_fields(body, mapping_fields)
        reasons.extend(vma_reasons)
        rss = parsed.get("Rss")
        pss = parsed.get("Pss")
        priv = sum_if_known(parsed, "Private_Clean", "Private_Dirty")

        entry = groups_data.setdefault(label, {"rss": [], "pss": [], "priv": [], "count": 0})
        entry["count"] += 1
        entry["rss"].append(rss)
        entry["pss"].append(pss)
        entry["priv"].append(priv)

    result = []
    for label, entry in groups_data.items():
        rss_list = entry["rss"]
        pss_list = entry["pss"]
        priv_list = entry["priv"]

        rss_total = None if any(x is None for x in rss_list) else sum(rss_list)
        pss_total = None if any(x is None for x in pss_list) else sum(pss_list)
        priv_total = None if any(x is None for x in priv_list) else sum(priv_list)

        result.append({
            "label": label,
            "mapping_count": entry["count"],
            "rss_kib": rss_total,
            "pss_kib": pss_total,
            "private_rss_kib": priv_total,
        })

    result.sort(
        key=lambda g: (g["rss_kib"] is not None, g["rss_kib"] if g["rss_kib"] is not None else -1),
        reverse=True
    )
    return result, reasons

def read_memory_snapshot(pid: int, *, proc_root: Path = Path("/proc"), include_mappings: bool = False) -> dict:
    proc_root = Path(proc_root)
    process_dir = proc_root / str(pid)

    report = {
        "schema_version": 1,
        "kind": "process_memory",
        "pid": pid,
        "source": "smaps_rollup",
        "captured_unix_ns": time.time_ns(),
        "process_start_ticks": None,
        "command": None,
        "executable": None,
        "rss_kib": None,
        "pss_kib": None,
        "private_rss_kib": None,
        "shared_rss_kib": None,
        "pss_anon_kib": None,
        "pss_file_kib": None,
        "pss_shmem_kib": None,
        "swap_kib": None,
        "private_hugetlb_kib": None,
        "mapping_groups": None,
        "unavailable": [],
    }

    start_ticks_before = None
    try:
        start_ticks_before = read_process_start_ticks(process_dir)
        report["process_start_ticks"] = start_ticks_before
    except Exception as e:
        report["unavailable"].append(f"process_identity: {e}")

    try:
        cmdline_raw = (process_dir / "cmdline").read_bytes()
        if cmdline_raw:
            parts = [p for p in cmdline_raw.decode("utf-8", errors="replace").split("\x00") if p]
            if parts:
                report["command"] = " ".join(parts)
    except Exception:
        pass

    if not report["command"]:
        try:
            stat_text = (process_dir / "stat").read_text()
            head, sep, _ = stat_text.rpartition(") ")
            if sep:
                report["command"] = head.partition("(")[2]
        except Exception:
            pass

    try:
        exe_path = process_dir / "exe"
        if exe_path.is_symlink():
            report["executable"] = os.readlink(exe_path)
        elif exe_path.exists():
            report["executable"] = str(exe_path)
    except Exception:
        pass

    rollup_fields = (
        "Rss",
        "Pss",
        "Private_Clean",
        "Private_Dirty",
        "Shared_Clean",
        "Shared_Dirty",
        "Pss_Anon",
        "Pss_File",
        "Pss_Shmem",
        "Swap",
        "Private_Hugetlb",
    )

    try:
        rollup_text = (process_dir / "smaps_rollup").read_text()
        parsed, reasons = parse_size_fields(rollup_text, rollup_fields)
        report["unavailable"].extend(reasons)
        report["rss_kib"] = parsed.get("Rss")
        report["pss_kib"] = parsed.get("Pss")
        report["private_rss_kib"] = sum_if_known(parsed, "Private_Clean", "Private_Dirty")
        report["shared_rss_kib"] = sum_if_known(parsed, "Shared_Clean", "Shared_Dirty")
        report["pss_anon_kib"] = parsed.get("Pss_Anon")
        report["pss_file_kib"] = parsed.get("Pss_File")
        report["pss_shmem_kib"] = parsed.get("Pss_Shmem")
        report["swap_kib"] = parsed.get("Swap")
        report["private_hugetlb_kib"] = parsed.get("Private_Hugetlb")
    except FileNotFoundError:
        report["unavailable"].append("smaps_rollup: process or file does not exist")
    except PermissionError:
        report["unavailable"].append("smaps_rollup: permission denied")
    except Exception as e:
        report["unavailable"].append(f"smaps_rollup: {e}")

    if include_mappings:
        try:
            smaps_text = (process_dir / "smaps").read_text()
            groups, mapping_reasons = parse_smaps_mappings(smaps_text)
            report["mapping_groups"] = groups
            report["unavailable"].extend(mapping_reasons)
        except FileNotFoundError:
            report["unavailable"].append("smaps: process or file does not exist")
        except PermissionError:
            report["unavailable"].append("smaps: permission denied")
        except Exception as e:
            report["unavailable"].append(f"smaps: {e}")

    if start_ticks_before is None:
        report["rss_kib"] = None
        report["pss_kib"] = None
        report["private_rss_kib"] = None
        report["shared_rss_kib"] = None
        report["pss_anon_kib"] = None
        report["pss_file_kib"] = None
        report["pss_shmem_kib"] = None
        report["swap_kib"] = None
        report["private_hugetlb_kib"] = None
        report["mapping_groups"] = None
    else:
        try:
            start_ticks_after = read_process_start_ticks(process_dir)
            if start_ticks_after != start_ticks_before:
                raise ValueError(f"process start ticks changed from {start_ticks_before} to {start_ticks_after}")
        except Exception as e:
            report["unavailable"].append(f"process_identity: {e}")
            report["process_start_ticks"] = None
            report["rss_kib"] = None
            report["pss_kib"] = None
            report["private_rss_kib"] = None
            report["shared_rss_kib"] = None
            report["pss_anon_kib"] = None
            report["pss_file_kib"] = None
            report["pss_shmem_kib"] = None
            report["swap_kib"] = None
            report["private_hugetlb_kib"] = None
            report["mapping_groups"] = None

    return report

# --- System and Hardware Monitor ---

class SystemMonitor:
    def __init__(self, target_pid: int = None, poll_interval: float = 0.5):
        self.target_pid = target_pid
        self.poll_interval = poll_interval
        self._stop_event = threading.Event()
        self._thread = None

        self.power_samples = []
        self.gpu_busy_samples = []
        self.rss_samples = []

        self.initial_cpu = 0.0
        self.final_cpu = 0.0
        self.initial_switches = 0
        self.final_switches = 0
        self.final_fd_count = 0

    def _sample_power_w(self):
        try:
            with open('/sys/class/power_supply/BATT/voltage_now') as f:
                v = float(f.read().strip()) / 1e6
            with open('/sys/class/power_supply/BATT/current_now') as f:
                c = float(f.read().strip()) / 1e6
            return v * c
        except Exception:
            return None

    def _sample_gpu_busy(self):
        try:
            with open('/sys/class/drm/card1/device/gpu_busy_percent') as f:
                return int(f.read().strip())
        except Exception:
            return None

    def _sample_proc(self):
        if not self.target_pid:
            return 0.0, None, 0, 0
        cpu_s = 0.0
        rss_kb = None
        switches = 0
        fd_count = 0

        try:
            with open(f'/proc/{self.target_pid}/stat') as f:
                parts = f.read().split()
                ticks = os.sysconf('SC_CLK_TCK')
                cpu_s = (int(parts[13]) + int(parts[14])) / ticks
        except Exception:
            pass

        try:
            with open(f'/proc/{self.target_pid}/status') as f:
                for line in f:
                    if line.startswith('VmRSS:'):
                        rss_kb = int(line.split()[1])
                    elif line.startswith('voluntary_ctxt_switches:') or line.startswith('nonvoluntary_ctxt_switches:'):
                        switches += int(line.split()[1])
        except Exception:
            pass

        try:
            fd_count = len(os.listdir(f'/proc/{self.target_pid}/fd'))
        except Exception:
            pass

        return cpu_s, rss_kb, switches, fd_count

    def start(self):
        if self.target_pid:
            cpu, rss, sw, fd = self._sample_proc()
            self.initial_cpu = cpu
            self.initial_switches = sw
            if rss is not None:
                self.rss_samples.append(rss)

        def monitor_loop():
            while not self._stop_event.is_set():
                p = self._sample_power_w()
                if p is not None:
                    self.power_samples.append(p)

                g = self._sample_gpu_busy()
                if g is not None:
                    self.gpu_busy_samples.append(g)

                if self.target_pid:
                    cpu, rss, sw, fd = self._sample_proc()
                    if rss is not None:
                        self.rss_samples.append(rss)
                    self.final_cpu = cpu
                    self.final_switches = sw
                    self.final_fd_count = fd

                time.sleep(self.poll_interval)

        self._thread = threading.Thread(target=monitor_loop, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop_event.set()
        if self._thread:
            self._thread.join(timeout=1.0)

    @property
    def avg_power_w(self):
        return round(sum(self.power_samples) / len(self.power_samples), 2) if self.power_samples else None

    @property
    def avg_gpu_busy(self):
        return round(sum(self.gpu_busy_samples) / len(self.gpu_busy_samples), 1) if self.gpu_busy_samples else None

    @property
    def max_rss_kb(self):
        return max(self.rss_samples) if self.rss_samples else None

    @property
    def cpu_seconds(self):
        return round(max(0.0, self.final_cpu - self.initial_cpu), 3) if self.target_pid else None

    @property
    def total_context_switches(self):
        return max(0, self.final_switches - self.initial_switches) if self.target_pid else None

    @property
    def total_wakeups(self):
        """Deprecated alias for total_context_switches (voluntary + involuntary OS context switches)."""
        return self.total_context_switches

# --- i3 Helpers ---

def get_i3_workspaces(display: str) -> list[dict]:
    try:
        res = subprocess.run(
            ["i3-msg", "-t", "get_workspaces"],
            capture_output=True, text=True, check=True,
            env={**os.environ, "DISPLAY": display}
        )
        return json.loads(res.stdout)
    except Exception:
        return []

_last_switched_ws = {}

def switch_i3_workspace(display: str, workspace_name: str):
    if _last_switched_ws.get(display) == workspace_name:
        return
    try:
        subprocess.run(
            ["i3-msg", f"workspace {workspace_name}"],
            capture_output=True, text=True, check=True,
            env={**os.environ, "DISPLAY": display}
        )
        _last_switched_ws[display] = workspace_name
    except Exception as e:
        print(f"Warning: i3-msg switch failed: {e}", file=sys.stderr)

# --- Idle Assertion ---

def assert_idle(before: dict, after: dict):
    if before["pid"] != after["pid"]:
        raise AssertionError("compositor restarted during idle measurement")
    for key in ("render_attempts", "successful_swaps", "full_repaints", "partial_repaints"):
        delta = after.get(key, 0) - before.get(key, 0)
        if delta != 0:
            raise AssertionError(f"idle {key} changed by {delta}")

# --- Verification Scenarios ---

def test_selection_collision(binary: str, display: str) -> bool:
    print("  [Test] Selection ownership and refusal of duplicate compositor...")
    s1 = SlopcomProcess(binary, display)
    owner1 = s1.start()
    assert owner1 != 0, "First slopcom must acquire selection"

    # Attempt to start a second instance
    s2 = subprocess.run(
        [binary, display],
        capture_output=True, text=True,
        env={**os.environ, "DISPLAY": display}
    )
    assert s2.returncode == 1, f"Second compositor must fail with exit 1, got {s2.returncode}"
    assert "already owns screen" in s2.stderr or "Another compositing manager" in s2.stderr, (
        f"Expected refusal error message in stderr: {s2.stderr}"
    )

    # First instance must still be alive and own the selection
    assert s1.proc.poll() is None, "First compositor must remain running"
    owner_now = get_selection_owner(display)
    assert owner_now == owner1, "First compositor retains selection ownership"

    rc, snap = s1.stop()
    assert rc == 0, f"First compositor must exit cleanly with 0, got {rc}"
    assert get_selection_owner(display) == 0, "Selection must be released to None on exit"
    print("  [Pass] Selection ownership and refusal confirmed.")
    return True

def test_start_stop_cycles(binary: str, display: str, count: int = 105) -> bool:
    print(f"  [Test] Executing {count} rapid start/stop cycles...")
    t0 = time.time()
    for i in range(count):
        s = SlopcomProcess(binary, display)
        owner = s.start(timeout=3.0)
        assert owner != 0, f"Cycle {i+1}: Failed to acquire selection"
        rc, exit_snap = s.stop(timeout=3.0)
        assert rc == 0, f"Cycle {i+1}: Unexpected exit code {rc}"
        assert get_selection_owner(display) == 0, f"Cycle {i+1}: Selection not released"
        if exit_snap:
            for res in ("live_pixmaps", "live_images", "live_textures", "live_damage", "live_regions"):
                val = exit_snap.get(res, 0)
                assert val == 0, f"Cycle {i+1}: Resource leak in {res} = {val}"
        if (i + 1) % 25 == 0 or i + 1 == count:
            print(f"    Completed {i + 1}/{count} cycles (elapsed {time.time() - t0:.1f}s)")
    print(f"  [Pass] All {count} start/stop cycles passed with 0 leaks.")
    return True

def test_fixture_scenarios(binary: str, fixture_bin: str, display: str, quick: bool = False) -> bool:
    print("  [Test] Real-X functional checks (managed, ARGB, GL producer, bounding shape, live opacity)...")
    s = SlopcomProcess(binary, display)
    s.start()

    dur_args = ["--frames", "15"] if quick else ["--duration", "2"]
    dur_long_args = ["--frames", "30"] if quick else ["--duration", "3"]

    try:
        # 1. Managed RGB
        print("    Running managed RGB window...")
        subprocess.run([fixture_bin, "--mode", "rgb", "--managed"] + dur_args,
                       check=True, env={**os.environ, "DISPLAY": display})

        # 2. Managed ARGB translucent
        print("    Running managed ARGB translucent window...")
        subprocess.run([fixture_bin, "--mode", "argb", "--managed", "--opacity", "0.75"] + dur_args,
                       check=True, env={**os.environ, "DISPLAY": display})

        # 3. GL Producer
        print("    Running GL direct-rendered producer...")
        subprocess.run([fixture_bin, "--mode", "gl"] + dur_args,
                       check=True, env={**os.environ, "DISPLAY": display})

        # 4. Bounding shape with center hole
        print("    Running bounding shape with center hole...")
        subprocess.run([fixture_bin, "--mode", "both", "--shape-hole"] + dur_args,
                       check=True, env={**os.environ, "DISPLAY": display})

        # 5. Live shape and opacity cycling
        print("    Running live shape and opacity cycling...")
        subprocess.run([fixture_bin, "--mode", "both", "--shape-cycle", "--opacity-cycle"] + dur_long_args,
                       check=True, env={**os.environ, "DISPLAY": display})

        snap = s.query_snapshot()
        print(f"    Mid-test snapshot: renders={snap.get('render_attempts')}, swaps={snap.get('successful_swaps')}")
        assert snap.get("render_attempts", 0) > 0, "Compositor must have rendered frames"
        assert snap.get("successful_swaps", 0) > 0, "Compositor must have swapped frames"

    finally:
        rc, exit_snap = s.stop()
        assert rc == 0, f"Compositor exited with {rc}"
        if exit_snap:
            for res in ("live_pixmaps", "live_images", "live_textures", "live_damage", "live_regions"):
                val = exit_snap.get(res, 0)
                assert val == 0, f"Resource leak in {res} = {val}"

    print("  [Pass] Real-X fixture scenarios passed.")
    return True

def test_scripted_churn(binary: str, fixture_bin: str, display: str, duration_sec: int) -> bool:
    print(f"  [Test] Scripted churn and i3 workspace switching for {duration_sec}s...")
    workspaces = get_i3_workspaces(display)
    initial_ws = None
    for ws in workspaces:
        if ws.get("focused"):
            initial_ws = ws.get("name")
            break

    s = SlopcomProcess(binary, display)
    s.start()

    try:
        t_start = time.time()
        churn_proc = subprocess.Popen(
            [fixture_bin, "--mode", "churn", "--duration", str(duration_sec)],
            env={**os.environ, "DISPLAY": display}
        )

        ws_cycle = ["1", "2", "3", "4"]
        ws_idx = 0
        switch_interval = 1.0 if duration_sec <= 10 else 5.0
        while churn_proc.poll() is None:
            time.sleep(switch_interval)
            target_ws = ws_cycle[ws_idx % len(ws_cycle)]
            switch_i3_workspace(display, target_ws)
            ws_idx += 1

            try:
                snap = s.query_snapshot(timeout=2.0)
                elapsed = int(time.time() - t_start)
                print(f"    [{elapsed}s / {duration_sec}s] slopcom live: "
                      f"swaps={snap.get('successful_swaps')}, "
                      f"pixmaps={snap.get('live_pixmaps')}, "
                      f"textures={snap.get('live_textures')}")
            except Exception as e:
                print(f"    Warning querying snapshot: {e}", file=sys.stderr)

        churn_proc.wait()
        assert churn_proc.returncode == 0, "Fixture churn process failed"

    finally:
        if initial_ws:
            print(f"    Restoring focused workspace {initial_ws}...")
            switch_i3_workspace(display, initial_ws)

        rc, exit_snap = s.stop()
        assert rc == 0, f"Compositor exited with error {rc}"
        if exit_snap:
            for res in ("live_pixmaps", "live_images", "live_textures", "live_damage", "live_regions"):
                val = exit_snap.get(res, 0)
                assert val == 0, f"Teardown resource leak: {res} = {val}"

    print(f"  [Pass] Scripted churn ({duration_sec}s) passed with 0 crashes or resource leaks.")
    return True

def test_i3_interactions(binary: str, fixture_bin: str, display: str) -> bool:
    print("  [Test] Real-world i3 interactions (floating, fullscreen, scratchpad, workspace, focus)...")
    original_ws = None
    for ws in get_i3_workspaces(display):
        if ws.get("focused"):
            original_ws = ws.get("name")
            break

    switch_i3_workspace(display, "bench")
    time.sleep(0.5)

    s = SlopcomProcess(binary, display)
    s.start()

    fixture_proc = None
    try:
        snap0 = s.query_snapshot()
        base_pixmaps = snap0.get("live_pixmaps", 0)

        # 1. Start managed window (tiled in i3)
        print("    [1/6] Launching managed tiled window...")
        fixture_proc = subprocess.Popen(
            [fixture_bin, "--mode", "rgb", "--managed", "--duration", "60"],
            env={**os.environ, "DISPLAY": display}
        )
        time.sleep(1.0)
        snap1 = s.query_snapshot()
        assert snap1.get("live_pixmaps", 0) >= base_pixmaps + 1, "Managed window must be captured"
        assert snap1.get("successful_swaps", 0) > 0, "Managed window must trigger composite render"

        # 2. Toggle floating
        print("    [2/6] Toggling floating mode...")
        subprocess.run(["i3-msg", "floating toggle"], check=True,
                       env={**os.environ, "DISPLAY": display}, stdout=subprocess.DEVNULL)
        time.sleep(0.5)
        snap2 = s.query_snapshot()
        assert snap2.get("successful_swaps", 0) >= snap1.get("successful_swaps", 0)

        # 3. Toggle fullscreen
        print("    [3/6] Toggling fullscreen mode...")
        subprocess.run(["i3-msg", "fullscreen toggle"], check=True,
                       env={**os.environ, "DISPLAY": display}, stdout=subprocess.DEVNULL)
        time.sleep(0.5)
        # Toggle back from fullscreen
        subprocess.run(["i3-msg", "fullscreen toggle"], check=True,
                       env={**os.environ, "DISPLAY": display}, stdout=subprocess.DEVNULL)
        time.sleep(0.5)

        # 4. Scratchpad move and show
        print("    [4/6] Exercising scratchpad move and show...")
        subprocess.run(["i3-msg", "move scratchpad"], check=True,
                       env={**os.environ, "DISPLAY": display}, stdout=subprocess.DEVNULL)
        time.sleep(0.5)
        snap_scratch = s.query_snapshot()
        assert snap_scratch.get("live_pixmaps", 0) < snap1.get("live_pixmaps", 0), "Unmapped scratchpad window must release pixmap"
        subprocess.run(["i3-msg", "scratchpad show"], check=True,
                       env={**os.environ, "DISPLAY": display}, stdout=subprocess.DEVNULL)
        time.sleep(0.5)
        snap_restore = s.query_snapshot()
        assert snap_restore.get("live_pixmaps", 0) >= snap1.get("live_pixmaps", 0), "Restored scratchpad window must reimport"

        # 5. Workspace migration
        print("    [5/6] Migrating window between workspaces...")
        subprocess.run(["i3-msg", "move container to workspace i3_test2"], check=True,
                       env={**os.environ, "DISPLAY": display}, stdout=subprocess.DEVNULL)
        time.sleep(0.5)
        switch_i3_workspace(display, "i3_test2")
        time.sleep(0.5)
        snap_ws2 = s.query_snapshot()
        assert snap_ws2.get("live_pixmaps", 0) >= base_pixmaps + 1, "Window on new workspace must be captured"
        switch_i3_workspace(display, "bench")
        time.sleep(0.5)

        # 6. Clean closing
        print("    [6/6] Closing window and verifying clean release...")
        if fixture_proc.poll() is None:
            fixture_proc.terminate()
            fixture_proc.wait(timeout=2.0)
        time.sleep(0.5)
        snap_end = s.query_snapshot()
        assert snap_end.get("live_pixmaps", 0) <= base_pixmaps, "Closed window must release all pixmaps"

    finally:
        if fixture_proc and fixture_proc.poll() is None:
            fixture_proc.kill()
        if original_ws:
            switch_i3_workspace(display, original_ws)
        rc, exit_snap = s.stop()
        assert rc == 0, f"Compositor exited with {rc}"
        if exit_snap:
            for res in ("live_pixmaps", "live_images", "live_textures", "live_damage", "live_regions"):
                val = exit_snap.get(res, 0)
                assert val == 0, f"Resource leak after i3 interactions in {res} = {val}"

    print("  [Pass] All real-world i3 interaction scenarios passed cleanly.")
    return True

def run_soak_test(binary: str, fixture_bin: str, display: str, duration_sec: int, sample_interval: float = 10.0) -> dict:
    print(f"=== Starting slopcom v0.5 Soak Test on {display} ===")
    print(f"Target duration: {duration_sec}s ({duration_sec / 3600:.2f} hours), sample interval: {sample_interval}s")

    original_ws = None
    for ws in get_i3_workspaces(display):
        if ws.get("focused"):
            original_ws = ws.get("name")
            break

    switch_i3_workspace(display, "bench")
    time.sleep(0.5)

    soak_dir = Path("benchmarks/results/soak")
    soak_dir.mkdir(parents=True, exist_ok=True)
    ts = int(time.time())
    jsonl_path = soak_dir / f"{ts}_soak.jsonl"

    s = SlopcomProcess(binary, display)
    s.start()
    pid = s.proc.pid

    samples = []
    churn_proc = None

    try:
        churn_proc = subprocess.Popen(
            [fixture_bin, "--mode", "churn", "--duration", str(duration_sec + 60)],
            env={**os.environ, "DISPLAY": display}
        )

        t_start = time.time()
        tick = 0
        ws_cycle = ["bench", "soak1", "soak2"]

        with open(jsonl_path, "w") as jf:
            while time.time() - t_start < duration_sec:
                time.sleep(sample_interval)
                now = time.time()
                elapsed = now - t_start
                tick += 1

                if tick % 3 == 0:
                    switch_i3_workspace(display, ws_cycle[(tick // 3) % len(ws_cycle)])

                if s.proc.poll() is not None:
                    raise RuntimeError(f"Compositor died prematurely with code {s.proc.returncode}")

                snap = s.query_snapshot(timeout=3.0)

                cpu_s = 0.0
                rss_kb = 0
                fd_count = 0
                try:
                    with open(f"/proc/{pid}/stat") as f:
                        parts = f.read().split()
                        ticks_per_sec = os.sysconf("SC_CLK_TCK")
                        cpu_s = (int(parts[13]) + int(parts[14])) / ticks_per_sec
                except Exception:
                    pass

                try:
                    with open(f"/proc/{pid}/status") as f:
                        for line in f:
                            if line.startswith("VmRSS:"):
                                rss_kb = int(line.split()[1])
                except Exception:
                    pass

                try:
                    fd_count = len(os.listdir(f"/proc/{pid}/fd"))
                except Exception:
                    pass

                gpu_busy = None
                try:
                    gpu_busy = int(open("/sys/class/drm/card1/device/gpu_busy_percent").read().strip())
                except Exception:
                    pass

                sample_entry = {
                    "timestamp": now,
                    "elapsed_s": round(elapsed, 1),
                    "cpu_seconds": round(cpu_s, 3),
                    "rss_kb": rss_kb,
                    "fd_count": fd_count,
                    "gpu_busy_pct": gpu_busy,
                    "render_attempts": snap.get("render_attempts"),
                    "successful_swaps": snap.get("successful_swaps"),
                    "live_pixmaps": snap.get("live_pixmaps"),
                    "live_textures": snap.get("live_textures"),
                    "live_images": snap.get("live_images"),
                    "live_damage": snap.get("live_damage"),
                    "live_regions": snap.get("live_regions")
                }
                samples.append(sample_entry)
                jf.write(json.dumps(sample_entry) + "\n")
                jf.flush()

                print(f"[{elapsed:.0f}s / {duration_sec}s] "
                      f"swaps={sample_entry['successful_swaps']} | "
                      f"RSS={rss_kb}KB | "
                      f"FDs={fd_count} | "
                      f"CPU={cpu_s:.2f}s | "
                      f"live_pix={sample_entry['live_pixmaps']} | "
                      f"live_tex={sample_entry['live_textures']}")

    finally:
        if churn_proc and churn_proc.poll() is None:
            churn_proc.terminate()
            try:
                churn_proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                churn_proc.kill()

        if original_ws:
            switch_i3_workspace(display, original_ws)

        rc, exit_snap = s.stop()
        assert rc == 0, f"Compositor failed to exit cleanly: returncode {rc}"
        if exit_snap:
            for res in ("live_pixmaps", "live_images", "live_textures", "live_damage", "live_regions"):
                val = exit_snap.get(res, 0)
                assert val == 0, f"Resource leak detected on exit: {res} = {val}"

    if len(samples) >= 3:
        n = len(samples)
        t_vals = [s["elapsed_s"] for s in samples]
        rss_vals = [s["rss_kb"] for s in samples]
        fd_vals = [s["fd_count"] for s in samples]

        mean_t = sum(t_vals) / n
        mean_rss = sum(rss_vals) / n
        denom = sum((t - mean_t) ** 2 for t in t_vals)
        rss_slope = sum((t - mean_t) * (rss - mean_rss) for t, rss in zip(t_vals, rss_vals)) / denom if denom > 0 else 0.0

        fd_delta = max(fd_vals) - min(fd_vals)
    else:
        rss_slope = 0.0
        fd_delta = 0

    summary = {
        "timestamp": ts,
        "duration_sec": duration_sec,
        "samples_count": len(samples),
        "rss_initial_kb": samples[0]["rss_kb"] if samples else 0,
        "rss_final_kb": samples[-1]["rss_kb"] if samples else 0,
        "rss_slope_kb_per_s": round(rss_slope, 4),
        "fd_delta": fd_delta,
        "total_swaps": samples[-1]["successful_swaps"] if samples else 0,
        "clean_shutdown": True
    }

    summary_path = soak_dir / f"{ts}_summary.json"
    with open(summary_path, "w") as sf:
        json.dump(summary, sf, indent=2)

    print(f"\nSoak test completed ({len(samples)} samples recorded in {jsonl_path}).")
    print(f"Memory analysis: initial={summary['rss_initial_kb']}KB, final={summary['rss_final_kb']}KB, slope={rss_slope:.4f} KB/s")
    print(f"FD analysis: delta={fd_delta} (no steady FD leak)")

    assert rss_slope < 2.0, f"Steady RSS growth rate ({rss_slope:.4f} KB/s) exceeded tolerance"
    assert fd_delta <= 5, f"FD count varied excessively ({fd_delta})"

    return summary

# --- Benchmark Engine ---

def run_single_workload(
    backend: str,
    workload: str,
    repeat: int,
    settle_sec: int,
    sample_sec: int,
    display: str,
    binary: str,
    fixture_bin: str,
    capture_memory: bool = False
) -> dict:
    total_time = settle_sec + sample_sec + 300

    record = {
        "backend": backend,
        "workload": workload,
        "repeat": repeat,
        "sample_seconds": sample_sec,
        "settle_seconds": settle_sec,
        "build": "release",
        "render_delta": None,
        "swap_delta": None,
        "skipped_delta": None,
        "skipped_occluded_delta": None,
        "full_repaints_delta": None,
        "partial_repaints_delta": None,
        "poll_wakeups_delta": None,
        "x_round_trips_delta": None,
        "cpu_seconds": None,
        "max_rss_kb": None,
        "memory_before": None,
        "memory_after": None,
        "context_switches": None,
        "wakeups": None,
        "system_power_w": None,
        "gpu_busy_pct": None,
        "timing_cpu_ns": None,
        "unavailable": []
    }

    if backend == "picom":
        if not shutil.which("picom"):
            record["unavailable"].append("picom binary is not installed on this system")
            return record

    switch_i3_workspace(display, "bench")
    time.sleep(0.5)

    compositor_proc = None
    target_pid = None

    try:
        # Start backend
        if backend == "slopcom":
            compositor_proc = SlopcomProcess(binary, display)
            compositor_proc.start()
            target_pid = compositor_proc.proc.pid
        elif backend == "slopcom-full":
            compositor_proc = SlopcomProcess(binary, display, extra_args=["--full-redraw"])
            compositor_proc.start()
            target_pid = compositor_proc.proc.pid
        elif backend == "picom":
            proc = subprocess.Popen(["picom", "--backend", "egl", "--vsync"],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    env={**os.environ, "DISPLAY": display})
            compositor_proc = proc
            target_pid = proc.pid
            time.sleep(1.0)
        elif backend == "none":
            record["unavailable"].append("transparency rendered without composition (direct X11)")

        # Start fixture workload if not empty-idle
        fixture_proc = None
        if workload == "overlapping-idle":
            fixture_proc = subprocess.Popen([fixture_bin, "--workload", "idle", "--duration", str(total_time)],
                                            env={**os.environ, "DISPLAY": display})
        elif workload == "typing":
            fixture_proc = subprocess.Popen([fixture_bin, "--workload", "typing", "--duration", str(total_time)],
                                            env={**os.environ, "DISPLAY": display})
        elif workload == "drag-resize":
            fixture_proc = subprocess.Popen([fixture_bin, "--workload", "drag-resize", "--duration", str(total_time)],
                                            env={**os.environ, "DISPLAY": display})
        elif workload == "scrolling":
            fixture_proc = subprocess.Popen([fixture_bin, "--workload", "scrolling", "--duration", str(total_time)],
                                            env={**os.environ, "DISPLAY": display})
        elif workload == "video":
            fixture_proc = subprocess.Popen([fixture_bin, "--workload", "video", "--duration", str(total_time)],
                                            env={**os.environ, "DISPLAY": display})

        # Settle
        time.sleep(settle_sec)
        switch_i3_workspace(display, "bench")

        # Memory snapshot before
        if capture_memory and target_pid is not None:
            record["memory_before"] = read_memory_snapshot(target_pid)

        # Pre-sample snapshot
        snap_before = None
        if backend in ("slopcom", "slopcom-full"):
            snap_before = compositor_proc.query_snapshot()

        # Start monitor with adaptive polling
        poll_interval = min(0.5, max(0.1, sample_sec / 4))
        monitor = SystemMonitor(target_pid=target_pid, poll_interval=poll_interval)
        monitor.start()

        # Measurement window
        time.sleep(sample_sec)

        # Stop monitor
        monitor.stop()

        # Memory snapshot after
        if capture_memory and target_pid is not None:
            record["memory_after"] = read_memory_snapshot(target_pid)

        # Post-sample snapshot
        snap_after = None
        if backend in ("slopcom", "slopcom-full"):
            snap_after = compositor_proc.query_snapshot()

        # Check idle invariant if applicable
        if workload in ("empty-idle", "overlapping-idle") and backend in ("slopcom", "slopcom-full"):
            assert_idle(snap_before, snap_after)

        # Compute deltas
        if snap_before and snap_after:
            record["render_delta"] = snap_after.get("render_attempts", 0) - snap_before.get("render_attempts", 0)
            record["swap_delta"] = snap_after.get("successful_swaps", 0) - snap_before.get("successful_swaps", 0)
            record["skipped_delta"] = snap_after.get("skipped_presentations", 0) - snap_before.get("skipped_presentations", 0)
            record["skipped_occluded_delta"] = snap_after.get("skipped_occluded_damage", 0) - snap_before.get("skipped_occluded_damage", 0)
            record["full_repaints_delta"] = snap_after.get("full_repaints", 0) - snap_before.get("full_repaints", 0)
            record["partial_repaints_delta"] = snap_after.get("partial_repaints", 0) - snap_before.get("partial_repaints", 0)
            record["poll_wakeups_delta"] = snap_after.get("poll_wakeups", 0) - snap_before.get("poll_wakeups", 0)
            record["x_round_trips_delta"] = snap_after.get("x_round_trips", 0) - snap_before.get("x_round_trips", 0)
            record["timing_cpu_ns"] = snap_after.get("timing_cpu_ns")

        record["cpu_seconds"] = monitor.cpu_seconds
        record["max_rss_kb"] = monitor.max_rss_kb
        record["context_switches"] = monitor.total_context_switches
        record["wakeups"] = monitor.total_context_switches
        record["system_power_w"] = monitor.avg_power_w
        record["gpu_busy_pct"] = monitor.avg_gpu_busy

    finally:
        if fixture_proc and fixture_proc.poll() is None:
            fixture_proc.terminate()
            try:
                fixture_proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                fixture_proc.kill()

        if backend in ("slopcom", "slopcom-full") and compositor_proc:
            compositor_proc.stop()
        elif backend == "picom" and compositor_proc and compositor_proc.poll() is None:
            compositor_proc.terminate()
            try:
                compositor_proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                compositor_proc.kill()

    return record

# --- Main CLI ---

def main():
    parser = argparse.ArgumentParser(description="slopcom benchmark and verification runner")
    subparsers = parser.add_subparsers(dest="command", required=True)

    # memory
    memory_p = subparsers.add_parser("memory", help="Read one process memory snapshot")
    memory_p.add_argument("--pid", type=positive_pid, required=True, help="Process ID to inspect")
    memory_p.add_argument("--mappings", action="store_true", help="Also summarize mapping attribution")

    # verify
    verify_p = subparsers.add_parser("verify", help="Run v0.2 reliability verification")
    verify_p.add_argument("--display", default=os.environ.get("DISPLAY", ":1"), help="X display")
    verify_p.add_argument("--binary", default="./build/release/slopcom", help="Path to slopcom binary")
    verify_p.add_argument("--fixture", default="./build/release/fixture", help="Path to fixture binary")
    verify_p.add_argument("--test", choices=["all", "collision", "cycles", "fixtures", "churn", "i3"], default="all",
                          help="Run specific verification test (default: all)")
    verify_p.add_argument("--cycles", type=int, default=105, help="Number of start/stop cycles")
    verify_p.add_argument("--churn-duration", type=int, default=600, help="Churn duration in seconds (default 600s = 10m)")
    verify_p.add_argument("--quick", action="store_true", help="Quick run (10 cycles, 5s churn, fast fixtures, ~8s total)")

    # bench
    bench_p = subparsers.add_parser("bench", help="Run v0.3 resource and power baseline benchmarks")
    bench_p.add_argument("--display", default=os.environ.get("DISPLAY", ":1"), help="X display")
    bench_p.add_argument("--binary", default="./build/release/slopcom", help="Path to slopcom binary")
    bench_p.add_argument("--fixture", default="./build/release/fixture", help="Path to fixture binary")
    bench_p.add_argument("--backends", nargs="+", default=["none", "slopcom", "picom"], help="Backends to benchmark")
    bench_p.add_argument("--workloads", nargs="+",
                         default=["empty-idle", "overlapping-idle", "typing", "drag-resize", "scrolling", "video"],
                         help="Workloads to test")
    bench_p.add_argument("--repetitions", type=int, default=3, help="Repetitions per workload/backend (default 3)")
    bench_p.add_argument("--settle-seconds", type=float, default=10, help="Settle seconds before sample (default 10)")
    bench_p.add_argument("--sample-seconds", type=float, default=30, help="Sample seconds per run (default 30)")
    bench_p.add_argument("--smoke", action="store_true", help="Ultra-fast smoke test (1 rep, 0.5s settle, 1.5s sample, ~20s total)")
    bench_p.add_argument("--quick", action="store_true", help="Quick run (1 rep, 1s settle, 3s sample, ~45s total)")
    bench_p.add_argument("--fast", action="store_true", help="Fast benchmark (1 rep, 2s settle, 5s sample, ~1m total)")
    bench_p.add_argument("--gate", action="store_true", help="Formal 60s gate (3 reps, 30s settle, 60s sample)")
    bench_p.add_argument("--memory", action="store_true", help="Record memory snapshots before and after sample window")

    # soak
    soak_p = subparsers.add_parser("soak", help="Run v0.5 long-run soak test with memory and resource monitoring")
    soak_p.add_argument("--display", default=os.environ.get("DISPLAY", ":1"), help="X display")
    soak_p.add_argument("--binary", default="./build/release/slopcom", help="Path to slopcom binary")
    soak_p.add_argument("--fixture", default="./build/release/fixture", help="Path to fixture binary")
    soak_p.add_argument("--duration", type=int, default=14400, help="Soak duration in seconds (default 14400 = 4 hours)")
    soak_p.add_argument("--sample-interval", type=float, default=10.0, help="Sampling interval in seconds (default 10s)")
    soak_p.add_argument("--quick", action="store_true", help="Quick soak test (60s, 2s sample interval)")

    args = parser.parse_args()

    if args.command == "memory":
        report = read_memory_snapshot(args.pid, include_mappings=args.mappings)
        print(json.dumps(report, allow_nan=False))
        return 0 if report["rss_kib"] is not None else 1

    display = args.display
    initial_owner = get_selection_owner(display)
    if initial_owner != 0:
        print(f"Error: Another compositing manager (window 0x{initial_owner:x}) already owns _NET_WM_CM on {display}.", file=sys.stderr)
        print("Per requirements, we do not displace an active compositor. Stop it before running verification.", file=sys.stderr)
        sys.exit(1)

    if args.command == "verify":
        if args.quick:
            args.cycles = 10
            args.churn_duration = 5

        print(f"=== Starting slopcom v0.2 Verification on {display} ===")
        results = {
            "timestamp": time.time(),
            "display": display,
            "binary": args.binary,
            "tests": {}
        }

        # Step 1: Selection collision
        if args.test in ("all", "collision"):
            t0 = time.time()
            test_selection_collision(args.binary, display)
            results["tests"]["selection_collision"] = {"status": "pass", "duration_sec": time.time() - t0}

        # Step 2: Start/Stop cycles
        if args.test in ("all", "cycles"):
            t0 = time.time()
            test_start_stop_cycles(args.binary, display, count=args.cycles)
            results["tests"]["start_stop_cycles"] = {"status": "pass", "cycles": args.cycles, "duration_sec": time.time() - t0}

        # Step 3: Real-X Fixtures
        if args.test in ("all", "fixtures"):
            t0 = time.time()
            test_fixture_scenarios(args.binary, args.fixture, display, quick=args.quick)
            results["tests"]["fixture_scenarios"] = {"status": "pass", "duration_sec": time.time() - t0}

        # Step 4: Churn
        if args.test in ("all", "churn"):
            t0 = time.time()
            test_scripted_churn(args.binary, args.fixture, display, duration_sec=args.churn_duration)
            results["tests"]["scripted_churn"] = {"status": "pass", "duration_sec": time.time() - t0}

        # Step 5: i3 Interactions
        if args.test in ("all", "i3"):
            t0 = time.time()
            test_i3_interactions(args.binary, args.fixture, display)
            results["tests"]["i3_interactions"] = {"status": "pass", "duration_sec": time.time() - t0}

        # Save results
        out_file = Path("benchmarks/results/verify.json")
        out_file.parent.mkdir(parents=True, exist_ok=True)
        with open(out_file, "w") as f:
            json.dump(results, f, indent=2)

        print(f"\nVerification test(s) PASSED. Evidence recorded in {out_file}.")

    elif args.command == "soak":
        duration = 60 if args.quick else args.duration
        sample_interval = 2.0 if args.quick else args.sample_interval
        run_soak_test(
            binary=args.binary,
            fixture_bin=args.fixture,
            display=display,
            duration_sec=duration,
            sample_interval=sample_interval
        )

    elif args.command == "bench":
        original_ws = None
        for ws in get_i3_workspaces(display):
            if ws.get("focused"):
                original_ws = ws.get("name")
                break

        if args.smoke:
            settle_sec = 0.5
            sample_sec = 1.5
            repetitions = 1
        elif args.quick:
            settle_sec = 1.0
            sample_sec = 3.0
            repetitions = 1
        elif args.fast:
            settle_sec = 2.0
            sample_sec = 5.0
            repetitions = 1
        elif args.gate:
            settle_sec = 30.0
            sample_sec = 60.0
            repetitions = 3
        else:
            settle_sec = args.settle_seconds
            sample_sec = args.sample_seconds
            repetitions = args.repetitions

        # Pre-filter backends
        active_backends = []
        unavailable_backends = {}
        for b in args.backends:
            if b == "picom" and not shutil.which("picom"):
                unavailable_backends["picom"] = "picom binary is not installed on this system"
            else:
                active_backends.append(b)

        print(f"=== Starting slopcom v0.3 Benchmarks on {display} ===")
        print(f"Configuration: settle={settle_sec}s, sample={sample_sec}s, reps={repetitions}")
        print(f"Backends: {active_backends}")
        if unavailable_backends:
            for b, reason in unavailable_backends.items():
                print(f"  (Skipping '{b}': {reason})")
        print(f"Workloads: {args.workloads}\n")

        bench_dir = Path("benchmarks/results/bench")
        bench_dir.mkdir(parents=True, exist_ok=True)

        all_records = []
        # Record unavailable markers once
        for b, reason in unavailable_backends.items():
            all_records.append({
                "backend": b,
                "workload": "all",
                "unavailable": [reason]
            })

        try:
            for workload in args.workloads:
                for rep in range(1, repetitions + 1):
                    backends = list(active_backends)
                    if rep % 2 == 0:
                        backends.reverse()

                    for backend in backends:
                        print(f"[{backend}] workload={workload} rep={rep}/{repetitions} (settle {settle_sec}s, sample {sample_sec}s)...", end="", flush=True)
                        t0 = time.time()
                        try:
                            record = run_single_workload(
                                backend=backend,
                                workload=workload,
                                repeat=rep,
                                settle_sec=settle_sec,
                                sample_sec=sample_sec,
                                display=display,
                                binary=args.binary,
                                fixture_bin=args.fixture,
                                capture_memory=args.memory
                            )
                            record["elapsed_wall_s"] = round(time.time() - t0, 1)
                            all_records.append(record)

                            # Write individual sample record
                            ts = int(time.time())
                            sample_path = bench_dir / f"{ts}_{backend}_{workload}_{rep}.json"
                            with open(sample_path, "w") as f:
                                json.dump(record, f, indent=2)

                            # Print summary
                            swaps = record["swap_delta"] if record["swap_delta"] is not None else "-"
                            power = f"{record['system_power_w']}W" if record["system_power_w"] is not None else "-"
                            gpu = f"{record['gpu_busy_pct']}%" if record["gpu_busy_pct"] is not None else "-"
                            cpu = f"{record['cpu_seconds']}s" if record["cpu_seconds"] is not None else "-"
                            rss = f"{record['max_rss_kb']}KB" if record["max_rss_kb"] is not None else "-"
                            repaints = f"{record.get('full_repaints_delta') or 0}f+{record.get('partial_repaints_delta') or 0}p" if record.get("swap_delta") is not None else "-"
                            csw = record["context_switches"] if record["context_switches"] is not None else "-"
                            print(f" DONE | swaps={swaps} | repaints={repaints} | ctx_sw={csw} | power={power} | gpu={gpu} | cpu={cpu} | rss={rss}")
                            if record["unavailable"]:
                                print(f"    (Unavailable: {', '.join(record['unavailable'])})")

                        except Exception as e:
                            print(f" FAILED: {e}")
                            raise
        finally:
            if original_ws:
                switch_i3_workspace(display, original_ws)

        # Save summary
        summary_path = bench_dir / "summary.json"
        with open(summary_path, "w") as f:
            json.dump(all_records, f, indent=2)

        print(f"\nAll benchmark runs completed. Results recorded in {bench_dir}.")

if __name__ == "__main__":
    sys.exit(main())
