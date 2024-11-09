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

#include <sys/epoll.h>
#include <sys/queue.h>

#define LOOP_RETURN 1
#define LOOP_OK 0

typedef int(*Fd_Handler)(int fd, void *data);

struct FD_Function {
  Fd_Handler func;
  void*   data;
  int     fd;
  int     events;
};

struct List_Node {
  LIST_ENTRY(List_Node) node;
  void *data;
};

void loop_add_fd(int fd, Fd_Handler handler, int events);
void loop_add_fd1(int fd, Fd_Handler handler, int events, void *data);
void loop_mod_fd(int fd, Fd_Handler handler, int events, void *data);
void loop_remove_fd(int fd);

void loop_create();
void loop_init();
void loop_main();
void loop_destroy();
