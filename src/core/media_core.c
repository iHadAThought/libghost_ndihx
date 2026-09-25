/**
 * @file media_core.c
 * @brief Protocol-agnostic module registry and shared helpers.
 *
 * Intentionally has zero dependency on NDI, FFmpeg, GStreamer, or Live555 so
 * FULL NDI / 2110 / RTSP modules can link the same core.
 */
#include "media_core.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static const media_module_t *g_modules[MEDIA_MAX_MODULES];
static int g_module_count = 0;

const char *media_core_version(void) { return MEDIA_CORE_VERSION; }

media_caps_t media_session_caps(const media_module_t *module,
                                const media_session_t *session) {
  if (!module || !session || !module->capabilities)
    return MEDIA_CAP_NONE;
  return module->capabilities(session);
}

bool media_caps_has_ptz(media_caps_t caps) { return (caps & MEDIA_CAP_PTZ) != 0; }

const char *media_protocol_name(media_protocol_id_t id) {
  switch (id) {
  case MEDIA_PROTO_NDI_HX:
    return "ghost_ndihx";
  case MEDIA_PROTO_NDI_FULL:
    return "ndi_full";
  case MEDIA_PROTO_ST2110:
    return "st2110";
  case MEDIA_PROTO_RTSP:
    return "rtsp";
  case MEDIA_PROTO_SRT:
    return "srt";
  case MEDIA_PROTO_RTMP:
    return "rtmp";
  default:
    return "unknown";
  }
}

void media_open_params_defaults(media_open_params_t *params) {
  if (!params)
    return;
  memset(params, 0, sizeof(*params));
  params->auto_search = true;
  params->find_ms = 4000;
  params->rescan_ms = 3000;
  snprintf(params->label, sizeof(params->label), "%s", "media-core");
}

int media_register_module(const media_module_t *module) {
  if (!module || !module->id || !*module->id)
    return -1;
  if (!module->open || !module->close || !module->discover || !module->connect ||
      !module->capture_newest)
    return -1;

  for (int i = 0; i < g_module_count; i++) {
    if (g_modules[i] == module)
      return 0;
    if (g_modules[i] && g_modules[i]->id && !strcmp(g_modules[i]->id, module->id)) {
      g_modules[i] = module; /* replace same id */
      return 0;
    }
  }
  if (g_module_count >= MEDIA_MAX_MODULES)
    return -1;
  g_modules[g_module_count++] = module;
  return 0;
}

const media_module_t *media_find_module(const char *id) {
  if (!id || !*id)
    return NULL;
  for (int i = 0; i < g_module_count; i++) {
    if (g_modules[i] && g_modules[i]->id && !strcmp(g_modules[i]->id, id))
      return g_modules[i];
  }
  return NULL;
}

int media_list_modules(const media_module_t **out, int cap) {
  if (!out || cap <= 0)
    return 0;
  int n = g_module_count < cap ? g_module_count : cap;
  for (int i = 0; i < n; i++)
    out[i] = g_modules[i];
  return n;
}

static bool substr_match(const char *hay, const char *needle) {
  if (!needle || !*needle)
    return true;
  if (!hay)
    return false;
  return strcasestr(hay, needle) != NULL;
}

int media_pick_source(const media_source_t *sources, int count,
                      const media_open_params_t *params, const char *tag_prefer) {
  if (!sources || count <= 0 || !params)
    return -1;

  if (params->source_substr[0]) {
    for (int i = 0; i < count; i++) {
      if (substr_match(sources[i].name, params->source_substr) ||
          substr_match(sources[i].url, params->source_substr))
        return i;
    }
    return -1;
  }

  if (params->ip_substr[0]) {
    int tagged = -1, any = -1;
    for (int i = 0; i < count; i++) {
      if (!substr_match(sources[i].name, params->ip_substr) &&
          !substr_match(sources[i].url, params->ip_substr))
        continue;
      if (any < 0)
        any = i;
      if (tag_prefer && tag_prefer[0] && sources[i].tag[0] &&
          !strcasecmp(sources[i].tag, tag_prefer)) {
        tagged = i;
        break;
      }
    }
    if (tagged >= 0)
      return tagged;
    return any;
  }

  if (tag_prefer && tag_prefer[0]) {
    for (int i = 0; i < count; i++) {
      if (sources[i].tag[0] && !strcasecmp(sources[i].tag, tag_prefer))
        return i;
    }
  }
  return 0;
}
