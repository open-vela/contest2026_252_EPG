/****************************************************************************
 * board/contest_board/src/sigtest.c
 *
 * Console SIGINT diagnostic -- what actually gates Ctrl-C.
 *
 * The chain lives entirely inside the serial driver: uart_recvchars() calls
 * uart_check_special() on every received chunk, which returns SIGINT when a
 * byte matches CONFIG_TTY_SIGINT_CHAR, and uart_recvchars() then does
 * nxsig_tgkill(-1, dev->pid, signo).  Two gates decide whether any of that
 * happens, and neither can be settled by reading the sources:
 *
 *   dev->tc_lflag & ISIG   set by uart_register(), but only for a device
 *                          whose isconsole flag is true at that moment
 *   dev->pid > 0           set by the TIOCSCTTY ioctl
 *
 * Both are reachable from userspace if you ask the right question.
 * tcgetattr() reports c_lflag directly.  TIOCSCTTY's return value reports
 * the pid slot: it fails with EINVAL when one is already registered, so a
 * failure here is the healthy answer -- it means somebody (NSH) claimed the
 * tty for this task, and a success would mean nobody had.
 *
 * This is what found the original fault: c_lflag read back as 0, with ISIG,
 * ECHO and ICANON all clear.  Those three are set by one statement in
 * uart_register(), so all three being clear meant the statement never ran --
 * this port never calls arm_earlyserialinit(), where isconsole was being set.
 * See arm_serialinit() in bk7258_serial.c for the one-line fix, and
 * PORTING_NOTES for the full account.
 *
 * Kept in the tree because the next console signal problem should cost
 * minutes rather than an afternoon of reading.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *path = "/dev/console";
  struct termios term;
  int seconds = 20;
  int claim = 1;
  int fd;
  int ret;
  int i;

  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
        {
          path = argv[++i];
        }
      else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
        {
          seconds = atoi(argv[++i]);
        }
      else if (strcmp(argv[i], "-n") == 0)
        {
          claim = 0;      /* skip TIOCSCTTY, rely on whoever holds it */
        }
      else
        {
          printf("Usage: sigtest [-d dev] [-t seconds] [-n]\n");
          return ERROR;
        }
    }

  fd = open(path, O_RDWR);
  if (fd < 0)
    {
      printf("sigtest: open %s failed: %d\n", path, errno);
      return ERROR;
    }

  printf("sigtest: %s, pid %d\n", path, (int)getpid());

  ret = tcgetattr(fd, &term);
  if (ret < 0)
    {
      printf("  tcgetattr failed: %d (%s)\n", errno, strerror(errno));
    }
  else
    {
      printf("  c_lflag = 0x%08lx   ISIG=%s ICANON=%s ECHO=%s\n",
             (unsigned long)term.c_lflag,
             (term.c_lflag & ISIG)   ? "yes" : "NO",
             (term.c_lflag & ICANON) ? "yes" : "no",
             (term.c_lflag & ECHO)   ? "yes" : "no");
    }

  if (claim)
    {
      ret = ioctl(fd, TIOCSCTTY, getpid());
      if (ret < 0)
        {
          printf("  TIOCSCTTY -> %d, errno %d (%s)"
                 "  [EINVAL here means a pid is already registered]\n",
                 ret, errno, strerror(errno));
        }
      else
        {
          printf("  TIOCSCTTY -> ok, this task now owns the tty\n");
        }
    }
  else
    {
      printf("  TIOCSCTTY skipped (-n)\n");
    }

  printf("  waiting %d s -- press Ctrl-C now\n", seconds);
  fflush(stdout);

  for (i = 0; i < seconds; i++)
    {
      sleep(1);
      printf(".");
      fflush(stdout);
    }

  printf("\n  survived %d s: no SIGINT was delivered\n", seconds);

  if (claim)
    {
      ioctl(fd, TIOCNOTTY, 0);
    }

  close(fd);
  return OK;
}
