#include "TestFramework.h"
#include "Core/StemAligner.h"
#include <vector>

using namespace mma;

namespace {

/// A ramp starting at `first`: sample n carries first + n, so what comes out
/// says which input sample it was (zero is silence the aligner wrote).
std::vector<float> ramp (int first, int count)
{
    std::vector<float> v (static_cast<size_t> (count));
    for (int i = 0; i < count; ++i)
        v[static_cast<size_t> (i)] = static_cast<float> (first + i);
    return v;
}

} // namespace

TEST_CASE (StemAligner_AChannelWithNoOffsetPassesStraightThrough)
{
    StemAligner a;
    a.prepare (1);

    auto block = ramp (1, 64);
    a.process (0, block.data(), block.size());

    for (int i = 0; i < 64; ++i)
        REQUIRE (block[static_cast<size_t> (i)] == static_cast<float> (1 + i));

    REQUIRE (a.isExact());
    REQUIRE (a.getSilenceInserted (0) == 0u);
}

TEST_CASE (StemAligner_TheStartingOffsetOpensTheStemWithThatMuchSilence)
{
    // The quicker interface's stem starts 37 samples of silence later, and
    // then carries every sample it handed over, in order.
    StemAligner a;
    a.prepare (2);
    a.setOffset (0, 37);
    a.setOffset (1, 0);

    auto quick = ramp (1, 200), slow = ramp (1, 200);
    a.process (0, quick.data(), 100);
    a.process (0, quick.data() + 100, 100); // across a chunk boundary
    a.process (1, slow.data(), slow.size());

    for (int i = 0; i < 37; ++i)
        REQUIRE (quick[static_cast<size_t> (i)] == 0.0f);
    for (int i = 37; i < 200; ++i)
        REQUIRE (quick[static_cast<size_t> (i)] == static_cast<float> (i - 37 + 1));

    REQUIRE (slow[0] == 1.0f);
    REQUIRE (a.getStartOffset (0) == 37);
    REQUIRE (a.getOffset (0) == 37);

    // Starting silence is the take's alignment, not a mid-take change.
    REQUIRE (a.getSilenceInserted (0) == 0u);
    REQUIRE (a.getSamplesDropped (0) == 0u);
}

TEST_CASE (StemAligner_TheStartingOffsetIsKnownBeforeTheFirstSampleReachesTheChannel)
{
    // The writer reaches a channel some milliseconds into the take, and the
    // take's start-time record is written before then. It said the stem
    // starts unshifted (0) beside an offset of 37 -- the record a crash in
    // the first half-minute leaves, and the one a user with a clamped latency
    // is told to line the stems up by.
    StemAligner a;
    a.prepare (2);
    a.setOffset (0, 37);

    REQUIRE (! a.hasStarted (0));
    REQUIRE (a.getStartOffset (0) == 37);
    REQUIRE (a.getStartOffset (1) == 0);

    auto block = ramp (1, 64);
    a.process (0, block.data(), block.size());

    // Fixed by the first sample: a later change is a change, not the start.
    REQUIRE (a.hasStarted (0));
    REQUIRE (! a.hasStarted (1));
    a.setOffset (0, 50);
    REQUIRE (a.getStartOffset (0) == 37);
    REQUIRE (a.getOffset (0) == 50);

    // And a new take starts unstarted again.
    a.prepare (2);
    REQUIRE (! a.hasStarted (0));
    REQUIRE (a.getStartOffset (0) == 0);
}

