/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "udp_preload.h"

#include <dlfcn.h>

static struct upl_ctx g_upl_ctx;

static inline struct upl_ctx* upl_get_ctx(void) { return &g_upl_ctx; }

static inline bool upl_stopped(struct upl_ctx* ctx) {
  return ctx->upl_entires ? false : true;
}

static inline int upl_set_upl_entry(struct upl_ctx* ctx, int kfd, void* upl) {
  if (upl_stopped(ctx)) {
    err("%s(%d), upl stopped\n", __func__, kfd);
    return -EIO;
  }
  if (ctx->upl_entires[kfd]) {
    warn("%s(%d), already has upl %p\n", __func__, kfd, ctx->upl_entires[kfd]);
  }
  ctx->upl_entires[kfd] = upl;
  dbg("%s(%d), upl entry %p\n", __func__, kfd, upl);
  return 0;
}

static inline void* upl_get_upl_entry(struct upl_ctx* ctx, int kfd) {
  if (upl_stopped(ctx)) return NULL;
  return ctx->upl_entires[kfd];
}

static inline int upl_clear_upl_entry(struct upl_ctx* ctx, int kfd) {
  ctx->upl_entires[kfd] = NULL;
  return 0;
}

static inline struct upl_ufd_entry* upl_get_ufd_entry(struct upl_ctx* ctx, int kfd) {
  struct upl_ufd_entry* entry = upl_get_upl_entry(ctx, kfd);
  if (entry && entry->base.upl_type != UPL_ENTRY_UFD) {
    dbg("%s(%d), entry %p error type %d\n", __func__, kfd, entry, entry->base.upl_type);
    return NULL;
  }
  dbg("%s(%d), ufd entry %p\n", __func__, kfd, entry);
  return entry;
}

static inline struct upl_efd_entry* upl_get_efd_entry(struct upl_ctx* ctx, int kfd) {
  struct upl_efd_entry* entry = upl_get_upl_entry(ctx, kfd);
  if (entry && entry->base.upl_type != UPL_ENTRY_EPOLL) {
    err("%s(%d), entry %p error type %d\n", __func__, kfd, entry, entry->base.upl_type);
    return NULL;
  }
  dbg("%s(%d), efd entry %p\n", __func__, kfd, entry);
  return entry;
}

static inline bool upl_is_ufd_entry(struct upl_ctx* ctx, int kfd) {
  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, kfd);
  dbg("%s(%d), ufd entry %p\n", __func__, kfd, entry);
  if (!entry || entry->bind_kfd)
    return false;
  else
    return true;
}

static const char* upl_type_names[UPL_ENTRY_MAX] = {
    "unknown",
    "ufd",
    "efd",
};

static const char* upl_type_name(enum upl_entry_type type) {
  return upl_type_names[type];
}

static int upl_uinit_ctx(struct upl_ctx* ctx) {
  if (ctx->upl_entires) {
    for (int i = 0; i < ctx->upl_entires_nb; i++) {
      struct upl_base_entry* entry = upl_get_upl_entry(ctx, i);
      if (entry) {
        warn("%s, upl still active on %d, upl type %s\n", __func__, i,
             upl_type_name(entry->upl_type));
      }
    }
    upl_free(ctx->upl_entires);
    ctx->upl_entires = NULL;
  }

  return 0;
}

