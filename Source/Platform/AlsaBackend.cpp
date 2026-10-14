#include "AlsaBackend.h"

#if defined(__linux__) && ! defined(MMA_NO_ALSA)

#include "AlsaInputPolicy.h"
#include "../Core/DeviceInputStream.h" // kSourceBufferBlocks: the driver's ring is sized so the app's own ring can take it whole

#include <alsa/asoundlib.h>
#include <sys/inotify.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <cerrno>    // EBUSY, to tell "in use" from "not there"
#include <climits>   // NAME_MAX, for the inotify read buffer
#include <unistd.h>
#include <pthread.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace mma {

namespace {

#if defined(MMA_ALLOW_TEST_INPUTS)
constexpr bool kTestInputsCompiledIn = true;
#else
constexpr bool kTestInputsCompiledIn = false;
#endif

/// §11: the callback must not allocate, so every buffer a stream needs is
/// sized once at open time and reused for the life of the stream.
struct ConversionBuffers
{
    std::vector<unsigned char> interleaved;    // raw frames in the device's format
    std::vector<float> interleavedFloat;       // the same frames as float, still interleaved
    std::vector<float> planar;                 // per-channel, what the callback contract wants
    std::vector<const float*> inputPointers;
    std::vector<float*> outputPointers;
};

int bytesPerSampleFor (snd_pcm_format_t format) noexcept
{
    switch (format)
    {
        case SND_PCM_FORMAT_S16_LE:   return 2;
        case SND_PCM_FORMAT_S32_LE:   return 4;
        case SND_PCM_FORMAT_FLOAT_LE: return 4;
        default:                      return 0;
    }
}

void toFloat (const unsigned char* src, float* dest, snd_pcm_format_t format, size_t count) noexcept
{
    switch (format)
    {
        case SND_PCM_FORMAT_S16_LE:
        {
            const auto* s = reinterpret_cast<const int16_t*> (src);
            for (size_t i = 0; i < count; ++i)
                dest[i] = static_cast<float> (s[i]) / 32768.0f;
            break;
        }
        case SND_PCM_FORMAT_S32_LE:
        {
            const auto* s = reinterpret_cast<const int32_t*> (src);
            for (size_t i = 0; i < count; ++i)
                dest[i] = static_cast<float> (s[i]) / 2147483648.0f;
            break;
        }
        case SND_PCM_FORMAT_FLOAT_LE:
            std::memcpy (dest, src, count * sizeof (float));
            break;
        default:
            std::fill (dest, dest + count, 0.0f);
            break;
    }
}

void fromFloat (const float* src, unsigned char* dest, snd_pcm_format_t format, size_t count) noexcept
{
    switch (format)
    {
        case SND_PCM_FORMAT_S16_LE:
        {
            auto* d = reinterpret_cast<int16_t*> (dest);
            for (size_t i = 0; i < count; ++i)
                d[i] = static_cast<int16_t> (std::max (-1.0f, std::min (1.0f, src[i])) * 32767.0f);
            break;
        }
        case SND_PCM_FORMAT_S32_LE:
        {
            auto* d = reinterpret_cast<int32_t*> (dest);
            for (size_t i = 0; i < count; ++i)
                d[i] = static_cast<int32_t> (std::max (-1.0f, std::min (1.0f, src[i])) * 2147483647.0);
            break;
        }
        case SND_PCM_FORMAT_FLOAT_LE:
            std::memcpy (dest, src, count * sizeof (float));
            break;
        default:
            std::memset (dest, 0, count * bytesPerSampleFor (format));
            break;
    }
}

/// Best effort: RT scheduling needs privileges this process may not have.
/// Failing is not fatal -- it costs latency headroom, not correctness -- so it
/// is neither retried nor reported as an error.
void requestRealtimePriority() noexcept
{
    sched_param param {};
    param.sched_priority = std::min (80, sched_get_priority_max (SCHED_FIFO));
    pthread_setschedparam (pthread_self(), SCHED_FIFO, &param);
}

} // namespace

struct AlsaStream
{
    snd_pcm_t* pcm = nullptr;
    snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;
    unsigned int channels = 1;
    snd_pcm_uframes_t periodFrames = 0;
    /// What the driver actually granted, or 0 when it would not say. Kept
    /// APART from periodFrames, which sizes the I/O the worker performs: the
    /// two answer different questions and conflating them changes the shape of
    /// every read for the sake of a number that is only reported.
    snd_pcm_uframes_t grantedPeriodFrames = 0;
    /// The kernel's own ring for this stream, as granted. For capture it is
    /// asked for deep -- kSourceBufferBlocks periods -- because it costs no monitor
    /// latency (the worker still reads one period as soon as it is ready) and
    /// it is what stands between a late worker wake and an xrun in the driver,
    /// where the audio is gone before this app ever sees it.
    snd_pcm_uframes_t grantedBufferFrames = 0;
    double sampleRate = 48000.0;
    bool isInput = true;

    AudioCallback callback;
    ConversionBuffers buffers;

    std::thread worker;
    std::atomic<bool> running { false };

    /// §0.1: frames the device dropped and this app never saw. An xrun is
    /// recoverable and the stream carries on, which is exactly why it needs
    /// counting: nothing else in the run leaves a trace of it.
    ///
    /// Capture only. The monitor path's own xruns are audible but are not lost
    /// recording, and mixing the two makes the take's record say something
    /// untrue about the audio on the card.
    std::atomic<uint64_t> framesDropped { 0 };

    /// Recovered xruns on the monitor output: heard, not recorded.
    std::atomic<uint64_t> outputGlitches { 0 };

    ~AlsaStream()
    {
        running.store (false, std::memory_order_release);

        if (worker.joinable())
            worker.join();

        if (pcm != nullptr)
        {
            snd_pcm_drop (pcm);
            snd_pcm_close (pcm);
            pcm = nullptr;
        }
    }
};

/// §2: hotplug from the kernel, never a timer. inotify on /dev/snd fires when
/// a card's device nodes appear or disappear, which is what a USB mic being
/// plugged in actually does.
struct AlsaHotplugWatcher
{
    int fd = -1;
    int wakeFd = -1;
    std::thread worker;
    std::atomic<bool> running { false };

