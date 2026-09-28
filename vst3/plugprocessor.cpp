#include "plugprocessor.h"
#include "plugids.h"
#include "base/source/fstreamer.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include <algorithm>
#include <cmath>

namespace RotatingHrtf {

PlugProcessor::PlugProcessor ()
{
    setControllerClass (kControllerUID);
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
    mSampleRate = setup.sampleRate;
    mMonoBuffer.resize (setup.maxSamplesPerBlock > 0 ? setup.maxSamplesPerBlock : 1024);
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
        size_t maxBlock = processSetup.maxSamplesPerBlock > 0 ? (size_t)processSetup.maxSamplesPerBlock : 1024;
        mCore = hrtf_create (processSetup.sampleRate, maxBlock);
        if (mCore)
        {
            double dist_m = 0.05 + mDistanceNorm * (20.0 - 0.05);
            hrtf_set_distance (mCore, dist_m);
            hrtf_set_rotation_phase (mCore, mRotationNorm);
            hrtf_set_elevation_deg (mCore, -90.0 + mElevationNorm * 180.0);
            hrtf_set_space (mCore, mSpaceNorm);
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

void PlugProcessor::applyParameter (Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value)
{
    /* A non-finite or out-of-range value must never be cached or saved. */
    if (!std::isfinite (value)) return;
    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;

    if (id == kParamDistance)
    {
        mDistanceNorm = value;
        if (mCore)
            hrtf_set_distance (mCore, 0.05 + value * (20.0 - 0.05));
    }
    else if (id == kParamRotation)
    {
        mRotationNorm = value;
        if (mCore)
            hrtf_set_rotation_phase (mCore, value);
    }
    else if (id == kParamElevation)
    {
        mElevationNorm = value;
        if (mCore)
            hrtf_set_elevation_deg (mCore, -90.0 + value * 180.0);
    }
    else if (id == kParamSpace)
    {
        mSpaceNorm = value;
        if (mCore)
            hrtf_set_space (mCore, value);
    }
}

Steinberg::tresult PLUGIN_API PlugProcessor::process (Steinberg::Vst::ProcessData& data)
{
    /* Parameter changes are honoured at the sample offset the host asked
       for, not applied to the whole block from its first sample. */
    constexpr Steinberg::int32 kMaxQueues = 8;
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

    if (data.numSamples <= 0 || !mCore)
    {
        /* No audio to render, but a host flushes the final value this way. */
        for (Steinberg::int32 i = 0; i < numQueues; ++i)
        {
            if (!queues[i]) continue;
            const Steinberg::int32 points = queues[i]->getPointCount ();
            if (points <= 0) continue;

            Steinberg::int32 offset = 0;
            Steinberg::Vst::ParamValue value = 0.0;
            if (queues[i]->getPoint (points - 1, offset, value) == Steinberg::kResultTrue)
                applyParameter (queues[i]->getParameterId (), value);
        }
        return Steinberg::kResultOk;
    }

    if (data.numInputs < 1 || data.numOutputs < 1)
        return Steinberg::kResultOk;

    Steinberg::Vst::AudioBusBuffers& inBus = data.inputs[0];
    Steinberg::Vst::AudioBusBuffers& outBus = data.outputs[0];

    /* A host may supply null buffers for an inactive bus, so the pointers
       are validated as well as the channel counts. */
    if (outBus.numChannels < 2 || !outBus.channelBuffers32 ||
        !outBus.channelBuffers32[0] || !outBus.channelBuffers32[1])
        return Steinberg::kResultOk;

    const Steinberg::uint32 numSamples = (Steinberg::uint32)data.numSamples;
    const size_t capacity = mMonoBuffer.size ();
    if (capacity == 0)
        return Steinberg::kResultOk; /* setupProcessing was never called */

    float* mono = mMonoBuffer.data ();

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

        copyMono (inBus, mono, windowStart, windowFrames);

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

            float* outSeg[2] = { outBus.channelBuffers32[0] + cursor,
                                 outBus.channelBuffers32[1] + cursor };
            hrtf_process (mCore, mono + (cursor - windowStart), outSeg, (size_t)(next - cursor));
            cursor = next;
        }
    }

    /* The whole block was written, so the output is not silent. A stale
       "silent" flag left set makes hosts skip mixing real audio. */
    outBus.silenceFlags = 0;

    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API PlugProcessor::setState (Steinberg::IBStream* state)
{
    if (!state) return Steinberg::kResultFalse;
    Steinberg::IBStreamer streamer (state);

    Steinberg::int32 version = 0;
    if (!streamer.readInt32 (version)) return Steinberg::kResultFalse;
    if (version != 1 && version != kStateVersion) return Steinberg::kResultFalse;

    double dNorm = 0.0, rNorm = 0.0;
    if (!streamer.readDouble (dNorm)) return Steinberg::kResultFalse;
    if (!streamer.readDouble (rNorm)) return Steinberg::kResultFalse;

    /* Out-of-range or non-finite values are ignored rather than pushed into
       the core, where they would poison the DSP state for the session. */
    if (std::isfinite (dNorm) && dNorm >= 0.0 && dNorm <= 1.0)
        mDistanceNorm = dNorm;
    if (std::isfinite (rNorm) && rNorm >= 0.0 && rNorm <= 1.0)
        mRotationNorm = rNorm;

    if (version >= 2)
    {
        double eNorm = 0.5, sNorm = 0.15;
        if (!streamer.readDouble (eNorm)) return Steinberg::kResultFalse;
        if (!streamer.readDouble (sNorm)) return Steinberg::kResultFalse;

        if (std::isfinite (eNorm) && eNorm >= 0.0 && eNorm <= 1.0)
            mElevationNorm = eNorm;
        if (std::isfinite (sNorm) && sNorm >= 0.0 && sNorm <= 1.0)
            mSpaceNorm = sNorm;
    }

    if (mCore)
    {
        double dist_m = 0.05 + mDistanceNorm * (20.0 - 0.05);
        hrtf_set_distance (mCore, dist_m);
        hrtf_set_rotation_phase (mCore, mRotationNorm);
        hrtf_set_elevation_deg (mCore, -90.0 + mElevationNorm * 180.0);
        hrtf_set_space (mCore, mSpaceNorm);
    }

    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API PlugProcessor::getState (Steinberg::IBStream* state)
{
    if (!state) return Steinberg::kResultFalse;
    Steinberg::IBStreamer streamer (state);

    streamer.writeInt32 (kStateVersion);
    streamer.writeDouble (mDistanceNorm);
    streamer.writeDouble (mRotationNorm);
    streamer.writeDouble (mElevationNorm);
    streamer.writeDouble (mSpaceNorm);

    return Steinberg::kResultOk;
}

} // namespace RotatingHrtf
