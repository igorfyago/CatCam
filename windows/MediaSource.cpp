// ============================================================================
// MediaSource.cpp — Implementation of IMFMediaSource
// ============================================================================
#include "MediaSource.h"
#include "MediaStream.h"
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>

VirtualCamMediaSource::VirtualCamMediaSource() : _state(MFMEDIASOURCE_STOPPED) {}
VirtualCamMediaSource::~VirtualCamMediaSource() {}

HRESULT VirtualCamMediaSource::RuntimeClassInitialize()
{
    Log("VirtualCamMediaSource::RuntimeClassInitialize");

    HRESULT hr = MFCreateEventQueue(&_eventQueue);
    if (FAILED(hr)) { Log("MFCreateEventQueue failed: 0x%08X", hr); return hr; }

    hr = MFCreateAttributes(&_sourceAttributes, 8);
    if (FAILED(hr)) return hr;

    // We are a software source, disable D3D integrations
    _sourceAttributes->SetUINT32(MF_SA_D3D_AWARE, FALSE);
    _sourceAttributes->SetUINT32(MF_SA_D3D11_AWARE, FALSE);

    _stream = Microsoft::WRL::Make<VirtualCamMediaStream>();
    if (!_stream) return E_OUTOFMEMORY;
    
    hr = _stream->RuntimeClassInitialize(this);
    if (FAILED(hr)) { Log("Stream initialization failed: 0x%08X", hr); return hr; }

    return S_OK;
}

STDMETHODIMP VirtualCamMediaSource::GetSourceAttributes(IMFAttributes** ppAttributes)
{
    if (!ppAttributes) return E_POINTER;
    _sourceAttributes.CopyTo(ppAttributes);
    return S_OK;
}

STDMETHODIMP VirtualCamMediaSource::GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes)
{
    if (!ppAttributes) return E_POINTER;

    Microsoft::WRL::ComPtr<IMFAttributes> streamAttrs;
    HRESULT hr = MFCreateAttributes(&streamAttrs, 8);
    if (FAILED(hr)) return hr;

    // PINNAME_VIDEO_CAPTURE dictates this is a camera capture stream
    static const GUID PINNAME_VIDEO_CAPTURE_GUID =
        { 0xFB6C4281, 0x0353, 0x11d1, { 0x90, 0x5F, 0x00, 0x00, 0xC0, 0xCC, 0x16, 0xBA } };

    streamAttrs->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE_GUID);
    streamAttrs->SetUINT32(MF_DEVICESTREAM_STREAM_ID, dwStreamIdentifier);
    streamAttrs->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    streamAttrs->SetUINT32(MF_SA_D3D_AWARE, FALSE);
    streamAttrs->SetUINT32(MF_SA_D3D11_AWARE, FALSE);

    *ppAttributes = streamAttrs.Detach();
    return S_OK;
}

STDMETHODIMP VirtualCamMediaSource::SetD3DManager(IUnknown* /*pManager*/)
{
    return S_OK;
}

STDMETHODIMP VirtualCamMediaSource::GetCharacteristics(DWORD* characteristics)
{
    if (!characteristics) return E_POINTER;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP VirtualCamMediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppPD)
{
    if (!ppPD) return E_POINTER;

    Microsoft::WRL::ComPtr<IMFStreamDescriptor> sd;
    HRESULT hr = _stream->GetStreamDescriptor(&sd);
    if (FAILED(hr)) return hr;

    IMFStreamDescriptor* sdArr[] = { sd.Get() };
    Microsoft::WRL::ComPtr<IMFPresentationDescriptor> pd;
    
    hr = MFCreatePresentationDescriptor(1, sdArr, &pd);
    if (FAILED(hr)) return hr;

    pd->SelectStream(0);
    *ppPD = pd.Detach();
    
    return S_OK;
}

STDMETHODIMP VirtualCamMediaSource::Start(IMFPresentationDescriptor* pPD, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition)
{
    Log("VirtualCamMediaSource::Start called");
    _state = MFMEDIASOURCE_RUNNING;
    _stream->SetActive(true);

    // Provide the stream to the media pipeline
    Microsoft::WRL::ComPtr<IUnknown> streamUnk;
    _stream.As(&streamUnk);
    
    HRESULT hr = _eventQueue->QueueEventParamUnk(MENewStream, GUID_NULL, S_OK, streamUnk.Get());
    Log("Start: MENewStream hr=0x%08lX", hr);
    if (FAILED(hr)) return hr;

    hr = _eventQueue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, pvarStartPosition);
    Log("Start: MESourceStarted hr=0x%08lX", hr);
    if (FAILED(hr)) return hr;

    hr = _stream->FireStreamStarted(pvarStartPosition);
    Log("Start: MEStreamStarted hr=0x%08lX", hr);
    return hr;
}

