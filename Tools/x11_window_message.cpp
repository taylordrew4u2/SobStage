// Sends a window the message a window manager sends it, for the e2e gates.
//
// The gates run the app under Xvfb with no window manager, so nothing can
// press a window's close button: `xdotool windowclose` destroys the window
// outright and SIGINT quits JUCE without asking. A quit through the close
// button -- the path that asks "Still saving..." and waits -- is reached only
// by the WM_DELETE_WINDOW message a window manager sends. WM_TAKE_FOCUS, the
// other one, gives a dialog the keyboard so `xdotool key` reaches it.
//
// JUCE looks the protocol atoms up once, at launch, without creating them, and
// a bare X server has none until a window manager makes them. So `atoms` is
// run first, before the app starts, to make them as a window manager would --
// against an Xvfb started with -noreset, or the server forgets them the
// moment this, its only client, disconnects.
//
//   x11_window_message atoms
//   x11_window_message <window id> close|focus
#include <X11/Xlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main (int argc, char** argv)
{
    const bool atomsOnly = argc == 2 && std::strcmp (argv[1], "atoms") == 0;

    if (! atomsOnly && (argc != 3 || (std::strcmp (argv[2], "close") != 0 && std::strcmp (argv[2], "focus") != 0)))
    {
        std::fprintf (stderr, "usage: x11_window_message atoms | <window id> close|focus\n");
        return 2;
    }

    Display* display = XOpenDisplay (nullptr);
    if (display == nullptr)
    {
        std::fprintf (stderr, "x11_window_message: no display\n");
        return 1;
    }

    const Atom protocols = XInternAtom (display, "WM_PROTOCOLS", False);
    const Atom deleteWindow = XInternAtom (display, "WM_DELETE_WINDOW", False);
    const Atom takeFocus = XInternAtom (display, "WM_TAKE_FOCUS", False);

    if (atomsOnly)
    {
        XSync (display, False);
        XCloseDisplay (display);
        return 0;
    }

    const auto window = static_cast<Window> (std::strtoul (argv[1], nullptr, 0));
    const bool close = std::strcmp (argv[2], "close") == 0;

    XEvent event {};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = protocols;
    event.xclient.format = 32;
    event.xclient.data.l[0] = static_cast<long> (close ? deleteWindow : takeFocus);
    event.xclient.data.l[1] = CurrentTime;

    const Status sent = XSendEvent (display, window, False, NoEventMask, &event);
    XSync (display, False);
    XCloseDisplay (display);
    return sent != 0 ? 0 : 1;
}
