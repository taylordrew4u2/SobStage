// Proves the Mac's built-in combine on the real AVFoundation: makes a short
// silent movie and a WAV the way a take does, combines them, and opens the
// result. Run by CI on macOS; there is no stand-in for this framework.

#include "Platform/MacMovieCombiner.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check (bool ok, const char* what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok)
        ++failures;
}

void putLe (std::ofstream& f, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i)
        f.put (static_cast<char> ((v >> (8 * i)) & 0xff));
}

// 24-bit mono PCM, as the take writes it.
bool writeWav (const std::string& path, double seconds)
{
    const uint32_t rate = 48000;
    const auto frames = static_cast<uint32_t> (seconds * rate);
    const uint32_t dataBytes = frames * 3;

    std::ofstream f (path, std::ios::binary);
    if (! f)
        return false;

    f.write ("RIFF", 4); putLe (f, 36 + dataBytes, 4); f.write ("WAVE", 4);
    f.write ("fmt ", 4); putLe (f, 16, 4); putLe (f, 1, 2); putLe (f, 1, 2);
    putLe (f, rate, 4); putLe (f, rate * 3, 4); putLe (f, 3, 2); putLe (f, 24, 2);
    f.write ("data", 4); putLe (f, dataBytes, 4);

    for (uint32_t i = 0; i < frames; ++i)
    {
        const auto s = static_cast<int32_t> (std::sin (i * 0.05) * 4000000.0);
        putLe (f, static_cast<uint32_t> (s) & 0xffffff, 3);
    }

    return static_cast<bool> (f);
}

bool writeMovie (const std::string& path, double seconds, int fps = 30)
{
    @autoreleasepool
    {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        [[NSFileManager defaultManager] removeItemAtURL:url error:nil];

        NSError* error = nil;
        AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL:url fileType:AVFileTypeQuickTimeMovie error:&error];
        if (writer == nil)
            return false;

        NSDictionary* settings = @{ AVVideoCodecKey : AVVideoCodecTypeH264,
                                    AVVideoWidthKey : @64, AVVideoHeightKey : @64 };
        AVAssetWriterInput* input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                                       outputSettings:settings];
        input.expectsMediaDataInRealTime = NO;

        NSDictionary* bufferAttributes = @{ (__bridge NSString*) kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
                                            (__bridge NSString*) kCVPixelBufferWidthKey : @64,
                                            (__bridge NSString*) kCVPixelBufferHeightKey : @64 };
        AVAssetWriterInputPixelBufferAdaptor* adaptor =
            [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input
                                                                           sourcePixelBufferAttributes:bufferAttributes];
        [writer addInput:input];

        if (! [writer startWriting])
            return false;

        [writer startSessionAtSourceTime:kCMTimeZero];

        const int frameCount = static_cast<int> (seconds * fps);

        for (int i = 0; i < frameCount; ++i)
        {
            while (! input.readyForMoreMediaData)
                [NSThread sleepForTimeInterval:0.005];

            CVPixelBufferRef buffer = nullptr;
            if (CVPixelBufferPoolCreatePixelBuffer (nullptr, adaptor.pixelBufferPool, &buffer) != kCVReturnSuccess
                || buffer == nullptr)
                return false;

            CVPixelBufferLockBaseAddress (buffer, 0);
            auto* bytes = static_cast<uint8_t*> (CVPixelBufferGetBaseAddress (buffer));
            std::fill (bytes, bytes + CVPixelBufferGetDataSize (buffer), static_cast<uint8_t> (i * 4));
            CVPixelBufferUnlockBaseAddress (buffer, 0);

            const bool appended = [adaptor appendPixelBuffer:buffer withPresentationTime:CMTimeMake (i, fps)];
            CVPixelBufferRelease (buffer);
            if (! appended)
                return false;
        }

        [input markAsFinished];
        [writer endSessionAtSourceTime:CMTimeMake (frameCount, fps)];

        dispatch_semaphore_t done = dispatch_semaphore_create (0);
        [writer finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal (done); }];
        dispatch_semaphore_wait (done, dispatch_time (DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));

        return writer.status == AVAssetWriterStatusCompleted;
    }
}

