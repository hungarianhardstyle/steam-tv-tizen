/*
 *  _____  _   _  _____  _  _  _
 * |_   _|| | | |/  ___|| |(_)| |     Steam
 *   | |  | |_| |\ `--. | | _ | |__     In-Home
 *   | |  |  _  | `--. \| || || '_ \      Streaming
 *  _| |_ | | | |/\__/ /| || || |_) |       Library
 *  \___/ \_| |_/\____/ |_||_||_.__/
 *
 * Copyright (c) 2022 Mariotaku <https://github.com/mariotaku>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "base.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "crypto.h"
#include "endianness.h"
#include "ihs_buffer.h"
#include "session/channels/ch_control.h"

static void BaseWorker(IHS_Base *base);

static void BaseUpdateSecretKey(IHS_Base *base, const uint8_t *secretKey);

static bool initialized;

/* Raw datagram counter: the control-channel log lines only show control packets, so an audio or
 * video data flow is invisible to them. This counts every datagram the socket actually delivered,
 * which is what separates "the host stopped sending / the socket is dead" from "the receive thread
 * is wedged and never reaches the socket". Single writer (the receive worker). */
static volatile unsigned long receivedDatagrams;

unsigned long IHS_BaseReceivedDatagramCount(void) {
    return receivedDatagrams;
}

/* Upstream counterpart: sends the kernel refused. Reported alongside the datagram counter so a run
 * says whether the link was dropping our packets (a busy TV Wi-Fi while a 20-30 Mbit/s video flows
 * down) or whether the client simply had nothing to say. */
unsigned long IHS_BaseFailedSendCount(void) {
    return IHS_UDPSocketSendFailuresGlobal();
}

void IHS_Init() {
    if (initialized)
        return;
    IHS_ControlDiagnosticsInit();
    initialized = true;
    IHS_TimerInit();
}

void IHS_Quit() {
    if (!initialized)
        return;
    IHS_TimerQuit();
    IHS_ControlDiagnosticsQuit();
    initialized = false;
}

void IHS_BaseInit(IHS_Base *base, const IHS_ClientConfig *config, IHS_BaseReceivedFunction recvCb,
                  bool broadcast) {
    assert(base != NULL);
    assert(initialized);
    memset(base, 0, sizeof(IHS_Base));
    atomic_init(&base->interrupted, false);
    base->broadcast = broadcast;
    base->lock = IHS_MutexCreate();
    base->callbacks.received = recvCb;

    base->deviceId = config->deviceId;
    strncpy(base->deviceName, config->deviceName ? config->deviceName : "IHSLib",
            sizeof(base->deviceName) - 1);
    base->deviceName[sizeof(base->deviceName) - 1] = '\0';
    BaseUpdateSecretKey(base, config->secretKey);
}

void IHS_BaseSetSecretKey(IHS_Base *base, const uint8_t *secretKey) {
    assert(base != NULL);
    IHS_BaseLock(base);
    BaseUpdateSecretKey(base, secretKey);
    IHS_BaseUnlock(base);
}

void IHS_BaseGetSecretKey(IHS_Base *base, uint8_t *secretKey) {
    assert(base != NULL);
    IHS_BaseLock(base);
    memcpy(secretKey, base->secretKey, sizeof(base->secretKey));
    IHS_BaseUnlock(base);
}

/**
 * The device token, SymmetricEncrypt(u64le device ID), is derived from the secret key.
 */
static void BaseUpdateSecretKey(IHS_Base *base, const uint8_t *secretKey) {
    memcpy(base->secretKey, secretKey, sizeof(base->secretKey));
    uint8_t in[8];
    size_t deviceTokenLen = sizeof(base->deviceToken);
    IHS_WriteUInt64LE(in, base->deviceId);
    IHS_CryptoSymmetricEncrypt(in, 8, base->secretKey, sizeof(base->secretKey), base->deviceToken,
                               &deviceTokenLen);
}

void IHS_BaseSetLogFunction(IHS_Base *base, IHS_LogFunction *logFunction) {
    assert(base != NULL);
    IHS_BaseLock(base);
    base->callbacks.log = logFunction;
    IHS_BaseUnlock(base);
}

void IHS_BaseSetRunCallbacks(IHS_Base *base, const IHS_BaseRunCallbacks *callbacks, void *context) {
    assert(base != NULL);
    IHS_BaseLock(base);
    base->callbacks.run = callbacks;
    base->callbackContexts.run = context;
    IHS_BaseUnlock(base);
}

void IHS_BaseLog(IHS_Base *base, IHS_LogLevel level, const char *tag, const char *fmt, ...) {
    assert(base != NULL);
    /* Deliberately NOT under base->lock. The lock is held across a socket send (see IHS_BaseSend),
     * and taking it here means one slow send silences every thread of the session at once: the log
     * function is installed once, before any worker starts, so reading it is safe. Measured
     * 2026-10-14: during a stalled send the client's log to the host stops dead, which is what a
     * wedged session looks like from the outside. */
    IHS_LogFunction *log = base->callbacks.log;
    if (!log)
        return;
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, 4095, fmt, args);
    log(level, tag, buf);
    va_end(args);
}

