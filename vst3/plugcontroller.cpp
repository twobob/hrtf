#include "plugcontroller.h"
#include "plugids.h"
#include "base/source/fstreamer.h"
#include "public.sdk/source/vst/vstparameters.h"

namespace RotatingHrtf {

Steinberg::tresult PLUGIN_API PlugController::initialize (Steinberg::FUnknown* context)
{
    Steinberg::tresult result = EditController::initialize (context);
    if (result != Steinberg::kResultTrue)
        return Steinberg::kResultFalse;

    // Distance parameter: 0.05 m to 20.0 m, default 2.0 m
    auto* distParam = new Steinberg::Vst::RangeParameter (
        STR16 ("Distance"), kParamDistance, STR16 ("m"),
        0.05, 20.0, 2.0, 0,
        Steinberg::Vst::ParameterInfo::kCanAutomate);
    parameters.addParameter (distParam);

    // Rotation parameter: 0.0 deg to 360.0 deg, default 0.0 deg
    auto* rotParam = new Steinberg::Vst::RangeParameter (
        STR16 ("Rotation"), kParamRotation, STR16 ("deg"),
        0.0, 360.0, 0.0, 0,
        Steinberg::Vst::ParameterInfo::kCanAutomate);
    parameters.addParameter (rotParam);

    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API PlugController::setComponentState (Steinberg::IBStream* state)
{
    if (!state) return Steinberg::kResultFalse;
    Steinberg::IBStreamer streamer (state);

    Steinberg::int32 version = 0;
    if (!streamer.readInt32 (version)) return Steinberg::kResultFalse;

    double dNorm = 0.0, rNorm = 0.0;
    if (!streamer.readDouble (dNorm)) return Steinberg::kResultFalse;
    if (!streamer.readDouble (rNorm)) return Steinberg::kResultFalse;

    setParamNormalized (kParamDistance, dNorm);
    setParamNormalized (kParamRotation, rNorm);

    return Steinberg::kResultOk;
}

} // namespace RotatingHrtf
