#pragma once

#include "../Core/PermissionGuidance.h"

#include <string>

namespace mma {

/// The OS-level privacy answers PermissionGuidance turns into sentences.
///
/// Split deliberately into a pure mapping and a platform probe. The mapping is
/// compiled and unit-tested on every platform; only the probe around it is
/// conditional, so the part that decides what a status MEANS is never the part
/// that goes untested because the build host is the wrong OS.
namespace permissions {

/// AVAuthorizationStatus, as macOS defines it:
///   0 notDetermined, 1 restricted, 2 denied, 3 authorized.
///
/// Restricted maps to Denied on purpose. The app cannot capture either way,
/// and the honest failure is "you are not allowed to use the microphones",
/// even though a user under MDM restriction cannot act on the advice. Saying
/// nothing would leave them staring at working hardware that records silence.
///
/// An unrecognised status maps to NotApplicable — silent. A future enumerator
/// we cannot interpret is not evidence of a denial, and guessing "denied"
/// would put a blocking error in front of a user whose microphone works.
PermissionState fromAVAuthorizationStatus (long status) noexcept;

/// True for AVAuthorizationStatusRestricted (1): an administrator (MDM, a
/// configuration profile, Screen Time) has blocked access, so the user cannot
/// switch it on themselves. Only the wording differs from Denied; the app
/// behaves the same way for both.
bool isRestrictedAVAuthorizationStatus (long status) noexcept;

/// Windows stores per-capability consent under the CapabilityAccessManager
/// consent store as the string "Allow" or "Deny".
PermissionState fromWindowsConsentValue (const std::string& value) noexcept;

/// The microphone consent a plain desktop app like this one has on Windows:
/// the main switch ("Microphone access", the store's own Value) and, under it,
/// "Let desktop apps access your microphone" (its NonPackaged subkey's Value,
/// `desktopApps`, empty when it has none). Either one off is a refusal --
/// Initialize answers "access denied" -- and the master switch on with only
/// the desktop-apps switch off used to read as allowed, so the user was given
/// no permission guidance at all.
PermissionState fromWindowsMicrophoneConsent (const std::string& master, const std::string& desktopApps) noexcept;

/// The errno left by a failed attempt to write to the destination.
///
/// This is how a removable-volume denial actually presents: there is no query
/// API on macOS, so the only truthful answer comes from trying. EACCES and
/// EPERM mean we are not allowed; anything else (a missing directory, a full
/// card) is a different problem the app reports through its own path, so this
/// stays quiet rather than blaming privacy settings for a pulled card.
PermissionState fromWriteProbeErrno (int probeErrno) noexcept;

} // namespace permissions

/// Asks the OS whether this app may use the microphones.
///
/// Returns NotApplicable where the platform has no such concept (Linux), which
/// PermissionGuidance treats as silence rather than as permission.
PermissionState queryMicrophonePermission() noexcept;

/// macOS only: asks AVFoundation for microphone access when the answer is
/// still "not yet asked". Shows the system prompt if one is due; if the OS
/// already allows this app, it settles the status to Granted without a prompt.
/// Returns at once; the answer is read later by queryMicrophonePermission().
/// A no-op on other platforms, or when AVFoundation is not loaded.
void requestMicrophoneAccess() noexcept;

/// macOS only: the camera's AVFoundation status, as for the microphone.
/// NotApplicable elsewhere.
PermissionState queryCameraPermission() noexcept;

/// macOS only: true when access is Restricted (blocked by an administrator)
/// rather than refused by the user. False everywhere else.
bool isMicrophoneAccessRestricted() noexcept;
bool isCameraAccessRestricted() noexcept;

/// macOS only: shows the camera prompt if one is due. Returns at once; read
/// the answer later with queryCameraPermission().
void requestCameraAccess() noexcept;

/// Asks whether this app may write to `destinationPath`, by trying.
///
/// An empty path, or one that is not where the recording is going, returns
/// NotApplicable: this probe writes, so it must never run speculatively.
PermissionState queryVolumeWritePermission (const std::string& destinationPath) noexcept;

} // namespace mma
