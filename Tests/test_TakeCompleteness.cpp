#include "TestFramework.h"
#include "../Source/Core/TakeCompleteness.h"

using namespace mma;

TEST_CASE (TakeCompleteness_HeadersOnlyCountsAsNoAudio)
{
    // What a take looks like when the card was pulled before any audio landed:
    // every stem finalized, every one of them a bare WAV header.
    REQUIRE (takeHoldsNoAudio ({ { "MIX.wav", 44 },
                                 { "01_Kitchen.wav", 44 },
                                 { "02_Sofa.wav", 44 },
                                 { "session.json", 900 } }));
}

TEST_CASE (TakeCompleteness_ARealTakeIsNotEmpty)
{
    REQUIRE_FALSE (takeHoldsNoAudio ({ { "MIX.wav", 5'000'000 },
                                       { "01_Kitchen.wav", 5'000'000 },
                                       { "session.json", 900 } }));
}

TEST_CASE (TakeCompleteness_SessionJsonAloneCannotMakeATakeLookRecorded)
{
    // The metadata is a few hundred bytes whatever happened. Counting it would
    // push a folder of empty stems over the threshold and call it saved.
    REQUIRE (takeHoldsNoAudio ({ { "MIX.wav", 44 }, { "session.json", 4000 } }));
}

TEST_CASE (TakeCompleteness_MetadataSuffixIsMatchedRegardlessOfCase)
{
    REQUIRE (takeHoldsNoAudio ({ { "MIX.wav", 44 }, { "SESSION.JSON", 4000 } }));
}

TEST_CASE (TakeCompleteness_AFolderWithNoAudioFilesIsNotJudged)
{
    // Nothing to judge: there are no stems, so "this take is empty" would be
    // reporting on a state this rule was not written for.
    REQUIRE_FALSE (takeHoldsNoAudio ({ { "session.json", 900 } }));
    REQUIRE_FALSE (takeHoldsNoAudio ({}));
}

TEST_CASE (TakeCompleteness_ThresholdIsPerFileNotForTheWholeTake)
{
    // Eight stems holding a header each sum to more than 1KB. Judging the
    // total against a single kilobyte would call that a real recording.
    REQUIRE (takeHoldsNoAudio ({ { "1.wav", 200 }, { "2.wav", 200 }, { "3.wav", 200 },
                                 { "4.wav", 200 }, { "5.wav", 200 }, { "6.wav", 200 },
                                 { "7.wav", 200 }, { "8.wav", 200 } }));

    // And one genuinely-recorded file among them still clears the per-file bar
    // on average, which is the case the panel has always accepted as recorded.
    REQUIRE_FALSE (takeHoldsNoAudio ({ { "1.wav", 44 }, { "2.wav", 5'000'000 } }));
}

TEST_CASE (TakeCompleteness_FullLengthFilesOfPureSilenceAreNotARecording)
{
    // The case the byte-count rule cannot see, and the one a user actually
    // hits: the device was there, the stream ran for the whole take, and every
    // sample of it was zero. A mic muted at its own switch, an interface
    // delivering a dead channel, a board whose USB audio never carried signal.
    //
    // Every file is megabytes. takeHoldsNoAudio looks at size alone and calls
    // that a recording, so the app said "Saved." and handed over silence --
    // which is §0.1's one unacceptable failure, audio lost without a word.
    const std::vector<TakeFile> fullLength { { "MIX.wav", 5'000'000 },
                                             { "01_Kitchen.wav", 5'000'000 },
                                             { "session.json", 900 } };

    // The rule this replaces, on the same folder: it sees megabytes and calls
    // it recorded. That is the blind spot, kept here so it cannot come back.
    REQUIRE_FALSE (takeHoldsNoAudio (fullLength));

    REQUIRE (judgeTakeAudio (fullLength, 0.0f) == TakeAudioVerdict::OnlySilence);

    // Signal anywhere in the take clears it, however quiet.
    REQUIRE (judgeTakeAudio (fullLength, 0.01f) == TakeAudioVerdict::Recorded);
}

TEST_CASE (TakeCompleteness_AQuietTakeIsStillARecording)
{
    // A whisper at the far end of a room is not silence, and calling it silence
    // would put a warning on a take that is perfectly good. The bar is digital
    // silence -- below -90 dBFS, which no microphone path reaches.
    const std::vector<TakeFile> files { { "MIX.wav", 5'000'000 } };

    REQUIRE (judgeTakeAudio (files, 0.001f) == TakeAudioVerdict::Recorded);   // -60 dBFS
    REQUIRE (judgeTakeAudio (files, 0.0001f) == TakeAudioVerdict::Recorded);  // -80 dBFS
}

