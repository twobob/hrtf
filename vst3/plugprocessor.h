#pragma once

#include "public.sdk/source/vst/vstaudioeffect.h"
#include "../hrtf_core.h"
#include <vector>
#include <atomic>

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
    Steinberg::uint32 PLUGIN_API getLatencySamples () SMTG_OVERRIDE { return 2; }

    /* The Test Pulse generator produces sound from silent input, so a host
       must never suspend processing when the input goes quiet. */
    Steinberg::uint32 PLUGIN_API getTailSamples () SMTG_OVERRIDE { return Steinberg::Vst::kInfiniteTail; }

    Steinberg::tresult PLUGIN_API setState (Steinberg::IBStream* state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getState (Steinberg::IBStream* state) SMTG_OVERRIDE;

    static Steinberg::FUnknown* createInstance (void*) {
        return (Steinberg::Vst::IAudioProcessor*)new PlugProcessor ();
    }

private:
    void applyParameter (Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value);
    void applyFinalParameterValues (Steinberg::Vst::IParameterChanges* changes);
    void syncCore ();
    std::atomic<Steinberg::Vst::ParamValue>* stateSlot (Steinberg::int32 index);

    HrtfCore* mCore = nullptr;
    std::atomic<Steinberg::Vst::ParamValue> mDistanceNorm { hrtf_position_from_distance (2.0) }; // 2 m on the log taper
    std::atomic<Steinberg::Vst::ParamValue> mReflectionsNorm { 1.0 }; // 1 = room reflections on
    std::atomic<Steinberg::Vst::ParamValue> mRotationNorm { 0.0 };
    std::atomic<Steinberg::Vst::ParamValue> mElevationNorm { 0.5 }; // (0.0 - (-90.0)) / 180.0 = 0.5 (0 deg)
    std::atomic<Steinberg::Vst::ParamValue> mSpaceNorm { 0.15 };     // 15% room externalisation
    std::atomic<Steinberg::Vst::ParamValue> mTestPulseNorm { 0.0 }; // 0 = off, 1 = on
    std::atomic<Steinberg::Vst::ParamValue> mTestToneNorm { 0.5 };  // 0 = low rumble, 0.5 = pink noise, 1.0 = crisp transient
    std::atomic<Steinberg::Vst::ParamValue> mEarScaleNorm { 0.5 };  // (1.00 - 0.70) / 0.60 = 0.5 (100% scale)
    HrtfTestGen mTestGen = {};
    std::vector<float> mMonoBuffer;
};

} // namespace RotatingHrtf
