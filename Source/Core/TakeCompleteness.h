#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mma {

/// One file in a finished session folder, as it stands on disk.
struct TakeFile
{
    std::string name;
    int64_t sizeBytes = 0;
};

/// True for a file the operating system, not this app, put in a take folder:
/// any name starting with '.'.
///
/// macOS leaves these beside a take as a matter of course. Finder writes
/// .DS_Store into any folder it shows, and on an ExFAT or FAT32 card every
/// file that carries an extended attribute gets an AppleDouble twin -- so
/// "MIX.wav" arrives with "._MIX.wav", a 4 KB file that ends in .wav and is
/// not audio. Counted as part of the take they were listed among its files on
/// the saved-take card, padded a headers-only take past the "nothing written"
/// bar, and turned up in an interrupted take's recovery as extra empty tracks.
/// The app never writes a dot-file into a take folder itself.
bool isSystemClutterFile (const std::string& fileName);

/// True for the hidden temporary file a safe replace of `targetStem` +
/// `targetExtension` leaves behind when the process dies between writing it
/// and renaming it over the target: juce::TemporaryFile's ".settings_temp1a2b3c4d.json"
/// for "settings" / ".json". An empty `targetStem` matches any stem, for a
/// folder whose files are all written that way (show templates).
///
/// The rename is atomic, so such a file is never the only copy of anything:
/// the target still holds the previous version. A crash, force-quit or power
/// cut at the wrong moment simply strands one, and nothing ever removed them --
/// and in the templates folder a stranded one was read back as a show.
bool isAbandonedSafeWriteTemp (const std::string& fileName,
                               const std::string& targetStem,
                               const std::string& targetExtension);

/// True for a file this app writes into a take folder only while making
/// something after the take, and renames away once that is finished: the
/// combined movie's hidden working file (".V01_Cam_with-sound.mov"), the
/// podcast-ready copy's "MIX - Apple Podcasts.wav.part", and a file being
/// copied to the card and not yet checked (".MIX.wav.sobcopy"). Found at
/// launch, one is what a crash, force-quit or power cut left of an unfinished
/// export or copy -- never a recording, and never something the user can play.
bool isAbandonedTakeWorkingFile (const std::string& fileName);

/// True when a finished take produced files but no audio worth the name.
///
/// A WAV header alone is 44 bytes plus the BWF chunk, so anything under a
/// kilobyte per file holds nothing. session.json is excluded: it is a few
/// hundred bytes whatever happened, and counting it would make a take that
/// recorded nothing look like it recorded something.
///
/// This lives here, rather than in the panel that first needed it, because two
/// places have to agree about it. The panel warned that the files were empty
/// while the status line said "Saved to ..." in the same breath -- and the
/// status line is the one a user reads while walking away.
bool takeHoldsNoAudio (const std::vector<TakeFile>& files);

/// What a finished take actually holds.
enum class TakeAudioVerdict
{
    NotJudged,      ///< No audio files: nothing this rule was written for.
    Recorded,       ///< Signal reached the files.
    NothingWritten, ///< Headers only -- the stream never arrived.
    OnlySilence,    ///< Full-length files, every sample of them flat.
    DroppedByApp    ///< Audio arrived and this app failed to write it.
};

/// The verdict on a take, from the files on disk AND the loudest sample that
/// actually reached the writer.
///
/// File size alone cannot see the failure that matters most here. A device
/// that is present and streaming digital silence -- a mic muted at its own
/// switch, a dead channel on an interface, a USB board whose audio never
/// carried signal -- produces stems that are megabytes long and completely
/// flat. takeHoldsNoAudio weighs bytes, so it calls that a recording, and the
/// app says "Saved." over a folder holding nothing. §0.1 names unreported loss
/// as the one unacceptable failure, and that is exactly what it is.
///
/// `peakAbs` is the largest absolute sample value the take wrote, or negative
/// when nobody measured one -- in which case silence is never reported, because
/// a measurement that was not taken is not evidence.
///
/// The silence bar is -90 dBFS. Real microphone paths carry preamp noise far
/// above that even in a quiet room, so a whisper still counts as a recording;
/// only a genuinely dead stream falls under it.
TakeAudioVerdict judgeTakeAudio (const std::vector<TakeFile>& files, float peakAbs);

/// The same judgement, told what ARRIVED as well as what was written.
///
/// The two can disagree, and when they do it is the most important thing the
/// app knows: audio reaching the capture path and not reaching the files is a
/// fault in this program, not in the user's rig. Without this the take is
/// simply "silent", and someone spends an hour checking cables and mute
/// switches that were never the problem.
///
/// `peakArrived` negative means nobody measured it, and the verdict falls back
/// to what the files and the written peak alone can say.
TakeAudioVerdict judgeTakeAudio (const std::vector<TakeFile>& files,
                                 float peakWritten, float peakArrived);

} // namespace mma