static int upl_get_libc_fn(struct upl_functions* fns) {
#define UPL_LIBC_FN(__name)                          \
  do {                                               \
    fns->__name = dlsym(RTLD_NEXT, #__name);         \
    if (!fns->__name) {                              \
      err("%s, dlsym %s fail\n", __func__, #__name); \
      UPL_ERR_RET(EIO);                              \
    }                                                \
  } while (0)

  UPL_LIBC_FN(socket);
  UPL_LIBC_FN(close);
  UPL_LIBC_FN(bind);
  UPL_LIBC_FN(sendto);
  UPL_LIBC_FN(send);
  UPL_LIBC_FN(sendmsg);
  UPL_LIBC_FN(poll);
  UPL_LIBC_FN(ppoll);
  UPL_LIBC_FN(select);
  UPL_LIBC_FN(pselect);
  UPL_LIBC_FN(recv);
  UPL_LIBC_FN(recvfrom);
  UPL_LIBC_FN(recvmsg);
  UPL_LIBC_FN(getsockopt);
  UPL_LIBC_FN(setsockopt);
  UPL_LIBC_FN(fcntl);
  UPL_LIBC_FN(fcntl64);
  UPL_LIBC_FN(ioctl);
  UPL_LIBC_FN(epoll_create);
  UPL_LIBC_FN(epoll_create1);
  UPL_LIBC_FN(epoll_ctl);
  UPL_LIBC_FN(epoll_wait);
  UPL_LIBC_FN(epoll_pwait);

  info("%s, succ\n", __func__);
  return 0;
}

static int upl_init_ctx(struct upl_ctx* ctx) {
  int ret;

  ctx->log_level = MTL_LOG_LEVEL_INFO;
  ctx->upl_entires_nb = 1024 * 10; /* max fd we support */
  ctx->upl_entires = upl_zmalloc(sizeof(*ctx->upl_entires) * ctx->upl_entires_nb);
  if (!ctx->upl_entires) {
    err("%s, upl_entires malloc fail, nb %d\n", __func__, ctx->upl_entires_nb);
    upl_uinit_ctx(ctx);
    UPL_ERR_RET(ENOMEM);
  }

  ret = upl_get_libc_fn(&ctx->libc_fn);
  if (ret < 0) {
    upl_uinit_ctx(ctx);
    UPL_ERR_RET(EIO);
  }

  ctx->init_succ = true;
  info("%s, succ ctx %p\n", __func__, ctx);
  return 0;
}

static void __attribute__((constructor)) upl_init() {
  struct upl_ctx* ctx = upl_get_ctx();
  int ret;

  ret = upl_init_ctx(ctx);
  if (ret < 0) {
    err("%s, init ctx fail %d\n", __func__, ret);
    return;
  }

  ret = mufd_init_context();
  if (ret < 0) {
    warn("%s, mufd init fail %d, fallback to posix socket\n", __func__, ret);
  } else {
    ctx->mtl_fd_base = mufd_base_fd();
    ctx->has_mtl_udp = true;
    ctx->log_level = mufd_log_level();
    info("%s, mufd init succ, base fd %d\n", __func__, ctx->mtl_fd_base);
  }
}

static void __attribute__((destructor)) upl_uinit() {
  struct upl_ctx* ctx = upl_get_ctx();
  upl_uinit_ctx(ctx);
  ctx->has_mtl_udp = false;
}

static int upl_stat_dump(void* priv) {
  struct upl_ufd_entry* entry = priv;
  int kfd = entry->kfd;

  if (entry->stat_tx_ufd_cnt || entry->stat_rx_ufd_cnt) {
    notice("%s(%d), ufd pkt tx %d rx %d\n", __func__, kfd, entry->stat_tx_ufd_cnt,
           entry->stat_rx_ufd_cnt);
    entry->stat_tx_ufd_cnt = 0;
    entry->stat_rx_ufd_cnt = 0;
  }
  if (entry->stat_tx_kfd_cnt || entry->stat_rx_kfd_cnt) {
    notice("%s(%d), kfd pkt tx %d rx %d\n", __func__, kfd, entry->stat_tx_kfd_cnt,
           entry->stat_rx_kfd_cnt);
    entry->stat_tx_kfd_cnt = 0;
    entry->stat_rx_kfd_cnt = 0;
  }
  if (entry->stat_epoll_cnt || entry->stat_epoll_revents_cnt) {
    notice("%s(%d), epoll %d revents %d\n", __func__, kfd, entry->stat_epoll_cnt,
           entry->stat_epoll_revents_cnt);
    entry->stat_epoll_cnt = 0;
    entry->stat_epoll_revents_cnt = 0;
  }
  if (entry->stat_select_cnt || entry->stat_select_revents_cnt) {
    notice("%s(%d), select %d revents %d\n", __func__, kfd, entry->stat_select_cnt,
           entry->stat_select_revents_cnt);
    entry->stat_select_cnt = 0;
    entry->stat_select_revents_cnt = 0;
  }
  if (entry->stat_poll_cnt || entry->stat_poll_revents_cnt) {
    notice("%s(%d), poll %d revents %d\n", __func__, kfd, entry->stat_poll_cnt,
           entry->stat_poll_revents_cnt);
    entry->stat_poll_cnt = 0;
    entry->stat_poll_revents_cnt = 0;
  }
  return 0;
}

static int upl_epoll_create(struct upl_ctx* ctx, int efd) {
  struct upl_efd_entry* entry = upl_zmalloc(sizeof(*entry));
  if (!entry) {
    err("%s, entry malloc fail for efd %d\n", __func__, efd);
    UPL_ERR_RET(ENOMEM);
  }
  entry->base.parent = ctx;
  entry->base.upl_type = UPL_ENTRY_EPOLL;
  entry->efd = efd;
  pthread_mutex_init(&entry->mutex, NULL);
  TAILQ_INIT(&entry->fds);

  upl_set_upl_entry(ctx, efd, entry);
  return 0;
}

static int upl_epoll_close(struct upl_efd_entry* entry) {
  struct upl_efd_fd_item* item;

  pthread_mutex_lock(&entry->mutex);
  /* check if any not removed */
  while ((item = TAILQ_FIRST(&entry->fds))) {
    dbg("%s(%d), kfd %d not close\n", __func__, entry->efd, item->ufd->kfd);
    item->ufd->efd = -1;
    TAILQ_REMOVE(&entry->fds, item, next);
    upl_free(item);
  }
  pthread_mutex_unlock(&entry->mutex);

  pthread_mutex_destroy(&entry->mutex);
  dbg("%s(%d), close epoll efd\n", __func__, efd_entry->efd);
  return 0;
}

static inline bool upl_epoll_has_ufd(struct upl_efd_entry* efd_entry) {
  return TAILQ_EMPTY(&efd_entry->fds) ? false : true;
}

static int upl_efd_ctl_add(struct upl_efd_entry* efd, struct upl_ufd_entry* ufd,
                           struct epoll_event* event) {
  struct upl_efd_fd_item* item = upl_zmalloc(sizeof(*item));
  if (!item) {
    err("%s, malloc fail\n", __func__);
    UPL_ERR_RET(ENOMEM);
  }
  if (event) item->event = *event;
  item->ufd = ufd;

  dbg("%s, efd %p ufd %p\n", __func__, efd, ufd);
  pthread_mutex_lock(&efd->mutex);
  ufd->efd = efd->efd;
  TAILQ_INSERT_TAIL(&efd->fds, item, next);
  efd->fds_cnt++;
  pthread_mutex_unlock(&efd->mutex);

  dbg("%s(%d), add ufd %d succ\n", __func__, efd->efd, ufd->kfd);
  return 0;
}

static int upl_efd_ctl_del(struct upl_efd_entry* efd, struct upl_ufd_entry* ufd) {
  struct upl_efd_fd_item *item, *tmp_item;

  pthread_mutex_lock(&efd->mutex);
  for (item = TAILQ_FIRST(&efd->fds); item != NULL; item = tmp_item) {
    tmp_item = TAILQ_NEXT(item, next);
    if (item->ufd == ufd) {
      /* found the matched item, remove it */
      TAILQ_REMOVE(&efd->fds, item, next);
      ufd->efd = -1;
      efd->fds_cnt--;
      pthread_mutex_unlock(&efd->mutex);
      upl_free(item);
      dbg("%s(%d), del ufd %d succ\n", __func__, efd->efd, ufd->kfd);
      return 0;
    }
  }
  pthread_mutex_unlock(&efd->mutex);

  err("%s(%d), del ufd %d fail\n", __func__, efd->efd, ufd->kfd);
  UPL_ERR_RET(EINVAL);
}

static int upl_efd_ctl_mod(struct upl_efd_entry* efd, struct upl_ufd_entry* ufd,
                           struct epoll_event* event) {
  struct upl_efd_fd_item *item, *tmp_item;

  pthread_mutex_lock(&efd->mutex);
  for (item = TAILQ_FIRST(&efd->fds); item != NULL; item = tmp_item) {
    tmp_item = TAILQ_NEXT(item, next);
    if (item->ufd == ufd) {
      /* found the matched item, remove it */
      item->event = *event;
      pthread_mutex_unlock(&efd->mutex);
      info("%s(%d), mod ufd %d succ\n", __func__, efd->efd, ufd->kfd);
      return 0;
    }
  }
  pthread_mutex_unlock(&efd->mutex);

  err("%s(%d), del ufd %d fail\n", __func__, efd->efd, ufd->kfd);
  UPL_ERR_RET(EINVAL);
}

static int upl_efd_epoll_query(void* priv) {
  struct upl_efd_entry* entry = priv;
  struct upl_ctx* ctx = entry->base.parent;
  int efd = entry->efd;
  int ret;
  dbg("%s(%d), start\n", __func__, efd);

  /* timeout to zero for query */
  if (entry->sigmask)
    ret =
        ctx->libc_fn.epoll_pwait(efd, entry->events, entry->maxevents, 0, entry->sigmask);
  else
    ret = ctx->libc_fn.epoll_wait(efd, entry->events, entry->maxevents, 0);
  if (ret != 0) { /* event on kfd */
    entry->kfd_ret = ret;
    info("%s(%d), ret %d\n", __func__, efd, ret);
  }

  return ret;
}

static int upl_select_query(void* priv) {
  struct upl_select_ctx* select_ctx = priv;
  struct upl_ctx* ctx = select_ctx->parent;
  int ret;

  struct timeval zero;
  zero.tv_sec = 0;
  zero.tv_usec = 0;
  struct timespec zero_spec;
  zero_spec.tv_sec = 0;
  zero_spec.tv_nsec = 0;

  /* timeout to zero for query */
  if (select_ctx->sigmask)
    ret =
        ctx->libc_fn.pselect(select_ctx->nfds, select_ctx->readfds, select_ctx->writefds,
                             select_ctx->exceptfds, &zero_spec, select_ctx->sigmask);
  else
    ret = ctx->libc_fn.select(select_ctx->nfds, select_ctx->readfds, select_ctx->writefds,
                              select_ctx->exceptfds, &zero);
  dbg("%s, ret %d\n", __func__, ret);
  return ret;
}

static int upl_poll_query(void* priv) {
  struct upl_poll_ctx* poll_ctx = priv;
  struct upl_ctx* ctx = poll_ctx->parent;
  int ret;

  /* use zero timeout as query */
  if (poll_ctx->tmo_p || poll_ctx->sigmask) {
    struct timespec zero;
    zero.tv_sec = 0;
    zero.tv_nsec = 0;
    ret = ctx->libc_fn.ppoll(poll_ctx->fds, poll_ctx->nfds, &zero, poll_ctx->sigmask);
  } else {
    ret = ctx->libc_fn.poll(poll_ctx->fds, poll_ctx->nfds, 0);
  }
  dbg("%s, ret %d\n", __func__, ret);
  return ret;
}

/* reuse mufd_poll now */
static int upl_efd_epoll_pwait(struct upl_efd_entry* entry, struct epoll_event* events,
                               int maxevents, int timeout_ms, const sigset_t* sigmask) {
  int efd = entry->efd;
  const int fds_cnt = entry->fds_cnt;
  struct upl_efd_fd_item* item;
  struct pollfd p_fds[fds_cnt];
  struct upl_efd_fd_item* efd_items[fds_cnt];
  int p_fds_cnt = 0;
  int kfd_cnt = atomic_load(&entry->kfd_cnt);
  int ret;

  dbg("%s(%d), timeout_ms %d maxevents %d kfd_cnt %d\n", __func__, efd, timeout_ms,
      maxevents, kfd_cnt);
  pthread_mutex_lock(&entry->mutex);
  TAILQ_FOREACH(item, &entry->fds, next) {
    if (p_fds_cnt >= fds_cnt) {
      err("%s(%d), wrong p_fds_cnt %d fds_cnt %d\n", __func__, efd, p_fds_cnt, fds_cnt);
      pthread_mutex_unlock(&entry->mutex);
      UPL_ERR_RET(EIO);
    }
    item->ufd->stat_epoll_cnt++;
    p_fds[p_fds_cnt].fd = item->ufd->ufd;
    p_fds[p_fds_cnt].events = POLLIN;
    efd_items[p_fds_cnt] = item;
    p_fds_cnt++;
  }
  pthread_mutex_unlock(&entry->mutex);

  entry->kfd_ret = 0;
  if (kfd_cnt > 0) {
    entry->events = events;
    entry->maxevents = maxevents;
    entry->sigmask = sigmask;
    ret = mufd_poll_query(p_fds, p_fds_cnt, timeout_ms, upl_efd_epoll_query, entry);
  } else {
    ret = mufd_poll(p_fds, p_fds_cnt, timeout_ms);
  }
  if (ret <= 0) return ret;

  /* event on the kfd */
  if (entry->kfd_ret > 0) return entry->kfd_ret;

  int ready = 0;
  for (int i = 0; i < p_fds_cnt; i++) {
    if (!p_fds[i].revents) continue;
    item = efd_items[i];
    dbg("%s, revents on ufd %d kfd %d\n", __func__, p_fds[i].fd, item->ufd->kfd);
    events[ready] = efd_items[i]->event;
    ready++;
    item->ufd->stat_epoll_revents_cnt++;
  }

  return ready;
}

static int upl_pselect(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
                       struct timeval* timeout, const struct timespec* timeout_spec,
                       const sigset_t* sigmask) {
  dbg("%s, nfds %d\n", __func__, nfds);
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    UPL_ERR_RET(EIO);
  }

  if (nfds <= 0 || nfds > FD_SETSIZE) {
    err("%s, invalid nfds %d\n", __func__, nfds);
    UPL_ERR_RET(EIO);
  }

  struct pollfd poll_ufds[nfds];
  int poll_ufds_kfd[nfds];
  int poll_ufds_cnt = 0;
  /* split fd with kernel and imtl */
  for (int i = 0; i < nfds; i++) {
    if (!upl_is_ufd_entry(ctx, i)) continue;
    if (readfds && FD_ISSET(i, readfds)) {
      FD_CLR(i, readfds); /* clear the readfds to kernel since it's a ufd */
      struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, i);
      poll_ufds[poll_ufds_cnt].fd = entry->ufd;
      poll_ufds[poll_ufds_cnt].events = POLLIN;
      entry->stat_select_cnt++;
      poll_ufds_kfd[poll_ufds_cnt] = i; /* save kfd */
      dbg("%s(%d), ufd %d add on %d\n", __func__, i, entry->ufd, poll_ufds_cnt);
      poll_ufds_cnt++;
    }
    if (writefds && FD_ISSET(i, writefds)) {
      warn("%s(%d), not support write select for ufd\n", __func__, i);
      FD_CLR(i, writefds); /* clear since it's ufd */
    }
    if (exceptfds && FD_ISSET(i, exceptfds)) {
      warn("%s(%d), not support except select for ufd\n", __func__, i);
      FD_CLR(i, exceptfds); /* clear since it's ufd */
    }
  }

  if (!poll_ufds_cnt) {
    if (sigmask)
      return ctx->libc_fn.pselect(nfds, readfds, writefds, exceptfds, timeout_spec,
                                  sigmask);
    else
      return ctx->libc_fn.select(nfds, readfds, writefds, exceptfds, timeout);
  }

  struct upl_select_ctx priv;
  priv.parent = ctx;
  priv.readfds = readfds;
  priv.writefds = writefds;
  priv.exceptfds = exceptfds;
  priv.timeout = timeout;
  priv.timeout_spec = timeout_spec;
  priv.sigmask = sigmask;
  int timeout_ms = 0;
  if (timeout) timeout_ms = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
  /* wa to fix end loop in userspace issue */
  if (timeout_ms <= 0) timeout_ms = 1000 * 2;
  int ret =
      mufd_poll_query(poll_ufds, poll_ufds_cnt, timeout_ms, upl_select_query, &priv);
  if (ret < 0) return ret;

  FD_ZERO(readfds);
  for (nfds_t i = 0; i < poll_ufds_cnt; i++) {
    if (!poll_ufds[i].revents) continue;
    int kfd = poll_ufds_kfd[i];
    struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, kfd);
    dbg("%s(%d), revents on ufd %d kfd %d\n", __func__, kfd, entry->ufd);
    entry->stat_select_revents_cnt++;
    FD_SET(kfd, readfds);
  }
  return ret;
}

