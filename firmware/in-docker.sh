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

if [[ -n "${ARGB_BACKEND:-}" ]]; then
    DOCKER_ARGS+=(-e ARGB_BACKEND)
fi

if [[ -n "${ARGB_LED_GPIO:-}" ]]; then
    DOCKER_ARGS+=(-e ARGB_LED_GPIO)
fi

if [[ -n "${ARGB_LED_COUNT:-}" ]]; then
    DOCKER_ARGS+=(-e ARGB_LED_COUNT)
fi

if [[ -n "${ARGB_COLOR_ORDER:-}" ]]; then
    DOCKER_ARGS+=(-e ARGB_COLOR_ORDER)
fi

for ARG_NAME in \
    ARGB_COLOR_CORRECTION_ENABLED \
    ARGB_COLOR_CORRECTION \
    ARGB_COLOR_TEMPERATURE \
    ARGB_COLOR_CORRECTION_R \
    ARGB_COLOR_CORRECTION_G \
    ARGB_COLOR_CORRECTION_B \
    ARGB_COLOR_TEMPERATURE_R \
    ARGB_COLOR_TEMPERATURE_G \
    ARGB_COLOR_TEMPERATURE_B \
    ARGB_COLOR_GAIN_R \
    ARGB_COLOR_GAIN_G \
    ARGB_COLOR_GAIN_B \
    ARGB_COLOR_GAMMA_R \
    ARGB_COLOR_GAMMA_G \
    ARGB_COLOR_GAMMA_B; do
    if [[ -n "${!ARG_NAME:-}" ]]; then
        DOCKER_ARGS+=(-e "$ARG_NAME")
    fi
done

CMD=("$@")
if [[ "${CMD[0]:-}" == "idf.py" ]]; then
    IDF_DEFINES=(
        "-DARGB_EUI_SUFFIX=${ARGB_EUI_SUFFIX:-FF:FE:05}"
        "-DARGB_SERIAL_DEBUG=${ARGB_SERIAL_DEBUG:-0}"
        "-DARGB_BACKEND=${ARGB_BACKEND:-ARGB_BACKEND_SERIAL_JSON}"
        "-DARGB_LED_GPIO=${ARGB_LED_GPIO:--1}"
        "-DARGB_LED_COUNT=${ARGB_LED_COUNT:-12}"
        "-DARGB_COLOR_ORDER=${ARGB_COLOR_ORDER:-GRB}"
        "-DARGB_COLOR_CORRECTION_ENABLED=${ARGB_COLOR_CORRECTION_ENABLED:-1}"
        "-DARGB_COLOR_CORRECTION=${ARGB_COLOR_CORRECTION:-TypicalLEDStrip}"
        "-DARGB_COLOR_TEMPERATURE=${ARGB_COLOR_TEMPERATURE:-Candle}"
        "-DARGB_COLOR_GAIN_R=${ARGB_COLOR_GAIN_R:-1.0}"
        "-DARGB_COLOR_GAIN_G=${ARGB_COLOR_GAIN_G:-1.0}"
        "-DARGB_COLOR_GAIN_B=${ARGB_COLOR_GAIN_B:-0.7}"
        "-DARGB_COLOR_GAMMA_R=${ARGB_COLOR_GAMMA_R:-1.0}"
        "-DARGB_COLOR_GAMMA_G=${ARGB_COLOR_GAMMA_G:-1.0}"
        "-DARGB_COLOR_GAMMA_B=${ARGB_COLOR_GAMMA_B:-1.0}"
    )
    for ARG_NAME in \
        ARGB_COLOR_CORRECTION_R \
        ARGB_COLOR_CORRECTION_G \
        ARGB_COLOR_CORRECTION_B \
        ARGB_COLOR_TEMPERATURE_R \
        ARGB_COLOR_TEMPERATURE_G \
        ARGB_COLOR_TEMPERATURE_B; do
        if [[ -n "${!ARG_NAME:-}" ]]; then
            IDF_DEFINES+=("-D${ARG_NAME}=${!ARG_NAME}")
        fi
    done
    CMD=("idf.py" "${IDF_DEFINES[@]}" "${CMD[@]:1}")
fi

# Only allocate a TTY when stdin is a terminal (interactive use).
if [[ -t 0 ]]; then
    DOCKER_ARGS+=(-it)
fi

exec docker run "${DOCKER_ARGS[@]}" "docker.io/espressif/idf:${IDFVER}" "${CMD[@]}"
