#pragma once

#include "public.sdk/source/vst/vstaudioeffect.h"
#include "../hrtf_core.h"
#include <vector>

namespace RotatingHrtf {

class PlugProcessor : public Steinberg::Vst::AudioEffect
{
public:
    PlugProcessor ();
    virtual ~PlugProcessor ();

    Steinberg::tresult PLUGIN_API initialize (Steinberg::FUnknown* context) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API terminate () SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API setBusArrangements (
        Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
        Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API canProcessSampleSize (Steinberg::int32 symbolicSampleSize) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setupProcessing (Steinberg::Vst::ProcessSetup& setup) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setActive (Steinberg::TBool state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API process (Steinberg::Vst::ProcessData& data) SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API setState (Steinberg::IBStream* state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getState (Steinberg::IBStream* state) SMTG_OVERRIDE;

    static Steinberg::FUnknown* createInstance (void*) {
        return (Steinberg::Vst::IAudioProcessor*)new PlugProcessor ();
    }

private:
    void applyParameter (Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value);

    HrtfCore* mCore = nullptr;
    double mSampleRate = 48000.0;
    Steinberg::Vst::ParamValue mDistanceNorm = 0.09774436; // (2.0 - 0.05) / 19.95 = ~0.097744
    Steinberg::Vst::ParamValue mRotationNorm = 0.0;
    Steinberg::Vst::ParamValue mElevationNorm = 0.5; // (0.0 - (-90.0)) / 180.0 = 0.5 (0 deg)
    Steinberg::Vst::ParamValue mSpaceNorm = 0.15;     // 15% room externalisation
    Steinberg::Vst::ParamValue mTestPulseNorm = 0.0; // 0 = off, 1 = on
    HrtfTestGen mTestGen = {};
    std::vector<float> mMonoBuffer;
};

} // namespace RotatingHrtf
