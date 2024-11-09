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
#include <sys/signalfd.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <errno.h>

LIST_HEAD(head_of_list, List_Node);
static struct head_of_list first_node;
static struct head_of_list *head_node = &first_node;
static int epoll_fd = -1;
static int sigFd;
static bool done = false;

static int loop_sig_handler(int fd, void *data) {
  struct signalfd_siginfo info;
  if (read(fd, &info, sizeof(info)) != sizeof(info))
    return LOOP_RETURN;
  switch (info.ssi_signo) {
    case SIGINT:
    case SIGTERM:
    case SIGQUIT:
    case SIGHUP:
      return LOOP_RETURN;
  }
  return LOOP_OK;
}

static inline int create_epoll_data (struct epoll_event *eventsi, int fd, void *data, Fd_Handler handler, int events, int opt) {
  struct FD_Function *epoll_event_info = NULL;
  struct List_Node *nodePtr = NULL;

  if (!handler || fd < 0 || events <= 0) {
    fprintf(stderr, "Can not add fd to epoll because of null handler\n");
    return -1;
  }

  if (opt == EPOLL_CTL_MOD) {
    LIST_FOREACH(nodePtr, head_node, node) {
      if(((struct FD_Function *)nodePtr->data)->fd == fd) {
        epoll_event_info = nodePtr->data;
        break;
      }
    }
  }
  else {
    nodePtr = malloc(sizeof(struct List_Node));
    epoll_event_info = malloc(sizeof(struct FD_Function));
    if (nodePtr && epoll_event_info) {
      memset(nodePtr, 0, sizeof(struct List_Node));
      memset(epoll_event_info, 0, sizeof(struct FD_Function));
      nodePtr->data = (void *) epoll_event_info;
      LIST_INSERT_HEAD(head_node, nodePtr, node);
    }
    else {
      if (nodePtr)
        free(nodePtr);
      nodePtr = NULL;
    }
  }
  if (epoll_event_info == NULL || nodePtr == NULL) {
    if (epoll_event_info)
      free(epoll_event_info);
    fprintf(stderr, "Can not modify epoll event info because of no address\n");
    return -1;
  }
  epoll_event_info->fd = fd;
  epoll_event_info->data = data;
  epoll_event_info->func = handler;
  epoll_event_info->events = events;
  eventsi->events = events;
  // not set fd to data.fd,beacause use ptr instead. union type
  //eventsi->data.fd = fd;
  eventsi->data.ptr = (void *)(epoll_event_info);
  return 0;
}

static void clear_epoll_data(int fd) {
  struct List_Node *nodePtr = NULL;

  LIST_FOREACH(nodePtr, head_node, node) {
    if(((struct FD_Function *)nodePtr->data)->fd == fd || fd == -2) {
      LIST_REMOVE(nodePtr, node);
      free(nodePtr->data);
      free(nodePtr);
      if (fd != -2)
        break;
    }
  }
}

static inline void fd_ctl(int fd, void *data, Fd_Handler handler, int events, int opt) {
  if (done || fd < 0)
    return;

  struct epoll_event event_data = {0};

  if (create_epoll_data(&event_data, fd, data, handler, events, opt) < 0)
    return;
  int err = epoll_ctl(epoll_fd, opt, fd, &event_data);
  if (err < 0) {
    if (opt == EPOLL_CTL_ADD)
      clear_epoll_data(fd);
    fprintf(stderr, "Can not add fd to epoll:%d\n", errno);
    exit(EXIT_FAILURE);
  }
  return;
}

void loop_add_fd(int fd, Fd_Handler handler, int events) {
  return fd_ctl(fd, NULL, handler, events, EPOLL_CTL_ADD);
}

void loop_add_fd1(int fd, Fd_Handler handler, int events, void *data) {
  return fd_ctl(fd, data, handler, events, EPOLL_CTL_ADD);
}

void loop_mod_fd(int fd, Fd_Handler handler, int events, void *data) {
  return fd_ctl(fd, data, handler, events, EPOLL_CTL_MOD);
}

void loop_remove_fd(int fd) {
  if (done || fd < 0)
    return;
  int err = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
  if (err < 0) {
    fprintf(stderr, "Can not delelte fd from epoll:%d\n", errno);
    return;
  }
  clear_epoll_data(fd);
  return;
}

void loop_create() {
  epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    fprintf(stderr, "Can not create epoll fd: %d\n", errno);
    exit(EXIT_FAILURE);
  }

  LIST_INIT(head_node);
}

void loop_init() {
  main_thread_id = pthread_self();
  sigset_t sigset;
  sigemptyset(&sigset);
  sigaddset(&sigset, SIGHUP);
  sigaddset(&sigset, SIGTERM);
  sigaddset(&sigset, SIGINT);
  sigaddset(&sigset, SIGQUIT);
  sigprocmask(SIG_BLOCK, &sigset, NULL);
  sigFd = signalfd(-1, &sigset, 0);
  loop_add_fd(sigFd, &loop_sig_handler, EPOLLIN | EPOLLERR | EPOLLHUP);
}

void loop_main() {
  done = false;
  int maxEvents = 300;

  while (!done) {
    struct epoll_event events[300] = {0};
    int fd_events = epoll_wait(epoll_fd, events, maxEvents, -1);
    if (fd_events < 0) {
      done = true;
    }
    for (int i = 0 ;i < fd_events; i++) {
      struct FD_Function *function = (struct FD_Function *)events[i].data.ptr;
      int ret = function->func(function->fd, function->data);
      if (ret == LOOP_RETURN) {
        done = true;
        break;
      }
    }
  }
}

void loop_destroy() {
  done = true;
  if (epoll_fd >= 0)
    close(epoll_fd);
  epoll_fd = -1;
  // -2 means clear list
  clear_epoll_data(-2);
}
