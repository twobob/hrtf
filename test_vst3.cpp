#include <iostream>
#include <windows.h>
#include <cmath>

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "vst3/plugids.h"

class DummyQueue : public Steinberg::Vst::IParamValueQueue
{
public:
    Steinberg::Vst::ParamID id = 0;
    double val = 0.0;
    Steinberg::tresult PLUGIN_API queryInterface (const Steinberg::TUID, void**) override { return Steinberg::kNoInterface; }
    Steinberg::uint32 PLUGIN_API addRef () override { return 1; }
    Steinberg::uint32 PLUGIN_API release () override { return 1; }
    Steinberg::Vst::ParamID PLUGIN_API getParameterId () override { return id; }
    Steinberg::int32 PLUGIN_API getPointCount () override { return 1; }
    Steinberg::tresult PLUGIN_API getPoint (Steinberg::int32, Steinberg::int32& offset, Steinberg::Vst::ParamValue& value) override {
        offset = 0; value = val; return Steinberg::kResultTrue;
    }
    Steinberg::tresult PLUGIN_API addPoint (Steinberg::int32, Steinberg::Vst::ParamValue, Steinberg::int32&) override { return Steinberg::kResultOk; }
};

class DummyChanges : public Steinberg::Vst::IParameterChanges
{
public:
    DummyQueue q;
    Steinberg::tresult PLUGIN_API queryInterface (const Steinberg::TUID, void**) override { return Steinberg::kNoInterface; }
    Steinberg::uint32 PLUGIN_API addRef () override { return 1; }
    Steinberg::uint32 PLUGIN_API release () override { return 1; }
    Steinberg::int32 PLUGIN_API getParameterCount () override { return 1; }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData (Steinberg::int32 index) override { return index == 0 ? &q : nullptr; }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData (const Steinberg::Vst::ParamID&, Steinberg::int32&) override { return &q; }
};

typedef bool (*InitDllFunc)();
typedef bool (*ExitDllFunc)();
typedef Steinberg::IPluginFactory* (*GetPluginFactoryFunc)();

