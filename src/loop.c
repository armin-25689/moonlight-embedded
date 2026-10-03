/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2019 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "loop.h"

#include "connection.h"
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <errno.h>

#define POLL_CTL_ADD 1
#define POLL_CTL_DEL 4
#define POLL_CTL_FLAG 0xF
#define POLL_FLAGS_ONESHOT_ADD 0x10
#define POLL_FLAGS_MOD_EVENT 0x20
#define MAX_UDEFINED_IDENT 0xFFFF

atomic_bool done = false;

LIST_HEAD(head_of_list, List_Node);
static struct head_of_list first_node;
static struct head_of_list *head_node = &first_node;
static struct Event_Handle_Info *oneshot_list = NULL;
static uint8_t *udefined_ident = NULL;
static u_int udefined_ident_count = 32;
static atomic_uint *udata_list = NULL;
static u_int udata_count = 4;
static int kqueue_fd = -1;
static uintptr_t loop_active_ident = 0;
static uintptr_t loop_window_ident = 0;
static bool exitnow = false;

static inline atomic_uint* get_udata_ptr () {
  static atomic_uint index = 0;
  u_int nowindex = atomic_fetch_add(&index, 1);
  u_int maxindex = udata_count;

  if (nowindex >= maxindex) {
    int new_count = (maxindex << 1);
    void *newlist = realloc(udata_list, sizeof(*udata_list) * new_count);
    if (newlist == NULL) {
      fprintf(stderr, "Realloc new udata list failed.\n");
      exit(EXIT_FAILURE);
    }
    udata_list = newlist;
    udata_count = new_count;
  }
  return &udata_list[index];
}

static inline uint32_t get_udefined_ident () {
  static atomic_uint index = 1;

#define GET_IDENT_INDEX(start, end) \
  for (int i = start; i < end; i++) { \
    if (udefined_ident[i] != 0) { \
      index = i; \
      udefined_ident[i] = 0; \
      return i; \
    } \
  }

  u_int now_index = index;
  u_int max_index = udefined_ident_count;
  GET_IDENT_INDEX(now_index, max_index);
  GET_IDENT_INDEX(1, now_index);
  if (max_index < MAX_UDEFINED_IDENT) {
    int new_count = (max_index << 1);
    void *newlist = realloc(udefined_ident, sizeof(*udefined_ident) * new_count);
    void *newoneshotlist = realloc(oneshot_list, sizeof(*oneshot_list) * new_count);
    if (newlist == NULL || newoneshotlist == NULL) {
      fprintf(stderr, "Realloc new list failed.\n");
      exit(EXIT_FAILURE);
    }
    udefined_ident = newlist;
    oneshot_list = newoneshotlist;
    memset(udefined_ident + max_index, 1, sizeof(*udefined_ident) * (new_count - max_index));
    udefined_ident[max_index] = 0;
    index = max_index;
    udefined_ident_count = new_count;
    return max_index;
  }
#undef GET_IDENT_INDEX
  fprintf(stderr, "Get index failed.\n");
  exit(EXIT_FAILURE);
}

static inline void restore_udefined_ident(uint32_t id) {
  udefined_ident[id] = 1;
}

static int loop_sig_handler(uintptr_t fd, void *data) {
  switch (fd) {
    case SIGINT:
    case SIGTERM:
    case SIGQUIT:
    case SIGHUP:
      exitnow = true;
      return LOOP_RETURN;
  }
  return LOOP_OK;
}

static int loop_active_handle(uintptr_t fd, void *data) { return LOOP_OK; };

#define CLEAR_QUEUE_DATA(loop_expression, test_conditions, exit_expression) \
  do { \
  struct List_Node *nodePtr = NULL; \
  loop_expression { \
    if((test_conditions)) { \
      LIST_REMOVE(nodePtr, node); \
      free(nodePtr->data); \
      free(nodePtr); \
      exit_expression; \
    } \
  } \
  } while(0)

static inline bool test_node(struct Event_Handle_Info *info, uintptr_t fd, int event) {
  return (info->fd == fd && info->events == event);
}

