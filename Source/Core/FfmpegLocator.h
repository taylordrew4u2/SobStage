#pragma once
#include <string>
#include <vector>

namespace mma {

enum class HostPlatform
{
    MacOS,
    Windows,
    Linux
};

/// Where to look for ffmpeg, best guess first.
///
/// PATH is not enough on macOS. A GUI app launched from Finder inherits the
/// launchd environment, not the one a login shell builds -- so Homebrew's
/// directories are missing, and an ffmpeg the user installed and can run in
/// Terminal is invisible to the app. Checking the two Homebrew prefixes
/// explicitly is what closes that gap, and the Apple-silicon prefix comes
/// first because that is what a machine bought in the last several years has.
///
/// Pure: it returns paths to try, it does not touch the file system. What is
/// actually there is the caller's business, which is what lets the ordering be
/// tested on a machine with no ffmpeg at all.
std::vector<std::string> ffmpegSearchPaths (HostPlatform platform);

/// The platform this build is for.
HostPlatform thisHostPlatform();

/// The combine's command line, run at the lowest disk priority on macOS.
///
/// ffmpeg copies a multi-gigabyte movie on the same card the next take is
/// recording to, and at normal priority it competed with that take's writer
/// -- ring fill, a fall back to mix-only, dropped video frames. taskpolicy
/// sets the policy and then execs ffmpeg in the same process, so killing it
/// and reading its exit code still act on ffmpeg itself. When nothing else is
/// writing it runs at full speed. Unchanged elsewhere.
std::vector<std::string> withLowDiskPriority (std::vector<std::string> args, HostPlatform platform);

/// True when `ffmpeg -version` printed this: it really is ffmpeg, and it
/// really runs here. A file that merely exists -- an Intel build migrated
/// onto an Apple-silicon Mac without Rosetta -- prints nothing and exits
/// with an error, and was accepted on existence alone, failing every take.
bool looksLikeFfmpegVersionOutput (const std::string& output);

/// How to get ffmpeg onto this kind of machine, in one sentence a
/// non-technical user can follow. Per platform because the one sentence that
/// existed told a Windows user to type a Mac command.
std::string ffmpegInstallHint (HostPlatform platform);

/// The line under "Also save video with the sound in one file" when ffmpeg
/// is missing: what it needs, that nothing is lost without it, and how to add
/// it on this platform.
std::string ffmpegMissingNote (HostPlatform platform);

} // namespace mma
