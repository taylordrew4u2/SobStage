#pragma once

namespace mma {

/// Makes sure a question the app is about to ask, or is already asking, can
/// be seen when the app is not the one in front.
///
/// Quit from the Dock, Log Out and Restart reach an app that is hidden
/// (Cmd-H) or behind another app's windows. Its question then opened where
/// nobody could see it -- not drawn at all while hidden -- and macOS had
/// already been told to wait, so the quit looked ignored and the obvious next
/// step was Force Quit in the middle of a take.
///
/// On macOS, when the app is not active: unhides it, and bounces its Dock
/// icon until it is. Activating it is asked for separately
/// (juce::Process::makeForegroundProcess); from macOS 14 that is only a
/// request the system may turn down, and the bouncing icon is what still
/// tells the user something is waiting. Becoming active stops the bounce.
/// Everywhere else this does nothing.
#if defined (__APPLE__)
void revealAppForQuestion();
#else
inline void revealAppForQuestion() {}
#endif

} // namespace mma
