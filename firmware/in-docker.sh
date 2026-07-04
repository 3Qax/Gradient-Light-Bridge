#!/bin/bash
# Build the ESP32-C6 firmware inside the official ESP-IDF Docker image.
# Keeps ESP-IDF setup inside the official Docker image.
set -euo pipefail

IDFVER="${IDFVER:-v5.3.2}"

# Ensure the host user can write build artifacts.
chmod a+w,g+s,o+t "$(dirname "$0")"

DOCKER_ARGS=(
    --rm
    -v "$PWD:/project"
    -w /project
    -u "${UID}"
    -e HOME=/tmp
)

if [[ -n "${PORT:-}" ]]; then
    DOCKER_ARGS+=(--device "$PORT")
fi

if [[ -n "${ARGB_EUI_SUFFIX:-}" ]]; then
    DOCKER_ARGS+=(-e ARGB_EUI_SUFFIX)
fi

if [[ -n "${ARGB_SERIAL_DEBUG:-}" ]]; then
    DOCKER_ARGS+=(-e ARGB_SERIAL_DEBUG)
fi

# Only allocate a TTY when stdin is a terminal (interactive use).
if [[ -t 0 ]]; then
    DOCKER_ARGS+=(-it)
fi

exec docker run "${DOCKER_ARGS[@]}" "docker.io/espressif/idf:${IDFVER}" "$@"
