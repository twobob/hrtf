#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"

#include "../hrtf_core.h"

namespace RotatingHrtf {

enum ParamIDs : Steinberg::Vst::ParamID
{
    kParamDistance = 100,
    kParamRotation = 101,
    kParamElevation = 102,
    kParamSpace = 103,
    kParamTestPulse = 104,
    kParamTestTone = 105,
    kParamEarScale = 106,
    kParamReflections = 107
};

constexpr Steinberg::int32 kParamCount = 8;

// Component-state layout written by getState() and read by setState():
// int32 version, then one normalised double per parameter in ParamIDs order.
constexpr Steinberg::int32 kStateVersion = HRTF_STATE_VERSION;

// Unique Class IDs for Processor and Controller (psipi.hrtf)
// {6C225F07-6E5D-4494-988A-38D5A2BAF1E0}
static const Steinberg::FUID kProcessorUID (0x6C225F07, 0x6E5D4494, 0x988A38D5, 0xA2BAF1E0);
// {71C449A2-B67B-4CFF-B351-93FCA56D83E1}
static const Steinberg::FUID kControllerUID (0x71C449A2, 0xB67B4CFF, 0xB35193FC, 0xA56D83E1);

} // namespace RotatingHrtf