int main()
{
    int failures = 0;

    std::cout << "Loading RotatingHRTF_v2.vst3...\n";
    HMODULE lib = LoadLibraryA("RotatingHRTF_v2.vst3");
    if (!lib)
    {
        std::cerr << "Failed to load RotatingHRTF_v2.vst3. Error: " << GetLastError() << "\n";
        return 1;
    }

    auto initDll = (InitDllFunc)GetProcAddress(lib, "InitDll");
    auto exitDll = (ExitDllFunc)GetProcAddress(lib, "ExitDll");
    auto getPluginFactory = (GetPluginFactoryFunc)GetProcAddress(lib, "GetPluginFactory");

    if (!initDll || !exitDll || !getPluginFactory)
    {
        std::cerr << "Missing VST3 entry points!\n";
        FreeLibrary(lib);
        return 1;
    }

    if (!initDll())
    {
        std::cerr << "InitDll failed!\n";
        FreeLibrary(lib);
        return 1;
    }

    Steinberg::IPluginFactory* factory = getPluginFactory();
    if (!factory)
    {
        std::cerr << "GetPluginFactory returned null!\n";
        exitDll();
        FreeLibrary(lib);
        return 1;
    }

    Steinberg::PFactoryInfo factoryInfo;
    factory->getFactoryInfo(&factoryInfo);
    std::cout << "Vendor: " << factoryInfo.vendor << "\n";
    std::cout << "Classes count: " << factory->countClasses() << "\n";

    for (Steinberg::int32 i = 0; i < factory->countClasses(); ++i)
    {
        Steinberg::PClassInfo classInfo;
        factory->getClassInfo(i, &classInfo);
        std::cout << "  Class " << i << ": " << classInfo.name << " (Category: " << classInfo.category << ")\n";
    }

    // Create processor
    Steinberg::Vst::IAudioProcessor* processor = nullptr;
    Steinberg::tresult res = factory->createInstance(
        RotatingHrtf::kProcessorUID,
        Steinberg::Vst::IAudioProcessor::iid,
        (void**)&processor);

    if (res != Steinberg::kResultTrue || !processor)
    {
        std::cerr << "Failed to create audio processor instance!\n";
        exitDll();
        FreeLibrary(lib);
        return 1;
    }

    // Initialize processor
    Steinberg::Vst::IComponent* comp = nullptr;
    if (processor->queryInterface(Steinberg::Vst::IComponent::iid, (void**)&comp) == Steinberg::kResultTrue && comp)
    {
        comp->initialize(nullptr);
    }

    // Setup processing
    Steinberg::Vst::ProcessSetup setup = {};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 256;
    setup.sampleRate = 48000.0;
    processor->setupProcessing(setup);

    if (comp)
    {
        comp->setActive(true);
    }

    // Create controller
    Steinberg::Vst::IEditController* controller = nullptr;
    res = factory->createInstance(
        RotatingHrtf::kControllerUID,
        Steinberg::Vst::IEditController::iid,
        (void**)&controller);

    if (res == Steinberg::kResultTrue && controller)
    {
        controller->initialize(nullptr);
        std::cout << "Controller parameter count: " << controller->getParameterCount() << "\n";
        for (Steinberg::int32 p = 0; p < controller->getParameterCount(); ++p)
        {
            Steinberg::Vst::ParameterInfo pInfo = {};
            controller->getParameterInfo(p, pInfo);
            std::wcout << L"    Param " << p << L": ID=" << pInfo.id << L", Title=" << (wchar_t*)pInfo.title << L", Units=" << (wchar_t*)pInfo.units << L"\n";
        }
        controller->terminate();
        controller->release();
    }

    // Process audio test block
    const int N = 256;
    float in_l[256];
    float in_r[256];
    float out_l[256] = {0};
    float out_r[256] = {0};

    for (int i = 0; i < N; ++i)
    {
        in_l[i] = sinf(2.0f * 3.14159265f * 440.0f * (float)i / 48000.0f);
        in_r[i] = in_l[i];
    }

    float* inChannelPtrs[2] = { in_l, in_r };
    float* outChannelPtrs[2] = { out_l, out_r };

    Steinberg::Vst::AudioBusBuffers inBus = {};
    inBus.numChannels = 2;
    inBus.channelBuffers32 = inChannelPtrs;

    Steinberg::Vst::AudioBusBuffers outBus = {};
    outBus.numChannels = 2;
    outBus.channelBuffers32 = outChannelPtrs;

    Steinberg::Vst::ProcessData processData = {};
    processData.processMode = Steinberg::Vst::kRealtime;
    processData.symbolicSampleSize = Steinberg::Vst::kSample32;
    processData.numSamples = N;
    processData.numInputs = 1;
    processData.inputs = &inBus;
    processData.numOutputs = 1;
    processData.outputs = &outBus;
    processData.inputParameterChanges = nullptr;
    processData.outputParameterChanges = nullptr;

    res = processor->process(processData);
    std::cout << "Process result (center): " << (res == Steinberg::kResultOk ? "OK" : "FAILED") << "\n";
    if (res != Steinberg::kResultOk)
    {
        std::cerr << "ERROR: process() did not return kResultOk!\n";
        ++failures;
    }

    float sum_l = 0.0f, sum_r = 0.0f;
    for (int i = 0; i < N; ++i)
    {
        sum_l += std::abs(out_l[i]);
        sum_r += std::abs(out_r[i]);
    }
    std::cout << "Center Energy: Left = " << sum_l << ", Right = " << sum_r << "\n";

    // Now test Parameter Changes: Rotate to 90 degrees Right (phase = 0.25)
    DummyChanges paramChanges;
    paramChanges.q.id = RotatingHrtf::kParamRotation;
    paramChanges.q.val = 0.25;

    processData.inputParameterChanges = &paramChanges;
    // Process several blocks to let parameter slew settle (20ms slew = ~960 samples @ 48kHz = 4 blocks)
    for (int block = 0; block < 10; ++block)
    {
        processor->process(processData);
        processData.inputParameterChanges = nullptr; // only send on first block
    }

    float rot_sum_l = 0.0f, rot_sum_r = 0.0f;
    for (int i = 0; i < N; ++i)
    {
        rot_sum_l += std::abs(out_l[i]);
        rot_sum_r += std::abs(out_r[i]);
    }
    std::cout << "Right-Panned Energy (90 deg): Left Ear = " << rot_sum_l << ", Right Ear = " << rot_sum_r << "\n";

    if (rot_sum_r > rot_sum_l * 1.5f)
    {
        std::cout << "SUCCESS: HRTF head-shadow and panning correctly biased energy to the right ear!\n";
    }
    else
    {
        std::cerr << "ERROR: expected the right ear to receive significantly more energy than the left at 90 deg (left="
                  << rot_sum_l << ", right=" << rot_sum_r << ")!\n";
        ++failures;
    }

    if (sum_l > 0.01f && sum_r > 0.01f)
    {
        std::cout << "SUCCESS: VST3 plugin processed stereo input into binaural output successfully!\n";
    }
    else
    {
        std::cerr << "ERROR: VST3 plugin output was silent!\n";
        ++failures;
    }

    if (comp)
    {
        comp->setActive(false);
        comp->terminate();
        comp->release();
    }
    processor->release();

    exitDll();
    FreeLibrary(lib);

    if (failures > 0)
    {
        std::cerr << "VST3 test FAILED (" << failures << " check(s))\n";
        return 1;
    }

    std::cout << "VST3 test completed successfully!\n";
    return 0;
}
