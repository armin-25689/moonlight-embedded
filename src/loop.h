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

#include <sys/event.h>
#include <sys/queue.h>
#include <stdatomic.h>
#include <stdint.h>

#define LOOP_REMOVE 2
#define LOOP_RETURN 1
#define LOOP_OK 0

typedef unsigned char evwcode;
enum evWindowCode { VTF1CODE = 1, VTF2CODE, VTF3CODE, VTF4CODE, VTF5CODE, VTF6CODE, VTF7CODE, VTF8CODE, VTF9CODE, VTFACODE, VTFBCODE, VTFCCODE, QUITCODE, GRABCODE, UNGRABCODE, FAKEGRABCODE, UNFAKEGRABCODE, WINDOWSIZECHANGED, FROMDISPLAY = 128 };

typedef int(*Fd_Handler_Legacy)(int fd, void *data);
typedef int(*Fd_Handler)(uintptr_t fd, void *data);
typedef void(*Fd_Clear)(uintptr_t fd, void *data);

struct Event_Handle_Info {
  Fd_Handler func;
  Fd_Clear   clean;
  void*      data;
  uintptr_t  fd;
  int        events;
  uint32_t   id;
};

struct List_Node {
  LIST_ENTRY(List_Node) node;
  void *data;
};

extern atomic_bool done;

// no use
void loop_add_fd(int fd, Fd_Handler_Legacy handler, int events);
// use add_fd0 instead add_fd
void loop_add_fd0(uintptr_t fd, Fd_Handler handler, int events);
void loop_add_fd1(uintptr_t fd, Fd_Handler handler, Fd_Clear clean, int events, void *data);
void loop_add_timer(uintptr_t *fd_ptr, Fd_Handler handler, uint32_t time_flag, int64_t time, void *data);
void loop_add_timer_oneshot(Fd_Handler handler, uint32_t time_flag, int64_t time, void *data);
void loop_add_notify(uintptr_t *fd_ptr, Fd_Handler handler);
void loop_notify(uintptr_t ident, uint32_t udata);
void loop_add_window_notify(Fd_Handler handler);
void loop_notify_window(uint32_t udata);
void loop_active_poll();
void loop_remove_fd(int fd);
void loop_remove_ident(uintptr_t fd, int event);

void loop_create();
void loop_start();
void loop_main();
void loop_destroy();
