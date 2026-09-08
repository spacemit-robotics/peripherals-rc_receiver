#!/usr/bin/env bash
set -euo pipefail

device="${RC_RECEIVER_DEVICE:-/dev/ttyS5}"
binary="${RC_RECEIVER_TEST_BINARY:-output/staging/bin/test_rc_receiver}"
timeout_s="${RC_RECEIVER_SMOKE_TIMEOUT_S:-5}"

if [[ ! -e "${device}" ]]; then
    echo "SKIP: SBUS device is not present: ${device}"
    exit 77
fi
if [[ ! -r "${device}" ]]; then
    echo "SKIP: SBUS device is not readable: ${device}"
    exit 77
fi
if [[ ! -x "${binary}" ]]; then
    echo "ERROR: diagnostic binary is not executable: ${binary}" >&2
    exit 1
fi

log_file="$(mktemp)"
trap 'rm -f "${log_file}"' EXIT
set +e
timeout "${timeout_s}" "${binary}" "${device}" >"${log_file}" 2>&1
status=$?
set -e

if ! grep -q '^CH1-16:' "${log_file}"; then
    echo "ERROR: no complete SBUS frame received from ${device}" >&2
    cat "${log_file}" >&2
    exit 1
fi

if [[ "${status}" -ne 0 && "${status}" -ne 124 ]]; then
    echo "ERROR: diagnostic exited with status ${status}" >&2
    cat "${log_file}" >&2
    exit "${status}"
fi

echo "SBUS UART smoke passed: ${device}"
