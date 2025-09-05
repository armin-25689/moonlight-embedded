/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
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

#include "mapping.h"

#define GRABCODE "grab"
#define UNGRABCODE "ungrab"
#define FAKEGRABCODE "fakegrab"
#define UNFAKEGRABCODE "unfakegrab"
#define EVDEV_HANDLE_BY_WINDOW 1
#define EVDEV_HANDLE_BY_EVDEV 0

enum grabWindowRequest {E_STOP_INPUT = -1, E_UNGRAB_WINDOW, E_GRAB_WINDOW};

extern int evdev_gamepads;

void evdev_create(const char* device, struct mapping* mappings, bool verbose, int rotate);
void evdev_remove_from_path(const char* path);
void evdev_loop();

void evdev_init(bool mouse_emulation_enabled);
void evdev_start();
void evdev_stop();
void evdev_map(char* device);
void evdev_rumble(unsigned short controller_id, unsigned short low_freq_motor, unsigned short high_freq_motor);
void evdev_trans_op_fd(int fd);
void evdev_init_vars(bool isfakegrab, bool issdlgp, bool isswapxyab, bool isinputadded);
int x11_sdl_init(char* mappings);
void grab_window(enum grabWindowRequest request);
void sync_input_state(bool isinputing);
void evdev_pass_mouse_mode(bool handled_by_window);
void evdev_switch_mouse_mode(bool handled_by_window);
