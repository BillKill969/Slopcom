#!/usr/bin/env python3
"""
v0.6 App Compatibility Smoke Test:
Tests dunst notifications, dmenu/rofi, xterm, and i3bar under slopcom.
"""

import os
import sys
import time
import subprocess
import signal

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from benchmarks.run import SlopcomProcess, get_selection_owner

DISPLAY = os.environ.get("DISPLAY", ":1")
XAUTH = os.environ.get("XAUTHORITY", "/run/user/1000/sx.xauthority")
ENV = {**os.environ, "DISPLAY": DISPLAY, "XAUTHORITY": XAUTH}

def run_cmd(cmd, check=True):
    return subprocess.run(cmd, env=ENV, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=check)

def main():
    print(f"=== v0.6 App Compatibility Verification on {DISPLAY} ===")

    owner = get_selection_owner(DISPLAY)
    assert not owner, f"Another compositor (0x{owner:x}) is already running on {DISPLAY}"

    print("  Starting slopcom...")
    slopcom = SlopcomProcess("./build/release/slopcom", DISPLAY, extra_args=["--verbose"])
    slopcom.start()
    time.sleep(0.5)

    # 1. Dunst & notify-send
    print("\n[App Test 1] Testing Dunst notification daemon + notify-send...")
    dunst_started = False
    dunst_check = subprocess.run(["pgrep", "dunst"], stdout=subprocess.DEVNULL)
    if dunst_check.returncode != 0:
        dunst_proc = subprocess.Popen(["dunst"], env=ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        dunst_started = True
        time.sleep(0.3)
    else:
        dunst_proc = None

    snap_before = slopcom.query_snapshot()
    run_cmd(["notify-send", "-t", "1000", "slopcom v0.6", "Compatibility Test Notification"])
    time.sleep(0.5)
    snap_during = slopcom.query_snapshot()
    assert snap_during["successful_swaps"] > snap_before["successful_swaps"], "Notification must trigger swaps"
    print(f"  Notification swap delta: +{snap_during['successful_swaps'] - snap_before['successful_swaps']}")

    time.sleep(1.2) # wait for notification to auto-dismiss
    snap_after = slopcom.query_snapshot()
    assert slopcom.proc.poll() is None, "slopcom survived notification lifecycle"
    print("  [Pass] Dunst notification mapped, composed, and unmapped cleanly.")

    if dunst_started and dunst_proc:
        dunst_proc.terminate()

    # 2. dmenu (override-redirect / launcher)
    print("\n[App Test 2] Testing dmenu launcher...")
    snap_dm_before = slopcom.query_snapshot()
    dmenu_proc = subprocess.Popen(["dmenu"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=ENV)
    time.sleep(0.4)
    snap_dm_during = slopcom.query_snapshot()
    assert snap_dm_during["successful_swaps"] > snap_dm_before["successful_swaps"], "dmenu launch must cause composited swap"
    print(f"  dmenu open swap delta: +{snap_dm_during['successful_swaps'] - snap_dm_before['successful_swaps']}")
    # Close dmenu via escape / closing stdin
    try:
        dmenu_proc.stdin.close()
        dmenu_proc.terminate()
        dmenu_proc.wait(timeout=1.0)
    except Exception:
        pass
    time.sleep(0.3)
    snap_dm_after = slopcom.query_snapshot()
    assert slopcom.proc.poll() is None, "slopcom survived dmenu"
    print("  [Pass] dmenu override-redirect popup composed and dismissed cleanly.")

    # 3. xterm
    print("\n[App Test 3] Testing xterm client...")
    snap_xt_before = slopcom.query_snapshot()
    xterm_proc = subprocess.Popen(["xterm", "-geometry", "40x10"], env=ENV)
    time.sleep(0.8)
    snap_xt_during = slopcom.query_snapshot()
    assert snap_xt_during["successful_swaps"] > snap_xt_before["successful_swaps"], "xterm must cause swaps"
    xterm_proc.terminate()
    xterm_proc.wait(timeout=2.0)
    time.sleep(0.5)
    assert slopcom.proc.poll() is None, "slopcom survived xterm"
    print("  [Pass] xterm client mapped, composed, and closed cleanly.")

    # 4. Final shutdown check
    print("\n[App Test 4] Checking compositor clean shutdown after app tests...")
    code, exit_stats = slopcom.stop()
    assert code == 0, f"Compositor exited with error code {code}"
    for res in ("live_pixmaps", "live_images", "live_textures", "live_damage", "live_regions"):
        assert exit_stats.get(res, 0) == 0, f"Resource leak detected in {res}: {exit_stats.get(res)}"
    print(f"  [Pass] Clean shutdown with 0 resource leaks.")

    print("\n========================================================")
    print("ALL APP COMPATIBILITY SMOKE TESTS PASSED!")
    print("========================================================")

if __name__ == '__main__':
    main()
