#pragma once
#include <string.h>
#include "ihslib/common.h"

static inline bool TVDiagnosticEnabled(IHS_LogLevel level, int phase, const char *tag, const char *message) {
    if (level > IHS_LogLevelDebug) return false;
    if (phase < 12 || level <= IHS_LogLevelWarn) return true;
    return level < IHS_LogLevelDebug && strncmp(message, "Control packet:", 15) != 0 &&
           !(strcmp(tag, "HID") == 0 && strncmp(message, "Message ", 8) == 0);
}