    ~AlsaHotplugWatcher()
    {
        running.store (false, std::memory_order_release);

        // Linux does not promise that close() in this thread interrupts a
        // blocking read() of the same descriptor in another thread. An eventfd
        // is an explicit poll wake-up, so teardown never hangs waiting for the
        // next physical device change.
        if (wakeFd >= 0)
        {
            const uint64_t wake = 1;
            (void) ::write (wakeFd, &wake, sizeof (wake));
        }

        if (worker.joinable())
            worker.join();

        if (fd >= 0)
            ::close (fd);
        if (wakeFd >= 0)
            ::close (wakeFd);
    }
};

namespace {

/// How many inputs a capture device actually presents.
///
/// A mixer or an audio interface is a device with several inputs, and each of
/// them is a person who expects their own track. ALSA's device hints carry no
/// channel count, so the only way to learn it is to ask the PCM -- which means
/// opening it. Opened in NONBLOCK and closed again immediately, exactly as the
/// exclusive-mode check above already does for outputs: what broke recording
/// once before was holding the device open past the probe, not the probe.
///
/// Answers 1 for anything it cannot ask, which is where this started: one
/// microphone is the safe reading of a device that will not say.

/// How long the app waits for a device to finish opening before it gives up on
/// that device and carries on without it.
///
/// Five seconds, matching the Windows backend: long enough that a USB interface
/// still enumerating is not abandoned, short enough that a person does not
/// conclude the app is broken.
constexpr auto kAlsaOpenDeadline = std::chrono::seconds (5);

/// The same bound for the enumeration probe, which only ever asks a device how
/// many inputs it has. Shorter because it runs once per device while the app is
/// starting up and its answer already has a documented fallback -- a device
/// that will not say is one microphone -- so waiting the full five seconds per
/// device buys nothing.
constexpr auto kAlsaProbeDeadline = std::chrono::seconds (2);

/// snd_pcm_open, bounded.
///
/// §0.1 is about not losing audio, and an app that never opens cannot record
/// any. snd_pcm_open is a BLOCKING open: the mode argument here is 0, so for a
/// device whose open cannot complete it does not return at all. It ran on the
/// calling thread, which at start-up is the message thread, so a single wedged
/// microphone held the entire app closed -- no window, no way to record with
/// the microphones that were working perfectly well beside it.
///
/// Reproduced against a real ALSA device whose open blocks (a FIFO with no
/// writer): the main thread parked in fifo_open/wait_for_partner inside
/// openat, two threads alive, and no window after twenty seconds. The code
/// below already anticipated the device being held by something else -- that is
/// the -EBUSY branch its caller has -- but a driver that BLOCKS instead of
/// returning -EBUSY never reaches that branch.
///
/// SND_PCM_NONBLOCK is not the answer and the probes that pass it are not
/// safe either: that flag governs the PCM's data semantics, not the open of
/// whatever backs the device, so the enumeration probe wedged in exactly the
/// same place -- and enumeration runs at start-up, on the message thread. Every
/// snd_pcm_open in this backend goes through here for that reason.
///
/// This is the same hazard the macOS and Windows backends already bound their
/// HAL and COM calls against; ALSA was the one backend still calling straight
/// through. The open runs on a worker with a deadline, and a worker that
/// finishes after the caller has stopped waiting closes the handle it opened
/// rather than leaking it -- ALSA has no reference counting to do that for us.
int openPcmBounded (snd_pcm_t** pcm, const std::string& deviceId,
                    snd_pcm_stream_t direction, int mode,
                    std::chrono::milliseconds deadline, bool& timedOut)
{
    struct Attempt
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool finished = false;
        bool abandoned = false;
        int result = 0;
        snd_pcm_t* pcm = nullptr;
    };

    auto attempt = std::make_shared<Attempt>();
    timedOut = false;

    std::thread worker;
    try
    {
        worker = std::thread ([attempt, deviceId, direction, mode]
        {
            snd_pcm_t* opened = nullptr;
            const int err = snd_pcm_open (&opened, deviceId.c_str(), direction, mode);

            const std::lock_guard<std::mutex> guard (attempt->mutex);

            // Nobody is waiting any more. Whatever the driver eventually handed
            // over is ours to dispose of: left open it is a device the user
            // cannot reselect, because ALSA would then report it busy.
            if (attempt->abandoned)
            {
                if (err >= 0 && opened != nullptr)
                    snd_pcm_close (opened);

                return;
            }

            attempt->pcm = opened;
            attempt->result = err;
            attempt->finished = true;
            attempt->changed.notify_one();
        });
    }
    catch (...)
    {
        // No worker, so no bound. Failing the open is the honest answer; the
        // alternative is calling straight through and risking the wedge this
        // whole function exists to prevent.
        return -ENOMEM;
    }

    std::unique_lock<std::mutex> lock (attempt->mutex);

    if (! attempt->changed.wait_for (lock, deadline,
                                     [&attempt] { return attempt->finished; }))
    {
        attempt->abandoned = true;
        lock.unlock();

        // Detached, not joined: joining is exactly the wait we just declined.
        worker.detach();
        timedOut = true;
        return -ETIMEDOUT;
    }

    *pcm = attempt->pcm;
    const int result = attempt->result;
    lock.unlock();
    worker.join();
    return result;
}