static void clear_kqueue_data(uintptr_t fd, int event) {
  CLEAR_QUEUE_DATA(LIST_FOREACH(nodePtr, head_node, node), test_node(nodePtr->data, fd, event), break);
}

static inline void clear_kqueue_data_all() {
  CLEAR_QUEUE_DATA(while((nodePtr = LIST_FIRST(head_node))), true, continue);
}
#undef CLEAR_QUEUE_DATA

#define EVENT_STORE_INFO(info, ident, vdata, vhandler, vclean, vevents, vid) \
  info->fd = ident; \
  info->data = vdata; \
  info->func = vhandler; \
  info->clean = vclean; \
  info->events = vevents; \
  info->id = vid;

static inline struct Event_Handle_Info *create_kqueue_data (uintptr_t fd, void *data, Fd_Handler handler, Fd_Clear clean, int events, int opt) {
  struct Event_Handle_Info *kqueue_event_info = NULL;
  struct List_Node *nodePtr = NULL;

  if (opt & POLL_FLAGS_MOD_EVENT) {
    LIST_FOREACH(nodePtr, head_node, node) {
      if(test_node(nodePtr->data, fd, events)) {
        kqueue_event_info = nodePtr->data;
        break;
      }
    }
  }
  if (kqueue_event_info == NULL) {
    nodePtr = malloc(sizeof(struct List_Node));
    kqueue_event_info = malloc(sizeof(struct Event_Handle_Info));
    if (nodePtr && kqueue_event_info) {
      memset(nodePtr, 0, sizeof(struct List_Node));
      memset(kqueue_event_info, 0, sizeof(struct Event_Handle_Info));
      nodePtr->data = (void *) kqueue_event_info;
      LIST_INSERT_HEAD(head_node, nodePtr, node);
    }
    else {
      if (nodePtr)
        free(nodePtr);
      nodePtr = NULL;
    }
  }
  if (kqueue_event_info == NULL || nodePtr == NULL) {
    if (kqueue_event_info)
      free(kqueue_event_info);
    fprintf(stderr, "Can not modify kqueue event info because of no address\n");
    return NULL;
  }

  EVENT_STORE_INFO(kqueue_event_info, fd, data, handler, clean, events, 0);
  return kqueue_event_info;
}

static inline void fd_ctl(uintptr_t fd, void *data, Fd_Handler handler, Fd_Clear clean, int events, uint32_t event_flags, uint32_t event_fflags, int64_t event_data, int opt) {
  if (atomic_load_explicit(&done, memory_order_relaxed))
    return;

  if (events == 0) {
    fprintf(stderr, "Can not add fd to kqueue because of invalid fd or events\n");
    return;
  }

  void *infos = NULL;
  uintptr_t ident = fd;
  u_short flags = event_flags;
  u_int fflags = event_fflags;
  int64_t fdata = event_data;

  switch (opt & POLL_CTL_FLAG) {
  case POLL_CTL_ADD:
    switch (events) {
    case EVFILT_TIMER:
    case EVFILT_USER:
      if (fd == 0) {
        fprintf(stderr, "Invalid fd.\n");
        return;
      }
      ident = *((uintptr_t *)fd);
      break;
    case EVFILT_READ:
    case EVFILT_SIGNAL:
      break;
    case EVFILT_VNODE:
      flags |= EV_CLEAR;
      if (fflags == 0)
        fflags |= NOTE_WRITE;
      break;
    default:
      fprintf(stderr, "Not supported event type: %d\n", events);
      return;
    }
    if (handler == NULL) {
      fprintf(stderr, "Not handler to store.\n");
      return;
    }
    if (opt & POLL_FLAGS_ONESHOT_ADD) {
      uint32_t id = get_udefined_ident();
      infos = &oneshot_list[id];
      struct Event_Handle_Info *kqueue_event_info = infos;
      EVENT_STORE_INFO(kqueue_event_info, ident, data, handler, clean, events, id);
    }
    else {
      infos = create_kqueue_data(ident, data, handler, clean, events, opt);
      if (infos == NULL) {
        fprintf(stderr, "Can not create queue data:%lu,%d\n", ident, events);
        return;
      }
    }
    if (ident != fd) {
      if (*((uintptr_t *)fd) != (uintptr_t)infos) {
        *((uintptr_t *)fd) = (uintptr_t)infos;
        ident = (uintptr_t)infos;
        ((struct Event_Handle_Info *)infos)->fd = ident;
      }
    }
    break;
  case POLL_CTL_DEL:
    if (ident >= 0) {
      clear_kqueue_data(ident, events);
    }
    else
      fprintf(stderr, "Can not delelte fd from kqueue:%lu,%d\n", ident, events);
    break;
  default:
    fprintf(stderr, "Can not opt kqueue.\n");
    return;
  }

  struct kevent add_event = {0};
  EV_SET(&add_event, ident, events, flags, fflags, fdata, infos);
  if (kevent(kqueue_fd, &add_event, 1, NULL, 0, NULL) < 0) {
    fprintf(stderr, "WARNNING: Opt poll list failed: ident(%lu),event(%d),opts(%d)\n", ident, events, opt);
  }

  return;
}

