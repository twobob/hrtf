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

Steinberg::tresult PLUGIN_API PlugProcessor::process (Steinberg::Vst::ProcessData& data)
{
    if (data.inputParameterChanges)
    {
        Steinberg::int32 numParams = data.inputParameterChanges->getParameterCount ();
        for (Steinberg::int32 i = 0; i < numParams; ++i)
        {
            Steinberg::Vst::IParamValueQueue* queue = data.inputParameterChanges->getParameterData (i);
            if (!queue) continue;

            Steinberg::Vst::ParamValue val;
            Steinberg::int32 sampleOffset;
            Steinberg::int32 numPoints = queue->getPointCount ();
            if (numPoints > 0 && queue->getPoint (numPoints - 1, sampleOffset, val) == Steinberg::kResultTrue)
            {
                if (queue->getParameterId () == kParamDistance)
                {
                    mDistanceNorm = val;
                    if (mCore)
                    {
                        double dist_m = 0.05 + val * (20.0 - 0.05);
                        hrtf_set_distance (mCore, dist_m);
                    }
                }
                else if (queue->getParameterId () == kParamRotation)
                {
                    mRotationNorm = val;
                    if (mCore)
                    {
                        hrtf_set_rotation_phase (mCore, val);
                    }
                }
            }
        }
    }

    if (data.numSamples <= 0 || !mCore)
        return Steinberg::kResultOk;

    if (data.numInputs < 1 || data.numOutputs < 1)
        return Steinberg::kResultOk;

    Steinberg::Vst::AudioBusBuffers& inBus = data.inputs[0];
    Steinberg::Vst::AudioBusBuffers& outBus = data.outputs[0];

    if (outBus.numChannels < 2)
        return Steinberg::kResultOk;

    Steinberg::uint32 numSamples = (Steinberg::uint32)data.numSamples;
    if (mMonoBuffer.size () < numSamples)
        mMonoBuffer.resize (numSamples);

    float* mono = mMonoBuffer.data ();

    if (inBus.numChannels >= 2 && inBus.channelBuffers32[0] && inBus.channelBuffers32[1])
    {
        const float* inL = inBus.channelBuffers32[0];
        const float* inR = inBus.channelBuffers32[1];
        for (Steinberg::uint32 i = 0; i < numSamples; ++i)
            mono[i] = 0.5f * (inL[i] + inR[i]);
    }
    else if (inBus.numChannels >= 1 && inBus.channelBuffers32[0])
    {
        const float* inL = inBus.channelBuffers32[0];
        std::copy (inL, inL + numSamples, mono);
    }
    else
    {
        std::fill (mono, mono + numSamples, 0.0f);
    }

    float* outStereo[2] = { outBus.channelBuffers32[0], outBus.channelBuffers32[1] };
    hrtf_process (mCore, mono, outStereo, numSamples);

    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API PlugProcessor::setState (Steinberg::IBStream* state)
{
    if (!state) return Steinberg::kResultFalse;
    Steinberg::IBStreamer streamer (state);

    Steinberg::int32 version = 0;
    if (!streamer.readInt32 (version)) return Steinberg::kResultFalse;

    double dNorm = 0.0, rNorm = 0.0;
    if (!streamer.readDouble (dNorm)) return Steinberg::kResultFalse;
    if (!streamer.readDouble (rNorm)) return Steinberg::kResultFalse;

    mDistanceNorm = dNorm;
    mRotationNorm = rNorm;

    if (mCore)
    {
        double dist_m = 0.05 + mDistanceNorm * (20.0 - 0.05);
        hrtf_set_distance (mCore, dist_m);
        hrtf_set_rotation_phase (mCore, mRotationNorm);
    }

    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API PlugProcessor::getState (Steinberg::IBStream* state)
{
    if (!state) return Steinberg::kResultFalse;
    Steinberg::IBStreamer streamer (state);

    streamer.writeInt32 (1);
    streamer.writeDouble (mDistanceNorm);
    streamer.writeDouble (mRotationNorm);

    return Steinberg::kResultOk;
}

} // namespace RotatingHrtf
