/* Copyright (C) 2025 John Törnblom

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include <sys/errno.h>
#include <sys/mman.h>

#include "io.h"


/**
 * Size of the header in front of each allocation that holds its mapped length.
 **/
#define MEM_HEADER 16


void*
mem_alloc(size_t n) {
  size_t len = n + MEM_HEADER;
  void* base;

  if(len < n) {
    errno = ENOMEM;
    return NULL;
  }

  base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
	      -1, 0);
  if(base == MAP_FAILED) {
    return NULL;
  }

  *(size_t*)base = len;

  return (uint8_t*)base + MEM_HEADER;
}


void
mem_free(void* ptr) {
  uint8_t* base;
  int saved = errno;

  if(!ptr) {
    return;
  }

  base = (uint8_t*)ptr - MEM_HEADER;
  munmap(base, *(size_t*)base);
  errno = saved;
}


int
io_nread(int fd, void* buf, size_t n) {
  size_t done = 0;
  ssize_t r;

  while(done < n) {
    if((r=read(fd, (uint8_t*)buf + done, n - done)) < 0) {
      if(errno == EINTR) {
	continue;
      }
      return -1;
    }

    if(!r) {
      errno = EIO;
      return -1;
    }

    done += r;
  }

  return 0;
}


int
io_nwrite(int fd, const void* buf, size_t n) {
  size_t done = 0;
  ssize_t r;

  while(done < n) {
    if((r=write(fd, (const uint8_t*)buf + done, n - done)) < 0) {
      if(errno == EINTR) {
	continue;
      }
      return -1;
    }

    if(!r) {
      errno = EIO;
      return -1;
    }

    done += r;
  }

  return 0;
}


int
io_ncopy(int fd_in, int fd_out, size_t size) {
  size_t copied = 0;
  void* buf;
  ssize_t n;

  if(!(buf=mem_alloc(IO_COPY_BUFSIZE))) {
    return -1;
  }

  while(copied < size) {
    n = size - copied;
    if(n > IO_COPY_BUFSIZE) {
      n = IO_COPY_BUFSIZE;
    }

    if(io_nread(fd_in, buf, n)) {
      mem_free(buf);
      return -1;
    }
    if(io_nwrite(fd_out, buf, n)) {
      mem_free(buf);
      return -1;
    }

    copied += n;
  }

  mem_free(buf);
  return 0;
}


int
io_pread(int fd, void* buf, size_t n, off_t off) {
  size_t done = 0;
  ssize_t r;

  while(done < n) {
    if((r=pread(fd, (uint8_t*)buf + done, n - done, off + done)) < 0) {
      if(errno == EINTR) {
	continue;
      }
      return -1;
    }

    if(!r) {
      errno = EIO;
      return -1;
    }

    done += r;
  }

  return 0;
}


int
io_pwrite(int fd, const void* buf, size_t n, off_t off) {
  size_t done = 0;
  ssize_t r;

  while(done < n) {
    if((r=pwrite(fd, (const uint8_t*)buf + done, n - done, off + done)) < 0) {
      if(errno == EINTR) {
	continue;
      }
      return -1;
    }

    if(!r) {
      errno = EIO;
      return -1;
    }

    done += r;
  }

  return 0;
}


int
io_pcopy(int fd_in, int fd_out, off_t off_in, off_t off_out, size_t size) {
  size_t copied = 0;
  void* buf;
  ssize_t n;

  if(!(buf=mem_alloc(IO_COPY_BUFSIZE))) {
    return -1;
  }

  while(copied < size) {
    n = size - copied;
    if(n > IO_COPY_BUFSIZE) {
      n = IO_COPY_BUFSIZE;
    }

    if(io_pread(fd_in, buf, n, off_in)) {
      mem_free(buf);
      return -1;
    }
    if(io_pwrite(fd_out, buf, n, off_out)) {
      mem_free(buf);
      return -1;
    }

    off_out += n;
    off_in += n;
    copied += n;
  }

  mem_free(buf);
  return 0;
}
