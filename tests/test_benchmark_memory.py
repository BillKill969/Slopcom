#!/usr/bin/env python3
"""
tests/test_benchmark_memory.py - Offline regression tests for benchmark memory tooling.
"""

import os
import sys
import json
import tempfile
import runpy
from pathlib import Path
import unittest
from unittest.mock import patch, MagicMock

# Import runner without executing main()
RUNNER_PATH = str(Path(__file__).resolve().parents[1] / "benchmarks" / "run.py")
runner = runpy.run_path(RUNNER_PATH)
mod_globals = runner["main"].__globals__

parse_size_fields = runner["parse_size_fields"]
sum_if_known = runner["sum_if_known"]
read_process_start_ticks = runner["read_process_start_ticks"]
parse_smaps_mappings = runner["parse_smaps_mappings"]
read_memory_snapshot = runner["read_memory_snapshot"]
SystemMonitor = runner["SystemMonitor"]
run_single_workload = runner["run_single_workload"]
main_func = runner["main"]


class TestMemoryParsing(unittest.TestCase):
    """Test pure parsing functions with edge cases."""

    def test_complete_rollup_fields(self):
        text = (
            "Rss: 1024 kB\n"
            "Pss: 512 kB\n"
            "Shared_Clean: 256 kB\n"
            "Shared_Dirty: 128 kB\n"
            "Private_Clean: 500 kB\n"
            "Private_Dirty: 140 kB\n"
            "Pss_Anon: 100 kB\n"
            "Pss_File: 400 kB\n"
            "Pss_Shmem: 12 kB\n"
            "Swap: 0 kB\n"
            "Private_Hugetlb: 0 kB\n"
        )
        wanted = (
            "Rss", "Pss", "Private_Clean", "Private_Dirty",
            "Shared_Clean", "Shared_Dirty", "Pss_Anon",
            "Pss_File", "Pss_Shmem", "Swap", "Private_Hugetlb",
        )
        values, reasons = parse_size_fields(text, wanted)
        self.assertEqual(values["Rss"], 1024)
        self.assertEqual(values["Pss"], 512)
        self.assertEqual(values["Swap"], 0)
        self.assertEqual(values["Private_Hugetlb"], 0)
        self.assertEqual(sum_if_known(values, "Private_Clean", "Private_Dirty"), 640)
        self.assertEqual(sum_if_known(values, "Shared_Clean", "Shared_Dirty"), 384)
        self.assertEqual(reasons, [])

    def test_missing_private_operand_is_not_zero(self):
        fields, reasons = parse_size_fields(
            "Rss: 100 kB\nPrivate_Clean: 0 kB\n",
            ("Rss", "Private_Clean", "Private_Dirty")
        )
        self.assertEqual(fields["Rss"], 100)
        self.assertEqual(fields["Private_Clean"], 0)
        self.assertIsNone(fields["Private_Dirty"])
        self.assertIsNone(sum_if_known(fields, "Private_Clean", "Private_Dirty"))
        self.assertTrue(any("Private_Dirty: unavailable" in r for r in reasons))

    def test_negative_size_is_invalid(self):
        fields, reasons = parse_size_fields("Rss: -100 kB\n", ("Rss",))
        self.assertIsNone(fields["Rss"])
        self.assertTrue(any("negative size" in r for r in reasons))

    def test_wrong_unit_is_invalid(self):
        fields, reasons = parse_size_fields("Rss: 100 MB\n", ("Rss",))
        self.assertIsNone(fields["Rss"])
        self.assertTrue(any("expected integer kB value" in r for r in reasons))

    def test_non_integer_value_is_invalid(self):
        fields, reasons = parse_size_fields("Rss: notanumber kB\n", ("Rss",))
        self.assertIsNone(fields["Rss"])
        self.assertTrue(any("invalid literal" in r or "expected integer" in r for r in reasons))

    def test_sum_if_known_strict(self):
        self.assertEqual(sum_if_known({"a": 10, "b": 20}, "a", "b"), 30)
        self.assertEqual(sum_if_known({"a": 0, "b": 0}, "a", "b"), 0)
        self.assertIsNone(sum_if_known({"a": 10, "b": None}, "a", "b"))
        self.assertIsNone(sum_if_known({"a": 10}, "a", "b"))


