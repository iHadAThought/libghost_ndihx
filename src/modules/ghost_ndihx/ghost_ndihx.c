/**
 * @file ghost_ndihx.c
 * @brief NDI|HX protocol module + native ghost_ndihx_* API.
 *
 * First pluggable module under media_core. Native `ghost_ndihx_*` calls stay stable
 * for direct embeds; `ghost_ndihx_media_module()` exposes the same session via the
 * protocol-agnostic vtable for a future multi-protocol main app.
 *
 * Design goals (docs/integration.md, docs/modular-compatibility.md):
 *  - Newest-frame drain for near-zero latency (drop stale frames).
 *  - Explicit free of every superseded NDI video frame (no queue growth / leaks).
 *  - No UI deps — safe to link from Electron, Qt, Python ctypes, etc.
 *  - Portable aarch64 and x86_64 Linux (libndi path chosen by install-deps.sh).
 */
#include "ghost_ndihx.h"
#include "media_core.h"

#include <Processing.NDI.Lib.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define GHOST_NDIHX_VERSION_STR "0.4.0"

/* -------------------------------------------------------------------------- */
/* Internal session                                                           */
/* -------------------------------------------------------------------------- */

struct ghost_ndihx_session {
  ghost_ndihx_options_t opt;
  NDIlib_find_instance_t find;
  NDIlib_recv_instance_t recv;
  ghost_ndihx_source_t connected;
  NDIlib_video_frame_v2_t video; /* owned while have_video */
  bool have_video;
  bool ndi_ok;
};

static int g_init_count = 0;

/* -------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* -------------------------------------------------------------------------- */

static void sleep_ms(int ms) {
  if (ms <= 0)
    return;
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    /* retry remaining */
  }
}

static void trim_inplace(char *s) {
  char *start = s;
  while (*start && isspace((unsigned char)*start))
    start++;
  if (start != s)
    memmove(s, start, strlen(start) + 1);
  size_t n = strlen(s);
  while (n > 0 && isspace((unsigned char)s[n - 1]))
    s[--n] = '\0';
}

static bool parse_bool(const char *v, bool *out) {
  if (!v)
    return false;
  if (!strcasecmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes") ||
      !strcasecmp(v, "on")) {
    *out = true;
    return true;
  }
  if (!strcasecmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "no") ||
      !strcasecmp(v, "off")) {
    *out = false;
    return true;
  }
  return false;
}

static bool name_match(const char *hay, const char *needle) {
  if (!needle || !*needle)
    return true;
  if (!hay)
    return false;
  return strcasestr(hay, needle) != NULL;
}

static NDIlib_recv_bandwidth_e to_ndi_bw(ghost_ndihx_bandwidth_t bw) {
  return bw == GHOST_NDIHX_BW_LOWEST ? NDIlib_recv_bandwidth_lowest
                                : NDIlib_recv_bandwidth_highest;
}

static void free_video(ghost_ndihx_session_t *s) {
  if (s->have_video && s->recv) {
    NDIlib_recv_free_video_v2(s->recv, &s->video);
    memset(&s->video, 0, sizeof(s->video));
    s->have_video = false;
  }
}

static void clamp_options(ghost_ndihx_options_t *opt) {
  if (opt->find_ms < 500)
    opt->find_ms = 500;
  if (opt->rescan_ms < 500)
    opt->rescan_ms = 500;
  if (opt->capture_wait_ms < 0)
    opt->capture_wait_ms = 0;
  if (opt->capture_wait_ms > 100)
    opt->capture_wait_ms = 100;
  if (!opt->recv_name[0])
    snprintf(opt->recv_name, sizeof(opt->recv_name), "%s", GHOST_NDIHX_DEFAULT_RECV_NAME);
}

/* -------------------------------------------------------------------------- */
/* Public: lifecycle                                                          */
/* -------------------------------------------------------------------------- */

const char *ghost_ndihx_version(void) { return GHOST_NDIHX_VERSION_STR; }

