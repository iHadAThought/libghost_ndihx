# NDI discovery (libghost_discover)

GhostVidStream discovers NDI sources through **libghost_discover** — a first-class
module with the same bar as `media_core` protocol plugins. Bonjour is **not** a
one-off hook inside `ghost_ndihx.c` or `viewer_main.c`.

## Module layout

| Path | Role |
| --- | --- |
| [`include/ghost_discover.h`](../include/ghost_discover.h) | Public API |
| [`src/modules/discover/`](../src/modules/discover/) | Registry + backends |
| `libghost_discover.a` | Build unit (optional link) |

| Backend id | Implementation | Service / API |
| --- | --- | --- |
| `bonjour` | Avahi (Linux) / dns_sd (macOS) | `_ndi._tcp` on `local.` |
| `ndi_sdk` | NDI SDK finder | `NDIlib_find_*` |
| `auto` | Policy in `ghost_discover_browse` | Bonjour first, SDK merge/fallback |

## Enable / use

```bash
# Default: auto (Bonjour then NDI SDK)
./ghostvidstream --auto --ip 172.16.1.189

# Force custom Bonjour only
./ghostvidstream --list --discover bonjour --find-ms 4000

# Force NDI SDK finder only
./ghostvidstream --list --discover ndi_sdk

# Config file
discover=auto   # or bonjour | ndi_sdk
```

Viewer UI **Auto** / **Rescan** and `ghost_ndihx_connect_auto` use the same
`ghost_ndihx_discover` → `ghost_discover_browse` → `ghost_discover_pick` path
(source / ip / prefer_hx rules unchanged).

## Embeds / GhostSpot

Link `-lghost_discover` (plus Avahi on Linux). Call `ghost_discover_browse` /
`ghost_discover_pick` directly, or keep using `ghost_ndihx_discover` which
delegates. The GhostSpot `ndi_hx_bridge` can reuse this module later without
reimplementing mDNS.

## Security notes

- Native APIs only (no `dns-sd` / `avahi-browse` shell-outs).
- Do not put SSH passwords or camera credentials in docs or configs committed to git.