const char *IHS_LogLevelName(IHS_LogLevel level) {
    switch (level) {
    case IHS_LogLevelFatal:
        return "Fatal";
    case IHS_LogLevelError:
        return "Error";
    case IHS_LogLevelWarn:
        return "Warn";
    case IHS_LogLevelInfo:
        return "Info";
    case IHS_LogLevelDebug:
        return "Debug";
    case IHS_LogLevelVerbose:
        return "Verbose";
    default:
        return "";
    }
}

bool IHS_BaseStartWorker(IHS_Base *base, const char *name) {
    assert(base != NULL);
    IHS_BaseLock(base);
    if (base->worker != NULL) {
        IHS_BaseUnlock(base);
        return false;
    }
    base->worker = IHS_ThreadCreate((IHS_ThreadFunction *)BaseWorker, name, base);
    IHS_BaseLog(base, IHS_LogLevelInfo, "Worker", "Worker thread %s created", name);
    IHS_BaseUnlock(base);
    return true;
}

void IHS_BaseInterruptWorker(IHS_Base *base) {
    assert(base != NULL);
    IHS_BaseLock(base);
    if (base->interrupted) {
        IHS_BaseUnlock(base);
        return;
    }
    base->interrupted = true;
    IHS_BaseUnlock(base);
}

void IHS_BaseWaitWorker(IHS_Base *base) {
    assert(base != NULL);
    IHS_ThreadJoin(base->worker);
    base->worker = NULL;
}

void IHS_BaseDestroy(IHS_Base *base) {
    assert(base != NULL);
    IHS_MutexDestroy(base->lock);
}

bool IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *data) {
    assert(base != NULL);
    // Hold base->lock across the send so the worker thread's close-on-shutdown can't
    // free the socket out from under us. The worker sets socket = NULL under the same
    // lock before calling IHS_UDPSocketClose, so any send that completes here uses a
    // socket the worker has agreed to keep alive.
    IHS_BaseLock(base);
    bool ret = false;
    if (base->socket != NULL) {
        IHS_UDPPacket packet = {.address = address, .buffer = *data};
        ret = IHS_UDPSocketSend(base->socket, &packet);
    }
    IHS_BaseUnlock(base);
    return ret;
}

void IHS_BaseLock(IHS_Base *base) {
    assert(base != NULL);
    IHS_MutexLock(base->lock);
}

void IHS_BaseUnlock(IHS_Base *base) {
    assert(base != NULL);
    IHS_MutexUnlock(base->lock);
}

static void BaseWorker(IHS_Base *base) {
    assert(base != NULL);
    IHS_BaseLog(base, IHS_LogLevelInfo, "Worker", "Opening UDP socket");
    IHS_UDPSocket *socket = IHS_UDPSocketOpen(base->broadcast);
    IHS_BaseLog(base, IHS_LogLevelInfo, "Worker", "UDP socket opened");
    IHS_BaseLock(base);
    base->socket = socket;
    IHS_BaseUnlock(base);
    if (base->callbacks.run && base->callbacks.run->initialized) {
        base->callbacks.run->initialized(base, base->callbackContexts.run);
    }
    IHS_BaseLog(base, IHS_LogLevelInfo, "Worker", "UDP worker initialized");
    IHS_UDPPacket recv;
    IHS_BufferInit(&recv.buffer, 2048, 2048);
    int receiveErrors = 0;
    for (;;) {
        IHS_BaseLock(base);
        bool interrupted = base->interrupted;
        IHS_BaseUnlock(base);
        if (interrupted)
            break;
        int ret;
        if ((ret = IHS_UDPSocketReceive(socket, &recv)) < 0) {
            /* This loop used to `break` on any negative return, which turns a single transient
             * receive error into a permanently deaf session: the thread leaves, every later inbound
             * packet is lost, while sends keep working because the socket stays open. On a TV where
             * both pthread condition variables and the SDL waits have already proved unreliable,
             * that failure would be silent. Log it (once per error value) and keep receiving. */
            receiveErrors++;
            if (receiveErrors == 1 || receiveErrors % 1000 == 0) {
                IHS_BaseLog(base, IHS_LogLevelWarn, "Worker",
                            "UDP receive returned %d (%d times so far); continuing", ret,
                            receiveErrors);
            }
            usleep(1000);
            continue;
        }
        if (ret) {
            receivedDatagrams++;
            base->callbacks.received(base, &recv.address, &recv.buffer);
        }
        IHS_BufferClear(&recv.buffer, false);
    }
    IHS_BufferClear(&recv.buffer, true);
    if (base->callbacks.run && base->callbacks.run->finalized) {
        base->callbacks.run->finalized(base, base->callbackContexts.run);
    }
    // Clear base->socket under the lock so any concurrent IHS_BaseSend that takes
    // the lock after this point sees NULL and bails. Then close the socket after
    // releasing the lock — at that point no in-flight send can still reference it
    // (each send takes the lock for its full duration).
    IHS_BaseLock(base);
    base->socket = NULL;
    IHS_BaseUnlock(base);
    IHS_UDPSocketClose(socket);
}