/// How many inputs an OPEN capture PCM has, or 0 when it will not say.
///
/// Asked of a handle the caller already holds, never by opening the device
/// again. A capture device on real hardware opens once: a second open, from
/// this app as much as any other, is -EBUSY. openStream used to ask by opening
/// the device it had just opened, got -EBUSY, and recorded a four-input
/// interface as one track.
unsigned int captureChannelsOf (snd_pcm_t* pcm)
{
    // Where a real device stops and a plugin's shrug begins.
    //
    // A PCM backed by hardware answers with its actual count -- 2 for a small
    // interface, 18 for a big one. A plugin PCM (default, plug, file, null)
    // has no channels of its own and will be configured to whatever it is
    // asked for, so it answers 1073741823: not "I have a billion inputs" but
    // "I have no opinion". Read literally that would put a billion tracks --
    // clamped to some arbitrary ceiling -- on every virtual device on the
    // machine, which is a worse failure than the one this fixes.
    //
    // Anything above this line is taken as the shrug it is, and a device with
    // no opinion is one microphone.
    constexpr unsigned int kMostInputsRealHardwareHas = 64;

    snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca (&params);

    if (snd_pcm_hw_params_any (pcm, params) < 0)
        return 0;

    unsigned int reported = 0;

    // Zero is success here, not one -- getting that wrong makes the branch
    // unreachable, and an unreachable probe answers 1 for real hardware too,
    // which is the bug this function exists to fix wearing a disguise.
    if (snd_pcm_hw_params_get_channels_max (params, &reported) == 0
        && reported >= 1 && reported <= kMostInputsRealHardwareHas)
        return reported;

    return 1;
}

/// What one bounded open can answer about a capture device: how many inputs it
/// has, and which depths it will accept.
///
/// Both questions used to be asked separately, and each one opened the PCM to
/// ask. That is two bounded opens per device on every enumeration -- and on a
/// device that is not answering, two full probe timeouts rather than one. With
/// a wedged microphone attached, enumeration measured 4003 ms, which is exactly
/// two 2-second deadlines and no actual work.
///
/// snd_pcm_hw_params_any fills the same parameter set both answers come from,
/// so there was never a reason to open twice.
struct CaptureCapabilities
{
    /// 0 is "could not ask", which the descriptor already means by 0. It is
    /// not "one input": the commonest reason the probe cannot open a device is
    /// that this app is recording from it, and answering 1 then shrank the
    /// strips of the interface being recorded on every device-list change.
    unsigned int channels = 0;
    std::vector<int> bitDepths;
};

CaptureCapabilities captureCapabilitiesFor (const char* name)
{
    CaptureCapabilities caps;

    snd_pcm_t* pcm = nullptr;
    bool probeTimedOut = false;

    // A device that cannot be opened to ask keeps the defaults: zero channels,
    // and an EMPTY depth list. Both mean "not reported" -- the caller keeps
    // what it already knew, and the depth chooser turns empty into the
    // fallback -- while a wrong answer would silently change the rig.
    if (openPcmBounded (&pcm, name, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK,
                        kAlsaProbeDeadline, probeTimedOut) < 0)
        return caps;

    // Opened but unwilling to describe itself is still a device, and still one
    // microphone -- the answer this probe has always given it.
    caps.channels = std::max (1u, captureChannelsOf (pcm));

    snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca (&params);

    if (snd_pcm_hw_params_any (pcm, params) >= 0)
    {
        // Only the depths this app can write. S24_3LE is the packed 3-byte
        // layout SessionWriter lays down; S24_LE is the same 24 bits in a
        // 4-byte container, and either one means the device can give 24.
        const struct { snd_pcm_format_t format; int depth; } candidates[] = {
            { SND_PCM_FORMAT_S16_LE,   16 },
            { SND_PCM_FORMAT_S24_3LE,  24 },
            { SND_PCM_FORMAT_S24_LE,   24 },
            { SND_PCM_FORMAT_S32_LE,   32 },
        };

        for (const auto& candidate : candidates)
        {
            if (snd_pcm_hw_params_test_format (pcm, params, candidate.format) != 0)
                continue;

            if (std::find (caps.bitDepths.begin(), caps.bitDepths.end(), candidate.depth)
                == caps.bitDepths.end())
                caps.bitDepths.push_back (candidate.depth);
        }
    }

    snd_pcm_close (pcm);
    return caps;
}

/// `probeCapabilities` false answers the question openInputStream actually
/// asks -- "is this device one I am allowed to open?" -- without opening any
/// PCM to describe it. The GATE is unchanged either way: same kernel-card walk,
/// same removable-ancestry rule, same snd_ctl checks, applied afresh. Only the
/// descriptive half is skipped, and openInputStream never read it.
///
/// It matters because the revalidation runs once per open. With one wedged
/// microphone attached, opening each HEALTHY microphone measured 4003 ms of
/// which none was its own: it was re-probing the wedged neighbour, again, per
/// open. A four-microphone rig paid that four times over.
std::vector<AudioDeviceDescriptor> enumerateDirectExternalInputs (bool probeCapabilities)
{
    std::vector<AudioDeviceDescriptor> result;

    // snd_card_next exposes kernel sound cards only. ALSA plugins such as
    // PipeWire, PulseAudio, BlueALSA, loopback aliases and the file-backed CI
    // fixture never enter this path because they are not hardware cards.
    int card = -1;

    while (snd_card_next (&card) == 0 && card >= 0)
    {
        // The kernel decides whether the hardware can physically be removed by
        // the user. Fixed, unknown and missing sysfs evidence all fail closed.
        if (! alsa_detail::isDirectExternalHardwareCard (card))
            continue;

        const std::string controlName = "hw:" + std::to_string (card);
        snd_ctl_t* control = nullptr;

        if (snd_ctl_open (&control, controlName.c_str(), SND_CTL_NONBLOCK) < 0)
            continue;

        snd_ctl_card_info_t* cardInfo = nullptr;
        snd_ctl_card_info_alloca (&cardInfo);

        if (snd_ctl_card_info (control, cardInfo) < 0)
        {
            snd_ctl_close (control);
            continue;
        }

        const char* alsaCardId = snd_ctl_card_info_get_id (cardInfo);
        const char* cardName = snd_ctl_card_info_get_name (cardInfo);

        // ALSA card IDs are kernel-issued identifiers accepted by the hw PCM
        // syntax. Unlike a numeric card index, they continue to address the
        // same card when enumeration order changes after a replug.
        if (alsaCardId == nullptr || *alsaCardId == '\0')
        {
            snd_ctl_close (control);
            continue;
        }

        snd_pcm_info_t* pcmInfo = nullptr;
        snd_pcm_info_alloca (&pcmInfo);

        int device = -1;

        while (snd_ctl_pcm_next_device (control, &device) == 0 && device >= 0)
        {
            snd_pcm_info_set_device (pcmInfo, static_cast<unsigned int> (device));
            snd_pcm_info_set_subdevice (pcmInfo, 0);
            snd_pcm_info_set_stream (pcmInfo, SND_PCM_STREAM_CAPTURE);

            // Playback-only devices on an otherwise removable card are not
            // recording inputs and therefore do not appear.
            if (snd_ctl_pcm_info (control, pcmInfo) < 0)
                continue;

            AudioDeviceDescriptor descriptor;
            const char* pcmName = snd_pcm_info_get_name (pcmInfo);

            descriptor.name = (cardName != nullptr && *cardName != '\0')
                ? std::string (cardName)
                : std::string ("External audio input");

            if (pcmName != nullptr && *pcmName != '\0'
                && descriptor.name != pcmName)
                descriptor.name += " - " + std::string (pcmName);

            descriptor.usbLocationId = "hw:CARD=" + std::string (alsaCardId)
                                     + ",DEV=" + std::to_string (device);
            descriptor.isMicrophone = true;
            descriptor.hasPhysicalHeadphoneJack = false;
            descriptor.supportedSampleRates = { 44100, 48000 };

            if (probeCapabilities)
            {
                const auto caps = captureCapabilitiesFor (descriptor.usbLocationId.c_str());
                descriptor.maxInputChannels = static_cast<int> (caps.channels);
                descriptor.supportedBitDepths = caps.bitDepths;
            }

            result.push_back (std::move (descriptor));
        }

        snd_ctl_close (control);
    }

    return result;
}

} // namespace