const char *ghost_ndihx_bandwidth_name(ghost_ndihx_bandwidth_t bandwidth) {
  return bandwidth == GHOST_NDIHX_BW_LOWEST ? "lowest" : "highest";
}

bool ghost_ndihx_source_looks_hx(const char *name) {
  if (!name || !*name)
    return false;
  char buf[512];
  size_t n = strlen(name);
  if (n >= sizeof(buf))
    n = sizeof(buf) - 1;
  for (size_t i = 0; i < n; i++)
    buf[i] = (char)tolower((unsigned char)name[i]);
  buf[n] = '\0';
  if (strstr(buf, "hx-stream"))
    return true;
  if (strstr(buf, "ndi|hx"))
    return true;
  if (strstr(buf, "(hx)") || strstr(buf, "(hx2)") || strstr(buf, "(hx3)"))
    return true;
  return false;
}

int ghost_ndihx_init(void) {
  if (g_init_count > 0) {
    g_init_count++;
    return 0;
  }
  if (!NDIlib_initialize())
    return -1;
  g_init_count = 1;
  return 0;
}

void ghost_ndihx_shutdown(void) {
  if (g_init_count <= 0)
    return;
  g_init_count--;
  if (g_init_count == 0)
    NDIlib_destroy();
}

void ghost_ndihx_options_defaults(ghost_ndihx_options_t *opt) {
  if (!opt)
    return;
  memset(opt, 0, sizeof(*opt));
  opt->prefer_hx = true;
  opt->auto_search = true;
  opt->show_local = true;
  opt->find_ms = 4000;
  opt->rescan_ms = 3000;
  opt->capture_wait_ms = 8;
  opt->bandwidth = GHOST_NDIHX_BW_HIGHEST;
  snprintf(opt->recv_name, sizeof(opt->recv_name), "%s", GHOST_NDIHX_DEFAULT_RECV_NAME);
}

static int apply_kv(ghost_ndihx_options_t *opt, const char *key, const char *val) {
  if (!strcmp(key, "source")) {
    snprintf(opt->source_substr, sizeof(opt->source_substr), "%s", val ? val : "");
  } else if (!strcmp(key, "ip")) {
    snprintf(opt->ip_substr, sizeof(opt->ip_substr), "%s", val ? val : "");
  } else if (!strcmp(key, "recv_name")) {
    snprintf(opt->recv_name, sizeof(opt->recv_name), "%s", val ? val : "");
  } else if (!strcmp(key, "prefer_hx")) {
    return parse_bool(val, &opt->prefer_hx) ? 0 : -1;
  } else if (!strcmp(key, "auto") || !strcmp(key, "auto_search")) {
    return parse_bool(val, &opt->auto_search) ? 0 : -1;
  } else if (!strcmp(key, "show_local")) {
    return parse_bool(val, &opt->show_local) ? 0 : -1;
  } else if (!strcmp(key, "find_ms")) {
    opt->find_ms = atoi(val);
  } else if (!strcmp(key, "rescan_ms")) {
    opt->rescan_ms = atoi(val);
  } else if (!strcmp(key, "capture_wait_ms")) {
    opt->capture_wait_ms = atoi(val);
  } else if (!strcmp(key, "bandwidth")) {
    if (!strcasecmp(val, "highest") || !strcasecmp(val, "high") || !strcasecmp(val, "full"))
      opt->bandwidth = GHOST_NDIHX_BW_HIGHEST;
    else if (!strcasecmp(val, "lowest") || !strcasecmp(val, "low"))
      opt->bandwidth = GHOST_NDIHX_BW_LOWEST;
    else
      return -1;
  } else {
    return -1;
  }
  return 0;
}

