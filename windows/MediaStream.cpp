// ============================================================================
// MediaStream.cpp — Implementation of the video stream
// ============================================================================
#include "MediaStream.h"
#include "MediaSource.h"
#include "Logger.h"
#include <mfapi.h>
#include <mferror.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")

VirtualCamMediaStream::VirtualCamMediaStream() : _active(false), _lastFrameIndex(0)
{
    InitializeCriticalSection(&_paceLock);
}
VirtualCamMediaStream::~VirtualCamMediaStream()
{
    DeleteCriticalSection(&_paceLock);
}

STDMETHODIMP VirtualCamMediaStream::QueryInterface(REFIID riid, void** ppv)
{
    HRESULT hr = RuntimeClass::QueryInterface(riid, ppv);
    if (FAILED(hr)) {
        char guidStr[64];
        snprintf(guidStr, sizeof(guidStr), "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
            riid.Data1, riid.Data2, riid.Data3,
            riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
            riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
        Log("Stream QI FAILED: %s", guidStr);
    }
    return hr;
}

HRESULT VirtualCamMediaStream::RuntimeClassInitialize(VirtualCamMediaSource* pSource)
{
    _source = pSource;

    HRESULT hr = MFCreateEventQueue(&_eventQueue);
    if (FAILED(hr)) return hr;

    // Initialize the shared memory frame reader FIRST so the media type can
    // follow the host's current dimensions.
    _frameServer = std::make_unique<FrameServer>();
    _frameServer->Initialize(); // Non-fatal if the host isn't running yet

    Microsoft::WRL::ComPtr<IMFMediaType> mediaType;
    hr = CreateDefaultMediaType(&mediaType);
    if (FAILED(hr)) return hr;

    IMFMediaType* typeArr[] = { mediaType.Get() };
    hr = MFCreateStreamDescriptor(0, 1, typeArr, &_streamDescriptor);
    if (FAILED(hr)) return hr;

    // Must be set to prevent MF_E_ATTRIBUTENOTFOUND
    hr = SetCurrentMediaTypeOnHandler();
    if (FAILED(hr)) return hr;

    return S_OK;
}

HRESULT VirtualCamMediaStream::CreateDefaultMediaType(IMFMediaType** ppMediaType)
{
    // Dimensions from the shared-mem header (host already locked them before
    // registering the camera). Fall back to registry, then 1280x720.
    UINT32 w = 1280, h = 720;
    if (_frameServer) {
        UINT32 hw = _frameServer->GetWidth(), hh = _frameServer->GetHeight();
        if (hw > 0 && hh > 0) { w = hw; h = hh; }
    }
    _mediaW = w; _mediaH = h;
    Log("CreateDefaultMediaType: %ux%u", w, h);
    return CreateMediaType(w, h, ppMediaType);
}

HRESULT VirtualCamMediaStream::CreateMediaType(UINT32 WIDTH, UINT32 HEIGHT, IMFMediaType** ppMediaType)
{
    const UINT32 STRIDE = WIDTH;
    // NV12 format size: Y plane + (UV plane)
    const UINT32 SAMPLE_SIZE = WIDTH * HEIGHT * 3 / 2;

    Microsoft::WRL::ComPtr<IMFMediaType> mediaType;
    HRESULT hr = MFCreateMediaType(&mediaType);
    if (FAILED(hr)) return hr;

    hr = mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, WIDTH, HEIGHT);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(mediaType.Get(), MF_MT_FRAME_RATE, 30, 1);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(mediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(hr)) return hr;
    
    hr = mediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetUINT32(MF_MT_SAMPLE_SIZE, SAMPLE_SIZE);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetUINT32(MF_MT_DEFAULT_STRIDE, STRIDE);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_Normal);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    if (FAILED(hr)) return hr;
    hr = mediaType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    if (FAILED(hr)) return hr;

    *ppMediaType = mediaType.Detach();
    return S_OK;
}

