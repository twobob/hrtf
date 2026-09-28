#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace RotatingHrtf {

enum ParamIDs : Steinberg::Vst::ParamID
{
    kParamDistance = 100,
    kParamRotation = 101,
    kParamElevation = 102,
    kParamSpace = 103,
    kParamTestPulse = 104
};

// Component-state layout written by getState() and read by setState().
constexpr Steinberg::int32 kStateVersion = 3;

// Unique Class IDs for Processor and Controller (v2)
// {3480B66E-0C1B-4E38-B7D4-9988C7E72B20}
static const Steinberg::FUID kProcessorUID (0x3480B66E, 0x0C1B4E38, 0xB7D49988, 0xC7E72B20);
// {3480B66E-0C1B-4E38-B7D4-9988C7E72B21}
static const Steinberg::FUID kControllerUID (0x3480B66E, 0x0C1B4E38, 0xB7D49988, 0xC7E72B21);

} // namespace RotatingHrtf