TEST_CASE (TakeCompleteness_HeaderOnlyFilesReportNothingWrittenNotSilence)
{
    // Two different failures with two different causes, so they must not share
    // a message: nothing arrived at all, versus a stream that ran and was flat.
    REQUIRE (judgeTakeAudio ({ { "MIX.wav", 44 }, { "01.wav", 44 } }, 0.0f)
             == TakeAudioVerdict::NothingWritten);
}

TEST_CASE (TakeCompleteness_AnUnmeasuredPeakNeverReportsSilence)
{
    // A negative peak means nobody measured one. Reporting silence on the
    // strength of a measurement that was never taken would be a guess.
    REQUIRE (judgeTakeAudio ({ { "MIX.wav", 5'000'000 } }, -1.0f)
             == TakeAudioVerdict::Recorded);
}

TEST_CASE (TakeCompleteness_AudioThatArrivedAndWasNotWrittenIsThisAppsFault)
{
    // The distinction that matters most when a take comes back silent. If no
    // audio arrived, the rig is worth checking: a mute switch, a cable, an
    // input selection. If audio DID arrive and none of it reached the files,
    // nothing about the rig explains it -- the samples were here and this
    // program lost them.
    //
    // Telling those apart is the difference between someone spending an hour
    // on their microphone and knowing at a glance it was never the microphone.
    const std::vector<TakeFile> fullLength { { "MIX.wav", 5'000'000 },
                                             { "01_Kitchen.wav", 5'000'000 } };
    const std::vector<TakeFile> headersOnly { { "MIX.wav", 44 }, { "01_Kitchen.wav", 44 } };

    // Written nothing, but plenty arrived.
    REQUIRE (judgeTakeAudio (fullLength, 0.0f, 0.5f) == TakeAudioVerdict::DroppedByApp);
    REQUIRE (judgeTakeAudio (headersOnly, 0.0f, 0.5f) == TakeAudioVerdict::DroppedByApp);

    // Nothing arrived either: the rig really is silent, and that is what to say.
    REQUIRE (judgeTakeAudio (fullLength, 0.0f, 0.0f) == TakeAudioVerdict::OnlySilence);
    REQUIRE (judgeTakeAudio (headersOnly, 0.0f, 0.0f) == TakeAudioVerdict::NothingWritten);

    // A healthy take is unaffected by either reading.
    REQUIRE (judgeTakeAudio (fullLength, 0.5f, 0.5f) == TakeAudioVerdict::Recorded);

    // No arrival measurement: fall back to what the files and the write say,
    // never invent a fault out of a number nobody took.
    REQUIRE (judgeTakeAudio (fullLength, 0.0f, -1.0f) == TakeAudioVerdict::OnlySilence);
}

// Finder's .DS_Store (the folder was opened to watch it grow) and an ExFAT
// card's AppleDouble "._" twins are kilobytes each. Counted as audio they
// pushed a headers-only take past the "nothing written" bar, and it was
// reported as a silent recording instead.
TEST_CASE (TakeCompleteness_FindersOwnFilesDoNotCountAsAudio)
{
    REQUIRE (isSystemClutterFile (".DS_Store"));
    REQUIRE (isSystemClutterFile ("._MIX.wav"));
    REQUIRE_FALSE (isSystemClutterFile ("MIX.wav"));
    REQUIRE_FALSE (isSystemClutterFile ("01_Alice.wav"));

    const std::vector<TakeFile> files { { "MIX.wav", 666 },
                                        { "01_Alice.wav", 666 },
                                        { ".DS_Store", 6148 },
                                        { "._MIX.wav", 4096 },
                                        { "._01_Alice.wav", 4096 },
                                        { "session.json", 900 } };

    REQUIRE (takeHoldsNoAudio (files));
    REQUIRE (judgeTakeAudio (files, 0.0f) == TakeAudioVerdict::NothingWritten);
}

