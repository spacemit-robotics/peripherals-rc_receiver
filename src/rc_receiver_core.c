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

int rc_receiver_init(struct rc_receiver *receiver)
{
    if (receiver == NULL || receiver->ops == NULL ||
            receiver->ops->init == NULL || receiver->callback != NULL) {
        return RC_RECEIVER_ERROR;
    }
    return receiver->ops->init(receiver);
}

int rc_receiver_read(struct rc_receiver *receiver,
        struct rc_receiver_frame *frame)
{
    if (receiver == NULL || frame == NULL || receiver->ops == NULL ||
            receiver->ops->read == NULL || receiver->callback != NULL) {
        return RC_RECEIVER_ERROR;
    }
    return receiver->ops->read(receiver, frame);
}

int rc_receiver_set_callback(struct rc_receiver *receiver,
        rc_receiver_callback_t callback, void *context)
{
    int result;

    if (receiver == NULL || receiver->ops == NULL) {
        return RC_RECEIVER_ERROR;
    }

    if (receiver->callback != NULL) {
        if (receiver->ops->set_callback == NULL ||
                receiver->ops->set_callback(receiver, NULL, NULL) != 0) {
            return RC_RECEIVER_ERROR;
        }
        receiver->callback = NULL;
        receiver->callback_context = NULL;
    }
    if (callback == NULL) {
        return 0;
    }
    if (receiver->ops->read == NULL || receiver->ops->set_callback == NULL) {
        return RC_RECEIVER_ERROR;
    }

    receiver->callback = callback;
    receiver->callback_context = context;
    result = receiver->ops->set_callback(receiver, callback, context);
    if (result != 0) {
        receiver->callback = NULL;
        receiver->callback_context = NULL;
        return RC_RECEIVER_ERROR;
    }
    return 0;
}

void rc_receiver_free(struct rc_receiver *receiver)
{
    if (receiver == NULL) {
        return;
    }

    if (receiver->callback != NULL &&
            rc_receiver_set_callback(receiver, NULL, NULL) != 0) {
        return;
    }

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
    if (receiver->callback != NULL &&
            rc_receiver_set_callback(receiver, NULL, NULL) != 0) {
        return;
    }
    if (receiver->ops != NULL &&
            receiver->ops->close != NULL) {
        receiver->ops->close(receiver);
    }
}

uint8_t rc_receiver_is_open(const struct rc_receiver *receiver)
{
    if (receiver == NULL || receiver->ops == NULL ||
            receiver->ops->is_open == NULL) {
        return 0U;
    }
    return receiver->ops->is_open(receiver);
}

uint64_t rc_receiver_invalid_frames(const struct rc_receiver *receiver)
{
    if (receiver == NULL || receiver->ops == NULL ||
            receiver->ops->invalid_frames == NULL) {
        return 0U;
    }
    return receiver->ops->invalid_frames(receiver);
}

int rc_receiver_last_error(const struct rc_receiver *receiver)
{
    if (receiver == NULL || receiver->ops == NULL ||
            receiver->ops->last_error == NULL) {
        return 0;
    }
    return receiver->ops->last_error(receiver);
}
