/**
 * @file README.md
 * @brief libghost_discover — pluggable NDI LAN discovery module.
 *
 * First-class discovery backends for GhostVidStream / libghost_ndihx / embeds
 * (GhostSpot bridge later). Not a one-off hook inside the NDI receive path.
 *
 * ## Layout
 *
 * | File | Role |
 * | --- | --- |
 * | `include/ghost_discover.h` | Public API |
 * | `ghost_discover.c` | Registry, AUTO policy, pick helpers |
 * | `ghost_discover_bonjour.c` | `_ndi._tcp` via Avahi / dns_sd |
 * | `ghost_discover_ndi_sdk.c` | NDI SDK `NDIlib_find_*` |
 *
 * Build unit: `libghost_discover.a` (optional link; Bonjour needs Avahi or
 * system dns_sd).
 *
 * ## Backends
 *
 * | Id | Config / CLI | Behavior |
 * | --- | --- | --- |
 * | `auto` | `discover=auto` / `--discover auto` | Bonjour first, NDI SDK merge/fallback |
 * | `bonjour` | `discover=bonjour` | Custom mDNS `_ndi._tcp` only |
 * | `ndi_sdk` | `discover=ndi_sdk` | SDK finder only |
 *
 * ## Pick rules (shared)
 *
 * Same as GhostVidStream auto-connect: `source` → `ip` (+ prefer HX) → first HX → index 0.
 */
