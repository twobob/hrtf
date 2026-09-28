#include "plugcontroller.h"
#include "plugids.h"
#include "base/source/fstreamer.h"
#include "public.sdk/source/vst/vstparameters.h"
#include <cmath>

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

    // Elevation parameter: -90.0 deg to 90.0 deg, default 0.0 deg
    auto* elevParam = new Steinberg::Vst::RangeParameter (
        STR16 ("Elevation"), kParamElevation, STR16 ("deg"),
        -90.0, 90.0, 0.0, 0,
        Steinberg::Vst::ParameterInfo::kCanAutomate);
    parameters.addParameter (elevParam);

    // Space (Room Externalisation) parameter: 0.0 % to 100.0 %, default 15.0 %
    auto* spaceParam = new Steinberg::Vst::RangeParameter (
        STR16 ("Space"), kParamSpace, STR16 ("%"),
        0.0, 100.0, 15.0, 0,
        Steinberg::Vst::ParameterInfo::kCanAutomate);
    parameters.addParameter (spaceParam);

    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API PlugController::setComponentState (Steinberg::IBStream* state)
{
    if (!state) return Steinberg::kResultFalse;
    Steinberg::IBStreamer streamer (state);

    Steinberg::int32 version = 0;
    if (!streamer.readInt32 (version)) return Steinberg::kResultFalse;
    if (version != 1 && version != kStateVersion) return Steinberg::kResultFalse;

    double dNorm = 0.0, rNorm = 0.0;
    if (!streamer.readDouble (dNorm)) return Steinberg::kResultFalse;
    if (!streamer.readDouble (rNorm)) return Steinberg::kResultFalse;

    if (std::isfinite (dNorm) && dNorm >= 0.0 && dNorm <= 1.0)
        setParamNormalized (kParamDistance, dNorm);
    if (std::isfinite (rNorm) && rNorm >= 0.0 && rNorm <= 1.0)
        setParamNormalized (kParamRotation, rNorm);

    if (version >= 2)
    {
        double eNorm = 0.5, sNorm = 0.15;
        if (!streamer.readDouble (eNorm)) return Steinberg::kResultFalse;
        if (!streamer.readDouble (sNorm)) return Steinberg::kResultFalse;

        if (std::isfinite (eNorm) && eNorm >= 0.0 && eNorm <= 1.0)
            setParamNormalized (kParamElevation, eNorm);
        if (std::isfinite (sNorm) && sNorm >= 0.0 && sNorm <= 1.0)
            setParamNormalized (kParamSpace, sNorm);
    }

    return Steinberg::kResultOk;
}

} // namespace RotatingHrtf