int ghost_ndihx_options_load_file(ghost_ndihx_options_t *opt, const char *path, char *err,
                             size_t err_len) {
  if (!opt || !path) {
    if (err && err_len)
      snprintf(err, err_len, "null argument");
    return -1;
  }
  FILE *f = fopen(path, "r");
  if (!f) {
    if (err && err_len)
      snprintf(err, err_len, "open %s: %s", path, strerror(errno));
    return -1;
  }
  char line[512];
  int lineno = 0;
  while (fgets(line, sizeof(line), f)) {
    lineno++;
    trim_inplace(line);
    if (!line[0] || line[0] == '#' || line[0] == ';')
      continue;
    char *eq = strchr(line, '=');
    if (!eq) {
      if (err && err_len)
        snprintf(err, err_len, "%s:%d: expected key=value", path, lineno);
      fclose(f);
      return -1;
    }
    *eq = '\0';
    char *key = line;
    char *val = eq + 1;
    trim_inplace(key);
    trim_inplace(val);
    size_t vn = strlen(val);
    if (vn >= 2 && ((val[0] == '"' && val[vn - 1] == '"') ||
                    (val[0] == '\'' && val[vn - 1] == '\''))) {
      val[vn - 1] = '\0';
      val++;
    }
    if (apply_kv(opt, key, val) != 0) {
      if (err && err_len)
        snprintf(err, err_len, "%s:%d: bad key/value for '%s'", path, lineno, key);
      fclose(f);
      return -1;
    }
  }
  fclose(f);
  clamp_options(opt);
  return 0;
}

ghost_ndihx_session_t *ghost_ndihx_session_create(const ghost_ndihx_options_t *opt) {
  if (ghost_ndihx_init() != 0)
    return NULL;

  ghost_ndihx_session_t *s = calloc(1, sizeof(*s));
  if (!s) {
    ghost_ndihx_shutdown();
    return NULL;
  }
  if (opt)
    s->opt = *opt;
  else
    ghost_ndihx_options_defaults(&s->opt);
  clamp_options(&s->opt);
  s->ndi_ok = true;

  NDIlib_find_create_t find_desc;
  memset(&find_desc, 0, sizeof(find_desc));
  find_desc.show_local_sources = s->opt.show_local;
  s->find = NDIlib_find_create_v2(&find_desc);
  if (!s->find) {
    free(s);
    ghost_ndihx_shutdown();
    return NULL;
  }
  return s;
}

void ghost_ndihx_session_destroy(ghost_ndihx_session_t *session) {
  if (!session)
    return;
  ghost_ndihx_disconnect(session);
  if (session->find) {
    NDIlib_find_destroy(session->find);
    session->find = NULL;
  }
  free(session);
  ghost_ndihx_shutdown();
}

void ghost_ndihx_session_get_options(const ghost_ndihx_session_t *session, ghost_ndihx_options_t *out) {
  if (!session || !out)
    return;
  *out = session->opt;
}

/* -------------------------------------------------------------------------- */
/* Discovery / pick / connect                                                 */
/* -------------------------------------------------------------------------- */

int ghost_ndihx_discover(ghost_ndihx_session_t *session, ghost_ndihx_source_t *out, int cap, int wait_ms) {
  if (!session || !session->find || !out || cap <= 0)
    return -1;
  if (wait_ms < 0)
    wait_ms = 0;
  NDIlib_find_wait_for_sources(session->find, (uint32_t)wait_ms);
  uint32_t count = 0;
  const NDIlib_source_t *sources = NDIlib_find_get_current_sources(session->find, &count);
  if (!sources || count == 0)
    return 0;
  int n = (int)count < cap ? (int)count : cap;
  for (int i = 0; i < n; i++) {
    memset(&out[i], 0, sizeof(out[i]));
    snprintf(out[i].name, sizeof(out[i].name), "%s",
             sources[i].p_ndi_name ? sources[i].p_ndi_name : "");
    snprintf(out[i].url, sizeof(out[i].url), "%s",
             sources[i].p_url_address ? sources[i].p_url_address : "");
    out[i].is_hx = ghost_ndihx_source_looks_hx(out[i].name);
  }
  return n;
}