class TestMappingAttribution(unittest.TestCase):
    """Test smaps VMA parsing and aggregation."""

    def test_mappings_aggregation(self):
        smaps_sample = (
            # 1. Binary segment 1
            "00400000-00452000 r-xp 00000000 08:01 123456 /opt/my app/bin\n"
            "Rss: 100 kB\n"
            "Pss: 50 kB\n"
            "Private_Clean: 100 kB\n"
            "Private_Dirty: 0 kB\n"
            # 2. Binary segment 2 (same label, path with space)
            "00452000-00460000 rw-p 00052000 08:01 123456 /opt/my app/bin\n"
            "Rss: 50 kB\n"
            "Pss: 50 kB\n"
            "Private_Clean: 0 kB\n"
            "Private_Dirty: 50 kB\n"
            # 3. Anonymous mapping (5 parts, no path)
            "7f000000-7f010000 rw-p 00000000 00:00 0\n"
            "Rss: 200 kB\n"
            "Pss: 200 kB\n"
            "Private_Clean: 0 kB\n"
            "Private_Dirty: 200 kB\n"
            # 4. Heap
            "01a00000-01a20000 rw-p 00000000 00:00 0 [heap]\n"
            "Rss: 80 kB\n"
            "Pss: 80 kB\n"
            "Private_Clean: 0 kB\n"
            "Private_Dirty: 80 kB\n"
        )
        groups, reasons = parse_smaps_mappings(smaps_sample)
        self.assertEqual(reasons, [])
        self.assertEqual(len(groups), 3)

        # Sorted descending by RSS: [anon] (200), /opt/my app/bin (150), [heap] (80)
        self.assertEqual(groups[0]["label"], "[anon]")
        self.assertEqual(groups[0]["rss_kib"], 200)
        self.assertEqual(groups[0]["mapping_count"], 1)

        self.assertEqual(groups[1]["label"], "/opt/my app/bin")
        self.assertEqual(groups[1]["rss_kib"], 150)
        self.assertEqual(groups[1]["pss_kib"], 100)
        self.assertEqual(groups[1]["private_rss_kib"], 150)
        self.assertEqual(groups[1]["mapping_count"], 2)

        self.assertEqual(groups[2]["label"], "[heap]")
        self.assertEqual(groups[2]["rss_kib"], 80)
        self.assertEqual(groups[2]["mapping_count"], 1)

    def test_mappings_with_malformed_vma(self):
        smaps_sample = (
            "00400000-00452000 r-xp 00000000 08:01 123456 /usr/lib/libtest.so\n"
            "Rss: invalid kB\n"
            "Pss: 50 kB\n"
            "Private_Clean: 100 kB\n"
            "Private_Dirty: 0 kB\n"
        )
        groups, reasons = parse_smaps_mappings(smaps_sample)
        self.assertEqual(len(groups), 1)
        self.assertIsNone(groups[0]["rss_kib"])
        self.assertEqual(groups[0]["pss_kib"], 50)
        self.assertTrue(reasons)


