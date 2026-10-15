#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "ihslib.h"
const IHS_StreamVideoCallbacks *TVVideoCallbacks(void);
const IHS_StreamAudioCallbacks *TVAudioCallbacks(void);
/* Implemented by probe.c; called from the video control path when the host reports the capture
 * size, which is the client's first evidence that the host's stream pipeline exists. */
void TVHostStreamStarted(void);
/* Both are safe to call from any library thread: they only buffer data. JavaScript is called by the
 * main loop instead, because a MAIN_THREAD_EM_ASM proxied call from a library thread never returns
 * on this TV and froze the session receive path on 2026-10-14 10:56. */
void TVStatus(const char *text);
void TVRequestAwaitPicture(void);
void TVRequestMediaPrepare(void);
/* Filled by JavaScript: the video element's clock and the frames it actually presented. */
double TVVideoClockTime(void);
unsigned TVVideoClockPresented(void);
int TVVideoClockReady(void);
int TVVideoClockPaused(void);
/* Browser-reported presented-frame counter (requestVideoFrameCallback metadata.presentedFrames) and
 * the media time of the last presented frame. These are the closest thing to a rendered-frame count
 * this platform offers; see outputs/steam-tizen-measurements.md for what they do and do not prove. */
unsigned TVVideoClockPresentedFrames(void);
double TVVideoClockMediaTime(void);
/* JavaScript-side load indicators written into the shared buffer by app.js. */
unsigned TVVideoClockPadPolls(void);
unsigned TVVideoClockPadMaxGap(void);
/* Browser-thread cost of the main loop's JavaScript proxy calls (written by probe.c). */
unsigned TVVideoClockProxyUs(void);
unsigned TVVideoClockProxyMaxUs(void);
/* Pipeline preparation runs on the app main loop. */
int TVMediaPrepare(void);
void TVMediaPublish(IHS_Session *session);
/* Build version, defined once in probe.c; every measurement line carries it so a log can be tied to
 * the exact build that produced it. */
const char *TVVersion(void);
/* Cleanup worker only, after the session's media workers have been joined; SDK close proxies to the browser. */
void TVMediaStop(void);
/* Reject new submissions before joining session/media workers. */
void TVMediaBeginStop(void);
void TVMediaSelfCheck(void);
bool TVMediaFailed(void);
#ifdef __cplusplus
}
#endif