int ghost_ndihx_pick(const ghost_ndihx_source_t *sources, int count, const ghost_ndihx_options_t *opt) {
  if (!sources || count <= 0 || !opt)
    return -1;

  if (opt->source_substr[0]) {
    for (int i = 0; i < count; i++) {
      if (name_match(sources[i].name, opt->source_substr) ||
          name_match(sources[i].url, opt->source_substr))
        return i;
    }
    return -1;
  }

  if (opt->ip_substr[0]) {
    int hx = -1, any = -1;
    for (int i = 0; i < count; i++) {
      if (!name_match(sources[i].name, opt->ip_substr) &&
          !name_match(sources[i].url, opt->ip_substr))
        continue;
      if (any < 0)
        any = i;
      if (sources[i].is_hx || ghost_ndihx_source_looks_hx(sources[i].name)) {
        hx = i;
        break;
      }
    }
    if (opt->prefer_hx && hx >= 0)
      return hx;
    return any;
  }

  if (opt->prefer_hx) {
    for (int i = 0; i < count; i++) {
      if (sources[i].is_hx || ghost_ndihx_source_looks_hx(sources[i].name))
        return i;
    }
  }
  return 0;
}

void ghost_ndihx_disconnect(ghost_ndihx_session_t *session) {
  if (!session)
    return;
  free_video(session);
  if (session->recv) {
    NDIlib_recv_destroy(session->recv);
    session->recv = NULL;
  }
  memset(&session->connected, 0, sizeof(session->connected));
}

bool ghost_ndihx_is_connected(const ghost_ndihx_session_t *session) {
  return session && session->recv != NULL;
}

void ghost_ndihx_connected_source(const ghost_ndihx_session_t *session, ghost_ndihx_source_t *out) {
  if (!out)
    return;
  if (!session) {
    memset(out, 0, sizeof(*out));
    return;
  }
  *out = session->connected;
}

int ghost_ndihx_connect(ghost_ndihx_session_t *session, const ghost_ndihx_source_t *src) {
  if (!session || !src || !src->name[0])
    return -1;

  ghost_ndihx_disconnect(session);

  NDIlib_source_t selected;
  memset(&selected, 0, sizeof(selected));
  selected.p_ndi_name = src->name;
  selected.p_url_address = src->url[0] ? src->url : NULL;

  NDIlib_recv_create_v3_t recv_desc;
  memset(&recv_desc, 0, sizeof(recv_desc));
  recv_desc.source_to_connect_to = selected;
  /*
   * Always request BGRX/BGRA. This is a portable host layout (SDL/OpenGL/CPU)
   * and — with bandwidth=highest — preserves full sender resolution. We never
   * use NDIlib_recv_color_format_fastest here: that can silently deliver UYVY
   * and force every embed to add a convert path (or look wrong).
   */
  recv_desc.color_format = NDIlib_recv_color_format_BGRX_BGRA;
  recv_desc.bandwidth = to_ndi_bw(session->opt.bandwidth);
  recv_desc.allow_video_fields = false; /* progressive only — simpler + lower latency */
  recv_desc.p_ndi_recv_name = session->opt.recv_name;

  session->recv = NDIlib_recv_create_v3(&recv_desc);
  if (!session->recv)
    return -1;

  session->connected = *src;
  session->connected.is_hx =
      src->is_hx || ghost_ndihx_source_looks_hx(src->name);
  return 0;
}

int ghost_ndihx_connect_auto(ghost_ndihx_session_t *session, volatile const int *cancel) {
  if (!session)
    return -1;

  for (;;) {
    if (cancel && *cancel)
      return -1;

    ghost_ndihx_source_t sources[GHOST_NDIHX_MAX_SOURCES];
    int n = ghost_ndihx_discover(session, sources, GHOST_NDIHX_MAX_SOURCES, session->opt.find_ms);
    if (n < 0)
      return -1;

    int idx = ghost_ndihx_pick(sources, n, &session->opt);
    if (idx >= 0) {
      if (ghost_ndihx_connect(session, &sources[idx]) == 0)
        return 0;
      /* connect failed — fall through to retry if auto_search */
    }

    if (!session->opt.auto_search)
      return -1;

    sleep_ms(session->opt.rescan_ms);
  }
}

