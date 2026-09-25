/**
 * @file ghost_discover.c
 * @brief Discovery registry, AUTO policy, and shared pick helpers.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ghost_discover.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const ghost_discover_backend_t *g_backends[GHOST_DISCOVER_MAX_BACKENDS];
static int g_backend_count = 0;
static int g_builtins_registered = 0;

const char *ghost_discover_version(void) { return GHOST_DISCOVER_VERSION; }

void ghost_discover_options_defaults(ghost_discover_options_t *opt) {
  if (!opt)
    return;
  memset(opt, 0, sizeof(*opt));
  opt->backend = GHOST_DISCOVER_AUTO;
  opt->show_local = true;
  opt->find_ms = 4000;
}

bool ghost_discover_backend_parse(const char *s, ghost_discover_backend_id_t *out) {
  if (!s || !out)
    return false;
  if (!strcasecmp(s, "auto") || !strcasecmp(s, "both") || !strcasecmp(s, "default")) {
    *out = GHOST_DISCOVER_AUTO;
    return true;
  }
  if (!strcasecmp(s, "bonjour") || !strcasecmp(s, "mdns") || !strcasecmp(s, "avahi") ||
      !strcasecmp(s, "dns-sd") || !strcasecmp(s, "dns_sd")) {
    *out = GHOST_DISCOVER_BONJOUR;
    return true;
  }
  if (!strcasecmp(s, "ndi_sdk") || !strcasecmp(s, "ndi-sdk") || !strcasecmp(s, "sdk") ||
      !strcasecmp(s, "ndi") || !strcasecmp(s, "finder")) {
    *out = GHOST_DISCOVER_NDI_SDK;
    return true;
  }
  return false;
}

const char *ghost_discover_backend_name(ghost_discover_backend_id_t id) {
  switch (id) {
  case GHOST_DISCOVER_BONJOUR:
    return "bonjour";
  case GHOST_DISCOVER_NDI_SDK:
    return "ndi_sdk";
  case GHOST_DISCOVER_AUTO:
  default:
    return "auto";
  }
}

bool ghost_discover_source_looks_hx(const char *name) {
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

int ghost_discover_register_backend(const ghost_discover_backend_t *backend) {
  if (!backend || !backend->id || !*backend->id || !backend->browse)
    return -1;
  for (int i = 0; i < g_backend_count; i++) {
    if (g_backends[i] == backend)
      return 0;
    if (g_backends[i] && g_backends[i]->id && !strcmp(g_backends[i]->id, backend->id)) {
      g_backends[i] = backend;
      return 0;
    }
  }
  if (g_backend_count >= GHOST_DISCOVER_MAX_BACKENDS)
    return -1;
  g_backends[g_backend_count++] = backend;
  return 0;
}

int ghost_discover_register_builtins(void) {
  if (g_builtins_registered)
    return 0;
  const ghost_discover_backend_t *b = ghost_discover_bonjour_backend();
  const ghost_discover_backend_t *s = ghost_discover_ndi_sdk_backend();
  if (b && ghost_discover_register_backend(b) != 0)
    return -1;
  if (s && ghost_discover_register_backend(s) != 0)
    return -1;
  g_builtins_registered = 1;
  return 0;
}

const ghost_discover_backend_t *ghost_discover_find_backend(const char *id) {
  if (!id || !*id)
    return NULL;
  ghost_discover_register_builtins();
  for (int i = 0; i < g_backend_count; i++) {
    if (g_backends[i] && g_backends[i]->id && !strcmp(g_backends[i]->id, id))
      return g_backends[i];
  }
  return NULL;
}

int ghost_discover_list_backends(const ghost_discover_backend_t **out, int cap) {
  ghost_discover_register_builtins();
  if (!out || cap <= 0)
    return g_backend_count;
  int n = g_backend_count < cap ? g_backend_count : cap;
  for (int i = 0; i < n; i++)
    out[i] = g_backends[i];
  return n;
}

bool ghost_discover_backend_available(ghost_discover_backend_id_t id) {
  ghost_discover_register_builtins();
  if (id == GHOST_DISCOVER_AUTO) {
    return ghost_discover_backend_available(GHOST_DISCOVER_BONJOUR) ||
           ghost_discover_backend_available(GHOST_DISCOVER_NDI_SDK);
  }
  const char *name = ghost_discover_backend_name(id);
  const ghost_discover_backend_t *b = ghost_discover_find_backend(name);
  if (!b)
    return false;
  if (b->available)
    return b->available();
  return true;
}

static int clamp_find_ms(int ms) {
  if (ms < 500)
    return 500;
  return ms;
}

static bool source_dup(const ghost_discover_source_t *a, const ghost_discover_source_t *b) {
  if (a->name[0] && b->name[0] && !strcasecmp(a->name, b->name))
    return true;
  if (a->url[0] && b->url[0] && !strcasecmp(a->url, b->url))
    return true;
  return false;
}

static int append_unique(ghost_discover_source_t *dst, int n, int cap,
                         const ghost_discover_source_t *src, int src_n) {
  for (int i = 0; i < src_n && n < cap; i++) {
    int dup = 0;
    for (int j = 0; j < n; j++) {
      if (source_dup(&dst[j], &src[i])) {
        dup = 1;
        break;
      }
    }
    if (dup)
      continue;
    dst[n++] = src[i];
  }
  return n;
}

static int browse_one(const char *id, const ghost_discover_options_t *opt,
                      ghost_discover_source_t *out, int cap) {
  const ghost_discover_backend_t *b = ghost_discover_find_backend(id);
  if (!b)
    return -1;
  if (b->available && !b->available())
    return 0;
  return b->browse(opt, out, cap);
}

int ghost_discover_browse(const ghost_discover_options_t *opt, ghost_discover_source_t *out,
                          int cap) {
  if (!out || cap <= 0)
    return -1;
  ghost_discover_options_t local;
  if (!opt) {
    ghost_discover_options_defaults(&local);
    opt = &local;
  } else {
    local = *opt;
    local.find_ms = clamp_find_ms(local.find_ms);
    opt = &local;
  }

  ghost_discover_register_builtins();

  if (opt->backend == GHOST_DISCOVER_BONJOUR)
    return browse_one("bonjour", opt, out, cap);
  if (opt->backend == GHOST_DISCOVER_NDI_SDK)
    return browse_one("ndi_sdk", opt, out, cap);

  /* AUTO: Bonjour first, then SDK fill / fallback. Bonjour entries stay first
   * for pick order. If Bonjour is unavailable / empty, SDK alone is fallback. */
  ghost_discover_source_t tmp[GHOST_DISCOVER_MAX_SOURCES];
  int n = 0;
  int bn = browse_one("bonjour", opt, tmp, GHOST_DISCOVER_MAX_SOURCES);
  if (bn > 0)
    n = append_unique(out, n, cap, tmp, bn);

  ghost_discover_options_t sdk_opt = *opt;
  /* When Bonjour already found sources, keep SDK wait short for merge fill. */
  if (n > 0 && sdk_opt.find_ms > 1500)
    sdk_opt.find_ms = 1500;
  int sn = browse_one("ndi_sdk", &sdk_opt, tmp, GHOST_DISCOVER_MAX_SOURCES);
  if (sn > 0)
    n = append_unique(out, n, cap, tmp, sn);
  else if (sn < 0 && n == 0)
    return -1;

  return n;
}