void loop_add_fd(int fd, Fd_Handler_Legacy handler, int events) {
  return;
}

void loop_add_fd0(uintptr_t fd, Fd_Handler handler, int events) {
  return fd_ctl(fd, NULL, handler, NULL, events == 0 ? EVFILT_READ : events, EV_ADD, 0, 0, POLL_CTL_ADD | POLL_FLAGS_MOD_EVENT);
}

void loop_add_fd1(uintptr_t fd, Fd_Handler handler, Fd_Clear clean, int events, void *data) {
  return fd_ctl(fd, data, handler, clean, events == 0 ? EVFILT_READ : events, EV_ADD, 0, 0, POLL_CTL_ADD | POLL_FLAGS_MOD_EVENT);
}

void loop_add_timer(uintptr_t *fd_ptr, Fd_Handler handler, uint32_t time_flag, int64_t time, void *data) {
  return fd_ctl((uintptr_t)fd_ptr, data, handler, NULL, EVFILT_TIMER, EV_ADD, time_flag, time, POLL_CTL_ADD | POLL_FLAGS_MOD_EVENT);
}

void loop_add_timer_oneshot(Fd_Handler handler, uint32_t time_flag, int64_t time, void *data) {
  uintptr_t fd = 0;
  return fd_ctl((uintptr_t)(&fd), data, handler, NULL, EVFILT_TIMER, EV_ADD | EV_ONESHOT, time_flag, time, POLL_CTL_ADD | POLL_FLAGS_ONESHOT_ADD);
}

void loop_add_notify(uintptr_t *fd_ptr, Fd_Handler handler) {
  atomic_uint *udata_ptr = get_udata_ptr();
  atomic_store(udata_ptr, 0);
  return fd_ctl((uintptr_t)fd_ptr, udata_ptr, handler, NULL, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, POLL_CTL_ADD | POLL_FLAGS_MOD_EVENT);
}

void loop_notify (uintptr_t ident, uint32_t udata) {
  if (ident == 0)
    return;
  struct kevent event = {0};
  struct Event_Handle_Info *infos = ((void *)ident);
  atomic_uint *data = infos->data;
  if (data == NULL) {
    if (udata > 0)
      fprintf(stderr, "Added notify data is NULL, cannot to add userdata to notify(%lu).\n", ident);
    return;
  }
  atomic_fetch_or(data, udata);
  EV_SET(&event, ident, EVFILT_USER, EV_KEEPUDATA, NOTE_TRIGGER, 0, infos);
  kevent(kqueue_fd, &event, 1, NULL, 0, NULL);
  return;
}

void loop_add_window_notify(Fd_Handler handler) {
  return loop_add_notify(&loop_window_ident, handler);
}

void loop_notify_window(uint32_t udata) {
  return loop_notify(loop_window_ident, udata);
}

void loop_active_poll () {
  return loop_notify(loop_active_ident, 0);
}

void loop_remove_ident(uintptr_t fd, int event) {
  return fd_ctl(fd, NULL, NULL, NULL, event, EV_DELETE, 0, 0, POLL_CTL_DEL);
}

void loop_remove_fd(int fd) {
  return loop_remove_ident(fd, EVFILT_READ);
}

