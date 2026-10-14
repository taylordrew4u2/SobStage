#include "TakeCompleteness.h"

#include <algorithm>
#include <cctype>

namespace mma {

namespace {

bool isMetadata (const std::string& name)
{
    // Case-insensitive suffix test: the writer produces "session.json", but a
    // folder is a folder and nothing stops one holding "SESSION.JSON".
    const std::string suffix = ".json";

    if (name.size() < suffix.size())
        return false;

    return std::equal (suffix.rbegin(), suffix.rend(), name.rbegin(),
                       [] (char a, char b)
                       {
                           return std::tolower (static_cast<unsigned char> (a))
                                  == std::tolower (static_cast<unsigned char> (b));
                       });
}

} // namespace

bool isSystemClutterFile (const std::string& fileName)
{
    return ! fileName.empty() && fileName.front() == '.';
}

bool isAbandonedSafeWriteTemp (const std::string& fileName,
                               const std::string& targetStem,
                               const std::string& targetExtension)
{
    // "." + stem + "_temp" + hex [+ "_" + n, or + n] + extension. The number is
    // juce::File::getNonexistentChildFile's, should the first name be taken.
    if (fileName.size() < 1 + targetExtension.size() || fileName.front() != '.')
        return false;

    if (fileName.compare (fileName.size() - targetExtension.size(), targetExtension.size(),
                          targetExtension) != 0)
        return false;

    const auto body = fileName.substr (1, fileName.size() - 1 - targetExtension.size());
    const std::string marker = "_temp";

    std::string::size_type markerAt = std::string::npos;

    if (targetStem.empty())
    {
        markerAt = body.rfind (marker);

        if (markerAt == std::string::npos || markerAt == 0)
            return false;
    }
    else
    {
        if (body.compare (0, targetStem.size(), targetStem) != 0)
            return false;

        markerAt = targetStem.size();

        if (body.compare (markerAt, marker.size(), marker) != 0)
            return false;
    }

    const auto tail = body.substr (markerAt + marker.size());

    if (tail.empty() || ! std::isxdigit (static_cast<unsigned char> (tail.front())))
        return false;

    return std::all_of (tail.begin(), tail.end(), [] (char c)
    {
        return std::isxdigit (static_cast<unsigned char> (c)) || c == '_';
    });
}

bool isAbandonedTakeWorkingFile (const std::string& fileName)
{
    const auto endsWith = [&fileName] (const std::string& suffix)
    {
        return fileName.size() > suffix.size()
            && fileName.compare (fileName.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    // The combined movie, under its hidden working name. "._" is a Finder
    // AppleDouble twin, which belongs to whatever file it shadows.
    if (fileName.size() > 2 && fileName[0] == '.' && fileName[1] != '_')
        for (const char* container : { "_with-sound.mov", "_with-sound.mkv", "_with-sound.mp4" })
            if (endsWith (container) && fileName.size() > 1 + std::string (container).size())
                return true;

    // A file on its way to the card, written as ".<name>.sobcopy" and renamed
    // to its real name only once it has been read back and matched. The real
    // file it stands for is still in the take's folder on this computer.
    if (fileName.size() > 1 + std::string (".sobcopy").size() && fileName.front() == '.'
        && endsWith (".sobcopy"))
        return true;

    // The podcast-ready copy: "<mix> - <service>.wav", written as ".part".
    if (endsWith (".wav.part") && fileName.find (" - ") != std::string::npos && fileName.front() != '.')
        return true;

    return false;
}

namespace {

/// -90 dBFS. Chosen well below any real microphone path's noise floor: a 24-bit
/// LSB sits near -138 dBFS and preamp noise in a quiet room is nowhere near
/// this low, so nothing that actually recorded can fall under it.
constexpr float kSilenceFloor = 0.0000316f;

} // namespace

TakeAudioVerdict judgeTakeAudio (const std::vector<TakeFile>& files,
                                 float peakWritten, float peakArrived)
{
    const auto base = judgeTakeAudio (files, peakWritten);

    // Audio came in and none of it reached the files. Nothing about the user's
    // hardware explains that -- the samples were here.
    if ((base == TakeAudioVerdict::OnlySilence || base == TakeAudioVerdict::NothingWritten)
        && peakArrived >= kSilenceFloor)
        return TakeAudioVerdict::DroppedByApp;

    return base;
}

TakeAudioVerdict judgeTakeAudio (const std::vector<TakeFile>& files, float peakAbs)
{
    int64_t audioBytes = 0;
    int audioFiles = 0;

    for (const auto& f : files)
    {
        if (isMetadata (f.name) || isSystemClutterFile (f.name))
            continue;

        audioBytes += f.sizeBytes;
        ++audioFiles;
    }

    if (audioFiles == 0)
        return TakeAudioVerdict::NotJudged;

    // Nothing arrived at all: the files are headers. Reported separately from
    // silence because the causes are different and so is what the user should
    // go and check.
    if (audioBytes < static_cast<int64_t> (audioFiles) * 1024)
        return TakeAudioVerdict::NothingWritten;

    // A negative peak means no measurement was taken. Never report silence on
    // the strength of one that was not made.
    if (peakAbs >= 0.0f && peakAbs < kSilenceFloor)
        return TakeAudioVerdict::OnlySilence;

    return TakeAudioVerdict::Recorded;
}

bool takeHoldsNoAudio (const std::vector<TakeFile>& files)
{
    int64_t audioBytes = 0;
    int audioFiles = 0;

    for (const auto& f : files)
    {
        if (isMetadata (f.name) || isSystemClutterFile (f.name))
            continue;

        audioBytes += f.sizeBytes;
        ++audioFiles;
    }

    // No audio files at all is not "an empty take" -- there is nothing to
    // judge, and saying a take is empty when the folder holds only metadata
    // would misreport a state this rule was not written for.
    return audioFiles > 0 && audioBytes < static_cast<int64_t> (audioFiles) * 1024;
}

} // namespace mma