STDMETHODIMP VirtualCamMediaSource::Stop()
{
    Log("VirtualCamMediaSource::Stop called");
    _state = MFMEDIASOURCE_STOPPED;
    _stream->SetActive(false);
    // Announce the stop at BOTH levels; a stream left silently inactive
    // keeps the pipeline's capture graph half-alive (TDR review).
    _stream->QueueEvent(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    _eventQueue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
    return S_OK;
}

STDMETHODIMP VirtualCamMediaSource::Pause()
{
    // Frame Server sources do not support Pause.
    return MF_E_INVALID_STATE_TRANSITION;
}

STDMETHODIMP VirtualCamMediaSource::Shutdown()
{
    Log("VirtualCamMediaSource::Shutdown called");
    if (_eventQueue) _eventQueue->Shutdown();
    if (_stream)     _stream->Shutdown();
    return S_OK;
}

STDMETHODIMP VirtualCamMediaSource::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    return _eventQueue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP VirtualCamMediaSource::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    static LONG count = 0; // is anyone actually subscribed to our events?
    LONG n = InterlockedIncrement(&count);
    if (n <= 3) Log("Source BeginGetEvent #%ld", n);
    return _eventQueue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP VirtualCamMediaSource::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    return _eventQueue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP VirtualCamMediaSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    return _eventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

STDMETHODIMP VirtualCamMediaSource::GetService(REFGUID guidService, REFIID riid, LPVOID* ppvObject)
{
    // Frame Server contract: IMFGetService is mandatory; when the source
    // exposes no services it must return MF_E_UNSUPPORTED_SERVICE (not E_NOINTERFACE).
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

STDMETHODIMP VirtualCamMediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
STDMETHODIMP VirtualCamMediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*)     { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
STDMETHODIMP VirtualCamMediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*)       { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }

// IMFRealTimeClientEx — we have no dedicated worker threads; the no-op
// implementation is what the pipeline accepts for simple software sources.
STDMETHODIMP VirtualCamMediaSource::RegisterThreadsEx(DWORD* pdwTaskIndex, LPCWSTR /*wszClassName*/, LONG /*lBasePriority*/)
{
    if (!pdwTaskIndex) return E_POINTER;
    Log("RegisterThreadsEx");
    return S_OK;
}

STDMETHODIMP VirtualCamMediaSource::UnregisterThreads()
{
    Log("UnregisterThreads");
    return S_OK;
}

STDMETHODIMP VirtualCamMediaSource::SetWorkQueueEx(DWORD /*dwMultithreadedWorkQueueId*/, LONG /*lWorkItemBasePriority*/)
{
    Log("SetWorkQueueEx");
    return S_OK;
}

// IMFMediaSource2 — single fixed 720p NV12 type; accept only our own type.
STDMETHODIMP VirtualCamMediaSource::SetMediaType(DWORD dwStreamID, IMFMediaType* pMediaType)
{
    if (dwStreamID != 0 || !pMediaType) return MF_E_INVALIDSTREAMNUMBER;
    GUID sub{};
    pMediaType->GetGUID(MF_MT_SUBTYPE, &sub);
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(pMediaType, MF_MT_FRAME_SIZE, &w, &h);
    // Accept the dims our stream currently advertises (follows the host's
    // locked tablet dims), not a hardcoded 1280x720 — a portrait stream
    // advertises 720x1280 and Frame Server validates against that.
    UINT32 aw = 1280, ah = 720;
    if (_stream) { _stream->GetAdvertisedDims(&aw, &ah); }
    HRESULT hr = (sub == MFVideoFormat_NV12 && w == aw && h == ah) ? S_OK : MF_E_INVALIDMEDIATYPE;
    Log("SetMediaType stream=%lu asked=%ux%u advertised=%ux%u -> 0x%08X", dwStreamID, w, h, aw, ah, hr);
    return hr;
}

STDMETHODIMP VirtualCamMediaSource::QueryInterface(REFIID riid, void** ppv)
{
    HRESULT hr = RuntimeClass::QueryInterface(riid, ppv);
    if (FAILED(hr)) {
        char guidStr[64];
        snprintf(guidStr, sizeof(guidStr), "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
            riid.Data1, riid.Data2, riid.Data3,
            riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
            riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
        Log("Source QI FAILED: %s", guidStr);
    }
    return hr;
}
