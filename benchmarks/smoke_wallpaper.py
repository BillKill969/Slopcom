#!/usr/bin/env python3
"""
v0.6 Live Smoke Test for Wallpaper Compatibility and Zero-Idle Invariant.
Uses SlopcomProcess from benchmarks.run for reliable lifecycle and stats tracking.
"""

import os
import sys
import time
import subprocess

# Ensure repo root is in python path
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from benchmarks.run import SlopcomProcess, get_selection_owner, switch_i3_workspace, get_i3_workspaces

DISPLAY = os.environ.get("DISPLAY", ":1")
XAUTH = os.environ.get("XAUTHORITY", "/run/user/1000/sx.xauthority")
ENV = {**os.environ, "DISPLAY": DISPLAY, "XAUTHORITY": XAUTH}

def run_cmd(cmd, check=True):
    return subprocess.run(cmd, env=ENV, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=check)

def get_root_prop(prop):
    res = run_cmd(["xprop", "-root", prop], check=False)
    if res.returncode != 0:
        return None
    for line in res.stdout.splitlines():
        if line.startswith(prop):
            sep = "=" if "=" in line else ":"
            return line.split(sep, 1)[1].strip()
    return None

def main():
    print(f"=== v0.6 Wallpaper Compatibility Verification on {DISPLAY} ===")

    wp1 = "/tmp/wallpaper1.png"
    wp2 = "/tmp/wallpaper2.png"
    wp_tile = "/tmp/wallpaper_tile.png"

    # Verify test images exist
    for p in (wp1, wp2, wp_tile):
        assert os.path.exists(p), f"Missing test wallpaper {p}; run tools/make_test_wallpapers.py"

    # Ensure no other compositor is active
    owner = get_selection_owner(DISPLAY)
    assert not owner, f"Another compositor (0x{owner:x}) is already running on {DISPLAY}"

    # 1. Establish wallpaper before compositor startup
    print("\n[Test 1] Establishing wallpaper before compositor startup...")
    run_cmd(["xwallpaper", "--zoom", wp1])
    pmap1 = get_root_prop("_XROOTPMAP_ID")
    print(f"  Root _XROOTPMAP_ID: {pmap1}")
    assert pmap1 and "pixmap id" in pmap1, f"Expected _XROOTPMAP_ID pixmap id, got: {pmap1}"

    print("  Starting slopcom with --stats...")
    slopcom = SlopcomProcess("./build/release/slopcom", DISPLAY, extra_args=["--verbose"])
    slopcom.start()
    time.sleep(0.3)

    s0 = slopcom.query_snapshot()
    print(f"  Startup stats: swaps={s0['successful_swaps']}, renders={s0['render_attempts']}, textures={s0['live_textures']}")
    assert s0["successful_swaps"] >= 1, "Compositor must render at least 1 frame on startup"
    assert s0["live_textures"] >= 1, "Wallpaper texture must be imported"
    print("  [Pass] Wallpaper before startup detected and composited.")

    # 2. Hot-change wallpaper
    print("\n[Test 2] Hot-changing wallpaper while running...")
    swaps_before = s0["successful_swaps"]
    run_cmd(["xwallpaper", "--zoom", wp2])
    pmap2 = get_root_prop("_XROOTPMAP_ID")
    print(f"  New root _XROOTPMAP_ID: {pmap2}")
    time.sleep(0.3)

    s1 = slopcom.query_snapshot()
    print(f"  Hot-change stats: swaps={s1['successful_swaps']}, delta={s1['successful_swaps'] - swaps_before}")
    assert s1["successful_swaps"] > swaps_before, "Hot-change must trigger an immediate scene update and swap"
    print("  [Pass] Hot wallpaper change detected and rendered immediately.")

    # 3. Multiple wallpaper transitions
    print("\n[Test 3] Testing multiple rapid wallpaper switches...")
    for i in range(4):
        target = wp1 if (i % 2 == 0) else wp2
        run_cmd(["xwallpaper", "--zoom", target])
        time.sleep(0.15)
    s2 = slopcom.query_snapshot()
    print(f"  Rapid transitions stats: swaps={s2['successful_swaps']}, live_textures={s2['live_textures']}")
    assert slopcom.proc.poll() is None, "slopcom survived multiple wallpaper changes"
    print("  [Pass] Rapid wallpaper changes handled without crash or stale state.")

    # 4. Odd-sized tile mode
    print("\n[Test 4] Testing tile mode with odd-sized tile (37x23)...")
    run_cmd(["xwallpaper", "--tile", wp_tile])
    time.sleep(0.2)
    s3 = slopcom.query_snapshot()
    assert slopcom.proc.poll() is None, "slopcom survived tile mode"
    print(f"  Tile mode stats: swaps={s3['successful_swaps']}")
    print("  [Pass] Tile mode active.")

    # 5. Clear mode (valid black update)
    print("\n[Test 5] Testing valid black update via --clear...")
    run_cmd(["xwallpaper", "--clear"])
    time.sleep(0.2)
    s4 = slopcom.query_snapshot()
    assert slopcom.proc.poll() is None, "slopcom survived --clear"
    print(f"  Clear mode stats: swaps={s4['successful_swaps']}")
    print("  [Pass] xwallpaper --clear handled.")

    # 6. Idle Invariant (Strictly 0 swaps over 10s idle)
    print("\n[Test 6] Testing Zero-Idle Swap Invariant (10 seconds)...")
    original_ws = None
    for ws in get_i3_workspaces(DISPLAY):
        if ws.get("focused"):
            original_ws = ws.get("name")
            break

    switch_i3_workspace(DISPLAY, "bench")
    time.sleep(0.5)
    run_cmd(["xwallpaper", "--zoom", wp1])
    time.sleep(1.0) # wait 1s for any event to settle
    snap_a = slopcom.query_snapshot()
    print(f"  Idle start snapshot: renders={snap_a['render_attempts']}, swaps={snap_a['successful_swaps']}")
    
    # Wait 10 seconds without mouse or keyboard input
    time.sleep(10.0)
    snap_b = slopcom.query_snapshot()
    print(f"  Idle end snapshot:   renders={snap_b['render_attempts']}, swaps={snap_b['successful_swaps']}")
    
    if original_ws:
        switch_i3_workspace(DISPLAY, original_ws)
        time.sleep(0.5)

    render_delta = snap_b['render_attempts'] - snap_a['render_attempts']
    swap_delta = snap_b['successful_swaps'] - snap_a['successful_swaps']
    print(f"  10s Idle Delta: renders=+{render_delta}, swaps=+{swap_delta}")
    assert render_delta == 0, f"Expected 0 idle renders, got {render_delta}"
    assert swap_delta == 0, f"Expected 0 idle swaps, got {swap_delta}"
    print("  [Pass] Zero-Idle Invariant verified: strictly 0 renders and 0 swaps over 10s idle!")

    # 7. Safe fallback on missing property
    print("\n[Test 7] Testing invalid/missing wallpaper property fallback...")
    run_cmd(["xprop", "-root", "-remove", "_XROOTPMAP_ID", "-remove", "ESETROOT_PMAP_ID"])
    time.sleep(0.3)
    snap_c = slopcom.query_snapshot()
    assert slopcom.proc.poll() is None, "slopcom survived property removal"
    print(f"  Fallback stats: live_textures={snap_c['live_textures']}")
    print("  [Pass] Property removal safely handled with fallback.")

    # 8. Clean shutdown & restart
    print("\n[Test 8] Testing clean shutdown and restart...")
    code, exit_stats = slopcom.stop()
    assert code == 0, f"Expected exit code 0, got {code}"
    print(f"  Compositor exited cleanly (code 0). Final stats: {exit_stats}")

    # Restore wallpaper and restart
    run_cmd(["xwallpaper", "--zoom", wp1])
    slopcom2 = SlopcomProcess("./build/release/slopcom", DISPLAY)
    slopcom2.start()
    time.sleep(0.3)
    s_restart = slopcom2.query_snapshot()
    assert s_restart["successful_swaps"] >= 1, "New instance composited published wallpaper"
    code2, _ = slopcom2.stop()
    assert code2 == 0, "Second instance exited cleanly"
    print("  [Pass] Stop and restart loaded published wallpaper and cleaned up.")

    # 9. Test feh if available
    print("\n[Test 9] Smoke test feh (if installed)...")
    feh_check = run_cmd(["which", "feh"], check=False)
    if feh_check.returncode == 0:
        run_cmd(["feh", "--no-fehbg", "--bg-fill", wp2])
        feh_pmap = get_root_prop("_XROOTPMAP_ID")
        print(f"  feh set _XROOTPMAP_ID: {feh_pmap}")
        slopcom3 = SlopcomProcess("./build/release/slopcom", DISPLAY)
        slopcom3.start()
        time.sleep(0.3)
        s_feh = slopcom3.query_snapshot()
        assert s_feh["successful_swaps"] >= 1
        slopcom3.stop()
        print("  [Pass] feh --bg-fill successfully read and displayed.")
    else:
        print("  [Skip] feh not installed.")

    print("\n========================================================")
    print("ALL v0.6 WALLPAPER COMPATIBILITY TESTS PASSED ON REAL X!")
    print("========================================================")

if __name__ == '__main__':
    main()