static int upl_ppoll(struct pollfd* fds, nfds_t nfds, int timeout,
                     const struct timespec* tmo_p, const sigset_t* sigmask) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    UPL_ERR_RET(EIO);
  }

  if (nfds <= 0) {
    err("%s, invalid nfds %" PRIu64 "\n", __func__, nfds);
    UPL_ERR_RET(EIO);
  }

  struct pollfd ufds[nfds];
  nfds_t ufds_pos[nfds];
  nfds_t ufds_cnt = 0;
  struct pollfd kfds[nfds];
  nfds_t kfds_pos[nfds];
  nfds_t kfds_cnt = 0;

  /* loop tp check if ufd */
  for (nfds_t i = 0; i < nfds; i++) {
    int kfd = fds[i].fd;
    fds[i].revents = 0; /* clear all revents */
    if (upl_is_ufd_entry(ctx, kfd)) {
      struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, kfd);
      entry->stat_poll_cnt++;
      ufds[ufds_cnt].fd = entry->ufd;
      ufds[ufds_cnt].events = fds[i].events;
      ufds[ufds_cnt].revents = 0;
      ufds_pos[ufds_cnt] = i;
      ufds_cnt++;
    } else {
      kfds[kfds_cnt].fd = kfd;
      kfds[kfds_cnt].events = fds[i].events;
      kfds[kfds_cnt].revents = 0;
      kfds_pos[kfds_cnt] = i;
      kfds_cnt++;
    }
  }

  if (!ufds_cnt) {
    if (tmo_p || sigmask)
      return ctx->libc_fn.ppoll(fds, nfds, tmo_p, sigmask);
    else
      return ctx->libc_fn.poll(fds, nfds, timeout);
  }

  struct upl_poll_ctx priv;
  priv.parent = ctx;
  priv.fds = kfds;
  priv.nfds = kfds_cnt;
  priv.timeout = timeout;
  priv.tmo_p = tmo_p;
  priv.sigmask = sigmask;

  int ret;
  if (kfds_cnt)
    ret = mufd_poll_query(ufds, ufds_cnt, timeout, upl_poll_query, &priv);
  else
    ret = mufd_poll(ufds, ufds_cnt, timeout);
  dbg("%s, mufd_poll ret %d timeout %d\n", __func__, ret, timeout);
  if (ret <= 0) return ret;

  /* check if any ufd ready */
  for (nfds_t i = 0; i < ufds_cnt; i++) {
    if (ufds[i].revents) { /* set revents on fds */
      nfds_t pos = ufds_pos[i];
      int kfd = fds[pos].fd;
      struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, kfd);
      entry->stat_poll_revents_cnt++;
      fds[pos].revents = ufds[i].revents;
      dbg("%s(%d), revents %d on ufd %d\n", __func__, kfd, fds[pos].revents, entry->ufd);
      if (entry->kfd != fds[pos].fd) {
        err("%s(%d), not match with entry ufd %d kfd %d\n", __func__, kfd, entry->ufd,
            entry->kfd);
      }
    }
  }
  if (!kfds_cnt) return ret;

  /* check if any kfd ready */
  for (nfds_t i = 0; i < kfds_cnt; i++) {
    if (kfds[i].revents) { /* set revents on fds */
      nfds_t pos = kfds_pos[i];
      int kfd = fds[pos].fd;
      fds[pos].revents = kfds[i].revents;
      dbg("%s(%d), revents %d on kfd\n", __func__, kfd, fds[pos].revents);
      if (kfds[i].fd != kfd) {
        err("%s(%d), not match with kfd %d\n", __func__, kfd, kfds[i].fd);
      }
    }
  }

  return ret;
}