int ghost_ndihx_set_bandwidth(ghost_ndihx_session_t *session, ghost_ndihx_bandwidth_t bandwidth) {
  if (!session)
    return -1;
  session->opt.bandwidth = bandwidth;
  if (!session->recv)
    return 0;
  /* Bandwidth is fixed at recv create time — reconnect to apply. */
  ghost_ndihx_source_t src = session->connected;
  return ghost_ndihx_connect(session, &src);
}

void ghost_ndihx_set_auto_search(ghost_ndihx_session_t *session, bool enabled) {
  if (!session)
    return;
  session->opt.auto_search = enabled;
}

/* -------------------------------------------------------------------------- */
/* Capture (newest-frame drain)                                               */
/* -------------------------------------------------------------------------- */

/**
 * Non-blocking drain: keep only the newest video frame in session->video.
 * Continues past status_change so a PTZ/capability event cannot strand stale
 * frames in the NDI queue. Returns frames freed this call.
 */
static uint64_t drain_to_newest(ghost_ndihx_session_t *session) {
  uint64_t dropped = 0;
  for (;;) {
    NDIlib_video_frame_v2_t newer;
    NDIlib_frame_type_e t =
        NDIlib_recv_capture_v2(session->recv, &newer, NULL, NULL, 0);
    if (t == NDIlib_frame_type_video) {
      if (session->have_video) {
        NDIlib_recv_free_video_v2(session->recv, &session->video);
        dropped++;
      }
      session->video = newer;
      session->have_video = true;
      continue;
    }
    if (t == NDIlib_frame_type_none || t == NDIlib_frame_type_error)
      break;
    /* status_change (and any future non-video types): keep draining. */
  }

  /* Belt-and-suspenders: if the SDK still reports queued video, pull again. */
  NDIlib_recv_queue_t q;
  memset(&q, 0, sizeof(q));
  NDIlib_recv_get_queue(session->recv, &q);
  int guard = 0;
  while (q.video_frames > 0 && guard++ < 64) {
    NDIlib_video_frame_v2_t newer;
    if (NDIlib_recv_capture_v2(session->recv, &newer, NULL, NULL, 0) !=
        NDIlib_frame_type_video)
      break;
    if (session->have_video) {
      NDIlib_recv_free_video_v2(session->recv, &session->video);
      dropped++;
    }
    session->video = newer;
    session->have_video = true;
    memset(&q, 0, sizeof(q));
    NDIlib_recv_get_queue(session->recv, &q);
  }
  return dropped;
}

bool ghost_ndihx_capture_newest(ghost_ndihx_session_t *session, ghost_ndihx_frame_t *out) {
  if (!session || !session->recv || !out)
    return false;

  uint32_t wait_ms = (uint32_t)session->opt.capture_wait_ms;
  uint64_t dropped = 0;

  NDIlib_video_frame_v2_t frame;
  NDIlib_frame_type_e t =
      NDIlib_recv_capture_v2(session->recv, &frame, NULL, NULL, wait_ms);
  if (t == NDIlib_frame_type_video) {
    if (session->have_video) {
      NDIlib_recv_free_video_v2(session->recv, &session->video);
      dropped++;
    }
    session->video = frame;
    session->have_video = true;
  }

  dropped += drain_to_newest(session);

  if (!session->have_video)
    return false;

  memset(out, 0, sizeof(*out));
  out->data = (const uint8_t *)session->video.p_data;
  out->width = session->video.xres;
  out->height = session->video.yres;
  out->stride = session->video.line_stride_in_bytes;
  out->fourcc = (uint32_t)session->video.FourCC;
  out->frame_rate_n = session->video.frame_rate_N;
  out->frame_rate_d = session->video.frame_rate_D;
  out->dropped = dropped;
  return true;
}

