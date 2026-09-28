#include "public.sdk/source/main/pluginfactory.h"
#include "plugids.h"
#include "plugprocessor.h"
#include "plugcontroller.h"
#include "version.h"

#define stringSubCategory Steinberg::Vst::PlugType::kSpatialFx

BEGIN_FACTORY_DEF (stringCompanyName, stringCompanyWeb, stringCompanyEmail)

    DEF_CLASS2 (INLINE_UID_FROM_FUID (RotatingHrtf::kProcessorUID),
                PClassInfo::kManyInstances,
                kVstAudioEffectClass,
                stringPluginName,
                Steinberg::Vst::kDistributable,
                stringSubCategory,
                FULL_VERSION_STR,
                kVstVersionString,
                RotatingHrtf::PlugProcessor::createInstance)

    DEF_CLASS2 (INLINE_UID_FROM_FUID (RotatingHrtf::kControllerUID),
                PClassInfo::kManyInstances,
                kVstComponentControllerClass,
                stringPluginName " Controller",
                0,
                "",
                FULL_VERSION_STR,
                kVstVersionString,
                RotatingHrtf::PlugController::createInstance)

END_FACTORY
