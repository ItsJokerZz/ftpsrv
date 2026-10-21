/* Copyright (C) 2023 John Törnblom

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

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "cmd.h"
#include "io.h"
#include "log.h"
#include "self.h"


/**
 * Milliseconds to wait for a client to open a passive data connection.
 **/
#define FTP_DATA_TIMEOUT 30000


/**
 * Create a string representation of a file mode.
 **/
static void
ftp_mode_string(mode_t mode, char *buf) {
  char c, d;
  int i, bit;

  buf[10] = 0;
  for(i=0; i<9; i++) {
    bit = mode & (1<<i);
    c = i%3;
    if(!c && (mode & (1<<((d=i/3)+9)))) {
      c = "tss"[(int)d];
      if (!bit) c &= ~0x20;
    } else c = bit ? "xwr"[(int)c] : '-';
    buf[9-i] = c;
  }

  if (S_ISDIR(mode)) c = 'd';
  else if (S_ISBLK(mode)) c = 'b';
  else if (S_ISCHR(mode)) c = 'c';
  else if (S_ISLNK(mode)) c = 'l';
  else if (S_ISFIFO(mode)) c = 'p';
  else if (S_ISSOCK(mode)) c = 's';
  else c = '-';
  *buf = c;
}


/**
 * Open the data connection.
 */
int
ftp_data_open(ftp_env_t *env) {
  struct sockaddr_in peer_addr;
  struct sockaddr_in data_addr;
  socklen_t addr_len;
  struct pollfd pfd;
  int opt;
  int ret;
  int fd;

  if(env->data_fd != -1) {
    close(env->data_fd);
    env->data_fd = -1;
  }

  if(env->data_addr.sin_port) {
    data_addr = env->data_addr;
    memset(&env->data_addr, 0, sizeof(env->data_addr));

    if((fd=socket(AF_INET, SOCK_STREAM, 0)) < 0) {
      return -1;
    }

    if(connect(fd, (struct sockaddr*)&data_addr, sizeof(data_addr))) {
      opt = errno;
      close(fd);
      errno = opt;
      return -1;
    }
  } else {
    if(env->passive_fd < 0) {
      errno = ENOTCONN;
      return -1;
    }

    pfd.fd = env->passive_fd;
    pfd.events = POLLIN;
    do {
      pfd.revents = 0;
      do {
	ret = poll(&pfd, 1, FTP_DATA_TIMEOUT);
      } while(ret < 0 && errno == EINTR);

      if(ret <= 0) {
	if(!ret) {
	  errno = ETIMEDOUT;
	}
	return -1;
      }

      addr_len = sizeof(data_addr);
      if((fd=accept(env->passive_fd, (struct sockaddr*)&data_addr,
		    &addr_len)) < 0) {
	return -1;
      }

      addr_len = sizeof(peer_addr);
      if(!getpeername(env->active_fd, (struct sockaddr*)&peer_addr,
		      &addr_len) &&
	 peer_addr.sin_addr.s_addr == data_addr.sin_addr.s_addr) {
	break;
      }

      close(fd);
    } while(1);

    close(env->passive_fd);
    env->passive_fd = -1;
  }

  opt = 0x100000;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &opt, sizeof(opt));
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &opt, sizeof(opt));

  env->data_fd = fd;

  return 0;
}


/**
 * Write a formatted string to a file descriptor, truncated to fit a buffer.
 **/
static int
ftp_vwrite(int fd, const char *fmt, va_list args) {
  char buf[0x1000];
  int len;

  len = vsnprintf(buf, sizeof(buf), fmt, args);
  if(len < 0) {
    errno = EINVAL;
    return -1;
  }

  if((size_t)len >= sizeof(buf)) {
    len = sizeof(buf) - 1;
  }

  return io_nwrite(fd, buf, len);
}


/**
 * Transmit a formatted string via an existing data connection.
 **/