uint64_t ghost_ndihx_drain(ghost_ndihx_session_t *session) {
  if (!session || !session->recv)
    return 0;
  /* Drop any held frame first so pause/idle cannot pin an old buffer forever
   * while still consuming the live queue (keeps RSS flat when not presenting). */
  uint64_t dropped = 0;
  if (session->have_video) {
    NDIlib_recv_free_video_v2(session->recv, &session->video);
    memset(&session->video, 0, sizeof(session->video));
    session->have_video = false;
    dropped++;
  }
  /* Capture with 0 wait then drain — discard everything. */
  NDIlib_video_frame_v2_t frame;
  if (NDIlib_recv_capture_v2(session->recv, &frame, NULL, NULL, 0) ==
      NDIlib_frame_type_video) {
    NDIlib_recv_free_video_v2(session->recv, &frame);
    dropped++;
  }
  /* Re-use drain helper but free the survivor too. */
  dropped += drain_to_newest(session);
  if (session->have_video) {
    NDIlib_recv_free_video_v2(session->recv, &session->video);
    memset(&session->video, 0, sizeof(session->video));
    session->have_video = false;
    dropped++;
  }
  return dropped;
}

void ghost_ndihx_queue_depth(const ghost_ndihx_session_t *session, int *video, int *audio,
                        int *metadata) {
  if (video)
    *video = 0;
  if (audio)
    *audio = 0;
  if (metadata)
    *metadata = 0;
  if (!session || !session->recv)
    return;
  NDIlib_recv_queue_t q;
  memset(&q, 0, sizeof(q));
  NDIlib_recv_get_queue(session->recv, &q);
  if (video)
    *video = q.video_frames;
  if (audio)
    *audio = q.audio_frames;
  if (metadata)
    *metadata = q.metadata_frames;
}

/* -------------------------------------------------------------------------- */
/* PTZ (NDI recv extensions — probe after connect)                            */
/* -------------------------------------------------------------------------- */

bool ghost_ndihx_ptz_supported(const ghost_ndihx_session_t *session) {
  if (!session || !session->recv)
    return false;
  return NDIlib_recv_ptz_is_supported(session->recv);
}

uint32_t ghost_ndihx_capabilities(const ghost_ndihx_session_t *session) {
  if (!ghost_ndihx_ptz_supported(session))
    return 0u;
  /* NDI exposes continuous speed + absolute set + presets. Pose read is N/A. */
  return (uint32_t)(MEDIA_CAP_PTZ | MEDIA_CAP_PTZ_CONTINUOUS | MEDIA_CAP_PTZ_ABSOLUTE |
                    MEDIA_CAP_PTZ_HOME | MEDIA_CAP_PTZ_PRESET);
}

int ghost_ndihx_ptz_get(ghost_ndihx_session_t *session, float *pan, float *tilt, float *zoom,
                   bool *valid) {
  (void)session;
  if (pan)
    *pan = 0.f;
  if (tilt)
    *tilt = 0.f;
  if (zoom)
    *zoom = 0.f;
  if (valid)
    *valid = false;
  /* NDI SDK has no standard absolute pose query. */
  return -1;
}

int ghost_ndihx_ptz_move(ghost_ndihx_session_t *session, float pan, float tilt, float zoom,
                    bool continuous) {
  if (!session || !session->recv || !NDIlib_recv_ptz_is_supported(session->recv))
    return -1;
  if (continuous) {
    if (!NDIlib_recv_ptz_pan_tilt_speed(session->recv, pan, tilt))
      return -1;
    if (!NDIlib_recv_ptz_zoom_speed(session->recv, zoom))
      return -1;
    return 0;
  }
  /* Relative step: nudge via short continuous pulse then stop. Absolute set
   * would need known pose; without it, treat non-continuous as a brief speed. */
  float sp = pan, st = tilt, sz = zoom;
  if (sp > 1.f)
    sp = 1.f;
  if (sp < -1.f)
    sp = -1.f;
  if (st > 1.f)
    st = 1.f;
  if (st < -1.f)
    st = -1.f;
  if (sz > 1.f)
    sz = 1.f;
  if (sz < -1.f)
    sz = -1.f;
  if (!NDIlib_recv_ptz_pan_tilt_speed(session->recv, sp, st))
    return -1;
  if (!NDIlib_recv_ptz_zoom_speed(session->recv, sz))
    return -1;
  sleep_ms(120);
  NDIlib_recv_ptz_pan_tilt_speed(session->recv, 0.f, 0.f);
  NDIlib_recv_ptz_zoom_speed(session->recv, 0.f);
  return 0;
}

