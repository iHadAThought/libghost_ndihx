/**
 * @file ghost_discover_bonjour.c
 * @brief Bonjour / Avahi backend — browse `_ndi._tcp` (NDI source advertisements).
 *
 * Linux: Avahi client (libavahi-client). Darwin: Apple dns_sd.h.
 * No shell-outs to dns-sd/avahi-browse (avoids injection / PATH issues).
 *
 * Service type confirmed by GhostSpot ndi-crosshair and NDI LAN practice:
 *   `_ndi._tcp` on `local.`
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ghost_discover.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__)
#define GHOST_DISCOVER_HAVE_DNSSD 1
#include <arpa/inet.h>
#include <dns_sd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <unistd.h>
#ifndef DNSSD_API
#define DNSSD_API
#endif
#elif defined(__linux__)
#define GHOST_DISCOVER_HAVE_AVAHI 1
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/simple-watch.h>
#include <arpa/inet.h>
#else
/* Stub platform — backend reports unavailable. */
#endif

static void fill_source(ghost_discover_source_t *s, const char *name, const char *url) {
  memset(s, 0, sizeof(*s));
  if (name)
    snprintf(s->name, sizeof(s->name), "%s", name);
  if (url)
    snprintf(s->url, sizeof(s->url), "%s", url);
  s->is_hx = ghost_discover_source_looks_hx(s->name);
  s->via_mdns = true;
  snprintf(s->backend, sizeof(s->backend), "%s", "bonjour");
}

/* -------------------------------------------------------------------------- */
/* Darwin — dns_sd                                                            */
/* -------------------------------------------------------------------------- */
#if defined(GHOST_DISCOVER_HAVE_DNSSD)

typedef struct {
  ghost_discover_source_t *out;
  int cap;
  int count;
  DNSServiceRef browse_ref;
  int pending_resolve;
  int find_ms;
  struct timespec deadline;
} dnssd_ctx_t;

static int past_deadline(const struct timespec *deadline) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (now.tv_sec > deadline->tv_sec)
    return 1;
  if (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)
    return 1;
  return 0;
}

static void resolve_hostname_ipv4(const char *hostname, char *ip, size_t iplen) {
  if (!hostname || !ip || iplen == 0)
    return;
  ip[0] = '\0';
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(hostname, NULL, &hints, &res) != 0 || !res)
    return;
  char buf[INET_ADDRSTRLEN];
  struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
  if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf)))
    snprintf(ip, iplen, "%s", buf);
  freeaddrinfo(res);
}

static void DNSSD_API resolve_cb(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t ifIndex,
                                 DNSServiceErrorType errorCode, const char *fullnameName,
                                 const char *hosttarget, uint16_t port, uint16_t txtLen,
                                 const unsigned char *txtRecord, void *context) {
  (void)sdRef;
  (void)flags;
  (void)ifIndex;
  (void)txtLen;
  (void)txtRecord;
  dnssd_ctx_t *ctx = (dnssd_ctx_t *)context;
  ctx->pending_resolve--;
  if (errorCode != kDNSServiceErr_NoError || !fullnameName || ctx->count >= ctx->cap)
    return;

  uint16_t host_port = ntohs(port);
  char host[256];
  snprintf(host, sizeof(host), "%s", hosttarget ? hosttarget : "");
  /* Strip trailing dot from mDNS hostname. */
  size_t hl = strlen(host);
  while (hl > 0 && host[hl - 1] == '.')
    host[--hl] = '\0';

  char ip[64] = "";
  resolve_hostname_ipv4(host, ip, sizeof(ip));

  char url[256];
  if (ip[0])
    snprintf(url, sizeof(url), "%s:%u", ip, (unsigned)host_port);
  else if (host[0])
    snprintf(url, sizeof(url), "%s:%u", host, (unsigned)host_port);
  else
    url[0] = '\0';

  fill_source(&ctx->out[ctx->count], fullname, url);
  /* dns_sd fullname is "Instance._ndi._tcp.local." — strip type suffix. */
  char *dot = strstr(ctx->out[ctx->count].name, "._ndi._tcp");
  if (dot)
    *dot = '\0';
  ctx->out[ctx->count].is_hx = ghost_discover_source_looks_hx(ctx->out[ctx->count].name);
  ctx->count++;
}

