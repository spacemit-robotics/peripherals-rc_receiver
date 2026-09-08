/**
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file test_rc_receiver.c
 * @brief Print normalized channels from an SBUS UART receiver.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "rc_receiver.h"

#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static volatile sig_atomic_t running = 1;

static void stop_receiver(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static void print_usage(const char *program)
{
    fprintf(stderr, "Usage: %s [device]\n", program);
    fprintf(stderr, "The SBUS driver supplies the default device and baudrate.\n");
}

static void print_frame(struct rc_receiver *receiver,
        const struct rc_receiver_frame *frame, void *context)
{
    size_t channel;

    (void)receiver;
    (void)context;
    printf("CH1-%u:", (unsigned int)frame->channel_count);
    for (channel = 0U; channel < frame->channel_count; ++channel) {
        printf(" %u", (unsigned int)frame->channels[channel]);
    }
    printf(" flags=0x%02x\n", (unsigned int)frame->flags);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        print_usage(argv[0]);
        return 2;
    }

    const char *device = NULL;
    const char *driver = getenv("RC_RECEIVER_DRIVER");
    struct rc_receiver *receiver;

    if (argc > 1) {
        device = argv[1];
    }
    if (driver == NULL || driver[0] == '\0') {
        driver = "sbus";
    }

    receiver = rc_receiver_alloc_uart(driver, device, 0U, NULL);
    if (receiver == NULL) {
        fprintf(stderr, "failed to allocate %s receiver\n", driver);
        return 1;
    }
    if (rc_receiver_init(receiver) != 0) {
        fprintf(stderr, "failed to initialize %s receiver\n", driver);
        rc_receiver_free(receiver);
        return 1;
    }

    signal(SIGINT, stop_receiver);
    signal(SIGTERM, stop_receiver);
    printf("listening %s with %s driver; printing normalized CH1-%u "
            "(Ctrl-C to stop)\n", device != NULL ? device : "driver default",
            driver, (unsigned int)RC_RECEIVER_CHANNEL_COUNT);
    if (rc_receiver_set_callback(receiver, print_frame, NULL) != 0) {
        fprintf(stderr, "failed to start receiver callback\n");
        rc_receiver_free(receiver);
        return 1;
    }

    while (running) {
        const struct timespec delay = {0, 100000000L};
        (void)nanosleep(&delay, NULL);
    }

    (void)rc_receiver_set_callback(receiver, NULL, NULL);
    putchar('\n');
    rc_receiver_free(receiver);
    return 0;
}