HRESULT VirtualCamMediaStream::SetCurrentMediaTypeOnHandler()
{
    Microsoft::WRL::ComPtr<IMFMediaTypeHandler> handler;
    HRESULT hr = _streamDescriptor->GetMediaTypeHandler(&handler);
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IMFMediaType> mediaType;
    hr = handler->GetMediaTypeByIndex(0, &mediaType);
    if (FAILED(hr)) return hr;

    return handler->SetCurrentMediaType(mediaType.Get());
}

void VirtualCamMediaStream::SetActive(bool active)
{
    _active = active;
}

HRESULT VirtualCamMediaStream::FireStreamStarted(const PROPVARIANT* pvarStartPosition)
{
    // Important: MEStreamStarted must be fired AFTER the source fires MENewStream
    return _eventQueue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, pvarStartPosition);
}

STDMETHODIMP VirtualCamMediaStream::GetMediaSource(IMFMediaSource** ppMediaSource)
{
    return _source.CopyTo(ppMediaSource);
}

// IMFMediaStream2 — stream running/stopped state.
STDMETHODIMP VirtualCamMediaStream::SetStreamState(MF_STREAM_STATE value)
{
    const bool wasActive = _active;
    Log("SetStreamState %d (wasActive=%d)", (int)value, (int)wasActive);
    _active = (value == MF_STREAM_STATE_RUNNING);
    // State flips must be announced on the stream event queue (TDR review:
    // silent transitions left devproxy holding half-torn capture graphs
    // across the restart-churn window).
    if (_active && !wasActive)
        _eventQueue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
    else if (!_active && wasActive)
        _eventQueue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    return S_OK;
}

STDMETHODIMP VirtualCamMediaStream::GetStreamState(MF_STREAM_STATE* value)
{
    if (!value) return E_POINTER;
    *value = _active ? MF_STREAM_STATE_RUNNING : MF_STREAM_STATE_STOPPED;
    return S_OK;
}

STDMETHODIMP VirtualCamMediaStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor)
{
    if (!ppStreamDescriptor) return E_POINTER;
    *ppStreamDescriptor = _streamDescriptor.Get();
    if (_streamDescriptor) _streamDescriptor->AddRef();
    return S_OK;
}

STDMETHODIMP VirtualCamMediaStream::RequestSample(IUnknown* pToken)
{
    if (!_active) {
        // Rejected requests were invisible before; a pipeline that gives up
        // after silent WRONGSTATE bounces looks identical to one that never
        // requested at all.
        UINT32 n = ++_inactiveReqs;
        if (n <= 5 || n % 500 == 0) Log("RequestSample while inactive (#%u)", n);
        return MF_E_MEDIA_SOURCE_WRONGSTATE;
    }
    {
        UINT32 n = ++_activeReqs;
        if (n <= 3) Log("RequestSample #%u (active)", n);
    }

    // Cache sized to the advertised media type; starts as a valid black NV12
    // frame (Y 0x10, UV 0x80) so the very first delivery is a legal,
    // fully-timestamped sample. The old code queued a sample with NO buffer,
    // no time and no duration here, which violates the Frame Server sample
    // contract outright.
    const DWORD want = _mediaW * _mediaH * 3 / 2;
    if (want == 0) return MF_E_NOT_INITIALIZED;
    if (_lastFrame.size() != want) {
        _lastFrame.assign(want, 0x80);                    // UV neutral
        memset(_lastFrame.data(), 0x10, (size_t)_mediaW * _mediaH); // Y black
        _deliveredFirst = false;
    }

    EnterCriticalSection(&_paceLock);

    DWORD  len = 0;
    UINT64 idx = 0;
    HRESULT hr = _frameServer->CopyLatestFrame(_lastFrame.data(), want, &len, &idx);
    bool fresh = (hr == S_OK && len > 0);
    if (fresh) _lastFrameIndex = idx;

    // Pacing (lesson 32): completing a request is what triggers the next
    // one, so instant completion free-runs the pump (measured 1700 req/s,
    // the TDR mechanism). Deferred completion (MF timer, own thread) never
    // ran in the frame-server hosts, wedging consumers black, so pace the
    // MS-sample way: no NEW frame means block ~one frame time inline,
    // re-check, then ALWAYS deliver before returning. The sleep happens
    // outside the lock so a second pipeline thread isn't serialized onto
    // two stacked waits. The black starter still goes out instantly.
    if (!fresh && _deliveredFirst) {
        _pacedCount++;
        // Short slices, bail on the first new frame: delivery locks to the
        // tablet's true cadence (~30 fresh/s, no duplicate GPU feed) instead
        // of the ~23/s a flat 33ms overshoot gave. Tablet stalled = deliver
        // the cached frame after ~36ms (frozen picture by design, pump alive).
        for (int tries = 0; tries < 3 && !fresh; tries++) {
            LeaveCriticalSection(&_paceLock);
            Sleep(12);
            EnterCriticalSection(&_paceLock);
            hr = _frameServer->CopyLatestFrame(_lastFrame.data(), want, &len, &idx);
            fresh = (hr == S_OK && len > 0);
            if (fresh) _lastFrameIndex = idx;
        }
    }

    // Telemetry: one line per second; this is what exposed the free-run.
    // Healthy paced consumer: req near 30, fresh+paced covering all of it.
    _reqCount++;
    if (fresh) _freshCount++;
    const LONGLONG nowQpc = MFGetSystemTime();
    if (_statStart == 0) _statStart = nowQpc;
    if (nowQpc - _statStart >= 10'000'000LL) { // 1s in 100ns units
        Log("deliver/s: req=%u fresh=%u paced=%u",
            _reqCount, _freshCount, _pacedCount);
        _reqCount = 0; _freshCount = 0; _pacedCount = 0; _statStart = nowQpc;
    }

    HRESULT out = DeliverSample(pToken);

    LeaveCriticalSection(&_paceLock);
    return out;
}

