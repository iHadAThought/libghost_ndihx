/**
 * @file ghost_ndihx.h
 * @brief libghost_ndihx — NDI|HX receive API (first GhostVidStream decoder plugin).
 *
 * UI-free NDI|HX library for Linux aarch64 + x86_64. GhostVidStream is the
 * multi-protocol shell; this header is the **NDI|HX** module only. Hosts may:
 *   A) Call `ghost_ndihx_*` directly (this header), or
 *   B) Use protocol-agnostic `media_core.h` after `ghost_ndihx_register_media_module()`.
 *
 * Typical native embed flow:
 *   1. ghost_ndihx_init()
 *   2. ghost_ndihx_options_defaults(&opt);  optionally load a config / set filters
 *   3. session = ghost_ndihx_session_create(&opt)
 *   4. ghost_ndihx_connect_auto(session)   // discovers + picks + connects
 *   5. loop: ghost_ndihx_capture_newest(session, &frame)  // newest-frame drain
 *   6. ghost_ndihx_session_destroy(session); ghost_ndihx_shutdown()
 *
 * Threading: one session is not thread-safe. Use one session per thread, or
 * external locking around capture/connect/disconnect.
 *
 * Dependencies: libndi (NDI SDK v6), FFmpeg >= 7 for HX.
 * See README.md, docs/integration.md, docs/modular-compatibility.md.
 */
#ifndef GHOST_NDIHX_H
#define GHOST_NDIHX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward-declare so hosts can register without pulling all of media_core.h
 * into every translation unit. Full type is in media_core.h. */
struct media_module;

/** Library / receiver name reported to NDI peers (override via options). */
#define GHOST_NDIHX_DEFAULT_RECV_NAME "GhostVidStream"

/** Max sources returned by a single discover call. */
#define GHOST_NDIHX_MAX_SOURCES 64

/** Opaque session: finder + optional receiver + last video frame ownership. */
typedef struct ghost_ndihx_session ghost_ndihx_session_t;

/** Receiver bandwidth — maps to NDIlib_recv_bandwidth_*. */
typedef enum ghost_ndihx_bandwidth {
  GHOST_NDIHX_BW_HIGHEST = 0, /**< Full resolution / quality (default). */
  GHOST_NDIHX_BW_LOWEST = 1   /**< Lower res/bitrate from sender — less CPU/RAM. */
} ghost_ndihx_bandwidth_t;

/**
 * One discovered NDI source. Strings are always NUL-terminated; empty if unknown.
 * `url` is typically "host:port" (useful for --ip style matching).
 */
typedef struct ghost_ndihx_source {
  char name[256];
  char url[256];
  bool is_hx; /**< Heuristic: name looks like NDI|HX / HX-Stream / (HX). */
} ghost_ndihx_source_t;

/**
 * Session options. All string fields are owned by the caller (copied into the
 * session at create time). Use ghost_ndihx_options_defaults() then override.
 *
 * Note: resolution / fps / Hz of the *camera encode* are controlled by the
 * sender. This API exposes receiver-side efficiency knobs (bandwidth) plus
 * filters for auto-connect. Display fps/Hz caps belong in the host UI.
 */
typedef struct ghost_ndihx_options {
  char source_substr[256]; /**< Prefer name/url containing this (empty = any). */
  char ip_substr[128];     /**< Prefer name/url containing this IP/host. */
  char recv_name[128];     /**< NDI receiver name shown to peers. */

  bool prefer_hx;   /**< Prefer HX-looking sources when picking (default true). */
  bool auto_search; /**< connect_auto keeps rescanning until a match. */
  bool show_local;  /**< Include local machine sources (default true). */

  int find_ms;   /**< Wait for discovery (ms), min 500. */
  int rescan_ms; /**< Pause between auto-search attempts (ms). */
  /**
   * First capture wait (ms) inside ghost_ndihx_capture_newest before a non-blocking
   * drain. Lower = snappier latency; higher = fewer idle wakeups. Default 8.
   * Clamped to 0..100. Does not change resolution/quality.
   */
  int capture_wait_ms;

  ghost_ndihx_bandwidth_t bandwidth;
} ghost_ndihx_options_t;

/**
 * Latest video frame view. Memory is owned by the session until the next
 * successful capture, disconnect, bandwidth reconnect, or destroy.
 * Pixel layout: BGRX (4 bytes/pixel), `stride` bytes per row.
 */
typedef struct ghost_ndihx_frame {
  const uint8_t *data;
  int width;
  int height;
  int stride;
  uint32_t fourcc; /**< NDI FourCC as delivered (usually BGRX). */
  int frame_rate_n;
  int frame_rate_d; /**< Source timing: fps ≈ n/d when d > 0. */
  uint64_t dropped; /**< Frames freed this call while draining to newest. */
} ghost_ndihx_frame_t;

/** Initialize NDI runtime (refcounted). Returns 0 on success, -1 on failure. */
int ghost_ndihx_init(void);

/** Final shutdown when the last ghost_ndihx_init() has been balanced. Safe to call extra. */
void ghost_ndihx_shutdown(void);

/** Fill options with safe defaults (HX preferred, auto-search on, highest BW). */
void ghost_ndihx_options_defaults(ghost_ndihx_options_t *opt);

/**
 * Load key=value settings from a file into @p opt (does not clear unspecified keys).
 * Supported keys: source, ip, prefer_hx, auto|auto_search, find_ms, rescan_ms,
 * capture_wait_ms, bandwidth (highest|lowest), recv_name.
 * Returns 0 on success; on failure writes a message into @p err (may be NULL).
 */
int ghost_ndihx_options_load_file(ghost_ndihx_options_t *opt, const char *path, char *err,
                             size_t err_len);