static int upl_ufd_close(struct upl_ufd_entry* ufd_entry) {
  int ufd = ufd_entry->ufd;
  int kfd = ufd_entry->kfd;
  int efd = ufd_entry->efd;

  if (efd > 0) {
    struct upl_ctx* ctx = ufd_entry->base.parent;
    struct upl_efd_entry* efd_entry = upl_get_efd_entry(ctx, efd);
    info("%s(%d), remove epoll ctl on efd %d\n", __func__, kfd, efd);
    upl_efd_ctl_del(efd_entry, ufd_entry);
  }

  mufd_close(ufd);
  info("%s(%d), close ufd %d\n", __func__, kfd, ufd);
  return 0;
}

int socket(int domain, int type, int protocol) {
  struct upl_ctx* ctx = upl_get_ctx();
  int kfd;
  int ret;

  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    UPL_ERR_RET(EIO);
  }

  kfd = ctx->libc_fn.socket(domain, type, protocol);
  dbg("%s, kfd %d for domain %d type %d protocol %d\n", __func__, kfd, domain, type,
      protocol);
  if (kfd < 0) {
    err("%s, create kfd fail %d for domain %d type %d protocol %d\n", __func__, kfd,
        domain, type, protocol);
    return kfd;
  }
  if (kfd > ctx->upl_entires_nb) {
    err("%s, kfd %d too big, consider enlarge entires space %d\n", __func__, kfd,
        ctx->upl_entires_nb);
    return kfd;
  }
  if (upl_stopped(ctx)) return kfd;

  if (!ctx->has_mtl_udp) return kfd;
  ret = mufd_socket_check(domain, type, protocol);
  if (ret < 0) return kfd; /* not support by mufd */

  int ufd = mufd_socket(domain, type, protocol);
  if (ufd < 0) {
    err("%s, create ufd fail %d for domain %d type %d protocol %d\n", __func__, ufd,
        domain, type, protocol);
    return kfd; /* return kfd for fallback path */
  }

  struct upl_ufd_entry* entry = upl_zmalloc(sizeof(*entry));
  if (!entry) {
    err("%s, entry malloc fail for ufd %d\n", __func__, ufd);
    mufd_close(ufd);
    return kfd; /* return kfd for fallback path */
  }
  entry->base.upl_type = UPL_ENTRY_UFD;
  entry->base.parent = ctx;
  entry->ufd = ufd;
  entry->kfd = kfd;
  entry->efd = -1;

  ret = mufd_register_stat_dump_cb(ufd, upl_stat_dump, entry);
  if (ret < 0) {
    err("%s, register stat dump for ufd %d\n", __func__, ufd);
    upl_free(entry);
    mufd_close(ufd);
    return kfd; /* return kfd for fallback path */
  }

  upl_set_upl_entry(ctx, kfd, entry);
  info("%s, ufd %d kfd %d for domain %d type %d protocol %d\n", __func__, ufd, kfd,
       domain, type, protocol);
  return kfd;
}

