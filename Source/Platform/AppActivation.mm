#import <AppKit/AppKit.h>
#include "AppActivation.h"

namespace mma {

void revealAppForQuestion()
{
    NSApplication* app = [NSApplication sharedApplication];

    // Already in front: the question opens where the user is looking.
    if ([app isActive])
        return;

    // Hidden with Cmd-H: none of the app's windows is drawn, the question
    // included, until the app is unhidden.
    if ([app isHidden])
        [app unhideWithoutActivation];

    // Keeps bouncing until the app becomes active, which cancels it.
    [app requestUserAttention: NSCriticalRequest];
}

} // namespace mma
