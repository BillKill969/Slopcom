# slopcom, in numbers

**Zero frames in the recorded idle samples. About 59 presentations per second on the historical 60 Hz video fixture, using roughly 2.7% of one CPU core.**

The performance table below is historical: Radeon 780M, Mesa, and i3 at 2880 × 1760, with single 1.5-second samples recorded before the recent fixes. Update rates follow fixture cadences, not maximum throughput.

| Workload | Presentations/sec | CPU use¹ | Repaint |
| --- | ---: | ---: | --- |
| Empty desktop | **0** | 0.00 s recorded | None |
| Static overlapping windows | **0** | 0.00 s recorded | None |
| Typing fixture | 8 | ~0.7% | Partial |
| Moving and resizing | 20 | ~0.7% | Partial |
| Scrolling fixture | 30 | ~2.0% | Partial |
| Video fixture | **59.3** | **~2.7%** | 88 partial frames, 1 full |

Both historical idle samples also recorded **zero poll wakeups**. A separate v0.9 memory snapshot, recorded at 2880 × 1800, explains the process footprint:

| Memory metric | Recorded value |
| --- | ---: |
| Total resident memory (RSS) | **154.7 MiB** |
| Shared resident pages | **137.9 MiB — 89.1% of RSS** |
| Private resident pages | **16.9 MiB** |
| Proportional share (PSS) | **41.6 MiB** |

Private memory includes library and driver pages, not just slopcom's bookkeeping. These are overlapping views of process memory; do not add them together. They do not account for all X server or GPU allocations.

The useful bits: direct GPU pixmap import, damage-based redraws, buffer-age repair, culling of offscreen or fully covered windows, opaque blending bypass, inline presentation damage, zero-extra-buffer EGL config selection, reusable quad geometry, and selective XCB child query batching. VSync, real transparency, shaped windows, and live wallpaper updates are supported. When the desktop is still, the compositor blocks waiting for work.

The latest release check passes **69 Make invocations**: 67 C regression cases and two offline Python suites with **19 tests** (total **86 case executions**). A debug ASan/UBSan run also passed with leak detection disabled for the tracing environment (this limitation does not verify absence of leaks).

Focused rendering and protocol batching optimizations were completed: opaque blending bypass, bounded presentation-damage storage, zero-extra-buffer EGL config selection, static quad geometry reuse with vertex state caching, and selective XCB child-window attribute/geometry query batching. These deliver bounded reductions in API, round-trip serialization, and allocation overhead verified by headless regressions; they do not claim measured FPS, memory, or battery savings.

¹ CPU percentages are CPU time divided by elapsed time, expressed as a share of one core. A recorded 0.00 s reflects measurement precision. These results do not establish battery savings.

[Historical performance records](benchmarks/results/bench/1794201964_slopcom_video_1.json) (other workloads use the same recorded series) · [Memory snapshot](benchmarks/results/memory_attribution_settled.json) · [v0.9 closeout](docs/DONE6.md) · [Validation notes](docs/validation.md)
