// ============================================================================
// MediaStream.h — IMFMediaStream implementation
// Delivers samples (video frames) to the Media Foundation pipeline.
// ============================================================================
#pragma once
#include <windows.h>

#ifndef STATUS_WAIT_0
#define STATUS_WAIT_0 ((DWORD)0x00000000L)
#endif
#ifndef STATUS_ABANDONED_WAIT_0
#define STATUS_ABANDONED_WAIT_0 ((DWORD)0x00000080L)
#endif

#include <mfidl.h>
#include <mfapi.h>
#include <ks.h>
#include <ksproxy.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <memory>
#include <vector>
#include "FrameServer.h"

class VirtualCamMediaSource;

class VirtualCamMediaStream :
    public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
        IMFMediaStream2,
        IMFMediaStream,
        IMFMediaEventGenerator,
        IKsControl>
{
public:
    VirtualCamMediaStream();
    ~VirtualCamMediaStream();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;

    HRESULT RuntimeClassInitialize(VirtualCamMediaSource* pSource);
    void SetActive(bool active);
    void Shutdown();
    HRESULT SetCurrentMediaTypeOnHandler();
    HRESULT FireStreamStarted(const PROPVARIANT* pvarStartPosition);
    void GetAdvertisedDims(UINT32* w, UINT32* h) { if (w) *w = _mediaW; if (h) *h = _mediaH; }

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** ppMediaSource) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor) override;
    STDMETHODIMP RequestSample(IUnknown* pToken) override;

    // IMFMediaStream2 (mandatory for Frame Server custom sources)
    STDMETHODIMP SetStreamState(MF_STREAM_STATE value) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* value) override;

    // IKsControl (pipeline QIs the stream per-pin for KS property sets;
    // ERROR_SET_NOT_FOUND is the standard AVStream "not supported" code)
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override
        { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override
        { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override
        { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) override;

private:
    HRESULT CreateDefaultMediaType(IMFMediaType** ppMediaType);
    HRESULT CreateMediaType(UINT32 w, UINT32 h, IMFMediaType** ppMediaType);

    Microsoft::WRL::ComPtr<IMFMediaEventQueue> _eventQueue;
    Microsoft::WRL::ComPtr<IMFStreamDescriptor> _streamDescriptor;
    Microsoft::WRL::ComPtr<VirtualCamMediaSource> _source;
    
    HRESULT DeliverSample(IUnknown* pToken); // build + queue one sample now

    std::unique_ptr<FrameServer> _frameServer;
    bool _active;
    bool _deliveredFirst = false; // black starter until the first real frame
    UINT32 _reqCount = 0, _freshCount = 0; // per-second delivery telemetry
    LONGLONG _statStart = 0;
    // Pacing (HANDOFF lesson 32): completing a request is what triggers the
    // pipeline's next one, so instant completion free-runs the pump
    // (measured 1700 req/s, the TDR mechanism) and MEStreamTick wedges it
    // (lesson 29). Deferred completion is NOT AN OPTION in the frame-server
    // hosts: parked tokens were never completed there by an MF scheduled
    // work item OR by a dedicated Win32 thread (both measured: the callback/
    // thread simply never ran in the live hosting process; consumers black
    // after 2-3 samples). So pace synchronously, the MS VirtualCamera-sample
    // way: a request with no NEW frame blocks ~one frame time inline,
    // re-checks, and ALWAYS delivers before returning. No cross-thread
    // state exists, so a wedge is structurally impossible.
    CRITICAL_SECTION _paceLock;
    UINT32 _pacedCount = 0; // requests that took the inline pacing wait
    UINT32 _inactiveReqs = 0, _activeReqs = 0; // handshake diagnostics
    UINT64 _lastFrameIndex;
    UINT32 _mediaW = 0, _mediaH = 0;
    std::vector<BYTE> _lastFrame; // Caches the last frame to simulate freezing on disconnect
};