int ghost_ndihx_ptz_stop(ghost_ndihx_session_t *session) {
  if (!session || !session->recv || !NDIlib_recv_ptz_is_supported(session->recv))
    return -1;
  bool ok = NDIlib_recv_ptz_pan_tilt_speed(session->recv, 0.f, 0.f);
  ok = NDIlib_recv_ptz_zoom_speed(session->recv, 0.f) && ok;
  return ok ? 0 : -1;
}

int ghost_ndihx_ptz_home(ghost_ndihx_session_t *session) {
  if (!session || !session->recv || !NDIlib_recv_ptz_is_supported(session->recv))
    return -1;
  return NDIlib_recv_ptz_recall_preset(session->recv, 0, 1.0f) ? 0 : -1;
}

int ghost_ndihx_ptz_preset(ghost_ndihx_session_t *session, int index, bool store) {
  if (!session || !session->recv || !NDIlib_recv_ptz_is_supported(session->recv))
    return -1;
  if (index < 0 || index > 99)
    return -1;
  if (store)
    return NDIlib_recv_ptz_store_preset(session->recv, index) ? 0 : -1;
  return NDIlib_recv_ptz_recall_preset(session->recv, index, 1.0f) ? 0 : -1;
}

/* -------------------------------------------------------------------------- */
/* media_core module adapter (keeps NDI types out of media_core.h)            */
/* -------------------------------------------------------------------------- */

static void source_to_media(const ghost_ndihx_source_t *in, media_source_t *out) {
  memset(out, 0, sizeof(*out));
  snprintf(out->name, sizeof(out->name), "%s", in->name);
  snprintf(out->url, sizeof(out->url), "%s", in->url);
  snprintf(out->tag, sizeof(out->tag), "%s",
           (in->is_hx || ghost_ndihx_source_looks_hx(in->name)) ? "hx" : "ndi");
  out->protocol = MEDIA_PROTO_NDI_HX;
}

static media_session_t *mod_open(const media_open_params_t *params) {
  ghost_ndihx_options_t opt;
  if (params && params->protocol_opts)
    opt = *(const ghost_ndihx_options_t *)params->protocol_opts;
  else
    ghost_ndihx_options_defaults(&opt);

  if (params) {
    if (params->source_substr[0])
      snprintf(opt.source_substr, sizeof(opt.source_substr), "%s", params->source_substr);
    if (params->ip_substr[0])
      snprintf(opt.ip_substr, sizeof(opt.ip_substr), "%s", params->ip_substr);
    if (params->label[0])
      snprintf(opt.recv_name, sizeof(opt.recv_name), "%s", params->label);
    opt.auto_search = params->auto_search;
    if (params->find_ms > 0)
      opt.find_ms = params->find_ms;
    if (params->rescan_ms > 0)
      opt.rescan_ms = params->rescan_ms;
  }

  return (media_session_t *)ghost_ndihx_session_create(&opt);
}

static void mod_close(media_session_t *session) {
  ghost_ndihx_session_destroy((ghost_ndihx_session_t *)session);
}

static int mod_discover(media_session_t *session, media_source_t *out, int cap, int wait_ms) {
  ghost_ndihx_source_t tmp[GHOST_NDIHX_MAX_SOURCES];
  int n = ghost_ndihx_discover((ghost_ndihx_session_t *)session, tmp, GHOST_NDIHX_MAX_SOURCES, wait_ms);
  if (n < 0)
    return -1;
  if (n > cap)
    n = cap;
  for (int i = 0; i < n; i++)
    source_to_media(&tmp[i], &out[i]);
  return n;
}