static bool name_match(const char *hay, const char *needle) {
  if (!needle || !*needle)
    return true;
  if (!hay)
    return false;
  return strcasestr(hay, needle) != NULL;
}

int ghost_discover_pick(const ghost_discover_source_t *sources, int count,
                        const char *source_substr, const char *ip_substr, bool prefer_hx) {
  if (!sources || count <= 0)
    return -1;

  if (source_substr && source_substr[0]) {
    for (int i = 0; i < count; i++) {
      if (name_match(sources[i].name, source_substr) ||
          name_match(sources[i].url, source_substr))
        return i;
    }
    return -1;
  }

  if (ip_substr && ip_substr[0]) {
    int hx = -1, any = -1;
    for (int i = 0; i < count; i++) {
      if (!name_match(sources[i].name, ip_substr) && !name_match(sources[i].url, ip_substr))
        continue;
      if (any < 0)
        any = i;
      if (sources[i].is_hx || ghost_discover_source_looks_hx(sources[i].name)) {
        hx = i;
        break;
      }
    }
    if (prefer_hx && hx >= 0)
      return hx;
    return any;
  }

  if (prefer_hx) {
    for (int i = 0; i < count; i++) {
      if (sources[i].is_hx || ghost_discover_source_looks_hx(sources[i].name))
        return i;
    }
  }
  return 0;
}
