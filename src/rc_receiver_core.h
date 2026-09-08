/**
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file rc_receiver_core.h
 * @brief Private receiver registry and driver contract.
 */
#ifndef RC_RECEIVER_CORE_H
#define RC_RECEIVER_CORE_H

#include "rc_receiver.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rc_receiver_args_uart {
    const char *instance;
    const char *dev_path;
    uint32_t baudrate;
    void *ex_args;
};

enum rc_receiver_driver_type {
    RC_RECEIVER_DRV_UART = 0,
};

struct rc_receiver_ops {
    /* --- required operations --- */
    int (*init)(struct rc_receiver *receiver);
    int (*read)(struct rc_receiver *receiver,
            struct rc_receiver_frame *frame);
    void (*free)(struct rc_receiver *receiver);

    /* --- optional transport and diagnostic operations --- */
    int (*set_callback)(struct rc_receiver *receiver,
            rc_receiver_callback_t callback, void *context);
    void (*close)(struct rc_receiver *receiver);
    uint8_t (*is_open)(const struct rc_receiver *receiver);
    uint64_t (*invalid_frames)(const struct rc_receiver *receiver);
    int (*last_error)(const struct rc_receiver *receiver);
};

struct rc_receiver {
    char *name;
    const struct rc_receiver_ops *ops;
    void *priv_data;
    rc_receiver_callback_t callback;
    void *callback_context;
};

typedef struct rc_receiver *(*rc_receiver_factory_t)(void *args);

struct rc_receiver_driver_info {
    const char *name;
    enum rc_receiver_driver_type type;
    rc_receiver_factory_t factory;
    struct rc_receiver_driver_info *next;
};

void rc_receiver_driver_register(struct rc_receiver_driver_info *info);

#define REGISTER_RC_RECEIVER_DRIVER(_name, _type, _factory) \
    static struct rc_receiver_driver_info __rc_receiver_driver_##_factory = { \
        .name = _name, \
        .type = _type, \
        .factory = _factory, \
        .next = NULL, \
    }; \
    __attribute__((constructor)) \
    static void __rc_receiver_register_##_factory(void) \
    { \
        rc_receiver_driver_register(&__rc_receiver_driver_##_factory); \
    }

struct rc_receiver *rc_receiver_dev_alloc(const char *instance,
        size_t priv_size);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* RC_RECEIVER_CORE_H */