TEST_CASE (StemAligner_ALongerOffsetMidTakeWritesSilenceAndLosesNothing)
{
    // Another device's IO block grew: this channel is held back 1092 more
    // from sample 300 on. Silence goes out there, and every sample it handed
    // over still comes out, in order, just that much later.
    StemAligner a;
    a.prepare (1);
    a.setOffset (0, 10);

    auto first = ramp (1, 300);
    a.process (0, first.data(), first.size());
    a.setOffset (0, 10 + 1092);

    auto second = ramp (301, 3000);
    a.process (0, second.data(), second.size());

    std::vector<float> out (first);
    out.insert (out.end(), second.begin(), second.end());

    // 10 of starting silence, 290 samples, 1092 of silence, then on from 291.
    for (int i = 0; i < 10; ++i)
        REQUIRE (out[static_cast<size_t> (i)] == 0.0f);
    for (int i = 10; i < 300; ++i)
        REQUIRE (out[static_cast<size_t> (i)] == static_cast<float> (i - 9));
    for (int i = 300; i < 300 + 1092; ++i)
        REQUIRE (out[static_cast<size_t> (i)] == 0.0f);
    for (int i = 300 + 1092; i < 3300; ++i)
        REQUIRE (out[static_cast<size_t> (i)] == static_cast<float> (i - 9 - 1092));

    REQUIRE (a.getSilenceInserted (0) == 1092u);
    REQUIRE (a.getSamplesDropped (0) == 0u);
    REQUIRE (a.getOffset (0) == 1102);
}

TEST_CASE (StemAligner_AShorterOffsetTakesOutTheMovesSilenceAlreadyWaiting)
{
    // This channel's own device grew its block: its stream put 500 samples
    // of silence in as it moved, and the writer brings the stem forward by
    // the same 500. What is taken out is what had just come in -- that
    // silence -- so the audio either side of it joins up.
    StemAligner a;
    a.prepare (1);
    a.setOffset (0, 1092);

    std::vector<float> in;
    for (int i = 0; i < 2000; ++i)
        in.push_back (static_cast<float> (i + 1));
    for (int i = 0; i < 500; ++i)
        in.push_back (0.0f); // the stream's own move
    for (int i = 2000; i < 6000; ++i)
        in.push_back (static_cast<float> (i + 1));

    auto out = in;
    a.process (0, out.data(), 2500);
    a.setOffset (0, 1092 - 500);
    a.process (0, out.data() + 2500, out.size() - 2500);

    REQUIRE (a.getSamplesDropped (0) == 500u);
    REQUIRE (a.getSilenceInserted (0) == 0u);

    // After the starting silence the ramp runs on without a gap or a repeat.
    for (size_t i = 1092; i < out.size(); ++i)
        REQUIRE (out[i] == static_cast<float> (i - 1092 + 1));
}

TEST_CASE (StemAligner_AShorterOffsetTakesOutTheMovesSilenceStillArriving)
{
    // A move the cushion covered: the offset shrinks at the block whose start
    // carries the stream's silence (300 samples), and some of the move went
    // out as a dry pull just before it (200). Both are taken out -- the run
    // already waiting and the run arriving -- and the ramp joins up.
    StemAligner a;
    a.prepare (1);
    a.setOffset (0, 2000);

    std::vector<float> before;
    for (int i = 0; i < 3000; ++i)
        before.push_back (static_cast<float> (i + 1));
    for (int i = 0; i < 200; ++i)
        before.push_back (0.0f);

    std::vector<float> after;
    for (int i = 0; i < 300; ++i)
        after.push_back (0.0f);
    for (int i = 3000; i < 7000; ++i)
        after.push_back (static_cast<float> (i + 1));

    a.process (0, before.data(), before.size());
    a.setOffset (0, 2000 - 500);
    a.process (0, after.data(), after.size());

    std::vector<float> out (before);
    out.insert (out.end(), after.begin(), after.end());

    REQUIRE (a.getSamplesDropped (0) == 500u);
    for (size_t i = 2000; i < out.size(); ++i)
        REQUIRE (out[i] == static_cast<float> (i - 2000 + 1));
}