AlsaBackend::AlsaBackend() = default;

AlsaBackend::~AlsaBackend()
{
    hotplug.reset();
    closeAllStreams();
}

std::vector<AudioDeviceDescriptor> AlsaBackend::enumerate (bool wantInput, bool probeCapabilities) const
{
    // Shipping input enumeration is deliberately a positive allowlist based
    // on ALSA kernel cards plus sysfs removability. The old hint path remains
    // byte-for-byte available to outputs and to an explicitly compiled test
    // binary so the file-backed Linux fixture can still exercise the stack.
    if (! alsa_detail::shouldUseHintEnumeration (wantInput, kTestInputsCompiledIn))
        return enumerateDirectExternalInputs (probeCapabilities);

    std::vector<AudioDeviceDescriptor> result;

    void** hints = nullptr;

    if (snd_device_name_hint (-1, "pcm", &hints) != 0)
        return result;

    const char* wanted = wantInput ? "Input" : "Output";

    for (void** hint = hints; *hint != nullptr; ++hint)
    {
        char* name = snd_device_name_get_hint (*hint, "NAME");
        char* desc = snd_device_name_get_hint (*hint, "DESC");
        char* ioid = snd_device_name_get_hint (*hint, "IOID");

        // A null IOID means the PCM serves both directions.
        const bool matches = (ioid == nullptr) || (std::strcmp (ioid, wanted) == 0);

        if (name != nullptr && matches && std::strcmp (name, "null") != 0
            && ! (wantInput && alsa_detail::alsaInputNameIsASystemAlias (name)))
        {
            AudioDeviceDescriptor d;
            d.name = (desc != nullptr) ? std::string (desc) : std::string (name);

            // The first line of DESC is the human name; the rest is detail that
            // would make a meter label unreadable.
            const auto newline = d.name.find ('\n');
            if (newline != std::string::npos)
                d.name.erase (newline);

            // The PCM name IS the stable identifier on ALSA, and it encodes the
            // card and device, so it survives replug of the same port (§2.4).
            d.usbLocationId = name;
            d.isMicrophone = wantInput;
            d.hasPhysicalHeadphoneJack = ! wantInput;
            // What the device actually presents, so a mixer or an interface
            // gets a track per input. Claiming one input for everything meant
            // that the moment someone plugged in an interface, every performer
            // but the first was unreachable -- and openStream asks the same
            // question the same way, so the take gets the channels the list
            // promised rather than silence where the rest should be.
            d.supportedSampleRates = { 44100, 48000 };

            if (wantInput && probeCapabilities)
            {
                const auto caps = captureCapabilitiesFor (name);
                d.maxInputChannels = static_cast<int> (caps.channels);
                d.supportedBitDepths = caps.bitDepths;
            }

            result.push_back (std::move (d));
        }

        free (name);
        free (desc);
        free (ioid);
    }

    snd_device_name_free_hint (hints);
    return result;
}

std::vector<AudioDeviceDescriptor> AlsaBackend::enumerateInputDevices()  { return enumerate (true, true); }
std::vector<AudioDeviceDescriptor> AlsaBackend::enumerateOutputDevices() { return enumerate (false, true); }

