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

bool writeMovie (const std::string& path, double seconds)
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

        const int fps = 30;
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

    // The take folder renamed in Finder while the export ran -- here, just as
    // it finished. The finished file is named in the folder as it is now, not
    // looked for in the old one and left hidden for the next launch to delete
    // as unfinished.
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
        check (renamed && problem.empty(), "a take folder renamed during the combine still gets its combined file");
        check ([[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:(after + "/V01_Cam_with-sound.mov").c_str()]],
               "the combined file is in the folder under its new name");
        check (! [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:(after + "/.V01_Cam_with-sound.mov").c_str()]],
               "no hidden working file is left in the renamed folder");
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
