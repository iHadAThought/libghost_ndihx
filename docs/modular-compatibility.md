# Modular protocol compatibility

This repo is structured so a future **main app** can plug in multiple receive
protocols without rewriting the NDI|HX path.

Sibling Project plan (store): `docs/modular-protocol-plan.md` (drafted separately).

## Layout

```
include/media_core.h          # protocol-agnostic types + module vtable
include/ghost_ndihx.h              # NDI|HX native API (first module)
src/core/media_core.c         # registry + shared pick helpers (no NDI deps)
src/modules/ghost_ndihx/           # first module (implemented)
src/modules/ndi_full/         # placeholder
src/modules/st2110/           # placeholder
src/modules/rtsp/             # placeholder
src/viewer_main.c             # SDL reference (uses native ghost_ndihx_* today)
```

## Contracts (do not paint into a corner)

1. **`media_core.h` never includes protocol SDKs** (no Processing.NDI, no
   GStreamer, no Live555). New protocols add a module dir + register a
   `media_module_t`.
2. **BGRX (or documented) frames** via `media_frame_t` / `capture_newest` —
   newest-frame drain semantics stay the same across modules.
3. **Native module APIs are allowed** (`ghost_ndihx.h`) for embeds that only need one
   protocol; they must also expose `*_media_module()` + `*_register_media_module()`.
4. **Protocol ids are additive enums** (`MEDIA_PROTO_NDI_HX`, `_NDI_FULL`,
   `_ST2110`, `_RTSP`). Do not renumber.
5. **Open params** carry shared filters; protocol-specific knobs go in
   `protocol_opts` (opaque pointer), not in the core struct.
6. **PTZ is capability-gated.** Modules may set `MEDIA_CAP_PTZ` (and continuous /
   absolute / home / preset bits) via optional `capabilities` + `ptz_*` vtable
   entries. Hosts must hide PTZ UI when the flag is clear. NDI|HX probes
   `NDIlib_recv_ptz_is_supported` after connect (may take ~1–2s). FULL NDI should
   use the same NDI PTZ APIs; RTSP prefers ONVIF when advertised; SMPTE 2110
   usually leaves PTZ unsupported.

## Using the module API

```c
#include <media_core.h>
#include <ghost_ndihx.h>

ghost_ndihx_register_media_module();
const media_module_t *m = media_find_module("ghost_ndihx");

media_open_params_t p;
media_open_params_defaults(&p);
snprintf(p.ip_substr, sizeof(p.ip_substr), "%s", "172.16.1.189");

media_session_t *s = m->open(&p);
m->connect_auto(s, NULL);
media_frame_t fr;
while (m->capture_newest(s, &fr)) { /* … */ }
m->close(s);
```

Link: `-lndi_hx -lmedia_core -lndi -ldl -lpthread -lm`

## Future modules

| Id | Protocol | Notes |
| --- | --- | --- |
| `ghost_ndihx` | NDI\|HX | Implemented (libndi + FFmpeg ≥ 7) |
| `ndi_full` | FULL NDI | Same SDK family; different bandwidth/color defaults |
| `st2110` | SMPTE 2110 | Likely different deps; still fills `media_frame_t` |
| `rtsp` | RTSP/RTP | URL-centric discover/connect |

Placeholders live under `src/modules/<id>/README.md` until implemented.