void AlsaBackend::setDeviceChangeCallback (DeviceChangeCallback callback)
{
    // Stop and join the old watcher before replacing the function it may be
    // invoking. Assigning first races a live watcher reading the same
    // std::function when the callback is changed or cleared.
    hotplug.reset();
    deviceChangeCallback = std::move (callback);

    if (! deviceChangeCallback)
        return;

    hotplugProblem.clear();

    // Both failures below used to return in silence, and what they cost is not
    // small: with no watch, a microphone plugged in is never noticed, and a
    // microphone pulled out MID-TAKE is never reported (§6.5) -- a take that
    // lost a channel looks exactly like one where nothing went wrong. On a
    // machine with no /dev/snd (a container, a system where ALSA devices live
    // elsewhere) that is the permanent state of the app, unsaid.
    static constexpr const char* kNoWatch =
        "This computer won't tell the app when microphones are plugged in or unplugged, so "
        "the list only updates when the app starts. Restart it after changing your rig.";

    auto watcher = std::make_unique<AlsaHotplugWatcher>();
    watcher->fd = inotify_init1 (IN_CLOEXEC | IN_NONBLOCK);

    if (watcher->fd < 0)
    {
        hotplugProblem = kNoWatch;
        return;
    }

    watcher->wakeFd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (watcher->wakeFd < 0)
    {
        hotplugProblem = kNoWatch;
        return;
    }

    if (inotify_add_watch (watcher->fd, "/dev/snd", IN_CREATE | IN_DELETE) < 0)
    {
        hotplugProblem = kNoWatch;
        return;
    }

    watcher->running.store (true, std::memory_order_release);

    auto* raw = watcher.get();
    watcher->worker = std::thread ([this, raw]
    {
        // Sized for the documented worst case: one event plus a NAME_MAX name.
        alignas (struct inotify_event) char buffer[sizeof (struct inotify_event) + NAME_MAX + 1];

        pollfd descriptors[] = {
            { raw->fd, POLLIN, 0 },
            { raw->wakeFd, POLLIN, 0 }
        };

        while (raw->running.load (std::memory_order_acquire))
        {
            descriptors[0].revents = 0;
            descriptors[1].revents = 0;
            const int result = ::poll (descriptors, 2, -1);

            if (result < 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }

            if ((descriptors[1].revents & (POLLIN | POLLERR | POLLHUP)) != 0
                || ! raw->running.load (std::memory_order_acquire))
                break;

            if ((descriptors[0].revents & POLLIN) == 0)
            {
                if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
                    break;
                continue;
            }

            bool sawChange = false;
            for (;;)
            {
                const auto bytes = ::read (raw->fd, buffer, sizeof (buffer));
                if (bytes > 0)
                {
                    sawChange = true;
                    continue;
                }

                if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    break;

                // EOF or a real read error means the watch cannot recover.
                return;
            }

            if (sawChange && raw->running.load (std::memory_order_acquire)
                && deviceChangeCallback)
                deviceChangeCallback();
        }
    });

    hotplug = std::move (watcher);
}

ExclusiveModeCapability AlsaBackend::checkExclusiveModeCapability (const std::string& outputDeviceId,
                                                                   double sampleRate, int bufferSizeSamples)
{
    ExclusiveModeCapability cap;

    if (! alsa_detail::alsaOutputNameIsExclusiveCapable (outputDeviceId))
    {
        // §5.4: name the cause. "default" is the common case and the message
        // has to make sense to someone who has never heard of dmix.
        //
        // It no longer says "choose a specific sound card", because the names
        // that reach here after the allowlist widened are the shared ones --
        // default, dmix, pulse -- and the card entries the picker offers
        // alongside them are the answer. Telling someone who had already
        // picked their interface to pick their interface was the old bug's
        // most confusing half.
        cap.unavailableReason =
            "This sound output is shared with other apps, which adds too much delay for live monitoring. "
            "In Advanced, pick the entry named after your audio interface rather than the system default.";
        return cap;
    }

    snd_pcm_t* pcm = nullptr;

    // Opening it is the only honest test: another client may already hold it.
    bool capabilityProbeTimedOut = false;

    if (const int err = openPcmBounded (&pcm, outputDeviceId, SND_PCM_STREAM_PLAYBACK,
                                        SND_PCM_NONBLOCK, kAlsaProbeDeadline,
                                        capabilityProbeTimedOut); err < 0)
    {
        // Three causes with three different answers, where this said "another
        // app is using it" to all of them -- including to someone whose
        // headphones were simply unplugged, who would then go looking for an
        // app that was never there. openStream a few lines down has told these
        // apart all along; only the capability check collapsed them.
        //
        // capabilityProbeTimedOut was computed here and never read, so a device
        // that hung on open was reported as busy too.
        cap.unavailableReason =
            capabilityProbeTimedOut
                ? "This sound output took too long to respond, so SobStage couldn't set up "
                  "monitoring on it. Pick it again in Advanced, or choose a different one."
                : (err == -EBUSY
                       ? "Another app is using this sound output. Close it, or choose a different "
                         "output in Advanced."
                       : "This sound output isn't there any more. Pick a different one in Advanced.");
        return cap;
    }

    // The device opened. That says nothing about whether it will run at the
    // rate the take is being recorded at, and this never asked -- so a card
    // that is 44100-only was reported as ready for exclusive monitoring at
    // 48000, and the refusal only surfaced later, from openStream, as a
    // monitoring failure with no mention of the rate. This is the same hole
    // CoreAudio had: an open treated as proof of a configuration.
    {
        snd_pcm_hw_params_t* hw = nullptr;
        snd_pcm_hw_params_alloca (&hw);

        const unsigned int wanted = static_cast<unsigned int> (sampleRate + 0.5);

        if (sampleRate > 0.0
            && snd_pcm_hw_params_any (pcm, hw) >= 0
            && snd_pcm_hw_params_test_rate (pcm, hw, wanted, 0) < 0)
        {
            snd_pcm_close (pcm);

            // Named, because the fix is a setting the user can change: the
            // sample rate in Advanced. "Monitoring is unavailable" without the
            // number sends them hunting through cables for a settings problem.
            cap.unavailableReason =
                "This sound output can't run at " + std::to_string (wanted)
                + " Hz, so SobStage can't use it for live monitoring. Choose a different "
                  "sample rate in Advanced, or a different output.";
            return cap;
        }
    }

    snd_pcm_close (pcm);

    cap.exclusiveModeAvailable = true;

    // Round trip -- in and out -- which is what monitoring latency means and
    // what the other two backends report. This counted one buffer, so Linux
    // reported half the figure macOS and Windows give for the same hardware
    // settings. openStream a few lines down already doubles it when it asks
    // ALSA for a latency, so the file disagreed with itself.
    cap.measuredOrEstimatedLatencyMs = (sampleRate > 0.0)
        ? (static_cast<double> (bufferSizeSamples) / sampleRate) * 1000.0 * 2.0 : 0.0;
    return cap;
}


