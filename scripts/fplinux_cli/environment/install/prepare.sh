#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

environment_lock=$1
environment_inputs=$2
environment_resources=$3
environment_recipe=$4
environment_generation=$5
shift 5

printf '%.32s\n' "$environment_recipe" >/etc/machine-id

# The base rootfs has no Python interpreter; the complete local APK closure supplies it.
# An unprivileged box with a mapped UID range cannot preserve the ownership of a
# few setuid files.  APK reports those as warnings but exits 2; accept only that
# exact case so genuine installation errors still abort the environment build.
set +e
apk_output=$(apk add --no-network --no-cache "$@" 2>&1)
apk_status=$?
set -e
printf '%s\n' "$apk_output"
if [ "$apk_status" -ne 0 ]; then
	warnings=$(printf '%s\n' "$apk_output" | grep -c '^WARNING:' || true)
	if [ "$apk_status" -eq 2 ] && [ "$warnings" -gt 0 ] &&
		! printf '%s\n' "$apk_output" | grep -qE '(^|[[:space:]])(ERROR|error):'; then
		printf 'ignoring non-fatal APK ownership warnings\n' >&2
	else
		exit "$apk_status"
	fi
fi

exec python3 -B "$environment_resources/scripts/fplinux_cli/environment/install.py" \
	--lock "$environment_lock" \
	--inputs "$environment_inputs" \
	--resources "$environment_resources" \
	--recipe "$environment_recipe" \
	--generation "$environment_generation"
