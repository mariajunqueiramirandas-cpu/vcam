// End-to-end test of the media source: loads vcamsource.dll in-process, drives
// it the way Windows Frame Server does (activate, allocator, start, request
// samples, format switch, shutdown) and checks the frames against what a real
// producer, using the SDK, sends.
//
//     source_test [path\to\vcamsource.dll]
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common.h"
#include "vcam.h"

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("  FAILED line %d: %s: ", __LINE__, #cond);                 \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

#define REQUIRE(cond, ...)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FATAL line %d: %s: ", __LINE__, #cond);                  \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

constexpr int kCamera = 4;  // test on the last slot, away from cameras people use

// Stands in for the device information Frame Server sets on the activate object.
const GUID kTestAttribute = {0x6f1d7a52, 0x0c2e, 0x4b8e, {0x9b, 0x41, 0x2a, 0x5c, 0x77, 0x13, 0xd0, 0x8e}};

// Waits for the next event of `type` (skipping others) without blocking forever.
HRESULT WaitEvent(IMFMediaEventGenerator* gen, MediaEventType type, PROPVARIANT* value, DWORD timeout_ms = 3000)
{
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        ComPtr<IMFMediaEvent> ev;
        const HRESULT hr = gen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            Sleep(1);
            continue;
        }
        if (FAILED(hr)) {
            return hr;
        }
        MediaEventType got = MEUnknown;
        ev->GetType(&got);
        HRESULT status = S_OK;
        ev->GetStatus(&status);
        if (got == type) {
            if (value) {
                ev->GetValue(value);
            }
            return status;
        }
    }
    return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
}

struct Pixel {
    int y = -1, u = -1, v = -1;
};

// Reads one pixel from a sample in the stream's current format.
Pixel ReadPixel(IMFSample* sample, const GUID& subtype, UINT32 width, UINT32 height, UINT32 x, UINT32 y)
{
    Pixel p;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer))) {
        return p;
    }
    ComPtr<IMF2DBuffer> buffer2d;
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    BYTE* data = nullptr;
    DWORD max_length = 0, length = 0;
    const bool is2d = SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch));
    if (!is2d) {
        if (FAILED(buffer->Lock(&data, &max_length, &length))) {
            return p;
        }
        scan0 = data;
        pitch = static_cast<LONG>(subtype == MFVideoFormat_NV12 ? width : width * (subtype == MFVideoFormat_YUY2 ? 2 : 4));
    }
    if (subtype == MFVideoFormat_NV12) {
        p.y = scan0[static_cast<ptrdiff_t>(y) * pitch + x];
        const BYTE* uv = scan0 + static_cast<ptrdiff_t>(pitch) * height + static_cast<ptrdiff_t>(y / 2) * pitch;
        p.u = uv[(x / 2) * 2];
        p.v = uv[(x / 2) * 2 + 1];
    } else if (subtype == MFVideoFormat_YUY2) {
        const BYTE* row = scan0 + static_cast<ptrdiff_t>(y) * pitch + (x / 2) * 4;
        p.y = row[(x & 1) ? 2 : 0];
        p.u = row[1];
        p.v = row[3];
    } else {  // RGB32: report B, G, R
        const BYTE* px = scan0 + static_cast<ptrdiff_t>(y) * pitch + x * 4;
        p.y = px[2];
        p.u = px[1];
        p.v = px[0];
    }
    if (is2d) {
        buffer2d->Unlock2D();
    } else {
        buffer->Unlock();
    }
    return p;
}

class Session {
public:
    explicit Session(IMFMediaSource* source) : source_(source) {}