int close(int fd) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, fd);
    UPL_ERR_RET(EIO);
  }

  dbg("%s(%d), start\n", __func__, fd);
  struct upl_base_entry* entry = upl_get_upl_entry(ctx, fd);
  if (!entry) return ctx->libc_fn.close(fd);

  if (entry->upl_type == UPL_ENTRY_UFD) {
    struct upl_ufd_entry* ufd_entry = (struct upl_ufd_entry*)entry;
    upl_ufd_close(ufd_entry);
  } else if (entry->upl_type == UPL_ENTRY_EPOLL) {
    struct upl_efd_entry* efd_entry = (struct upl_efd_entry*)entry;
    upl_epoll_close(efd_entry);
  } else {
    warn("%s(%d), unknow upl type %d\n", __func__, fd, entry->upl_type);
  }

  upl_free(entry);
  upl_clear_upl_entry(ctx, fd);
  /* close the kfd */
  return ctx->libc_fn.close(fd);
}

int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
#if 0
  const struct sockaddr_in* addr_in = (struct sockaddr_in*)addr;
  info("%s(%d), port %u\n", __func__, sockfd, htons(addr_in->sin_port));
#endif
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry) return ctx->libc_fn.bind(sockfd, addr, addrlen);

  int ufd = entry->ufd;
  int ret = mufd_bind(ufd, addr, addrlen);
  if (ret >= 0) return ret; /* mufd bind succ */

  /* try kernel fallback path */
  ret = ctx->libc_fn.bind(sockfd, addr, addrlen);
  if (ret < 0) return ret;
  entry->bind_kfd = true;
  info("%s(%d), mufd bind fail, fall back to libc\n", __func__, sockfd);
  return 0;
}