static void DNSSD_API browse_cb(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t ifIndex,
                                DNSServiceErrorType errorCode, const char *serviceName,
                                const char *regtype, const char *replyDomain, void *context) {
  (void)sdRef;
  dnssd_ctx_t *ctx = (dnssd_ctx_t *)context;
  if (errorCode != kDNSServiceErr_NoError || !serviceName)
    return;
  if (!(flags & kDNSServiceFlagsAdd))
    return;
  if (ctx->count + ctx->pending_resolve >= ctx->cap)
    return;

  DNSServiceRef resolve_ref = NULL;
  ctx->pending_resolve++;
  DNSServiceErrorType err =
      DNSServiceResolve(&resolve_ref, 0, ifIndex, serviceName, regtype, replyDomain, resolve_cb,
                        ctx);
  if (err != kDNSServiceErr_NoError || !resolve_ref) {
    ctx->pending_resolve--;
    return;
  }

  /* Process resolve inline with remaining budget. */
  int fd = DNSServiceRefSockFD(resolve_ref);
  while (ctx->pending_resolve > 0 && !past_deadline(&ctx->deadline)) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    int r = select(fd + 1, &fds, NULL, NULL, &tv);
    if (r > 0 && FD_ISSET(fd, &fds)) {
      if (DNSServiceProcessResult(resolve_ref) != kDNSServiceErr_NoError)
        break;
    }
  }
  DNSServiceRefDeallocate(resolve_ref);
}

static int bonjour_browse(const ghost_discover_options_t *opt, ghost_discover_source_t *out,
                          int cap) {
  dnssd_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.out = out;
  ctx.cap = cap;
  ctx.find_ms = opt && opt->find_ms > 0 ? opt->find_ms : 4000;
  clock_gettime(CLOCK_MONOTONIC, &ctx.deadline);
  ctx.deadline.tv_sec += ctx.find_ms / 1000;
  ctx.deadline.tv_nsec += (long)(ctx.find_ms % 1000) * 1000000L;
  if (ctx.deadline.tv_nsec >= 1000000000L) {
    ctx.deadline.tv_sec++;
    ctx.deadline.tv_nsec -= 1000000000L;
  }

  DNSServiceErrorType err =
      DNSServiceBrowse(&ctx.browse_ref, 0, 0, "_ndi._tcp", "local.", browse_cb, &ctx);
  if (err != kDNSServiceErr_NoError || !ctx.browse_ref)
    return -1;

  int fd = DNSServiceRefSockFD(ctx.browse_ref);
  while (!past_deadline(&ctx.deadline)) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    int r = select(fd + 1, &fds, NULL, NULL, &tv);
    if (r > 0 && FD_ISSET(fd, &fds)) {
      if (DNSServiceProcessResult(ctx.browse_ref) != kDNSServiceErr_NoError)
        break;
    }
  }
  DNSServiceRefDeallocate(ctx.browse_ref);
  return ctx.count;
}

static bool bonjour_available(void) { return true; }

/* -------------------------------------------------------------------------- */
/* Linux — Avahi                                                              */
/* -------------------------------------------------------------------------- */
#elif defined(GHOST_DISCOVER_HAVE_AVAHI)

typedef struct {
  AvahiSimplePoll *poll;
  ghost_discover_source_t *out;
  int cap;
  int count;
  int resolving;
  int find_ms;
  int timed_out;
} avahi_ctx_t;

static void avahi_fill_from_resolve(avahi_ctx_t *ctx, const char *name, const char *host,
                                    const AvahiAddress *address, uint16_t port) {
  if (ctx->count >= ctx->cap || !name)
    return;
  char url[256] = "";
  char ip[64] = "";
  if (address) {
    if (address->proto == AVAHI_PROTO_INET) {
      avahi_address_snprint(ip, sizeof(ip), address);
    }
  }
  if (ip[0])
    snprintf(url, sizeof(url), "%s:%u", ip, (unsigned)port);
  else if (host && host[0])
    snprintf(url, sizeof(url), "%s:%u", host, (unsigned)port);

  fill_source(&ctx->out[ctx->count], name, url);
  ctx->count++;
}

