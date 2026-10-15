#include "gamepad.h"
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "ihs_thread.h"
#include "hid/device.h"
#include "hid/manager.h"
#include "hid/provider.h"
#include "endianness.h"

// Generic RAW V2 follows the library's SDL provider, without an SDL3 dependency.
typedef struct { uint32_t present, kind; uint8_t wire[72]; } PadState;
typedef struct { IHS_HIDDevice base; int index; uint32_t generation; uint8_t previous[72]; bool sent; } PadDevice;
static PadState pads[2];
/* Filled by JavaScript on the browser thread (see TVLink.pollGamepads in app.js) and only read
 * here. Reading the pads used to be a MAIN_THREAD_EM_ASM proxy call from the main loop every 8 ms,
 * and on this TV that call parks the caller until the browser thread services it — with the video
 * pipeline keeping that thread busy, the input poll was delayed by an unpredictable amount, which is
 * what made the controller feel laggy. A plain memory read cannot be delayed. */
static struct { PadState pads[2]; atomic_uint sequence; } shared;
_Static_assert(offsetof(typeof(shared), sequence) == 160, "JS snapshot sequence layout");
EMSCRIPTEN_KEEPALIVE void *TVGamepadBuffer(void) { return shared.pads; }
static IHS_Mutex *padLock;
static const char *paths[] = { "browser://0", "browser://1" };
_Static_assert(sizeof(PadState) == 80, "JS gamepad layout");
bool TVGamepadPoll(void) {
    if (!padLock) padLock = IHS_MutexCreate();
    uint32_t sequence = atomic_load(&shared.sequence);
    if (sequence & 1) return false;
    PadState snapshot[2];
    memcpy(snapshot, shared.pads, sizeof(snapshot));
    if (atomic_load(&shared.sequence) != sequence) return false;
    bool changed = false;
    IHS_MutexLock(padLock);
    for (int i = 0; i < 2; i++)
        if (pads[i].present != snapshot[i].present || pads[i].kind != snapshot[i].kind) changed = true;
    memcpy(pads, snapshot, sizeof(pads));
    IHS_MutexUnlock(padLock);
    return changed;
}
int TVGamepadCount(void) { IHS_MutexLock(padLock); int n = !!pads[0].present + !!pads[1].present; IHS_MutexUnlock(padLock); return n; }
/* Only complete published snapshots reach the HID worker; JS does not hold padLock while writing. */
static PadState State(int index) {
    PadState state;
    IHS_MutexLock(padLock);
    state = pads[index];
    IHS_MutexUnlock(padLock);
    return state;
}
static IHS_HIDDevice *AllocDevice(const IHS_HIDDeviceClass *cls) { PadDevice *p = calloc(1, sizeof(*p)); if (p) p->base.cls = cls; return (IHS_HIDDevice *)p; }
static void FreeDevice(IHS_HIDDevice *d) { free(d); }
static void CloseDevice(IHS_HIDDevice *d) { (void)d; }
static int String(IHS_HIDDevice *d, IHS_Buffer *out) { (void)d; const uint8_t empty = 0; IHS_BufferWriteMem(out, 0, &empty, 1); return 0; }
static int Read(IHS_HIDDevice *d, IHS_Buffer *out, size_t length, uint32_t timeout) {
    (void)timeout; PadState state = State(((PadDevice *)d)->index); size_t n = length < 72 ? length : 72;
    /* The library attaches the payload to the response only when the device reports success, and
     * the response carries whatever was written into the buffer (control_hid.c HandleDeviceRead). */
    if (n) IHS_BufferWriteMem(out, 0, state.wire, n);
    return 0;
}
static int FeatureWrite(IHS_HIDDevice *d, const uint8_t *data, size_t n) { (void)d; (void)data; (void)n; return -1; }
static int Write(IHS_HIDDevice *d, const uint8_t *data, size_t n) {
    (void)d; if (!data || !n) return -1;
    // shortcut: standard browser gamepads provide buttons/axes; rumble and sensors need a platform extension.
    return (data[0] == 9 || data[0] == 11) && n >= 2 ? 0 : -1;
}
static int Feature(IHS_HIDDevice *d, const uint8_t *number, size_t numberLen, IHS_Buffer *out, size_t length) {
    if (!number || !numberLen) return -1;
    PadDevice *p = (PadDevice *)d; PadState state = State(p->index); uint8_t report[21] = {0}; size_t n;
    report[0] = number[0];
    if (number[0] == 4) { report[1] = 1; IHS_WriteUInt32LE(report + 3, state.kind == 45 ? 45 : 32); IHS_WriteUInt32LE(report + 7, p->index); n = 21; }
    else if (number[0] == 2) { report[1] = 255; n = 2; }
    else if (number[0] == 7) { report[1] = '0' + p->index; n = 3; }
    else return -1;
    if (length < n) return -1;
    IHS_BufferWriteMem(out, 0, report, n); return 0;
}
static int Report(IHS_HIDDevice *d, bool force) {
    PadDevice *p = (PadDevice *)d; PadState state = State(p->index);
    if (!state.present || state.present != p->generation) return -1;
    size_t n = d->managed->reportHolder.reportLength;
    if (!n) return 0;
    if (n > 72) n = 72;
    if (!force && p->sent && memcmp(p->previous, state.wire, n) == 0) return 0;
    if (force || !p->sent) IHS_HIDDeviceReportAddFullForced(d, state.wire, n);
    else IHS_HIDDeviceReportAddDelta(d, p->previous, state.wire, n);
    memcpy(p->previous, state.wire, 72); p->sent = true;
    uint16_t buttons; IHS_ReadUInt16LE(state.wire + 16, &buttons); bool active = buttons != 0;
    for (int i = 0; i < 6; i++) { int16_t value; IHS_ReadSInt16LE(state.wire + i * 2, &value); active |= i < 4 ? value >= 16384 || value < -16384 : value > 0; }
    d->managed->reportActivityKnown = true; d->managed->reportActiveInput |= active;
    return 1;
}
static int Start(IHS_HIDDevice *d, size_t n) { if (!n) return -1; return Report(d, true) < 0 ? -1 : 0; }
static int Full(IHS_HIDDevice *d) { return Report(d, true) < 0 ? -1 : 0; }
static int Poll(IHS_HIDDevice *d) { return Report(d, false); }
static int Disconnect(IHS_HIDDevice *d, int method, const uint8_t *data, size_t n) { (void)d; (void)method; (void)data; (void)n; return 0; }
static const IHS_HIDDeviceClass DeviceClass = {
    .alloc = AllocDevice, .free = FreeDevice, .close = CloseDevice, .write = Write, .read = Read,
    .sendFeatureReport = FeatureWrite, .getFeatureReport = Feature,
    .getVendorString = String, .getProductString = String, .getSerialNumberString = String,
    .startInputReports = Start, .requestFullReport = Full, .requestDisconnect = Disconnect, .poll = Poll
};
static IHS_HIDProvider *AllocProvider(const IHS_HIDProviderClass *cls) { IHS_HIDProvider *p = calloc(1, sizeof(*p)); if (p) p->cls = cls; return p; }
static void FreeProvider(IHS_HIDProvider *p) { free(p); }
static bool Supports(IHS_HIDProvider *p, const char *path) { (void)p; return path && (strcmp(path, paths[0]) == 0 || strcmp(path, paths[1]) == 0); }
static IHS_HIDDevice *Open(IHS_HIDProvider *p, const char *path) {
    if (!Supports(p, path)) return NULL;
    int index = strcmp(path, paths[1]) == 0;
    if (!State(index).present) return NULL;
    PadDevice *d = (PadDevice *)IHS_HIDDeviceCreate(&DeviceClass); if (d) { d->index = index; d->generation = State(index).present; } return (IHS_HIDDevice *)d;
}
static bool HasChange(IHS_HIDProvider *p) { (void)p; return false; }
static IHS_Enumeration *Enumerate(IHS_HIDProvider *p) {
    (void)p; int *indices = malloc(2 * sizeof(int)); if (!indices) return IHS_EnumerationEmptyCreate(); size_t n = 0;
    IHS_MutexLock(padLock); for (int i = 0; i < 2; i++) if (pads[i].present) indices[n++] = i; IHS_MutexUnlock(padLock);
    return IHS_EnumerationArrayCreate(indices, sizeof(int), n, free);
}
static void Info(IHS_HIDProvider *p, IHS_Enumeration *e, IHS_HIDDeviceInfo *info) {
    (void)p; int *index = e->cls->get(e); if (!index || *index < 0 || *index > 1) return;
    bool ps5 = State(*index).kind == 45;
    *info = (IHS_HIDDeviceInfo){ .path = paths[*index], .product_string = ps5 ? "PS5 Gamepad" : "Xbox Gamepad", .serial_number = paths[*index], .vendor_id = ps5 ? 0x054c : 0x045e, .product_id = ps5 ? 0x0ce6 : 0x02ea };
}
IHS_HIDProvider *TVGamepadProvider(void) {
    static const IHS_HIDProviderClass cls = { AllocProvider, FreeProvider, Supports, Open, HasChange, Enumerate, Info };
    return IHS_SessionHIDProviderCreate(&cls);
}