void loop_create() {
  kqueue_fd = kqueuex(KQUEUE_CLOEXEC);
  if (kqueue_fd < 0) {
    fprintf(stderr, "Can not create kqueue fd: %d\n", errno);
    exit(EXIT_FAILURE);
  }
  oneshot_list = calloc(udefined_ident_count, sizeof(*oneshot_list));
  udefined_ident = calloc(udefined_ident_count, sizeof(*udefined_ident));
  udata_list = calloc(udata_count, sizeof(*udata_list));
  if (udefined_ident == NULL || oneshot_list == NULL || udata_list == NULL) {
    close(kqueue_fd);
    fprintf(stderr, "Can not create u_ident or oneshot_list pool: %d\n", errno);
    exit(EXIT_FAILURE);
  }
  memset(udefined_ident, 1, sizeof(*udefined_ident) * udefined_ident_count);

  LIST_INIT(head_node);

  main_thread_id = pthread_self();
  sigset_t sigset;
  sigemptyset(&sigset);
  sigaddset(&sigset, SIGHUP);
  sigaddset(&sigset, SIGTERM);
  sigaddset(&sigset, SIGINT);
  sigaddset(&sigset, SIGQUIT);
  sigaddset(&sigset, SIGTSTP);
  sigprocmask(SIG_BLOCK, &sigset, NULL);
  loop_add_fd0(SIGHUP, &loop_sig_handler, EVFILT_SIGNAL);
  loop_add_fd0(SIGTERM, &loop_sig_handler, EVFILT_SIGNAL);
  loop_add_fd0(SIGINT, &loop_sig_handler, EVFILT_SIGNAL);
  loop_add_fd0(SIGQUIT, &loop_sig_handler, EVFILT_SIGNAL);
  loop_add_notify(&loop_active_ident, &loop_active_handle);
  if (loop_active_ident == 0) {
    fprintf(stderr, "Add active event to queue failed.\n");
    exit(EXIT_FAILURE);
  }

  return;
}

void loop_main() {
  #define MAXEVENTSCOUNT 100

  while (!atomic_load_explicit(&done, memory_order_relaxed)) {
    struct kevent events[MAXEVENTSCOUNT] = {0};
    int fd_events = kevent(kqueue_fd, NULL, 0, events, MAXEVENTSCOUNT, NULL);
    if (fd_events < 0) {
      if (errno == EINTR)
        continue;
      else
        goto failed;
      break;
    }
    for (int i = 0 ;i < fd_events; i++) {
      if (events[i].udata == NULL)
        continue;
      struct Event_Handle_Info *info = (struct Event_Handle_Info *)events[i].udata;
      if (events[i].flags & (EV_EOF | EV_ERROR)) {
        if (info->clean)
          info->clean(events[i].ident, info->data);
        loop_remove_ident(events[i].ident, events[i].filter);
        for (int j = (i + 1); j < fd_events; j++) {
          if (events[i].udata == events[j].udata)
            events[j].udata = NULL;
        }
        //restore tmp key
        if (info->id > 0) restore_udefined_ident(info->id);
        continue;
      }
      int ret = info->func(events[i].ident, info->data);
      if (info->id > 0) restore_udefined_ident(info->id);
      switch (ret) {
      case LOOP_OK:
        break;
      case LOOP_RETURN:
        goto failed;
      case LOOP_REMOVE:
        for (int j = (i + 1); j < fd_events; j++) {
          if (events[i].udata == events[j].udata)
            events[j].udata = NULL;
        }
        break;
      }
    }
  }
  #undef MAXEVENTSCOUNT

failed:
  done = true;
}

void loop_start() {
  if (exitnow) return;
  done = false;
  loop_main();
}

void loop_destroy() {
  done = true;
  if (kqueue_fd >= 0)
    close(kqueue_fd);
  kqueue_fd = -1;
  clear_kqueue_data_all();
  if (udefined_ident)
    free(udefined_ident);
  udefined_ident = NULL;
  if (oneshot_list)
    free(oneshot_list);
  oneshot_list = NULL;
  if (udata_list)
    free(udata_list);
  udata_list = NULL;
}
