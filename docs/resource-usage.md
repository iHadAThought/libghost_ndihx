# Resource usage (libghost_ndihx)

Steady-state measurements for the **NDI|HX receive library** on Linux
**aarch64**. Numbers were taken via the GhostVidStream reference host (SDL
viewer + stress harness) on the same process that links `libghost_ndihx` —
so they bound what **embeds** should expect for the decoder path itself.

Overlays / windowing cost above the library are called out separately; they
are small.

## Test setup

| Item | Value |
| --- | --- |
| Host | `172.16.1.144` · Ubuntu · **4× aarch64** cores |
| Camera | `172.16.1.189` (HX stream) |
| When | 2026-09-25 (baseline + harden remeasure) |
| Library | `libghost_ndihx` (linked into `ghostvidstream` / stress binary) |
| Method | ~12 s warmup, then `pidstat` / `/proc` jiffies ~5 s; RSS/threads/fds from `/proc` |

**CPU %** uses a **one-core = 100%** scale. On 4 cores, ~70% means roughly
under one core saturated — not 70% of the whole machine.

No passwords or credentials appear in this document.

## Steady-state (decoder-dominated)

| Scenario | CPU % | RSS | Threads | FDs | Resolution | Src fps |
| --- | ---: | ---: | ---: | ---: | --- | ---: |
| **No stream** (finder / auto-search only) | **~0** | **~9.6 MiB** | **2** | **6** | — | — |
| **Low quality** (`GHOST_NDIHX_BW_LOWEST`) | **~72–78** | **~190–194 MiB** | **29** | **38** | **640×360** | 30 |
| **Max quality** (`GHOST_NDIHX_BW_HIGHEST`) | **~65–80** | **~217–227 MiB** | **29** | **38** | **1920×1080** | 30 |

Host UI chrome on top of the same session (for comparison only — **not** part of the library):

| Host overlay | Extra vs decode | Notes |
| --- | --- | --- |
| Stats HUD on/off | few % CPU / few MiB | Negligible vs libndi+FFmpeg |
| Controls overlay (`c`) | few % CPU | Same |
| Pause **without** drain (pre-harden) | RSS could climb (~296 MiB class) | Fixed: call `ghost_ndihx_drain()` while not presenting |
| Pause **with** drain (post-harden) | RSS ≈ live (~227 MiB) | Recommended embed pattern |

## What embeds should plan for

1. **Idle / disconnected** sessions stay cheap (~0% CPU, ~10 MiB).
2. **Connected HX decode** on this class of aarch64 host: order-of-magnitude **~65–80% of one core**, **~190–230 MiB RSS**, **~29 threads** (almost all inside libndi / FFmpeg — not the shim).
3. Prefer **`ghost_ndihx_capture_newest`** always; while paused or not presenting, call **`ghost_ndihx_drain()`** so the SDK queue cannot grow RSS.
4. HUD / UI in the host process will not meaningfully change the decoder budget on this path.
5. You cannot shrink the ~29 NDI/FFmpeg worker threads from the C shim alone.

## Soak / stability (library stress harness)

| Suite | Result (2026-09-25) |
| --- | --- |
| discover / reconnect / bw flap / PTZ / multi / wrong-IP / soak | **PASS** |
| ASan soak | **PASS** (no hits in our `.c`) |
| Valgrind | Harness **PASS**; remaining Invalid reads classified in **libndi / FFmpeg** only |
| 1080p soak RSS growth | ~25–50 MiB over 45–180 s (under 64 MiB fail threshold) |
| Headless pull rate | ~74–132 fps newest-frame drain (not glass-to-glass) |

Re-run: `tests/stress/run_all.sh`, `tests/stress/resource_profile.sh` with
`NDI_STRESS_IP=<camera>`.

## Related

- Viewer-framed copy: sibling **GhostVidStream** → `docs/resource-usage.md`
- Embed API: [integration.md](integration.md)
- Module contract: [modular-compatibility.md](modular-compatibility.md)