// Builds one sample from the cache and queues it. Caller holds _paceLock.
HRESULT VirtualCamMediaStream::DeliverSample(IUnknown* pToken)
{
    const DWORD want = _mediaW * _mediaH * 3 / 2;
    HRESULT hr;
    Microsoft::WRL::ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return hr;

    // 2D buffer, the recommended carrier for uncompressed NV12 into the
    // Frame Server (1D memory buffers force a repack on GPU upload).
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    hr = MFCreate2DMediaBuffer(_mediaW, _mediaH, 0x3231564E /* FOURCC 'NV12' */, FALSE, &buffer);
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IMF2DBuffer2> b2;
    hr = buffer.As(&b2);
    if (FAILED(hr)) return hr;

    BYTE* scan0 = nullptr; LONG pitch = 0; BYTE* bufStart = nullptr; DWORD bufLen = 0;
    hr = b2->Lock2DSize(MF2DBuffer_LockFlags_Write, &scan0, &pitch, &bufStart, &bufLen);
    if (FAILED(hr)) return hr;
    const BYTE* src = _lastFrame.data();
    const UINT32 w = _mediaW, h = _mediaH;
    for (UINT32 r = 0; r < h; r++)                       // Y plane
        memcpy(scan0 + (size_t)pitch * r, src + (size_t)w * r, w);
    const BYTE* srcUV = src + (size_t)w * h;
    BYTE* dstUV = scan0 + (size_t)pitch * h;             // UV follows Y
    for (UINT32 r = 0; r < h / 2; r++)
        memcpy(dstUV + (size_t)pitch * r, srcUV + (size_t)w * r, w);
    b2->Unlock2D();
    buffer->SetCurrentLength(want);

    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(MFGetSystemTime());
    sample->SetSampleDuration(333333); // ~30 fps in 100-nanosecond units

    if (pToken)
        sample->SetUnknown(MFSampleExtension_Token, pToken);

    _eventQueue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    _deliveredFirst = true;
    return S_OK;
}

void VirtualCamMediaStream::Shutdown()
{
    _eventQueue->Shutdown();
}

STDMETHODIMP VirtualCamMediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    return _eventQueue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP VirtualCamMediaStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    static LONG count = 0; // MEMediaSample goes to this queue: is it drained?
    LONG n = InterlockedIncrement(&count);
    if (n <= 3) Log("Stream BeginGetEvent #%ld", n);
    return _eventQueue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP VirtualCamMediaStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    return _eventQueue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP VirtualCamMediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    return _eventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}
