#include "plugprocessor.h"
#include "plugids.h"
#include "base/source/fstreamer.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include <algorithm>
#include <cmath>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>
#include <pmmintrin.h>

struct ScopedFtzDaz {
    unsigned int old_mxcsr;
    ScopedFtzDaz () : old_mxcsr (_mm_getcsr ()) {
        _mm_setcsr (old_mxcsr | 0x8040); /* Enable FTZ (bit 15) and DAZ (bit 6) */
    }
    ~ScopedFtzDaz () {
        _mm_setcsr (old_mxcsr);
    }
};
#else
struct ScopedFtzDaz {};
#endif

namespace RotatingHrtf {

PlugProcessor::PlugProcessor ()
{
    setControllerClass (kControllerUID);

    /* AudioEffect advertises IProcessContextRequirements, and hosts that honour
       it only fill in what is requested. The tempo-synchronised test pulse
       needs the tempo, the musical position and the play state. */
    processContextRequirements.needTempo ().needProjectTimeMusic ().needTransportState ();
}

PlugProcessor::~PlugProcessor ()
{
    if (mCore)
    {
        hrtf_destroy (mCore);
        mCore = nullptr;
    }
}

Steinberg::tresult PLUGIN_API PlugProcessor::initialize (Steinberg::FUnknown* context)
{
    Steinberg::tresult result = AudioEffect::initialize (context);
    if (result != Steinberg::kResultTrue)
        return Steinberg::kResultFalse;

    addAudioInput (STR16 ("Audio Input"), Steinberg::Vst::SpeakerArr::kStereo);
    addAudioOutput (STR16 ("Binaural Output"), Steinberg::Vst::SpeakerArr::kStereo);

    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API PlugProcessor::terminate ()
{
    if (mCore)
    {
        hrtf_destroy (mCore);
        mCore = nullptr;
    }
    return AudioEffect::terminate ();
}

Steinberg::tresult PLUGIN_API PlugProcessor::setBusArrangements (
    Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
    Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts)
{
    if (numIns == 1 && numOuts == 1 &&
        (inputs[0] == Steinberg::Vst::SpeakerArr::kMono || inputs[0] == Steinberg::Vst::SpeakerArr::kStereo) &&
        outputs[0] == Steinberg::Vst::SpeakerArr::kStereo)
    {
        return AudioEffect::setBusArrangements (inputs, numIns, outputs, numOuts);
    }
    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API PlugProcessor::canProcessSampleSize (Steinberg::int32 symbolicSampleSize)
{
    return (symbolicSampleSize == Steinberg::Vst::kSample32) ? Steinberg::kResultTrue : Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API PlugProcessor::setupProcessing (Steinberg::Vst::ProcessSetup& setup)
{
    mMonoBuffer.resize (setup.maxSamplesPerBlock > 0 ? setup.maxSamplesPerBlock : 1024);
    hrtf_test_gen_init (&mTestGen, setup.sampleRate);
    hrtf_test_gen_set_tone (&mTestGen, mTestToneNorm.load (std::memory_order_relaxed));
    return AudioEffect::setupProcessing (setup);
}

Steinberg::tresult PLUGIN_API PlugProcessor::setActive (Steinberg::TBool state)
{
    if (state)
    {
        if (mCore)
        {
            hrtf_destroy (mCore);
            mCore = nullptr;
        }
        mCore = hrtf_create (processSetup.sampleRate);
        if (mCore)
        {
            syncCore ();
            hrtf_reset (mCore);
        }
    }
    else
    {
        if (mCore)
        {
            hrtf_destroy (mCore);
            mCore = nullptr;
        }
    }
    return AudioEffect::setActive (state);
}

/* Downmix the input bus into the scratch buffer for [start, start+count).
   Touches only preallocated memory, so it is safe on the audio thread. */
static void copyMono (const Steinberg::Vst::AudioBusBuffers& inBus, float* mono,
                      Steinberg::int32 start, Steinberg::int32 count)
{
    if (inBus.channelBuffers32 && inBus.numChannels >= 2 &&
        inBus.channelBuffers32[0] && inBus.channelBuffers32[1])
    {
        const float* inL = inBus.channelBuffers32[0] + start;
        const float* inR = inBus.channelBuffers32[1] + start;
        for (Steinberg::int32 i = 0; i < count; ++i)
            mono[i] = 0.5f * (inL[i] + inR[i]);
    }
    else if (inBus.channelBuffers32 && inBus.numChannels >= 1 &&
             inBus.channelBuffers32[0])
    {
        const float* inL = inBus.channelBuffers32[0] + start;
        std::copy (inL, inL + count, mono);
    }
    else
    {
        std::fill (mono, mono + count, 0.0f);
    }
}

/* Push every DSP parameter from the atomics into the core. */
void PlugProcessor::syncCore ()
{
    if (!mCore) return;
    hrtf_set_distance (mCore, hrtf_distance_from_position (mDistanceNorm.load (std::memory_order_relaxed)));
    hrtf_set_rotation_phase (mCore, mRotationNorm.load (std::memory_order_relaxed));
    hrtf_set_elevation_deg (mCore, -90.0 + mElevationNorm.load (std::memory_order_relaxed) * 180.0);
    hrtf_set_space (mCore, mSpaceNorm.load (std::memory_order_relaxed));
    hrtf_set_ear_scale (mCore, 0.70 + mEarScaleNorm.load (std::memory_order_relaxed) * 0.60);
    hrtf_set_reflections (mCore, mReflectionsNorm.load (std::memory_order_relaxed) >= 0.5);
}

void PlugProcessor::applyParameter (Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value)
{
    /* A non-finite or out-of-range value must never be cached or saved. */
    if (!std::isfinite (value)) return;
    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;

    switch (id)
    {
        case kParamDistance: mDistanceNorm.store (value, std::memory_order_relaxed); break;
        case kParamRotation: mRotationNorm.store (value, std::memory_order_relaxed); break;
        case kParamElevation: mElevationNorm.store (value, std::memory_order_relaxed); break;
        case kParamSpace: mSpaceNorm.store (value, std::memory_order_relaxed); break;
        case kParamTestPulse: mTestPulseNorm.store (value, std::memory_order_relaxed); break;
        case kParamTestTone:
            mTestToneNorm.store (value, std::memory_order_relaxed);
            hrtf_test_gen_set_tone (&mTestGen, value);
            break;
        case kParamEarScale: mEarScaleNorm.store (value, std::memory_order_relaxed); break;
        case kParamReflections: mReflectionsNorm.store (value, std::memory_order_relaxed); break;
        default: return;
    }
    syncCore ();
}

/* Apply only the last point of each queue. Used whenever a block cannot be
   rendered, so the host's parameter changes are still honoured. */
void PlugProcessor::applyFinalParameterValues (Steinberg::Vst::IParameterChanges* changes)
{
    if (!changes) return;
    const Steinberg::int32 numQueues = changes->getParameterCount ();
    for (Steinberg::int32 i = 0; i < numQueues; ++i)
    {
        Steinberg::Vst::IParamValueQueue* queue = changes->getParameterData (i);
        if (!queue) continue;
        const Steinberg::int32 points = queue->getPointCount ();
        if (points <= 0) continue;

        Steinberg::int32 offset = 0;
        Steinberg::Vst::ParamValue value = 0.0;
        if (queue->getPoint (points - 1, offset, value) == Steinberg::kResultTrue)
            applyParameter (queue->getParameterId (), value);
    }
}

/* Zero every valid output channel of every output bus for [0, numSamples). */
static void clearOutputs (Steinberg::Vst::ProcessData& data)
{
    if (data.numSamples <= 0 || !data.outputs) return;
    for (Steinberg::int32 b = 0; b < data.numOutputs; ++b)
    {
        Steinberg::Vst::AudioBusBuffers& bus = data.outputs[b];
        if (!bus.channelBuffers32) continue;
        for (Steinberg::int32 c = 0; c < bus.numChannels; ++c)
        {
            if (bus.channelBuffers32[c])
                std::fill (bus.channelBuffers32[c], bus.channelBuffers32[c] + data.numSamples, 0.0f);
        }
        bus.silenceFlags = bus.numChannels >= 64 ? ~(Steinberg::uint64)0
                                                 : (((Steinberg::uint64)1 << bus.numChannels) - 1);
    }
}

Steinberg::tresult PLUGIN_API PlugProcessor::process (Steinberg::Vst::ProcessData& data)
{
    ScopedFtzDaz ftzDaz;

    /* Parameter changes are honoured at the sample offset the host asked
       for, not applied to the whole block from its first sample.
       Fixed stack array of 64 queues accommodates >9x the plugin's 7 parameters
       with zero heap allocation on the audio thread. */
    constexpr Steinberg::int32 kMaxQueues = 64;
    Steinberg::Vst::IParamValueQueue* queues[kMaxQueues] = {};
    Steinberg::int32 consumed[kMaxQueues] = {};
    Steinberg::int32 numQueues = 0;

    if (data.inputParameterChanges)
    {
        numQueues = data.inputParameterChanges->getParameterCount ();
        if (numQueues > kMaxQueues) numQueues = kMaxQueues;
        for (Steinberg::int32 i = 0; i < numQueues; ++i)
            queues[i] = data.inputParameterChanges->getParameterData (i);
    }

    /* Every path that cannot render still honours the block's parameter
       changes (a host flushes final values with numSamples == 0) and leaves
       any output it was handed silent rather than holding stale samples. */
    if (data.numSamples <= 0 || !mCore || data.numOutputs < 1 || !data.outputs ||
        mMonoBuffer.empty () /* setupProcessing was never called */)
    {
        applyFinalParameterValues (data.inputParameterChanges);
        clearOutputs (data);
        return Steinberg::kResultOk;
    }

    /* No input bus renders as silence in, which the Test Pulse still fills. */
    static const Steinberg::Vst::AudioBusBuffers kNoInput = {};
    const Steinberg::Vst::AudioBusBuffers& inBus =
        (data.numInputs >= 1 && data.inputs) ? data.inputs[0] : kNoInput;
    Steinberg::Vst::AudioBusBuffers& outBus = data.outputs[0];

    /* A host may supply null buffers for an inactive bus, so the pointers
       are validated as well as the channel counts. */
    if (outBus.numChannels < 2 || !outBus.channelBuffers32 ||
        !outBus.channelBuffers32[0] || !outBus.channelBuffers32[1])
    {
        applyFinalParameterValues (data.inputParameterChanges);
        clearOutputs (data);
        return Steinberg::kResultOk;
    }

    const Steinberg::uint32 numSamples = (Steinberg::uint32)data.numSamples;
    const size_t capacity = mMonoBuffer.size ();

    float* mono = mMonoBuffer.data ();

    syncCore ();
    hrtf_test_gen_set_tone (&mTestGen, mTestToneNorm.load (std::memory_order_relaxed));

    Steinberg::int32 cursor = 0;
    while (cursor < (Steinberg::int32)numSamples)
    {
        /* Never allocate on the audio thread. The block is filled and
           rendered in windows no larger than the buffer that
           setupProcessing sized, so a host that exceeds maxSamplesPerBlock
           still gets correct audio instead of a reallocation in the
           callback. */
        const Steinberg::int32 remaining = (Steinberg::int32)numSamples - cursor;
        const Steinberg::int32 windowFrames =
            (Steinberg::int32)std::min<size_t> (capacity, (size_t)remaining);
        const Steinberg::int32 windowStart = cursor;
        const Steinberg::int32 windowEnd = windowStart + windowFrames;

        while (cursor < windowEnd)
        {
            Steinberg::int32 next = windowEnd;

            for (Steinberg::int32 i = 0; i < numQueues; ++i)
            {
                if (!queues[i]) continue;
                const Steinberg::int32 points = queues[i]->getPointCount ();

                for (;;)
                {
                    if (consumed[i] >= points) break;

                    Steinberg::int32 offset = 0;
                    Steinberg::Vst::ParamValue value = 0.0;
                    if (queues[i]->getPoint (consumed[i], offset, value) != Steinberg::kResultTrue)
                    {
                        ++consumed[i];
                        continue;
                    }
                    if (offset <= cursor)
                    {
                        applyParameter (queues[i]->getParameterId (), value);
                        ++consumed[i];
                        continue;
                    }
                    if (offset < next) next = offset;
                    break;
                }
            }

            if (next <= cursor) break; /* no forward progress: stop rather than spin */

            const Steinberg::int32 sliceFrames = next - cursor;
            copyMono (inBus, mono, cursor, sliceFrames);

            /* The Test Pulse replaces the input, crossfading whenever it is
               switched so neither edge clicks. */
            const int pulseOn = mTestPulseNorm.load (std::memory_order_relaxed) >= 0.5;
            if (pulseOn || mTestGen.mix > 0.0)
            {
                double bpm = 120.0;
                double beatPos = 0.0;
                int isPlaying = 0;
                if (data.processContext)
                {
                    if ((data.processContext->state & Steinberg::Vst::ProcessContext::kTempoValid) &&
                        std::isfinite (data.processContext->tempo) && data.processContext->tempo > 1.0)
                    {
                        bpm = data.processContext->tempo;
                    }
                    if ((data.processContext->state & Steinberg::Vst::ProcessContext::kProjectTimeMusicValid) &&
                        std::isfinite (data.processContext->projectTimeMusic))
                    {
                        beatPos = data.processContext->projectTimeMusic;
                    }
                    if (data.processContext->state & Steinberg::Vst::ProcessContext::kPlaying)
                    {
                        isPlaying = 1;
                    }
                }
                double sliceBeat = beatPos;
                if (isPlaying && processSetup.sampleRate > 1000.0)
                {
                    sliceBeat += (double)cursor * (bpm / (60.0 * processSetup.sampleRate));
                }
                hrtf_test_gen_render (&mTestGen, mono, mono, (size_t)sliceFrames, bpm, sliceBeat,
                                      isPlaying, pulseOn);
            }

            float* outSeg[2] = { outBus.channelBuffers32[0] + cursor,
                                 outBus.channelBuffers32[1] + cursor };
            hrtf_process (mCore, mono, outSeg, (size_t)sliceFrames);
            cursor = next;
        }
    }

    /* The whole block was written, so the output is not silent. A stale
       "silent" flag left set makes hosts skip mixing real audio. */
    outBus.silenceFlags = 0;

    return Steinberg::kResultOk;
}

/* The atomics in state order (ParamIDs order). */
std::atomic<Steinberg::Vst::ParamValue>* PlugProcessor::stateSlot (Steinberg::int32 index)
{
    std::atomic<Steinberg::Vst::ParamValue>* slots[kParamCount] = {
        &mDistanceNorm, &mRotationNorm, &mElevationNorm, &mSpaceNorm,
        &mTestPulseNorm, &mTestToneNorm, &mEarScaleNorm, &mReflectionsNorm
    };
    return (index >= 0 && index < kParamCount) ? slots[index] : nullptr;
}

Steinberg::tresult PLUGIN_API PlugProcessor::setState (Steinberg::IBStream* state)
{
    if (!state) return Steinberg::kResultFalse;
    Steinberg::IBStreamer streamer (state);

    Steinberg::int32 version = 0;
    if (!streamer.readInt32 (version)) return Steinberg::kResultFalse;
    if (version != kStateVersion) return Steinberg::kResultFalse;

    // Read the whole payload before touching anything, so a truncated
    // stream leaves every parameter as it was.
    double norm[kParamCount] = {};
    for (Steinberg::int32 i = 0; i < kParamCount; ++i)
    {
        if (!streamer.readDouble (norm[i])) return Steinberg::kResultFalse;
    }

    /* Out-of-range or non-finite values are ignored rather than pushed into
       the core, where they would poison the DSP state for the session. */
    for (Steinberg::int32 i = 0; i < kParamCount; ++i)
    {
        if (std::isfinite (norm[i]) && norm[i] >= 0.0 && norm[i] <= 1.0)
            stateSlot (i)->store (norm[i], std::memory_order_relaxed);
    }

    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API PlugProcessor::getState (Steinberg::IBStream* state)
{
    if (!state) return Steinberg::kResultFalse;
    Steinberg::IBStreamer streamer (state);

    streamer.writeInt32 (kStateVersion);
    for (Steinberg::int32 i = 0; i < kParamCount; ++i)
        streamer.writeDouble (stateSlot (i)->load (std::memory_order_relaxed));

    return Steinberg::kResultOk;
}

} // namespace RotatingHrtf
