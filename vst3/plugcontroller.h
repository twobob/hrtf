#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"

namespace RotatingHrtf {

class PlugController : public Steinberg::Vst::EditController
{
public:
    static Steinberg::FUnknown* createInstance (void*) {
        return (Steinberg::Vst::IEditController*)new PlugController ();
    }

    Steinberg::tresult PLUGIN_API initialize (Steinberg::FUnknown* context) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setComponentState (Steinberg::IBStream* state) SMTG_OVERRIDE;
};

} // namespace RotatingHrtf