static int
ftp_data_printf(ftp_env_t *env, const char *fmt, ...) {
  va_list args;
  int ret;

  va_start(args, fmt);
  ret = ftp_vwrite(env->data_fd, fmt, args);
  va_end(args);

  return ret;
}


/**
 * Read data from an existing data connection.
 **/
static int
ftp_data_read(ftp_env_t *env, void *buf, size_t count) {
  return recv(env->data_fd, buf, count, 0);
}


/**
 * Close the data connection.
 **/
int
ftp_data_close(ftp_env_t *env) {
  if(close(env->data_fd)) {
    return -1;
  }

  env->data_fd = -1;

  return 0;
}


/**
 * Write a string to the active connection with printf semantics.
 **/
int
ftp_active_printf(ftp_env_t *env, const char *fmt, ...) {
  va_list args;
  int ret;

  va_start(args, fmt);
  ret = ftp_vwrite(env->active_fd, fmt, args);
  va_end(args);

  return ret;
}


/**
 * Write a string to the active connection with perror semantics.
 **/
int
ftp_perror(ftp_env_t *env) {
  char buf[255];

  if(strerror_r(errno, buf, sizeof(buf))) {
    strncpy(buf, "Unknown error", sizeof(buf));
  }

  return ftp_active_printf(env, "550 %s\r\n", buf);
}


/**
 * Create an absolute path from the current working directory. The result is
 * left empty if it does not fit in PATH_MAX bytes.
 **/
void
ftp_abspath(ftp_env_t *env, char *abspath, const char *path) {
  int len;

  if(path[0] == '/') {
    len = snprintf(abspath, PATH_MAX, "%s", path);
  } else if(!strcmp(env->cwd, "/")) {
    len = snprintf(abspath, PATH_MAX, "/%s", path);
  } else {
    len = snprintf(abspath, PATH_MAX, "%s/%s", env->cwd, path);
  }

  if(len < 0 || len >= PATH_MAX) {
    abspath[0] = '\0';
  }
}


/**
 * Collapse repeated slashes, "." and ".." in an absolute path, in place.
 **/
static void
ftp_normpath(char *path) {
  size_t out = 0;
  size_t len;
  char *comp;
  char *p = path;

  if(!path[0]) {
    return;
  }

  while(*p) {
    while(*p == '/') {
      p++;
    }

    comp = p;
    while(*p && *p != '/') {
      p++;
    }

    len = p - comp;
    if(!len || (len == 1 && comp[0] == '.')) {
      continue;
    }

    if(len == 2 && comp[0] == '.' && comp[1] == '.') {
      while(out > 0 && path[out-1] != '/') {
	out--;
      }
      if(out > 0) {
	out--;
      }
      continue;
    }

    path[out++] = '/';
    memmove(path + out, comp, len);
    out += len;
  }

  if(!out) {
    path[out++] = '/';
  }
  path[out] = '\0';
}


/**
 * Quote a path for use in a reply by doubling any double quotes in it.
 **/
static void
ftp_quote(char *quoted, size_t size, const char *path) {
  size_t pos = 0;

  for(size_t i=0; path[i] && pos+2<size; i++) {
    if(path[i] == '"') {
      quoted[pos++] = '"';
    }
    quoted[pos++] = path[i];
  }
  quoted[pos] = '\0';
}


/**
 * Enter passive mode.
 **/