bool AlsaBackend::openStream (const std::string& deviceId, double sampleRate, int bufferSizeSamples,
                              bool isInput, AudioCallback callback)
{
    auto stream = std::make_unique<AlsaStream>();
    stream->isInput = isInput;
    stream->callback = std::move (callback);

    const auto direction = isInput ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;

    lastOpenError.clear();
    lastOpenRefusal = OpenRefusal::unknown;

    bool openTimedOut = false;

    if (const int err = openPcmBounded (&stream->pcm, deviceId, direction, 0,
                                        kAlsaOpenDeadline, openTimedOut); err < 0)
    {
        // A device that never finished opening is neither busy nor gone, and
        // saying either would send the user chasing the wrong thing.
        if (openTimedOut)
        {
            lastOpenError = isInput
                ? "This microphone took too long to connect, so SobStage carried on without it. "
                  "Unplug it and plug it back in, then pick it again."
                : "This sound output took too long to connect, so SobStage carried on without it. "
                  "Pick it again in Advanced, or choose a different one.";
            lastOpenRefusal = OpenRefusal::timedOut;
            return false;
        }

        // The two causes worth telling apart, because they have different
        // answers: something else is holding the device, or the device is not
        // there any more. snd_strerror is not shown to the user -- it is a
        // developer string -- so the message says what to do instead.
        lastOpenRefusal = err == -EBUSY ? OpenRefusal::inUse : OpenRefusal::gone;
        lastOpenError = err == -EBUSY
            ? (isInput ? "Another app is using this microphone. Close anything else recording or "
                         "streaming from it, then try again."
                       : "Another app has taken these headphones. Close anything else playing "
                         "sound, or pick a different output in Advanced.")
            : (isInput ? "This microphone is no longer connected. Unplug it and plug it back in, "
                         "then try again."
                       : "This sound output isn't there any more. Pick a different one in "
                         "Advanced.");
        return false;
    }

    // Float first because it needs no conversion; the integer formats are the
    // fallbacks real hardware actually offers.
    const snd_pcm_format_t candidates[] = { SND_PCM_FORMAT_FLOAT_LE,
                                            SND_PCM_FORMAT_S32_LE,
                                            SND_PCM_FORMAT_S16_LE };

    // Inputs: every input the device has, because each one is somebody's
    // track. Outputs: stereo, which is what a monitor mix is. The input count
    // comes from the same question enumeration asked, so the take opens with
    // the channels the microphone list promised -- asked of the handle just
    // opened, because opening the device again to ask is -EBUSY on hardware.
    const unsigned int channels = isInput ? std::max (1u, captureChannelsOf (stream->pcm)) : 2u;
    const auto rate = static_cast<unsigned int> (sampleRate);
    const auto latencyMicroseconds = static_cast<unsigned int> (
        (static_cast<double> (bufferSizeSamples) / std::max (1.0, sampleRate)) * 1.0e6 * 2.0);

    bool configured = false;

    // Every input first, then one, and never nothing.
    //
    // A device can report inputs it will not actually open at this rate, and
    // asking for all of them and giving up would turn an interface that used to
    // record one track into one that records none -- a worse rig than before
    // the app knew interfaces existed. So the full count is tried, then mono.
    std::vector<unsigned int> counts { channels };

    if (isInput && channels > 1)
        counts.push_back (1u);

    // Capture asks for its parameters explicitly: a period of one block, so
    // the worker's reads and the driver's interrupts agree and no latency is
    // added, and a buffer of DeviceInputStream::kSourceBufferBlocks periods,
    // so a worker that wakes late finds its audio still in the driver's ring
    // rather than dropped there -- and the app's own ring is sized to take
    // that whole burst when it does. snd_pcm_set_params sized that buffer
    // from a two-block latency hint -- 2.7 ms at 64/48k -- so any scheduling
    // delay wider than that was an xrun in the kernel, audio lost before this
    // app could count it. Playback keeps the hint: a deeper output buffer
    // would be filled, and that is monitor latency.
    const auto configureCapture = [&] (snd_pcm_format_t format, unsigned int wanted) -> bool
    {
        snd_pcm_hw_params_t* hw = nullptr;
        snd_pcm_hw_params_alloca (&hw);

        snd_pcm_uframes_t period = static_cast<snd_pcm_uframes_t> (std::max (1, bufferSizeSamples));
        snd_pcm_uframes_t buffer = period * static_cast<snd_pcm_uframes_t> (DeviceInputStream::kSourceBufferBlocks);
        int dir = 0;

        if (snd_pcm_hw_params_any (stream->pcm, hw) < 0
            || snd_pcm_hw_params_set_rate_resample (stream->pcm, hw, 1) < 0
            || snd_pcm_hw_params_set_access (stream->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0
            || snd_pcm_hw_params_set_format (stream->pcm, hw, format) < 0
            || snd_pcm_hw_params_set_channels (stream->pcm, hw, wanted) < 0
            || snd_pcm_hw_params_set_rate (stream->pcm, hw, rate, 0) < 0
            || snd_pcm_hw_params_set_period_size_near (stream->pcm, hw, &period, &dir) < 0
            || snd_pcm_hw_params_set_buffer_size_near (stream->pcm, hw, &buffer) < 0
            || snd_pcm_hw_params (stream->pcm, hw) < 0)
            return false;

        snd_pcm_sw_params_t* sw = nullptr;
        snd_pcm_sw_params_alloca (&sw);

        // Start on the first read and wake per period, which is what
        // snd_pcm_set_params arranges for capture.
        if (snd_pcm_sw_params_current (stream->pcm, sw) < 0
            || snd_pcm_sw_params_set_start_threshold (stream->pcm, sw, 1) < 0
            || snd_pcm_sw_params_set_avail_min (stream->pcm, sw, period) < 0
            || snd_pcm_sw_params (stream->pcm, sw) < 0)
            return false;

        return true;
    };

    for (auto wanted : counts)
    {
        for (auto format : candidates)
        {
            const bool ok = (isInput && configureCapture (format, wanted))
                         || snd_pcm_set_params (stream->pcm, format, SND_PCM_ACCESS_RW_INTERLEAVED,
                                                wanted, rate, 1 /* allow resampling */,
                                                latencyMicroseconds) == 0;

            if (ok)
            {
                stream->format = format;
                stream->channels = wanted;
                configured = true;
                break;
            }
        }

        if (configured)
            break;
    }

    stream->sampleRate = sampleRate;

    // Fewer inputs than the microphone list promised, which means the tracks
    // for the rest would be written as silence. Said rather than left to be
    // discovered in the files afterwards -- silence that nobody warned about is
    // the whole failure this app is built against.
    if (configured && isInput && stream->channels < channels)
        streamFailures.note (deviceId,
                             "only gave this app one input, though it reports "
                                 + std::to_string (channels)
                                 + ". The other tracks from it will be silent. Try a different USB "
                                   "port, or record it at a different sample rate.");

    if (! configured)
    {
        // Every format the device could plausibly want was offered and refused.
        lastOpenError = isInput
            ? "This microphone won't record in any format this app can use. Try a different USB "
              "port, or a different microphone."
            : "These headphones won't accept audio in any format this app can use. Pick a "
              "different output in Advanced.";
        lastOpenRefusal = OpenRefusal::settingsRefused;
        return false;
    }

    // What the device GRANTED, not what was asked for. snd_pcm_set_params takes
    // a latency hint and ALSA picks its own period from it, so the two are
    // routinely different -- and the §5.4 latency figure was computed from the
    // request, so it described what the app wanted rather than what the card
    // agreed to.
    //
    // Recorded alongside the request, never in place of it. Resizing the
    // worker's reads to the granted period is a behaviour change nothing here
    // asked for: it alters how capture is chunked against the device, and it
    // cost an end-to-end frequency check on the Linux fixture. The request
    // still sizes the I/O; the granted figure is only ever reported.
    stream->periodFrames = static_cast<snd_pcm_uframes_t> (std::max (1, bufferSizeSamples));

    {
        snd_pcm_uframes_t grantedBuffer = 0;
        snd_pcm_uframes_t grantedPeriod = 0;

        if (snd_pcm_get_params (stream->pcm, &grantedBuffer, &grantedPeriod) == 0
            && grantedPeriod > 0)
        {
            stream->grantedPeriodFrames = grantedPeriod;
            stream->grantedBufferFrames = grantedBuffer;
        }
    }

    // §11: sized once, here, and never touched again from the audio thread.
    const size_t sampleCount = static_cast<size_t> (stream->periodFrames) * channels;
    stream->buffers.interleaved.assign (sampleCount * static_cast<size_t> (bytesPerSampleFor (stream->format)), 0);
    stream->buffers.interleavedFloat.assign (sampleCount, 0.0f);
    stream->buffers.planar.assign (sampleCount, 0.0f);
    stream->buffers.inputPointers.assign (channels, nullptr);
    stream->buffers.outputPointers.assign (channels, nullptr);

    for (unsigned int ch = 0; ch < channels; ++ch)
    {
        float* base = stream->buffers.planar.data() + static_cast<size_t> (ch) * stream->periodFrames;
        stream->buffers.inputPointers[ch] = base;
        stream->buffers.outputPointers[ch] = base;
    }

    stream->running.store (true, std::memory_order_release);

    auto* raw = stream.get();
    auto* failureSink = &streamFailures;

    // §0.1: the worker below gives up on a dead PCM and exits. It used to exit
    // in silence -- monitoring simply stopped, or a microphone's track went on
    // being written as silence for the rest of a four-hour take -- so it now
    // says which device stopped and what to do about it on its way out.
    const auto failureId = isInput ? deviceId : std::string();
    const std::string failureReason = isInput
        ? "stopped sending audio. Unplug it and plug it back in."
        : "stopped accepting audio, so you can't hear anything through it. "
          "Try selecting it again.";

    stream->worker = std::thread ([raw, failureSink, failureId, failureReason]
    {
        bool died = false;

        requestRealtimePriority();

        const auto frames = raw->periodFrames;
        const auto channelCount = raw->channels;

        // Reset by every successful read, so only an unbroken run counts.
        int consecutiveRecoveries = 0;

        // When the last read came back, so an xrun can be charged with what
        // was actually lost: the driver's ring held grantedBufferFrames, and
        // everything the device produced beyond that before this thread woke
        // is gone. One period was the old figure, whatever the gap.
        auto lastReadAt = std::chrono::steady_clock::now();

        while (raw->running.load (std::memory_order_acquire))
        {
            if (raw->isInput)
            {
                const auto got = snd_pcm_readi (raw->pcm, raw->buffers.interleaved.data(), frames);

                if (got < 0)
                {
                    // An xrun is recoverable and expected under load; anything
                    // else ends the stream rather than spinning on a dead PCM.
                    if (snd_pcm_recover (raw->pcm, static_cast<int> (got), 1) < 0)
                    {
                        died = true;
                        break;
                    }

                    // Recovered, but not without cost: an xrun IS lost audio.
                    // The device kept running and the app carried on, so this
                    // was the one loss on Linux that nothing counted and nothing
                    // reported -- the same hole the Windows and macOS counters
                    // were added to close, left open on the platform whose CI
                    // job is the only one that opens a real device.
                    //
                    // What the device produced since the last read, less what
                    // the driver's ring could hold, is what it dropped on the
                    // floor; never less than the period this read was for.
                    {
                        const auto gap = std::chrono::duration<double> (std::chrono::steady_clock::now() - lastReadAt).count();
                        const auto produced = static_cast<uint64_t> (std::max (0.0, gap * raw->sampleRate));
                        const auto held = static_cast<uint64_t> (raw->grantedBufferFrames);
                        const auto lost = std::max<uint64_t> (static_cast<uint64_t> (frames),
                                                              produced > held ? produced - held : 0);
                        raw->framesDropped.fetch_add (lost, std::memory_order_relaxed);
                        lastReadAt = std::chrono::steady_clock::now();

                        // Told to the streams as well, as produced-and-lost:
                        // no inputs, that many frames. Their drift measurement
                        // counts the device's clock, and a stall's worth
                        // missing from the delivered count read as the clock
                        // running slow for a minute after every xrun.
                        if (raw->isInput && raw->callback)
                            raw->callback (nullptr, 0, nullptr, 0,
                                           static_cast<int> (std::min<uint64_t> (lost, 1u << 30)));
                    }

                    // Recovering is not the same as working. A PCM that fails
                    // and recovers on every read reaches neither of this
                    // loop's exits -- the failure is recoverable, and no read
                    // ever returns zero -- so it spun here for the rest of the
                    // take: the mic written as silence with only a rising
                    // dropped-frame number to show for it, which is exactly
                    // the case the other two backends' counters exist to end.
                    if (alsa_detail::alsaRecoveryRunMeansDeviceIsDead (++consecutiveRecoveries))
                    {
                        died = true;
                        break;
                    }

                    continue;
                }

                consecutiveRecoveries = 0;
                lastReadAt = std::chrono::steady_clock::now();

                if (got == 0)
                    break; // end of a file-backed device: nothing more will arrive

                const auto count = static_cast<size_t> (got) * channelCount;

                // Two buffers, not one: deinterleaving through the same array it
                // reads from would alias and corrupt every channel after the
                // first. Mono skips the second pass entirely.
                if (channelCount == 1)
                {
                    toFloat (raw->buffers.interleaved.data(), raw->buffers.planar.data(), raw->format, count);
                }
                else
                {
                    toFloat (raw->buffers.interleaved.data(), raw->buffers.interleavedFloat.data(),
                             raw->format, count);

                    for (unsigned int ch = 0; ch < channelCount; ++ch)
                    {
                        float* dest = raw->buffers.planar.data() + static_cast<size_t> (ch) * frames;

                        for (snd_pcm_sframes_t f = 0; f < got; ++f)
                            dest[f] = raw->buffers.interleavedFloat[static_cast<size_t> (f) * channelCount + ch];
                    }
                }

                if (raw->callback)
                    raw->callback (raw->buffers.inputPointers.data(), static_cast<int> (channelCount),
                                   nullptr, 0, static_cast<int> (got));
            }
            else
            {
                for (auto& sample : raw->buffers.planar)
                    sample = 0.0f;

                if (raw->callback)
                    raw->callback (nullptr, 0, raw->buffers.outputPointers.data(),
                                   static_cast<int> (channelCount), static_cast<int> (frames));

                // Interleave from the planar layout the callback filled -- again
                // through a second buffer, for the same aliasing reason.
                const float* source = raw->buffers.planar.data();

                if (channelCount > 1)
                {
                    for (snd_pcm_uframes_t f = 0; f < frames; ++f)
                        for (unsigned int ch = 0; ch < channelCount; ++ch)
                            raw->buffers.interleavedFloat[static_cast<size_t> (f) * channelCount + ch] =
                                raw->buffers.outputPointers[ch][f];

                    source = raw->buffers.interleavedFloat.data();
                }

                fromFloat (source, raw->buffers.interleaved.data(),
                           raw->format, static_cast<size_t> (frames) * channelCount);

                const auto put = snd_pcm_writei (raw->pcm, raw->buffers.interleaved.data(), frames);

                if (put < 0)
                {
                    if (snd_pcm_recover (raw->pcm, static_cast<int> (put), 1) < 0)
                    {
                        died = true;
                        break;
                    }

                    // Counted apart from the capture side, and deliberately.
                    // The previous attempt put these on framesDropped, which
                    // feeds a sentence about audio lost BEFORE RECORDING and a
                    // permanent line in the take's own record -- so a monitor
                    // glitch on a take that recorded perfectly wrote a lasting
                    // claim that recorded audio had been lost. The comment even
                    // said "it is not recorded audio" while doing exactly that.
                    raw->outputGlitches.fetch_add (1, std::memory_order_relaxed);
                }
            }
        }

        raw->running.store (false, std::memory_order_release);

        // Only a genuine failure. A stream that ended because it was closed, or
        // because a file-backed device ran out, is not something to alarm
        // anyone about.
        if (died && failureSink != nullptr)
            failureSink->note (failureId, failureReason);
    });

    openStreams.push_back (std::move (stream));
    return true;
}

bool AlsaBackend::openExclusiveOutputStream (const std::string& outputDeviceId, double sampleRate,
                                             int bufferSizeSamples, AudioCallback callback)
{
    return openStream (outputDeviceId, sampleRate, bufferSizeSamples, false, std::move (callback));
}

bool AlsaBackend::openInputStream (const std::string& inputDeviceId, double sampleRate,
                                   int bufferSizeSamples, AudioCallback callback)
{
    // The list is not an authorization token. Re-apply the current shipping
    // policy immediately before open so an arbitrary PCM name, or a card that
    // changed after enumeration, cannot bypass the kernel-card/removable-
    // ancestry gate. Test-only builds still revalidate against their explicitly
    // compiled hint enumeration, which keeps the virtual ALSA fixture isolated
    // from downloadable production binaries.
    const auto currentlyEligible = enumerate (true, false);
    if (std::none_of (currentlyEligible.begin(), currentlyEligible.end(),
                      [&] (const AudioDeviceDescriptor& candidate)
                      { return candidate.usbLocationId == inputDeviceId; }))
    {
        lastOpenError = "SobStage only records from directly connected external audio hardware.";
        lastOpenRefusal = OpenRefusal::notExternal;
        return false;
    }

    return openStream (inputDeviceId, sampleRate, bufferSizeSamples, true, std::move (callback));
}

uint64_t AlsaBackend::getFramesDroppedByBackend() const
{
    uint64_t total = 0;

    for (const auto& stream : openStreams)
        if (stream != nullptr)
            total += stream->framesDropped.load (std::memory_order_relaxed);

    return total;
}

int AlsaBackend::getGrantedOutputBufferFrames() const
{
    for (const auto& stream : openStreams)
        if (stream != nullptr && ! stream->isInput && stream->grantedPeriodFrames > 0)
            return static_cast<int> (stream->grantedPeriodFrames);

    return 0;
}

uint64_t AlsaBackend::getOutputGlitchCount() const
{
    uint64_t total = 0;

    for (const auto& stream : openStreams)
        if (stream != nullptr)
            total += stream->outputGlitches.load (std::memory_order_relaxed);

    return total;
}

void AlsaBackend::closeAllStreams()
{
    openStreams.clear(); // each AlsaStream stops and joins its worker in its destructor
}

} // namespace mma

#endif // __linux__ && ! MMA_NO_ALSA