TEST_CASE (StemAligner_AudioIsTakenOutOnlyWhereNoSilenceIsFound)
{
    // Nothing silent either side of the change: the newest 100 samples
    // waiting go, and from then on the stem is 100 samples further on.
    StemAligner a;
    a.prepare (1);
    a.setOffset (0, 400);

    auto first = ramp (1, 1000);
    a.process (0, first.data(), first.size());
    a.setOffset (0, 300);

    auto second = ramp (1001, 1000);
    a.process (0, second.data(), second.size());

    REQUIRE (a.getSamplesDropped (0) == 100u);

    // Up to the change: the starting silence and then the ramp, held 400 back.
    REQUIRE (first[399] == 0.0f);
    REQUIRE (first[400] == 1.0f);
    REQUIRE (first[999] == 600.0f);

    // After it: samples 601..900 were waiting and still come out; 901..1000
    // (the newest) were taken out; then the ramp goes on from 1001.
    REQUIRE (second[0] == 601.0f);
    REQUIRE (second[299] == 900.0f);
    REQUIRE (second[300] == 1001.0f);
    REQUIRE (second[999] == 1700.0f);
}

TEST_CASE (StemAligner_AShorterOffsetFirstCancelsSilenceNotYetWritten)
{
    // A longer offset and then a shorter one before the silence for the first
    // has all gone out: the rest of it is simply not written, and nothing is
    // taken out of the audio.
    StemAligner a;
    a.prepare (1);

    auto first = ramp (1, 100);
    a.process (0, first.data(), first.size());
    a.setOffset (0, 1000);

    auto second = ramp (101, 300);
    a.process (0, second.data(), second.size()); // 300 of the 1000 written
    a.setOffset (0, 0);                           // 700 never written, 300 taken back

    auto third = ramp (401, 100);
    a.process (0, third.data(), third.size());

    REQUIRE (a.getSilenceInserted (0) == 300u);
    REQUIRE (a.getSamplesDropped (0) == 300u);
    REQUIRE (a.getOffset (0) == 0);

    // Back to passing straight through.
    for (int i = 0; i < 100; ++i)
        REQUIRE (third[static_cast<size_t> (i)] == static_cast<float> (401 + i));
}

TEST_CASE (StemAligner_ALongerOffsetFirstCancelsSilenceNotYetTakenOut)
{
    // A shorter offset with no silence waiting to take out leaves it to be
    // taken out as it arrives. A longer one before any has arrived simply
    // cancels that: nothing is written, nothing is taken out, and the channel
    // is held back exactly as before.
    StemAligner a;
    a.prepare (1);
    a.setOffset (0, 500);

    auto first = ramp (1, 1000);
    a.process (0, first.data(), first.size());
    a.setOffset (0, 200); // 300 still to take out, none of it here yet
    a.setOffset (0, 500); // ...and back again before any arrives

    auto second = ramp (1001, 1000);
    a.process (0, second.data(), second.size());

    REQUIRE (a.getSilenceInserted (0) == 0u);
    REQUIRE (a.getSamplesDropped (0) == 0u);

    std::vector<float> out (first);
    out.insert (out.end(), second.begin(), second.end());

    for (size_t i = 500; i < out.size(); ++i)
        REQUIRE (out[i] == static_cast<float> (i - 500 + 1));
}

namespace {

/// A stream whose driver held its audio back and then handed it over in one
/// piece: a ramp, `gap` samples of the stream's silence, then the ramp again
/// (the late audio itself is the stream's to skip, and does not reach here).
std::vector<float> rampWithGap (int before, int gap, int after)
{
    std::vector<float> v;
    for (int i = 0; i < before; ++i)
        v.push_back (static_cast<float> (i + 1));
    v.insert (v.end(), static_cast<size_t> (gap), 0.0f);
    for (int i = 0; i < after; ++i)
        v.push_back (static_cast<float> (before + gap + i + 1));
    return v;
}

} // namespace

