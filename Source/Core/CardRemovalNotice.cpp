#include "CardRemovalNotice.h"

namespace mma {

CardRemovalNotice CardRemovalNotice::build (const std::string& mirrorFolder, bool mirrorWasRunning)
{
    CardRemovalNotice notice;
    notice.aCompleteCopySurvives = mirrorWasRunning && ! mirrorFolder.empty();

    if (notice.aCompleteCopySurvives)
    {
        notice.survivingFolder = mirrorFolder;
        notice.message = "The drive you were recording to stopped responding, so the recording "
                         "has been stopped and every file closed. A complete backup copy is safe "
                         "on this computer: " + mirrorFolder;
    }
    else
    {
        // No copy to point at, so the sentence must not imply there is one.
        // §10.6: what happened, then what to do -- and the only thing to do
        // here is check the drive before recording again.
        notice.message = "The drive you were recording to stopped responding, so the recording "
                         "has been stopped and every file closed. Check that it is plugged in "
                         "properly before recording again, and keep the backup copy switched on.";
    }

    return notice;
}

CardRemovalNotice CardRemovalNotice::buildForTakeRecordedHere (const std::string& takeFolder)
{
    // Not the card -- the take never touched it -- and no backup copy: a take
    // recorded here has none. "Check that it is plugged in, keep the backup
    // switched on" sent the user to the one thing that was fine.
    CardRemovalNotice notice;
    notice.message = "This computer's drive stopped accepting writes, so the recording has been stopped and "
                     "every file closed."
                   + (takeFolder.empty() ? std::string() : " What was recorded is in " + takeFolder + ".")
                   + " Check this computer has room before recording again.";
    return notice;
}

} // namespace mma
