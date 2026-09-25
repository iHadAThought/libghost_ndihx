# Embedding libghost_ndihx in another app

**GhostVidStream** is the product (SDL viewer). Embeds link **`libghost_ndihx`**
— a small **C** library with **no SDL / GUI dependency**. Use it from Electron
(N-API), Qt, Python ctypes/cffi, Go cgo, Rust FFI, or plain C/C++.

Companion headers:

| Header | Role |
| --- | --- |
| [`include/ghost_ndihx.h`](../include/ghost_ndihx.h) | Native NDI\|HX API (`ghost_ndihx_*`) |
| [`include/media_core.h`](../include/media_core.h) | Protocol-agnostic module registry + PTZ caps |

Multi-protocol hosts (FULL NDI / 2110 / RTSP later): see
[`modular-compatibility.md`](modular-compatibility.md).

Library version: **libghost_ndihx 0.4.x**.

---

## Architectures

| Host | `uname -m` | NDI SDK lib dir (via `install-deps.sh`) |
| --- | --- | --- |
| ARM 64-bit | `aarch64` / `arm64` | `aarch64-rpi4-linux-gnueabi` |
| Intel/AMD 64-bit | `x86_64` / `amd64` | `x86_64-linux-gnu` |

Runtime deps (same on both):

- **libndi** (NDI SDK v6)
- **FFmpeg ≥ 7** shared libs (HX decode)
- **Avahi** (mDNS discovery)

```bash
./install-deps.sh          # sudo; installs into /usr/local by default
make
sudo make install-lib      # media_core.h + ghost_ndihx.h + .a archives
```

Set `PREFIX` / `DESTDIR` if you install outside `/usr/local`.

---

## Link flags

```bash
gcc -O2 -I/usr/local/include -o myapp myapp.c \
  /usr/local/lib/libghost_ndihx.a /usr/local/lib/libmedia_core.a \
  -L/usr/local/lib -Wl,-rpath,/usr/local/lib \
  -lndi -ldl -lpthread -lm
```

Or:

```bash
gcc -O2 -I/usr/local/include -o myapp myapp.c \
  -L/usr/local/lib -Wl,-rpath,/usr/local/lib \
  -lghost_ndihx -lmedia_core -lndi -ldl -lpthread -lm
```

Order matters with some linkers: put `libghost_ndihx.a` / `-lghost_ndihx` **before** `-lndi`.

C++: headers already wrap in `extern "C"`.

---

## Minimal C example

```c
#include <ghost_ndihx.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
  ghost_ndihx_options_t opt;
  ghost_ndihx_options_defaults(&opt);
  snprintf(opt.ip_substr, sizeof(opt.ip_substr), "%s", "172.16.1.189");
  opt.prefer_hx = true;
  opt.auto_search = true;
  opt.bandwidth = GHOST_NDIHX_BW_HIGHEST; /* full-res; never silently downscales */
  opt.capture_wait_ms = 8;

  ghost_ndihx_session_t *s = ghost_ndihx_session_create(&opt);
  if (!s) {
    fprintf(stderr, "session_create failed (libndi / FFmpeg?)\n");
    return 1;
  }

  volatile int cancel = 0;
  if (ghost_ndihx_connect_auto(s, &cancel) != 0) {
    fprintf(stderr, "connect_auto failed\n");
    ghost_ndihx_session_destroy(s);
    return 1;
  }

  for (;;) {
    ghost_ndihx_frame_t fr;
    if (!ghost_ndihx_capture_newest(s, &fr)) {
      usleep(1000);
      continue;
    }
    /* fr.data = BGRX (4 bytes/pixel). Owned by session until next capture. */
    (void)fr;
  }

  ghost_ndihx_session_destroy(s);
  return 0;
}
```

---

## API map

