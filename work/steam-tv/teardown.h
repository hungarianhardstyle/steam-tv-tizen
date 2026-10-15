#pragma once
#include <stdatomic.h>
#include <unistd.h>
#include "ihslib.h"
#include "ihs_thread.h"
#include "hid/provider.h"

typedef struct {
    IHS_Client *client;
    IHS_Session *session;
    IHS_HIDProvider *provider;
    IHS_Thread *thread;
    atomic_int stage;
    atomic_bool done;
    atomic_int *logSending;
    void (*stopMedia)(void);
} TVTeardown;

static void TVTeardownWorker(void *context) {
    TVTeardown *job = context;
    atomic_store(&job->stage, 1);
    if (job->client) IHS_ClientStreamingCancel(job->client);
    // The unpublished session may still be used by a diagnostic call that already took its snapshot.
    while (job->logSending && atomic_load(job->logSending)) usleep(2000);
    atomic_store(&job->stage, 2);
    if (job->session) IHS_SessionDestroy(job->session);
    job->session = NULL;
    atomic_store(&job->stage, 3);
    if (job->provider) IHS_SessionHIDProviderDestroy(job->provider);
    job->provider = NULL;
    atomic_store(&job->stage, 4);
    job->stopMedia();
    atomic_store(&job->stage, 5);
    atomic_store_explicit(&job->done, true, memory_order_release);
}

static bool TVTeardownStart(TVTeardown *job) {
    atomic_store(&job->done, false);
    atomic_store(&job->stage, 0);
    job->thread = IHS_ThreadCreate(TVTeardownWorker, "TVTeardown", job);
    return job->thread != NULL;
}

static bool TVTeardownFinish(TVTeardown *job) {
    if (!job->thread || !atomic_load_explicit(&job->done, memory_order_acquire)) return false;
    IHS_ThreadJoin(job->thread);
    job->thread = NULL;
    return true;
}
