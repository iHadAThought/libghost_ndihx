# libghost_ndihx

NDI|HX-focused receive library for Linux **aarch64** and **x86_64**.

Part of the **GhostVidStream** product family. This repo is the **decoder-only**
package (no SDL viewer).

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

## Related

- Full app (GhostVidStream viewer): GitHub `iHadAThought/GhostVidStream`, Forgejo `Brendan/GhostVidStream`
