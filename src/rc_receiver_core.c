/**
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file rc_receiver_core.c
 * @brief Generic receiver registry and lifecycle implementation.
 */
#include "rc_receiver_core.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>

static struct rc_receiver_driver_info *g_driver_list;

enum split_driver_result {
    SPLIT_DRIVER_ERROR = -1,
    SPLIT_DRIVER_ONLY = 0,
    SPLIT_DRIVER_INSTANCE = 1,
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

static struct rc_receiver_driver_info *find_driver(const char *name,
        enum rc_receiver_driver_type type)
{
    struct rc_receiver_driver_info *driver = g_driver_list;

    while (driver != NULL) {
        if (driver->name != NULL && name != NULL &&
                strcmp(driver->name, name) == 0) {
            return driver->type == type ? driver : NULL;
        }
        driver = driver->next;
    }
    return NULL;
}

static int split_driver_instance(const char *name, char *driver,
        size_t driver_size, const char **instance)
{
    const char *separator;
    size_t driver_length;

    if (name == NULL || name[0] == '\0' || driver == NULL ||
            driver_size == 0U || instance == NULL) {
        return SPLIT_DRIVER_ERROR;
    }

    separator = strchr(name, ':');
    if (separator == NULL) {
        if (strlen(name) >= driver_size) {
            return SPLIT_DRIVER_ERROR;
        }
        memcpy(driver, name, strlen(name) + 1U);
        *instance = name;
        return SPLIT_DRIVER_ONLY;
    }

    driver_length = (size_t)(separator - name);
    if (driver_length == 0U || driver_length >= driver_size ||
            separator[1] == '\0') {
        return SPLIT_DRIVER_ERROR;
    }

    memcpy(driver, name, driver_length);
    driver[driver_length] = '\0';
    *instance = separator + 1;
    return SPLIT_DRIVER_INSTANCE;
}

void rc_receiver_driver_register(struct rc_receiver_driver_info *info)
{
    if (info == NULL || info->name == NULL || info->factory == NULL) {
        return;
    }
    info->next = g_driver_list;
    g_driver_list = info;
}

struct rc_receiver *rc_receiver_dev_alloc(const char *instance,
        size_t priv_size)
{
    struct rc_receiver *receiver;

    if (instance == NULL || instance[0] == '\0') {
        return NULL;
    }

    receiver = (struct rc_receiver *)calloc(1U, sizeof(*receiver));
    if (receiver == NULL) {
        return NULL;
    }

    receiver->name = duplicate_string(instance);
    if (receiver->name == NULL) {
        free(receiver);
        return NULL;
    }

    if (priv_size != 0U) {
        receiver->priv_data = calloc(1U, priv_size);
        if (receiver->priv_data == NULL) {
            free(receiver->name);
            free(receiver);
            return NULL;
        }
    }

    receiver->stop_fd = -1;
    receiver->event_fd = -1;
    if (pthread_mutex_init(&receiver->state_lock, NULL) != 0) {
        free(receiver->priv_data);
        free(receiver->name);
        free(receiver);
        return NULL;
    }
    if (pthread_mutex_init(&receiver->io_lock, NULL) != 0) {
        pthread_mutex_destroy(&receiver->state_lock);
        free(receiver->priv_data);
        free(receiver->name);
        free(receiver);
        return NULL;
    }
    return receiver;
}

struct rc_receiver *rc_receiver_alloc_uart(const char *name,
        const char *uart_dev, uint32_t baudrate, void *ex_args)
{
    struct rc_receiver_args_uart args;
    struct rc_receiver_driver_info *driver;
    char driver_name[64];
    const char *instance;
    int split_result;

    split_result = split_driver_instance(name, driver_name,
            sizeof(driver_name), &instance);
    if (split_result == SPLIT_DRIVER_ERROR) {
        return NULL;
    }

    driver = find_driver(driver_name, RC_RECEIVER_DRV_UART);
    if (driver == NULL) {
        return NULL;
    }

    args.instance = instance;
    args.dev_path = uart_dev;
    args.baudrate = baudrate;
    args.ex_args = ex_args;
    return driver->factory(&args);
}

/* Lifecycle calls are externally serialized. Only the worker can unregister
 * concurrently; its self-unregister leaves the join to an external caller. */
static int stop_worker(struct rc_receiver *receiver)
{
    receiver->callback = NULL;
    receiver->callback_context = NULL;
    if (!receiver->worker_valid) return 0;
    receiver->mode = RC_STOPPING;
    uint64_t one = 1;
    ssize_t written;
    do {
        written = write(receiver->stop_fd, &one, sizeof(one));
    } while (written < 0 && errno == EINTR);
    if (pthread_equal(pthread_self(), receiver->worker)) return 0;
    pthread_mutex_unlock(&receiver->state_lock);
    int result = pthread_join(receiver->worker, NULL);
    pthread_mutex_lock(&receiver->state_lock);
    if (result) return RC_RECEIVER_ERROR;
    pthread_mutex_lock(&receiver->io_lock);
    receiver->ops->event_stop(receiver);
    pthread_mutex_unlock(&receiver->io_lock);
    close(receiver->stop_fd);
    receiver->stop_fd = -1;
    receiver->event_fd = -1;
    receiver->worker_valid = 0;
    receiver->mode = RC_SYNC;
    return 0;
}

static void *callback_worker(void *arg)
{
    struct rc_receiver *receiver = arg;
    struct pollfd fds[2] = {
        {receiver->event_fd, POLLIN, 0}, {receiver->stop_fd, POLLIN, 0}};
    int error = 0;
    for (;;) {
        int ready = poll(fds, 2, -1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { error = errno; break; }
        if (fds[1].revents) break;
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            error = EIO;
            break;
        }
        if (!(fds[0].revents & POLLIN)) continue;
        struct rc_receiver_frame frame;
        pthread_mutex_lock(&receiver->state_lock);
        if (receiver->mode != RC_ACTIVE) {
            pthread_mutex_unlock(&receiver->state_lock);
            break;
        }
        pthread_mutex_lock(&receiver->io_lock);
        pthread_mutex_unlock(&receiver->state_lock);
        int result = receiver->ops->event_read(receiver, &frame);
        pthread_mutex_unlock(&receiver->io_lock);
        if (result < 0) { error = EIO; break; }
        if (result != RC_RECEIVER_FRAME) continue;
        pthread_mutex_lock(&receiver->state_lock);
        rc_receiver_callback_t cb = receiver->mode == RC_ACTIVE ? receiver->callback : NULL;
        void *context = receiver->callback_context;
        pthread_mutex_unlock(&receiver->state_lock);
        if (cb) cb(receiver, &frame, context);
    }
    pthread_mutex_lock(&receiver->state_lock);
    if (receiver->mode == RC_ACTIVE) {
        receiver->mode = RC_FAULT;
        receiver->callback_error = error ? error : EIO;
        fprintf(stderr, "rc_receiver: callback fault (%d); unregister and reinitialize\n",
                receiver->callback_error);
    }
    pthread_mutex_unlock(&receiver->state_lock);
    return NULL;
}

static int start_worker(struct rc_receiver *receiver)
{
    int result;
    if (!receiver->ops->event_start || !receiver->ops->event_read ||
            !receiver->ops->event_stop) {
        receiver->callback_error = ENOTSUP;
        return RC_RECEIVER_ERROR;
    }
    pthread_mutex_lock(&receiver->io_lock);
    result = receiver->ops->event_start(receiver);
    pthread_mutex_unlock(&receiver->io_lock);
    if (result < 0) {
        receiver->callback_error = -result;
        return RC_RECEIVER_ERROR;
    }
    receiver->event_fd = result;
    receiver->stop_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (receiver->stop_fd < 0) result = errno;
    else {
        receiver->mode = RC_ACTIVE;
        result = pthread_create(&receiver->worker, NULL, callback_worker, receiver);
    }
    if (result) {
        if (receiver->stop_fd >= 0) close(receiver->stop_fd);
        receiver->stop_fd = -1;
        receiver->event_fd = -1;
        receiver->mode = RC_SYNC;
        receiver->callback_error = result;
        pthread_mutex_lock(&receiver->io_lock);
        receiver->ops->event_stop(receiver);
        pthread_mutex_unlock(&receiver->io_lock);
        return RC_RECEIVER_ERROR;
    }
    receiver->callback_error = 0;
    receiver->worker_valid = 1;
    return 0;
}

int rc_receiver_init(struct rc_receiver *receiver)
{
    if (!receiver || !receiver->ops || !receiver->ops->init) return RC_RECEIVER_ERROR;
    pthread_mutex_lock(&receiver->state_lock);
    if (receiver->mode != RC_SYNC) {
        pthread_mutex_unlock(&receiver->state_lock);
        errno = EBUSY;
        return RC_RECEIVER_ERROR;
    }
    pthread_mutex_lock(&receiver->io_lock);
    int result = receiver->ops->init(receiver);
    pthread_mutex_unlock(&receiver->io_lock);
    receiver->initialized = result == 0;
    receiver->callback_error = 0;
    if (!result && receiver->callback) result = start_worker(receiver);
    if (result) {
        receiver->callback = NULL;
        receiver->callback_context = NULL;
    }
    pthread_mutex_unlock(&receiver->state_lock);
    return result;
}

int rc_receiver_read(struct rc_receiver *receiver,
        struct rc_receiver_frame *frame)
{
    if (receiver == NULL || frame == NULL || receiver->ops == NULL ||
            receiver->ops->read == NULL) {
        return RC_RECEIVER_ERROR;
    }
    pthread_mutex_lock(&receiver->state_lock);
    if (receiver->mode != RC_SYNC || !receiver->initialized) {
        pthread_mutex_unlock(&receiver->state_lock);
        errno = EBUSY;
        return RC_RECEIVER_ERROR;
    }
    pthread_mutex_lock(&receiver->io_lock);
    pthread_mutex_unlock(&receiver->state_lock);
    int result = receiver->ops->read(receiver, frame);
    pthread_mutex_unlock(&receiver->io_lock);
    return result;
}

int rc_receiver_set_callback(struct rc_receiver *receiver,
        rc_receiver_callback_t callback, void *context)
{
    int result;

    if (receiver == NULL || receiver->ops == NULL) {
        return RC_RECEIVER_ERROR;
    }

    pthread_mutex_lock(&receiver->state_lock);
    if (callback && (receiver->mode == RC_FAULT ||
            (receiver->worker_valid && pthread_equal(pthread_self(), receiver->worker)))) {
        pthread_mutex_unlock(&receiver->state_lock);
        errno = EBUSY;
        return RC_RECEIVER_ERROR;
    }
    result = stop_worker(receiver);
    if (!result && callback) {
        receiver->callback = callback;
        receiver->callback_context = context;
        if (receiver->initialized) result = start_worker(receiver);
        if (result) {
            receiver->callback = NULL;
            receiver->callback_context = NULL;
        }
    }
    pthread_mutex_unlock(&receiver->state_lock);
    return result;
}

void rc_receiver_free(struct rc_receiver *receiver)
{
    if (receiver == NULL) {
        return;
    }

    pthread_mutex_lock(&receiver->state_lock);
    if (receiver->worker_valid && pthread_equal(pthread_self(), receiver->worker)) {
        pthread_mutex_unlock(&receiver->state_lock);
        return;
    }
    int result = stop_worker(receiver);
    pthread_mutex_unlock(&receiver->state_lock);
    if (result) return;
    pthread_mutex_lock(&receiver->io_lock);
    pthread_mutex_unlock(&receiver->io_lock);
    pthread_mutex_destroy(&receiver->io_lock);
    pthread_mutex_destroy(&receiver->state_lock);

    if (receiver->ops != NULL && receiver->ops->free != NULL) {
        receiver->ops->free(receiver);
        return;
    }

    free(receiver->priv_data);
    free(receiver->name);
    free(receiver);
}

void rc_receiver_close(struct rc_receiver *receiver)
{
    if (receiver == NULL) {
        return;
    }
    pthread_mutex_lock(&receiver->state_lock);
    if (receiver->worker_valid && pthread_equal(pthread_self(), receiver->worker)) {
        pthread_mutex_unlock(&receiver->state_lock);
        return;
    }
    if (stop_worker(receiver)) {
        pthread_mutex_unlock(&receiver->state_lock);
        return;
    }
    pthread_mutex_lock(&receiver->io_lock);
    if (receiver->ops != NULL &&
            receiver->ops->close != NULL) {
        receiver->ops->close(receiver);
    }
    pthread_mutex_unlock(&receiver->io_lock);
    pthread_mutex_unlock(&receiver->state_lock);
}

uint8_t rc_receiver_is_open(const struct rc_receiver *receiver)
{
    if (receiver == NULL || receiver->ops == NULL ||
            receiver->ops->is_open == NULL) {
        return 0U;
    }
    pthread_mutex_t *lock = (pthread_mutex_t *)&receiver->io_lock;
    pthread_mutex_lock(lock);
    uint8_t result = receiver->ops->is_open(receiver);
    pthread_mutex_unlock(lock);
    return result;
}

uint64_t rc_receiver_invalid_frames(const struct rc_receiver *receiver)
{
    if (receiver == NULL || receiver->ops == NULL ||
            receiver->ops->invalid_frames == NULL) {
        return 0U;
    }
    pthread_mutex_t *lock = (pthread_mutex_t *)&receiver->io_lock;
    pthread_mutex_lock(lock);
    uint64_t result = receiver->ops->invalid_frames(receiver);
    pthread_mutex_unlock(lock);
    return result;
}

int rc_receiver_last_error(const struct rc_receiver *receiver)
{
    if (receiver == NULL) {
        return 0;
    }
    pthread_mutex_t *state = (pthread_mutex_t *)&receiver->state_lock;
    pthread_mutex_t *io = (pthread_mutex_t *)&receiver->io_lock;
    pthread_mutex_lock(state);
    pthread_mutex_lock(io);
    int result = receiver->callback_error ? receiver->callback_error :
        (receiver->ops && receiver->ops->last_error ? receiver->ops->last_error(receiver) : 0);
    pthread_mutex_unlock(io);
    pthread_mutex_unlock(state);
    return result;
}