// juce::TemporaryFile's names for a safe replace: what a crash between the
// write and the rename leaves behind, and nothing else.
TEST_CASE (TakeCompleteness_AStrandedSafeWriteTemporaryIsRecognisedAndNothingElse)
{
    REQUIRE (isAbandonedSafeWriteTemp (".settings_temp1a2b3c4d.json", "settings", ".json"));
    REQUIRE (isAbandonedSafeWriteTemp (".settings_tempFFFFFFFF.json", "settings", ".json"));
    REQUIRE (isAbandonedSafeWriteTemp (".settings_temp1a2b3c4d_2.json", "settings", ".json"));
    REQUIRE (isAbandonedSafeWriteTemp (".camera-starting_temp7f.txt", "camera-starting", ".txt"));
    REQUIRE (isAbandonedSafeWriteTemp (".session_temp7f.json", "session", ".json"));
    REQUIRE (isAbandonedSafeWriteTemp (".activity_temp7f.log", "activity", ".log"));

    REQUIRE_FALSE (isAbandonedSafeWriteTemp ("settings.json", "settings", ".json"));
    REQUIRE_FALSE (isAbandonedSafeWriteTemp ("settings_temp1a.json", "settings", ".json"));
    REQUIRE_FALSE (isAbandonedSafeWriteTemp (".settings_temp.json", "settings", ".json"));
    REQUIRE_FALSE (isAbandonedSafeWriteTemp (".settings_tempxyz.json", "settings", ".json"));
    REQUIRE_FALSE (isAbandonedSafeWriteTemp (".settings_temp1a.txt", "settings", ".json"));
    REQUIRE_FALSE (isAbandonedSafeWriteTemp (".other_temp1a.json", "settings", ".json"));
    REQUIRE_FALSE (isAbandonedSafeWriteTemp (".DS_Store", "settings", ".json"));

    // Any stem, for the show templates folder.
    REQUIRE (isAbandonedSafeWriteTemp (".Friday gig_temp0c1d.json", "", ".json"));
    REQUIRE (isAbandonedSafeWriteTemp (".My_temp_show_temp0c1d.json", "", ".json"));
    REQUIRE_FALSE (isAbandonedSafeWriteTemp ("Friday gig.json", "", ".json"));
    REQUIRE_FALSE (isAbandonedSafeWriteTemp ("._Friday gig.json", "", ".json"));
    REQUIRE_FALSE (isAbandonedSafeWriteTemp ("._temp1a.json", "", ".json"));
}

// Only the working names of what is made after a take; never the finished
// files, never the recording, never a Finder twin.
TEST_CASE (TakeCompleteness_AnUnfinishedExportsWorkingFileIsRecognisedAndNothingElse)
{
    REQUIRE (isAbandonedTakeWorkingFile (".V01_Kitchen-Cam_with-sound.mov"));
    REQUIRE (isAbandonedTakeWorkingFile (".V02_Desk_with-sound.mkv"));
    REQUIRE (isAbandonedTakeWorkingFile ("MIX - Apple Podcasts.wav.part"));
    REQUIRE (isAbandonedTakeWorkingFile ("MIX - Spotify-2.wav.part"));

    REQUIRE_FALSE (isAbandonedTakeWorkingFile ("V01_Kitchen-Cam_with-sound.mov"));
    REQUIRE_FALSE (isAbandonedTakeWorkingFile ("._V01_Kitchen-Cam_with-sound.mov"));
    REQUIRE_FALSE (isAbandonedTakeWorkingFile ("._with-sound.mov"));
    REQUIRE_FALSE (isAbandonedTakeWorkingFile ("V01_Kitchen-Cam.mov"));
    REQUIRE_FALSE (isAbandonedTakeWorkingFile ("MIX - Apple Podcasts.wav"));
    REQUIRE_FALSE (isAbandonedTakeWorkingFile ("MIX.wav"));
    REQUIRE_FALSE (isAbandonedTakeWorkingFile ("MIX.wav.part"));
    REQUIRE_FALSE (isAbandonedTakeWorkingFile ("._MIX - Apple Podcasts.wav.part"));
    REQUIRE_FALSE (isAbandonedTakeWorkingFile ("session.json"));
}

// A file on its way to the card is copied under ".<name>.sobcopy" and renamed
// once it is checked. One left by a crash or a pulled card is never the take:
// the take is still on this computer, so the card's recovery sweep clears it.
TEST_CASE (TakeCompleteness_ACardCopyWorkingFileIsAbandonedWork)
{
    REQUIRE (isAbandonedTakeWorkingFile (".MIX.wav.sobcopy"));
    REQUIRE (isAbandonedTakeWorkingFile (".01_Alice.wav.sobcopy"));
    REQUIRE (isAbandonedTakeWorkingFile (".session.json.sobcopy"));

    // Not hidden: not a name this app writes, so not this app's to clear.
    REQUIRE_FALSE (isAbandonedTakeWorkingFile ("MIX.wav.sobcopy"));
    REQUIRE_FALSE (isAbandonedTakeWorkingFile (".sobcopy"));

    // The marker that says a take still has to be copied is not abandoned.
    REQUIRE_FALSE (isAbandonedTakeWorkingFile (".sobstage-card-copy.json"));
    REQUIRE_FALSE (isAbandonedTakeWorkingFile ("MIX.wav"));
}
