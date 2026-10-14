add_executable(mma_core_tests
    main.cpp
    test_DriftCompensator.cpp
    test_ChannelLayoutAnalyzer.cpp
    test_SampleRateNegotiator.cpp
    test_PolarPatternDetector.cpp
    test_DeadChannelDetector.cpp
    test_Metering.cpp
    test_MonitorBus.cpp
    test_PeakLimiter.cpp
    test_FeedbackDetector.cpp
    test_SessionFolderNaming.cpp
    test_CameraSelection.cpp
    test_AppSettings.cpp
    test_ShowTemplate.cpp
    test_SessionRecovery.cpp
    test_BackgroundRecordWriter.cpp
    test_ActivityJournal.cpp
    test_CardRemovalNotice.cpp
    test_PreflightThroughputTest.cpp
    test_SessionMetadata.cpp
    test_RingBuffer.cpp
    test_DeviceManager.cpp
    test_TakeCompleteness.cpp
    test_RecordingEngine.cpp
    test_SessionWriter.cpp
    test_OutputDeviceSelector.cpp
    test_HeadphoneRouting.cpp
    test_PreflightScratchFile.cpp
    test_CapacityMonitor.cpp
    test_BufferLadder.cpp
    test_CpuPressureMonitor.cpp
    test_MirrorPolicy.cpp
    test_SetupAdvisor.cpp
    test_BusPowerDetector.cpp
    test_TakeCopy.cpp
    test_SampleFormat.cpp
    test_WritePipeline.cpp
    test_StemAligner.cpp
    test_CombinedTake.cpp
    test_Loudness.cpp
    test_TapToNameDetector.cpp
    test_PermissionGuidance.cpp
    test_SystemPermissions.cpp
    test_SleepInhibitor.cpp
    test_HelpTopics.cpp
    test_SetupGuide.cpp
    test_TakeWatchdog.cpp
    test_RecordingProof.cpp
    test_CaptureCoordinator.cpp
    test_DeviceInputStream.cpp
    test_ChannelPlan.cpp
    test_AlsaInputPolicy.cpp
    test_FilesystemStatusProbe.cpp
    test_DetachedRefreshCache.cpp
    test_StreamFailurePolicy.cpp
    test_CallbackWatchdogSleepGuard.cpp
    test_FfmpegLocator.cpp
    test_AlarmTone.cpp
    test_QuitGate.cpp
    test_QuitQuestions.cpp
    test_WriteSafetyActions.cpp
    test_FailedOpenRetry.cpp
    test_BatteryRecordingNotice.cpp
    test_UpdateCheck.cpp
    test_LogSizeBudget.cpp
    test_VolumeCapacity.cpp
    test_TakeFolderTracker.cpp
    test_SlowMicrophoneNotice.cpp
)

target_link_libraries(mma_core_tests PRIVATE mma_core mma_warnings)
target_include_directories(mma_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME mma_core_tests COMMAND mma_core_tests)

# Release packaging checks written in Python (standard library only, so any
# Python 3 runs them): the macOS disk-image window must never put an icon on
# the first-launch command drawn on its background, the newest release
# notes must not link to a file or section that does not exist, and bringing
# a published release page up to date must keep GitHub's generated list and
# the latest release.
find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
    add_test(NAME dmg_layout
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_dmg_layout.py)
    add_test(NAME release_notes_links
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_release_notes.py)
    add_test(NAME release_body
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_release_body.py)
endif()