TEST_CASE (StemAligner_ARefusedProvisionalOffsetPutsItsSilenceBackWhereItWas)
{
    // The quicker channel, held back 1400. Its driver goes quiet: its stream
    // writes 1055 samples of silence, then one large delivery lands and the
    // stream moves for it as if the device had grown to 1152 -- the offset
    // shortens by 1088, provisionally, taking the gap's silence out of the
    // newest end of the line and 33 more as it arrives. The device's next
    // delivery is back at 64: a backlog, refused, and the offset goes back
    // to 1400. The silence goes back where it was taken from, behind the
    // samples that were waiting ahead of it, so the stem is the stream's
    // audio held back by 1400 throughout -- nothing moved, nothing counted.
    StemAligner a;
    a.prepare (1);
    a.setOffset (0, 1400);

    // 3000 samples of ramp, the 1055-sample gap, then 128 of the stream's
    // silence buffering up for the block it thought had grown, then audio.
    auto in = rampWithGap (3000, 1055 + 128, 4000);
    auto out = in;

    const size_t atProvisional = 3000 + 1055;
    a.process (0, out.data(), atProvisional);
    a.setOffset (0, 1400 - 1088, StemOffsetKind::provisional);
    a.process (0, out.data() + atProvisional, 64);
    a.setOffset (0, 1400, StemOffsetKind::refused);
    a.process (0, out.data() + atProvisional + 64, out.size() - atProvisional - 64);

    REQUIRE (a.getSilenceInserted (0) == 0u);
    REQUIRE (a.getSamplesDropped (0) == 0u);

    for (size_t i = 0; i < 1400; ++i)
        REQUIRE (out[i] == 0.0f);
    for (size_t i = 1400; i < out.size(); ++i)
        REQUIRE (out[i] == in[i - 1400]);
}

TEST_CASE (StemAligner_AConfirmedProvisionalOffsetStands)
{
    // The same provisional change, followed by a settled one rather than a
    // refusal (another device moved the reference): the move's silence
    // stays taken out, and is counted.
    StemAligner a;
    a.prepare (1);
    a.setOffset (0, 1400);

    auto in = rampWithGap (3000, 1055 + 128, 4000);
    auto out = in;

    const size_t atProvisional = 3000 + 1055;
    a.process (0, out.data(), atProvisional);
    a.setOffset (0, 1400 - 1088, StemOffsetKind::provisional);
    a.process (0, out.data() + atProvisional, 64);
    a.setOffset (0, 1400 - 1088 + 10);
    a.process (0, out.data() + atProvisional + 64, out.size() - atProvisional - 64);

    REQUIRE (a.getSamplesDropped (0) == 1088u);
    REQUIRE (a.getSilenceInserted (0) == 10u);
}

TEST_CASE (StemAligner_RoomGivenBackPutsItsSilenceWhereTheStreamSkipped)
{
    // The quicker channel, held back 308 behind the slowest device. Its own
    // stream gives back 1092 samples of room it held for a block its device
    // has left: it drops that much quiet from the newest end of what it had
    // buffered, so its next sample is 1092 later in the room than the newest
    // one it handed over. The silence belongs exactly there. The 308 samples
    // waiting in the line came before the skip and come out on time; only
    // what follows them is 1092 later. Written at the head instead, the
    // silence went out first and the 308 waiting samples after it: a gap
    // 308 samples before the moment the stream skipped, inside whatever
    // sound was there, and those samples 1092 late against every other stem.
    StemAligner a;
    a.prepare (1);
    a.setOffset (0, 308);

    auto first = ramp (1, 3000);
    a.process (0, first.data(), first.size());
    a.setOffset (0, 308 + 1092, StemOffsetKind::released, 1092);

    auto second = ramp (3001, 4000);
    a.process (0, second.data(), second.size());

    std::vector<float> out (first);
    out.insert (out.end(), second.begin(), second.end());

    // 308 of starting silence, then every sample up to the skip, in order and
    // unbroken -- the last of them, 3000, at 3000 + 308 - 1 -- then the room
    // given back, then on from 3001.
    for (size_t i = 0; i < 308; ++i)
        REQUIRE (out[i] == 0.0f);
    for (size_t i = 308; i < 3308; ++i)
        REQUIRE (out[i] == static_cast<float> (i - 307));
    for (size_t i = 3308; i < 3308 + 1092; ++i)
        REQUIRE (out[i] == 0.0f);
    for (size_t i = 3308 + 1092; i < out.size(); ++i)
        REQUIRE (out[i] == static_cast<float> (i - 307 - 1092));

    REQUIRE (a.getSilenceInserted (0) == 1092u);
    REQUIRE (a.getSamplesDropped (0) == 0u);
    REQUIRE (a.getOffset (0) == 1400);
}

