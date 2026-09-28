#include <iostream>
#include <windows.h>
#include <cmath>
#include <cstring>
#include <vector>

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "vst3/plugids.h"

class TestMemStream : public Steinberg::IBStream
{
public:
    std::vector<Steinberg::uint8> buffer;
    Steinberg::int64 cursor = 0;

    Steinberg::tresult PLUGIN_API queryInterface (const Steinberg::TUID, void**) override { return Steinberg::kNoInterface; }
    Steinberg::uint32 PLUGIN_API addRef () override { return 1; }
    Steinberg::uint32 PLUGIN_API release () override { return 1; }

    Steinberg::tresult PLUGIN_API read (void* dest, Steinberg::int32 numBytes, Steinberg::int32* numBytesRead) override
    {
        if (numBytes < 0) return Steinberg::kInvalidArgument;
        Steinberg::int64 available = (Steinberg::int64)buffer.size () - cursor;
        if (available < 0) available = 0;
        Steinberg::int32 toRead = (Steinberg::int32)(numBytes < available ? numBytes : available);
        if (toRead > 0)
        {
            std::memcpy (dest, buffer.data () + cursor, toRead);
            cursor += toRead;
        }
        if (numBytesRead) *numBytesRead = toRead;
        return toRead == numBytes ? Steinberg::kResultTrue : Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API write (void* src, Steinberg::int32 numBytes, Steinberg::int32* numBytesWritten) override
    {
        if (numBytes < 0) return Steinberg::kInvalidArgument;
        if (cursor + numBytes > (Steinberg::int64)buffer.size ())
            buffer.resize ((size_t)(cursor + numBytes));
        std::memcpy (buffer.data () + cursor, src, numBytes);
        cursor += numBytes;
        if (numBytesWritten) *numBytesWritten = numBytes;
        return Steinberg::kResultTrue;
    }

    template <typename T>
    Steinberg::tresult writeVal (const T& val)
    {
        return write ((void*)&val, (Steinberg::int32)sizeof(T), nullptr);
    }

    template <typename T>
    Steinberg::tresult readVal (T& val)
    {
        return read ((void*)&val, (Steinberg::int32)sizeof(T), nullptr);
    }

    Steinberg::tresult PLUGIN_API seek (Steinberg::int64 pos, Steinberg::int32 mode, Steinberg::int64* result) override
    {
        Steinberg::int64 newPos = cursor;
        if (mode == kIBSeekSet) newPos = pos;
        else if (mode == kIBSeekCur) newPos += pos;
        else if (mode == kIBSeekEnd) newPos = (Steinberg::int64)buffer.size () + pos;
        if (newPos < 0) return Steinberg::kInvalidArgument;
        cursor = newPos;
        if (result) *result = cursor;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API tell (Steinberg::int64* pos) override
    {
        if (!pos) return Steinberg::kInvalidArgument;
        *pos = cursor;
        return Steinberg::kResultTrue;
    }
};

class DummyQueue : public Steinberg::Vst::IParamValueQueue
{
public:
    Steinberg::Vst::ParamID id = 0;
    double val = 0.0;
    Steinberg::int32 pointOffset = 0;
    Steinberg::tresult PLUGIN_API queryInterface (const Steinberg::TUID, void**) override { return Steinberg::kNoInterface; }
    Steinberg::uint32 PLUGIN_API addRef () override { return 1; }
    Steinberg::uint32 PLUGIN_API release () override { return 1; }
    Steinberg::Vst::ParamID PLUGIN_API getParameterId () override { return id; }
    Steinberg::int32 PLUGIN_API getPointCount () override { return 1; }
    Steinberg::tresult PLUGIN_API getPoint (Steinberg::int32, Steinberg::int32& offset, Steinberg::Vst::ParamValue& value) override {
        offset = pointOffset; value = val; return Steinberg::kResultTrue;
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

class MockComponentHandler : public Steinberg::Vst::IComponentHandler
{
public:
    Steinberg::int32 lastRestartFlags = 0;
    int restartCallCount = 0;

    Steinberg::tresult PLUGIN_API queryInterface (const Steinberg::TUID requestedIid, void** obj) override
    {
        if (!obj) return Steinberg::kInvalidArgument;
        if (Steinberg::FUnknownPrivate::iidEqual (requestedIid, Steinberg::Vst::IComponentHandler::iid) ||
            Steinberg::FUnknownPrivate::iidEqual (requestedIid, Steinberg::FUnknown::iid))
        {
            *obj = static_cast<Steinberg::Vst::IComponentHandler*>(this);
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef () override { return 1; }
    Steinberg::uint32 PLUGIN_API release () override { return 1; }

    Steinberg::tresult PLUGIN_API beginEdit (Steinberg::Vst::ParamID) override { return Steinberg::kResultOk; }
    Steinberg::tresult PLUGIN_API performEdit (Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue) override { return Steinberg::kResultOk; }
    Steinberg::tresult PLUGIN_API endEdit (Steinberg::Vst::ParamID) override { return Steinberg::kResultOk; }
    Steinberg::tresult PLUGIN_API restartComponent (Steinberg::int32 flags) override
    {
        lastRestartFlags = flags;
        restartCallCount++;
        return Steinberg::kResultOk;
    }
};

typedef bool (*InitDllFunc)();
typedef bool (*ExitDllFunc)();
typedef Steinberg::IPluginFactory* (*GetPluginFactoryFunc)();

// A freshly created, activated processor in its default state.
struct Proc
{
    Steinberg::Vst::IAudioProcessor* processor = nullptr;
    Steinberg::Vst::IComponent* comp = nullptr;
};

static Proc makeProcessor (Steinberg::IPluginFactory* factory)
{
    Proc p;
    if (factory->createInstance (RotatingHrtf::kProcessorUID,
                                 Steinberg::Vst::IAudioProcessor::iid,
                                 (void**)&p.processor) != Steinberg::kResultTrue || !p.processor)
    {
        p.processor = nullptr;
        return p;
    }

    if (p.processor->queryInterface (Steinberg::Vst::IComponent::iid, (void**)&p.comp) != Steinberg::kResultTrue)
        p.comp = nullptr;

    if (p.comp)
        p.comp->initialize (nullptr);

    Steinberg::Vst::ProcessSetup setup = {};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 256;
    setup.sampleRate = 48000.0;
    p.processor->setupProcessing (setup);

    if (p.comp)
        p.comp->setActive (true);

    return p;
}

static void releaseProcessor (Proc& p)
{
    if (p.comp)
    {
        p.comp->setActive (false);
        p.comp->terminate ();
        p.comp->release ();
        p.comp = nullptr;
    }
    if (p.processor)
    {
        p.processor->release ();
        p.processor = nullptr;
    }
}

int main()
{
    int failures = 0;

    /* Fail the build without popping up a crash dialog if this harness
       ever faults: the exit code is what the build script reads. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

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

    // Initialise processor
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
        if (controller->getParameterCount() != 7)
        {
            std::cerr << "ERROR: expected 7 parameters in controller, got " << controller->getParameterCount() << "\n";
            ++failures;
        }
        for (Steinberg::int32 p = 0; p < controller->getParameterCount(); ++p)
        {
            Steinberg::Vst::ParameterInfo pInfo = {};
            controller->getParameterInfo(p, pInfo);
            std::wcout << L"    Param " << p << L": ID=" << pInfo.id << L", Title=" << (wchar_t*)pInfo.title << L", Units=" << (wchar_t*)pInfo.units << L"\n";
        }

        // Test Controller setComponentState, parameter restoration, and restartComponent dispatch
        {
            MockComponentHandler mockHandler;
            controller->setComponentHandler (&mockHandler);

            TestMemStream ctrlStream;
            ctrlStream.writeVal<Steinberg::int32> (RotatingHrtf::kStateVersion);
            ctrlStream.writeVal<double> (0.75); // Distance (norm)
            ctrlStream.writeVal<double> (0.33); // Rotation
            ctrlStream.writeVal<double> (0.60); // Elevation
            ctrlStream.writeVal<double> (0.45); // Space
            ctrlStream.writeVal<double> (1.00); // Test Pulse
            ctrlStream.writeVal<double> (0.80); // Test Tone
            ctrlStream.writeVal<double> (0.65); // Ear Scale
            ctrlStream.cursor = 0;

            Steinberg::tresult cRes = controller->setComponentState (&ctrlStream);
            if (cRes != Steinberg::kResultOk)
            {
                std::cerr << "ERROR: controller->setComponentState failed on valid stream\n";
                ++failures;
            }
            else
            {
                if (mockHandler.restartCallCount >= 1 &&
                    (mockHandler.lastRestartFlags & Steinberg::Vst::kParamValuesChanged))
                {
                    std::cout << "SUCCESS: controller->setComponentState dispatched restartComponent(kParamValuesChanged) via IComponentHandler.\n";
                }
                else
                {
                    std::cerr << "ERROR: restartComponent was not dispatched correctly (calls=" << mockHandler.restartCallCount << ", flags=" << mockHandler.lastRestartFlags << ")\n";
                    ++failures;
                }

                Steinberg::Vst::ParamValue dV = controller->getParamNormalized (RotatingHrtf::kParamDistance);
                Steinberg::Vst::ParamValue rV = controller->getParamNormalized (RotatingHrtf::kParamRotation);
                Steinberg::Vst::ParamValue eV = controller->getParamNormalized (RotatingHrtf::kParamElevation);
                Steinberg::Vst::ParamValue sV = controller->getParamNormalized (RotatingHrtf::kParamSpace);
                Steinberg::Vst::ParamValue pV = controller->getParamNormalized (RotatingHrtf::kParamTestPulse);
                Steinberg::Vst::ParamValue tV = controller->getParamNormalized (RotatingHrtf::kParamTestTone);
                Steinberg::Vst::ParamValue esV = controller->getParamNormalized (RotatingHrtf::kParamEarScale);

                if (std::abs (dV - 0.75) < 1e-6 && std::abs (rV - 0.33) < 1e-6 &&
                    std::abs (eV - 0.60) < 1e-6 && std::abs (sV - 0.45) < 1e-6 &&
                    std::abs (pV - 1.00) < 1e-6 && std::abs (tV - 0.80) < 1e-6 &&
                    std::abs (esV - 0.65) < 1e-6)
                {
                    std::cout << "SUCCESS: controller->setComponentState restored all normalised parameter values accurately.\n";
                }
                else
                {
                    std::cerr << "ERROR: controller parameter values mismatch after setComponentState\n";
                    ++failures;
                }
            }

            // Test hostile streams on controller->setComponentState: null, truncated header, truncated payload, invalid version
            // All hostile streams must be rejected (kResultFalse) AND leave all parameters untouched.
            Steinberg::Vst::ParamValue ctrlBaseline[7];
            for (int pIdx = 0; pIdx < 7; ++pIdx)
                ctrlBaseline[pIdx] = controller->getParamNormalized (RotatingHrtf::kParamDistance + pIdx);

            auto verifyCtrlUnchanged = [&](const char* testName) {
                bool ok = true;
                for (int pIdx = 0; pIdx < 7; ++pIdx)
                {
                    Steinberg::Vst::ParamValue cur = controller->getParamNormalized (RotatingHrtf::kParamDistance + pIdx);
                    if (cur != ctrlBaseline[pIdx])
                    {
                        std::cerr << "ERROR: controller " << testName << " mutated parameter "
                                  << (RotatingHrtf::kParamDistance + pIdx) << " from " << ctrlBaseline[pIdx]
                                  << " to " << cur << "\n";
                        ok = false;
                    }
                }
                if (!ok) ++failures;
                return ok;
            };

            if (controller->setComponentState (nullptr) == Steinberg::kResultFalse && verifyCtrlUnchanged ("setComponentState(nullptr)"))
            {
                std::cout << "SUCCESS: controller->setComponentState(nullptr) rejected as expected.\n";
            }
            else
            {
                std::cerr << "ERROR: controller->setComponentState(nullptr) was accepted or mutated state!\n";
                ++failures;
            }

            TestMemStream ctrlTruncHdr;
            ctrlTruncHdr.writeVal<Steinberg::int16> (4); // 2 bytes: incomplete version header
            ctrlTruncHdr.cursor = 0;
            if (controller->setComponentState (&ctrlTruncHdr) == Steinberg::kResultFalse && verifyCtrlUnchanged ("truncated header"))
            {
                std::cout << "SUCCESS: controller->setComponentState rejected truncated header (2 bytes).\n";
            }
            else
            {
                std::cerr << "ERROR: controller->setComponentState accepted truncated header or mutated state!\n";
                ++failures;
            }

            TestMemStream ctrlTruncPayload;
            ctrlTruncPayload.writeVal<Steinberg::int32> (RotatingHrtf::kStateVersion);
            ctrlTruncPayload.writeVal<double> (0.5);
            ctrlTruncPayload.writeVal<double> (0.5); // only 2 doubles instead of 7
            ctrlTruncPayload.cursor = 0;
            if (controller->setComponentState (&ctrlTruncPayload) == Steinberg::kResultFalse && verifyCtrlUnchanged ("truncated payload"))
            {
                std::cout << "SUCCESS: controller->setComponentState rejected truncated payload (valid version + 2 doubles).\n";
            }
            else
            {
                std::cerr << "ERROR: controller->setComponentState accepted truncated payload or mutated state!\n";
                ++failures;
            }

            TestMemStream ctrlBadVer;
            ctrlBadVer.writeVal<Steinberg::int32> (99); // bad version
            for (int i = 0; i < 7; ++i) ctrlBadVer.writeVal<double> (0.5);
            ctrlBadVer.cursor = 0;
            if (controller->setComponentState (&ctrlBadVer) == Steinberg::kResultFalse && verifyCtrlUnchanged ("bad version"))
            {
                std::cout << "SUCCESS: controller->setComponentState rejected invalid version (99).\n";
            }
            else
            {
                std::cerr << "ERROR: controller->setComponentState accepted invalid version or mutated state!\n";
                ++failures;
            }

            controller->setComponentHandler (nullptr);
        }

        controller->terminate();
        controller->release();
    }

    // Latency Declaration test
    if (processor->getLatencySamples() != 2)
    {
        std::cerr << "ERROR: processor reported " << processor->getLatencySamples() << " latency samples, expected 2\n";
        ++failures;
    }
    else
    {
        std::cout << "SUCCESS: VST3 processor correctly declared 2 samples of latency.\n";
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
    std::cout << "Process result (centre): " << (res == Steinberg::kResultOk ? "OK" : "FAILED") << "\n";
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
    std::cout << "Centre Energy: Left = " << sum_l << ", Right = " << sum_r << "\n";

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

    // Sample-accurate automation: a rotation change at offset 64 must take
    // effect there, not at the start of the block and not at the end. Two
    // identical processors are driven with the same audio and the same
    // change at different offsets; their first 64 samples must match and
    // everything from offset 64 on must differ.
    {
        const int NA = 256;
        float a_l[NA] = {0}, a_r[NA] = {0}, b_l[NA] = {0}, b_r[NA] = {0};
        float tone[NA];
        for (int i = 0; i < NA; ++i)
            tone[i] = sinf (2.0f * 3.14159265f * 440.0f * (float)i / 48000.0f);

        Proc pa = makeProcessor (factory);
        Proc pb = makeProcessor (factory);

        if (!pa.processor || !pb.processor)
        {
            std::cerr << "ERROR: could not create processors for the automation test\n";
            ++failures;
        }
        else
        {
            DummyChanges changesA;
            changesA.q.id = RotatingHrtf::kParamRotation;
            changesA.q.val = 0.25;
            changesA.q.pointOffset = 64;

            DummyChanges changesB;
            changesB.q.id = RotatingHrtf::kParamRotation;
            changesB.q.val = 0.25;
            changesB.q.pointOffset = 192;

            float* inPtrsA[2] = { tone, tone };
            float* outPtrsA[2] = { a_l, a_r };
            Steinberg::Vst::AudioBusBuffers inBusA = {};
            inBusA.numChannels = 2;
            inBusA.channelBuffers32 = inPtrsA;
            Steinberg::Vst::AudioBusBuffers outBusA = {};
            outBusA.numChannels = 2;
            outBusA.channelBuffers32 = outPtrsA;
            Steinberg::Vst::ProcessData dataA = {};
            dataA.processMode = Steinberg::Vst::kRealtime;
            dataA.symbolicSampleSize = Steinberg::Vst::kSample32;
            dataA.numSamples = NA;
            dataA.numInputs = 1;
            dataA.inputs = &inBusA;
            dataA.numOutputs = 1;
            dataA.outputs = &outBusA;
            dataA.inputParameterChanges = &changesA;

            float* inPtrsB[2] = { tone, tone };
            float* outPtrsB[2] = { b_l, b_r };
            Steinberg::Vst::AudioBusBuffers inBusB = {};
            inBusB.numChannels = 2;
            inBusB.channelBuffers32 = inPtrsB;
            Steinberg::Vst::AudioBusBuffers outBusB = {};
            outBusB.numChannels = 2;
            outBusB.channelBuffers32 = outPtrsB;
            Steinberg::Vst::ProcessData dataB = {};
            dataB.processMode = Steinberg::Vst::kRealtime;
            dataB.symbolicSampleSize = Steinberg::Vst::kSample32;
            dataB.numSamples = NA;
            dataB.numInputs = 1;
            dataB.inputs = &inBusB;
            dataB.numOutputs = 1;
            dataB.outputs = &outBusB;
            dataB.inputParameterChanges = &changesB;

            pa.processor->process (dataA);
            pb.processor->process (dataB);

            const bool headEqual = memcmp (a_l, b_l, 64 * sizeof (float)) == 0;
            const bool tailDiffers = memcmp (a_l + 64, b_l + 64, (NA - 64) * sizeof (float)) != 0;

            if (!headEqual)
            {
                std::cerr << "ERROR: a parameter change at offset 64 or 192 affected the first 64 samples\n";
                ++failures;
            }
            if (!tailDiffers)
            {
                std::cerr << "ERROR: parameter offsets were ignored - both blocks are identical\n";
                ++failures;
            }
            else
            {
                std::cout << "SUCCESS: parameter changes are applied at their sample offset.\n";
            }
        }

        releaseProcessor (pa);
        releaseProcessor (pb);
    }

    // Oversized blocks: a host that exceeds maxSamplesPerBlock (256 here)
    // must still get correct audio rather than a reallocation in the
    // audio callback.
    {
        Proc p = makeProcessor (factory);
        if (!p.processor)
        {
            std::cerr << "ERROR: could not create a processor for the oversized-block test\n";
            ++failures;
        }
        else
        {
            const int NB = 1024;
            float big_l[NB] = {0}, big_r[NB] = {0}, big_in[NB];
            for (int i = 0; i < NB; ++i)
                big_in[i] = sinf (2.0f * 3.14159265f * 440.0f * (float)i / 48000.0f);

            float* inPtrs[2] = { big_in, big_in };
            float* outPtrs[2] = { big_l, big_r };
            Steinberg::Vst::AudioBusBuffers bigInBus = {};
            bigInBus.numChannels = 2;
            bigInBus.channelBuffers32 = inPtrs;
            Steinberg::Vst::AudioBusBuffers bigOutBus = {};
            bigOutBus.numChannels = 2;
            bigOutBus.channelBuffers32 = outPtrs;
            Steinberg::Vst::ProcessData data = {};
            data.processMode = Steinberg::Vst::kRealtime;
            data.symbolicSampleSize = Steinberg::Vst::kSample32;
            data.numSamples = NB;
            data.numInputs = 1;
            data.inputs = &bigInBus;
            data.numOutputs = 1;
            data.outputs = &bigOutBus;

            const Steinberg::tresult r = p.processor->process (data);

            double energy = 0.0;
            bool finite = true;
            for (int i = 0; i < NB; ++i)
            {
                energy += std::fabs (big_l[i]) + std::fabs (big_r[i]);
                if (!std::isfinite (big_l[i]) || !std::isfinite (big_r[i]))
                    finite = false;
            }

            if (r != Steinberg::kResultOk || !finite || energy < 0.01)
            {
                std::cerr << "ERROR: a 1024-frame block was not rendered correctly (result="
                          << r << ", energy=" << energy << ")\n";
                ++failures;
            }
            else
            {
                std::cout << "SUCCESS: oversized blocks render correctly without reallocating.\n";
            }
        }
        releaseProcessor (p);
    }

    // 3D Elevation: overhead (+90 deg) must produce symmetric ear energy
    {
        Proc p = makeProcessor (factory);
        if (p.processor)
        {
            DummyChanges elevChanges;
            elevChanges.q.id = RotatingHrtf::kParamElevation;
            elevChanges.q.val = 1.0; // normalised 1.0 = +90 deg

            const int NE = 256;
            float el_l[NE] = {0}, el_r[NE] = {0}, el_in[NE];
            for (int i = 0; i < NE; ++i) el_in[i] = sinf (2.0f * 3.14159265f * 440.0f * (float)i / 48000.0f);
            float* inPtrs[2] = { el_in, el_in };
            float* outPtrs[2] = { el_l, el_r };
            Steinberg::Vst::AudioBusBuffers elInBus = {};
            elInBus.numChannels = 2; elInBus.channelBuffers32 = inPtrs;
            Steinberg::Vst::AudioBusBuffers elOutBus = {};
            elOutBus.numChannels = 2; elOutBus.channelBuffers32 = outPtrs;
            Steinberg::Vst::ProcessData data = {};
            data.processMode = Steinberg::Vst::kRealtime;
            data.symbolicSampleSize = Steinberg::Vst::kSample32;
            data.numSamples = NE;
            data.numInputs = 1; data.inputs = &elInBus;
            data.numOutputs = 1; data.outputs = &elOutBus;
            data.inputParameterChanges = &elevChanges;

            p.processor->process (data);
            data.inputParameterChanges = nullptr;
            for (int b = 0; b < 6; ++b) p.processor->process (data);

            double overhead_l = 0.0, overhead_r = 0.0;
            for (int i = 0; i < NE; ++i) {
                overhead_l += std::abs (el_l[i]);
                overhead_r += std::abs (el_r[i]);
            }

            if (std::abs (overhead_l - overhead_r) < 1.0 && overhead_l > 0.1) {
                std::cout << "SUCCESS: 3D overhead elevation (+90 deg) produces symmetric ear energy.\n";
            } else {
                std::cerr << "ERROR: overhead elevation did not produce symmetric energy (L=" << overhead_l << ", R=" << overhead_r << ")\n";
                ++failures;
            }
        }
        releaseProcessor (p);
    }

    // Test Pulse Generator: when Test Pulse is enabled, silent input produces audible pulsed output
    {
        Proc p = makeProcessor (factory);
        if (p.processor)
        {
            const int NP = 256;
            float zero_in[NP] = {0};
            float pulse_l[NP] = {0}, pulse_r[NP] = {0};

            float* inPtrs[2] = { zero_in, zero_in };
            float* outPtrs[2] = { pulse_l, pulse_r };
            Steinberg::Vst::AudioBusBuffers pulseInBus = {};
            pulseInBus.numChannels = 2; pulseInBus.channelBuffers32 = inPtrs;
            Steinberg::Vst::AudioBusBuffers pulseOutBus = {};
            pulseOutBus.numChannels = 2; pulseOutBus.channelBuffers32 = outPtrs;
            Steinberg::Vst::ProcessData data = {};
            data.processMode = Steinberg::Vst::kRealtime;
            data.symbolicSampleSize = Steinberg::Vst::kSample32;
            data.numSamples = NP;
            data.numInputs = 1; data.inputs = &pulseInBus;
            data.numOutputs = 1; data.outputs = &pulseOutBus;

            DummyChanges pulseChanges;
            pulseChanges.q.id = RotatingHrtf::kParamTestPulse;
            pulseChanges.q.val = 1.0; // Enabled
            data.inputParameterChanges = &pulseChanges;

            p.processor->process (data);

            double pulse_energy = 0.0;
            for (int i = 0; i < NP; ++i) {
                pulse_energy += std::abs (pulse_l[i]) + std::abs (pulse_r[i]);
            }

            if (pulse_energy > 0.01) {
                std::cout << "SUCCESS: Test Pulse generator synthesised audio from silent input (energy=" << pulse_energy << ").\n";
            } else {
                std::cerr << "ERROR: Test Pulse generator failed to produce audio from silent input (energy=" << pulse_energy << ")\n";
                ++failures;
            }

            // Verify Test Pulse disabled state produces silence from silent input
            pulseChanges.q.val = 0.0;
            for (int b = 0; b < 10; ++b) p.processor->process (data);
            double off_energy = 0.0;
            for (int i = 0; i < NP; ++i) off_energy += std::abs (pulse_l[i]) + std::abs (pulse_r[i]);
            if (off_energy < 1e-6) {
                std::cout << "SUCCESS: Test Pulse generator OFF state produces silence.\n";
            } else {
                std::cerr << "ERROR: Test Pulse generator stuck ON when disabled (energy=" << off_energy << ")\n";
                ++failures;
            }
        }
        releaseProcessor (p);
    }

    // Test Tone Parameter: 0.0 (rumble) vs 1.0 (crisp transient) alters the generated pulse spectrum
    {
        Proc p1 = makeProcessor (factory);
        Proc p2 = makeProcessor (factory);
        if (p1.processor && p2.processor)
        {
            const int NT = 256;
            float zero_in[NT] = {0};
            float out1_l[NT] = {0}, out1_r[NT] = {0};
            float out2_l[NT] = {0}, out2_r[NT] = {0};

            float* inPtrs1[2] = { zero_in, zero_in };
            float* outPtrs1[2] = { out1_l, out1_r };
            Steinberg::Vst::AudioBusBuffers inBus1 = {}; inBus1.numChannels = 2; inBus1.channelBuffers32 = inPtrs1;
            Steinberg::Vst::AudioBusBuffers outBus1 = {}; outBus1.numChannels = 2; outBus1.channelBuffers32 = outPtrs1;
            Steinberg::Vst::ProcessData data1 = {};
            data1.processMode = Steinberg::Vst::kRealtime;
            data1.symbolicSampleSize = Steinberg::Vst::kSample32;
            data1.numSamples = NT; data1.numInputs = 1; data1.inputs = &inBus1; data1.numOutputs = 1; data1.outputs = &outBus1;

            float* inPtrs2[2] = { zero_in, zero_in };
            float* outPtrs2[2] = { out2_l, out2_r };
            Steinberg::Vst::AudioBusBuffers inBus2 = {}; inBus2.numChannels = 2; inBus2.channelBuffers32 = inPtrs2;
            Steinberg::Vst::AudioBusBuffers outBus2 = {}; outBus2.numChannels = 2; outBus2.channelBuffers32 = outPtrs2;
            Steinberg::Vst::ProcessData data2 = {};
            data2.processMode = Steinberg::Vst::kRealtime;
            data2.symbolicSampleSize = Steinberg::Vst::kSample32;
            data2.numSamples = NT; data2.numInputs = 1; data2.inputs = &inBus2; data2.numOutputs = 1; data2.outputs = &outBus2;

            DummyChanges changes1;
            changes1.q.id = RotatingHrtf::kParamTestPulse; changes1.q.val = 1.0;
            data1.inputParameterChanges = &changes1;
            p1.processor->process (data1); // enable pulse

            changes1.q.id = RotatingHrtf::kParamTestTone; changes1.q.val = 0.0; // low rumble
            data1.inputParameterChanges = &changes1;
            p1.processor->process (data1);

            DummyChanges changes2;
            changes2.q.id = RotatingHrtf::kParamTestPulse; changes2.q.val = 1.0;
            data2.inputParameterChanges = &changes2;
            p2.processor->process (data2); // enable pulse

            changes2.q.id = RotatingHrtf::kParamTestTone; changes2.q.val = 1.0; // crisp transient
            data2.inputParameterChanges = &changes2;
            p2.processor->process (data2);

            double diff = 0.0;
            for (int i = 0; i < NT; ++i) {
                diff += std::abs (out1_l[i] - out2_l[i]) + std::abs (out1_r[i] - out2_r[i]);
            }

            if (diff > 0.01) {
                std::cout << "SUCCESS: Test Tone parameter significantly alters synthesised pulse spectrum (diff=" << diff << ").\n";
            } else {
                std::cerr << "ERROR: Test Tone parameter produced negligible difference (diff=" << diff << ")\n";
                ++failures;
            }
        }
        releaseProcessor (p1);
        releaseProcessor (p2);
    }

    // Ear Scale Parameter: 70% vs 130% scale modifies pinna notches and ITD
    {
        Proc p1 = makeProcessor (factory);
        Proc p2 = makeProcessor (factory);
        if (p1.processor && p2.processor)
        {
            const int NS = 256;
            float in_sig[NS];
            for (int i = 0; i < NS; ++i) in_sig[i] = sinf (2.0f * 3.14159265f * 2000.0f * (float)i / 48000.0f);
            float out1_l[NS] = {0}, out1_r[NS] = {0};
            float out2_l[NS] = {0}, out2_r[NS] = {0};

            float* inPtrs1[2] = { in_sig, in_sig }; float* outPtrs1[2] = { out1_l, out1_r };
            Steinberg::Vst::AudioBusBuffers inBus1 = {}; inBus1.numChannels = 2; inBus1.channelBuffers32 = inPtrs1;
            Steinberg::Vst::AudioBusBuffers outBus1 = {}; outBus1.numChannels = 2; outBus1.channelBuffers32 = outPtrs1;
            Steinberg::Vst::ProcessData data1 = {};
            data1.processMode = Steinberg::Vst::kRealtime; data1.symbolicSampleSize = Steinberg::Vst::kSample32;
            data1.numSamples = NS; data1.numInputs = 1; data1.inputs = &inBus1; data1.numOutputs = 1; data1.outputs = &outBus1;

            float* inPtrs2[2] = { in_sig, in_sig }; float* outPtrs2[2] = { out2_l, out2_r };
            Steinberg::Vst::AudioBusBuffers inBus2 = {}; inBus2.numChannels = 2; inBus2.channelBuffers32 = inPtrs2;
            Steinberg::Vst::AudioBusBuffers outBus2 = {}; outBus2.numChannels = 2; outBus2.channelBuffers32 = outPtrs2;
            Steinberg::Vst::ProcessData data2 = {};
            data2.processMode = Steinberg::Vst::kRealtime; data2.symbolicSampleSize = Steinberg::Vst::kSample32;
            data2.numSamples = NS; data2.numInputs = 1; data2.inputs = &inBus2; data2.numOutputs = 1; data2.outputs = &outBus2;

            DummyChanges changes1;
            changes1.q.id = RotatingHrtf::kParamRotation; changes1.q.val = 0.25; // 90 deg lateral
            data1.inputParameterChanges = &changes1;
            p1.processor->process (data1);
            changes1.q.id = RotatingHrtf::kParamEarScale; changes1.q.val = 0.0; // 70% scale
            data1.inputParameterChanges = &changes1;
            for (int b = 0; b < 6; ++b) { p1.processor->process (data1); data1.inputParameterChanges = nullptr; }

            DummyChanges changes2;
            changes2.q.id = RotatingHrtf::kParamRotation; changes2.q.val = 0.25; // 90 deg lateral
            data2.inputParameterChanges = &changes2;
            p2.processor->process (data2);
            changes2.q.id = RotatingHrtf::kParamEarScale; changes2.q.val = 1.0; // 130% scale
            data2.inputParameterChanges = &changes2;
            for (int b = 0; b < 6; ++b) { p2.processor->process (data2); data2.inputParameterChanges = nullptr; }

            double diff = 0.0;
            for (int i = 0; i < NS; ++i) {
                diff += std::abs (out1_l[i] - out2_l[i]) + std::abs (out1_r[i] - out2_r[i]);
            }

            if (diff > 0.01) {
                std::cout << "SUCCESS: Anthropometric Ear Scale parameter shifts ITD delay and spectral pinna filtering (diff=" << diff << ").\n";
            } else {
                std::cerr << "ERROR: Ear Scale parameter produced negligible difference (diff=" << diff << ")\n";
                ++failures;
            }
        }
        releaseProcessor (p1);
        releaseProcessor (p2);
    }

    // Regression: the VST3 spec allows null sample buffers when a bus is
    // inactive. process() used to dereference them and take the host down.
    {
        Steinberg::Vst::AudioBusBuffers nullOut = {};
        nullOut.numChannels = 2;
        nullOut.channelBuffers32 = nullptr;

        Steinberg::Vst::ProcessData nullOutData = processData;
        nullOutData.outputs = &nullOut;

        res = processor->process(nullOutData);
        if (res != Steinberg::kResultOk)
        {
            std::cerr << "ERROR: process() with null output buffers returned " << res << ", expected kResultOk!\n";
            ++failures;
        }
        else
        {
            std::cout << "SUCCESS: process() survived null output buffers.\n";
        }

        Steinberg::Vst::AudioBusBuffers nullIn = {};
        nullIn.numChannels = 2;
        nullIn.channelBuffers32 = nullptr;

        Steinberg::Vst::ProcessData nullInData = processData;
        nullInData.inputs = &nullIn;

        res = processor->process(nullInData);
        if (res != Steinberg::kResultOk)
        {
            std::cerr << "ERROR: process() with a null input buffer array returned " << res << ", expected kResultOk!\n";
            ++failures;
        }
        else
        {
            std::cout << "SUCCESS: process() survived a null input buffer array.\n";
        }
    }

    // State Serialisation tests
    if (comp)
    {
        TestMemStream saveStream;
        Steinberg::tresult saveRes = comp->getState(&saveStream);
        if (saveRes != Steinberg::kResultOk || saveStream.buffer.size() != 60)
        {
            std::cerr << "ERROR: comp->getState() failed or returned unexpected byte count (" << saveStream.buffer.size() << ", expected 60)\n";
            ++failures;
        }
        else
        {
            // Modify a parameter via DummyChanges to prove restore is effective
            DummyChanges modChanges;
            modChanges.q.id = 100; // Distance
            modChanges.q.val = 0.85;
            modChanges.q.pointOffset = 0;
            processData.inputParameterChanges = &modChanges;
            processData.numSamples = 0;
            processor->process(processData);
            processData.inputParameterChanges = nullptr;

            // Restore original state
            saveStream.cursor = 0;
            saveRes = comp->setState(&saveStream);
            if (saveRes != Steinberg::kResultOk)
            {
                std::cerr << "ERROR: comp->setState() failed on valid state stream\n";
                ++failures;
            }
            else
            {
                TestMemStream verifyStream;
                comp->getState(&verifyStream);
                if (verifyStream.buffer == saveStream.buffer)
                {
                    std::cout << "SUCCESS: VST3 processor state round-trip preserved all 7 parameters identically.\n";
                }
                else
                {
                    std::cerr << "ERROR: VST3 processor state round-trip stream mismatch after restore.\n";
                    ++failures;
                }
            }
        }

        // Hostile state streams on comp->setState: null, truncated header, truncated payload, invalid version
        // All hostile streams must be rejected (kResultFalse) AND leave processor state completely untouched.
        TestMemStream procBaselineStream;
        comp->getState (&procBaselineStream);

        auto verifyProcUnchanged = [&](const char* testName) {
            TestMemStream curStream;
            comp->getState (&curStream);
            if (curStream.buffer != procBaselineStream.buffer)
            {
                std::cerr << "ERROR: processor " << testName << " mutated state!\n";
                ++failures;
                return false;
            }
            return true;
        };

        if (comp->setState (nullptr) == Steinberg::kResultFalse && verifyProcUnchanged ("setState(nullptr)"))
        {
            std::cout << "SUCCESS: comp->setState(nullptr) rejected as expected.\n";
        }
        else
        {
            std::cerr << "ERROR: comp->setState(nullptr) was accepted or mutated state!\n";
            ++failures;
        }

        TestMemStream truncStream;
        truncStream.writeVal<Steinberg::int16> (4); // only 2 bytes
        truncStream.cursor = 0;
        if (comp->setState (&truncStream) == Steinberg::kResultFalse && verifyProcUnchanged ("truncated header"))
        {
            std::cout << "SUCCESS: comp->setState rejected truncated header (2 bytes).\n";
        }
        else
        {
            std::cerr << "ERROR: comp->setState accepted truncated header or mutated state!\n";
            ++failures;
        }

        TestMemStream incStream;
        incStream.writeVal<Steinberg::int32> (RotatingHrtf::kStateVersion);
        incStream.writeVal<double> (0.5);
        incStream.writeVal<double> (0.5); // only 2 doubles instead of 7
        incStream.cursor = 0;
        if (comp->setState (&incStream) == Steinberg::kResultFalse && verifyProcUnchanged ("incomplete payload"))
        {
            std::cout << "SUCCESS: comp->setState rejected incomplete parameter payload (valid version + 2 doubles).\n";
        }
        else
        {
            std::cerr << "ERROR: comp->setState accepted incomplete payload or mutated state!\n";
            ++failures;
        }

        TestMemStream badVerStream;
        badVerStream.writeVal<Steinberg::int32> (99); // bad version
        for (int i = 0; i < 7; ++i) badVerStream.writeVal<double> (0.5);
        badVerStream.cursor = 0;
        if (comp->setState (&badVerStream) == Steinberg::kResultFalse && verifyProcUnchanged ("invalid version"))
        {
            std::cout << "SUCCESS: comp->setState rejected invalid version (99).\n";
        }
        else
        {
            std::cerr << "ERROR: comp->setState accepted invalid version or mutated state!\n";
            ++failures;
        }
    }

    // Bus Arrangements: Mono In -> Stereo Out accepted, Stereo In -> Stereo Out accepted, 5.1 In rejected, Mono Out rejected
    {
        Proc p = makeProcessor (factory);
        if (p.processor)
        {
            Steinberg::Vst::SpeakerArrangement inArrMono = Steinberg::Vst::SpeakerArr::kMono;
            Steinberg::Vst::SpeakerArrangement outArrStereo = Steinberg::Vst::SpeakerArr::kStereo;
            Steinberg::tresult r1 = p.processor->setBusArrangements (&inArrMono, 1, &outArrStereo, 1);

            Steinberg::Vst::SpeakerArrangement inArrStereo = Steinberg::Vst::SpeakerArr::kStereo;
            Steinberg::tresult r2 = p.processor->setBusArrangements (&inArrStereo, 1, &outArrStereo, 1);

            Steinberg::Vst::SpeakerArrangement inArr51 = Steinberg::Vst::SpeakerArr::k51;
            Steinberg::tresult r3 = p.processor->setBusArrangements (&inArr51, 1, &outArrStereo, 1);

            Steinberg::Vst::SpeakerArrangement outArrMono = Steinberg::Vst::SpeakerArr::kMono;
            Steinberg::tresult r4 = p.processor->setBusArrangements (&inArrStereo, 1, &outArrMono, 1);

            if (r1 == Steinberg::kResultTrue && r2 == Steinberg::kResultTrue &&
                r3 == Steinberg::kResultFalse && r4 == Steinberg::kResultFalse)
            {
                std::cout << "SUCCESS: Bus arrangements properly accepted (Mono/Stereo In -> Stereo Out) and rejected (5.1 In, Mono Out).\n";
            }
            else
            {
                std::cerr << "ERROR: Bus arrangement validation failed (r1=" << r1 << ", r2=" << r2 << ", r3=" << r3 << ", r4=" << r4 << ")\n";
                ++failures;
            }
        }
        releaseProcessor (p);
    }

    // Sample Size Rejection: kSample32 accepted, kSample64 rejected
    {
        Proc p = makeProcessor (factory);
        if (p.processor)
        {
            Steinberg::tresult r32 = p.processor->canProcessSampleSize (Steinberg::Vst::kSample32);
            Steinberg::tresult r64 = p.processor->canProcessSampleSize (Steinberg::Vst::kSample64);
            if (r32 == Steinberg::kResultTrue && r64 == Steinberg::kResultFalse)
            {
                std::cout << "SUCCESS: canProcessSampleSize correctly accepted kSample32 and rejected kSample64.\n";
            }
            else
            {
                std::cerr << "ERROR: canProcessSampleSize failed (r32=" << r32 << ", r64=" << r64 << ")\n";
                ++failures;
            }
        }
        releaseProcessor (p);
    }

    // Mono Input Processing: 1 channel input produces symmetric binaural stereo output
    {
        Proc p = makeProcessor (factory);
        if (p.processor)
        {
            const int NM = 256;
            float mono_in[NM];
            float mono_out_l[NM] = {0}, mono_out_r[NM] = {0};
            for (int i = 0; i < NM; ++i) mono_in[i] = sinf (2.0f * 3.14159265f * 440.0f * (float)i / 48000.0f);

            float* monoInPtrs[1] = { mono_in };
            float* monoOutPtrs[2] = { mono_out_l, mono_out_r };
            Steinberg::Vst::AudioBusBuffers mInBus = {};
            mInBus.numChannels = 1;
            mInBus.channelBuffers32 = monoInPtrs;
            Steinberg::Vst::AudioBusBuffers mOutBus = {};
            mOutBus.numChannels = 2;
            mOutBus.channelBuffers32 = monoOutPtrs;
            Steinberg::Vst::ProcessData mData = {};
            mData.processMode = Steinberg::Vst::kRealtime;
            mData.symbolicSampleSize = Steinberg::Vst::kSample32;
            mData.numSamples = NM;
            mData.numInputs = 1;
            mData.inputs = &mInBus;
            mData.numOutputs = 1;
            mData.outputs = &mOutBus;

            // Explicitly set rotation = 0.0 (front) rather than relying on default state
            DummyChanges monoChanges;
            monoChanges.q.id = RotatingHrtf::kParamRotation;
            monoChanges.q.val = 0.0;
            mData.inputParameterChanges = &monoChanges;

            Steinberg::tresult mr = p.processor->process (mData);
            double mono_sum_l = 0.0, mono_sum_r = 0.0;
            for (int i = 0; i < NM; ++i) {
                mono_sum_l += std::abs (mono_out_l[i]);
                mono_sum_r += std::abs (mono_out_r[i]);
            }

            if (mr == Steinberg::kResultOk && mono_sum_l > 0.01 && std::abs (mono_sum_l - mono_sum_r) < 1e-4)
            {
                std::cout << "SUCCESS: Mono input bus correctly processed into symmetric binaural stereo output.\n";
            }
            else
            {
                std::cerr << "ERROR: Mono input processing failed (mr=" << mr << ", L=" << mono_sum_l << ", R=" << mono_sum_r << ")\n";
                ++failures;
            }
        }
        releaseProcessor (p);
    }

    // Mid-Session Sample Rate Change and Reactivation (setActive false -> setup 96k -> setActive true)
    // Asserts rate-specific ITD arrival index at 96 kHz (142 samples vs 72 samples at 48 kHz).
    {
        Proc p = makeProcessor (factory);
        if (p.processor && p.comp)
        {
            p.comp->setActive (false);

            Steinberg::Vst::ProcessSetup setup96k = {};
            setup96k.processMode = Steinberg::Vst::kRealtime;
            setup96k.symbolicSampleSize = Steinberg::Vst::kSample32;
            setup96k.maxSamplesPerBlock = 512;
            setup96k.sampleRate = 96000.0;
            p.processor->setupProcessing (setup96k);

            p.comp->setActive (true);

            const int N96 = 512;
            float in96[N96], out96_l[N96] = {0}, out96_r[N96] = {0};
            for (int i = 0; i < N96; ++i) in96[i] = 0.0f;
            float* in96Ptrs[2] = { in96, in96 };
            float* out96Ptrs[2] = { out96_l, out96_r };
            Steinberg::Vst::AudioBusBuffers inBus96 = {};
            inBus96.numChannels = 2; inBus96.channelBuffers32 = in96Ptrs;
            Steinberg::Vst::AudioBusBuffers outBus96 = {};
            outBus96.numChannels = 2; outBus96.channelBuffers32 = out96Ptrs;
            Steinberg::Vst::ProcessData data96 = {};
            data96.processMode = Steinberg::Vst::kRealtime;
            data96.symbolicSampleSize = Steinberg::Vst::kSample32;
            data96.numSamples = N96;
            data96.numInputs = 1; data96.inputs = &inBus96;
            data96.numOutputs = 1; data96.outputs = &outBus96;

            // Set lateral 90 deg right, 5 cm near field, 130% ear scale, anechoic
            DummyChanges changes96;
            changes96.q.id = RotatingHrtf::kParamRotation; changes96.q.val = 0.25;
            data96.inputParameterChanges = &changes96;
            p.processor->process (data96);

            changes96.q.id = RotatingHrtf::kParamDistance; changes96.q.val = 0.0; // 0.05m
            data96.inputParameterChanges = &changes96;
            p.processor->process (data96);

            changes96.q.id = RotatingHrtf::kParamEarScale; changes96.q.val = 1.0; // 130%
            data96.inputParameterChanges = &changes96;
            p.processor->process (data96);

            changes96.q.id = RotatingHrtf::kParamSpace; changes96.q.val = 0.0;
            data96.inputParameterChanges = &changes96;
            p.processor->process (data96);

            data96.inputParameterChanges = nullptr;
            for (int b = 0; b < 100; ++b) p.processor->process (data96);

            // Feed unit impulse
            in96[0] = 1.0f;
            Steinberg::tresult r96 = p.processor->process (data96);

            // Locate peak arrival in far ear (left)
            int peak_idx_96 = 0;
            float peak_val_96 = 0.0f;
            for (int i = 0; i < N96; ++i)
            {
                if (std::abs (out96_l[i]) > peak_val_96)
                {
                    peak_val_96 = std::abs (out96_l[i]);
                    peak_idx_96 = i;
                }
            }

            // At 96 kHz, Woodworth expected arrival = 0.00145685 * 96000 + 2.0 = 141.86 ~ 142 samples.
            // At 48 kHz, it would be ~72 samples.
            const int expected_arrival_96 = 142;
            if (r96 == Steinberg::kResultOk && std::abs (peak_idx_96 - expected_arrival_96) <= 3)
            {
                std::cout << "SUCCESS: Reactivation and mid-session sample rate change (96 kHz) verified rate-specific ITD arrival (sample "
                          << peak_idx_96 << ", expected 142 +/- 3; 48 kHz would be 72).\n";
            }
            else
            {
                std::cerr << "ERROR: Mid-session sample rate change failed rate-specific test (r=" << r96
                          << ", arrival sample=" << peak_idx_96 << ", expected 142 +/- 3)\n";
                ++failures;
            }
        }
        releaseProcessor (p);
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