ssize_t sendto(int sockfd, const void* buf, size_t len, int flags,
               const struct sockaddr* dest_addr, socklen_t addrlen) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  dbg("%s(%d), len %" PRIu64 "\n", __func__, sockfd, len);
  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry) return ctx->libc_fn.sendto(sockfd, buf, len, flags, dest_addr, addrlen);

  /* ufd only support ipv4 now */
  const struct sockaddr_in* addr_in = (struct sockaddr_in*)dest_addr;
  uint8_t* ip = (uint8_t*)&addr_in->sin_addr.s_addr;
  int ufd = entry->ufd;

  if (mufd_tx_valid_ip(ufd, ip) < 0) {
    /* fallback to kfd if it's not in ufd address scope */
    dbg("%s(%d), fallback to kernel for ip %u.%u.%u.%u\n", __func__, sockfd, ip[0], ip[1],
        ip[2], ip[3]);
    entry->stat_tx_kfd_cnt++;
    return ctx->libc_fn.sendto(sockfd, buf, len, flags, dest_addr, addrlen);
  } else {
    entry->stat_tx_ufd_cnt++;
    return mufd_sendto(ufd, buf, len, flags, dest_addr, addrlen);
  }
}

ssize_t sendmsg(int sockfd, const struct msghdr* msg, int flags) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  dbg("%s(%d), start\n", __func__, sockfd);
  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || !msg->msg_name) return ctx->libc_fn.sendmsg(sockfd, msg, flags);

  if (!msg->msg_name || msg->msg_namelen < sizeof(struct sockaddr_in)) {
    warn("%s(%d), no msg_name or msg_namelen not valid\n", __func__, sockfd);
    return ctx->libc_fn.sendmsg(sockfd, msg, flags);
  }

  /* ufd only support ipv4 now */
  const struct sockaddr_in* addr_in = (struct sockaddr_in*)msg->msg_name;
  uint8_t* ip = (uint8_t*)&addr_in->sin_addr.s_addr;
  dbg("%s(%d), dst ip %u.%u.%u.%u\n", __func__, sockfd, ip[0], ip[1], ip[2], ip[3]);
  int ufd = entry->ufd;

  if (mufd_tx_valid_ip(ufd, ip) < 0) {
    /* fallback to kfd if it's not in ufd address scope */
    dbg("%s(%d), fallback to kernel for ip %u.%u.%u.%u\n", __func__, sockfd, ip[0], ip[1],
        ip[2], ip[3]);
    entry->stat_tx_kfd_cnt++;
    return ctx->libc_fn.sendmsg(sockfd, msg, flags);
  } else {
    entry->stat_tx_ufd_cnt++;
    return mufd_sendmsg(ufd, msg, flags);
  }
}

