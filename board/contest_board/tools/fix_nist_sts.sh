#!/usr/bin/env bash
#
# Make apps/testing/drivers/nist-sts buildable, for xTS case 1.3.16 (RNG).
#
# The problem
# -----------
# That package does not ship the NIST Statistical Test Suite; it downloads
# sts-2_1_2.zip from csrc.nist.gov at configure time.  Two things then do not
# line up, and between them no source file is ever found -- the build still
# "succeeds" and only the final link fails, on an unresolved nist_sts_main
# reached from the builtin table:
#
#   1. The archive extracts as "sts-2.1.2", while the package's own
#      CMakeLists globs ${NIST_DIR}/sts/src/*.c.
#   2. Its PATCH_COMMAND runs "patch -p0 -d <dir>/nist-sts" against patches
#      whose paths start with "nist-sts/sts/", so the -d is one level too
#      deep and both patches are rejected (they land an Oops.rej instead).
#
# The fix
# -------
# Rename the extracted tree to what the glob expects and apply the two
# patches with a strip level that matches their paths.  Everything happens
# inside apps/testing/drivers/nist-sts/nist-sts, which that package's own
# .gitignore excludes -- so this changes no tracked file in any public
# repository, which is this project's hard constraint.
#
# It does not survive a fresh checkout: the download directory is recreated
# empty and the same mismatch returns.  Re-run this afterwards.  The real fix
# belongs upstream in apps/; this is a local unblock.
#
# Usage:  tools/fix_nist_sts.sh [<workspace root>]
#         Defaults to walking up from this script to find apps/.
#
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="${1:-}"

if [ -z "$root" ]; then
  # board/contest_board/tools -> ... -> workspace root holding apps/
  root="$here"
  while [ "$root" != "/" ] && [ ! -d "$root/apps/testing/drivers/nist-sts" ]; do
    root="$(dirname "$root")"
  done
fi

pkg="$root/apps/testing/drivers/nist-sts"
dl="$pkg/nist-sts"

if [ ! -d "$pkg" ]; then
  echo "fix_nist_sts: cannot find apps/testing/drivers/nist-sts under $root" >&2
  exit 1
fi

if [ ! -d "$dl" ]; then
  echo "fix_nist_sts: $dl does not exist yet."
  echo "  Configure the build once with CONFIG_TESTING_NIST_STS=y so CMake"
  echo "  downloads the archive, then re-run this script."
  exit 1
fi

# Step 1: rename + patch.  Skipped once it has been done; step 2 below has
# its own guard and always gets a chance to run.

if [ -d "$dl/sts/src" ]; then
  echo "fix_nist_sts: sources already in place ($dl/sts/src)"
else

extracted=""
for candidate in "$dl"/sts-*; do
  if [ -d "$candidate/src" ]; then
    extracted="$candidate"
    break
  fi
done

if [ -z "$extracted" ]; then
  echo "fix_nist_sts: no extracted sts-*/src under $dl" >&2
  echo "  Contents:" >&2
  ls -la "$dl" >&2
  exit 1
fi

echo "fix_nist_sts: $(basename "$extracted") -> sts"
mv "$extracted" "$dl/sts"
rm -f "$dl/Oops.rej"

# The patch paths start at "nist-sts/sts/", so from inside $dl/sts two
# leading components have to come off.
for p in "$pkg"/0001-*.patch "$pkg"/0002-*.patch; do
  [ -e "$p" ] || continue
  printf '  applying %-52s ' "$(basename "$p")"
  if patch -p2 -d "$dl/sts" -s < "$p"; then
    echo "ok"
  else
    echo "FAILED"
    exit 1
  fi
done

fi   # end of step 1

# partitionResultFile() splits results.txt into one dataN.txt per sub-result
# -- 148 of them for NonOverlappingTemplate, 18 for RandomExcursionsVariant,
# 8 for RandomExcursions.  The author already avoided holding them all open:
# the function works in batches, opening a batch, writing it, closing it.
# But the batch size is hard-coded 20, and NuttX allows 16 FILE streams per
# task (_POSIX_STREAM_MAX, nuttx/include/limits.h, no Kconfig).  At that
# point stdin/stdout/stderr, the summary and results.txt are already open, so
# only 11 remain -- the 12th fopen fails and the suite dies on "data12.txt --
# file not found", which reads like a missing file and is not.
#
# Eight keeps a margin.  The batching logic is four literals in one function.

assess="$dl/sts/src/assess.c"
if grep -q 'numOfFiles/20;' "$assess"; then
  printf '  shrinking partitionResultFile batch 20 -> 8 %s' ''
  sed -i.bak \
    -e 's|m = numOfFiles/20;|m = numOfFiles/8;|' \
    -e 's|if ( (numOfFiles%20) != 0 )|if ( (numOfFiles%8) != 0 )|' \
    -e 's|start = k\*20;|start = k*8;|' \
    -e 's|end   = k\*20+19;|end   = k*8+7;|' \
    "$assess"
  rm -f "$assess.bak"
  if grep -q 'numOfFiles/8;' "$assess"; then echo "ok"; else echo "FAILED"; exit 1; fi
else
  echo "  partitionResultFile batch already shrunk"
fi

echo "fix_nist_sts: done -- $(ls "$dl"/sts/src/*.c | wc -l | tr -d ' ') sources in place"
