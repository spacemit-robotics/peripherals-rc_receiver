/**
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file drv_uart_sbus.c
 * @brief SBUS protocol parser and Linux UART receiver driver.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../rc_receiver_core.h"

#include <asm/termbits.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define RC_RECEIVER_SBUS_START_BYTE 0x0FU
#define RC_RECEIVER_SBUS_FRAME_SIZE 25U
#define RC_RECEIVER_SBUS_CHANNEL_COUNT 16U
#define RC_RECEIVER_SBUS_CHANNEL_VALUE_MAX 2047U

#define RC_RECEIVER_SBUS_DEFAULT_DEVICE "/dev/ttyS5"
#define RC_RECEIVER_SBUS_DEFAULT_BAUDRATE 100000U
#define RC_RECEIVER_SBUS_READ_BUFFER_SIZE 512U
#define RC_RECEIVER_SBUS_REOPEN_DELAY_MS 1000U
#define RC_RECEIVER_SBUS_CALLBACK_WAIT_MS 100
#define RC_RECEIVER_SBUS_STATUS_OFFSET 23U
#define RC_RECEIVER_SBUS_CLASSIC_FOOTER 0x00U
#define RC_RECEIVER_SBUS2_FOOTER_MASK 0x0FU
#define RC_RECEIVER_SBUS2_FOOTER_VALUE 0x04U
#define RC_RECEIVER_SBUS_STATUS_FRAME_LOST 0x04U
#define RC_RECEIVER_SBUS_STATUS_FAILSAFE 0x08U

struct rc_receiver_sbus_config {
    uint8_t accept_sbus2_footer;
};

struct rc_receiver_sbus_parser {
    uint8_t frame[RC_RECEIVER_SBUS_FRAME_SIZE];
    size_t position;
    uint64_t invalid_frames;
    uint8_t accept_sbus2_footer;
};
typedef struct rc_receiver_sbus_parser rc_receiver_sbus_parser_t;

static rc_receiver_sbus_parser_t *rc_receiver_sbus_parser_create(
    uint8_t accept_sbus2_footer);
static void rc_receiver_sbus_parser_destroy(
    rc_receiver_sbus_parser_t *parser);
static void rc_receiver_sbus_parser_reset(
    rc_receiver_sbus_parser_t *parser);
static int rc_receiver_sbus_parser_feed(
    rc_receiver_sbus_parser_t *parser,
    const uint8_t *data,
    size_t size,
    struct rc_receiver_frame *output);
static uint64_t rc_receiver_sbus_parser_invalid_frames(
    const rc_receiver_sbus_parser_t *parser);

struct rc_receiver_sbus_priv {
    char *device;
    uint32_t baudrate;
    int fd;
    struct termios2 saved_options;
    uint8_t saved_options_valid;
    uint64_t next_open_attempt_ms;
    int last_error;
    rc_receiver_sbus_parser_t *parser;
    pthread_t callback_thread;
    pthread_mutex_t callback_lock;
    uint8_t callback_lock_valid;
    uint8_t callback_thread_running;
    uint8_t callback_thread_stop;
};

static char *duplicate_string(const char *value)
{
    char *copy;
    size_t length;

    if (value == NULL) {
        return NULL;
    }
    length = strlen(value) + 1U;
    copy = (char *)malloc(length);
    if (copy != NULL) {
        memcpy(copy, value, length);
    }
    return copy;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * 1000U +
        (uint64_t)now.tv_nsec / 1000000U;
}

static uint64_t monotonic_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * 1000000U +
        (uint64_t)now.tv_nsec / 1000U;
}

static uint16_t normalize_channel(uint16_t value)
{
    return (uint16_t)(((uint32_t)value * RC_RECEIVER_CHANNEL_MAX +
        RC_RECEIVER_SBUS_CHANNEL_VALUE_MAX / 2U) /
        RC_RECEIVER_SBUS_CHANNEL_VALUE_MAX);
}

static int valid_footer(const rc_receiver_sbus_parser_t *parser,
        uint8_t value)
{
    return value == RC_RECEIVER_SBUS_CLASSIC_FOOTER ||
        (parser->accept_sbus2_footer &&
        (value & RC_RECEIVER_SBUS2_FOOTER_MASK) ==
        RC_RECEIVER_SBUS2_FOOTER_VALUE);
}

static int decode_frame(const rc_receiver_sbus_parser_t *parser,
        struct rc_receiver_frame *output)
{
    size_t channel;
    uint8_t status;

    if (parser->frame[0] != RC_RECEIVER_SBUS_START_BYTE ||
            !valid_footer(parser,
            parser->frame[RC_RECEIVER_SBUS_FRAME_SIZE - 1U])) {
        return 0;
    }
    if (output == NULL) {
        return 1;
    }

    memset(output, 0, sizeof(*output));
    output->channel_count = RC_RECEIVER_SBUS_CHANNEL_COUNT;
    for (channel = 0U; channel < RC_RECEIVER_SBUS_CHANNEL_COUNT; ++channel) {
        const size_t bit_offset = channel * 11U;
        const size_t byte_offset = 1U + bit_offset / 8U;
        const unsigned shift = (unsigned)(bit_offset % 8U);
        const uint32_t word = (uint32_t)parser->frame[byte_offset] |
            ((uint32_t)parser->frame[byte_offset + 1U] << 8U) |
            ((uint32_t)parser->frame[byte_offset + 2U] << 16U);

        output->channels[channel] = normalize_channel((uint16_t)(
            (word >> shift) & RC_RECEIVER_SBUS_CHANNEL_VALUE_MAX));
    }

    status = parser->frame[RC_RECEIVER_SBUS_STATUS_OFFSET];
    if ((status & RC_RECEIVER_SBUS_STATUS_FRAME_LOST) != 0U) {
        output->flags |= RC_RECEIVER_FLAG_FRAME_LOST;
    }
    if ((status & RC_RECEIVER_SBUS_STATUS_FAILSAFE) != 0U) {
        output->flags |= RC_RECEIVER_FLAG_FAILSAFE;
    }
    return 1;
}

static void resynchronize(rc_receiver_sbus_parser_t *parser)
{
    size_t next = RC_RECEIVER_SBUS_FRAME_SIZE;
    size_t index;

    for (index = 1U; index < RC_RECEIVER_SBUS_FRAME_SIZE; ++index) {
        if (parser->frame[index] == RC_RECEIVER_SBUS_START_BYTE) {
            next = index;
            break;
        }
    }

    if (next == RC_RECEIVER_SBUS_FRAME_SIZE) {
        parser->position = 0U;
        return;
    }

    memmove(parser->frame, parser->frame + next,
            RC_RECEIVER_SBUS_FRAME_SIZE - next);
    parser->position = RC_RECEIVER_SBUS_FRAME_SIZE - next;
}

static rc_receiver_sbus_parser_t *rc_receiver_sbus_parser_create(
        uint8_t accept_sbus2_footer)
{
    rc_receiver_sbus_parser_t *parser;

    parser = (rc_receiver_sbus_parser_t *)calloc(1U, sizeof(*parser));
    if (parser != NULL) {
        parser->accept_sbus2_footer = accept_sbus2_footer != 0U;
    }
    return parser;
}

static void rc_receiver_sbus_parser_destroy(rc_receiver_sbus_parser_t *parser)
{
    free(parser);
}

static void rc_receiver_sbus_parser_reset(rc_receiver_sbus_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }
    memset(parser->frame, 0, sizeof(parser->frame));
    parser->position = 0U;
}

static int rc_receiver_sbus_parser_feed(rc_receiver_sbus_parser_t *parser,
        const uint8_t *data, size_t size, struct rc_receiver_frame *output)
{
    int received = RC_RECEIVER_NO_DATA;
    size_t index;

    if (parser == NULL || (data == NULL && size != 0U)) {
        return RC_RECEIVER_ERROR;
    }

    for (index = 0U; index < size; ++index) {
        const uint8_t value = data[index];

        if (parser->position == 0U) {
            if (value == RC_RECEIVER_SBUS_START_BYTE) {
                parser->frame[0U] = value;
                parser->position = 1U;
            }
            continue;
        }

        parser->frame[parser->position++] = value;
        if (parser->position < RC_RECEIVER_SBUS_FRAME_SIZE) {
            continue;
        }

        if (decode_frame(parser, output)) {
            received = RC_RECEIVER_FRAME;
            parser->position = 0U;
        } else {
            ++parser->invalid_frames;
            resynchronize(parser);
        }
    }
    return received;
}

static uint64_t rc_receiver_sbus_parser_invalid_frames(
        const rc_receiver_sbus_parser_t *parser)
{
    return parser == NULL ? 0U : parser->invalid_frames;
}

static void close_port(struct rc_receiver_sbus_priv *priv)
{
    if (priv == NULL || priv->fd < 0) {
        return;
    }
    if (priv->saved_options_valid != 0U) {
        (void)ioctl(priv->fd, TCSETS2, &priv->saved_options);
    }
    (void)close(priv->fd);
    priv->fd = -1;
    priv->saved_options_valid = 0U;
}

static int configure_port(struct rc_receiver_sbus_priv *priv, int fd)
{
    struct termios2 options;

    memset(&options, 0, sizeof(options));
    if (ioctl(fd, TCGETS2, &options) != 0) {
        return -1;
    }

    priv->saved_options = options;
    priv->saved_options_valid = 1U;
    options.c_cflag &= ~(CBAUD | CSIZE | PARODD | CRTSCTS);
    options.c_cflag |= BOTHER | CS8 | PARENB | CSTOPB | CREAD | CLOCAL;
    options.c_iflag &= ~(IXON | IXOFF | IXANY | ISTRIP | INLCR | ICRNL |
            IGNCR | PARMRK);
    options.c_iflag |= INPCK | IGNPAR;
    options.c_lflag = 0U;
    options.c_oflag = 0U;
    options.c_cc[VMIN] = 0U;
    options.c_cc[VTIME] = 0U;
    options.c_ispeed = priv->baudrate;
    options.c_ospeed = priv->baudrate;
    if (ioctl(fd, TCSETS2, &options) != 0) {
        priv->saved_options_valid = 0U;
        return -1;
    }
    return 0;
}

static int open_port(struct rc_receiver_sbus_priv *priv)
{
    const uint64_t now = monotonic_ms();
    int fd;

    if (priv->next_open_attempt_ms != 0U &&
            now < priv->next_open_attempt_ms) {
        return 0;
    }
    priv->next_open_attempt_ms = now + RC_RECEIVER_SBUS_REOPEN_DELAY_MS;

    fd = open(priv->device, O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        priv->last_error = errno;
        return -1;
    }
    if (configure_port(priv, fd) != 0) {
        priv->last_error = errno;
        if (priv->saved_options_valid != 0U) {
            (void)ioctl(fd, TCSETS2, &priv->saved_options);
            priv->saved_options_valid = 0U;
        }
        (void)close(fd);
        return -1;
    }

    (void)ioctl(fd, TCFLSH, TCIOFLUSH);
    priv->fd = fd;
    priv->last_error = 0;
    priv->next_open_attempt_ms = 0U;
    return 1;
}

static int read_port(struct rc_receiver_sbus_priv *priv, uint8_t *buffer,
        size_t size, ssize_t *count)
{
    ssize_t result;

    if (priv == NULL || buffer == NULL || count == NULL || size == 0U) {
        return RC_RECEIVER_ERROR;
    }
    *count = 0;
    if (priv->fd < 0) {
        const int open_result = open_port(priv);
        if (open_result <= 0) {
            return RC_RECEIVER_NO_DATA;
        }
    }

    result = read(priv->fd, buffer, size);
    if (result > 0) {
        *count = result;
        return RC_RECEIVER_NO_DATA;
    }
    if (result == 0 || errno == EAGAIN || errno == EWOULDBLOCK ||
            errno == EINTR) {
        return RC_RECEIVER_NO_DATA;
    }

    priv->last_error = errno;
    close_port(priv);
    priv->next_open_attempt_ms = monotonic_ms() +
        RC_RECEIVER_SBUS_REOPEN_DELAY_MS;
    return RC_RECEIVER_ERROR;
}

static int sbus_init(struct rc_receiver *receiver)
{
    struct rc_receiver_sbus_priv *priv;

    if (receiver == NULL) {
        return RC_RECEIVER_ERROR;
    }
    priv = (struct rc_receiver_sbus_priv *)receiver->priv_data;
    if (priv == NULL || priv->parser == NULL) {
        return RC_RECEIVER_ERROR;
    }
    priv->last_error = 0;
    rc_receiver_sbus_parser_reset(priv->parser);
    close_port(priv);
    return 0;
}

static int sbus_read(struct rc_receiver *receiver,
        struct rc_receiver_frame *frame)
{
    struct rc_receiver_sbus_priv *priv;
    uint8_t buffer[RC_RECEIVER_SBUS_READ_BUFFER_SIZE];
    int received = RC_RECEIVER_NO_DATA;

    if (receiver == NULL || frame == NULL) {
        return RC_RECEIVER_ERROR;
    }
    priv = (struct rc_receiver_sbus_priv *)receiver->priv_data;
    if (priv == NULL || priv->parser == NULL) {
        return RC_RECEIVER_ERROR;
    }

    for (;;) {
        ssize_t count = 0;
        const int read_result = read_port(priv, buffer, sizeof(buffer),
                &count);

        if (read_result == RC_RECEIVER_ERROR && count <= 0) {
            return received == RC_RECEIVER_FRAME ? received :
                RC_RECEIVER_ERROR;
        }
        if (count <= 0) {
            return received;
        }

        if (rc_receiver_sbus_parser_feed(priv->parser, buffer,
                (size_t)count, frame) == RC_RECEIVER_FRAME) {
            received = RC_RECEIVER_FRAME;
            frame->timestamp_us = monotonic_us();
        }
        if ((size_t)count < sizeof(buffer)) {
            return received;
        }
    }
}

static uint8_t callback_should_stop(struct rc_receiver_sbus_priv *priv)
{
    uint8_t stop;

    (void)pthread_mutex_lock(&priv->callback_lock);
    stop = priv->callback_thread_stop;
    (void)pthread_mutex_unlock(&priv->callback_lock);
    return stop;
}

static void deliver_callback_frame(struct rc_receiver *receiver,
        const struct rc_receiver_frame *frame)
{
    struct rc_receiver_sbus_priv *priv =
        (struct rc_receiver_sbus_priv *)receiver->priv_data;
    rc_receiver_callback_t callback;
    void *context;
    uint8_t stop;

    (void)pthread_mutex_lock(&priv->callback_lock);
    stop = priv->callback_thread_stop;
    (void)pthread_mutex_unlock(&priv->callback_lock);
    callback = receiver->callback;
    context = receiver->callback_context;
    if (stop == 0U && callback != NULL) {
        callback(receiver, frame, context);
    }
}

static void *sbus_callback_worker(void *arg)
{
    struct rc_receiver *receiver = (struct rc_receiver *)arg;
    struct rc_receiver_sbus_priv *priv =
        (struct rc_receiver_sbus_priv *)receiver->priv_data;

    while (callback_should_stop(priv) == 0U) {
        struct pollfd event;
        struct rc_receiver_frame frame;
        int poll_result;

        if (priv->fd < 0) {
            if (open_port(priv) <= 0) {
                (void)poll(NULL, 0U, RC_RECEIVER_SBUS_CALLBACK_WAIT_MS);
                continue;
            }
        }

        event.fd = priv->fd;
        event.events = POLLIN | POLLERR | POLLHUP;
        event.revents = 0;
        do {
            poll_result = poll(&event, 1U,
                    RC_RECEIVER_SBUS_CALLBACK_WAIT_MS);
        } while (poll_result < 0 && errno == EINTR);

        if (poll_result == 0) {
            continue;
        }
        if (poll_result < 0) {
            priv->last_error = errno;
            close_port(priv);
            priv->next_open_attempt_ms = monotonic_ms() +
                RC_RECEIVER_SBUS_REOPEN_DELAY_MS;
            continue;
        }
        if ((event.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            priv->last_error = EIO;
            close_port(priv);
            priv->next_open_attempt_ms = monotonic_ms() +
                RC_RECEIVER_SBUS_REOPEN_DELAY_MS;
            continue;
        }
        if ((event.revents & POLLIN) != 0 &&
                sbus_read(receiver, &frame) == RC_RECEIVER_FRAME) {
            deliver_callback_frame(receiver, &frame);
        }
    }

    (void)pthread_mutex_lock(&priv->callback_lock);
    priv->callback_thread_running = 0U;
    (void)pthread_mutex_unlock(&priv->callback_lock);
    return NULL;
}

static int stop_callback_thread(struct rc_receiver_sbus_priv *priv)
{
    pthread_t thread;
    int join_result;

    if (priv == NULL || priv->callback_lock_valid == 0U) {
        return 0;
    }
    if (pthread_mutex_lock(&priv->callback_lock) != 0) {
        return RC_RECEIVER_ERROR;
    }
    if (priv->callback_thread_running == 0U) {
        priv->callback_thread_stop = 0U;
        (void)pthread_mutex_unlock(&priv->callback_lock);
        return 0;
    }
    if (pthread_equal(pthread_self(), priv->callback_thread) != 0) {
        (void)pthread_mutex_unlock(&priv->callback_lock);
        return RC_RECEIVER_ERROR;
    }

    priv->callback_thread_stop = 1U;
    thread = priv->callback_thread;
    (void)pthread_mutex_unlock(&priv->callback_lock);

    join_result = pthread_join(thread, NULL);
    if (join_result != 0) {
        return RC_RECEIVER_ERROR;
    }

    (void)pthread_mutex_lock(&priv->callback_lock);
    priv->callback_thread_running = 0U;
    priv->callback_thread_stop = 0U;
    (void)pthread_mutex_unlock(&priv->callback_lock);
    return 0;
}

static int sbus_set_callback(struct rc_receiver *receiver,
        rc_receiver_callback_t callback, void *context)
{
    struct rc_receiver_sbus_priv *priv;
    int create_result;

    if (receiver == NULL || receiver->priv_data == NULL) {
        return RC_RECEIVER_ERROR;
    }
    priv = (struct rc_receiver_sbus_priv *)receiver->priv_data;
    if (priv->callback_lock_valid == 0U ||
            stop_callback_thread(priv) != 0) {
        return RC_RECEIVER_ERROR;
    }
    if (callback == NULL) {
        return 0;
    }
    (void)context;

    if (pthread_mutex_lock(&priv->callback_lock) != 0) {
        return RC_RECEIVER_ERROR;
    }
    priv->callback_thread_stop = 0U;
    priv->callback_thread_running = 1U;
    create_result = pthread_create(&priv->callback_thread, NULL,
            sbus_callback_worker, receiver);
    if (create_result != 0) {
        priv->callback_thread_running = 0U;
        priv->last_error = create_result;
    }
    (void)pthread_mutex_unlock(&priv->callback_lock);
    return create_result == 0 ? 0 : RC_RECEIVER_ERROR;
}

static void sbus_close(struct rc_receiver *receiver)
{
    if (receiver != NULL) {
        close_port((struct rc_receiver_sbus_priv *)receiver->priv_data);
    }
}

static void sbus_free(struct rc_receiver *receiver)
{
    struct rc_receiver_sbus_priv *priv;

    if (receiver == NULL) {
        return;
    }
    priv = (struct rc_receiver_sbus_priv *)receiver->priv_data;
    if (priv != NULL) {
        if (stop_callback_thread(priv) != 0) {
            return;
        }
        close_port(priv);
        if (priv->callback_lock_valid != 0U) {
            (void)pthread_mutex_destroy(&priv->callback_lock);
            priv->callback_lock_valid = 0U;
        }
        rc_receiver_sbus_parser_destroy(priv->parser);
        free(priv->device);
        free(priv);
    }
    free(receiver->name);
    free(receiver);
}

static uint8_t sbus_is_open(const struct rc_receiver *receiver)
{
    const struct rc_receiver_sbus_priv *priv;

    if (receiver == NULL) {
        return 0U;
    }
    priv = (const struct rc_receiver_sbus_priv *)receiver->priv_data;
    return priv != NULL && priv->fd >= 0 ? 1U : 0U;
}

static uint64_t sbus_invalid_frames(const struct rc_receiver *receiver)
{
    const struct rc_receiver_sbus_priv *priv;

    if (receiver == NULL) {
        return 0U;
    }
    priv = (const struct rc_receiver_sbus_priv *)receiver->priv_data;
    return priv == NULL ? 0U :
        rc_receiver_sbus_parser_invalid_frames(priv->parser);
}

static int sbus_last_error(const struct rc_receiver *receiver)
{
    const struct rc_receiver_sbus_priv *priv;

    if (receiver == NULL) {
        return 0;
    }
    priv = (const struct rc_receiver_sbus_priv *)receiver->priv_data;
    return priv == NULL ? 0 : priv->last_error;
}

static const struct rc_receiver_ops sbus_ops = {
    .init = sbus_init,
    .read = sbus_read,
    .free = sbus_free,
    .set_callback = sbus_set_callback,
    .close = sbus_close,
    .is_open = sbus_is_open,
    .invalid_frames = sbus_invalid_frames,
    .last_error = sbus_last_error,
};

static struct rc_receiver *sbus_create(void *raw_args)
{
    const struct rc_receiver_args_uart *args =
        (const struct rc_receiver_args_uart *)raw_args;
    const struct rc_receiver_sbus_config *config;
    struct rc_receiver_sbus_priv *priv;
    struct rc_receiver *receiver;
    const char *device;
    uint32_t baudrate;
    uint8_t accept_sbus2_footer = 1U;

    if (args == NULL || args->instance == NULL) {
        return NULL;
    }
    device = args->dev_path != NULL ? args->dev_path :
        RC_RECEIVER_SBUS_DEFAULT_DEVICE;
    baudrate = args->baudrate != 0U ? args->baudrate :
        RC_RECEIVER_SBUS_DEFAULT_BAUDRATE;
    if (device[0] == '\0') {
        return NULL;
    }

    config = (const struct rc_receiver_sbus_config *)args->ex_args;
    if (config != NULL) {
        accept_sbus2_footer = config->accept_sbus2_footer != 0U;
    }

    receiver = rc_receiver_dev_alloc(args->instance, sizeof(*priv));
    if (receiver == NULL) {
        return NULL;
    }
    receiver->ops = &sbus_ops;
    priv = (struct rc_receiver_sbus_priv *)receiver->priv_data;
    priv->device = duplicate_string(device);
    priv->baudrate = baudrate;
    priv->fd = -1;
    priv->last_error = 0;
    priv->parser = rc_receiver_sbus_parser_create(accept_sbus2_footer);
    if (priv->device == NULL || priv->parser == NULL) {
        rc_receiver_free(receiver);
        return NULL;
    }
    priv->last_error = pthread_mutex_init(&priv->callback_lock, NULL);
    if (priv->last_error != 0) {
        rc_receiver_free(receiver);
        return NULL;
    }
    priv->callback_lock_valid = 1U;
    return receiver;
}

REGISTER_RC_RECEIVER_DRIVER("sbus", RC_RECEIVER_DRV_UART, sbus_create)