| Call | Role |
| --- | --- |
| `ghost_ndihx_init` / `ghost_ndihx_shutdown` | Refcounted NDI runtime |
| `ghost_ndihx_options_defaults` / `_load_file` | Settings |
| `ghost_ndihx_session_create` / `_destroy` | Finder + optional receiver |
| `ghost_ndihx_discover` / `ghost_ndihx_pick` | LAN search / selection |
| `ghost_ndihx_connect` / `ghost_ndihx_connect_auto` | Attach to a source |
| `ghost_ndihx_capture_newest` | Low-latency frame (drains queue) |
| `ghost_ndihx_drain` | Discard queued video without presenting |
| `ghost_ndihx_queue_depth` | Debug queue sizes |
| `ghost_ndihx_set_bandwidth` | `highest` / `lowest` (reconnects) |
| `ghost_ndihx_disconnect` | Drop receiver; keep finder |
| `ghost_ndihx_capabilities` / `ghost_ndihx_ptz_*` | Optional PTZ |

### Config / options keys

| Key | Meaning |
| --- | --- |
| `source` / `ip` | Prefer substrings |
| `prefer_hx` / `auto_search` | Pick + retry behavior |
| `find_ms` / `rescan_ms` / `capture_wait_ms` | Timing |
| `bandwidth` | `highest` (default) or `lowest` |
| `recv_name` | Name shown to NDI peers (default **GhostVidStream**) |

Camera **encode** settings are on the **sender**, not this library.

---

## Threading

- **One session is not thread-safe.** One session per thread, or mutex all
  capture / connect / disconnect / bandwidth / PTZ calls.
- `connect_auto` blocks while searching; pass `volatile int *cancel` to abort.

---

## Quality, latency, resources

| Goal | What to do |
| --- | --- |
| Full quality | `bandwidth = GHOST_NDIHX_BW_HIGHEST` (default). Always **BGRX/BGRA** at sender resolution. |
| Lower CPU/RAM | `GHOST_NDIHX_BW_LOWEST` or host-side present caps |
| Lowest latency | Always `ghost_ndihx_capture_newest`; small `capture_wait_ms` |
| Pause without RAM growth | Call `ghost_ndihx_drain()` while not presenting |
| Assert latency | After capture, `ghost_ndihx_queue_depth` video ≈ 0 |

---

## Error handling

| Situation | Behavior |
| --- | --- |
| Missing libndi / FFmpeg | `session_create` → `NULL` |
| No matching source | `connect_auto` → `-1` |
| Capture idle | `capture_newest` → `false` |
| PTZ unsupported | `ghost_ndihx_ptz_*` → `-1` |

Always destroy the session on teardown (including error exits).

---

## Optional PTZ

```c
uint32_t caps = ghost_ndihx_capabilities(s);
if (caps & MEDIA_CAP_PTZ) {
  ghost_ndihx_ptz_move(s, 0.2f, 0.f, 0.f, true);
  ghost_ndihx_ptz_stop(s);
  ghost_ndihx_ptz_home(s);
}
```

Re-probe a second or two after connect. Via `media_core`:
`ghost_ndihx_register_media_module()` then `media_find_module("ghost_ndihx")`.

---

## media_core embed (short)

```c
#include <media_core.h>
#include <ghost_ndihx.h>

ghost_ndihx_register_media_module();
const media_module_t *m = media_find_module("ghost_ndihx");
media_open_params_t p = {0};
p.auto_search = true;
snprintf(p.ip_substr, sizeof(p.ip_substr), "%s", "172.16.1.189");
media_session_t *s = m->open(&p);
m->connect_auto(s, NULL);
media_frame_t fr;
while (m->capture_newest(s, &fr)) { /* BGRX */ }
m->close(s);
```

---

## aarch64 vs x86_64 notes

- Same source and Makefile; `install-deps.sh` picks the NDI SDK `.so` from `uname -m`.
- HX decode is **software** (FFmpeg inside libndi) on both.
- Ship/run with `LD_LIBRARY_PATH` or `-Wl,-rpath` to `/usr/local/lib`.

---

## GhostVidStream viewer (not required for embeds)

`ghostvidstream` (alias `ndi-hx-viewer`) is the SDL2 product demo only. Product
hosts should link **libghost_ndihx** directly. Viewer flags such as `--max-w` /
`--fps-cap` are display-side and are **not** part of the library API.

### Rename note (from libndi_hx)

Older checkouts used `libndi_hx` / `ndi_hx.h` / `ndi_hx_*`. Those names are
**removed**. Update includes, link lines, and call sites to `libghost_ndihx` /
`ghost_ndihx.h` / `ghost_ndihx_*`.
