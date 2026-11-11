#!/usr/bin/env bash
# Installs apt packages on a CI runner, retrying: the Ubuntu mirrors fail to resolve often
# enough that a single attempt makes the whole pipeline flaky for reasons unrelated to the code.
set -uo pipefail

for attempt in 1 2 3 4 5; do
    if sudo apt-get update && sudo apt-get install -y "$@"; then
        exit 0
    fi
    echo "apt attempt $attempt failed; retrying in $((attempt * 10)) s" >&2
    sleep $((attempt * 10))
done
echo "apt-get could not install: $*" >&2
exit 1