static int mod_connect(media_session_t *session, const media_source_t *src) {
  ghost_ndihx_source_t s;
  memset(&s, 0, sizeof(s));
  snprintf(s.name, sizeof(s.name), "%s", src->name);
  snprintf(s.url, sizeof(s.url), "%s", src->url);
  s.is_hx = (src->tag[0] && !strcasecmp(src->tag, "hx")) || ghost_ndihx_source_looks_hx(src->name);
  return ghost_ndihx_connect((ghost_ndihx_session_t *)session, &s);
}

static int mod_connect_auto(media_session_t *session, volatile const int *cancel) {
  return ghost_ndihx_connect_auto((ghost_ndihx_session_t *)session, cancel);
}

static void mod_disconnect(media_session_t *session) {
  ghost_ndihx_disconnect((ghost_ndihx_session_t *)session);
}

static bool mod_is_connected(const media_session_t *session) {
  return ghost_ndihx_is_connected((const ghost_ndihx_session_t *)session);
}

static void mod_connected_source(const media_session_t *session, media_source_t *out) {
  ghost_ndihx_source_t s;
  ghost_ndihx_connected_source((const ghost_ndihx_session_t *)session, &s);
  source_to_media(&s, out);
}

static bool mod_capture_newest(media_session_t *session, media_frame_t *out) {
  ghost_ndihx_frame_t fr;
  if (!ghost_ndihx_capture_newest((ghost_ndihx_session_t *)session, &fr))
    return false;
  memset(out, 0, sizeof(*out));
  out->data = fr.data;
  out->width = fr.width;
  out->height = fr.height;
  out->stride = fr.stride;
  out->fourcc = fr.fourcc;
  out->frame_rate_n = fr.frame_rate_n;
  out->frame_rate_d = fr.frame_rate_d;
  out->dropped = fr.dropped;
  return true;
}

static media_caps_t mod_capabilities(const media_session_t *session) {
  return (media_caps_t)ghost_ndihx_capabilities((const ghost_ndihx_session_t *)session);
}

static int mod_ptz_get(media_session_t *session, media_ptz_state_t *out) {
  if (!out)
    return -1;
  memset(out, 0, sizeof(*out));
  return ghost_ndihx_ptz_get((ghost_ndihx_session_t *)session, &out->pan, &out->tilt, &out->zoom,
                        &out->valid);
}

static int mod_ptz_move(media_session_t *session, const media_ptz_move_t *move) {
  if (!move)
    return -1;
  return ghost_ndihx_ptz_move((ghost_ndihx_session_t *)session, move->pan, move->tilt, move->zoom,
                         move->continuous);
}

static int mod_ptz_stop(media_session_t *session) {
  return ghost_ndihx_ptz_stop((ghost_ndihx_session_t *)session);
}

static int mod_ptz_home(media_session_t *session) {
  return ghost_ndihx_ptz_home((ghost_ndihx_session_t *)session);
}

static int mod_ptz_preset(media_session_t *session, int index, bool store) {
  return ghost_ndihx_ptz_preset((ghost_ndihx_session_t *)session, index, store);
}

static const media_module_t g_ghost_ndihx_module = {
    .id = "ghost_ndihx",
    .protocol = MEDIA_PROTO_NDI_HX,
    .description = "NDI|HX low-latency receiver (libndi + FFmpeg >= 7)",
    .init = ghost_ndihx_init,
    .shutdown = ghost_ndihx_shutdown,
    .open = mod_open,
    .close = mod_close,
    .discover = mod_discover,
    .connect = mod_connect,
    .connect_auto = mod_connect_auto,
    .disconnect = mod_disconnect,
    .is_connected = mod_is_connected,
    .connected_source = mod_connected_source,
    .capture_newest = mod_capture_newest,
    .capabilities = mod_capabilities,
    .ptz_get = mod_ptz_get,
    .ptz_move = mod_ptz_move,
    .ptz_stop = mod_ptz_stop,
    .ptz_home = mod_ptz_home,
    .ptz_preset = mod_ptz_preset,
};

const media_module_t *ghost_ndihx_media_module(void) { return &g_ghost_ndihx_module; }

int ghost_ndihx_register_media_module(void) {
  return media_register_module(&g_ghost_ndihx_module);
}
