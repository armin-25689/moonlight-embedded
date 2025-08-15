/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2017 Iwan Timmer
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

#include "util.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int write_bool(char *path, bool val) {
  int fd = open(path, O_RDWR);

  if(fd >= 0) {
    int ret = write(fd, val ? "1" : "0", 1);
    if (ret < 0)
      fprintf(stderr, "Failed to write %d to %s: %d\n", val ? 1 : 0, path, ret);

    close(fd);
    return 0;
  } else
    return -1;
}

int read_file(char *path, char* output, int output_len) {
  int fd = open(path, O_RDONLY);

  if(fd >= 0) {
    output_len = read(fd, output, output_len);
    close(fd);
    return output_len;
  } else
    return -1;
}

bool ensure_buf_size(void **buf, size_t *buf_size, size_t required_size) {
  if (*buf_size >= required_size)
    return false;

  *buf_size = required_size;
  *buf = realloc(*buf, *buf_size);
  if (!*buf) {
    fprintf(stderr, "Failed to allocate %zu bytes\n", *buf_size);
    abort();
  }

  return true;
}

int get_drm_render_fd(char exportedPath[64]) {
  int fd = -1;
  char path[64];
  int n, max_devices = 8;
  for (n = 0; n < max_devices; n++) {
    snprintf(path, sizeof(path), "/dev/dri/renderD%d", 128 + n);
    fd = open(path, O_RDWR);
    if (fd < 0) {
      if (errno == ENOENT) {
        if (n != max_devices - 1) {
          fprintf(stderr, "No drm render device %s,try next drm render node.\n", path);
          continue;
        }
        fprintf(stderr, "No available render node.\n");
      }
      else {
        fprintf(stderr, "Cannot open drm node: /dev/dri/renderD128 + %d.\n", n);
      }
      break;
    }
    else {
      memcpy(exportedPath, path, strlen(path) + 1);
      break;
    }
  }

  return fd;
}

static int mkdir_p_for_file(char *path) {
  char inpath[255] = {0};

  if (path == NULL)
    return -1;

  strcpy(inpath, path);
  dirname(inpath);
  if (access(inpath, W_OK) == 0) {
    return 0;
  }
  else if (strcmp(path, inpath) == 0) {
    perror("Cannot create the directory");
    return -1;
  }
  else {
    if (mkdir_p_for_file(inpath) == 0) {
      if (mkdir(inpath, 0755) < 0)
        perror("Cannot create directory");
      return 0;
     }
     else
      return -1;
  }
  
  return -1;
}

int create_file(char *filename) {
  if (mkdir_p_for_file(filename) != 0)
    return -1;
  int fd = open(filename,  O_WRONLY | O_CREAT, 0644);
  if (fd < 0) {
    perror("Cannot create the file");
    return -1;
  }
  else {
    close(fd);
    return 0;
  }

  return -1;
}
