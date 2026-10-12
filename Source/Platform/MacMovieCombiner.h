#pragma once
#include <functional>
#include <string>
#include <vector>

namespace mma {

/// Writes one movie with the camera's picture and the take's sound, using the
/// Mac's own AVFoundation -- no ffmpeg to install. Neither stream is
/// re-encoded: the picture is copied as recorded and the WAV's PCM goes into
/// the .mov as it is, at the depth it was recorded at.
///
/// audioParts are the mix's split files in order (MIX.wav, MIX_001.wav ...);
/// they play back to back. The first audioLeadSeconds of the sound are skipped
/// so the picture's first frame lines up with the sound. The file ends where
/// the shorter of the two does, like ffmpeg's -shortest: sound past the end of
/// the picture, or picture past the end of the sound, is trimmed off.
///
/// Returns empty on success, otherwise what went wrong in plain words. Blocks
/// until done; call it from a worker. `shouldStop` is asked every 100 ms while
/// the export runs -- quitting, or a take starting on the card -- and true
/// abandons the export, leaving no file behind.
///
/// `currentFolder`, when given, is asked once the export has ended where the
/// output's folder is now (UTF-8; empty for "where outputPath says"). The take
/// folder can be renamed in Finder while a long export runs, and the export
/// goes on writing into it under its new name, so the finished file is named
/// -- or an unfinished one removed -- there.
std::string combineMovieWithSound (const std::string& videoPath,
                                   const std::vector<std::string>& audioParts,
                                   const std::string& outputPath,
                                   double audioLeadSeconds,
                                   const std::function<bool()>& shouldStop,
                                   const std::function<std::string()>& currentFolder = {});

} // namespace mma
