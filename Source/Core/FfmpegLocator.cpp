#include "FfmpegLocator.h"

namespace mma {

HostPlatform thisHostPlatform()
{
#if defined (__APPLE__)
    return HostPlatform::MacOS;
#elif defined (_WIN32)
    return HostPlatform::Windows;
#else
    return HostPlatform::Linux;
#endif
}

std::vector<std::string> ffmpegSearchPaths (HostPlatform platform)
{
    switch (platform)
    {
        case HostPlatform::MacOS:
            return {
                "/opt/homebrew/bin/ffmpeg", // Apple silicon Homebrew
                "/usr/local/bin/ffmpeg",    // Intel Homebrew, and most manual installs
                "/opt/local/bin/ffmpeg",    // MacPorts
                "ffmpeg"                    // and PATH, for a launch from a shell
            };

        case HostPlatform::Windows:
            return {
                "C:\\Program Files\\ffmpeg\\bin\\ffmpeg.exe",
                "C:\\ffmpeg\\bin\\ffmpeg.exe",
                "ffmpeg.exe"
            };

        case HostPlatform::Linux:
        default:
            return { "/usr/bin/ffmpeg", "/usr/local/bin/ffmpeg", "ffmpeg" };
    }
}

std::vector<std::string> withLowDiskPriority (std::vector<std::string> args, HostPlatform platform)
{
    if (platform != HostPlatform::MacOS || args.empty())
        return args;

    std::vector<std::string> wrapped { "/usr/sbin/taskpolicy", "-d", "throttle" };
    wrapped.insert (wrapped.end(), args.begin(), args.end());
    return wrapped;
}

bool looksLikeFfmpegVersionOutput (const std::string& output)
{
    const std::string expected = "ffmpeg version";
    size_t start = 0;
    while (start < output.size() && (output[start] == ' ' || output[start] == '\n' || output[start] == '\r'))
        ++start;
    return output.compare (start, expected.size(), expected) == 0;
}

std::string ffmpegInstallHint (HostPlatform platform)
{
    switch (platform)
    {
        case HostPlatform::MacOS:
            return "To add it, type brew install ffmpeg in Terminal (this needs Homebrew, from brew.sh).";

        // The first place ffmpegSearchPaths looks, so following this sentence
        // is enough for the app to find it without touching PATH.
        case HostPlatform::Windows:
            return "To add it, put ffmpeg in C:\\Program Files\\ffmpeg (ffmpeg.exe goes in its bin folder).";

        case HostPlatform::Linux:
        default:
            return "Install ffmpeg with your package manager.";
    }
}

std::string ffmpegMissingNote (HostPlatform platform)
{
    // §10.6: named before a take rather than discovered after one, and said
    // with what is still safe, so it does not read like a failed recording.
    return "Combined video needs ffmpeg, which isn't installed. Your picture and sound are "
           "still recorded, as separate files. " + ffmpegInstallHint (platform);
}

} // namespace mma