    // Selects the first media type matching subtype/size/fps and starts streaming.
    void Start(const GUID& subtype, UINT32 width, UINT32 height, UINT32 fps)
    {
        subtype_ = subtype;
        width_ = width;
        height_ = height;
        ComPtr<IMFPresentationDescriptor> pd;
        REQUIRE(SUCCEEDED(source_->CreatePresentationDescriptor(&pd)), "CreatePresentationDescriptor");
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        REQUIRE(SUCCEEDED(pd->GetStreamDescriptorByIndex(0, &selected, &sd)), "GetStreamDescriptorByIndex");
        ComPtr<IMFMediaTypeHandler> handler;
        REQUIRE(SUCCEEDED(sd->GetMediaTypeHandler(&handler)), "GetMediaTypeHandler");
        DWORD count = 0;
        handler->GetMediaTypeCount(&count);
        ComPtr<IMFMediaType> chosen;
        for (DWORD i = 0; i < count && !chosen; i++) {
            ComPtr<IMFMediaType> t;
            handler->GetMediaTypeByIndex(i, &t);
            GUID s = GUID_NULL;
            UINT32 w = 0, h = 0, num = 0, den = 0;
            t->GetGUID(MF_MT_SUBTYPE, &s);
            MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
            MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
            if (s == subtype && w == width && h == height && den && num / den == fps) {
                chosen = t;
            }
        }
        REQUIRE(chosen, "no %ux%u@%u media type among %lu", width, height, fps, count);
        REQUIRE(SUCCEEDED(handler->SetCurrentMediaType(chosen.Get())), "SetCurrentMediaType");
        pd->SelectStream(0);

        PROPVARIANT start;
        PropVariantInit(&start);
        REQUIRE(SUCCEEDED(source_->Start(pd.Get(), nullptr, &start)), "Start");
        PROPVARIANT value;
        PropVariantInit(&value);
        HRESULT hr = WaitEvent(source_.Get(), MENewStream, &value);
        REQUIRE(SUCCEEDED(hr) && value.vt == VT_UNKNOWN, "MENewStream (0x%08lX)", static_cast<unsigned long>(hr));
        stream_.Reset();
        REQUIRE(SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&stream_))), "stream interface");
        PropVariantClear(&value);
        REQUIRE(SUCCEEDED(WaitEvent(source_.Get(), MESourceStarted, nullptr)), "MESourceStarted");
        REQUIRE(SUCCEEDED(WaitEvent(stream_.Get(), MEStreamStarted, nullptr)), "MEStreamStarted");
    }

    void Stop()
    {
        CHECK(SUCCEEDED(source_->Stop()), "Stop");
        CHECK(SUCCEEDED(WaitEvent(stream_.Get(), MEStreamStopped, nullptr)), "MEStreamStopped");
        CHECK(SUCCEEDED(WaitEvent(source_.Get(), MESourceStopped, nullptr)), "MESourceStopped");
    }

    ComPtr<IMFSample> Next(DWORD timeout_ms = 2000)
    {
        HRESULT hr = stream_->RequestSample(nullptr);
        if (FAILED(hr)) {
            printf("  RequestSample failed 0x%08lX\n", static_cast<unsigned long>(hr));
            return nullptr;
        }
        PROPVARIANT value;
        PropVariantInit(&value);
        hr = WaitEvent(stream_.Get(), MEMediaSample, &value, timeout_ms);
        ComPtr<IMFSample> sample;
        if (SUCCEEDED(hr) && value.vt == VT_UNKNOWN) {
            value.punkVal->QueryInterface(IID_PPV_ARGS(&sample));
        }
        PropVariantClear(&value);
        return sample;
    }

    Pixel At(IMFSample* sample, UINT32 x, UINT32 y) const { return ReadPixel(sample, subtype_, width_, height_, x, y); }
    IMFMediaStream* stream() const { return stream_.Get(); }

private:
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFMediaStream> stream_;
    GUID subtype_ = GUID_NULL;
    UINT32 width_ = 0, height_ = 0;
};

bool Near(int a, int b, int tolerance = 3)
{
    return std::abs(a - b) <= tolerance;
}

