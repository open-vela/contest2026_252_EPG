/****************************************************************************
 * board/contest_board/src/fdtest.c
 *
 * How many files can one task hold open at once, and which layer runs out
 * first.
 *
 * Written to settle a specific question: the NIST statistical suite (xTS case
 * 1.3.16) wants a stats.txt and a results.txt open simultaneously for each of
 * its 15 tests, and it dies on the 11th with "MAX # OF OPENED FILES HAS BEEN
 * REACHED".  That message also offers "-OR- THE OUTPUT DIRECTORY DOES NOT
 * EXIST", which is misleading -- the directories are there.  Raising
 * CONFIG_NFILE_DESCRIPTORS_PER_BLOCK from 8 to 64 did not move the number,
 * which is the hint that descriptors are not what runs out.
 *
 * So: count raw open() separately from fopen().  Descriptors and FILE streams
 * are different pools, and only one of them can be the wall.
 *
 *   fdtest [-n <count>] [-d <dir>]
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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FDTEST_MAX 128

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int try_open(FAR const char *dir, int want)
{
  static int fds[FDTEST_MAX];
  char path[PATH_MAX];
  int opened = 0;
  int saved = 0;
  int i;

  for (i = 0; i < want; i++)
    {
      snprintf(path, sizeof(path), "%s/o%03d.tmp", dir, i);
      fds[i] = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
      if (fds[i] < 0)
        {
          saved = errno;
          break;
        }

      opened++;
    }

  printf("  open()   held %3d simultaneously", opened);
  if (opened < want)
    {
      printf(", then failed with errno %d (%s)\n", saved, strerror(saved));
    }
  else
    {
      printf(" (asked for %d, no failure)\n", want);
    }

  for (i = 0; i < opened; i++)
    {
      close(fds[i]);
    }

  return opened;
}

static int try_fopen(FAR const char *dir, int want)
{
  static FAR FILE *fps[FDTEST_MAX];
  char path[PATH_MAX];
  int opened = 0;
  int saved = 0;
  int i;

  for (i = 0; i < want; i++)
    {
      snprintf(path, sizeof(path), "%s/f%03d.tmp", dir, i);
      fps[i] = fopen(path, "w");
      if (fps[i] == NULL)
        {
          saved = errno;
          break;
        }

      opened++;
    }

  printf("  fopen()  held %3d simultaneously", opened);
  if (opened < want)
    {
      printf(", then failed with errno %d (%s)\n", saved, strerror(saved));
    }
  else
    {
      printf(" (asked for %d, no failure)\n", want);
    }

  for (i = 0; i < opened; i++)
    {
      fclose(fps[i]);
    }

  return opened;
}

static void cleanup(FAR const char *dir, int n)
{
  char path[PATH_MAX];
  int i;

  for (i = 0; i < n; i++)
    {
      snprintf(path, sizeof(path), "%s/o%03d.tmp", dir, i);
      unlink(path);
      snprintf(path, sizeof(path), "%s/f%03d.tmp", dir, i);
      unlink(path);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *dir = "/tmp";
  int want = 40;
  int nopen;
  int nfopen;
  int i;

  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
        {
          want = atoi(argv[++i]);
        }
      else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
        {
          dir = argv[++i];
        }
      else
        {
          printf("Usage: fdtest [-n count] [-d dir]\n");
          return ERROR;
        }
    }

  if (want < 1 || want > FDTEST_MAX)
    {
      printf("fdtest: count must be 1..%d\n", FDTEST_MAX);
      return ERROR;
    }

  printf("fdtest: %s, up to %d files\n", dir, want);

  nopen = try_open(dir, want);
  nfopen = try_fopen(dir, want);

  cleanup(dir, want);

  /* The verdict the NIST case needs. */

  if (nfopen < nopen)
    {
      printf("  -> FILE streams run out first (%d vs %d descriptors):"
             " a stdio limit, not a descriptor limit\n", nfopen, nopen);
    }
  else if (nopen < want)
    {
      printf("  -> descriptors run out first (%d): a descriptor limit\n",
             nopen);
    }
  else
    {
      printf("  -> neither ran out at %d\n", want);
    }

  return OK;
}
