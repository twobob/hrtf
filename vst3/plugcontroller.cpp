#include "plugcontroller.h"
#include "plugids.h"
#include "base/source/fstreamer.h"
#include "public.sdk/source/vst/vstparameters.h"
#include <cmath>

namespace RotatingHrtf {

namespace {

/* Distance in metres on a logarithmic taper, so every doubling of distance
   takes the same knob travel (5 cm to 1 m is half of it). The mapping is
   shared with the processor and the CLAP build through hrtf_core.h. */
class DistanceParameter : public Steinberg::Vst::RangeParameter
{
public:
    DistanceParameter (const Steinberg::Vst::TChar* title, Steinberg::Vst::ParamID tag,
                       const Steinberg::Vst::TChar* units, Steinberg::Vst::ParamValue defaultPlain,
                       Steinberg::int32 flags)
    : RangeParameter (title, tag, units, HRTF_DISTANCE_MIN_M, HRTF_DISTANCE_MAX_M, defaultPlain, 0, flags)
    {
        // The base constructor normalised the default linearly (virtual
        // dispatch does not reach this class during construction).
        info.defaultNormalizedValue = valueNormalized = toNormalized (defaultPlain);
        setPrecision (2);
    }

    Steinberg::Vst::ParamValue toPlain (Steinberg::Vst::ParamValue normalised) const SMTG_OVERRIDE
    {
        return hrtf_distance_from_position (normalised);
    }

    Steinberg::Vst::ParamValue toNormalized (Steinberg::Vst::ParamValue plain) const SMTG_OVERRIDE
    {
        return hrtf_position_from_distance (plain);
    }
};

Steinberg::Vst::StringListParameter* makeSwitch (const Steinberg::Vst::TChar* title,
                                                 Steinberg::Vst::ParamID tag, bool defaultOn)
{
    auto* param = new Steinberg::Vst::StringListParameter (
        title, tag, nullptr,
        Steinberg::Vst::ParameterInfo::kCanAutomate | Steinberg::Vst::ParameterInfo::kIsList);
    param->appendString (STR16 ("Off"));
    param->appendString (STR16 ("On"));
    param->getInfo ().defaultNormalizedValue = defaultOn ? 1.0 : 0.0;
    param->setNormalized (defaultOn ? 1.0 : 0.0);
    return param;
}

} // namespace

Steinberg::tresult PLUGIN_API PlugController::initialize (Steinberg::FUnknown* context)
{
    Steinberg::tresult result = EditController::initialize (context);
    if (result != Steinberg::kResultTrue)
        return Steinberg::kResultFalse;

    // Distance parameter: 0.05 m to 20.0 m (logarithmic), default 2.0 m
    parameters.addParameter (new DistanceParameter (
        STR16 ("Distance"), kParamDistance, STR16 ("m"), 2.0,
        Steinberg::Vst::ParameterInfo::kCanAutomate));

    // Rotation parameter: 0.0 deg to 360.0 deg, default 0.0 deg, wraps round
    auto* rotParam = new Steinberg::Vst::RangeParameter (
        STR16 ("Rotation"), kParamRotation, STR16 ("deg"),
        0.0, 360.0, 0.0, 0,
        Steinberg::Vst::ParameterInfo::kCanAutomate | Steinberg::Vst::ParameterInfo::kIsWrapAround);
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

    // Test Pulse parameter: Off / On, default Off (tick box in Ableton Live)
    parameters.addParameter (makeSwitch (STR16 ("Test Pulse"), kParamTestPulse, false));

    // Test Tone parameter: 0.0 % to 100.0 %, default 50.0 % (0 = low rumble, 50 = pink noise, 100 = crisp transient)
    auto* toneParam = new Steinberg::Vst::RangeParameter (
        STR16 ("Test Tone"), kParamTestTone, STR16 ("%"),
        0.0, 100.0, 50.0, 0,
        Steinberg::Vst::ParameterInfo::kCanAutomate);
    parameters.addParameter (toneParam);

    // Ear Scale (Anthropometric Pinna/Head Scaling) parameter: 70.0 % to 130.0 %, default 100.0 %
    auto* earParam = new Steinberg::Vst::RangeParameter (
        STR16 ("Ear Scale"), kParamEarScale, STR16 ("%"),
        70.0, 130.0, 100.0, 0,
        Steinberg::Vst::ParameterInfo::kCanAutomate);
    parameters.addParameter (earParam);

    // Reflections parameter: room reflections On / Off (bypass), default On
    parameters.addParameter (makeSwitch (STR16 ("Reflections"), kParamReflections, true));

    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API PlugController::setComponentState (Steinberg::IBStream* state)
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

    for (Steinberg::int32 i = 0; i < kParamCount; ++i)
    {
        if (std::isfinite (norm[i]) && norm[i] >= 0.0 && norm[i] <= 1.0)
            setParamNormalized (kParamDistance + i, norm[i]);
    }

    if (componentHandler)
        componentHandler->restartComponent (Steinberg::Vst::kParamValuesChanged);

    return Steinberg::kResultOk;
}

} // namespace RotatingHrtf
