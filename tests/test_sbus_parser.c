/**
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file test_sbus_parser.c
 * @brief Parser regression tests using synthetic SBUS frames.
 */
/* Include the implementation so private parser functions remain file-local. */
#include "../src/drivers/drv_uart_sbus.c"

#include <assert.h>
#include <stdint.h>

int main(void)
{
    static const uint8_t frame[RC_RECEIVER_SBUS_FRAME_SIZE] = {
        0x0f, 0xac, 0x00, 0xdf, 0xc4, 0xc1, 0x07, 0x3e, 0xf0,
        0x81, 0x0f, 0x7c, 0xe0, 0x03, 0x1f, 0xf8, 0xc0, 0x07,
        0x3e, 0xf0, 0x81, 0x0f, 0x7c, 0x00, 0x00,
    };

    struct rc_receiver_frame decoded;
    rc_receiver_sbus_parser_t *parser =
        rc_receiver_sbus_parser_create(1U);
    assert(parser != NULL);
    for (size_t i = 0U; i < sizeof(frame); ++i) {
        const int result =
            rc_receiver_sbus_parser_feed(parser, &frame[i], 1U, &decoded);
        if (i + 1U < sizeof(frame)) {
            assert(result == RC_RECEIVER_NO_DATA);
        } else {
            assert(result == RC_RECEIVER_FRAME);
        }
    }
    assert(decoded.channel_count == RC_RECEIVER_SBUS_CHANNEL_COUNT);
    assert(decoded.channels[0] == 5507U);
    assert(decoded.channels[1] == 31759U);
    assert(decoded.channels[2] == 57979U);
    for (size_t i = 3U; i < RC_RECEIVER_SBUS_CHANNEL_COUNT; ++i) {
        assert(decoded.channels[i] == 31759U);
    }
    assert(decoded.flags == 0U);

    uint8_t sbus2[sizeof(frame)];
    for (size_t i = 0U; i < sizeof(sbus2); ++i) {
        sbus2[i] = frame[i];
    }
    /* CH17/CH18 are protocol-specific and must not leak into link flags. */
    sbus2[23U] = 0x0f;
    sbus2[24U] = 0x14;
    rc_receiver_sbus_parser_reset(parser);
    assert(rc_receiver_sbus_parser_feed(parser, sbus2, sizeof(sbus2), &decoded) ==
        RC_RECEIVER_FRAME);
    assert(decoded.flags ==
        (RC_RECEIVER_FLAG_FRAME_LOST | RC_RECEIVER_FLAG_FAILSAFE));
    rc_receiver_sbus_parser_destroy(parser);

    uint8_t bad_then_good[2U * sizeof(frame)];
    for (size_t i = 0U; i < sizeof(frame); ++i) {
        bad_then_good[i] = frame[i];
        bad_then_good[sizeof(frame) + i] = frame[i];
    }
    bad_then_good[sizeof(frame) - 1U] = 0x55;
    parser = rc_receiver_sbus_parser_create(1U);
    assert(parser != NULL);
    assert(rc_receiver_sbus_parser_feed(
        parser, bad_then_good, sizeof(bad_then_good), &decoded) ==
        RC_RECEIVER_FRAME);
    assert(decoded.channels[0] == 5507U);
    assert(rc_receiver_sbus_parser_invalid_frames(parser) >= 1U);
    rc_receiver_sbus_parser_destroy(parser);
    return 0;
}