ssize_t send(int sockfd, const void* buf, size_t len, int flags) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  dbg("%s(%d), len %" PRIu64 "\n", __func__, sockfd, len);
  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry) return ctx->libc_fn.send(sockfd, buf, len, flags);

  err("%s(%d), not support ufd now\n", __func__, sockfd);
  UPL_ERR_RET(ENOTSUP);
}

int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
  return upl_ppoll(fds, nfds, timeout, NULL, NULL);
}

int ppoll(struct pollfd* fds, nfds_t nfds, const struct timespec* tmo_p,
          const sigset_t* sigmask) {
  return upl_ppoll(fds, nfds, 0, tmo_p, sigmask);
}

int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
           struct timeval* timeout) {
  return upl_pselect(nfds, readfds, writefds, exceptfds, timeout, NULL, NULL);
}

int pselect(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
            const struct timespec* timeout, const sigset_t* sigmask) {
  return upl_pselect(nfds, readfds, writefds, exceptfds, NULL, timeout, sigmask);
}

ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags, struct sockaddr* src_addr,
                 socklen_t* addrlen) {
  dbg("%s(%d), start\n", __func__, sockfd);
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || entry->bind_kfd) {
    if (entry) entry->stat_rx_kfd_cnt++;
    return ctx->libc_fn.recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
  } else {
    entry->stat_rx_ufd_cnt++;
    return mufd_recvfrom(entry->ufd, buf, len, flags, src_addr, addrlen);
  }
}

ssize_t recv(int sockfd, void* buf, size_t len, int flags) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || entry->bind_kfd) {
    if (entry) entry->stat_rx_kfd_cnt++;
    return ctx->libc_fn.recv(sockfd, buf, len, flags);
  } else {
    entry->stat_rx_ufd_cnt++;
    return mufd_recv(entry->ufd, buf, len, flags);
  }
}

ssize_t recvmsg(int sockfd, struct msghdr* msg, int flags) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || entry->bind_kfd) {
    if (entry) entry->stat_rx_kfd_cnt++;
    return ctx->libc_fn.recvmsg(sockfd, msg, flags);
  } else {
    entry->stat_rx_ufd_cnt++;
    return mufd_recvmsg(entry->ufd, msg, flags);
  }
}

int getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || entry->bind_kfd)
    return ctx->libc_fn.getsockopt(sockfd, level, optname, optval, optlen);
  else
    return mufd_getsockopt(entry->ufd, level, optname, optval, optlen);
}

int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || entry->bind_kfd)
    return ctx->libc_fn.setsockopt(sockfd, level, optname, optval, optlen);
  else
    return mufd_setsockopt(entry->ufd, level, optname, optval, optlen);
}

