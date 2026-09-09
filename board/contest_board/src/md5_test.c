/****************************************************************************
 * board/contest_board/src/md5_test.c
 *
 * Repeated-MD5 checker for xTS general self-test 1.1.12 ("Kernel-md5").
 *
 * The test digests a file out of the /etc ROMFS N times and requires every
 * digest to agree.  What it actually exercises is the read path: the ROMFS
 * mount, the block-device layer under it, and the file-descriptor plumbing.
 * A digest that drifts between iterations means one of those returned short
 * or stale data, which a single-shot checksum would never catch.
 *
 * The xTS document drives this through a "md5_test" builtin that upstream
 * NuttX does not ship (its CONFIG_TESTS_TESTCASES / CONFIG_FS_TEST symbols
 * do not exist in this tree), so the board provides it -- the same thing
 * vendor/sifli does for the SF32LB52.
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

#include <crypto/md5.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MD5_TEST_BUF_SIZE   512
#define MD5_DIGEST_HEX_LEN  32

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void md5_to_hex(FAR const uint8_t digest[16], FAR char hex[33])
{
  int i;

  for (i = 0; i < 16; i++)
    {
      snprintf(&hex[i * 2], 3, "%02x", digest[i]);
    }

  hex[MD5_DIGEST_HEX_LEN] = '\0';
}

static int md5_file(FAR const char *path, FAR uint8_t digest[16])
{
  MD5_CTX ctx;
  uint8_t buf[MD5_TEST_BUF_SIZE];
  ssize_t nread;
  int fd;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      printf("md5_test: open %s failed: %d\n", path, errno);
      return -errno;
    }

  md5init(&ctx);

  for (; ; )
    {
      nread = read(fd, buf, sizeof(buf));
      if (nread == 0)
        {
          break;
        }

      if (nread < 0)
        {
          int errcode = errno;
          close(fd);
          printf("md5_test: read %s failed: %d\n", path, errcode);
          return -errcode;
        }

      md5update(&ctx, buf, (size_t)nread);
    }

  close(fd);
  md5final(digest, &ctx);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *path = "/etc/1.txt";
  int count = 1;
  uint8_t digest[16];
  char hex[33];
  char first_hex[33] =
  {
    0
  };

  int i;

  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
        {
          path = argv[++i];
        }
      else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
        {
          count = atoi(argv[++i]);
        }
      else
        {
          printf("Usage: md5_test [-f file] [-c count]\n");
          return ERROR;
        }
    }

  if (count <= 0)
    {
      printf("md5_test: invalid count %d\n", count);
      return ERROR;
    }

  for (i = 0; i < count; i++)
    {
      if (md5_file(path, digest) < 0)
        {
          return ERROR;
        }

      md5_to_hex(digest, hex);
      printf("%s\n", hex);

      if (i == 0)
        {
          strlcpy(first_hex, hex, sizeof(first_hex));
        }
      else if (strcmp(first_hex, hex) != 0)
        {
          printf("md5_test: mismatch at iteration %d: %s != %s\n",
                 i + 1, hex, first_hex);
          return ERROR;
        }
    }

  printf("md5_test: done count=%d\n", count);
  return OK;
}
