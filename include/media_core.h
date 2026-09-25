/**
 * @file media_core.h
 * @brief Protocol-agnostic media receive core for GhostVidStream modules.
 *
 * GhostVidStream is the multi-protocol viewer / receive shell. Modules plug in:
 *   - ghost_ndihx   (NDI|HX — implemented)
 *   - srt           (SRT — implemented)
 *   - rtmp          (RTMP / HTTP-FLV — implemented)
 *   - ndi_full      (FULL NDI — planned)
 *   - st2110        (SMPTE 2110 — planned)
 *   - rtsp          (RTSP/RTP — implemented)
 *
 * This header has **no** NDI / FFmpeg / GStreamer types. Modules implement
 * `media_module_t` and register with `media_register_module()`. Hosts pick a
 * module by id, open a session, discover/connect, then pull BGRX (or module-
 * documented) frames via `capture_newest`.
 *
 * The NDI|HX decoder also exposes a native API in `ghost_ndihx.h` for embeds that
 * only need that protocol; it registers itself with media_core when linked.
 *
 * See docs/modular-compatibility.md and (Project store) modular-protocol-plan.
 */
#ifndef MEDIA_CORE_H
#define MEDIA_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_CORE_VERSION "0.1.0"
#define MEDIA_MAX_MODULES 16
#define MEDIA_MAX_SOURCES 64

/** Stable protocol ids for UI / config (new ids are additive only). */
typedef enum media_protocol_id {
  MEDIA_PROTO_UNKNOWN = 0,
  MEDIA_PROTO_NDI_HX = 1,
  MEDIA_PROTO_NDI_FULL = 2,
  MEDIA_PROTO_ST2110 = 3,
  MEDIA_PROTO_RTSP = 4,
  MEDIA_PROTO_SRT = 5,
  MEDIA_PROTO_RTMP = 6
} media_protocol_id_t;

/**
 * Discovered source — protocol-agnostic.
 * `url` may be host:port, rtsp://…, or SDP path depending on module.
 * `tag` is a short module-specific flag (e.g. "hx", "full", "jpeg").
 */
typedef struct media_source {
  char name[256];
  char url[256];
  char tag[64];
  media_protocol_id_t protocol;
} media_source_t;

/**
 * Newest video frame view. Pixel layout is module-defined; NDI|HX uses BGRX.
 * Memory owned by the module session until the next capture/disconnect/close.
 */
typedef struct media_frame {
  const uint8_t *data;
  int width;
  int height;
  int stride;
  uint32_t fourcc; /**< Module FourCC / pixel tag; 0 if unused. */
  int frame_rate_n;
  int frame_rate_d;
  uint64_t dropped; /**< Stale frames freed while draining to newest. */
} media_frame_t;

/**
 * Common open filters shared by modules. Protocol-specific knobs go in
 * `protocol_opts` (e.g. pointer to ghost_ndihx_options_t). Modules copy what they need.
 */
typedef struct media_open_params {
  char source_substr[256];
  char ip_substr[128];
  char label[128]; /**< Receiver / client name shown to peers when applicable. */
  bool auto_search;
  int find_ms;
  int rescan_ms;
  void *protocol_opts; /**< Optional; module-defined. */
} media_open_params_t;

/** Opaque per-module session (modules cast to their private type). */
typedef struct media_session media_session_t;

/** Capability bitfield (modules report what the live session supports). */
typedef uint32_t media_caps_t;
#define MEDIA_CAP_NONE 0u
#define MEDIA_CAP_PTZ (1u << 0)            /**< Any PTZ control available. */
#define MEDIA_CAP_PTZ_CONTINUOUS (1u << 1) /**< Velocity / hold-to-move. */
#define MEDIA_CAP_PTZ_ABSOLUTE (1u << 2)   /**< Absolute pan/tilt/zoom set. */
#define MEDIA_CAP_PTZ_HOME (1u << 3)       /**< Home / preset 0. */
#define MEDIA_CAP_PTZ_PRESET (1u << 4)     /**< Store/recall numbered presets. */

