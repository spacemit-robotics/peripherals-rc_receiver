/**
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file test_rc_receiver_core.c
 * @brief Core API and SBUS callback tests with fake data sources.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "rc_receiver_core.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_CHANNEL_COUNT 16U
#define TEST_CALLBACK_CYCLES 100U
#define TEST_WAIT_ATTEMPTS 1000U

static const uint8_t test_sbus_frame[25U] = {
    0x0f, 0xac, 0x00, 0xdf, 0xc4, 0xc1, 0x07, 0x3e, 0xf0,
    0x81, 0x0f, 0x7c, 0xe0, 0x03, 0x1f, 0xf8, 0xc0, 0x07,
    0x3e, 0xf0, 0x81, 0x0f, 0x7c, 0x00, 0x00,
};

struct fake_receiver_priv {
    uint32_t frames_left;
    uint16_t next_value;
    uint32_t init_calls;
    uint32_t close_calls;
    uint8_t open;
    uint8_t fail_read;
};

struct callback_state {
    atomic_uint calls;
    atomic_uint last_value;
    atomic_int failed;
};

static int fake_init(struct rc_receiver *receiver)
{
    struct fake_receiver_priv *priv;

    if (receiver == NULL || receiver->priv_data == NULL) {
        return RC_RECEIVER_ERROR;
    }
    priv = (struct fake_receiver_priv *)receiver->priv_data;
    ++priv->init_calls;
    priv->open = 1U;
    return 0;
}

static int fake_read(struct rc_receiver *receiver,
        struct rc_receiver_frame *frame)
{
    struct fake_receiver_priv *priv;

    if (receiver == NULL || frame == NULL || receiver->priv_data == NULL) {
        return RC_RECEIVER_ERROR;
    }
    priv = (struct fake_receiver_priv *)receiver->priv_data;
    if (priv->fail_read != 0U) {
        return RC_RECEIVER_ERROR;
    }
    if (priv->frames_left == 0U) {
        return RC_RECEIVER_NO_DATA;
    }

    memset(frame, 0, sizeof(*frame));
    frame->channel_count = TEST_CHANNEL_COUNT;
    frame->channels[0] = priv->next_value++;
    frame->timestamp_us = frame->channels[0];
    --priv->frames_left;
    return RC_RECEIVER_FRAME;
}

static void fake_close(struct rc_receiver *receiver)
{
    struct fake_receiver_priv *priv;

    if (receiver == NULL || receiver->priv_data == NULL) {
        return;
    }
    priv = (struct fake_receiver_priv *)receiver->priv_data;
    ++priv->close_calls;
    priv->open = 0U;
}

static void fake_free(struct rc_receiver *receiver)
{
    if (receiver == NULL) {
        return;
    }
    free(receiver->priv_data);
    free(receiver->name);
    free(receiver);
}

static uint8_t fake_is_open(const struct rc_receiver *receiver)
{
    const struct fake_receiver_priv *priv;

    if (receiver == NULL || receiver->priv_data == NULL) {
        return 0U;
    }
    priv = (const struct fake_receiver_priv *)receiver->priv_data;
    return priv->open;
}

static uint64_t fake_invalid_frames(const struct rc_receiver *receiver)
{
    (void)receiver;
    return 7U;
}

static int fake_last_error(const struct rc_receiver *receiver)
{
    (void)receiver;
    return 5;
}

static const struct rc_receiver_ops fake_ops = {
    .init = fake_init,
    .read = fake_read,
    .free = fake_free,
    .close = fake_close,
    .is_open = fake_is_open,
    .invalid_frames = fake_invalid_frames,
    .last_error = fake_last_error,
};

static struct rc_receiver *fake_create(void *raw_args)
{
    const struct rc_receiver_args_uart *args =
        (const struct rc_receiver_args_uart *)raw_args;
    struct rc_receiver *receiver;

    if (args == NULL || args->instance == NULL) {
        return NULL;
    }
    receiver = rc_receiver_dev_alloc(args->instance,
            sizeof(struct fake_receiver_priv));
    if (receiver != NULL) {
        receiver->ops = &fake_ops;
    }
    return receiver;
}

REGISTER_RC_RECEIVER_DRIVER("fake", RC_RECEIVER_DRV_UART, fake_create)

static void receive_frame(struct rc_receiver *receiver,
        const struct rc_receiver_frame *frame, void *context)
{
    struct callback_state *state = (struct callback_state *)context;

    if (receiver == NULL || frame == NULL || state == NULL ||
            frame->channel_count != TEST_CHANNEL_COUNT) {
        if (state != NULL) {
            atomic_store(&state->failed, 1);
        }
        return;
    }
    atomic_store(&state->last_value, frame->channels[0]);
    (void)atomic_fetch_add(&state->calls, 1U);
}

static void initialize_callback_state(struct callback_state *state)
{
    atomic_init(&state->calls, 0U);
    atomic_init(&state->last_value, 0U);
    atomic_init(&state->failed, 0);
}

static uint64_t monotonic_us(void)
{
    struct timespec now;

    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000000U +
        (uint64_t)now.tv_nsec / 1000U;
}

static int create_test_pty(char *slave_path, size_t slave_path_size,
        int *slave_fd)
{
    const char *path;
    int master_fd;
    int length;

    master_fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    assert(master_fd >= 0);
    assert(grantpt(master_fd) == 0);
    assert(unlockpt(master_fd) == 0);
    path = ptsname(master_fd);
    assert(path != NULL);
    length = snprintf(slave_path, slave_path_size, "%s", path);
    assert(length > 0 && (size_t)length < slave_path_size);
    *slave_fd = open(slave_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    assert(*slave_fd >= 0);
    return master_fd;
}

static void wait_for_receiver_open(struct rc_receiver *receiver)
{
    const struct timespec delay = {0, 1000000L};
    unsigned int attempt;

    for (attempt = 0U; attempt < TEST_WAIT_ATTEMPTS; ++attempt) {
        if (rc_receiver_is_open(receiver) != 0U) {
            return;
        }
        (void)nanosleep(&delay, NULL);
    }
    assert(rc_receiver_is_open(receiver) != 0U);
}

static void write_test_sbus_frame(int fd)
{
    size_t offset = 0U;

    while (offset < sizeof(test_sbus_frame)) {
        const ssize_t result = write(fd, test_sbus_frame + offset,
                sizeof(test_sbus_frame) - offset);

        if (result < 0 && errno == EINTR) {
            continue;
        }
        assert(result > 0);
        offset += (size_t)result;
    }
}

static void wait_for_polling_frame(struct rc_receiver *receiver,
        struct rc_receiver_frame *frame)
{
    const struct timespec delay = {0, 1000000L};
    unsigned int attempt;

    for (attempt = 0U; attempt < TEST_WAIT_ATTEMPTS; ++attempt) {
        const int result = rc_receiver_read(receiver, frame);

        if (result == RC_RECEIVER_FRAME) {
            return;
        }
        assert(result == RC_RECEIVER_NO_DATA);
        (void)nanosleep(&delay, NULL);
    }
    assert(0 && "timed out waiting for polling frame");
}

static void wait_for_callbacks(const struct callback_state *state,
        unsigned int expected)
{
    const struct timespec delay = {0, 1000000L};
    unsigned int attempt;

    for (attempt = 0U; attempt < TEST_WAIT_ATTEMPTS; ++attempt) {
        if (atomic_load(&state->calls) >= expected) {
            return;
        }
        (void)nanosleep(&delay, NULL);
    }
    assert(atomic_load(&state->calls) >= expected);
}

static void test_functional(void)
{
    const struct timespec delay = {0, 10000000L};
    char slave_path[128];
    struct rc_receiver_frame frame;
    struct fake_receiver_priv *priv;
    struct callback_state state;
    struct rc_receiver *receiver;
    int master_fd;
    int slave_fd;
    unsigned int stopped_count;

    /* The fake driver verifies the retained synchronous polling API. */
    receiver = rc_receiver_alloc_uart("fake:primary", NULL, 0U, NULL);
    assert(receiver != NULL);
    assert(strcmp(receiver->name, "primary") == 0);
    priv = (struct fake_receiver_priv *)receiver->priv_data;
    priv->next_value = 100U;

    assert(rc_receiver_init(receiver) == 0);
    assert(priv->init_calls == 1U);
    assert(rc_receiver_is_open(receiver) != 0U);

    priv->frames_left = 1U;
    assert(rc_receiver_read(receiver, &frame) == RC_RECEIVER_FRAME);
    assert(frame.channels[0] == 100U);
    assert(rc_receiver_invalid_frames(receiver) == 7U);
    assert(rc_receiver_last_error(receiver) == 5);
    rc_receiver_close(receiver);
    assert(priv->close_calls == 1U);
    assert(rc_receiver_is_open(receiver) == 0U);
    rc_receiver_free(receiver);

    /* A PTY provides real fd readiness events for the SBUS callback path. */
    master_fd = create_test_pty(slave_path, sizeof(slave_path), &slave_fd);
    receiver = rc_receiver_alloc_uart("sbus:callback", slave_path, 0U, NULL);
    assert(receiver != NULL);
    assert(rc_receiver_init(receiver) == 0);
    initialize_callback_state(&state);
    assert(rc_receiver_set_callback(receiver, receive_frame, &state) == 0);
    wait_for_receiver_open(receiver);
    assert(rc_receiver_read(receiver, &frame) == RC_RECEIVER_ERROR);

    write_test_sbus_frame(master_fd);
    wait_for_callbacks(&state, 1U);
    assert(atomic_load(&state.failed) == 0);
    assert(atomic_load(&state.calls) == 1U);
    assert(atomic_load(&state.last_value) == 5507U);

    assert(rc_receiver_set_callback(receiver, NULL, NULL) == 0);
    stopped_count = atomic_load(&state.calls);
    write_test_sbus_frame(master_fd);
    (void)nanosleep(&delay, NULL);
    assert(atomic_load(&state.calls) == stopped_count);
    wait_for_polling_frame(receiver, &frame);
    assert(frame.channels[0] == 5507U);

    assert(rc_receiver_set_callback(receiver, receive_frame, &state) == 0);
    rc_receiver_close(receiver);
    assert(rc_receiver_is_open(receiver) == 0U);
    rc_receiver_free(receiver);
    (void)close(slave_fd);
    (void)close(master_fd);
}

