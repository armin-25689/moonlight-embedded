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

static struct FD_Function fd_functions[maxEpollFds] = {};
static int epoll_fd = -1;
static int sigFd;

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
  if (!handler || fd < 0 || events <= 0) {
    fprintf(stderr, "Can not add fd to epoll because of null handler\n");
    return -1;
  }

  int index = -1;
  if (opt == EPOLL_CTL_MOD) {
    for (int i = 0; i < maxEpollFds; i++) {
      if (fd_functions[i].fd == fd) {
        index = i;
        break;
      }
    }
  }
  else {
    for (int i = 0; i < maxEpollFds; i++) {
      if (!fd_functions[i].func) {
        index = i;
        break;
      }
    }
  }
  if (index < 0) {
    fprintf(stderr, "Can not add fd to epoll because of max fd numbers\n");
    return -1;
  }
  fd_functions[index].fd = fd;
  fd_functions[index].data = data;
  fd_functions[index].func = handler;
  fd_functions[index].events = events;
  eventsi->events = events;
  // not set fd to data.fd,beacause use ptr instead. union type
  //eventsi->data.fd = fd;
  eventsi->data.ptr = (void *)(&fd_functions[index]);
  return 0;
}

static inline void fd_ctl(int fd, void *data, Fd_Handler handler, int events, int opt) {
  struct epoll_event event_data = {0};

  if (create_epoll_data(&event_data, fd, data, handler, events, opt) < 0)
    return;
  int err = epoll_ctl(epoll_fd, opt, fd, &event_data);
  if (err < 0) {
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
  int err = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
  if (err < 0) {
    fprintf(stderr, "Can not delelte fd from epoll:%d\n", errno);
    return;
  }
  for (int i = 0; i < maxEpollFds; i++) {
    if (fd_functions[i].fd == fd) {
      memset(&fd_functions[i], 0, sizeof(struct FD_Function));
      break;
    }
  }
  return;
}

void loop_create() {
  epoll_fd = epoll_create(maxEpollFds);
  if (epoll_fd < 0) {
    fprintf(stderr, "Can not create epoll fd: %d\n", errno);
    exit(EXIT_FAILURE);
  }
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
  bool done = false;
  while (!done) {
    struct epoll_event events[maxEpollFds] = {0};
    int fd_events = epoll_wait(epoll_fd, events, maxEpollFds, -1);
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
  if (epoll_fd >= 0)
    close(epoll_fd);
  epoll_fd = -1;
}
