# RTSP / SRT discovery — what exists (and what we skip)

GhostVidStream’s **NDI** path has real LAN auto-discovery via
[`libghost_discover`](ndi-discovery.md) (`_ndi._tcp` Bonjour + NDI SDK finder).

**RTSP** and **SRT** do **not** get a parallel Bonjour-style module in this tree.
Their receive APIs stay **URL-first**. This page records the standards check so we
do not invent fake LAN scanners.

## Research summary (in-scope + standards)

| Protocol | Credible discovery systems (industry) | In GhostVidStream / GhostSpot / theaterLightController | On AIDA cam `172.16.1.189` |
| --- | --- | --- | --- |
| **NDI** | Bonjour `_ndi._tcp`; NDI SDK finder | Implemented (`libghost_discover`) | Advertises `_ndi._tcp` |
| **RTSP** | ONVIF **WS-Discovery** (UDP/3702); occasional mDNS `_rtsp._tcp` / `_onvif._tcp`; vendor UPnS | **None.** `ghost_rtsp_discover` only synthesizes `rtsp://…` from options. GhostSpot Add Camera: discover `type=rtsp` → **400**; RTSP is `legacyNotCreatable` | RTSP `:554` open; **WS-Discovery `:3702` closed**; Avahi browse `_rtsp._tcp` / `_onvif._tcp` → **0** |
| **SRT** | No NDI-like LAN finder. Connection is **URL + mode** (caller/listener/rendezvous). SAP/SDP (RFC 2974) is for **multicast MPEG-TS** announcements — not Haivision SRT caller URLs | **None.** `ghost_srt_discover` synthesizes `srt://…` from IP/url options | SRT listen port may be off (`:1600` closed when not enabled); Avahi `_srt._tcp` → **0** |

Booth Avahi browse (4 s, service-type only — not a host scan):

```
_ndi._tcp: 1  → HD-NDI-X20 (HX-Stream-172.16.1.189)
_rtsp._tcp: 0
_srt._tcp: 0
_onvif._tcp: 0
```

## Why we skip modular RTSP/SRT discovery modules

1. **No sender advertisement** on the show camera for ONVIF WS-Discovery or RTSP/SRT mDNS.
2. **No in-repo consumer** expecting RTSP/SRT LAN browse (GhostSpot explicitly rejects RTSP discover).
3. **SRT has no standard Bonjour/finder equivalent** for the caller/listener URLs we use (`srt://host:1600?mode=caller&…`).
4. Building a “discovery” that port-sweeps the LAN would violate the network allowlist / “no inventing scanners” rule and is not a real protocol discovery system.

## Current behavior (intentional)

| Module | `*_discover` | `*_connect_auto` |
| --- | --- | --- |
| `ghost_rtsp` | Returns one synthetic source from `url` / `ip` + default path (`/stream/main`) | Retries opening that URL when `auto_search` |
| `ghost_srt` | Returns one synthetic source from `url` / `ip` + default port/query | Same |

Use:

```bash
ghostvidstream --protocol rtsp --url rtsp://172.16.1.189:554/stream/main
ghostvidstream --protocol srt --url 'srt://172.16.1.189:1600?mode=caller&latency=120&streamid=r=0'
```

## Future (only if a real advertiser appears)

| If we see… | Then consider… |
| --- | --- |
| ONVIF WS-Discovery responses from show cameras | Modular `libghost_discover` backend `onvif` → RTSP URLs (still no subnet scan) |
| Stable mDNS `_rtsp._tcp` / `_onvif._tcp` on the LAN | Bonjour backend entries for those types |
| Vendor SRT announce API | Documented vendor backend — not generic SRT |

Until then: **URL-only** for RTSP/SRT; **Bonjour+SDK** only for NDI.
