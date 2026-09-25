/**
 * @file ghost_discover_ndi_sdk.c
 * @brief NDI SDK finder backend (NDIlib_find_*).
 *
 * Optional `opt->ndi_find` reuses a live finder from a ghost_ndihx session.
 * Otherwise a short-lived finder is created (requires libndi + ghost_ndihx_init
 * / NDIlib_initialize already done by the host, or we initialize locally).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ghost_discover.h"

#include <Processing.NDI.Lib.h>

#include <stdio.h>
#include <string.h>

static int g_local_init = 0;

static int ensure_ndi(void) {
  if (NDIlib_is_supported_CPU() && g_local_init)
    return 0;
  if (!NDIlib_initialize())
    return -1;
  g_local_init = 1;
  return 0;
}

static void fill_from_ndi(ghost_discover_source_t *s, const NDIlib_source_t *src) {
  memset(s, 0, sizeof(*s));
  snprintf(s->name, sizeof(s->name), "%s", src->p_ndi_name ? src->p_ndi_name : "");
  snprintf(s->url, sizeof(s->url), "%s", src->p_url_address ? src->p_url_address : "");
  s->is_hx = ghost_discover_source_looks_hx(s->name);
  s->via_mdns = false;
  snprintf(s->backend, sizeof(s->backend), "%s", "ndi_sdk");
}

static int sdk_browse(const ghost_discover_options_t *opt, ghost_discover_source_t *out, int cap) {
  if (!out || cap <= 0)
    return -1;

  int find_ms = opt && opt->find_ms > 0 ? opt->find_ms : 4000;
  bool show_local = !opt || opt->show_local;
  NDIlib_find_instance_t find = opt ? (NDIlib_find_instance_t)opt->ndi_find : NULL;
  int owned = 0;

  if (!find) {
    if (ensure_ndi() != 0)
      return -1;
    NDIlib_find_create_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.show_local_sources = show_local;
    find = NDIlib_find_create_v2(&desc);
    if (!find)
      return -1;
    owned = 1;
  }

  NDIlib_find_wait_for_sources(find, (uint32_t)find_ms);
  uint32_t count = 0;
  const NDIlib_source_t *sources = NDIlib_find_get_current_sources(find, &count);
  int n = 0;
  if (sources && count > 0) {
    n = (int)count < cap ? (int)count : cap;
    for (int i = 0; i < n; i++)
      fill_from_ndi(&out[i], &sources[i]);
  }

  if (owned)
    NDIlib_find_destroy(find);
  return n;
}

static bool sdk_available(void) {
  /* Soft check: CPU + initialize once. */
  if (!NDIlib_is_supported_CPU())
    return false;
  return ensure_ndi() == 0;
}

static const ghost_discover_backend_t g_ndi_sdk_backend = {
    .id = "ndi_sdk",
    .label = "NDI SDK finder",
    .available = sdk_available,
    .browse = sdk_browse,
};

const ghost_discover_backend_t *ghost_discover_ndi_sdk_backend(void) {
  return &g_ndi_sdk_backend;
}
