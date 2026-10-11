#include "CombinedTakePlan.h"
#include <algorithm>
#include <cctype>   // std::tolower, which <algorithm> only happens to drag in
#include <cstdio>

namespace mma {

namespace {

std::string extensionOf (const std::string& fileName)
{
    const auto dot = fileName.find_last_of ('.');

    if (dot == std::string::npos)
        return {};

    std::string ext = fileName.substr (dot);
    std::transform (ext.begin(), ext.end(), ext.begin(),
                    [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return ext;
}

std::string stemOf (const std::string& fileName)
{
    const auto dot = fileName.find_last_of ('.');
    return dot == std::string::npos ? fileName : fileName.substr (0, dot);
}

} // namespace

bool hasRoomToCombine (std::optional<uint64_t> availableBytes, uint64_t inputBytes) noexcept
{
    if (! availableBytes.has_value())
        return true;

    // Saturating: a size near the top of the range must not wrap to "fits".
    const uint64_t needed = inputBytes > UINT64_MAX - kCombineHeadroomBytes
                                ? UINT64_MAX
                                : inputBytes + kCombineHeadroomBytes;
    return *availableBytes >= needed;
}

std::string combinedFileNameFor (const std::string& videoFileName)
{
    if (videoFileName.empty())
        return {};

    // The picture is copied, never re-encoded, so the container has to be one
    // that can legally carry whatever codec the camera chose. mp4 cannot hold
    // VC-1, which is what Windows writes; Matroska can hold anything.
    const auto container = extensionOf (videoFileName) == ".mov" ? ".mov" : ".mkv";

    return stemOf (videoFileName) + "_with-sound" + container;
}

std::vector<std::string> splitPartsInOrder (const std::string& firstFileName,
                                            const std::vector<std::string>& namesInFolder)
{
    const auto isThere = [&namesInFolder] (const std::string& name)
    {
        return std::find (namesInFolder.begin(), namesInFolder.end(), name) != namesInFolder.end();
    };

    if (! isThere (firstFileName))
        return {};

    std::vector<std::string> parts { firstFileName };

    // Named exactly as SessionWriter::makePathForSplit names them.
    const auto dot = firstFileName.find_last_of ('.');
    const auto stem = stemOf (firstFileName);
    const auto ext = dot == std::string::npos ? std::string() : firstFileName.substr (dot);

    for (int index = 1;; ++index)
    {
        char suffix[16];
        std::snprintf (suffix, sizeof (suffix), "_%03d", index);

        auto name = stem + suffix + ext;

        if (! isThere (name))
            break;

        parts.push_back (std::move (name));
    }

    return parts;
}

CombinedTakePlan buildCombinedTakePlan (CombinedVideoMode mode,
                                        const std::vector<CombinedTakeInput>& cameras,
                                        const std::string& mixFileName,
                                        int bitDepth)
{
    CombinedTakePlan plan;

    if (mode == CombinedVideoMode::Off)
        return plan; // not asked for; not a problem

    if (mixFileName.empty())
    {
        plan.problem = "The take has no mix file, so there is no sound to put with the picture.";
        return plan;
    }

    if (cameras.empty())
    {
        plan.problem = "No cameras were recording, so there is only sound to save.";
        return plan;
    }

    for (const auto& camera : cameras)
    {
        // A camera that never started wrote no file. Skipping it is the whole
        // of the handling: one camera failing must not cost the others theirs.
        if (camera.videoFile.empty())
            continue;

        CombinedTakeJob job;
        job.videoFile = camera.videoFile;
        job.audioFile = mixFileName;
        job.outputFile = combinedFileNameFor (camera.videoFile);
        job.audioBitDepth = bitDepth;

        // Never negative. A camera cannot start before the audio it is being
        // laid against -- the stems are opened first -- and a negative lead
        // would ask the muxer to trim time off the front of a file that does
        // not have it, which silently produces a file shorter than the take.
        job.audioLeadSeconds = std::max (0.0, camera.videoStartOffsetSeconds);

        plan.jobs.push_back (std::move (job));
    }

    if (plan.jobs.empty())
        plan.problem = "None of the cameras wrote a file, so there is nothing to combine.";

    return plan;
}

} // namespace mma
