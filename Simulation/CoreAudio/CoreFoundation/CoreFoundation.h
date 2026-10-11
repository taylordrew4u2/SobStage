#pragma once
// On macOS <CoreAudio/CoreAudio.h> brings CoreFoundation with it, so the CF
// stand-ins live in that shim. The backend also names this header itself, for
// CFAbsoluteTimeGetCurrent().
#include <CoreAudio/CoreAudio.h>