TEST_CASE (StemAligner_RoomGivenBackWhileAnotherDeviceGrowsSplitsTheChange)
{
    // The same room given back, in the same block as another device's growth
    // raises the reference by 200: the change is 1292, of which 1092 is this
    // channel's own skip. The growth's 200 belong at the head (that device's
    // stream moved at the moment the oldest sample waiting here stands for),
    // the 1092 behind what is waiting.
    StemAligner a;
    a.prepare (1);
    a.setOffset (0, 308);

    auto first = ramp (1, 3000);
    a.process (0, first.data(), first.size());
    a.setOffset (0, 308 + 1092 + 200, StemOffsetKind::released, 1092);

    auto second = ramp (3001, 4000);
    a.process (0, second.data(), second.size());

    std::vector<float> out (first);
    out.insert (out.end(), second.begin(), second.end());

    // Samples up to 2692, then the growth's 200 of silence, the 308 that
    // were waiting (2693 to 3000), the room given back, then on from 3001.
    for (size_t i = 308; i < 3000; ++i)
        REQUIRE (out[i] == static_cast<float> (i - 307));
    for (size_t i = 3000; i < 3200; ++i)
        REQUIRE (out[i] == 0.0f);
    for (size_t i = 3200; i < 3508; ++i)
        REQUIRE (out[i] == static_cast<float> (i - 307 - 200));
    for (size_t i = 3508; i < 3508 + 1092; ++i)
        REQUIRE (out[i] == 0.0f);
    for (size_t i = 3508 + 1092; i < out.size(); ++i)
        REQUIRE (out[i] == static_cast<float> (i - 307 - 200 - 1092));

    REQUIRE (a.getSilenceInserted (0) == 1292u);
    REQUIRE (a.getSamplesDropped (0) == 0u);
}

TEST_CASE (StemAligner_RoomGivenBackBeforeTheChannelStartsIsItsStartingOffset)
{
    // Before the channel's first sample there is nothing waiting to put the
    // silence behind: it is the take's starting alignment, as any other.
    StemAligner a;
    a.prepare (1);
    a.setOffset (0, 308);
    a.setOffset (0, 1400, StemOffsetKind::released, 1092);

    auto block = ramp (1, 2000);
    a.process (0, block.data(), block.size());

    for (size_t i = 0; i < 1400; ++i)
        REQUIRE (block[i] == 0.0f);
    REQUIRE (block[1400] == 1.0f);
    REQUIRE (a.getStartOffset (0) == 1400);
    REQUIRE (a.getSilenceInserted (0) == 0u);
}

TEST_CASE (StemAligner_AnOffsetPastItsBoundIsClampedAndSaidSo)
{
    StemAligner a;
    a.prepare (1);
    a.setOffset (0, StemAligner::kMaxOffsetSamples + 5);

    REQUIRE (! a.isExact());
    REQUIRE (a.getOffset (0) == StemAligner::kMaxOffsetSamples);

    // Still a working line at its bound.
    std::vector<float> block (static_cast<size_t> (StemAligner::kMaxOffsetSamples) + 10, 1.0f);
    a.process (0, block.data(), block.size());
    REQUIRE (block[static_cast<size_t> (StemAligner::kMaxOffsetSamples) - 1] == 0.0f);
    REQUIRE (block[static_cast<size_t> (StemAligner::kMaxOffsetSamples)] == 1.0f);
}

TEST_CASE (StemOffsetQueue_HandsChangesOverInOrderAndSaysWhenFull)
{
    StemOffsetQueue q;
    q.clear();

    for (size_t i = 0; i < StemOffsetQueue::kCapacity; ++i)
        REQUIRE (q.push ({ i, 0, static_cast<int> (i) }));

    REQUIRE (! q.push ({ 9999, 0, 1 }));

    StemOffsetEvent e;
    REQUIRE (q.peek (e));
    REQUIRE (e.frame == 0u);
    q.pop();
    REQUIRE (q.peek (e));
    REQUIRE (e.frame == 1u);

    // Room again for one.
    REQUIRE (q.push ({ 5000, 1, 7 }));
}
