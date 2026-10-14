#include "SystemPermissions.h"
#include "../Core/Utf8Path.h"

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <atomic>
#include <cstring>
#include <system_error>

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mma {
namespace permissions {

PermissionState fromAVAuthorizationStatus (long status) noexcept
{
    switch (status)
    {
        case 0:  return PermissionState::NotYetRequested;
        case 1:  return PermissionState::Denied;   // restricted; see the header
        case 2:  return PermissionState::Denied;
        case 3:  return PermissionState::Granted;
        default: return PermissionState::NotApplicable;
    }
}

bool isRestrictedAVAuthorizationStatus (long status) noexcept
{
    return status == 1;
}

PermissionState fromWindowsConsentValue (const std::string& value) noexcept
{
    if (value == "Allow")
        return PermissionState::Granted;

    if (value == "Deny")
        return PermissionState::Denied;

    // No value written yet means the user has never been asked, which is the
    // same position as macOS's notDetermined: the prompt comes when we open.
    return value.empty() ? PermissionState::NotYetRequested
                         : PermissionState::NotApplicable;
}

PermissionState fromWindowsMicrophoneConsent (const std::string& master, const std::string& desktopApps) noexcept
{
    const auto overall = fromWindowsConsentValue (master);

    if (overall == PermissionState::Denied || fromWindowsConsentValue (desktopApps) != PermissionState::Denied)
        return overall;

    return PermissionState::Denied;
}

PermissionState fromWriteProbeErrno (int probeErrno) noexcept
{
    if (probeErrno == 0)
        return PermissionState::Granted;

    if (probeErrno == EACCES || probeErrno == EPERM)
        return PermissionState::Denied;

    return PermissionState::NotApplicable;
}

} // namespace permissions

namespace
{
void requestAVAccess (const char* mediaTypeCode) noexcept
{
#if defined(__APPLE__) && defined(__BLOCKS__)
    // +[AVCaptureDevice requestAccessForMediaType: <media type>
    //                           completionHandler: ^(BOOL granted) {}]
    // The handler runs on an arbitrary queue, so it does nothing: the caller
    // polls the status on the message thread instead.
    const auto deviceClass = reinterpret_cast<id> (objc_getClass ("AVCaptureDevice"));
    const auto stringClass = reinterpret_cast<id> (objc_getClass ("NSString"));
    if (deviceClass == nullptr || stringClass == nullptr)
        return;

    const auto selector = sel_registerName ("requestAccessForMediaType:completionHandler:");
    using SendBoolAndSelector = signed char (*) (id, SEL, SEL);
    if (! reinterpret_cast<SendBoolAndSelector> (objc_msgSend) (
            deviceClass, sel_registerName ("respondsToSelector:"), selector))
        return;

    using SendStringFromUtf8 = id (*) (id, SEL, const char*);
    const auto mediaType = reinterpret_cast<SendStringFromUtf8> (objc_msgSend) (
        stringClass, sel_registerName ("stringWithUTF8String:"), mediaTypeCode);
    if (mediaType == nullptr)
        return;

    void (^handler) (signed char) = ^(signed char) {};
    using SendRequest = void (*) (id, SEL, id, void (^) (signed char));
    reinterpret_cast<SendRequest> (objc_msgSend) (deviceClass, selector, mediaType, handler);
#else
    (void) mediaTypeCode;
#endif
}

#if defined(__APPLE__)
/// The raw AVAuthorizationStatus, or -1 when it cannot be read.
long readAVAuthorizationStatus (const char* mediaTypeCode) noexcept
{
    // Application.cpp is C++, not Objective-C++, and SystemThermalState.cpp
    // already reaches a small public AppKit surface through the Objective-C
    // runtime rather than converting a translation unit for one enum. This
    // follows that, calling
    //   +[AVCaptureDevice authorizationStatusForMediaType: <media type>]
    // which is documented as safe to call at any time and never prompts.
    const auto deviceClass = reinterpret_cast<id> (objc_getClass ("AVCaptureDevice"));
    if (deviceClass == nullptr)
        return -1;

    const auto selector = sel_registerName ("authorizationStatusForMediaType:");
    const auto respondsSelector = sel_registerName ("respondsToSelector:");
    using SendBoolAndSelector = signed char (*) (id, SEL, SEL);

    if (! reinterpret_cast<SendBoolAndSelector> (objc_msgSend) (
            deviceClass, respondsSelector, selector))
        return -1;

    // AVMediaTypeAudio/Video are the NSString constants @"soun"/@"vide". Building it here
    // avoids linking AVFoundation just to read one exported symbol.
    const auto stringClass = reinterpret_cast<id> (objc_getClass ("NSString"));
    if (stringClass == nullptr)
        return -1;

    using SendStringFromUtf8 = id (*) (id, SEL, const char*);
    const auto mediaType = reinterpret_cast<SendStringFromUtf8> (objc_msgSend) (
        stringClass, sel_registerName ("stringWithUTF8String:"), mediaTypeCode);
    if (mediaType == nullptr)
        return -1;

    using SendIntegerWithObject = long (*) (id, SEL, id);
    return reinterpret_cast<SendIntegerWithObject> (objc_msgSend) (deviceClass, selector, mediaType);
}

PermissionState queryAVPermission (const char* mediaTypeCode) noexcept
{
    const auto status = readAVAuthorizationStatus (mediaTypeCode);
    return status < 0 ? PermissionState::NotApplicable
                      : permissions::fromAVAuthorizationStatus (status);
}

#endif
} // namespace

void requestMicrophoneAccess() noexcept
{
    // The CoreAudio streams raise their own prompt, but macOS 27 was seen
    // reporting "not yet asked" through AVFoundation while those streams were
    // already live, and the status never changed. Asking through the same API
    // whose status the app reads settles it either way; the 2 s permission
    // poll reads the result.
    requestAVAccess ("soun"); // AVMediaTypeAudio
}

void requestCameraAccess() noexcept
{
    // CameraController::applyPendingCameraList() reads the answer.
    requestAVAccess ("vide"); // AVMediaTypeVideo
}

bool isMicrophoneAccessRestricted() noexcept
{
#if defined(__APPLE__)
    return permissions::isRestrictedAVAuthorizationStatus (readAVAuthorizationStatus ("soun"));
#else
    return false;
#endif
}

bool isCameraAccessRestricted() noexcept
{
#if defined(__APPLE__)
    return permissions::isRestrictedAVAuthorizationStatus (readAVAuthorizationStatus ("vide"));
#else
    return false;
#endif
}

PermissionState queryCameraPermission() noexcept
{
#if defined(__APPLE__)
    return queryAVPermission ("vide");
#else
    // Windows asks when the camera opens and JUCE reports a refusal as a
    // failed open; Linux has no camera backend. Nothing to gate here.
    return PermissionState::NotApplicable;
#endif
}

PermissionState queryMicrophonePermission() noexcept
{
#if defined(__APPLE__)
    return queryAVPermission ("soun");

#elif defined(_WIN32)
    // The CapabilityAccessManager consent store. WinRT's AppCapability API is
    // the documented route, but it is packaged-app only; a plain desktop build
    // reads the same state the Settings app writes.
    // Both switches: the main one, and "Let desktop apps access your
    // microphone" under it (the NonPackaged subkey), which is the one a plain
    // desktop app like this needs too.
    const auto readConsent = [] (const char* path, bool& found)
    {
        found = false;
        HKEY key {};

        if (RegOpenKeyExA (HKEY_CURRENT_USER, path, 0, KEY_READ, &key) != ERROR_SUCCESS)
            return std::string();

        char value[32] {};
        DWORD size = sizeof (value);
        DWORD type = 0;
        const auto result = RegQueryValueExA (key, "Value", nullptr, &type,
                                              reinterpret_cast<LPBYTE> (value), &size);
        RegCloseKey (key);

        if (result != ERROR_SUCCESS || type != REG_SZ)
            return std::string();

        found = true;
        return std::string (value, strnlen (value, sizeof (value)));
    };

    bool masterFound = false, desktopFound = false;
    const auto master = readConsent (
        "Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone",
        masterFound);
    const auto desktopApps = readConsent (
        "Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone\\NonPackaged",
        desktopFound);

    if (! masterFound)
        return desktopFound && permissions::fromWindowsConsentValue (desktopApps) == PermissionState::Denied
            ? PermissionState::Denied
            : PermissionState::NotApplicable;

    return permissions::fromWindowsMicrophoneConsent (master, desktopFound ? desktopApps : std::string());

#else
    // Linux has no per-application microphone consent to read. NotApplicable,
    // not Granted: we have no evidence either way, and PermissionGuidance is
    // silent on both — but only one of them is honest.
    return PermissionState::NotApplicable;
#endif
}

PermissionState queryVolumeWritePermission (const std::string& destinationPath) noexcept
{
    if (destinationPath.empty())
        return PermissionState::NotApplicable;

    std::error_code ec;
    if (! std::filesystem::is_directory (pathFromUtf8 (destinationPath), ec) || ec)
        return PermissionState::NotApplicable;

    // A fixed name would collide between two copies of the app probing at
    // once; the point is to learn whether the OS lets us create anything here.
    static std::atomic<unsigned> counter { 0 };
    const auto probe = pathFromUtf8 (destinationPath)
                     / (".sobstage-access-probe-" + std::to_string (counter.fetch_add (1)));

    errno = 0;
    // _wfopen on Windows: probe.string() there is the ANSI code page, which
    // cannot name a folder like C:\Users\Zoë and throws on some that it can't.
#if defined(_WIN32)
    std::FILE* handle = _wfopen (probe.c_str(), L"wb");
#else
    std::FILE* handle = std::fopen (probe.c_str(), "wb");
#endif
    const int openErrno = (handle != nullptr) ? 0 : errno;

    if (handle != nullptr)
    {
        std::fclose (handle);
        std::error_code ignored;
        std::filesystem::remove (probe, ignored);
    }

    return permissions::fromWriteProbeErrno (openErrno);
}

} // namespace mma