static void test_error_paths(void)
{
    const struct timespec delay = {0, 10000000L};
    char long_name[80];
    struct rc_receiver_frame frame;
    struct fake_receiver_priv *priv;
    struct callback_state state;
    struct rc_receiver *receiver;
    uint64_t start_us;

    memset(long_name, 'x', sizeof(long_name));
    long_name[sizeof(long_name) - 1U] = '\0';
    assert(rc_receiver_alloc_uart(NULL, NULL, 0U, NULL) == NULL);
    assert(rc_receiver_alloc_uart("", NULL, 0U, NULL) == NULL);
    assert(rc_receiver_alloc_uart(":missing", NULL, 0U, NULL) == NULL);
    assert(rc_receiver_alloc_uart("fake:", NULL, 0U, NULL) == NULL);
    assert(rc_receiver_alloc_uart(long_name, NULL, 0U, NULL) == NULL);
    assert(rc_receiver_alloc_uart("unknown", NULL, 0U, NULL) == NULL);

    receiver = rc_receiver_alloc_uart("sbus:missing-uart",
            "/dev/rc_receiver_test_missing_uart", 0U, NULL);
    assert(receiver != NULL);
    assert(rc_receiver_init(receiver) == 0);
    start_us = monotonic_us();
    assert(rc_receiver_read(receiver, &frame) == RC_RECEIVER_NO_DATA);
    assert(monotonic_us() - start_us < 100000U);
    assert(rc_receiver_last_error(receiver) == ENOENT);
    rc_receiver_free(receiver);

    initialize_callback_state(&state);
    assert(rc_receiver_init(NULL) == RC_RECEIVER_ERROR);
    assert(rc_receiver_read(NULL, &frame) == RC_RECEIVER_ERROR);
    assert(rc_receiver_set_callback(NULL, receive_frame, &state) ==
        RC_RECEIVER_ERROR);

    receiver = rc_receiver_dev_alloc("incomplete", 0U);
    assert(receiver != NULL);
    assert(rc_receiver_init(receiver) == RC_RECEIVER_ERROR);
    assert(rc_receiver_read(receiver, &frame) == RC_RECEIVER_ERROR);
    assert(rc_receiver_read(receiver, NULL) == RC_RECEIVER_ERROR);
    assert(rc_receiver_set_callback(receiver, receive_frame, &state) ==
        RC_RECEIVER_ERROR);
    assert(rc_receiver_set_callback(receiver, NULL, NULL) ==
        RC_RECEIVER_ERROR);
    assert(rc_receiver_is_open(receiver) == 0U);
    assert(rc_receiver_invalid_frames(receiver) == 0U);
    assert(rc_receiver_last_error(receiver) == 0);
    rc_receiver_free(receiver);

    receiver = rc_receiver_alloc_uart("fake:error", NULL, 0U, NULL);
    assert(receiver != NULL);
    assert(rc_receiver_init(receiver) == 0);
    priv = (struct fake_receiver_priv *)receiver->priv_data;
    priv->fail_read = 1U;
    assert(rc_receiver_read(receiver, &frame) == RC_RECEIVER_ERROR);
    assert(rc_receiver_set_callback(receiver, receive_frame, &state) ==
        RC_RECEIVER_ERROR);
    assert(rc_receiver_read(receiver, &frame) == RC_RECEIVER_ERROR);
    assert(rc_receiver_set_callback(receiver, NULL, NULL) == 0);
    assert(atomic_load(&state.calls) == 0U);
    rc_receiver_free(receiver);

    receiver = rc_receiver_alloc_uart("sbus:free-active",
            "/dev/rc_receiver_test_missing_uart", 0U, NULL);
    assert(receiver != NULL);
    assert(rc_receiver_init(receiver) == 0);
    assert(rc_receiver_set_callback(receiver, receive_frame, &state) == 0);
    (void)nanosleep(&delay, NULL);
    start_us = monotonic_us();
    rc_receiver_free(receiver);
    assert(monotonic_us() - start_us < 200000U);

    rc_receiver_close(NULL);
    rc_receiver_free(NULL);
}

