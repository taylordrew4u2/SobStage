#include "MacMovieCombiner.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>

// Compiled with -fobjc-arc (see CMakeLists.txt).

namespace mma {

namespace {

NSURL* fileUrl (const std::string& path)
{
    return [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
}

std::string describe (NSError* error)
{
    if (error == nil)
        return {};

    const char* text = error.localizedDescription.UTF8String;
    return text != nullptr ? std::string (text) : std::string();
}

} // namespace

std::string combineMovieWithSound (const std::string& videoPath,
                                   const std::vector<std::string>& audioParts,
                                   const std::string& outputPath,
                                   double audioLeadSeconds,
                                   const std::function<bool()>& shouldStop)
{
    @autoreleasepool
    {
        if (audioParts.empty())
            return "There was no sound to put with the picture.";

        NSDictionary* exactTiming = @{ AVURLAssetPreferPreciseDurationAndTimingKey : @YES };

        AVURLAsset* videoAsset = [AVURLAsset URLAssetWithURL:fileUrl (videoPath) options:exactTiming];
        AVAssetTrack* videoTrack = [[videoAsset tracksWithMediaType:AVMediaTypeVideo] firstObject];

        if (videoTrack == nil)
            return "The camera's file has no picture in it.";

        const CMTime videoDuration = videoTrack.timeRange.duration;
        if (! CMTIME_IS_VALID (videoDuration) || CMTimeCompare (videoDuration, kCMTimeZero) <= 0)
            return "The camera's file is empty.";

        AVMutableComposition* composition = [AVMutableComposition composition];

        AVMutableCompositionTrack* pictureOut =
            [composition addMutableTrackWithMediaType:AVMediaTypeVideo
                                     preferredTrackID:kCMPersistentTrackID_Invalid];
        NSError* error = nil;

        if (! [pictureOut insertTimeRange:videoTrack.timeRange ofTrack:videoTrack atTime:kCMTimeZero error:&error])
            return "Couldn't read the picture: " + describe (error);

        pictureOut.preferredTransform = videoTrack.preferredTransform;

        AVMutableCompositionTrack* soundOut =
            [composition addMutableTrackWithMediaType:AVMediaTypeAudio
                                     preferredTrackID:kCMPersistentTrackID_Invalid];

        // The sound starts first; skip that much of it, across part
        // boundaries if a part is shorter than the lead.
        constexpr int32_t kTimescale = 48000;
        CMTime toSkip = CMTimeMakeWithSeconds (audioLeadSeconds > 0.0 ? audioLeadSeconds : 0.0, kTimescale);
        CMTime cursor = kCMTimeZero;

        for (const auto& part : audioParts)
        {
            if (CMTimeCompare (cursor, videoDuration) >= 0)
                break;

            AVURLAsset* soundAsset = [AVURLAsset URLAssetWithURL:fileUrl (part) options:exactTiming];
            AVAssetTrack* soundTrack = [[soundAsset tracksWithMediaType:AVMediaTypeAudio] firstObject];

            if (soundTrack == nil)
                return "Couldn't read the sound in " + part + ".";

            const CMTimeRange whole = soundTrack.timeRange;

            if (CMTimeCompare (toSkip, whole.duration) >= 0)
            {
                toSkip = CMTimeSubtract (toSkip, whole.duration);
                continue;
            }

            CMTime start = CMTimeAdd (whole.start, toSkip);
            CMTime length = CMTimeSubtract (whole.duration, toSkip);
            toSkip = kCMTimeZero;

            // Never past the end of the picture.
            const CMTime room = CMTimeSubtract (videoDuration, cursor);
            if (CMTimeCompare (length, room) > 0)
                length = room;

            if (! [soundOut insertTimeRange:CMTimeRangeMake (start, length) ofTrack:soundTrack
                                     atTime:cursor error:&error])
                return "Couldn't read the sound: " + describe (error);

            cursor = CMTimeAdd (cursor, length);
        }

        if (CMTimeCompare (cursor, kCMTimeZero) <= 0)
            return "The sound ended before the picture began.";

        // Ends where the shorter stream does, as the ffmpeg combine does
        // (-shortest). The cameras stop after the sound, so the picture used
        // to run on past the end of the sound -- a silent tail on the Mac's
        // combined file that the Windows one never had.
        if (CMTimeCompare (cursor, videoDuration) < 0)
            [pictureOut removeTimeRange:CMTimeRangeMake (cursor, CMTimeSubtract (videoDuration, cursor))];

        AVAssetExportSession* exporter =
            [[AVAssetExportSession alloc] initWithAsset:composition presetName:AVAssetExportPresetPassthrough];

        if (exporter == nil)
            return "macOS couldn't set up the combined file.";

        NSURL* outputUrl = fileUrl (outputPath);

        // Written under a hidden working name and renamed into place only once
        // complete. Exported straight to the final name, a combine cut short
        // by "Quit now", a crash or a power cut left a multi-gigabyte file
        // with no index -- unplayable, and named exactly like the finished
        // one. Hidden by a leading dot (the app's own rule for files that are
        // not part of the take), and still .mov, which the export requires.
        NSURL* workingUrl = [[outputUrl URLByDeletingLastPathComponent]
                                URLByAppendingPathComponent:[@"." stringByAppendingString:outputUrl.lastPathComponent]];
        NSFileManager* files = [NSFileManager defaultManager];

        [files removeItemAtURL:outputUrl error:nil];
        [files removeItemAtURL:workingUrl error:nil];

        exporter.outputURL = workingUrl;
        exporter.outputFileType = AVFileTypeQuickTimeMovie;
        exporter.shouldOptimizeForNetworkUse = NO;

        dispatch_semaphore_t done = dispatch_semaphore_create (0);
        [exporter exportAsynchronouslyWithCompletionHandler:^{ dispatch_semaphore_signal (done); }];

        // Polled rather than waited on outright, so quitting -- or a take
        // starting on this card -- is honoured promptly however long a
        // four-hour take takes to copy.
        while (dispatch_semaphore_wait (done, dispatch_time (DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0)
        {
            if (shouldStop())
            {
                [exporter cancelExport];

                // The file is removed only once the export has really
                // stopped: deleting it under a still-running export let the
                // export write it back, leaving a partial file that looked
                // finished. This runs on a detached worker, so waiting here
                // never holds up quitting.
                dispatch_semaphore_wait (done, dispatch_time (DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC));
                [files removeItemAtURL:workingUrl error:nil];
                return "Stopped before the combined file was finished.";
            }
        }

        if (exporter.status != AVAssetExportSessionStatusCompleted)
        {
            const auto why = describe (exporter.error);
            [files removeItemAtURL:workingUrl error:nil];
            return why.empty() ? std::string ("macOS couldn't finish the combined file.")
                               : "macOS couldn't finish the combined file: " + why;
        }

        NSError* moveError = nil;
        if (! [files moveItemAtURL:workingUrl toURL:outputUrl error:&moveError])
        {
            [files removeItemAtURL:workingUrl error:nil];
            return "Couldn't name the finished combined file: " + describe (moveError);
        }

        return {};
    }
}

} // namespace mma
