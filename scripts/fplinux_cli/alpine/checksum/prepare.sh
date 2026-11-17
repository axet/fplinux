#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

stage_root=$1
downloads=$2
package=$3
checksum_epoch=$4
source_cache=$downloads/alpine/sources
stage_aport=$stage_root/alpine/aports/$package

chmod 755 "$stage_root" "$stage_root/alpine" "$stage_root/alpine/aports"
install -d "$source_cache"

# Kern exposes host directories through virtiofs, which cannot change inode
# ownership from inside the box.  Run the checksum step as the sandbox root
# instead of dropping to builder; abuild's -F flag permits that explicitly.
export SRCDEST=$source_cache
export SOURCE_DATE_EPOCH=$checksum_epoch
cd "$stage_aport"
abuild -F checksum
apkbuild-lint APKBUILD
