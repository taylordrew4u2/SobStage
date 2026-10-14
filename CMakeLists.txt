cmake_minimum_required(VERSION 3.22)

# The Windows download is a portable ZIP, not an installer that can arrange a
# separate Visual C++ runtime. Select MSVC's static runtime before any target
# (including JUCE) is created so a clean Windows machine does not need
# MSVCP140/VCRUNTIME140 DLLs that are absent from the package.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING
    "MSVC runtime linked into SobStage's portable binaries")

# This must be set before project() enables the compiler or CMake will inherit
# the runner SDK's deployment floor. Keep it a cache default so packagers can
# deliberately override it while ordinary macOS builds remain compatible with
# the minimum version in docs/SPEC.md.
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING
    "Minimum macOS version supported by SobStage")

project(SobStage VERSION 1.13.27 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings were off entirely: the build ran at the compiler's defaults, so
# -Wshadow, unused private state and sign-conversion mistakes in the audio path
# had nothing watching for them. Enabled here after checking the whole tree
# compiles clean under them on both GCC and Clang, so the baseline is zero and
# the next one stands out.
#
# Not -Werror: a future compiler is entitled to invent a warning, and a
# toolchain upgrade should not stop the build of an audio recorder. MSVC is
# left at its default because /W4 has not been checked against JUCE's headers
# from here, and a wall of third-party warnings hides our own.
add_library(mma_warnings INTERFACE)
target_compile_options(mma_warnings INTERFACE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall;-Wextra;-Wshadow>)

option(MMA_BUILD_APP "Build the JUCE GUI application (requires network fetch of JUCE)" OFF)
option(MMA_BUILD_TESTS "Build the headless Core/ unit test target" ON)
option(MMA_ALLOW_TEST_INPUTS
    "Allow virtual ALSA recording inputs in test-only binaries" OFF)

# ---------------------------------------------------------------------------
# Core: platform-independent, no-JUCE-dependency logic library.
# Always built -- this is what compiles and is testable on any machine,
# including a headless Linux CI sandbox with no audio hardware and no JUCE.
# ---------------------------------------------------------------------------
add_library(mma_core STATIC
    Source/Core/DriftCompensator.cpp
    Source/Core/ChannelLayoutAnalyzer.cpp
    Source/Core/SampleRateNegotiator.cpp
    Source/Core/PolarPatternDetector.cpp
    Source/Core/DeadChannelDetector.cpp
    Source/Core/BusPowerDetector.cpp
    Source/Core/ControllerContentionDetector.cpp
    Source/Core/Metering.cpp
    Source/Core/MonitorBus.cpp
    Source/Core/FeedbackDetector.cpp
    Source/Core/AlarmTone.cpp
    Source/Core/MixBusLimiter.cpp
    Source/Core/PeakLimiter.cpp
    Source/Core/RingBuffer.cpp
    Source/Core/SessionWriter.cpp
    Source/Core/SessionMetadata.cpp
    Source/Core/SessionFolderNaming.cpp
    Source/Core/CameraSelection.cpp
    Source/Core/AppSettings.cpp
    Source/Core/ShowTemplate.cpp
    Source/Core/SessionRecovery.cpp
    Source/Core/ActivityJournal.cpp
    Source/Core/CardRemovalNotice.cpp
    Source/Core/QuitGate.cpp
    Source/Core/QuitQuestions.cpp
    Source/Core/FailedOpenRetry.cpp
    Source/Core/BatteryRecordingNotice.cpp
    Source/Core/PreflightThroughputTest.cpp
    Source/Core/PortIdentity.cpp
    Source/Core/DeviceManager.cpp
    Source/Core/ChannelPlan.cpp
    Source/Core/TakeCompleteness.cpp
    Source/Core/RecordingEngine.cpp
    Source/Core/OutputDeviceSelector.cpp
    Source/Core/CapacityMonitor.cpp
    Source/Core/BufferLadder.cpp
    Source/Core/CpuPressureMonitor.cpp
    Source/Core/MirrorPolicy.cpp
    Source/Core/TakeFolderTracker.cpp
    Source/Core/WriteSafetyActions.cpp
    Source/Core/SetupAdvisor.cpp
    Source/Core/StemAligner.cpp
    Source/Core/WritePipeline.cpp
    Source/Core/TapToNameDetector.cpp
    Source/Core/PermissionGuidance.cpp
    Source/Core/HelpTopics.cpp
    Source/Core/SetupGuide.cpp
    Source/Core/TakeWatchdog.cpp
    Source/Core/RecordingProof.cpp
    Source/Core/CaptureCoordinator.cpp
    Source/Core/DeviceInputStream.cpp
    Source/Core/CombinedTakePlan.cpp
    Source/Core/FfmpegCommand.cpp
    Source/Core/FfmpegLocator.cpp
    Source/Core/FilesystemStatusProbe.cpp
    Source/Core/TakeCopy.cpp
    Source/Core/LoudnessMeter.cpp
    Source/Core/StreamingTargets.cpp
    Source/Core/UpdateCheck.cpp
    Source/Core/LogSizeBudget.cpp
    Source/Platform/SystemPermissions.cpp
    Source/Platform/SleepInhibitor.cpp
    Source/Platform/VolumeCapacity.cpp
    Source/Platform/NativeCopyFileSystem.cpp
    Source/Platform/AlsaInputPolicy.cpp
    Source/Platform/AlsaBackend.cpp
)

if (MMA_ALLOW_TEST_INPUTS)
    target_compile_definitions(mma_core PRIVATE MMA_ALLOW_TEST_INPUTS=1)
endif()

# PRIVATE: these are how mma_core's own sources are compiled, not a rule
# imposed on anything that links it. The targets built from Tools/ and Tests/
# opt in for themselves below.
target_link_libraries(mma_core PRIVATE mma_warnings)
# ALSA on Linux: the backend is part of mma_core rather than the app target so
# the headless harnesses can drive a REAL OS audio API. It is guarded on
# __linux__, so on macOS and Windows the translation unit is empty.
# SystemPermissions.cpp reads +[AVCaptureDevice authorizationStatusForMediaType:]
# through the Objective-C runtime rather than converting a translation unit to
# Objective-C++ for one enum. That is a link dependency, and mma_core is linked
# by the unit tests and the headless tools as well as by the app -- so it goes
# here, PUBLIC, rather than on the app target alone.
if (APPLE)
    target_link_libraries(mma_core PUBLIC objc)
    # SleepInhibitor.cpp holds IOKit power assertions while a take records
    # (IOPMAssertionCreateWithName). Plain C API, so no Objective-C++; PUBLIC
    # for the same reason as objc above.
    target_link_libraries(mma_core PUBLIC "-framework IOKit" "-framework CoreFoundation")
endif()

if (UNIX AND NOT APPLE)
    find_package(ALSA)

    if (ALSA_FOUND)
        target_link_libraries(mma_core PUBLIC ALSA::ALSA)
    else()
        message(STATUS "ALSA not found: the Linux audio backend will be unavailable")
        target_compile_definitions(mma_core PUBLIC MMA_NO_ALSA=1)
    endif()
endif()

target_include_directories(mma_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty
)

if (MMA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()

# ---------------------------------------------------------------------------
# The full JUCE GUI application. Off by default because it requires fetching
# JUCE over the network (FetchContent) which is not available in every build
# environment (e.g. this sandbox). Enable with -DMMA_BUILD_APP=ON on a
# machine with network access and a real JUCE-supported target OS.
# ---------------------------------------------------------------------------
if (MMA_BUILD_APP)
    include(FetchContent)

    # Official release source bundles carry the exact pinned JUCE tree here so
    # their corresponding source builds without relying on a mutable network
    # download. A normal repository checkout continues to fetch the immutable
    # commit below.
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/JUCE/CMakeLists.txt")
        set(FETCHCONTENT_SOURCE_DIR_JUCE
            "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/JUCE")
    endif()

    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        # Immutable commit behind the upstream 7.0.12 tag.
        GIT_TAG        4f43011b96eb0636104cb3e433894cda98243626
    )
    # Populated and added manually rather than via MakeAvailable so JUCE's own
    # install() rules stay out of our install tree. MakeAvailable would put
    # JUCE's CMake package files, .nib templates and helper sources into
    # dist/lib/, which is clutter for anyone downloading a built app.
    FetchContent_GetProperties(JUCE)

    if (NOT juce_POPULATED)
        FetchContent_Populate(JUCE)
    endif()

    include(cmake/ApplyJucePatches.cmake)
    sobstage_apply_juce_camera_patch ("${juce_SOURCE_DIR}")

    if (NOT TARGET juce::juce_core)
        add_subdirectory(${juce_SOURCE_DIR} ${juce_BINARY_DIR} EXCLUDE_FROM_ALL)
    endif()

    # ICON_BIG makes JUCE generate the platform icon containers itself -- a
    # .icns inside the macOS bundle, an .ico linked into the Windows exe -- from
    # one source PNG. Resources/AppIcon.png is generated by Tools/make_icon.py
    # from the same §9.2 palette the UI uses, so it is reviewable as source
    # rather than checked in as an opaque binary nobody can regenerate.
    juce_add_gui_app(SobStage
        PRODUCT_NAME "SobStage"
        COMPANY_NAME "SobStage"
        BUNDLE_ID "com.taylordrew.sobstage"
        COMPANY_COPYRIGHT "Copyright (c) 2026 Taylor Drew Kozero"
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "SobStage records from external USB, FireWire, and Thunderbolt microphones and audio interfaces you connect."
        # A newly seen camera is opt-in; once enabled it is remembered and may
        # reopen on later launches until switched off. macOS still wants the
        # usage string declared up front, and §10.1 wants the reason to be the
        # one the user would have guessed.
        CAMERA_PERMISSION_ENABLED TRUE
        CAMERA_PERMISSION_TEXT "SobStage records video from the cameras you switch on, alongside your microphone recordings."
        HARDENED_RUNTIME_ENABLED TRUE
        HARDENED_RUNTIME_OPTIONS
            com.apple.security.device.audio-input
            com.apple.security.device.camera
        # Declare the supported macOS floor in Info.plist so Finder refuses an
        # older Mac with a clear message instead of a crash at launch. JUCE
        # parses this plist first and then only adds the keys it does not
        # already hold, so the privacy strings above are kept (checked by the
        # juce_info_plist_minimum_macos test and Tools/verify_macos_release.sh).
        # macOS also turns gesture Reactions (fireworks, balloons, confetti) on
        # for every app and draws them into the camera frames, so a performer's
        # thumbs-up would be burned into the take. Since macOS 14.4 this key
        # makes them default off for SobStage; a choice the user already made
        # in the menu-bar Video menu still wins.
        PLIST_TO_MERGE "<plist><dict><key>LSMinimumSystemVersion</key><string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string><key>NSCameraReactionEffectGesturesEnabledDefault</key><false/></dict></plist>"
        ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon.png"
    )

    target_sources(SobStage PRIVATE
        Source/Platform/CoreAudioBackend.cpp
        Source/Platform/WasapiAsioBackend.cpp
        Source/Platform/NullBackend.cpp
        Source/Platform/MacSystemAggregateDevice.cpp
        Source/Platform/NullSystemAggregateDevice.cpp
        Source/Platform/ReducedMotion.cpp
        Source/Platform/SystemThermalState.cpp
        Source/Platform/AsioOutputBackend.cpp
        Source/Platform/LicensedVirtualCableBackend.cpp
        Source/Platform/AttestationDriverBackend.cpp
        Source/UI/AppLookAndFeel.cpp
        Source/UI/ChannelMeterComponent.cpp
        Source/UI/MixBarComponent.cpp
        Source/UI/MainScreen.cpp
        Source/UI/AdvancedPanel.cpp
        Source/UI/HelpPanel.cpp
        Source/UI/SetupGuidePanel.cpp
        Source/UI/CameraPanel.cpp
        Source/UI/ModalCard.cpp
        Source/UI/SaveLocationPrompt.cpp
        Source/UI/SavedTakePanel.cpp
        Source/UI/RecoveredTakesPanel.cpp
        Source/UI/TakeAlertCard.cpp
        Source/UI/TakeBanner.cpp
        Source/UI/MainComponent.cpp
        Source/UI/Main.cpp
        Source/App/Application.cpp
        Source/App/CameraController.cpp
        Source/App/TakeCombiner.cpp
        Source/App/PodcastExport.cpp
        Source/App/CardCopier.cpp
        Source/App/AlarmSpeaker.cpp
    )

    # The combined video on the Mac is made with AVFoundation, which every Mac
    # has, rather than ffmpeg, which almost none do. Proven against the real
    # framework by mac_movie_combiner_check below.
    if (APPLE)
        target_sources(SobStage PRIVATE Source/Platform/MacMovieCombiner.mm)
        set_source_files_properties(Source/Platform/MacMovieCombiner.mm
                                    Tools/mac_movie_combiner_check.mm
                                    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_compile_definitions(SobStage PRIVATE MMA_NATIVE_MOVIE_COMBINER=1)
        target_link_libraries(SobStage PRIVATE
            "-framework AVFoundation" "-framework CoreMedia" "-framework Foundation")

        # Whether the window can be seen at all (covered, another Space), so
        # the meters stop repainting for nobody. Elsewhere the header's inline
        # stub reports "visible".
        target_sources(SobStage PRIVATE Source/Platform/WindowOcclusion.mm)
        set_source_files_properties(Source/Platform/WindowOcclusion.mm
                                    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(SobStage PRIVATE "-framework AppKit")

        # A quit question asked while the app is hidden or behind another app
        # unhides it and bounces its Dock icon. Elsewhere the header's inline
        # stub does nothing.
        target_sources(SobStage PRIVATE Source/Platform/AppActivation.mm)
        set_source_files_properties(Source/Platform/AppActivation.mm
                                    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

        add_executable(mac_movie_combiner_check
            Tools/mac_movie_combiner_check.mm
            Source/Platform/MacMovieCombiner.mm)
        target_include_directories(mac_movie_combiner_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
        target_link_libraries(mac_movie_combiner_check PRIVATE
            "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo" "-framework Foundation")
        add_test(NAME mac_movie_combiner_check COMMAND mac_movie_combiner_check)
        set_tests_properties(mac_movie_combiner_check PROPERTIES TIMEOUT 120)
    endif()

    target_compile_definitions(SobStage PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        # The one place the version is written down is project() above. Anything
        # that repeats it by hand goes stale the first time it is not updated in
        # lockstep -- which is exactly what happened to the window title, stuck
        # reading 0.1.0 while the app shipped 0.9.x.
        SOBSTAGE_VERSION_STRING="${PROJECT_VERSION}"
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:SobStage,JUCE_PRODUCT_NAME>"
    )

    # Test builds only: lets Tools/e2e_card_stall.sh measure how long the
    # window stops answering. Never in a production package, which is built
    # without MMA_ALLOW_TEST_INPUTS.
    if (MMA_ALLOW_TEST_INPUTS)
        target_compile_definitions(SobStage PRIVATE MMA_STALL_METER=1)
        # Tools/e2e_ui_walk.sh: the app walks its own screens. Inert unless
        # MMA_UI_WALK_REPORT is set; see Source/UI/UiWalker.h.
        target_compile_definitions(SobStage PRIVATE MMA_UI_WALK=1)
    endif()

    # Camera capture, on the two desktop targets §11 names. JUCE implements
    # CameraDevice on macOS and Windows only, so elsewhere -- a Linux build,
    # which is what the headless checks compile -- CameraController compiles to
    # its unsupported form and says so in one sentence rather than presenting
    # controls that cannot work.
    if (APPLE OR WIN32)
        target_compile_definitions(SobStage PRIVATE JUCE_USE_CAMERA=1)
        target_link_libraries(SobStage PRIVATE juce::juce_video)
    endif()

    target_link_libraries(SobStage PRIVATE
        mma_core
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_gui_extra
    )

    # -----------------------------------------------------------------------
    # Camera path, compiled where it otherwise could not be.
    #
    # JUCE only implements CameraDevice on macOS and Windows, so on Linux the
    # whole camera path is #if'd out and nothing checks it. This target
    # compiles CameraController.cpp unmodified with JUCE_USE_CAMERA=1 against
    # Simulation/Camera's stand-in juce_video, so a wrong signature or a stale
    # call fails the build on every platform rather than on the one machine
    # that happens to be a Mac.
    #
    # The runnable simulator below supplies a scoped JUCE message manager and
    # lightweight Components, so it verifies native-viewer ownership and UI
    # handoff as well as compiling the shipping controller surface.
    # -----------------------------------------------------------------------
    # A headless layout probe: MainScreen laid out with camera tiles, reporting
    # the sizes it actually produces. Component layout is arithmetic, so this
    # needs no display -- which is what lets the camera row be checked on a
    # machine where no camera can render.
    add_executable(layout_probe Tools/layout_probe.cpp
        Source/UI/AdvancedPanel.cpp
        Source/UI/MainScreen.cpp
        Source/UI/CameraPanel.cpp
        Source/UI/ChannelMeterComponent.cpp
        Source/UI/MixBarComponent.cpp
        Source/UI/AppLookAndFeel.cpp
        Source/UI/ModalCard.cpp
        Source/UI/SaveLocationPrompt.cpp
        Source/UI/SetupGuidePanel.cpp
        Source/UI/RecoveredTakesPanel.cpp
        Source/UI/SavedTakePanel.cpp
        Source/UI/TakeAlertCard.cpp)
    target_compile_definitions(layout_probe PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)
    target_include_directories(layout_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_link_libraries(layout_probe PRIVATE mma_core juce::juce_gui_basics)
    if (MMA_BUILD_TESTS)
        add_test (NAME layout_probe COMMAND layout_probe)
    endif()

    # The channel strip's crying face, driven by a real Metering through the
    # strip's own 60 Hz timer and checked from its pixels in every state:
    # asleep, frown, the -18 dBFS tear line, a sob on clip, the clip clearing,
    # and every strip size. Headless, like layout_probe. Modal loops are on
    # for this one target only, so the simulation can pump the message loop
    # for a set time between states.
    add_executable(sim_channel_meter Tools/sim_channel_meter.cpp
        Source/UI/ChannelMeterComponent.cpp
        Source/UI/AppLookAndFeel.cpp)
    target_compile_definitions(sim_channel_meter PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_include_directories(sim_channel_meter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_link_libraries(sim_channel_meter PRIVATE mma_core juce::juce_gui_basics)
    if (MMA_BUILD_TESTS)
        add_test (NAME sim_channel_meter COMMAND sim_channel_meter)
    endif()

    if (NOT WIN32)
        # Runnable, not merely compiled. The stand-in camera layer takes a
        # settable device list, so arrival, removal, duplicate names and a list
        # reordering between discovery and open are driven without touching real
        # camera hardware. The fake also creates lightweight viewer components
        # to drive persistent-preview ownership and failure/retry policy. Windows
        # needs COM linkage this portable
        # harness does not provide, so its normal app target covers the source.
        add_executable(sim_camera
            Tools/sim_camera.cpp
            Source/App/CameraController.cpp
            Simulation/Camera/FakeCameraDevice.cpp)
        target_compile_definitions(sim_camera PRIVATE
            SOBSTAGE_CAMERA_SIMULATION=1
            JUCE_USE_CAMERA=1
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_STANDALONE_APPLICATION=1)
        # Ahead of everything, so juce_video/juce_video.h resolves to the stub.
        target_include_directories(sim_camera BEFORE PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Simulation/Camera
            ${CMAKE_CURRENT_SOURCE_DIR}/Source)
        target_link_libraries(sim_camera PRIVATE mma_core juce::juce_gui_basics)
        add_test(NAME sim_camera COMMAND sim_camera)

        # JUCE's Windows camera writer has a synchronous start/stop contract.
        # The pinned patch publishes callbacks only around successful graph
        # creation/removal; drive that timing on every CI host so Windows
        # cannot silently stay STARTING or claim a failed writer.
        add_executable(sim_camera_sync_lifecycle
            Tools/sim_camera_sync_lifecycle.cpp
            Source/App/CameraController.cpp
            Simulation/Camera/FakeCameraDevice.cpp)
        target_compile_definitions(sim_camera_sync_lifecycle PRIVATE
            SOBSTAGE_CAMERA_SIMULATION=1
            JUCE_USE_CAMERA=1
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_STANDALONE_APPLICATION=1)
        target_include_directories(sim_camera_sync_lifecycle BEFORE PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Simulation/Camera
            ${CMAKE_CURRENT_SOURCE_DIR}/Source)
        target_link_libraries(sim_camera_sync_lifecycle PRIVATE mma_core juce::juce_gui_basics)
        add_test(NAME sim_camera_sync_lifecycle COMMAND sim_camera_sync_lifecycle)
    endif()

    # Static text guard for the macOS camera heartbeat fix in the JUCE patch
    # (MAC-CAM-1). The Objective-C++ backend itself only compiles on a Mac.
    add_test(NAME juce_camera_patch_invariants
        COMMAND ${CMAKE_COMMAND}
            -DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Patches/juce-7.0.12-camera-lifecycle.patch
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckJucePatchInvariants.cmake)

    # macOS holds Record while the microphone prompt is unanswered (the
    # streams carry silence until then). The gate is JUCE_MAC-only code.
    add_test(NAME mac_microphone_prompt_gate
        COMMAND ${CMAKE_COMMAND}
            -DAPPLICATION_CPP=${CMAKE_CURRENT_SOURCE_DIR}/Source/App/Application.cpp
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckMicPromptGate.cmake)

    # The 90% mix-only fallback and the backup's 1 GB stop must run before
    # any warning branch that can return on every status poll.
    add_test(NAME write_safety_poll_order
        COMMAND ${CMAKE_COMMAND}
            -DAPPLICATION_CPP=${CMAKE_CURRENT_SOURCE_DIR}/Source/App/Application.cpp
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckWriteSafetyOrder.cmake)

    # The Mac sleeping mid-take goes into the take's record and marks no
    # microphone dead; the policy is unit-tested, this pins that the
    # stream-failure loop applies it.
    add_test(NAME system_slept_take_note
        COMMAND ${CMAKE_COMMAND}
            -DAPPLICATION_CPP=${CMAKE_CURRENT_SOURCE_DIR}/Source/App/Application.cpp
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckSystemSleptNote.cmake)

    # A rate-change stop waits for the rest of the poll's stream reports, so
    # none of them is applied to the streams Stop has just reopened.
    add_test(NAME rate_change_stop_after_batch
        COMMAND ${CMAKE_COMMAND}
            -DAPPLICATION_CPP=${CMAKE_CURRENT_SOURCE_DIR}/Source/App/Application.cpp
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckRateChangeStopAfterBatch.cmake)

    # The headphone output held off the clock on the platform's word of a
    # rate change: neither lost to the rescan nor the computer's fault to the
    # take's health and the buffer ladder.
    add_test(NAME output_rate_hold
        COMMAND ${CMAKE_COMMAND}
            -DAPPLICATION_CPP=${CMAKE_CURRENT_SOURCE_DIR}/Source/App/Application.cpp
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckOutputRateHold.cmake)

    # The combined device's per-jack headphone gains follow every read of its
    # layout and reach only the stream open on it. HeadphoneJacks and the
    # coordinator are unit-tested; this pins that Application goes through
    # them.
    add_test(NAME headphone_jack_wiring
        COMMAND ${CMAKE_COMMAND}
            -DAPPLICATION_CPP=${CMAKE_CURRENT_SOURCE_DIR}/Source/App/Application.cpp
            -DAPPLICATION_H=${CMAKE_CURRENT_SOURCE_DIR}/Source/App/Application.h
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckHeadphoneJackWiring.cmake)

    # The combined device is never destroyed under the take's headphone mix,
    # judged by the stream open on it rather than the output chosen. The
    # decision is unit-tested; this pins that Application goes by it.
    add_test(NAME combined_device_remake_guard
        COMMAND ${CMAKE_COMMAND}
            -DAPPLICATION_CPP=${CMAKE_CURRENT_SOURCE_DIR}/Source/App/Application.cpp
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckCombinedDeviceRemake.cmake)

    # A rig with no microphone open is retried from the status poll; the
    # policy is unit-tested, this pins that the poll runs it.
    add_test(NAME failed_open_retry_poll
        COMMAND ${CMAKE_COMMAND}
            -DAPPLICATION_CPP=${CMAKE_CURRENT_SOURCE_DIR}/Source/App/Application.cpp
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckFailedOpenRetry.cmake)

    # MAC-PKG-2: the macOS Info.plist must carry LSMinimumSystemVersion and
    # still keep JUCE's microphone/camera usage strings. juceaide writes the
    # plist only on Apple builds, so run it here on SobStage's Info.txt.
    get_target_property(sobstage_juce_info_file SobStage JUCE_INFO_FILE)
    add_test(NAME juce_info_plist_minimum_macos
        COMMAND ${CMAKE_COMMAND}
            -DJUCEAIDE=$<TARGET_FILE:juce::juceaide>
            -DINFO_FILE=${sobstage_juce_info_file}
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/juce_info_plist_check/Info.plist
            -DEXPECTED_MINIMUM_MACOS=${CMAKE_OSX_DEPLOYMENT_TARGET}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckInfoPlist.cmake)

    # A real ffmpeg process can remain alive forever on a damaged removable
    # take. This executable invokes itself as that wedged child and proves that
    # TakeCombiner destruction never joins it and leaves no partial output.
    add_executable(sim_take_combiner
        Tools/sim_take_combiner.cpp
        Source/App/TakeCombiner.cpp)
    target_compile_definitions(sim_take_combiner PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)
    target_include_directories(sim_take_combiner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_link_libraries(sim_take_combiner PRIVATE mma_core juce::juce_core)
    add_test(NAME sim_take_combiner COMMAND sim_take_combiner)
    set_tests_properties(sim_take_combiner PROPERTIES TIMEOUT 10)

    # TakeCombiner built the way the Mac app builds it (its own AVFoundation
    # combine, MMA_NATIVE_MOVIE_COMBINER), against a stand-in for
    # MacMovieCombiner.mm, so the Mac branch runs on every platform: a take
    # started mid-combine gets the card, and the combine is made afterwards.
    add_executable(sim_take_combiner_native
        Tools/sim_take_combiner_native.cpp
        Source/App/TakeCombiner.cpp)
    target_compile_definitions(sim_take_combiner_native PRIVATE
        MMA_NATIVE_MOVIE_COMBINER=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)
    target_include_directories(sim_take_combiner_native PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_link_libraries(sim_take_combiner_native PRIVATE mma_core juce::juce_core)
    add_test(NAME sim_take_combiner_native COMMAND sim_take_combiner_native)
    set_tests_properties(sim_take_combiner_native PROPERTIES TIMEOUT 30)

    # settings.json, session.json and saved shows are replaced whole or not at
    # all: a full disk (simulated with RLIMIT_FSIZE) must leave the previous
    # file intact rather than renaming an empty one over it.
    if (UNIX)
        add_executable(sim_checked_text_write Tools/sim_checked_text_write.cpp)
        target_compile_definitions(sim_checked_text_write PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_STANDALONE_APPLICATION=1)
        target_include_directories(sim_checked_text_write PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Source)
        target_link_libraries(sim_checked_text_write PRIVATE juce::juce_core)
        add_test(NAME sim_checked_text_write COMMAND sim_checked_text_write)
        set_tests_properties(sim_checked_text_write PROPERTIES TIMEOUT 20)
    endif()

    # The podcast-ready copy written after a take: a quiet mono mix comes out
    # at each target's mono loudness with its true peak under the ceiling, a
    # transient-heavy one is limited onto the target rather than left short, a
    # split mix is read whole, and a silent or too-short one writes nothing.
    add_executable(sim_podcast_export
        Tools/sim_podcast_export.cpp
        Source/App/PodcastExport.cpp)
    target_compile_definitions(sim_podcast_export PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)
    target_include_directories(sim_podcast_export PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_link_libraries(sim_podcast_export PRIVATE
        mma_core juce::juce_core juce::juce_audio_formats)
    add_test(NAME sim_podcast_export COMMAND sim_podcast_export)
    set_tests_properties(sim_podcast_export PROPERTIES TIMEOUT 60)

    # Record on this computer, then copy to the card: CardCopier against the
    # real disk in a temporary folder, every call countable and holdable. A
    # copy held for a take makes no card call, one held mid-file stops at a
    # chunk and leaves nothing half-written, quitting while the card is stuck
    # in a write does not wait for it, a second pass after a combined video
    # reads back only what is new, and the system's own EFBIG reaches the
    # right words.
    add_executable(sim_card_copy
        Tools/sim_card_copy.cpp
        Source/App/CardCopier.cpp)
    target_include_directories(sim_card_copy PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_link_libraries(sim_card_copy PRIVATE mma_core mma_warnings)
    add_test(NAME sim_card_copy COMMAND sim_card_copy)
    set_tests_properties(sim_card_copy PROPERTIES TIMEOUT 30)

    # Its Windows branches, compiled here for syntax: the Windows runner is
    # otherwise the first place a name scoped inside `#if ! defined(_WIN32)`
    # is found to be missing. GCC only -- its errors in the system's own
    # headers are told apart from the project's by path.
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT WIN32)
        add_test(NAME windows_branches_compile
            COMMAND ${CMAKE_COMMAND}
                -DCOMPILER=${CMAKE_CXX_COMPILER}
                -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DFILES=Tools/sim_card_copy.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckWindowsBranches.cmake)
    endif()

    if (WIN32)
        # AvSetMmThreadCharacteristics: Pro Audio scheduling for the WASAPI
        # worker. Without it the OS will not wake a small exclusive-mode buffer
        # reliably enough to hold the §5.4 latency budget.
        target_link_libraries(SobStage PRIVATE avrt cfgmgr32)
    endif()

    # -----------------------------------------------------------------------
    # Install / package. juce_add_gui_app puts the real product under
    # <target>_artefacts/<config>/ -- a .app bundle on macOS, a plain
    # executable elsewhere -- so the install rule copies from there rather than
    # from the raw target, which on macOS would install the binary inside the
    # bundle and leave the bundle behind.
    # -----------------------------------------------------------------------
    if (APPLE)
        # TARGET_BUNDLE_DIR names the .app itself. Deriving it from
        # TARGET_FILE_DIR/../.. instead would install the bundle's *contents*
        # loose into the prefix, because CMake takes the destination name from
        # the last path component -- which would be "..".
        install(DIRECTORY "$<TARGET_BUNDLE_DIR:SobStage>"
                DESTINATION .
                USE_SOURCE_PERMISSIONS)
    else()
        install(PROGRAMS "$<TARGET_FILE:SobStage>" DESTINATION bin)
    endif()

    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSING.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/SUPPORT.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/PRIVACY.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/RELEASE_CHECKLIST.md"
        DESTINATION .)

    set(CPACK_PACKAGE_NAME "SobStage")
    set(CPACK_PACKAGE_VENDOR "SobStage")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Record up to 8 USB microphones as separate tracks, and hear them all live")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_GENERATOR "ZIP")
    include(CPack)
endif()

# ---------------------------------------------------------------------------
# Harnesses that drive the real capture path with synthetic audio. Not unit
# tests: they take minutes and answer questions unit tests cannot -- whether
# the §3.2 loop actually settles, and whether §3.4's 1 ms bound holds over a
# four-hour take. Both found real bugs (integral windup, and a pre-roll that
# was costing 21 ms of the §5.4 monitor budget).
#
#   ./e2e_capture <dir>   two mics, mismatched clocks, decode and check the WAVs
#   ./soak_drift  <hours> four dissimilar clocks, inter-channel drift at the end
# ---------------------------------------------------------------------------
option(MMA_BUILD_TOOLS "Build the capture harnesses (see Tools/)" ON)

if (MMA_BUILD_TOOLS)
    add_executable(e2e_capture Tools/e2e_capture.cpp)
    target_link_libraries(e2e_capture PRIVATE mma_core)

    # The ALSA worker's recovery path, driven against a real libasound with one
    # symbol interposed. Linux only: it is the only platform with an ALSA
    # backend to drive, and LD_PRELOAD is how the one unreachable branch is
    # reached without faking the other thirty-seven calls around it.
    if (UNIX AND NOT APPLE AND MMA_ALLOW_TEST_INPUTS)
        add_executable(alsa_recovery_death Tools/alsa_recovery_death.cpp)
        target_link_libraries(alsa_recovery_death PRIVATE mma_core)
        target_link_libraries(alsa_recovery_death PRIVATE mma_warnings)

        add_executable(alsa_mid_take_loss Tools/alsa_mid_take_loss.cpp)
        target_link_libraries(alsa_mid_take_loss PRIVATE mma_core)
        target_link_libraries(alsa_mid_take_loss PRIVATE mma_warnings)

        add_executable(disk_full_take Tools/disk_full_take.cpp)
        target_link_libraries(disk_full_take PRIVATE mma_core)
        target_link_libraries(disk_full_take PRIVATE mma_warnings)

        add_executable(alsa_monitor_loss Tools/alsa_monitor_loss.cpp)
        target_link_libraries(alsa_monitor_loss PRIVATE mma_core)
        target_link_libraries(alsa_monitor_loss PRIVATE mma_warnings)

        add_executable(alsa_rate_refusal Tools/alsa_rate_refusal.cpp)
        target_link_libraries(alsa_rate_refusal PRIVATE mma_core)
        target_link_libraries(alsa_rate_refusal PRIVATE mma_warnings)

        # A multi-input interface held open while it is re-probed; see the file.
        add_executable(alsa_busy_probe Tools/alsa_busy_probe.cpp)
        target_link_libraries(alsa_busy_probe PRIVATE mma_core)
        target_link_libraries(alsa_busy_probe PRIVATE mma_warnings)

        # A recording card that stops answering on demand; see the file.
        add_library(fs_stall_shim SHARED Tools/fs_stall_shim.cpp)
        target_link_libraries(fs_stall_shim PRIVATE ${CMAKE_DL_LIBS})

        # The close button, for gates under Xvfb with no window manager; see
        # the file. Only where X11's own headers are installed.
        find_package(X11 QUIET)
        if (X11_FOUND)
            add_executable(x11_window_message Tools/x11_window_message.cpp)
            target_include_directories(x11_window_message PRIVATE ${X11_INCLUDE_DIR})
            target_link_libraries(x11_window_message PRIVATE ${X11_LIBRARIES})
        endif()

        add_library(alsa_readi_shim SHARED Tools/alsa_readi_shim.cpp)
        target_link_libraries(alsa_readi_shim PRIVATE ${CMAKE_DL_LIBS})
        if (ALSA_FOUND)
            target_link_libraries(alsa_readi_shim PRIVATE ALSA::ALSA)
        endif()
    endif()

    add_executable(soak_drift Tools/soak_drift.cpp)
    target_link_libraries(soak_drift PRIVATE mma_core)

    # The same loop against devices that deliver whole blocks on their own
    # clocks, with the pull clock's phase slipping against them -- which is
    # what soak_drift's lockstep feeding cannot produce, and what made the loop
    # limit-cycle in the real app. Deterministic, sub-second, a test.
    add_executable(sim_drift_loop Tools/sim_drift_loop.cpp)
    target_link_libraries(sim_drift_loop PRIVATE mma_core mma_warnings)
    # No jitter, then the jitter each rung of §5.4's buffer ladder is expected
    # to absorb: stalls up to about a block and a half over per-event jitter of
    # a quarter block or so. What a rung cannot absorb is the next rung's job.
    add_test(NAME sim_drift_loop COMMAND sim_drift_loop 300)
    add_test(NAME sim_drift_loop_jitter_64 COMMAND sim_drift_loop 300 64 150,-150,60,-60,0 -1 -1 0.5 1.0 0.2)
    add_test(NAME sim_drift_loop_jitter_128 COMMAND sim_drift_loop 300 128 150,-150,60,-60,0 -1 -1 1.0 2.0 0.2)
    add_test(NAME sim_drift_loop_jitter_256 COMMAND sim_drift_loop 300 256 150,-150,60,-60,0 -1 -1 1.0 4.0 0.2)
    add_test(NAME sim_drift_loop_jitter_512 COMMAND sim_drift_loop 300 512 150,-150,60,-60,0 -1 -1 2.0 8.0 0.3)

    # The two limiters every take and every headphone feed go through, driven
    # with eight full-scale microphones, hard trims, clicks, and a driver that
    # hands over NaN and infinity. Reads the files back from disk. Seconds.
    add_executable(sim_mix_bus Tools/sim_mix_bus.cpp)
    target_link_libraries(sim_mix_bus PRIVATE mma_core mma_warnings)
    add_test(NAME sim_mix_bus COMMAND sim_mix_bus)

    # Registered, not merely built. The note above says these take minutes,
    # which is the reason nothing ever ran them -- and measured, the pair takes
    # under twenty seconds: e2e_capture 0.09 s, and soak_drift's FULL four-hour
    # §3.4 acceptance run 16.7 s, because the four hours are simulated rather
    # than waited out. Two harnesses that between them found integral windup
    # and a 21 ms pre-roll were compiled on every push and executed on none of
    # it.
    #
    # e2e_capture is given an explicit directory: its default is /tmp/e2e,
    # which does not exist on Windows, so leaving it implicit would register a
    # test that could only fail there.
    add_test(NAME e2e_capture COMMAND e2e_capture "${CMAKE_CURRENT_BINARY_DIR}/e2e-capture-out")

    # Four hours, the §3.4 acceptance figure. Anything less is a different
    # question from the one the ceiling is written about.
    add_test(NAME soak_drift COMMAND soak_drift 4)

    # Linux only: drives the real ALSA backend against the virtual devices
    # Tools/setup_alsa_fixture.sh creates.
    if (UNIX AND NOT APPLE AND ALSA_FOUND)
        add_executable(live_capture Tools/live_capture.cpp)
        target_link_libraries(live_capture PRIVATE mma_core)
    target_link_libraries(live_capture PRIVATE mma_warnings)

        # The drift loop on real, independent clocks: a time series of every
        # ring and every PI loop, for reading rather than for pass/fail. Run
        # under libalsa_readi_shim.so with MMA_SIM_REALTIME=1.
        add_executable(clock_trace Tools/clock_trace.cpp)
        target_link_libraries(clock_trace PRIVATE mma_core mma_warnings)
    endif()

    # -----------------------------------------------------------------------
    # Platform simulation.
    #
    # CoreAudioBackend and WasapiAsioBackend can only be compiled on their own
    # OS, so on every other machine they were unverified by construction --
    # which is how five user-facing defects lived in them undetected.
    #
    # Simulation/ supplies stand-in OS headers plus a configurable virtual
    # device layer. These targets compile the backend sources UNMODIFIED against
    # those headers, so the code under test is the code that ships; only the
    # operating system underneath it is fake. They build and run anywhere,
    # including on the OS the backend does not target.
    #
    #   ./sim_coreaudio   interleaved buffers, rate ranges, hog mode, hotplug
    #   ./sim_wasapi      exclusive-mode format negotiation, PCM conversion
    # -----------------------------------------------------------------------
    add_executable(sim_coreaudio
        Tools/sim_coreaudio.cpp
        Source/Platform/CoreAudioBackend.cpp
        Source/Platform/MacSystemAggregateDevice.cpp
        Source/Core/OutputDeviceSelector.cpp
        Simulation/CoreAudio/FakeCoreAudio.cpp)
    target_compile_definitions(sim_coreaudio PRIVATE MMA_SIMULATE_MAC=1)
    target_include_directories(sim_coreaudio PRIVATE Simulation/CoreAudio Source)
    target_link_libraries(sim_coreaudio PRIVATE mma_warnings)

    # The macOS path, end to end: the real backend, the real coordinator and the
    # real writer, against the virtual HAL. sim_coreaudio checks the backend and
    # live_capture checks the coordinator, and nothing joined them -- which is
    # how a stereo device recording at 24 bits reached a user untested.
    add_executable(sim_capture_mac
        Tools/sim_capture_mac.cpp
        Source/Platform/CoreAudioBackend.cpp
        Simulation/CoreAudio/FakeCoreAudio.cpp)
    target_compile_definitions(sim_capture_mac PRIVATE MMA_SIMULATE_MAC=1)
    target_include_directories(sim_capture_mac PRIVATE Simulation/CoreAudio Source)
    target_link_libraries(sim_capture_mac PRIVATE mma_core)
    target_link_libraries(sim_capture_mac PRIVATE mma_warnings)

    find_package(Threads REQUIRED)

    # The Windows path, end to end: the real backend, the real coordinator and
    # the real writer, against the virtual audio service. sim_wasapi checks the
    # backend and e2e_capture checks the coordinator, and nothing joined them --
    # the same gap that let a stereo device at 24 bits reach a user untested on
    # macOS, which is why sim_capture_mac exists.
    add_executable(sim_capture_win
        Tools/sim_capture_win.cpp
        Source/Platform/WasapiAsioBackend.cpp
        Simulation/Wasapi/FakeWasapi.cpp)
    target_compile_definitions(sim_capture_win PRIVATE MMA_SIMULATE_WINDOWS=1)
    target_include_directories(sim_capture_win PRIVATE Simulation/Wasapi Source)
    target_link_libraries(sim_capture_win PRIVATE mma_core Threads::Threads mma_warnings)

    add_executable(sim_wasapi
        Tools/sim_wasapi.cpp
        Source/Platform/WasapiAsioBackend.cpp
        Simulation/Wasapi/FakeWasapi.cpp)
    target_compile_definitions(sim_wasapi PRIVATE MMA_SIMULATE_WINDOWS=1)
    target_include_directories(sim_wasapi PRIVATE Simulation/Wasapi Source)
    target_link_libraries(sim_wasapi PRIVATE Threads::Threads)
    target_link_libraries(sim_wasapi PRIVATE mma_warnings)

    # Registered with ctest rather than run by hand, so they execute on every
    # platform's CI job through the same command as the unit tests -- and so a
    # regression in either backend fails the build on Linux, macOS and Windows
    # alike, not only on the one OS that can compile it natively.
    if (MMA_BUILD_TESTS)
        add_test(NAME sim_coreaudio COMMAND sim_coreaudio)
        add_test(NAME sim_capture_mac COMMAND sim_capture_mac)
        add_test(NAME sim_wasapi COMMAND sim_wasapi)
        add_test(NAME sim_capture_win COMMAND sim_capture_win)
    endif()
endif()
