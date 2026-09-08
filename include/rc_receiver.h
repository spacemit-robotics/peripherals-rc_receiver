/**
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file rc_receiver.h
 * @brief Generic radio-control receiver interface.
 */
#ifndef RC_RECEIVER_H
#define RC_RECEIVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of normalized analog channels exposed by the API. */
#define RC_RECEIVER_CHANNEL_COUNT 16U

/** Common normalized channel range used by every protocol driver. */
#define RC_RECEIVER_CHANNEL_MIN 0U
#define RC_RECEIVER_CHANNEL_CENTER 32768U
#define RC_RECEIVER_CHANNEL_MAX 65535U

/** Return values shared by receiver drivers and the public read API. */
enum rc_receiver_result {
    RC_RECEIVER_ERROR = -1,
    RC_RECEIVER_NO_DATA = 0,
    RC_RECEIVER_FRAME = 1,
};

/** Protocol-neutral receiver link status flags. */
enum rc_receiver_frame_flags {
    RC_RECEIVER_FLAG_FRAME_LOST = 1U << 0,
    RC_RECEIVER_FLAG_FAILSAFE = 1U << 1,
};

/**
 * Normalized receiver frame returned by every protocol driver.
 *
 * @param channels Analog channel values normalized to
 *                RC_RECEIVER_CHANNEL_MIN..RC_RECEIVER_CHANNEL_MAX.
 * @param channel_count Number of valid entries in channels.
 * @param flags Protocol-neutral receiver link status flags.
 * @param timestamp_us Monotonic timestamp assigned when the frame was read,
 *                     in us.
 */
struct rc_receiver_frame {
    uint16_t channels[RC_RECEIVER_CHANNEL_COUNT];
    uint8_t channel_count;
    uint8_t flags;
    uint64_t timestamp_us;
};
typedef struct rc_receiver_frame rc_receiver_frame_t;

/** Opaque receiver device handle. */
struct rc_receiver;
typedef struct rc_receiver rc_receiver_t;

/**
 * Callback invoked for each complete receiver frame.
 *
 * The callback runs in the selected driver's delivery context. The frame
 * pointer is valid only for the duration of the callback.
 */
typedef void (*rc_receiver_callback_t)(struct rc_receiver *receiver,
        const struct rc_receiver_frame *frame, void *context);

/**
 * Allocate a UART receiver using a registered protocol driver.
 *
 * @param name Driver name, optionally followed by an instance name using
 *             "driver:instance" (for example, "sbus:main").
 * @param uart_dev UART device path. NULL lets the selected driver use its
 *                 default path, when one is defined.
 * @param baudrate UART baud rate. Zero lets the selected driver use its
 *                 protocol default, when one is defined.
 * @param ex_args Optional protocol-specific arguments borrowed for this call.
 * @return Allocated receiver, or NULL when the driver is not available.
 */
struct rc_receiver *rc_receiver_alloc_uart(const char *name,
        const char *uart_dev, uint32_t baudrate, void *ex_args);

/* --- required lifecycle API --- */

/**
 * Initialize a receiver.
 *
 * @param receiver Receiver allocated by a UART factory.
 * @return 0 on success, or a negative error code.
 */
int rc_receiver_init(struct rc_receiver *receiver);

/**
 * Read the latest available frame without blocking.
 *
 * @param receiver Initialized receiver handle.
 * @param frame Output frame populated when a complete frame is available.
 * @return RC_RECEIVER_FRAME when a frame is available, RC_RECEIVER_NO_DATA
 *         when no complete frame is available, or RC_RECEIVER_ERROR. Returns
 *         RC_RECEIVER_ERROR while callback delivery is enabled.
 */
int rc_receiver_read(struct rc_receiver *receiver,
        struct rc_receiver_frame *frame);

/**
 * Enable or disable asynchronous frame delivery.
 *
 * Registering a callback starts asynchronous delivery in the selected driver.
 * Passing NULL as callback stops delivery and waits for any callback in
 * progress to return. The context pointer is borrowed until callback delivery
 * is disabled.
 *
 * Keep callbacks short. Do not call rc_receiver_set_callback(),
 * rc_receiver_close() or rc_receiver_free() from inside the callback.
 *
 * @param receiver Initialized receiver handle.
 * @param callback Frame callback, or NULL to disable callback delivery.
 * @param context User context passed to callback; ignored when callback is
 *                NULL.
 * @return 0 on success, or RC_RECEIVER_ERROR.
 */
int rc_receiver_set_callback(struct rc_receiver *receiver,
        rc_receiver_callback_t callback, void *context);

/**
 * Release a receiver and all driver-owned resources.
 * Active callback delivery is stopped before resources are released.
 *
 * @param receiver Receiver handle; NULL is ignored.
 */
void rc_receiver_free(struct rc_receiver *receiver);

/* --- optional transport and diagnostic API --- */

/**
 * Close the underlying transport while retaining the receiver object.
 * Active callback delivery is stopped before the transport is closed.
 *
 * @param receiver Receiver handle; NULL is ignored.
 */
void rc_receiver_close(struct rc_receiver *receiver);

/**
 * Return non-zero when the underlying transport is currently open.
 *
 * @param receiver Receiver handle.
 * @return Non-zero when open; zero when closed, invalid, or unsupported.
 */
uint8_t rc_receiver_is_open(const struct rc_receiver *receiver);

/**
 * Return the number of protocol frames rejected by the driver.
 *
 * @param receiver Receiver handle.
 * @return Rejected-frame count, or zero when unavailable.
 */
uint64_t rc_receiver_invalid_frames(const struct rc_receiver *receiver);

/**
 * Return the last transport error.
 *
 * @param receiver Receiver handle.
 * @return The last transport error, or zero when no error was recorded or
 *         the driver does not provide error diagnostics.
 */
int rc_receiver_last_error(const struct rc_receiver *receiver);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* RC_RECEIVER_H */