class TestMemorySnapshotReader(unittest.TestCase):
    """Test read_memory_snapshot with mocked /proc tree."""

    def setUp(self):
        self.test_dir = tempfile.TemporaryDirectory()
        self.proc_root = Path(self.test_dir.name)
        self.pid = 4242
        self.pdir = self.proc_root / str(self.pid)
        self.pdir.mkdir(parents=True)

        # Create valid mock stat: start ticks is field 22 (index 19 in tail).
        # tail[0] is state "R", then 18 fields, then field 22 (starttime=54321).
        stat_tail = " ".join(["0"] * 18 + ["54321"] + ["0"] * 30)
        (self.pdir / "stat").write_text(f"4242 (slopcom) R {stat_tail}\n")
        (self.pdir / "cmdline").write_bytes(b"/usr/bin/slopcom\x00--stats\x00:1\x00")

    def tearDown(self):
        self.test_dir.cleanup()

    def test_snapshot_complete_rollup(self):
        rollup_content = (
            "Rss: 2048 kB\n"
            "Pss: 1024 kB\n"
            "Shared_Clean: 512 kB\n"
            "Shared_Dirty: 256 kB\n"
            "Private_Clean: 1000 kB\n"
            "Private_Dirty: 280 kB\n"
            "Pss_Anon: 500 kB\n"
            "Pss_File: 500 kB\n"
            "Pss_Shmem: 24 kB\n"
            "Swap: 0 kB\n"
            "Private_Hugetlb: 0 kB\n"
        )
        (self.pdir / "smaps_rollup").write_text(rollup_content)

        snap = read_memory_snapshot(self.pid, proc_root=self.proc_root)
        self.assertEqual(snap["pid"], self.pid)
        self.assertEqual(snap["rss_kib"], 2048)
        self.assertEqual(snap["pss_kib"], 1024)
        self.assertEqual(snap["private_rss_kib"], 1280)
        self.assertEqual(snap["shared_rss_kib"], 768)
        self.assertEqual(snap["swap_kib"], 0)
        self.assertEqual(snap["private_hugetlb_kib"], 0)
        self.assertEqual(snap["command"], "/usr/bin/slopcom --stats :1")
        self.assertEqual(snap["process_start_ticks"], 54321)
        self.assertEqual(snap["unavailable"], [])

    def test_snapshot_optional_fields_missing(self):
        # Older kernel without Pss_*, swap, hugetlb
        rollup_content = (
            "Rss: 2048 kB\n"
            "Pss: 1024 kB\n"
            "Shared_Clean: 512 kB\n"
            "Shared_Dirty: 256 kB\n"
            "Private_Clean: 1000 kB\n"
            "Private_Dirty: 280 kB\n"
        )
        (self.pdir / "smaps_rollup").write_text(rollup_content)

        snap = read_memory_snapshot(self.pid, proc_root=self.proc_root)
        self.assertEqual(snap["rss_kib"], 2048)
        self.assertEqual(snap["pss_kib"], 1024)
        self.assertIsNone(snap["pss_anon_kib"])
        self.assertIsNone(snap["swap_kib"])
        self.assertIsNone(snap["private_hugetlb_kib"])
        self.assertTrue(any("Swap: unavailable" in r for r in snap["unavailable"]))

    def test_missing_process_returns_nulls(self):
        snap = read_memory_snapshot(99999, proc_root=self.proc_root)
        self.assertIsNone(snap["rss_kib"])
        self.assertIsNone(snap["pss_kib"])
        self.assertIsNone(snap["private_rss_kib"])
        self.assertIsNone(snap["process_start_ticks"])
        self.assertTrue(snap["unavailable"])

    def test_permission_denied_returns_nulls(self):
        with patch.object(Path, "read_text", side_effect=PermissionError("Permission denied")):
            snap = read_memory_snapshot(self.pid, proc_root=self.proc_root)
            self.assertIsNone(snap["rss_kib"])
            self.assertTrue(any("permission denied" in r.lower() for r in snap["unavailable"]))

    def test_pid_recycled_invalidates_numeric_measurements(self):
        rollup_content = "Rss: 2048 kB\nPss: 1024 kB\nPrivate_Clean: 1000 kB\nPrivate_Dirty: 0 kB\n"
        (self.pdir / "smaps_rollup").write_text(rollup_content)

        # First stat read succeeds with 54321, second stat read returns 99999 (recycled PID)
        read_counts = {"count": 0}

        def fake_read_ticks(pdir):
            read_counts["count"] += 1
            if read_counts["count"] == 1:
                return 54321
            return 99999  # Changed start ticks!

        with patch.dict(mod_globals, {"read_process_start_ticks": fake_read_ticks}):
            snap = read_memory_snapshot(self.pid, proc_root=self.proc_root)
            self.assertIsNone(snap["rss_kib"])
            self.assertIsNone(snap["pss_kib"])
            self.assertIsNone(snap["private_rss_kib"])
            self.assertIsNone(snap["process_start_ticks"])
            self.assertTrue(any("process start ticks changed" in r for r in snap["unavailable"]))


