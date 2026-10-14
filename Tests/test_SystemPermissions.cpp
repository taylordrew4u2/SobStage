#include "TestFramework.h"
#include "Platform/SystemPermissions.h"
#include "Core/PermissionGuidance.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace mma;

namespace {

// The mapping half of the probe is compiled everywhere, which is the whole
// reason it was split out: the Objective-C call around it can only run on
// macOS, but what an AVAuthorizationStatus MEANS is testable on any host.

TEST_CASE (SystemPermissions_AuthorizedIsTheOnlyGrant)
{
    REQUIRE (permissions::fromAVAuthorizationStatus (3) == PermissionState::Granted);
}

TEST_CASE (SystemPermissions_NotDeterminedIsNotADenial)
{
    // The OS prompt appears when we open the stream. Warning now would put an
    // error in front of a user who has not been asked anything yet.
    REQUIRE (permissions::fromAVAuthorizationStatus (0) == PermissionState::NotYetRequested);
    REQUIRE_FALSE (PermissionGuidance::blocksRecording (
        permissions::fromAVAuthorizationStatus (0)));
}

TEST_CASE (SystemPermissions_DeniedAndRestrictedBothBlock)
{
    // Restricted (MDM, parental controls) is not denied, but the app captures
    // nothing either way, so the user must be told rather than left watching
    // working hardware record silence.
    for (long status : { 1L, 2L })
    {
        REQUIRE (permissions::fromAVAuthorizationStatus (status) == PermissionState::Denied);
        REQUIRE (PermissionGuidance::blocksRecording (
            permissions::fromAVAuthorizationStatus (status)));
    }
}

TEST_CASE (SystemPermissions_OnlyRestrictedReadsAsRestricted)
{
    // Restricted changes only the wording (ask the administrator), so it must
    // be told apart from the user's own refusal and from everything else.
    REQUIRE (permissions::isRestrictedAVAuthorizationStatus (1));
    for (long status : { 0L, 2L, 3L, 4L, -1L })
        REQUIRE_FALSE (permissions::isRestrictedAVAuthorizationStatus (status));

    // Off the Mac there is no administrator policy to report.
   #if ! defined (__APPLE__)
    REQUIRE_FALSE (isMicrophoneAccessRestricted());
    REQUIRE_FALSE (isCameraAccessRestricted());
   #endif
}

TEST_CASE (SystemPermissions_AnUnknownStatusIsSilentNotBlocking)
{
    // A future enumerator we cannot interpret is not evidence of a denial.
    // Guessing "denied" would disable recording for a user whose mic works.
    REQUIRE (permissions::fromAVAuthorizationStatus (99) == PermissionState::NotApplicable);
    REQUIRE_FALSE (PermissionGuidance::blocksRecording (
        permissions::fromAVAuthorizationStatus (99)));
}

TEST_CASE (SystemPermissions_WindowsConsentStrings)
{
    REQUIRE (permissions::fromWindowsConsentValue ("Allow") == PermissionState::Granted);
    REQUIRE (permissions::fromWindowsConsentValue ("Deny") == PermissionState::Denied);
    // Never asked yet reads the same as macOS's notDetermined.
    REQUIRE (permissions::fromWindowsConsentValue ("") == PermissionState::NotYetRequested);
    // Anything else is a value we do not understand, so we stay quiet.
    REQUIRE (permissions::fromWindowsConsentValue ("Prompt") == PermissionState::NotApplicable);
}

TEST_CASE (SystemPermissions_WindowsDesktopAppsSwitchOffIsADenial)
{
    // "Let desktop apps access your microphone" off, the main switch on: a
    // desktop app is refused (Initialize answers 0x80070005), and must be told
    // so rather than read as allowed.
    REQUIRE (permissions::fromWindowsMicrophoneConsent ("Allow", "Deny") == PermissionState::Denied);
    REQUIRE (permissions::fromWindowsMicrophoneConsent ("Deny", "Allow") == PermissionState::Denied);
    REQUIRE (permissions::fromWindowsMicrophoneConsent ("Allow", "Allow") == PermissionState::Granted);
    // No desktop-apps value written: the main switch decides, as before.
    REQUIRE (permissions::fromWindowsMicrophoneConsent ("Allow", "") == PermissionState::Granted);
    REQUIRE (permissions::fromWindowsMicrophoneConsent ("", "") == PermissionState::NotYetRequested);
    REQUIRE (permissions::fromWindowsMicrophoneConsent ("", "Deny") == PermissionState::Denied);
}

TEST_CASE (SystemPermissions_OnlyAccessErrnosMeanDenied)
{
    REQUIRE (permissions::fromWriteProbeErrno (0) == PermissionState::Granted);
    REQUIRE (permissions::fromWriteProbeErrno (EACCES) == PermissionState::Denied);
    REQUIRE (permissions::fromWriteProbeErrno (EPERM) == PermissionState::Denied);

    // A pulled card (ENOENT) or a full one (ENOSPC) is a real problem, but the
    // app reports each of those through its own path. Blaming privacy settings
    // for them would send the user to the wrong settings pane.
    REQUIRE (permissions::fromWriteProbeErrno (ENOENT) == PermissionState::NotApplicable);
    REQUIRE (permissions::fromWriteProbeErrno (ENOSPC) == PermissionState::NotApplicable);
}

// --- The probe itself -------------------------------------------------------

std::filesystem::path scratch (const char* leaf)
{
    auto dir = std::filesystem::temp_directory_path() / ("mma-perm-" + std::string (leaf));
    std::error_code ec;
    std::filesystem::remove_all (dir, ec);
    std::filesystem::create_directories (dir, ec);
    return dir;
}

TEST_CASE (SystemPermissions_AWritableDestinationReadsAsGranted)
{
    const auto dir = scratch ("writable");
    REQUIRE (queryVolumeWritePermission (dir.string()) == PermissionState::Granted);

    // And it leaves nothing behind: this probe runs whenever the user picks a
    // save location, so a stray file would end up in every session folder.
    int entries = 0;
    for (const auto& e : std::filesystem::directory_iterator (dir))
    {
        (void) e;
        ++entries;
    }
    REQUIRE (entries == 0);

    std::error_code ec;
    std::filesystem::remove_all (dir, ec);
}

// The live denial probe is POSIX-only, and deliberately so.
//
// Windows has no mode bits for this: a directory the CI account cannot write
// to needs a DENY ACE, and that account may be elevated anyway, so anything
// arranged here would be a test that passes without ever seeing a denial.
// Excluded at compile time, where it is visible in the source, rather than
// returned early at runtime where it would print PASS. fromWriteProbeErrno
// above still covers the mapping on Windows; only the live probe is skipped.
#if !defined(_WIN32)

/// Returns a directory this process genuinely cannot create a file in, or an
/// empty path when the host offers none.
///
/// The obvious version of this -- chmod a scratch directory to r-x -- passes
/// vacuously under root, which is exactly how CI runs. Under root the mode
/// bits are ignored and the probe succeeds, so the assertion never gets to
/// see a denial and the test agrees with a broken implementation. /sys is
/// refused with EACCES even to root, which is the property this needs.
std::string somewhereWeCannotWrite()
{
    std::error_code ec;

    if (geteuid() != 0)
    {
        static const auto dir = scratch ("unwritable");
        std::filesystem::permissions (dir, std::filesystem::perms::owner_read
                                         | std::filesystem::perms::owner_exec,
                                      std::filesystem::perm_options::replace, ec);
        return dir.string();
    }

    if (std::filesystem::is_directory ("/sys", ec) && ! ec)
        return "/sys";

    return {};
}

TEST_CASE (SystemPermissions_AnUnwritableDestinationReadsAsDenied)
{
    const auto unwritable = somewhereWeCannotWrite();

    // Not silently skipped. A host where this cannot be arranged must say so
    // rather than report a pass it did not earn.
    if (unwritable.empty())
    {
        std::printf ("  (no unwritable directory available on this host)\n");
        REQUIRE (false);
        return;
    }

    REQUIRE (queryVolumeWritePermission (unwritable) == PermissionState::Denied);

    const auto problems = PermissionGuidance::evaluate (PermissionState::Granted,
                                                        PermissionState::Denied, true);
    REQUIRE (problems.size() == 1);
    // §10.4 wants this said, but it must not take the record button away: the
    // user may still be recording somewhere else entirely.
    REQUIRE_FALSE (problems[0].blocksRecording);
}

#endif // !_WIN32

TEST_CASE (SystemPermissions_NoDestinationIsNotProbedAtAll)
{
    // The probe writes. It must never run speculatively.
    REQUIRE (queryVolumeWritePermission ("") == PermissionState::NotApplicable);
    REQUIRE (queryVolumeWritePermission ("/definitely/not/a/directory/here")
             == PermissionState::NotApplicable);
}

} // namespace