static void resolve_cb(AvahiServiceResolver *r, AvahiIfIndex interface, AvahiProtocol protocol,
                       AvahiResolverEvent event, const char *name, const char *type,
                       const char *domain, const char *host_name, const AvahiAddress *address,
                       uint16_t port, AvahiStringList *txt, AvahiLookupResultFlags flags,
                       void *userdata) {
  (void)interface;
  (void)protocol;
  (void)type;
  (void)domain;
  (void)txt;
  (void)flags;
  avahi_ctx_t *ctx = (avahi_ctx_t *)userdata;
  ctx->resolving--;
  if (event == AVAHI_RESOLVER_FOUND)
    avahi_fill_from_resolve(ctx, name, host_name, address, port);
  avahi_service_resolver_free(r);
}

static void browse_cb(AvahiServiceBrowser *b, AvahiIfIndex interface, AvahiProtocol protocol,
                      AvahiBrowserEvent event, const char *name, const char *type,
                      const char *domain, AvahiLookupResultFlags flags, void *userdata) {
  (void)b;
  (void)flags;
  avahi_ctx_t *ctx = (avahi_ctx_t *)userdata;
  if (event != AVAHI_BROWSER_NEW || !name)
    return;
  if (ctx->count + ctx->resolving >= ctx->cap)
    return;
  AvahiClient *client = avahi_service_browser_get_client(b);
  if (!client)
    return;
  ctx->resolving++;
  if (!avahi_service_resolver_new(client, interface, protocol, name, type, domain,
                                  AVAHI_PROTO_UNSPEC, 0, resolve_cb, ctx)) {
    ctx->resolving--;
  }
}

static void client_cb(AvahiClient *c, AvahiClientState state, void *userdata) {
  (void)c;
  avahi_ctx_t *ctx = (avahi_ctx_t *)userdata;
  if (state == AVAHI_CLIENT_FAILURE)
    ctx->timed_out = 1;
}

static int bonjour_browse(const ghost_discover_options_t *opt, ghost_discover_source_t *out,
                          int cap) {
  avahi_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.out = out;
  ctx.cap = cap;
  ctx.find_ms = opt && opt->find_ms > 0 ? opt->find_ms : 4000;

  ctx.poll = avahi_simple_poll_new();
  if (!ctx.poll)
    return -1;

  int error = 0;
  AvahiClient *client =
      avahi_client_new(avahi_simple_poll_get(ctx.poll), 0, client_cb, &ctx, &error);
  if (!client) {
    avahi_simple_poll_free(ctx.poll);
    return -1;
  }

  AvahiServiceBrowser *browser =
      avahi_service_browser_new(client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, "_ndi._tcp", "local", 0,
                                browse_cb, &ctx);
  if (!browser) {
    avahi_client_free(client);
    avahi_simple_poll_free(ctx.poll);
    return -1;
  }

  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_sec += ctx.find_ms / 1000;
  deadline.tv_nsec += (long)(ctx.find_ms % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }

  while (!ctx.timed_out) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > deadline.tv_sec ||
        (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
      break;
    if (avahi_simple_poll_iterate(ctx.poll, 100) != 0)
      break;
  }

  /* Drain in-flight resolvers briefly after browse window. */
  int guard = 0;
  while (ctx.resolving > 0 && guard++ < 50) {
    if (avahi_simple_poll_iterate(ctx.poll, 50) != 0)
      break;
  }

  avahi_service_browser_free(browser);
  avahi_client_free(client);
  avahi_simple_poll_free(ctx.poll);
  return ctx.count;
}

static bool bonjour_available(void) {
  AvahiSimplePoll *poll = avahi_simple_poll_new();
  if (!poll)
    return false;
  int error = 0;
  AvahiClient *client = avahi_client_new(avahi_simple_poll_get(poll), 0, NULL, NULL, &error);
  bool ok = client != NULL;
  if (client)
    avahi_client_free(client);
  avahi_simple_poll_free(poll);
  return ok;
}

#else /* stub */

static int bonjour_browse(const ghost_discover_options_t *opt, ghost_discover_source_t *out,
                          int cap) {
  (void)opt;
  (void)out;
  (void)cap;
  return 0;
}

static bool bonjour_available(void) { return false; }

#endif

static const ghost_discover_backend_t g_bonjour_backend = {
    .id = "bonjour",
    .label = "Bonjour / Avahi (_ndi._tcp)",
    .available = bonjour_available,
    .browse = bonjour_browse,
};

const ghost_discover_backend_t *ghost_discover_bonjour_backend(void) {
  return &g_bonjour_backend;
}