static void test_stability(void)
{
    char slave_path[128];
    struct callback_state state;
    struct rc_receiver *receiver;
    int master_fd;
    int slave_fd;
    unsigned int cycle;

    master_fd = create_test_pty(slave_path, sizeof(slave_path), &slave_fd);
    receiver = rc_receiver_alloc_uart("sbus:stability", slave_path, 0U, NULL);
    assert(receiver != NULL);
    assert(rc_receiver_init(receiver) == 0);
    initialize_callback_state(&state);

    for (cycle = 0U; cycle < TEST_CALLBACK_CYCLES; ++cycle) {
        assert(rc_receiver_set_callback(receiver, receive_frame, &state) == 0);
        wait_for_receiver_open(receiver);
        write_test_sbus_frame(master_fd);
        wait_for_callbacks(&state, cycle + 1U);
        assert(rc_receiver_set_callback(receiver, NULL, NULL) == 0);
        assert(atomic_load(&state.calls) == cycle + 1U);
    }

    assert(atomic_load(&state.failed) == 0);
    assert(atomic_load(&state.last_value) == 5507U);
    rc_receiver_free(receiver);
    (void)close(slave_fd);
    (void)close(master_fd);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "functional") == 0) {
        test_functional();
    } else if (strcmp(argv[1], "error-paths") == 0) {
        test_error_paths();
    } else if (strcmp(argv[1], "stability") == 0) {
        test_stability();
    } else {
        assert(0 && "unknown test mode");
    }
    return 0;
}