int
ftp_cmd_PASV(ftp_env_t *env, const char* arg) {
  socklen_t sockaddr_len = sizeof(struct sockaddr_in);
  struct sockaddr_in sockaddr;
  uint32_t addr = 0;
  uint16_t port = 0;
  int ret = 0;

  if(getsockname(env->active_fd, (struct sockaddr*)&sockaddr, &sockaddr_len)) {
    return ftp_perror(env);
  }
  addr = sockaddr.sin_addr.s_addr;

  memset(&env->data_addr, 0, sizeof(env->data_addr));
  if(env->passive_fd >= 0) {
    close(env->passive_fd);
    env->passive_fd = -1;
  }

  if((env->passive_fd=socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    return ftp_perror(env);
  }

  memset(&sockaddr, 0, sockaddr_len);
  sockaddr.sin_family = AF_INET;
  sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  sockaddr.sin_port = htons(0);

  if(bind(env->passive_fd, (struct sockaddr*)&sockaddr, sockaddr_len) != 0) {
    ret = ftp_perror(env);
    close(env->passive_fd);
    env->passive_fd = -1;
    return ret;
  }

  if(listen(env->passive_fd, 1) != 0) {
    ret = ftp_perror(env);
    close(env->passive_fd);
    env->passive_fd = -1;
    return ret;
  }

  if(getsockname(env->passive_fd, (struct sockaddr*)&sockaddr, &sockaddr_len)) {
    ret = ftp_perror(env);
    close(env->passive_fd);
    env->passive_fd = -1;
    return ret;
  }
  port = sockaddr.sin_port;

  return ftp_active_printf(env, "227 Entering Passive Mode (%hhu,%hhu,%hhu,%hhu,%hhu,%hhu).\r\n",
			   (addr >> 0) & 0xFF,
			   (addr >> 8) & 0xFF,
			   (addr >> 16) & 0xFF,
			   (addr >> 24) & 0xFF,
			   (port >> 0) & 0xFF,
			   (port >> 8) & 0xFF);
}


/**
 * Change the working directory to its parent.
 **/
int
ftp_cmd_CDUP(ftp_env_t *env, const char* arg) {
  int pos = -1;

  for(size_t i=0; i<sizeof(env->cwd); i++) {
    if(!env->cwd[i]) {
      break;
    } else if(env->cwd[i] == '/') {
      pos = i;
    }
  }

  if(pos <= 0) {
    env->cwd[0] = '/';
    env->cwd[1] = '\0';
  } else if(pos > 0) {
    env->cwd[pos] = '\0';
  }

  return ftp_active_printf(env, "250 OK\r\n");
}


/**
 * Change the permission mode bits of a path.
 **/
int
ftp_cmd_CHMOD(ftp_env_t *env, const char* arg) {
  char pathbuf[PATH_MAX];
  mode_t mode = 0;
  const char* ptr;

  if(!arg[0] || !(ptr=strstr(arg, " "))) {
    return ftp_active_printf(env, "501 Usage: CHMOD <MODE> <PATH>\r\n");
  }

  mode = strtol(arg, 0, 8);
  ftp_abspath(env, pathbuf, ptr+1);

  if(chmod(pathbuf, mode)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "200 OK\r\n");
}


/**
 * Change the working directory.
 **/
int
ftp_cmd_CWD(ftp_env_t *env, const char* arg) {
  char pathbuf[PATH_MAX];
  struct stat st;

  if(!arg[0]) {
    return ftp_active_printf(env, "501 Usage: CWD <PATH>\r\n");
  }

  ftp_abspath(env, pathbuf, arg);
  ftp_normpath(pathbuf);
  if(stat(pathbuf, &st)) {
    return ftp_perror(env);
  }

  if(!S_ISDIR(st.st_mode)) {
    return ftp_active_printf(env, "550 No such directory\r\n");
  }

  snprintf(env->cwd, sizeof(env->cwd), "%s", pathbuf);

  return ftp_active_printf(env, "250 OK\r\n");
}


/**
 * Delete a given file.
 **/
int
ftp_cmd_DELE(ftp_env_t *env, const char* arg) {
  char pathbuf[PATH_MAX];

  if(!arg[0]) {
    return ftp_active_printf(env, "501 Usage: DELE <FILENAME>\r\n");
  }

  ftp_abspath(env, pathbuf, arg);
  if(remove(pathbuf)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "250 File deleted\r\n");
}


/**
 * List supported FTP protocol extensions.
 **/
int
ftp_cmd_FEAT(ftp_env_t *env, const char *arg) {
  return ftp_active_printf(env,
                           "211-Features:\r\n"
                           " MLSD\r\n"
                           " KILL\r\n"
                           " MTRW\r\n"
                           " SELF\r\n"
                           "211 End\r\n");
}


/**
 * Skip the options of a listing command (e.g., -la) and return the path that
 * follows them, or NULL if there is none. Set all if the options contain a.
 **/
static const char*
ftp_list_path(const char *arg, int *all) {
  if(all) {
    *all = 0;
  }

  while(arg[0] == '-') {
    for(arg++; arg[0] && arg[0] != ' '; arg++) {
      if(arg[0] == 'a' && all) {
	*all = 1;
      }
    }

    while(arg[0] == ' ') {
      arg++;
    }
  }

  return arg[0] ? arg : NULL;
}


/**
 * Join a directory and a name into a path.
 **/
static void
ftp_join(char *path, size_t size, const char *dir, const char *name) {
  snprintf(path, size, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
}


/**
 * Obtain the time of last modification, or of last change if it is unset.
 **/
static time_t
ftp_mtime(const struct stat *st) {
  return st->st_mtim.tv_sec ? st->st_mtim.tv_sec : st->st_ctim.tv_sec;
}


/**
 * Obtain the size of a file, which is the size of the embedded ELF for SELF
 * files if the client has asked for them to be converted.
 **/
static uint64_t
ftp_file_size(ftp_env_t *env, const char *path, const struct stat *st) {
  size_t size;

  if(env->self2elf && self_is_valid(path) == 1 &&
     (size=self_get_elfsize(path))) {
    return size;
  }

  return st->st_size;
}


/**
 * Transmit a line that describes a path in the style of ls -l.
 **/
static int
ftp_list_line(ftp_env_t *env, const char *path, const char *name,
	      const struct stat *st) {
  char timebuf[20];
  char modebuf[20];
  struct tm tm;
  time_t mtime = ftp_mtime(st);
  time_t now = time(NULL);

  ftp_mode_string(st->st_mode, modebuf);
  gmtime_r(&mtime, &tm);
  if(mtime > now || now - mtime > 15778800) {
    strftime(timebuf, sizeof(timebuf), "%b %d  %Y", &tm);
  } else {
    strftime(timebuf, sizeof(timebuf), "%b %d %H:%M", &tm);
  }

  return ftp_data_printf(env, "%s %llu %llu %llu %llu %s %s\r\n", modebuf,
			 (unsigned long long)st->st_nlink,
			 (unsigned long long)st->st_uid,
			 (unsigned long long)st->st_gid,
			 (unsigned long long)ftp_file_size(env, path, st),
			 timebuf, name);
}


/**
 * Transmit a machine-readable line that describes a path.
 **/
static int
ftp_mlsd_line(ftp_env_t *env, const char *path, const char *name,
	      const struct stat *st) {
  char timebuf[20];
  struct tm tm;
  time_t mtime = ftp_mtime(st);

  gmtime_r(&mtime, &tm);
  strftime(timebuf, sizeof(timebuf), "%Y%m%d%H%M%S", &tm);

  return ftp_data_printf(env, "type=%s;size=%llu;modify=%s; %s\r\n",
			 S_ISDIR(st->st_mode) ? "dir" : "file",
			 (unsigned long long)ftp_file_size(env, path, st),
			 timebuf, name);
}


/**
 * Finish the transfer of a listing.
 **/
static int
ftp_list_finish(ftp_env_t *env, DIR *dir, int failed) {
  int err;

  if(failed) {
    err = ftp_perror(env);
    ftp_data_close(env);
    if(dir) {
      closedir(dir);
    }
    return err;
  }

  if(ftp_data_close(env)) {
    err = ftp_perror(env);
    if(dir) {
      closedir(dir);
    }
    return err;
  }

  if(dir && closedir(dir)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "226 Transfer complete\r\n");
}


/**
 * Trasfer a list of files and folder.
 **/
int
ftp_cmd_LIST(ftp_env_t *env, const char* arg) {
  char pathbuf[PATH_MAX];
  char entbuf[PATH_MAX*2];
  struct dirent *ent;
  struct stat statbuf;
  const char *p;
  DIR *dir = NULL;
  int failed = 0;
  int err;

  p = ftp_list_path(arg, NULL);
  ftp_abspath(env, pathbuf, p ? p : env->cwd);
  if(stat(pathbuf, &statbuf)) {
    return ftp_perror(env);
  }

  if(S_ISDIR(statbuf.st_mode) && !(dir=opendir(pathbuf))) {
    return ftp_perror(env);
  }

  if(ftp_data_open(env)) {
    err = ftp_perror(env);
    if(dir) {
      closedir(dir);
    }
    return err;
  }

  ftp_active_printf(env, "150 Opening data transfer\r\n");

  if(!dir) {
    failed = ftp_list_line(env, pathbuf, p, &statbuf);
  }

  while(!failed && dir && (ent=readdir(dir))) {
    ftp_join(entbuf, sizeof(entbuf), pathbuf, ent->d_name);
    if(stat(entbuf, &statbuf) != 0) {
      continue;
    }

    failed = ftp_list_line(env, entbuf, ent->d_name, &statbuf);
  }

  return ftp_list_finish(env, dir, failed);
}


/**
 * Create a new directory at a given path.
 **/
int
ftp_cmd_MKD(ftp_env_t *env, const char* arg) {
  char pathbuf[PATH_MAX];
  char quoted[PATH_MAX*2];

  if(!arg[0]) {
    return ftp_active_printf(env, "501 Usage: MKD <DIRNAME>\r\n");
  }

  ftp_abspath(env, pathbuf, arg);
  if(mkdir(pathbuf, 0777)) {
    return ftp_perror(env);
  }

  ftp_normpath(pathbuf);
  ftp_quote(quoted, sizeof(quoted), pathbuf);

  return ftp_active_printf(env, "257 \"%s\" directory created\r\n", quoted);
}


/**
 * Transfer a machine-readable directory listing.
 **/
int
ftp_cmd_MLSD(ftp_env_t *env, const char *arg) {
  char pathbuf[PATH_MAX];
  char entbuf[PATH_MAX*2];
  struct stat statbuf;
  struct dirent *ent;
  const char *p;
  int failed = 0;
  DIR *dir;
  int err;

  p = ftp_list_path(arg, NULL);
  ftp_abspath(env, pathbuf, p ? p : env->cwd);
  if(!(dir=opendir(pathbuf))) {
    return ftp_perror(env);
  }

  if(ftp_data_open(env)) {
    err = ftp_perror(env);
    closedir(dir);
    return err;
  }

  ftp_active_printf(env, "150 Opening data transfer\r\n");

  while(!failed && (ent=readdir(dir))) {
    ftp_join(entbuf, sizeof(entbuf), pathbuf, ent->d_name);
    if(stat(entbuf, &statbuf) != 0) {
      continue;
    }

    failed = ftp_mlsd_line(env, entbuf, ent->d_name, &statbuf);
  }

  return ftp_list_finish(env, dir, failed);
}


/**
 * No operation.
 **/
int
ftp_cmd_NOOP(ftp_env_t *env, const char* arg) {
  return ftp_active_printf(env, "200 NOOP OK\r\n");
}


/**
 * Establish a data connection with client.
 **/
int
ftp_cmd_PORT(ftp_env_t *env, const char* arg) {
  struct sockaddr_in peer_addr;
  socklen_t peer_len = sizeof(peer_addr);
  uint8_t addr[6];
  uint32_t s_addr;

  if(sscanf(arg, "%hhu,%hhu,%hhu,%hhu,%hhu,%hhu",
	    addr, addr+1, addr+2, addr+3, addr+4, addr+5) != 6) {
    return ftp_active_printf(env, "501 Usage: PORT <addr>\r\n");
  }

  if(getpeername(env->active_fd, (struct sockaddr*)&peer_addr, &peer_len)) {
    return ftp_perror(env);
  }

  memcpy(&s_addr, addr, sizeof(s_addr));
  if(s_addr != peer_addr.sin_addr.s_addr) {
    return ftp_active_printf(env, "504 Illegal PORT command\r\n");
  }

  if(env->data_fd != -1) {
    close(env->data_fd);
    env->data_fd = -1;
  }
  if(env->passive_fd >= 0) {
    close(env->passive_fd);
    env->passive_fd = -1;
  }

  memset(&env->data_addr, 0, sizeof(env->data_addr));
  env->data_addr.sin_family = AF_INET;
  env->data_addr.sin_addr.s_addr = s_addr;
  env->data_addr.sin_port = htons((addr[4] << 8) | addr[5]);

  return ftp_active_printf(env, "200 PORT command successful.\r\n");
}


/**
 * Print working directory.
 **/
int
ftp_cmd_PWD(ftp_env_t *env, const char* arg) {
  char quoted[PATH_MAX*2];

  ftp_quote(quoted, sizeof(quoted), env->cwd);

  return ftp_active_printf(env, "257 \"%s\"\r\n", quoted);
}


/**
 * Disconnect client.
 **/
int
ftp_cmd_QUIT(ftp_env_t *env, const char* arg) {
  ftp_active_printf(env, "221 Goodbye\r\n");
  return -1;
}


/**
 * Mark the offset to start from in a future file transer.
 **/
int
ftp_cmd_REST(ftp_env_t *env, const char* arg) {
  long long off;
  char *end;

  if(!arg[0]) {
    return ftp_active_printf(env, "501 Usage: REST <OFFSET>\r\n");
  }

  off = strtoll(arg, &end, 10);
  if(end == arg || end[0] || off < 0) {
    return ftp_active_printf(env, "501 Invalid offset\r\n");
  }

  env->data_offset = off;

  return ftp_active_printf(env, "350 REST OK\r\n");
}


/**
 * Retreive data from a given file.
 **/
static int
ftp_cmd_RETR_fd(ftp_env_t *env, int fd, off_t off) {
  struct stat st;
  int err = 0;

  if(fstat(fd, &st)) {
    return ftp_perror(env);
  }
  if(S_ISDIR(st.st_mode)) {
    errno = EISDIR;
    return ftp_perror(env);
  }
  if(lseek(fd, off, SEEK_SET) < 0) {
    return ftp_perror(env);
  }

  if(ftp_active_printf(env, "150 Starting data transfer\r\n")) {
    return -1;
  }

  if(ftp_data_open(env)) {
    return ftp_perror(env);
  }

  if(io_ncopy(fd, env->data_fd, off < st.st_size ? st.st_size - off : 0)) {
    err = ftp_perror(env);
    ftp_data_close(env);
    return err;
  }

  if(ftp_data_close(env)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "226 Transfer completed\r\n");
}


/**
 * Retreive an ELF file embedded within a SELF file.
 **/
static int
ftp_cmd_RETR_self2elf(ftp_env_t *env, int fd, off_t off) {
  FILE* tmpf;
  int err;

  if(!(tmpf=tmpfile())) {
    return ftp_perror(env);
  }

  if(ftp_active_printf(env, "150-Extracting ELF...\r\n")) {
    fclose(tmpf);
    return -1;
  }
  if(self_extract_elf(fd, fileno(tmpf))) {
    if(errno != EBADMSG) {
      err = ftp_perror(env);
      fclose(tmpf);
      return err;
    }
    if(ftp_active_printf(env, "150-Warning: ELF digest mismatch\r\n")) {
      fclose(tmpf);
      return -1;
    }
  }

  rewind(tmpf);
  err = ftp_cmd_RETR_fd(env, fileno(tmpf), off);
  fclose(tmpf);

  return err;
}


/**
 * Retreive data from a given file.
 **/
int
ftp_cmd_RETR(ftp_env_t *env, const char* arg) {
  off_t off = env->data_offset;
  char path[PATH_MAX];
  int err;
  int fd;

  env->data_offset = 0;

  if(!arg[0]) {
    return ftp_active_printf(env, "501 Usage: RETR <PATH>\r\n");
  }

  ftp_abspath(env, path, arg);
  if((fd=open(path, O_RDONLY, 0)) < 0) {
    return ftp_perror(env);
  }

  if(env->self2elf && self_is_valid(path) == 1) {
    err = ftp_cmd_RETR_self2elf(env, fd, off);
  } else {
    err = ftp_cmd_RETR_fd(env, fd, off);
  }

  close(fd);
  return err;
}


/**
 * Remove a directory.
 **/
int
ftp_cmd_RMD(ftp_env_t *env, const char* arg) {
  char pathbuf[PATH_MAX];

  if(!arg[0]) {
    return ftp_active_printf(env, "501 Usage: RMD <DIRNAME>\r\n");
  }

  ftp_abspath(env, pathbuf, arg);
  if(rmdir(pathbuf)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "250 Directory deleted\r\n");
}


/**
 * Specify a path that will later be renamed by the RNTO command.
 **/
int
ftp_cmd_RNFR(ftp_env_t *env, const char* arg) {
  struct stat st;

  if(!arg[0]) {
    return ftp_active_printf(env, "501 Usage: RNFR <PATH>\r\n");
  }

  ftp_abspath(env, env->rename_path, arg);
  if(stat(env->rename_path, &st)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "350 Awaiting new name\r\n");
}


/**
 * Rename a path previously specified by the RNFR command.
 **/
int
ftp_cmd_RNTO(ftp_env_t *env, const char* arg) {
  char pathbuf[PATH_MAX];
  struct stat st;

  if(!arg[0]) {
    return ftp_active_printf(env, "501 Usage: RNTO <PATH>\r\n");
  }

  if(stat(env->rename_path, &st)) {
    return ftp_perror(env);
  }

  ftp_abspath(env, pathbuf, arg);
  if(rename(env->rename_path, pathbuf)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "250 Path renamed\r\n");
}


/**
 * Obtain the size of a given file.
 **/
int
ftp_cmd_SIZE(ftp_env_t *env, const char* arg) {
  char pathbuf[PATH_MAX];
  struct stat st = {0};
  size_t size;

  if(!arg[0]) {
    return ftp_active_printf(env, "501 Usage: SIZE <FILENAME>\r\n");
  }

  ftp_abspath(env, pathbuf, arg);
  if(stat(pathbuf, &st)) {
    return ftp_active_printf(env, "550 No such file or directory\r\n");
  }

  if(env->self2elf) {
    if((size=self_get_elfsize(pathbuf))) {
      st.st_size = size;
    }
  }

  return ftp_active_printf(env, "213 %"  PRIu64 "\r\n", st.st_size);
}


/**
 * Store recieved data in a given file.
 **/
int
ftp_cmd_STOR(ftp_env_t *env, const char* arg) {
  off_t off = env->data_offset;
  char pathbuf[PATH_MAX];
  int err = 0;
  ssize_t len;
  int fd;

  env->data_offset = 0;

  if(!arg[0]) {
    return ftp_active_printf(env, "501 Usage: STOR <FILENAME>\r\n");
  }

  if(env->type == 'A') {
    return ftp_active_printf(env, "504 STOR in ASCII mode is not supported\r\n");
  }

  ftp_abspath(env, pathbuf, arg);
  if((fd=open(pathbuf, O_CREAT | O_WRONLY, 0777)) < 0) {
    return ftp_perror(env);
  }

  if(lseek(fd, off, SEEK_CUR) < 0) {
    err = ftp_perror(env);
    close(fd);
    return err;
  }

  if(ftp_active_printf(env, "150 Opening data transfer\r\n")) {
    close(fd);
    return -1;
  }

  if(ftp_data_open(env)) {
    err = ftp_perror(env);
    close(fd);
    return err;
  }

  while((len=ftp_data_read(env, env->readbuf, env->readbuf_size)) > 0) {
    if(io_nwrite(fd, env->readbuf, len)) {
      err = ftp_perror(env);
      ftp_data_close(env);
      close(fd);
      return err;
    }
    off += len;
  }

  if(len < 0) {
    err = ftp_perror(env);
    ftp_data_close(env);
    close(fd);
    return err;
  }

  if(ftruncate(fd, off)) {
    err = ftp_perror(env);
    ftp_data_close(env);
    close(fd);
    return err;
  }

  close(fd);
  if(ftp_data_close(env)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "226 Data transfer complete\r\n");
}


/**
 * Append to an existing file.
 **/
int
ftp_cmd_APPE(ftp_env_t *env, const char* arg) {
  char pathbuf[PATH_MAX];
  struct stat statbuf;

  if(!arg[0]) {
    return ftp_active_printf(env, "501 Usage: APPE <FILENAME>\r\n");
  }

  if(!env->data_offset) {
    ftp_abspath(env, pathbuf, arg);
    if(!stat(pathbuf, &statbuf)) {
      env->data_offset = statbuf.st_size;
    } else if(errno != ENOENT) {
      return ftp_perror(env);
    }
  }

  return ftp_cmd_STOR(env, arg);
}


/**
 * Return system type.
 **/
int
ftp_cmd_SYST(ftp_env_t *env, const char* arg) {
  return ftp_active_printf(env, "215 UNIX Type: L8\r\n");
}


/**
 * Sets the transfer mode (ASCII or Binary).
 **/
int
ftp_cmd_TYPE(ftp_env_t *env, const char* arg) {
  int type = toupper((unsigned char)arg[0]);

  switch(type) {
  case 'A':
  case 'I':
    env->type = type;
    return ftp_active_printf(env, "200 Type set to %c\r\n", env->type);
  default:
    return ftp_active_printf(env, "501 Invalid argument to TYPE\r\n");
  }
}


/**
 * Authenticate user.
 **/
int
ftp_cmd_USER(ftp_env_t *env, const char* arg) {
  return ftp_active_printf(env, "230 User logged in\r\n");
}


/**
 * Custom command that terminates the server.
 **/
int
ftp_cmd_KILL(ftp_env_t *env, const char* arg) {
  FTP_LOG_PUTS("Server killed");
  exit(EXIT_SUCCESS);
  return -1;
}



/**
 * Custom command to toggle SELF transfer mode.
 **/
int
ftp_cmd_SELF(ftp_env_t *env, const char* arg) {
  env->self2elf = !env->self2elf;

  if(env->self2elf) {
    return ftp_active_printf(env, "226 SELF transfer mode enabled\r\n");
  } else {
    return ftp_active_printf(env, "226 SELF transfer mode disabled\r\n");
  }
}


/**
 * Unsupported command.
 **/
int
ftp_cmd_unavailable(ftp_env_t *env, const char* arg) {
  return ftp_active_printf(env, "502 Command not implemented\r\n");
}


/**
 * Unknown command.
 **/
int
ftp_cmd_unknown(ftp_env_t *env, const char* arg) {
  return ftp_active_printf(env, "502 Command not recognized\r\n");
}


/*
  Local Variables:
  c-file-style: "gnu"
  End:
*/
