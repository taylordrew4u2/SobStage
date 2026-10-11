#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mma {

/// Whether a take also produces one file per camera carrying the sound.
///
/// The picture and the sound are written separately and always have been, for
/// a reason worth keeping: the platform camera capture is video-only, the
/// microphones are the sound, and one clean track per person is the whole
/// point of the rig. A file with both in it is a *third* thing -- something to
/// review, send, or upload without an editor -- not a replacement for either.
///
/// So this never removes anything. The stems, the mix and the silent video all
/// stay exactly where they were; when this is on, a combined file appears
/// beside them.
enum class CombinedVideoMode
{
    Off,      ///< Today's behaviour: picture and sound, side by side.
    Combined  ///< Also write one video-with-sound file per camera.
};

/// One camera's part in the combining step.
struct CombinedTakeJob
{
    std::string videoFile;   ///< "V01_Kitchen-Cam.mov", relative to the session folder
    std::string audioFile;   ///< "MIX.wav", the summed mix with trims and the limiter
    std::string outputFile;  ///< "V01_Kitchen-Cam_with-sound.mov"

    /// The take's bit depth, carried through so the audio can be written at the
    /// depth it was recorded at rather than being resampled down to fit a
    /// codec's idea of enough.
    int audioBitDepth = 24;

    /// How far into the audio the video's first frame lands.
    ///
    /// The sound starts first: the writer thread and the stem files are opened
    /// before any camera is asked to record, so a camera's file begins some
    /// tens or hundreds of milliseconds into the take. Laying the two on top of
    /// each other without accounting for that puts the picture ahead of the
    /// sound by exactly that much -- small enough to look like nothing and
    /// large enough to look wrong, which is the worst size for a sync error.
    ///
    /// Trimming this much off the front of the audio is what lines them up.
    double audioLeadSeconds = 0.0;
};

/// Why no combined file can be made, in the app's voice. Empty means it can.
struct CombinedTakePlan
{
    std::vector<CombinedTakeJob> jobs;
    std::string problem;

    bool hasWork() const { return ! jobs.empty(); }
};

/// What one camera contributed to the take, as the plan needs to see it.
struct CombinedTakeInput
{
    std::string videoFile;          ///< file name with extension, or empty if it wrote nothing
    double videoStartOffsetSeconds; ///< how long after the audio's t=0 this camera began
};

/// §6.2 naming, applied to the combined file: the video's own name with a
/// suffix, so the folder still sorts the way it did and the origin of each
/// file is readable without opening it.
///
/// The container is chosen so that *neither* stream has to be re-encoded. The
/// picture is copied bit for bit whatever happens; the constraint that decides
/// the container is the audio, because the take is 24-bit PCM and the point is
/// to keep it that way.
///
///   .mov  ->  .mov   (what macOS writes. Carries PCM natively, so the sound
///                     arrives at the depth it was recorded at, and every
///                     editor opens it. mp4 was the previous answer and it is
///                     the wrong one here: its PCM support is an afterthought,
///                     so in practice audio in an mp4 is lossy.)
///   other ->  .mkv   (Windows writes .wmv, which mp4 cannot legally carry.
///                     Matroska takes any codec, video and FLAC alike, so the
///                     copy holds on both streams.)
std::string combinedFileNameFor (const std::string& videoFileName);

/// Every file one recording was written to, in the order it was written.
///
/// A take long enough to cross SessionWriter's 3.9 GB limit carries on in
/// "MIX_001.wav", "MIX_002.wav" and so on, and the mix is all of them end to
/// end -- not the first. The combined file used to be laid against the first
/// alone, so a long take's video stopped where that part did.
///
/// `namesInFolder` is what the take folder actually holds. The parts are
/// collected from _001 upward and stop at the first one missing, since a gap
/// means the ones after it are not a continuation of this recording. Empty
/// when `firstFileName` itself is not there.
///
/// Pure: the caller lists the folder.
std::vector<std::string> splitPartsInOrder (const std::string& firstFileName,
                                            const std::vector<std::string>& namesInFolder);

/// What a combine leaves free on the card beside its own file.
constexpr uint64_t kCombineHeadroomBytes = 1ull << 30;

/// Whether a combine may start: the card must hold the combined file and still
/// have kCombineHeadroomBytes to spare. `inputBytes` is the camera's file plus
/// every part of the mix -- neither stream is re-encoded, so the combined file
/// comes to about that.
///
/// A copy that ran a card dry failed at the end anyway, and until it did the
/// card read as full: Record said there was no room, or a take started then
/// ran out of it. Free space the system cannot report lets the combine try,
/// as it always has; a full card still only fails the combined file.
bool hasRoomToCombine (std::optional<uint64_t> availableBytes, uint64_t inputBytes) noexcept;

/// Builds the plan for a finished take.
///
/// `mixFileName` is the audio to lay under the picture -- the mix rather than a
/// stem, since that is the one already carrying the user's trims and the mix
/// bus limiter, and the one that sounds like the room rather than like one
/// person in it.
///
/// A camera that wrote nothing is skipped rather than failing the plan: one
/// camera failing to start must not cost the others their combined file. When
/// every camera is skipped the plan says so in one sentence.
///
/// Pure: no file system, no process, no platform. What can actually be run is
/// the muxer's business; what *should* be run is this.
CombinedTakePlan buildCombinedTakePlan (CombinedVideoMode mode,
                                        const std::vector<CombinedTakeInput>& cameras,
                                        const std::string& mixFileName,
                                        int bitDepth = 24);

} // namespace mma
