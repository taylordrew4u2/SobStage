#pragma once
// The backend includes this for the host clock it times its IOProcs on, as the
// real SDK lets it; on macOS <CoreAudio/CoreAudio.h> includes it too, so the
// calls live there.
#include <CoreAudio/CoreAudio.h>