/**
 * PTZ pose / speeds. Conventions (modules document deviations):
 *  - Absolute pan/tilt: -1..+1; zoom: 0 (wide) .. 1 (tele) unless noted.
 *  - Continuous speeds: -1..+1 (0 = stop).
 *  - `valid` false means values are unknown / unread.
 */
typedef struct media_ptz_state {
  float pan;
  float tilt;
  float zoom;
  bool valid;
} media_ptz_state_t;

/** Relative or continuous move. Speeds used when MEDIA_CAP_PTZ_CONTINUOUS. */
typedef struct media_ptz_move {
  float pan;  /**< Delta or speed. */
  float tilt;
  float zoom;
  bool continuous; /**< true = velocity until stop; false = relative step. */
} media_ptz_move_t;

/**
 * Module vtable. Required unless marked optional.
 * Sessions returned by `open` are closed with `close` (not free()'d by core).
 * Optional PTZ ops may be NULL; stubs return -1 / MEDIA_CAP_NONE.
 */
typedef struct media_module {
  const char *id; /**< Stable id: "ghost_ndihx", "ndi_full", "st2110", "rtsp". */
  media_protocol_id_t protocol;
  const char *description;

  int (*init)(void);
  void (*shutdown)(void);

  media_session_t *(*open)(const media_open_params_t *params);
  void (*close)(media_session_t *session);

  int (*discover)(media_session_t *session, media_source_t *out, int cap, int wait_ms);
  int (*connect)(media_session_t *session, const media_source_t *src);
  int (*connect_auto)(media_session_t *session, volatile const int *cancel);
  void (*disconnect)(media_session_t *session);
  bool (*is_connected)(const media_session_t *session);
  void (*connected_source)(const media_session_t *session, media_source_t *out);

  bool (*capture_newest)(media_session_t *session, media_frame_t *out);

  /** Optional: live session capability probe (re-query after connect). */
  media_caps_t (*capabilities)(const media_session_t *session);

  /** Optional PTZ — return 0 on success, -1 unsupported / error. */
  int (*ptz_get)(media_session_t *session, media_ptz_state_t *out);
  int (*ptz_move)(media_session_t *session, const media_ptz_move_t *move);
  int (*ptz_stop)(media_session_t *session);
  int (*ptz_home)(media_session_t *session);
  /** store=true stores preset @p index; false recalls it. */
  int (*ptz_preset)(media_session_t *session, int index, bool store);
} media_module_t;

/** Fill open-params with safe common defaults (auto_search on, find 4s). */
void media_open_params_defaults(media_open_params_t *params);

/**
 * Register a module (idempotent on same pointer/id).
 * Returns 0 on success, -1 if table full or invalid.
 */
int media_register_module(const media_module_t *module);

/** Lookup by id ("ghost_ndihx"). NULL if not registered / not linked. */
const media_module_t *media_find_module(const char *id);

/** Copy up to @p cap registered module pointers into @p out. Returns count. */
int media_list_modules(const media_module_t **out, int cap);

/** Protocol id → short label for UI/logs. */
const char *media_protocol_name(media_protocol_id_t id);

/**
 * Generic pick helper: prefer source_substr, then ip_substr, else first.
 * Optional @p tag_prefer (e.g. "hx") wins among ip matches when non-NULL.
 * Returns index or -1.
 */
int media_pick_source(const media_source_t *sources, int count,
                      const media_open_params_t *params, const char *tag_prefer);

/** Core version string. */
const char *media_core_version(void);

/**
 * Safe capability query: returns MEDIA_CAP_NONE if module/session missing or
 * `capabilities` is NULL.
 */
media_caps_t media_session_caps(const media_module_t *module,
                                const media_session_t *session);

/** True if @p caps includes MEDIA_CAP_PTZ. */
bool media_caps_has_ptz(media_caps_t caps);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_CORE_H */
