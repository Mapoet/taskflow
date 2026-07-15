#!/bin/sh
set -eu
printf 'cli:%s:%s\n' "$1" "${PRIVATE_TOKEN-unset}"
