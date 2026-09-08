#!/usr/bin/env bash
set -euo pipefail

library="${1:?library path required}"
include_dir="${2:?include directory required}"
test -f "${library}"
test -f "${include_dir}/rc_receiver.h"

tmp_dir="$(mktemp -d)"
trap 'rm -rf "${tmp_dir}"' EXIT
cat >"${tmp_dir}/api_contract.c" <<'EOF'
#include "rc_receiver.h"

#include <stddef.h>

static void receive_frame(struct rc_receiver *receiver,
        const struct rc_receiver_frame *frame, void *context)
{
    (void)receiver;
    (void)frame;
    (void)context;
}

int main(void)
{
    struct rc_receiver *receiver;

    receiver = rc_receiver_alloc_uart("sbus:contract", NULL, 0U, NULL);
    if (receiver == 0) {
        return 1;
    }
    if (rc_receiver_init(receiver) != 0) {
        rc_receiver_free(receiver);
        return 1;
    }
    if (rc_receiver_set_callback(receiver, receive_frame, NULL) != 0) {
        rc_receiver_free(receiver);
        return 1;
    }
    if (rc_receiver_set_callback(receiver, NULL, NULL) != 0) {
        rc_receiver_free(receiver);
        return 1;
    }
    rc_receiver_free(receiver);
    return 0;
}
EOF

cc -std=c11 -Wall -Wextra -Werror -I"${include_dir}" \
    "${tmp_dir}/api_contract.c" -L"$(dirname "${library}")" \
    -lrc_receiver -Wl,-rpath,"$(dirname "${library}")" \
    -o "${tmp_dir}/api_contract"
"${tmp_dir}/api_contract"
