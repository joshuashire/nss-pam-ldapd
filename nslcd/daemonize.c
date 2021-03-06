/*
   daemoninze.c - functions for properly daemonising an application

   Copyright (C) 2014 Arthur de Jong

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301 USA
*/

#include "config.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_PTHREAD_H
#include <pthread.h>
#endif /* HAVE_PTHREAD_H */

#include "daemonize.h"

/* the write end of a pipe that is used to signal the fact that the child
   process has finished initialising (see daemonize_daemon() and
   daemonize_ready() for details) */
static int daemonizefd = -1;

void daemonize_closefds(void)
{
  int i;
  /* close all file descriptors (except stdin/out/err) */
  i = sysconf(_SC_OPEN_MAX) - 1;
  /* if the system does not have OPEN_MAX just close the first 32 and
     hope we closed enough */
  if (i < 0)
    i = 32;
  for (; i > 3; i--)
    close(i);
}

void daemonize_redirect_stdio(void)
{
  /* close stdin, stdout and stderr */
  close(0);   /* stdin */
  close(1);   /* stdout */
  close(2);   /* stderr */
  /* reconnect to /dev/null */
  open("/dev/null", O_RDWR);  /* stdin, fd=0 */
  dup(0);     /* stdout, fd=1 */
  dup(0);     /* stderr, fd=2 */
}

/* try to fill the buffer until EOF or error */
static int read_response(int fd, char *buffer, size_t bufsz)
{
  int rc;
  size_t r = 0;
  while (r < bufsz)
  {
    rc = read(fd, buffer + r, bufsz - r);
    if (rc == 0)
      break;
    else if (rc > 0)
      r += rc;
    else if ((errno == EINTR) || (errno == EAGAIN))
      continue; /* ignore these errors and try again */
    else
      return -1;
  }
  return r;
}

/* ihe process calling daemonize_daemon() will end up here on success */
static int wait_for_response(int fd)
{
  int i, l, rc;
  char buffer[1024];
  /* read return code */
  i = read_response(fd, (void *)&rc, sizeof(int));
  if (i != sizeof(int))
    return -1;
  /* read string length */
  i = read_response(fd, (void *)&l, sizeof(int));
  if ((i != sizeof(int)) || (l <= 0))
    _exit(rc);
  /* read string */
  if ((size_t)l > (sizeof(buffer) - 1))
    l = sizeof(buffer) - 1;
  i = read_response(fd, buffer, l);
  buffer[sizeof(buffer) - 1] = '\0';
  if (i == l)
    fprintf(stderr, "%s", buffer);
  _exit(rc);
}

static void closefd(void)
{
  if (daemonizefd >= 0)
  {
    close(daemonizefd);
    daemonizefd = -1;
  }
}

int daemonize_daemon(void)
{
  int pipefds[2];
  int i;
  /* set up a pipe for communication */
  if (pipe(pipefds) < 0)
    return -1;
  /* set O_NONBLOCK on the write end to ensure that a call to
     daemonize_ready() will not lock the application */
  if ((i = fcntl(pipefds[1], F_GETFL, 0)) < 0)
  {
    close(pipefds[0]);
    close(pipefds[1]);
    return -1;
  }
  if (fcntl(pipefds[1], F_SETFL, i | O_NONBLOCK) < 0)
  {
    close(pipefds[0]);
    close(pipefds[1]);
    return -1;
  }
  /* fork() and exit() to detach from the parent process */
  switch (fork())
  {
    case 0:
      /* we are the child, close read end of pipe */
      close(pipefds[0]);
      break;
    case -1:
      /* we are the parent, but have an error */
      close(pipefds[0]);
      close(pipefds[1]);
      return -1;
    default:
      /* we are the parent, close write end and wait for information */
      close(pipefds[1]);
      return wait_for_response(pipefds[0]);
  }
  /* become process leader */
  if (setsid() < 0)
  {
    close(pipefds[1]);
    _exit(EXIT_FAILURE);
  }
  /* fork again so we cannot allocate a pty */
  switch (fork())
  {
    case 0:
      /* we are the child */
      break;
    case -1:
      /* we are the parent, but have an error */
      close(pipefds[1]);
      _exit(EXIT_FAILURE);
    default:
      /* we are the parent and we're done */
      close(pipefds[1]);
      _exit(EXIT_SUCCESS);
  }
  daemonizefd = pipefds[1];
  /* close the file descriptor on exec (ignore errors) */
  fcntl(daemonizefd, F_SETFD, FD_CLOEXEC);
#ifdef HAVE_PTHREAD_ATFORK
  /* handle any other forks by closing daemonizefd first */
  (void)pthread_atfork(NULL, NULL, closefd);
#endif /* HAVE_PTHREAD_ATFORK */
  return 0;
}

void daemonize_ready(int status, const char *message)
{
  if (daemonizefd >= 0)
  {
    /* we ignore any errors writing */
    write(daemonizefd, &status, sizeof(int));
    if ((message == NULL) || (message[0] == '\0'))
    {
      status = 0;
      write(daemonizefd, &status, sizeof(int));
    }
    else
    {
      status = strlen(message);
      write(daemonizefd, &status, sizeof(int));
      write(daemonizefd, message, status);
    }
    close(daemonizefd);
    daemonizefd = -1;
  }
}
