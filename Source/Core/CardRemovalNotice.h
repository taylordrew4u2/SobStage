#pragma once
#include <string>

namespace mma {

/// §6.5 "target card removed": what to say when the destination stops
/// accepting writes in the middle of a take.
///
/// The spec is unusually specific here, and for good reason -- this is the one
/// mid-take failure where the user's recording may already be gone: "Stop
/// immediately, finalize every open file, alert loudly. If the mirror is
/// running, state that a complete copy survives locally and give its path."
///
/// The path is the whole point of the second sentence. A user told only that
/// the card failed has lost their take as far as they know; a user given the
/// folder that still holds it has had an accident rather than a disaster, which
/// is exactly what §6.3 built the mirror for.
struct CardRemovalNotice
{
    /// True when a complete second copy survives and can be pointed at.
    bool aCompleteCopySurvives = false;

    /// Where that copy is. Empty when none survives.
    std::string survivingFolder;

    /// §10.6: what happened, then what to do, in plain language and no codes.
    std::string message;

    /// `mirrorFolder` is §6.3's local copy; `mirrorWasRunning` is whether it was
    /// still being written at the moment the card went, since a mirror that
    /// stopped earlier in the take is not a complete copy and must never be
    /// offered as one -- a copy with a hole in it is worse than no copy,
    /// because the user will trust it.
    static CardRemovalNotice build (const std::string& mirrorFolder, bool mirrorWasRunning);

    /// The same, for a take recorded on this computer to be copied to a card
    /// afterwards: it is this computer's drive that refused, the card was never
    /// written, and there is no backup copy to keep switched on. Says where the
    /// take's files are (`takeFolder`).
    static CardRemovalNotice buildForTakeRecordedHere (const std::string& takeFolder);
};

} // namespace mma