/** Create a session (starts mDNS/NDI finder). Returns NULL on failure. */
ghost_ndihx_session_t *ghost_ndihx_session_create(const ghost_ndihx_options_t *opt);

/** Disconnect, free last frame, destroy finder/receiver, free session. */
void ghost_ndihx_session_destroy(ghost_ndihx_session_t *session);

/** Copy current options out of the session (including runtime bandwidth). */
void ghost_ndihx_session_get_options(const ghost_ndihx_session_t *session, ghost_ndihx_options_t *out);

/**
 * Wait up to @p wait_ms and copy up to @p cap sources into @p out.
 * Returns the number of sources written (0..cap), or -1 on error.
 */
int ghost_ndihx_discover(ghost_ndihx_session_t *session, ghost_ndihx_source_t *out, int cap, int wait_ms);

/**
 * Pick best index into @p sources using @p opt filters. Returns -1 if none match.
 * Does not require a live session (pure helper — easy to unit-test / reuse).
 */
int ghost_ndihx_pick(const ghost_ndihx_source_t *sources, int count, const ghost_ndihx_options_t *opt);

/**
 * Connect (or reconnect) to @p src with the session's bandwidth setting.
 * Returns 0 on success, -1 on failure. Previous connection is torn down first.
 */
int ghost_ndihx_connect(ghost_ndihx_session_t *session, const ghost_ndihx_source_t *src);

/** True if a receiver is currently connected. */
bool ghost_ndihx_is_connected(const ghost_ndihx_session_t *session);

/** Name/url of the connected source (empty strings if disconnected). */
void ghost_ndihx_connected_source(const ghost_ndihx_session_t *session, ghost_ndihx_source_t *out);

/**
 * Discover + pick + connect. If auto_search is set, blocks/retries until success
 * or @p cancel becomes non-zero (checked between attempts; may be NULL).
 * Returns 0 on success, -1 on failure / cancel.
 */
int ghost_ndihx_connect_auto(ghost_ndihx_session_t *session, volatile const int *cancel);

/** Drop the receiver and free any held frame; finder stays alive for rescans. */
void ghost_ndihx_disconnect(ghost_ndihx_session_t *session);

/**
 * Change bandwidth. If connected, reconnects to the same source.
 * Returns 0 on success, -1 if reconnect failed (session left disconnected).
 */
int ghost_ndihx_set_bandwidth(ghost_ndihx_session_t *session, ghost_ndihx_bandwidth_t bandwidth);

/** Update auto-search flag used by connect_auto / host reconnect loops. */
void ghost_ndihx_set_auto_search(ghost_ndihx_session_t *session, bool enabled);

/**
 * Capture the newest video frame (drains the queue; frees older frames).
 * Returns true if @p out is filled. Audio/metadata are discarded for latency.
 * Idle: returns false after capture_wait_ms (default 8) so callers can sleep lightly.
 *
 * Quality: with bandwidth=highest the receiver always requests full-resolution
 * BGRX/BGRA (never silent downscale). lowest bandwidth is an explicit opt-in.
 */
bool ghost_ndihx_capture_newest(ghost_ndihx_session_t *session, ghost_ndihx_frame_t *out);

/**
 * Drain the receive queue without returning a frame (frees video as it goes).
 * Use while the host is paused / not presenting so NDI buffers cannot pile up.
 * Returns the number of video frames freed this call.
 */
uint64_t ghost_ndihx_drain(ghost_ndihx_session_t *session);

/**
 * Current NDI receive queue depths (0 if disconnected). Useful for embeds that
 * want to assert low latency (video should stay near 0 after capture_newest).
 */
void ghost_ndihx_queue_depth(const ghost_ndihx_session_t *session, int *video, int *audio,
                        int *metadata);

/** Heuristic HX name check (exported for UIs that list sources themselves). */
bool ghost_ndihx_source_looks_hx(const char *name);

/** Human-readable bandwidth label. */
const char *ghost_ndihx_bandwidth_name(ghost_ndihx_bandwidth_t bandwidth);

/** Library version string (semver-ish). */
const char *ghost_ndihx_version(void);

/**
 * Live session capability bits (maps to media_caps_t). Re-probe after connect —
 * NDI PTZ support can appear a second or two after the receiver attaches.
 */
uint32_t ghost_ndihx_capabilities(const ghost_ndihx_session_t *session);

/** True if the connected source currently advertises NDI PTZ. */
bool ghost_ndihx_ptz_supported(const ghost_ndihx_session_t *session);

/**
 * PTZ ops (NDI recv PTZ path). Return 0 on success, -1 if unsupported/error.
 * Continuous move uses speed APIs; stop zeros speeds. Home recalls preset 0.
 * Absolute pose read is not available from the NDI SDK — ptz_get leaves
 * valid=false.
 */
int ghost_ndihx_ptz_get(ghost_ndihx_session_t *session, float *pan, float *tilt, float *zoom,
                   bool *valid);
int ghost_ndihx_ptz_move(ghost_ndihx_session_t *session, float pan, float tilt, float zoom,
                    bool continuous);
int ghost_ndihx_ptz_stop(ghost_ndihx_session_t *session);
int ghost_ndihx_ptz_home(ghost_ndihx_session_t *session);
int ghost_ndihx_ptz_preset(ghost_ndihx_session_t *session, int index, bool store);

/**
 * media_core integration: return the GhostVidStream NDI|HX module vtable (never NULL).
 * Call `ghost_ndihx_register_media_module()` once so `media_find_module("ghost_ndihx")` works.
 */
const struct media_module *ghost_ndihx_media_module(void);
int ghost_ndihx_register_media_module(void);

#ifdef __cplusplus
}
#endif

#endif /* GHOST_NDIHX_H */
