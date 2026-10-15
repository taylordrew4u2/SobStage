#include "TestFramework.h"
#include "Core/FfmpegLocator.h"

using namespace mma;

namespace {

bool hasSeparator (const std::string& path)
{
    return path.find ('/') != std::string::npos || path.find ('\\') != std::string::npos;
}

} // namespace

TEST_CASE (FfmpegLocator_MacChecksBothHomebrewPrefixesBeforePath)
{
    // A Finder launch does not inherit a login shell's PATH, so an ffmpeg the
    // user installed with Homebrew is only found by its absolute location.
    const auto paths = ffmpegSearchPaths (HostPlatform::MacOS);
    REQUIRE (paths.size() >= 3);
    REQUIRE (paths[0] == "/opt/homebrew/bin/ffmpeg");
    REQUIRE (paths[1] == "/usr/local/bin/ffmpeg");
    REQUIRE (paths.back() == "ffmpeg");
}

TEST_CASE (FfmpegLocator_EveryPlatformEndsWithABareNameForPath)
{
    // TakeCombiner checks anything with a separator on disk without running
    // it, and runs a bare name to let PATH answer. Exactly one bare name, last,
    // keeps that from spawning a process for every candidate.
    for (auto platform : { HostPlatform::MacOS, HostPlatform::Windows, HostPlatform::Linux })
    {
        const auto paths = ffmpegSearchPaths (platform);
        REQUIRE (! paths.empty());
        REQUIRE (! hasSeparator (paths.back()));

        for (size_t i = 0; i + 1 < paths.size(); ++i)
            REQUIRE (hasSeparator (paths[i]));
    }
}

TEST_CASE (FfmpegLocator_WindowsLooksForTheExe)
{
    for (const auto& path : ffmpegSearchPaths (HostPlatform::Windows))
        REQUIRE (path.size() > 4 && path.substr (path.size() - 4) == ".exe");
}

TEST_CASE (FfmpegLocator_KnowsWhichPlatformItWasBuiltFor)
{
#if defined (__APPLE__)
    REQUIRE (thisHostPlatform() == HostPlatform::MacOS);
#elif defined (_WIN32)
    REQUIRE (thisHostPlatform() == HostPlatform::Windows);
#else
    REQUIRE (thisHostPlatform() == HostPlatform::Linux);
#endif
}

TEST_CASE (FfmpegLocator_TheMacCombineRunsAtTheLowestDiskPriority)
{
    const std::vector<std::string> args { "/opt/homebrew/bin/ffmpeg", "-i", "in.mov" };

    const auto mac = withLowDiskPriority (args, HostPlatform::MacOS);
    REQUIRE (mac.size() == args.size() + 3);
    REQUIRE (mac[0] == std::string ("/usr/sbin/taskpolicy"));
    REQUIRE (mac[1] == std::string ("-d"));
    REQUIRE (mac[2] == std::string ("throttle"));
    REQUIRE (mac[3] == args[0]);

    REQUIRE (withLowDiskPriority (args, HostPlatform::Linux) == args);
    REQUIRE (withLowDiskPriority (args, HostPlatform::Windows) == args);
}

TEST_CASE (FfmpegLocator_OnlyAnFfmpegThatRunsIsAccepted)
{
    REQUIRE (looksLikeFfmpegVersionOutput ("ffmpeg version 7.1 Copyright (c) 2000-2024\n"));
    REQUIRE (looksLikeFfmpegVersionOutput ("\nffmpeg version n6.0"));
    REQUIRE_FALSE (looksLikeFfmpegVersionOutput (""));
    REQUIRE_FALSE (looksLikeFfmpegVersionOutput ("Bad CPU type in executable"));
    REQUIRE_FALSE (looksLikeFfmpegVersionOutput ("ffprobe version 7.1"));
}

TEST_CASE (FfmpegMissingNote_WindowsNeverSaysMac)
{
    // The one note there was told a Windows user "On a Mac: brew install
    // ffmpeg". Each platform's note names where ffmpeg goes on that platform,
    // and the Windows one is the first place ffmpegSearchPaths looks.
    const auto windows = ffmpegMissingNote (HostPlatform::Windows);
    REQUIRE (windows.find ("C:\\Program Files\\ffmpeg") != std::string::npos);
    REQUIRE (windows.find ("bin") != std::string::npos);
    REQUIRE (windows.find ("brew") == std::string::npos);
    REQUIRE (windows.find ("Mac") == std::string::npos);
    REQUIRE (ffmpegSearchPaths (HostPlatform::Windows).front().rfind ("C:\\Program Files\\ffmpeg\\bin\\", 0) == 0);

    // Nothing is lost without it, said before the instructions.
    REQUIRE (windows.find ("separate files") != std::string::npos);

    const auto onLinux = ffmpegMissingNote (HostPlatform::Linux);
    REQUIRE (onLinux.find ("package manager") != std::string::npos);
    REQUIRE (onLinux.find ("brew") == std::string::npos);
    REQUIRE (onLinux.find ("Program Files") == std::string::npos);

    REQUIRE (ffmpegMissingNote (HostPlatform::MacOS).find ("brew install ffmpeg") != std::string::npos);
    REQUIRE (ffmpegInstallHint (HostPlatform::Windows).find ("brew") == std::string::npos);
}