int fcntl(int sockfd, int cmd, va_list args) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || entry->bind_kfd)
    return ctx->libc_fn.fcntl(sockfd, cmd, args);
  else
    return mufd_fcntl(entry->ufd, cmd, args);
}

int fcntl64(int sockfd, int cmd, va_list args) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || entry->bind_kfd)
    return ctx->libc_fn.fcntl64(sockfd, cmd, args);
  else
    return mufd_fcntl(entry->ufd, cmd, args);
}

int ioctl(int sockfd, unsigned long cmd, va_list args) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s(%d), ctx init fail, pls check setup\n", __func__, sockfd);
    UPL_ERR_RET(EIO);
  }

  struct upl_ufd_entry* entry = upl_get_ufd_entry(ctx, sockfd);
  if (!entry || entry->bind_kfd)
    return ctx->libc_fn.ioctl(sockfd, cmd, args);
  else
    return mufd_ioctl(entry->ufd, cmd, args);
}

int epoll_create(int size) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    UPL_ERR_RET(EIO);
  }
  int efd = ctx->libc_fn.epoll_create(size);
  if (efd < 0) return efd;
  if (upl_stopped(ctx)) return efd;

  dbg("%s(%d), size %d\n", __func__, efd, size);
  upl_epoll_create(ctx, efd);
  return efd;
}

int epoll_create1(int flags) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    UPL_ERR_RET(EIO);
  }

  int efd = ctx->libc_fn.epoll_create1(flags);
  if (efd < 0) return efd;
  if (upl_stopped(ctx)) return efd;

  dbg("%s(%d), flags 0x%x\n", __func__, efd, flags);
  upl_epoll_create(ctx, efd);
  return efd;
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    UPL_ERR_RET(EIO);
  }

  dbg("%s(%d), op %d fd %d\n", __func__, epfd, op, fd);
  struct upl_efd_entry* efd = upl_get_efd_entry(ctx, epfd);
  if (!efd) return ctx->libc_fn.epoll_ctl(epfd, op, fd, event);

  /* if it's a ufd entry */
  struct upl_ufd_entry* ufd = upl_get_ufd_entry(ctx, fd);
  if (!ufd || ufd->bind_kfd) {
    int ret = ctx->libc_fn.epoll_ctl(epfd, op, fd, event);
    if (ret < 0) return ret;
    dbg("%s(%d), op %d for fd %d succ with libc\n", __func__, epfd, op, fd);
    if (op == EPOLL_CTL_ADD) {
      atomic_fetch_add(&efd->kfd_cnt, 1);
    } else if (op == EPOLL_CTL_DEL) {
      atomic_fetch_sub(&efd->kfd_cnt, 1);
    }
    return ret;
  }

  dbg("%s(%d), efd %p ufd %p\n", __func__, epfd, efd, ufd);
  if (op == EPOLL_CTL_ADD) {
    return upl_efd_ctl_add(efd, ufd, event);
  } else if (op == EPOLL_CTL_DEL) {
    return upl_efd_ctl_del(efd, ufd);
  } else if (op == EPOLL_CTL_MOD) {
    return upl_efd_ctl_mod(efd, ufd, event);
  } else {
    err("%s(%d:%d), unknown op %d\n", __func__, epfd, fd, op);
    UPL_ERR_RET(EINVAL);
  }
}

int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    UPL_ERR_RET(EIO);
  }

  struct upl_efd_entry* efd = upl_get_efd_entry(ctx, epfd);
  if (!efd || !upl_epoll_has_ufd(efd))
    return ctx->libc_fn.epoll_wait(epfd, events, maxevents, timeout);

  dbg("%s(%d), timeout %d maxevents %d\n", __func__, epfd, timeout, maxevents);
  /* wa to fix end loop in userspace issue */
  if (timeout <= 0) timeout = 1000 * 2;
  return upl_efd_epoll_pwait(efd, events, maxevents, timeout, NULL);
}

int epoll_pwait(int epfd, struct epoll_event* events, int maxevents, int timeout,
                const sigset_t* sigmask) {
  struct upl_ctx* ctx = upl_get_ctx();
  if (!ctx->init_succ) {
    err("%s, ctx init fail, pls check setup\n", __func__);
    UPL_ERR_RET(EIO);
  }

  struct upl_efd_entry* efd = upl_get_efd_entry(ctx, epfd);
  if (!efd || !upl_epoll_has_ufd(efd))
    return ctx->libc_fn.epoll_pwait(epfd, events, maxevents, timeout, sigmask);

  int kfd_cnt = atomic_load(&efd->kfd_cnt);
  info("%s(%d), timeout %d, kfd_cnt %d\n", __func__, epfd, timeout, kfd_cnt);
  /* wa to fix end loop in userspace issue */
  if (timeout <= 0) timeout = 1000 * 2;
  return upl_efd_epoll_pwait(efd, events, maxevents, timeout, sigmask);
}

enum mtl_log_level upl_get_log_level(void) {
  struct upl_ctx* ctx = upl_get_ctx();
  return ctx->log_level;
}