void SendSolid(vcam_producer* producer, int width, int height, uint8_t r, uint8_t g, uint8_t b)
{
    std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < frame.size(); i += 4) {
        frame[i] = b, frame[i + 1] = g, frame[i + 2] = r, frame[i + 3] = 255;
    }
    const int rc = vcam_send_packed(producer, VCAM_FORMAT_BGRA, width, height, frame.data(), 0);
    CHECK(rc == VCAM_OK, "vcam_send_packed: %s", vcam_error_string(rc));
}

}  // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string dll_path = argc > 1 ? argv[1] : "vcamsource.dll";
    REQUIRE(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "CoInitializeEx");
    REQUIRE(SUCCEEDED(MFStartup(MF_VERSION)), "MFStartup");

    HMODULE dll = LoadLibraryA(dll_path.c_str());
    REQUIRE(dll, "LoadLibrary(%s) failed: %lu", dll_path.c_str(), GetLastError());
    using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    using CanUnloadFn = HRESULT(STDAPICALLTYPE*)();
    auto get_class_object =
        reinterpret_cast<GetClassObjectFn>(reinterpret_cast<void*>(GetProcAddress(dll, "DllGetClassObject")));
    auto can_unload = reinterpret_cast<CanUnloadFn>(reinterpret_cast<void*>(GetProcAddress(dll, "DllCanUnloadNow")));
    REQUIRE(get_class_object && can_unload, "exports missing");

    {
        printf("[activation]\n");
        ComPtr<IClassFactory> factory;
        const CLSID clsid = vcam::win::SourceClsid(kCamera - 1);
        REQUIRE(SUCCEEDED(get_class_object(clsid, IID_PPV_ARGS(&factory))), "DllGetClassObject");
        ComPtr<IClassFactory> none;
        CHECK(get_class_object(GUID_NULL, IID_PPV_ARGS(&none)) == CLASS_E_CLASSNOTAVAILABLE, "unknown CLSID refused");
        ComPtr<IMFActivate> activate;
        REQUIRE(SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&activate))), "CreateInstance");
        activate->SetUINT32(kTestAttribute, 1);
        ComPtr<IMFMediaSource> source;
        REQUIRE(SUCCEEDED(activate->ActivateObject(IID_PPV_ARGS(&source))), "ActivateObject");
        ComPtr<IMFMediaSource> again;
        CHECK(SUCCEEDED(activate->ActivateObject(IID_PPV_ARGS(&again))) && again.Get() == source.Get(),
              "ActivateObject returns the same source");

        ComPtr<IMFMediaSourceEx> source_ex;
        ComPtr<IMFGetService> service;
        ComPtr<IMFSampleAllocatorControl> allocator_control;
        CHECK(SUCCEEDED(source.As(&source_ex)), "IMFMediaSourceEx");
        CHECK(SUCCEEDED(source.As(&service)), "IMFGetService");
        CHECK(SUCCEEDED(source.As(&allocator_control)), "IMFSampleAllocatorControl");
        ComPtr<IUnknown> ks;
        CHECK(SUCCEEDED(source->QueryInterface(__uuidof(IUnknown), &ks)), "IUnknown");
        DWORD characteristics = 0;
        CHECK(SUCCEEDED(source->GetCharacteristics(&characteristics)) && (characteristics & MFMEDIASOURCE_IS_LIVE),
              "live source");
        ComPtr<IMFAttributes> attributes;
        UINT32 copied = 0;
        CHECK(SUCCEEDED(source_ex->GetSourceAttributes(&attributes)) &&
                  SUCCEEDED(attributes->GetUINT32(kTestAttribute, &copied)) && copied == 1,
              "activate attributes copied to the source");
        ComPtr<IMFAttributes> stream_attributes;
        GUID category = GUID_NULL;
        CHECK(SUCCEEDED(source_ex->GetStreamAttributes(0, &stream_attributes)) &&
                  SUCCEEDED(stream_attributes->GetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, &category)),
              "stream category set");
        CHECK(source->Pause() == MF_E_INVALID_STATE_TRANSITION, "Pause refused");
        DWORD input_id = 99;
        MFSampleAllocatorUsage usage;
        CHECK(SUCCEEDED(allocator_control->GetAllocatorUsage(0, &input_id, &usage)) &&
                  usage == MFSampleAllocatorUsage_UsesProvidedAllocator && input_id == 0,
              "allocator usage");

        // Frame Server provides the sample allocator.
        ComPtr<IMFVideoSampleAllocatorEx> allocator;
        REQUIRE(SUCCEEDED(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator))), "MFCreateVideoSampleAllocatorEx");
        CHECK(SUCCEEDED(allocator_control->SetDefaultAllocator(0, allocator.Get())), "SetDefaultAllocator");

        Session session(source.Get());

        printf("[placeholder without a producer]\n");
        session.Start(MFVideoFormat_NV12, 640, 480, 30);
        ComPtr<IMFSample> sample = session.Next();
        REQUIRE(sample, "no sample");
        LONGLONG time = 0, duration = 0;
        CHECK(SUCCEEDED(sample->GetSampleTime(&time)) && SUCCEEDED(sample->GetSampleDuration(&duration)) &&
                  duration == 333333,
              "timestamps (duration %lld)", static_cast<long long>(duration));
        CHECK(std::llabs(time - static_cast<LONGLONG>(MFGetSystemTime())) < 10'000'000, "sample time is QPC based");
        Pixel corner = session.At(sample.Get(), 2, 2);
        CHECK(corner.y >= 25 && corner.y <= 60, "placeholder background, Y=%d", corner.y);

        printf("[producer frames]\n");
        vcam_options options = VCAM_OPTIONS_INIT;
        options.camera = kCamera;
        vcam_producer* producer = nullptr;
        int rc = vcam_open(&options, &producer);
        REQUIRE(rc == VCAM_OK, "vcam_open: %s", vcam_error_string(rc));
        vcam_producer* second = nullptr;
        rc = vcam_open(&options, &second);
        CHECK(rc == VCAM_ERROR_BUSY, "second producer refused (%d)", rc);

        // The producer learns what the camera streams.
        vcam_status status{};
        status.struct_size = sizeof(status);
        Sleep(300);
        vcam_get_status(producer, &status);
        CHECK(status.connected && status.consumer_active && status.width == 640 && status.height == 480 &&
                  status.format == VCAM_FORMAT_NV12 && status.fps_numerator == 30,
              "status %d %d %dx%d fmt %d fps %d", status.connected, status.consumer_active, status.width,
              status.height, status.format, status.fps_numerator);

        // Red, 16:9 into a 4:3 stream: letterboxed.
        SendSolid(producer, 1280, 720, 255, 0, 0);
        Sleep(50);
        sample = session.Next();
        REQUIRE(sample, "no sample");
        Pixel centre = session.At(sample.Get(), 320, 240);
        Pixel top = session.At(sample.Get(), 320, 10);
        CHECK(Near(centre.y, 81) && Near(centre.u, 90) && Near(centre.v, 240), "red centre %d %d %d", centre.y,
              centre.u, centre.v);
        CHECK(Near(top.y, 16) && Near(top.u, 128) && Near(top.v, 128), "black bar %d %d %d", top.y, top.u, top.v);

        printf("[pacing]\n");
        // A producer at the camera's rate, and the camera's delivery cadence.
        LONGLONG first = 0, last = 0;
        const ULONGLONG wall_start = GetTickCount64();
        int fresh_green = 0;
        for (int i = 0; i < 30; i++) {
            SendSolid(producer, 640, 480, 0, static_cast<uint8_t>(100 + i * 5), 0);
            ComPtr<IMFSample> s = session.Next();
            REQUIRE(s, "no sample %d", i);
            s->GetSampleTime(i == 0 ? &first : &last);
            if (session.At(s.Get(), 100, 100).y > 40) {
                fresh_green++;
            }
        }
        const double seconds = (last - first) / 1e7;
        const ULONGLONG wall = GetTickCount64() - wall_start;
        CHECK(seconds > 0.85 && seconds < 1.3, "29 intervals took %.3f s", seconds);
        CHECK(wall < 2000, "30 frames took %llu ms of wall time", static_cast<unsigned long long>(wall));
        CHECK(fresh_green == 30, "%d of 30 samples showed the producer's frame", fresh_green);
        vcam_get_status(producer, &status);
        CHECK(status.frames_delivered >= 25, "frames_delivered %llu", static_cast<unsigned long long>(status.frames_delivered));

        printf("[format switch re-renders the last frame]\n");
        SendSolid(producer, 1280, 720, 255, 0, 0);  // the last frame the producer sends
        Sleep(50);
        session.Stop();
        session.Start(MFVideoFormat_YUY2, 1280, 720, 30);
        Sleep(500);  // the producer notices the new request and converts its last frame again
        sample = session.Next();
        REQUIRE(sample, "no sample");
        Pixel yuy2_corner = session.At(sample.Get(), 4, 4);
        CHECK(Near(yuy2_corner.y, 81) && Near(yuy2_corner.u, 90) && Near(yuy2_corner.v, 240),
              "red edge to edge, not boxed twice: %d %d %d", yuy2_corner.y, yuy2_corner.u, yuy2_corner.v);

        printf("[RGB32 and mirroring]\n");
        vcam_close(producer);
        options.mirror = 1;
        rc = vcam_open(&options, &producer);
        REQUIRE(rc == VCAM_OK, "vcam_open (mirror): %s", vcam_error_string(rc));
        session.Stop();
        session.Start(MFVideoFormat_RGB32, 640, 360, 30);
        Sleep(300);
        {
            // Left half blue, right half yellow; mirrored it must come out the other way round.
            std::vector<uint8_t> frame(640 * 360 * 4);
            for (int y = 0; y < 360; y++) {
                for (int x = 0; x < 640; x++) {
                    uint8_t* px = &frame[(static_cast<size_t>(y) * 640 + x) * 4];
                    const bool left = x < 320;
                    px[0] = left ? 255 : 0, px[1] = left ? 0 : 255, px[2] = left ? 0 : 255, px[3] = 255;
                }
            }
            vcam_send_packed(producer, VCAM_FORMAT_BGRA, 640, 360, frame.data(), 0);
        }
        Sleep(50);
        sample = session.Next();
        REQUIRE(sample, "no sample");
        Pixel left = session.At(sample.Get(), 10, 180);    // R, G, B
        Pixel right = session.At(sample.Get(), 630, 180);
        CHECK(left.y == 255 && left.u == 255 && left.v == 0, "left is yellow: %d %d %d", left.y, left.u, left.v);
        CHECK(right.y == 0 && right.u == 0 && right.v == 255, "right is blue: %d %d %d", right.y, right.u, right.v);

        printf("[producer gone -> placeholder]\n");
        vcam_close(producer);
        session.Stop();
        session.Start(MFVideoFormat_NV12, 640, 480, 30);
        sample = session.Next();
        REQUIRE(sample, "no sample");
        corner = session.At(sample.Get(), 2, 2);
        CHECK(corner.y >= 25 && corner.y <= 60, "placeholder again, Y=%d", corner.y);

        printf("[shutdown]\n");
        IMFMediaStream* stream = session.stream();
        stream->AddRef();
        CHECK(SUCCEEDED(source->Shutdown()), "Shutdown");
        CHECK(source->Shutdown() == MF_E_SHUTDOWN, "second Shutdown");
        CHECK(stream->RequestSample(nullptr) == MF_E_SHUTDOWN, "stream refuses requests after shutdown");
        ComPtr<IMFPresentationDescriptor> pd;
        CHECK(source->CreatePresentationDescriptor(&pd) == MF_E_SHUTDOWN, "source refuses calls after shutdown");
        stream->Release();
        activate->DetachObject();
    }
    CHECK(can_unload() == S_OK, "DllCanUnloadNow after everything is released");

    MFShutdown();
    CoUninitialize();
    printf(g_failures ? "%d FAILURES\n" : "all source tests passed\n", g_failures);
    return g_failures ? 1 : 0;
}
