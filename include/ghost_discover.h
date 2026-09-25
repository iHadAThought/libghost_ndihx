/**
 * @file ghost_discover.h
 * @brief libghost_discover — pluggable LAN discovery backends for GhostVidStream.
 *
 * First-class discovery module (same product bar as media_core protocol plugins).
 * Protocol receivers (libghost_ndihx, future FULL NDI, GhostSpot bridge) consume
 * this API; they do **not** embed Bonjour/mDNS themselves.
 *
 * Backends (register at link time):
 *   - "bonjour"  — browse `_ndi._tcp` via Avahi (Linux) or dns_sd (macOS)
 *   - "ndi_sdk"  — NDI SDK finder (`NDIlib_find_*`); optional reuse of a live find
 *
 * Selection: `GHOST_DISCOVER_AUTO` tries Bonjour first, then NDI SDK if empty
 * or Bonjour unavailable. Hosts may force a single backend via options / CLI.
 *
 * Typical embed:
 *   ghost_discover_options_t opt;
 *   ghost_discover_options_defaults(&opt);
 *   opt.backend = GHOST_DISCOVER_AUTO;
 *   ghost_discover_source_t src[GHOST_DISCOVER_MAX_SOURCES];
 *   int n = ghost_discover_browse(&opt, src, GHOST_DISCOVER_MAX_SOURCES);
 *   int i = ghost_discover_pick(src, n, "HX", "172.16.1.189", true);
 */
#ifndef GHOST_DISCOVER_H
#define GHOST_DISCOVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GHOST_DISCOVER_VERSION "0.1.0"
#define GHOST_DISCOVER_MAX_SOURCES 64
#define GHOST_DISCOVER_MAX_BACKENDS 8

/** Which discovery path to use. */
typedef enum ghost_discover_backend_id {
  GHOST_DISCOVER_AUTO = 0,    /**< Bonjour first; NDI SDK fallback if empty. */
  GHOST_DISCOVER_BONJOUR = 1, /**< Custom mDNS `_ndi._tcp` only. */
  GHOST_DISCOVER_NDI_SDK = 2  /**< NDI SDK finder only. */
} ghost_discover_backend_id_t;

/**
 * One discovered source. Strings NUL-terminated; empty if unknown.
 * `url` is typically "host:port" (IPv4 preferred when resolved).
 */
typedef struct ghost_discover_source {
  char name[256];
  char url[256];
  bool is_hx;      /**< Heuristic HX name (hx-stream / ndi|hx / (hx)). */
  bool via_mdns;   /**< True when found via Bonjour/Avahi path. */
  char backend[32]; /**< Backend id that produced this entry ("bonjour"|"ndi_sdk"). */
} ghost_discover_source_t;

/**
 * Browse options. `ndi_find` may be an existing `NDIlib_find_instance_t` when
 * the host already owns a finder (session reuse); otherwise the NDI SDK backend
 * creates a short-lived finder internally (requires libndi linked).
 */
typedef struct ghost_discover_options {
  ghost_discover_backend_id_t backend;
  bool show_local; /**< Include local-machine sources (SDK backend). */
  int find_ms;     /**< Browse / wait budget (ms), min 500. */
  void *ndi_find;  /**< Optional opaque NDIlib_find_instance_t*. */
} ghost_discover_options_t;

/**
 * Backend vtable — modules implement browse + availability.
 * `id` is a stable string ("bonjour", "ndi_sdk").
 */
typedef struct ghost_discover_backend {
  const char *id;
  const char *label; /**< Human label for logs / UI. */
  bool (*available)(void);
  /**
   * Browse LAN. Write up to @p cap sources into @p out.
   * Return count written (0..cap), or -1 on hard error.
   */
  int (*browse)(const ghost_discover_options_t *opt, ghost_discover_source_t *out, int cap);
} ghost_discover_backend_t;

/** Fill safe defaults (AUTO, show_local, find_ms=4000). */
void ghost_discover_options_defaults(ghost_discover_options_t *opt);

/** Parse "auto"|"bonjour"|"ndi_sdk"|"ndi-sdk"|"sdk" → id. Returns false if unknown. */
bool ghost_discover_backend_parse(const char *s, ghost_discover_backend_id_t *out);

/** Stable name for config/CLI ("auto", "bonjour", "ndi_sdk"). */
const char *ghost_discover_backend_name(ghost_discover_backend_id_t id);

/** True if the named backend is compiled in and usable on this host. */
bool ghost_discover_backend_available(ghost_discover_backend_id_t id);

/** Register a backend (idempotent replace on same id). Returns 0 / -1. */
int ghost_discover_register_backend(const ghost_discover_backend_t *backend);

/** Built-in registration (Bonjour + NDI SDK). Safe to call more than once. */
int ghost_discover_register_builtins(void);

/** Lookup registered backend by id string. */
const ghost_discover_backend_t *ghost_discover_find_backend(const char *id);

/** Enumerate registered backends into @p out (up to @p cap). Returns count. */
int ghost_discover_list_backends(const ghost_discover_backend_t **out, int cap);

/**
 * Browse using @p opt->backend policy. Registers builtins on first use.
 * Returns number of sources (0..cap), or -1 on error.
 */
int ghost_discover_browse(const ghost_discover_options_t *opt, ghost_discover_source_t *out,
                          int cap);

/**
 * Pick best index using the same rules as GhostVidStream NDI auto-connect:
 *   1. source_substr match on name/url (first hit)
 *   2. else ip_substr among matches (prefer HX if prefer_hx)
 *   3. else prefer_hx → first HX-looking
 *   4. else index 0
 * Returns -1 if none match.
 */
int ghost_discover_pick(const ghost_discover_source_t *sources, int count,
                        const char *source_substr, const char *ip_substr, bool prefer_hx);

/** Shared HX name heuristic (exported for UIs / bridges). */
bool ghost_discover_source_looks_hx(const char *name);

const char *ghost_discover_version(void);

/* Built-in backend accessors (also registered by ghost_discover_register_builtins). */
const ghost_discover_backend_t *ghost_discover_bonjour_backend(void);
const ghost_discover_backend_t *ghost_discover_ndi_sdk_backend(void);

#ifdef __cplusplus
}
#endif

#endif /* GHOST_DISCOVER_H */