struct Opened
{
    int videoTracks = 0, audioTracks = 0;
    double videoSeconds = 0.0, audioSeconds = 0.0;
};

bool fileExists (const std::string& path)
{
    return [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:path.c_str()]];
}

Opened open (const std::string& path)
{
    @autoreleasepool
    {
        Opened o;
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url
                                                options:@{ AVURLAssetPreferPreciseDurationAndTimingKey : @YES }];
        NSArray* video = [asset tracksWithMediaType:AVMediaTypeVideo];
        NSArray* audio = [asset tracksWithMediaType:AVMediaTypeAudio];
        o.videoTracks = static_cast<int> (video.count);
        o.audioTracks = static_cast<int> (audio.count);
        if (video.count > 0)
            o.videoSeconds = CMTimeGetSeconds (((AVAssetTrack*) video.firstObject).timeRange.duration);
        if (audio.count > 0)
            o.audioSeconds = CMTimeGetSeconds (((AVAssetTrack*) audio.firstObject).timeRange.duration);
        return o;
    }
}

} // namespace

int main()
{
  @autoreleasepool
  {
    const std::string dir = std::string ([NSTemporaryDirectory() UTF8String]) + "sobstage-combine-check-"
                          + std::to_string (static_cast<long long> ([[NSDate date] timeIntervalSince1970] * 1000));
    [[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:dir.c_str()]
                              withIntermediateDirectories:YES attributes:nil error:nil];

    const auto movie = dir + "/V01_Cam.mov";
    const auto mix = dir + "/MIX.wav";
    const auto mixA = dir + "/MIXA.wav";
    const auto mixB = dir + "/MIXA_001.wav";
    const auto out = dir + "/V01_Cam_with-sound.mov";
    const auto outSplit = dir + "/V01_Cam_split.mov";

    check (writeMovie (movie, 2.0), "made a 2 s test movie");
    check (writeWav (mix, 3.0), "made a 3 s 24-bit mix");
    check (writeWav (mixA, 1.0) && writeWav (mixB, 2.0), "made a mix split in two parts");

    const std::function<bool()> notCancelled = [] { return false; };

    // One mix; the sound started half a second before the picture.
    {
        const auto problem = mma::combineMovieWithSound (movie, { mix }, out, 0.5, notCancelled);
        if (! problem.empty())
            std::printf ("  problem: %s\n", problem.c_str());
        check (problem.empty(), "combined the movie with the mix");

        const auto o = open (out);
        check (o.videoTracks == 1 && o.audioTracks == 1, "the result has one picture and one sound");
        check (std::abs (o.videoSeconds - 2.0) < 0.1, "the picture is whole");
        check (std::abs (o.audioSeconds - 2.0) < 0.1, "the sound is trimmed to the picture");
        check (! [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:(dir + "/.V01_Cam_with-sound.mov").c_str()]],
               "no working file is left beside the finished one");
    }

    // A camera unplugged mid-take can leave a movie with no index: the bytes
    // are there, the moov atom is not. Reported, never a crash, a hang or a
    // file that claims to be the combined take.
    {
        NSData* whole = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:movie.c_str()]];
        const auto cut = dir + "/V02_Cut.mov";
        const auto cutOut = dir + "/V02_Cut_with-sound.mov";
        const bool wrote = whole != nil && whole.length > 64
            && [[whole subdataWithRange:NSMakeRange (0, whole.length / 2)]
                   writeToFile:[NSString stringWithUTF8String:cut.c_str()] atomically:NO];
        check (wrote, "made a movie cut off before its index");

        const auto problem = mma::combineMovieWithSound (cut, { mix }, cutOut, 0.0, notCancelled);
        check (! problem.empty(), "a movie with no index is reported");
        check (! [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:cutOut.c_str()]]
               && ! [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:(dir + "/.V02_Cut_with-sound.mov").c_str()]],
               "and leaves no combined or working file behind");
    }

    // A long take's mix split into parts, with the lead crossing into the second.
    {
        const auto problem = mma::combineMovieWithSound (movie, { mixA, mixB }, outSplit, 1.2, notCancelled);
        if (! problem.empty())
            std::printf ("  problem: %s\n", problem.c_str());
        check (problem.empty(), "combined across split mix files");

        const auto o = open (outSplit);
        check (o.videoTracks == 1 && o.audioTracks == 1, "the split result has one picture and one sound");
        check (std::abs (o.audioSeconds - 1.8) < 0.1, "the lead is skipped across the part boundary");
        check (std::abs (o.videoSeconds - 1.8) < 0.1, "the picture ends with the sound, as ffmpeg's -shortest does");
    }

    // The take folder renamed in Finder in the moment between the export
    // ending and the finished file being named. The rename is made from the
    // combine asking where the folder is, which it does only once the export
    // has ended. The finished file is named in the folder as it is now, not
    // looked for in the old one and left hidden for the next launch to delete
    // as unfinished. A rename while the export is still writing is checked
    // next.
    {
        const auto before = dir + "/Take-before";
        const auto after = dir + "/Take-after";
        [[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:before.c_str()]
                                  withIntermediateDirectories:YES attributes:nil error:nil];
        check (writeMovie (before + "/V01_Cam.mov", 2.0) && writeWav (before + "/MIX.wav", 3.0),
               "made a take to rename");

        bool renamed = false;
        const std::function<std::string()> folderNow = [&]
        {
            if (! renamed)
                renamed = std::rename (before.c_str(), after.c_str()) == 0;

            return renamed ? after : before;
        };

        const auto problem = mma::combineMovieWithSound (before + "/V01_Cam.mov", { before + "/MIX.wav" },
                                                         before + "/V01_Cam_with-sound.mov", 0.5,
                                                         notCancelled, folderNow);
        if (! problem.empty())
            std::printf ("  problem: %s\n", problem.c_str());
        check (renamed && problem.empty(), "a take folder renamed as the export ended still gets its combined file");
        check ([[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:(after + "/V01_Cam_with-sound.mov").c_str()]],
               "the combined file is in the folder under its new name");
        check (! [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:(after + "/.V01_Cam_with-sound.mov").c_str()]],
               "no hidden working file is left in the renamed folder");
    }

    // The take folder renamed in Finder while AVFoundation is still writing
    // the export into it -- the case a long take meets, minutes into its
    // combine. The rename is made from shouldStop, which the combine asks
    // only after the export has run 100 ms without finishing, so the folder
    // moves under a running export. The take is fifteen minutes of sound,
    // about 130 MB, far more than an export copies in 100 ms. If shouldStop
    // is still never asked, nothing was renamed and the check fails rather
    // than passing untested.
    //
    // Either outcome of the real framework is fine for SobStage, and the log
    // says which one this Mac gave: AVFoundation goes on writing into the
    // renamed folder and the finished file is named there; or it fails, and
    // leaves nothing hidden there, and TakeCombiner -- seeing the folder has
    // moved -- makes it once more from the new place.
    const auto longBefore = dir + "/Long-before";
    const auto longAfter = dir + "/Long-after";
    bool longRenamed = false;
    {
        const double takeSeconds = 900.0;
        const double leadSeconds = 0.5;
        const auto working = std::string ("/.V01_Cam_with-sound.mov");
        const auto finished = std::string ("/V01_Cam_with-sound.mov");

        [[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:longBefore.c_str()]
                                  withIntermediateDirectories:YES attributes:nil error:nil];
        // Two frames a second keeps the picture quick to make; the sound is
        // what makes the export long.
        check (writeMovie (longBefore + "/V01_Cam.mov", takeSeconds, 2) && writeWav (longBefore + "/MIX.wav", takeSeconds),
               "made a fifteen-minute take to rename while it combines");

        int polls = 0;
        int pollsAfterRename = 0;
        bool workingFileWasThere = false;
        const std::function<bool()> renameMidExport = [&]
        {
            ++polls;

            if (longRenamed)
            {
                ++pollsAfterRename;
                return false;
            }

            workingFileWasThere = fileExists (longBefore + working);
            longRenamed = std::rename (longBefore.c_str(), longAfter.c_str()) == 0;
            return false;
        };
        const std::function<std::string()> folderNow = [&] { return longRenamed ? longAfter : longBefore; };

        const auto problem = mma::combineMovieWithSound (longBefore + "/V01_Cam.mov", { longBefore + "/MIX.wav" },
                                                         longBefore + finished, leadSeconds,
                                                         renameMidExport, folderNow);
        std::printf ("  asked whether to stop %d times while the export ran, %d of them after the rename; "
                     "its working file %s already in the folder when it was renamed\n",
                     polls, pollsAfterRename, workingFileWasThere ? "was" : "was not");
        check (longRenamed, "the folder was renamed while the export was still running");

        if (longRenamed)
        {
            check (! fileExists (longAfter + working) && ! fileExists (longBefore + working),
                   "no hidden working file is left in either folder");

            if (problem.empty())
            {
                std::printf ("  AVFoundation went on writing into the renamed folder\n");
            }
            else
            {
                std::printf ("  the combine failed once the folder moved: %s\n", problem.c_str());
                check (! fileExists (longAfter + finished), "the failed combine left no file named as the finished one");

                // TakeCombiner sees the folder has moved and tries once more,
                // from where it is now.
                const auto again = mma::combineMovieWithSound (longAfter + "/V01_Cam.mov", { longAfter + "/MIX.wav" },
                                                               longAfter + finished, leadSeconds,
                                                               notCancelled, folderNow);
                if (! again.empty())
                    std::printf ("  problem: %s\n", again.c_str());
                check (again.empty() && ! fileExists (longAfter + working),
                       "tried once more from the renamed folder, as TakeCombiner does, it combines");
            }

            const auto o = open (longAfter + finished);
            check (o.videoTracks == 1 && o.audioTracks == 1
                       && std::abs (o.audioSeconds - (takeSeconds - leadSeconds)) < 0.1
                       && std::abs (o.videoSeconds - (takeSeconds - leadSeconds)) < 0.1,
                   "the combined file is whole, in the folder under its new name");
        }
    }

    // Stopped -- quitting, or a take starting on the card -- just after the
    // folder was renamed under the running export: the unfinished file is
    // cleared from the folder as it is now, not looked for under the old
    // name and left hidden.
    if (longRenamed)
    {
        const auto stoppedFolder = dir + "/Long-stopped";
        bool renamedAgain = false;
        const std::function<bool()> renameThenStop = [&]
        {
            if (! renamedAgain)
                renamedAgain = std::rename (longAfter.c_str(), stoppedFolder.c_str()) == 0;

            return true;
        };
        const std::function<std::string()> folderNow = [&] { return renamedAgain ? stoppedFolder : longAfter; };

        const auto problem = mma::combineMovieWithSound (longAfter + "/V01_Cam.mov", { longAfter + "/MIX.wav" },
                                                         longAfter + "/V01_Cam_stopped.mov", 0.5,
                                                         renameThenStop, folderNow);
        check (renamedAgain && ! problem.empty(), "a combine stopped after its folder was renamed says it stopped");
        check (! fileExists (stoppedFolder + "/.V01_Cam_stopped.mov") && ! fileExists (stoppedFolder + "/V01_Cam_stopped.mov"),
               "and leaves no working or combined file in the renamed folder");
    }

    // A missing picture is reported, not a crash or an empty file.
    {
        const auto missing = dir + "/nothing.mov";
        const auto problem = mma::combineMovieWithSound (missing, { mix }, dir + "/bad.mov", 0.0, notCancelled);
        check (! problem.empty(), "a missing camera file is reported");
        check (! [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:(dir + "/bad.mov").c_str()]],
               "and leaves no file behind");
    }

    [[NSFileManager defaultManager] removeItemAtPath:[NSString stringWithUTF8String:dir.c_str()] error:nil];

    std::printf (failures == 0 ? "ALL PASS\n" : "FAILURES: %d\n", failures);
    return failures == 0 ? 0 : 1;
  }
}
