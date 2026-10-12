#pragma once

#include <string>

namespace mma {

/// Keeps hold of a take's folder itself, so where it is now can be asked after
/// the take's audio files have closed.
///
/// The audio writers already follow a folder renamed or moved in Finder
/// mid-take, by asking their open MIX.wav where it is (SessionWriter's
/// resolveCurrentFilePath). That answer ends when the audio stops -- but the
/// cameras' movies are finished asynchronously AFTER that, and the stop-time
/// session.json, the saved-take card's file list and the combined video are all
/// produced only once they are. A rename in that window left all three looking
/// in a folder that no longer existed. This holds a descriptor on the folder
/// for the life of the take so the question can still be answered then.
///
/// macOS: O_EVTONLY, which never keeps a card from ejecting, and F_GETPATH.
/// Linux: O_PATH and /proc/self/fd. Elsewhere it holds nothing and answers
/// nothing (a Windows folder with open files cannot be renamed anyway).
class TakeFolderTracker
{
public:
    TakeFolderTracker() = default;
    ~TakeFolderTracker();

    TakeFolderTracker (const TakeFolderTracker&) = delete;
    TakeFolderTracker& operator= (const TakeFolderTracker&) = delete;

    /// Starts tracking `folder` (UTF-8), letting go of any earlier one. False
    /// when it could not be held, in which case resolve() answers nothing.
    bool open (const std::string& folder);
    void close();

    /// The two halves of open(), for a caller that must do the filesystem
    /// call on a thread it can walk away from: openDescriptor() there (-1 on
    /// failure), adopt() back on the owning thread. adopt() takes ownership of
    /// the descriptor, and of nothing when it is -1.
    static int openDescriptor (const std::string& folder);
    static void closeDescriptor (int fd);
    bool adopt (int fd, const std::string& folder);
    bool isOpen() const noexcept { return descriptor >= 0; }

    /// A second descriptor on the same folder, to adopt() into a tracker of
    /// its own: for work that goes on after this one lets go -- the combined
    /// video and the podcast copy run for minutes after the take, and the
    /// folder can be renamed under them too. -1 when this holds none. Only
    /// the process's own descriptor table is touched, never the card, so it
    /// is safe on the message thread.
    int duplicateDescriptor() const noexcept;

    /// Where the folder is now. The spelling it was opened with while it has
    /// not moved (the system answers with resolved paths, and /tmp on a Mac is
    /// really /private/tmp); the new path once it has. Empty when unknown: not
    /// open, deleted, or a platform that cannot say.
    ///
    /// Asks only the descriptor while the answer is the one already known, so
    /// a card that has stopped answering is not stat'ed from the caller's
    /// thread on every call.
    std::string resolve() const;

private:
    std::string currentSystemPath() const;

    int descriptor = -1;
    std::string openedPath;
    mutable std::string lastSystemPath;
    mutable std::string lastResolved;
};

} // namespace mma