class TestCLIDispatchAndSystemMonitor(unittest.TestCase):
    """Test CLI dispatch without DISPLAY and SystemMonitor RSS handling."""

    def test_cli_dispatch_bypasses_display_and_selection(self):
        mock_owner = MagicMock()
        with patch("sys.argv", ["run.py", "memory", "--pid", str(os.getpid())]), \
             patch.dict(mod_globals, {"get_selection_owner": mock_owner}), \
             patch("builtins.print") as mock_print:
            status = main_func()
            self.assertEqual(status, 0)
            mock_owner.assert_not_called()
            mock_print.assert_called_once()
            output = mock_print.call_args[0][0]
            data = json.loads(output)
            self.assertEqual(data["pid"], os.getpid())
            self.assertIsNotNone(data["rss_kib"])

    def test_cli_dispatch_missing_pid_returns_one(self):
        with patch("sys.argv", ["run.py", "memory", "--pid", "99999999"]), \
             patch("builtins.print"):
            status = main_func()
            self.assertEqual(status, 1)

    def test_system_monitor_unavailable_rss_is_none(self):
        monitor = SystemMonitor(target_pid=None)
        self.assertIsNone(monitor.max_rss_kb)

        # Mock target_pid with non-existent process /proc files
        monitor = SystemMonitor(target_pid=99999999)
        with patch("builtins.open", side_effect=FileNotFoundError), \
             patch.object(mod_globals["os"], "listdir", side_effect=FileNotFoundError):
            self.assertIsNone(monitor._sample_proc()[1])
        thread = MagicMock()

        def make_synchronous_thread(*, target, **kwargs):
            thread.start.side_effect = target
            return thread

        with patch.object(mod_globals["threading"], "Thread", side_effect=make_synchronous_thread), \
             patch.object(monitor, "_sample_proc", return_value=(0.0, None, 0, 0)) as proc_sample, \
             patch.object(monitor, "_sample_power_w", return_value=None) as power_sample, \
             patch.object(monitor, "_sample_gpu_busy", return_value=None) as gpu_sample, \
             patch.object(mod_globals["time"], "sleep"), \
             patch.object(monitor._stop_event, "is_set", side_effect=[False, True]):
            monitor.start()
            monitor.stop()
        thread.start.assert_called_once()
        thread.join.assert_called_once_with(timeout=1.0)
        self.assertEqual(proc_sample.call_count, 2)
        power_sample.assert_called_once()
        gpu_sample.assert_called_once()
        self.assertEqual(monitor.rss_samples, [])
        self.assertIsNone(monitor.max_rss_kb)

    def _run_isolated_workload(self, backend, capture_memory):
        monitor = MagicMock()
        monitor.cpu_seconds = 0
        monitor.max_rss_kb = None
        monitor.total_context_switches = 0
        monitor.avg_power_w = None
        monitor.avg_gpu_busy = None

        process = MagicMock()
        process.proc.pid = 4242
        process.query_snapshot.return_value = {
            "pid": 4242,
            "render_attempts": 0,
            "successful_swaps": 0,
            "full_repaints": 0,
            "partial_repaints": 0,
        }

        snapshot_reader = MagicMock(return_value={"rss_kib": 123})
        workspace_switch = MagicMock()
        process_launcher = MagicMock()

        with patch.dict(mod_globals, {
            "switch_i3_workspace": workspace_switch,
            "SlopcomProcess": MagicMock(return_value=process),
            "SystemMonitor": MagicMock(return_value=monitor),
            "read_memory_snapshot": snapshot_reader,
        }), patch.object(mod_globals["subprocess"], "Popen", process_launcher), \
             patch.object(mod_globals["subprocess"], "run") as process_runner, \
             patch.object(mod_globals["time"], "sleep"):
            record = run_single_workload(
                backend=backend,
                workload="empty-idle",
                repeat=1,
                settle_sec=0,
                sample_sec=0,
                display=":test-isolated",
                binary="/must/not/run/slopcom",
                fixture_bin="/must/not/run/fixture",
                capture_memory=capture_memory,
            )

        process_launcher.assert_not_called()
        process_runner.assert_not_called()
        monitor._sample_power_w.assert_not_called()
        monitor._sample_gpu_busy.assert_not_called()
        self.assertEqual(workspace_switch.call_count, 2)
        return record, snapshot_reader, workspace_switch

    def test_run_single_workload_none_backend_is_isolated_and_memory_stays_null(self):
        record, snapshot_reader, workspace_switch = self._run_isolated_workload(
            backend="none", capture_memory=True
        )
        self.assertIsNone(record["memory_before"])
        self.assertIsNone(record["memory_after"])
        snapshot_reader.assert_not_called()
        self.assertEqual(workspace_switch.call_count, 2)

    def test_run_single_workload_memory_capture_is_opt_in(self):
        record, snapshot_reader, _ = self._run_isolated_workload(
            backend="slopcom", capture_memory=False
        )
        self.assertIsNone(record["memory_before"])
        self.assertIsNone(record["memory_after"])
        snapshot_reader.assert_not_called()

        record, snapshot_reader, _ = self._run_isolated_workload(
            backend="slopcom", capture_memory=True
        )
        self.assertEqual(record["memory_before"], {"rss_kib": 123})
        self.assertEqual(record["memory_after"], {"rss_kib": 123})
        self.assertEqual(snapshot_reader.call_count, 2)


if __name__ == "__main__":
    unittest.main()
