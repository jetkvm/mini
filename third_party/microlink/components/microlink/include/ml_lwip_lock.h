/*
 * The lwIP state for MicroLink's raw lwIP calls.
 *
 * MicroLink calls a handful of raw lwIP APIs (udp_*, netif_*, wireguardif_*)
 * directly from its own tasks. They are safe only while the task holds the
 * lwIP state, which wireguardif_lwip_lock() takes: the core lock with
 * CONFIG_LWIP_TCPIP_CORE_LOCKING, otherwise the TCP/IP thread parked in a
 * callback. Some of the same calls are reached from inside lwIP callbacks,
 * which already hold the state, so a thread that holds it takes nothing.
 * ml_lwip_lock() returns whether it took the state; pass that back to
 * ml_lwip_unlock().
 */
#pragma once

#include <stdbool.h>

#include "wireguardif.h"

static inline bool ml_lwip_lock(void)
{
    return wireguardif_lwip_lock();
}

static inline void ml_lwip_unlock(bool acquired)
{
    wireguardif_lwip_unlock(acquired);
}
