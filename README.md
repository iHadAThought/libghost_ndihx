# libghost_ndihx

**NDI|HX-only** receive library for Linux **aarch64** and **x86_64**.

This is the first decoder plugin for the **GhostVidStream** multi-protocol
receive / viewer shell. This repo is **decoder-only** (no SDL viewer). Other
protocols (FULL NDI, SMPTE 2110, RTSP) will ship as separate modules — not here.

| Piece | Name |
| --- | --- |
| Library | `libghost_ndihx` |
| Header / API | `ghost_ndihx.h` · `ghost_ndihx_*` |
| Module core | `libmedia_core` (protocol-agnostic registry) |

## Build

```bash
./install-deps.sh    # libndi + FFmpeg ≥ 7 (sudo)
make
sudo make install-lib
```

## Embed

See [docs/integration.md](docs/integration.md).

## Resource usage

Steady-state CPU / RSS / threads (decoder perspective, aarch64 booth host):
[docs/resource-usage.md](docs/resource-usage.md).

## Related

- GhostVidStream shell (viewer + future modules):
  - GitHub: https://github.com/iHadAThought/GhostVidStream
  - Forgejo: https://git.ghostnetwork.app/Brendan/GhostVidStream
- BookStack: https://bookstack.ghostnetwork.app/books/libghost-ndihx

## Other protocols

SRT and RTMP decoder modules live in the **GhostVidStream** shell repo (not this
NDI|HX library). See that repo's `docs/srt-rtmp-decoder.md`.


## Discovery

LAN browse is **libghost_discover** (`ghost_discover.h`): backends `auto` / `bonjour` / `ndi_sdk`. See `docs/ndi-discovery.md`.
