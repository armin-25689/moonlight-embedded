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
#define POLL_CTL_MOD 2
#define POLL_CTL_DEL 4

LIST_HEAD(head_of_list, List_Node);
static struct head_of_list first_node;
static struct head_of_list *head_node = &first_node;
static int kqueue_fd = -1;
static bool exitnow = false;
bool done = false;

static int loop_sig_handler(int fd, void *data) {
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

static void clear_kqueue_data(int fd, int event) {
  struct List_Node *nodePtr = NULL;

  LIST_FOREACH(nodePtr, head_node, node) {
    if((((struct FD_Function *)nodePtr->data)->fd == fd && ((struct FD_Function *)nodePtr->data)->events == event) || fd == -2) {
      LIST_REMOVE(nodePtr, node);
      free(nodePtr->data);
      free(nodePtr);
      if (fd != -2)
        break;
    }
  }
}

static inline struct FD_Function *create_kqueue_data (int fd, void *data, Fd_Handler handler, Fd_Clear clean, int events, int opt) {
  struct FD_Function *kqueue_event_info = NULL;
  struct List_Node *nodePtr = NULL;

  if (fd < 0 || events == 0) {
    fprintf(stderr, "Can not add fd to kqueue because of invalid fd or events\n");
    return NULL;
  }

  if (handler == NULL) {
    fprintf(stderr, "Can not add fd to kqueue because of null handler\n");
    return NULL;
  }
  if (opt == POLL_CTL_MOD) {
    LIST_FOREACH(nodePtr, head_node, node) {
      if(((struct FD_Function *)nodePtr->data)->fd == fd && ((struct FD_Function *)nodePtr->data)->events == events) {
        kqueue_event_info = nodePtr->data;
        break;
      }
    }
  }
  if (kqueue_event_info == NULL) {
    nodePtr = malloc(sizeof(struct List_Node));
    kqueue_event_info = malloc(sizeof(struct FD_Function));
    if (nodePtr && kqueue_event_info) {
      memset(nodePtr, 0, sizeof(struct List_Node));
      memset(kqueue_event_info, 0, sizeof(struct FD_Function));
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

  kqueue_event_info->fd = fd;
  kqueue_event_info->data = data;
  kqueue_event_info->func = handler;
  kqueue_event_info->clean = clean;
  kqueue_event_info->events = events;
  return kqueue_event_info;
}

static inline void fd_ctl(int fd, void *data, Fd_Handler handler, Fd_Clear clean, int events, int opt) {
  if (done)
    return;

  struct kevent event_data = {0};
  struct FD_Function *infos = NULL;

  switch (opt) {
  case POLL_CTL_ADD:
  case POLL_CTL_MOD:
    infos = create_kqueue_data(fd, data, handler, clean, events, opt);
    if (infos == NULL) {
      fprintf(stderr, "Can not create queue data:%d,%d\n", fd, events);
      return;
    }
    u_int fflags = 0;
    u_short flags = EV_ADD;
    int64_t fdata = 0;
    switch (events) {
    case EVFILT_READ:
    case EVFILT_SIGNAL:
      break;
    case EVFILT_VNODE:
      flags |= EV_CLEAR;
      fflags |= NOTE_WRITE;
      break;
    case EVFILT_TIMER:
      fflags |= NOTE_MSECONDS;
      fdata = fd;
      break;
    }
    EV_SET(&event_data, fd, events, flags, fflags, fdata, (void *)infos);
    break;
  case POLL_CTL_DEL:
    if (events != 0 && fd >= 0)
      clear_kqueue_data(fd, events);
    else
      fprintf(stderr, "Can not delelte fd from kqueue:%d,%d\n", fd, events);
    EV_SET(&event_data, fd, events, EV_DELETE, 0, 0, NULL);
    break;
  default:
    fprintf(stderr, "Can not opt kqueue.\n");
    return;
  }
  int err = kevent(kqueue_fd, &event_data, 1, NULL, 0, NULL);
  if (err < 0) {
    if (opt != POLL_CTL_DEL) {
      clear_kqueue_data(fd, events);
      fprintf(stderr, "Can not add fd to kqueue:%d\n", errno);
      exit(EXIT_FAILURE);
    }
  }
  return;
}

void loop_add_fd(int fd, Fd_Handler handler, int events) {
  return fd_ctl(fd, NULL, handler, NULL, events >= 0 ? EVFILT_READ : events, POLL_CTL_ADD);
}

void loop_add_fd1(int fd, Fd_Handler handler, Fd_Clear clean, int events, void *data) {
  return fd_ctl(fd, data, handler, clean, events == 0 ? EVFILT_READ : events, POLL_CTL_ADD);
}

void loop_mod_fd(int fd, Fd_Handler handler, Fd_Clear clean, int events, void *data) {
  return fd_ctl(fd, data, handler, clean, events == 0 ? EVFILT_READ : events, POLL_CTL_MOD);
}

void loop_remove_ident(int fd, int event) {
  return fd_ctl(fd, NULL, NULL, NULL, event, POLL_CTL_DEL);
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
  loop_add_fd(SIGHUP, &loop_sig_handler, EVFILT_SIGNAL);
  loop_add_fd(SIGTERM, &loop_sig_handler, EVFILT_SIGNAL);
  loop_add_fd(SIGINT, &loop_sig_handler, EVFILT_SIGNAL);
  loop_add_fd(SIGQUIT, &loop_sig_handler, EVFILT_SIGNAL);
}

void loop_main() {
  int maxEvents = 300;

  while (!done) {
    struct kevent events[300] = {0};
    int fd_events = kevent(kqueue_fd, NULL, 0, events, maxEvents, NULL);
    if (fd_events < 0) {
      if (errno == EINTR)
        continue;
      else
        done = true;
      break;
    }
    for (int i = 0 ;i < fd_events; i++) {
      if (events[i].udata == NULL)
        continue;
      struct FD_Function *function = (struct FD_Function *)events[i].udata;
      if (events[i].flags & (EV_EOF | EV_ERROR)) {
        if (function->clean)
          function->clean((int) events[i].ident, function->data);
        loop_remove_ident((int) events[i].ident, events[i].filter);
        for (int j = (i + 1); j < fd_events; j++) {
          if (events[i].udata == events[j].udata)
            events[j].udata = NULL;
        }
        continue;
      }
      int ret = function->func((int) events[i].ident, function->data);
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
  // -2 means clear list
  clear_kqueue_data(-2, 0);
}
