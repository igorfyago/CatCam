// ============================================================================
// CatCamHost.exe — Feeder + virtual camera registrar.
//
//   1. Connects to the tablet (TCP :9000) and reads the muxed stream
//      (0x01 config / 0x02 H.264 frame / 0x03 PCM audio).
//   2. Decodes H.264 with the Media Foundation decoder MFT -> NV12.
//   3. Publishes the latest NV12 frame into Global\CatCam_SharedMem.
//   4. Registers the "CatCam" virtual camera via MFCreateVirtualCamera.
//   5. Renders tablet PCM (16k mono s16) to the default output device via
//      WASAPI as a local monitor. --mute / -nosound disables it,
//      --dumpwav <path> tees the raw PCM into a WAV for verification.
//
// Must run elevated (MFCreateVirtualCamera requires it). Stays alive while
// the camera should exist.
// ============================================================================
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mfvirtualcamera.h>
#include <icodecapi.h>
#include <codecapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h> // PKEY_Device_FriendlyName (named sink)
#include <wrl/client.h>
#include <stdio.h>
#include <stdint.h>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfsensorgroup.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

using Microsoft::WRL::ComPtr;

// Must match DllMain.cpp
static const WCHAR* SOURCE_CLSID = L"{A164EB71-2905-4859-BC91-5C87C90F3DC7}";
// Must match FrameServer.h
#define SHARED_MEM_NAME L"Global\\CatCam_SharedMem"
#define MUTEX_NAME      L"Global\\CatCam_Mutex"
#define CONTROL_MEM_NAME L"Global\\CatCam_Control"

#pragma pack(push, 1)
struct SharedMemHeader {
    UINT32 width, height, stride, frameSize;
    UINT64 frameIndex;
    UINT8  data[1];
};
#pragma pack(pop)

static const int WIDTH = 1280, HEIGHT = 720;
static const UINT32 FRAME_SIZE = WIDTH * HEIGHT * 3 / 2; // NV12

// Must match FrameServer.h. Demand side channel: the DLL stamps
// consumerBeat while an app pulls frames, the tray stamps previewBeat while
// its preview is open, this host publishes what the tablet reported and
// turns fresh beats into start/stop commands for an on-demand tablet.
#pragma pack(push, 1)
struct ControlBlock {
    UINT32 magic, version;
    UINT64 consumerBeat, previewBeat;
    UINT32 tabletState;      // 0 none, 1 READY (camera off), 2 live
    UINT32 tabletOnDemand;   // 1 = PC may drive the camera
    UINT64 hostBeat;
    UINT8  reserved[64 - 40];
};
#pragma pack(pop)
static const UINT32 CONTROL_MAGIC = 0x4C544343u;
static ControlBlock* g_ctrl = nullptr;

static void ControlInit() {
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
    SECURITY_ATTRIBUTES sa{ sizeof(sa), &sd, FALSE };
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
        0, sizeof(ControlBlock), CONTROL_MEM_NAME);
    if (!h) { printf("[WARN] control mapping: %lu\n", GetLastError()); return; }
    g_ctrl = (ControlBlock*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ControlBlock));
    if (!g_ctrl) { printf("[WARN] control map view: %lu\n", GetLastError()); return; }
    if (g_ctrl->magic == 0) { g_ctrl->version = 1; g_ctrl->magic = CONTROL_MAGIC; }
    g_ctrl->tabletState = 0; g_ctrl->tabletOnDemand = 0;
    g_ctrl->hostBeat = GetTickCount64();
}

// The tablet's hello (0x04): [w:4][h:4][flags:1], bit0 on-demand, bit1 live.
static void NoteHello(const BYTE* p, UINT32 len) {
    if (!g_ctrl || len < 9) return;
    g_ctrl->tabletOnDemand = (p[8] & 1) ? 1 : 0;
    g_ctrl->tabletState = (p[8] & 2) ? 2 : 1;
}

static void logts();   // defined below, used by the demand thread

// ---- PC -> tablet commands. Written from the demand thread while the
// stream thread blocks in recv(); TCP is full duplex, one lock serialises
// writers. g_cmdSock is the live connection or INVALID_SOCKET.
static CRITICAL_SECTION g_cmdCs;
static volatile SOCKET g_cmdSock = INVALID_SOCKET;

static bool SendCommand(const char* cmd) {
    EnterCriticalSection(&g_cmdCs);
    SOCKET s = g_cmdSock;
    bool ok = false;
    if (s != INVALID_SOCKET) {
        BYTE hdr[5]; UINT32 n = (UINT32)strlen(cmd);
        hdr[0] = 0x10; hdr[1] = (BYTE)(n >> 24); hdr[2] = (BYTE)(n >> 16);
        hdr[3] = (BYTE)(n >> 8); hdr[4] = (BYTE)n;
        ok = send(s, (const char*)hdr, 5, 0) == 5 && send(s, cmd, (int)n, 0) == (int)n;
    }
    LeaveCriticalSection(&g_cmdCs);
    return ok;
}

// Demand -> camera. Runs for the life of the process. Rules:
//   demand      = consumerBeat or previewBeat within 3s
//   READY + demand              -> "start" (at most every 2s until live)
//   live + no demand for 5s     -> "stop"  (only for an on-demand tablet)
// Everything is idempotent on the tablet, so re-sending is harmless; the
// rate limits only keep the log and the wire quiet.
static DWORD WINAPI DemandThread(LPVOID) {
    ULONGLONG lastDemand = 0, lastStart = 0, lastStop = 0;
    bool wasDemand = false;
    for (;; Sleep(500)) {
        if (!g_ctrl) continue;
        ULONGLONG now = GetTickCount64();
        g_ctrl->hostBeat = now;
        const bool demand = (now - g_ctrl->consumerBeat < 3000) || (now - g_ctrl->previewBeat < 3000);
        if (demand) lastDemand = now;
        if (demand != wasDemand) {
            logts(); printf("[DEMAND] %s\n", demand ? "an app is pulling frames" : "no consumer");
            wasDemand = demand;
        }
        if (g_cmdSock == INVALID_SOCKET || !g_ctrl->tabletOnDemand) continue;
        if (demand && g_ctrl->tabletState == 1 && now - lastStart >= 2000) {
            logts(); printf("[DEMAND] -> tablet: start\n");
            SendCommand("start"); lastStart = now;
        } else if (!demand && g_ctrl->tabletState == 2 && now - lastDemand >= 5000
                   && now - lastStop >= 2000) {
            logts(); printf("[DEMAND] -> tablet: stop (idle 5s)\n");
            SendCommand("stop"); lastStop = now;
        }
    }
    return 0;
}

// Timestamp prefix for state-transition log lines (TDR forensics need
// host events alignable with the System event log).
static void logts() {
    SYSTEMTIME st; GetLocalTime(&st);
    printf("[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

// ---------------------------------------------------------------------------
// Shared memory writer (user session side)
// ---------------------------------------------------------------------------
struct FrameWriter {
    HANDLE hMap = nullptr, hMutex = nullptr;
    SharedMemHeader* hdr = nullptr;

    // WAIT_ABANDONED still GRANTS ownership (previous owner died holding the
    // mutex, e.g. an old host killed mid-Publish). Treating it as failure
    // left this process owning the mutex forever while skipping the write:
    // dims stayed zeroed and every DLL read burned its timeout then read
    // torn data (HANDOFF lesson 27).
    bool Lock() {
        DWORD r = WaitForSingleObject(hMutex, 100);
        return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
    }

    bool Init() {
        // NULL DACL: the Frame Server (LOCAL SERVICE, Session 0) must be able
        // to open these objects; default security would deny it.
        SECURITY_DESCRIPTOR sd;
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
        SECURITY_ATTRIBUTES sa{ sizeof(sa), &sd, FALSE };
        hMutex = CreateMutexW(&sa, FALSE, MUTEX_NAME);
        if (!hMutex) return false;
        hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
            0, sizeof(SharedMemHeader) + FRAME_SIZE, SHARED_MEM_NAME);
        if (!hMap) { printf("[ERR] CreateFileMapping: %lu\n", GetLastError()); return false; }
        const bool existed = (GetLastError() == ERROR_ALREADY_EXISTS);
        hdr = (SharedMemHeader*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0,
            sizeof(SharedMemHeader) + FRAME_SIZE);
        if (!hdr) return false;
        // Initialize the header ONLY on a brand-new mapping. On host restart
        // the mapping survives inside Frame Server; zeroing dims and
        // frameIndex here while a consumer streams advertised one geometry
        // and delivered another (identical byte count in portrait!), the
        // prime TDR suspect (HANDOFF lesson 26). frameIndex must continue,
        // never restart.
        if (!existed) {
            if (!Lock()) return false;
            hdr->width = 0; hdr->height = 0;  // unknown until tablet config arrives
            hdr->stride = 0; hdr->frameSize = FRAME_SIZE; hdr->frameIndex = 0;
            ReleaseMutex(hMutex);
        } else {
            printf("[OK] shared memory already existed (dims %ux%u, frame %llu) - preserved\n",
                hdr->width, hdr->height, (unsigned long long)hdr->frameIndex);
        }
        return true;
    }

    void Publish(const BYTE* nv12) {
        if (!Lock()) return;
        memcpy(hdr->data, nv12, FRAME_SIZE);
        hdr->frameIndex++;
        ReleaseMutex(hMutex);
    }

    // Legal black NV12 (Y 0x10, UV 0x80) for the current dims.
    void PublishBlack() {
        if (!Lock()) return;
        const size_t y = (size_t)hdr->width * hdr->height;
        if (y && y * 3 / 2 <= FRAME_SIZE) {
            memset(hdr->data, 0x10, y);
            memset(hdr->data + y, 0x80, y / 2);
            hdr->frameIndex++;
        }
        ReleaseMutex(hMutex);
    }

    void SetDimensions(UINT32 w, UINT32 h) {
        if (hdr->width == w && hdr->height == h) return;
        logts(); printf("[OK] stream dimensions %ux%u\n", w, h);
        if (!Lock()) return;
        hdr->width = w; hdr->height = h; hdr->stride = w;
        hdr->frameSize = w * h * 3 / 2;
        ReleaseMutex(hMutex);
        // Also publish to the registry: the DLL reads this at init so its
        // advertised media type always matches, regardless of probe timing.
        HKEY hk;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\CatCam", 0, nullptr,
                0, KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(hk, L"Width", 0, REG_DWORD, (const BYTE*)&w, sizeof(w));
            RegSetValueExW(hk, L"Height", 0, REG_DWORD, (const BYTE*)&h, sizeof(h));
            RegCloseKey(hk);
        }
    }
};

// ---------------------------------------------------------------------------
// H.264 decoder (Media Foundation MFT) -> NV12
// ---------------------------------------------------------------------------
struct H264Decoder {
    ComPtr<IMFTransform> mft;
    ComPtr<IMFMediaType> inType, outType;
    DWORD inStream = 0, outStream = 0;
    bool started = false;

    bool Init() {
        // Find the H.264 decoder MFT
        MFT_REGISTER_TYPE_INFO inInfo{ MFMediaType_Video, MFVideoFormat_H264 };
        IMFActivate** acts = nullptr; UINT32 count = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
            MFT_ENUM_FLAG_SYNCMFT, &inInfo, nullptr, &acts, &count);
        if (FAILED(hr) || count == 0) { printf("[ERR] No H264 decoder MFT\n"); return false; }
        hr = acts[0]->ActivateObject(IID_PPV_ARGS(&mft));
        for (UINT32 i = 0; i < count; i++) acts[i]->Release();
        CoTaskMemFree(acts);
        if (FAILED(hr)) return false;

        MFCreateMediaType(&inType);
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, WIDTH, HEIGHT);
        mft->SetInputType(0, inType.Get(), 0);

        MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, WIDTH, HEIGHT);
        hr = mft->SetOutputType(0, outType.Get(), 0);
        if (FAILED(hr)) { printf("[ERR] decoder SetOutputType NV12: 0x%08lx\n", hr); return false; }

        DWORD st = 0;
        mft->GetInputStatus(0, &st);

        // Low-latency decode: no frame reordering delay
        ComPtr<ICodecAPI> codec;
        if (SUCCEEDED(mft->QueryInterface(IID_PPV_ARGS(&codec)))) {
            VARIANT v{}; v.vt = VT_UI4; v.ulVal = 1;
            codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        }

        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        started = true;
        printf("[OK] H.264 decoder MFT ready (NV12 out)\n");
        return true;
    }

    // Feed one Access Unit; call Drain() to collect frames.
    bool Feed(const BYTE* data, int len) {
        ComPtr<IMFSample> sample; ComPtr<IMFMediaBuffer> buf;
        MFCreateSample(&sample);
        MFCreateMemoryBuffer(len, &buf);
        BYTE* p; buf->Lock(&p, nullptr, nullptr);
        memcpy(p, data, len);
        buf->Unlock(); buf->SetCurrentLength(len);
        sample->AddBuffer(buf.Get());
        HRESULT hr = mft->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) return true; // needs drain first
        if (FAILED(hr) && _feedErrs++ < 5)
            printf("[DEC] ProcessInput err 0x%08lx (len=%d head=%02x %02x %02x %02x)\n",
                hr, len, data[0], data[1], data[2], data[3]);
        return SUCCEEDED(hr);
    }
    int _feedErrs = 0, _drainInfo = 0;

    // Pull all ready output frames; returns true if at least one NV12 produced.
    bool Drain(FrameWriter& fw) {
        bool produced = false;
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER od{};
            MFT_OUTPUT_STREAM_INFO osi{};
            mft->GetOutputStreamInfo(0, &osi);
            ComPtr<IMFSample> sample; ComPtr<IMFMediaBuffer> buf;
            MFCreateSample(&sample);
            MFCreateMemoryBuffer(osi.cbSize, &buf);
            sample->AddBuffer(buf.Get());
            od.pSample = sample.Get();
            DWORD status = 0;
            HRESULT hr = mft->ProcessOutput(0, 1, &od, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                ComPtr<IMFMediaType> avail;
                for (DWORD i = 0;; i++) {
                    if (FAILED(mft->GetOutputAvailableType(0, i, &avail))) break;
                    GUID sub; avail->GetGUID(MF_MT_SUBTYPE, &sub);
                    if (sub == MFVideoFormat_NV12) { mft->SetOutputType(0, avail.Get(), 0); break; }
                }
                continue;
            }
            if (FAILED(hr)) {
                if (_drainInfo++ < 8)
                    printf("[DEC] ProcessOutput hr=0x%08lx cbSize=%u\n", hr, osi.cbSize);
                break;
            }
            BYTE* p; DWORD len;
            buf->Lock(&p, nullptr, nullptr);
            buf->GetCurrentLength(&len);
            if (_drainInfo++ < 8) printf("[DEC] got output frame len=%u\n", len);
            if (len >= FRAME_SIZE) { fw.Publish(p); produced = true; }
            buf->Unlock();
        }
        return produced;
    }
};

// ---------------------------------------------------------------------------
// Local audio monitor: tablet PCM (16k mono s16) -> default output device.
// SPSC byte ring between the network thread (producer) and a WASAPI render
// thread (consumer): the network thread only ever does two memcpys, never an
// audio API call. Ring full -> the whole chunk is dropped (counted); ring
// empty -> the engine gets silence. AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM lets
// the engine convert 16k mono to the mix format, SRC included.
// ---------------------------------------------------------------------------
struct AudioRing {
    static const UINT32 CAP = 32768; // ~1s of 16k mono s16; bounds monitor latency
    BYTE buf[CAP];
    std::atomic<UINT64> w{ 0 }, r{ 0 }; // free-running byte counters

    bool Push(const BYTE* p, UINT32 len) { // network thread only
        UINT64 wr = w.load(std::memory_order_relaxed);
        UINT64 rd = r.load(std::memory_order_acquire);
        if (CAP - (UINT32)(wr - rd) < len) return false; // full: drop whole chunk
        UINT32 off = (UINT32)(wr % CAP);
        UINT32 first = min(len, CAP - off);
        memcpy(buf + off, p, first);
        memcpy(buf, p + first, len - first);
        w.store(wr + len, std::memory_order_release);
        return true;
    }
    UINT32 Pop(BYTE* out, UINT32 len) { // render thread only
        UINT64 rd = r.load(std::memory_order_relaxed);
        UINT64 wr = w.load(std::memory_order_acquire);
        UINT32 n = (UINT32)min((UINT64)len, wr - rd);
        UINT32 off = (UINT32)(rd % CAP);
        UINT32 first = min(n, CAP - off);
        memcpy(out, buf + off, first);
        memcpy(out + first, buf, n - first);
        r.store(rd + n, std::memory_order_release);
        return n;
    }
};

// Case-insensitive wide substring (avoids a shlwapi dependency).
static bool ContainsI(const wchar_t* hay, const wchar_t* needle) {
    const size_t nl = wcslen(needle);
    for (; *hay; hay++)
        if (_wcsnicmp(hay, needle, nl) == 0) return true;
    return false;
}

// Find an ACTIVE render endpoint whose friendly name contains `name` and
// not `excl`. Not-found keeps the caller's retry loop waiting for the
// device to appear (e.g. VB-Cable installed later, or after a reboot).
// The exclude matters: VB-Cable enumerates its 2ch render endpoint as
// "Speakers (VB-Audio Virtual Cable)" on this box (not "CABLE Input") plus
// a "CABLE In 16 Ch" sibling, so match the product name, skip the 16 Ch.
// The installer (CatCamAudio.exe setup) pins the exact VB-Cable render
// endpoint ID under HKLM\SOFTWARE\CatCam\CableRenderId after renaming it
// "CatCam Mic Feed". Prefer that: it survives any rename, and it can never
// pick a different cable on a machine with several. Falls back to the
// name match (the adapter name in parentheses still says VB-Audio).
static HRESULT FindRenderPinned(IMMDeviceEnumerator* en, Microsoft::WRL::ComPtr<IMMDevice>* out) {
    wchar_t id[256] = L""; DWORD cb = sizeof(id);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\CatCam", L"CableRenderId",
            RRF_RT_REG_SZ, nullptr, id, &cb) != ERROR_SUCCESS || !id[0])
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    Microsoft::WRL::ComPtr<IMMDevice> d;
    HRESULT hr = en->GetDevice(id, &d);
    if (FAILED(hr)) return hr;
    DWORD st = 0;
    if (FAILED(d->GetState(&st)) || st != DEVICE_STATE_ACTIVE) return HRESULT_FROM_WIN32(ERROR_NOT_READY);
    *out = d;
    return S_OK;
}

static HRESULT FindRenderByName(IMMDeviceEnumerator* en, const wchar_t* name,
                                const wchar_t* excl,
                                Microsoft::WRL::ComPtr<IMMDevice>* out) {
    if (SUCCEEDED(FindRenderPinned(en, out))) return S_OK;
    Microsoft::WRL::ComPtr<IMMDeviceCollection> col;
    HRESULT hr = en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    if (FAILED(hr)) return hr;
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        Microsoft::WRL::ComPtr<IMMDevice> d;
        if (FAILED(col->Item(i, &d))) continue;
        Microsoft::WRL::ComPtr<IPropertyStore> ps;
        if (FAILED(d->OpenPropertyStore(STGM_READ, &ps))) continue;
        PROPVARIANT v; PropVariantInit(&v);
        // excl covers both spellings VB has used ("16 Ch" adapter suffix,
        // "CABLE In 16ch" endpoint desc); a substring miss here would feed
        // the tablet's mic into the 16-channel cable and Teams hears nothing.
        bool match = SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) &&
                     v.vt == VT_LPWSTR && ContainsI(v.pwszVal, name) &&
                     !(excl && (ContainsI(v.pwszVal, excl) || ContainsI(v.pwszVal, L"16ch")));
        PropVariantClear(&v);
        if (match) { *out = d; return S_OK; }
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

struct AudioMonitor {
    AudioRing ring;
    HANDLE thread = nullptr;
    std::atomic<bool> quit{ false };
    std::atomic<UINT64> chunksIn{ 0 }, chunksDropped{ 0 }, playedBytes{ 0 };
    UINT64 lastLogTick = 0, lastPlayedB = 0, lastDroppedC = 0; // render thread only
    // Sink selection: nullptr = default output (the speaker monitor);
    // a name = first active render device containing it (the VB-Cable feed).
    const wchar_t* targetName = nullptr;
    const wchar_t* targetExcl = nullptr;
    const char*    label = "monitor";

    void Feed(const BYTE* p, UINT32 len) { // network thread; wait-free
        chunksIn.fetch_add(1, std::memory_order_relaxed);
        if (!ring.Push(p, len)) chunksDropped.fetch_add(1, std::memory_order_relaxed);
    }

    static DWORD WINAPI ThreadMain(void* ctx) { ((AudioMonitor*)ctx)->Run(); return 0; }
    bool Start() {
        lastLogTick = GetTickCount64();
        thread = CreateThread(nullptr, 0, ThreadMain, this, 0, nullptr);
        return thread != nullptr;
    }

    // One line per minute; stays quiet until the first chunk ever arrives so
    // an idle host does not fill host.log before the tablet connects.
    void MaybeLog() {
        UINT64 now = GetTickCount64();
        if (now - lastLogTick < 60000) return;
        lastLogTick = now;
        if (chunksIn.load(std::memory_order_relaxed) == 0) return;
        UINT64 pb = playedBytes.load(std::memory_order_relaxed);
        UINT64 dc = chunksDropped.load(std::memory_order_relaxed);
        logts(); printf("[AUD] 60s %s: played=%llu dropped=%llu chunks\n", label,
            (unsigned long long)((pb - lastPlayedB) / 3200),
            (unsigned long long)(dc - lastDroppedC));
        lastPlayedB = pb; lastDroppedC = dc;
    }

    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool errLogged = false;
        while (!quit.load()) {
            // (Re)open the default render endpoint; re-entered on device loss
            // (default device change, unplug) after a 1s backoff.
            ComPtr<IMMDeviceEnumerator> en;
            ComPtr<IMMDevice> dev;
            ComPtr<IAudioClient> ac;
            ComPtr<IAudioRenderClient> rc;
            HANDLE ev = nullptr;
            UINT32 bufFrames = 0;
            WAVEFORMATEX wf{};
            wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 1;
            wf.nSamplesPerSec = 16000; wf.wBitsPerSample = 16;
            wf.nBlockAlign = 2; wf.nAvgBytesPerSec = 32000;
            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                CLSCTX_ALL, IID_PPV_ARGS(&en));
            if (SUCCEEDED(hr)) hr = targetName
                ? FindRenderByName(en.Get(), targetName, targetExcl, &dev)
                : en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
            if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                nullptr, (void**)ac.GetAddressOf());
            if (SUCCEEDED(hr)) hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                2000000 /*200ms: rides out the 100ms chunk cadence*/, 0, &wf, nullptr);
            if (SUCCEEDED(hr)) hr = ac->GetBufferSize(&bufFrames);
            if (SUCCEEDED(hr)) hr = ac->GetService(IID_PPV_ARGS(&rc));
            if (SUCCEEDED(hr)) {
                ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                hr = ev ? ac->SetEventHandle(ev) : E_FAIL;
            }
            if (SUCCEEDED(hr)) {
                BYTE* p; // prefill with silence so Start() plays quiet, not garbage
                if (SUCCEEDED(rc->GetBuffer(bufFrames, &p)))
                    rc->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
                hr = ac->Start();
            }
            if (FAILED(hr)) {
                if (!errLogged) {
                    logts(); printf("[AUD] %s init failed 0x%08lx (device absent?), retrying\n", label, hr);
                    errLogged = true; // one line until it recovers, not one per second
                }
                if (ev) CloseHandle(ev);
                for (int i = 0; i < 10 && !quit.load(); i++) Sleep(100);
                MaybeLog();
                continue;
            }
            errLogged = false;
            logts(); printf("[AUD] %s rendering to %ls (buf=%u frames)\n", label,
                targetName ? targetName : L"default output", bufFrames);
            while (!quit.load()) {
                MaybeLog();
                DWORD wres = WaitForSingleObject(ev, 2000);
                UINT32 pad = 0;
                // Probe padding even on timeout: a vanished device may stop
                // signalling the event, and only the call returns the error.
                if (FAILED(hr = ac->GetCurrentPadding(&pad))) break;
                if (wres != WAIT_OBJECT_0) continue;
                UINT32 want = bufFrames - pad; // frames; 2 bytes per frame (mono s16)
                if (!want) continue;
                BYTE* dst;
                if (FAILED(hr = rc->GetBuffer(want, &dst))) break;
                UINT32 got = ring.Pop(dst, want * 2);
                if (got == 0) { rc->ReleaseBuffer(want, AUDCLNT_BUFFERFLAGS_SILENT); continue; }
                if (got < want * 2) memset(dst + got, 0, want * 2 - got); // underrun tail
                rc->ReleaseBuffer(want, 0);
                playedBytes.fetch_add(got, std::memory_order_relaxed);
            }
            ac->Stop();
            CloseHandle(ev);
            if (!quit.load()) {
                logts(); printf("[AUD] render lost 0x%08lx, reopening device\n", hr);
                Sleep(1000);
            }
        }
        CoUninitialize();
    }
};

// --dumpwav tee: header sizes are re-patched after every chunk so the file is
// a valid WAV at any instant, including after taskkill /F. Debug path, runs
// on the network thread (buffered stdio, ~32KB/s).
struct WavDump {
    FILE* f = nullptr;
    UINT32 dataBytes = 0;

    bool Open(const wchar_t* path) {
        f = _wfopen(path, L"wb");
        if (!f) return false;
        static const BYTE h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E',
                                    'f','m','t',' ', 16,0,0,0,
                                    1,0, 1,0,             // PCM, mono
                                    0x80,0x3E,0,0,        // 16000 Hz
                                    0x00,0x7D,0,0,        // 32000 bytes/s
                                    2,0, 16,0,            // block align, bits
                                    'd','a','t','a', 0,0,0,0 };
        fwrite(h, 1, sizeof(h), f);
        fflush(f);
        return true;
    }
    void Write(const BYTE* p, UINT32 len) {
        if (!f) return;
        fseek(f, 0, SEEK_END);
        fwrite(p, 1, len, f);
        dataBytes += len;
        UINT32 riff = 36 + dataBytes;
        fseek(f, 4, SEEK_SET);  fwrite(&riff, 4, 1, f);
        fseek(f, 40, SEEK_SET); fwrite(&dataBytes, 4, 1, f);
        fflush(f);
    }
};

static AudioMonitor g_audioMon;      // speakers; started in wmain unless --mute
static AudioMonitor g_cableMon;      // VB-Cable sink; always on, waits for the
                                     // "CABLE Input" device to exist (Teams
                                     // then picks "CABLE Output" as the mic)
static bool         g_audioEnabled = true;
static WavDump      g_wavDump;       // no-op unless --dumpwav opened it

// ---------------------------------------------------------------------------
// Network: read [type:1][len:4][payload] packets from the tablet
// ---------------------------------------------------------------------------
static bool RecvAll(SOCKET s, BYTE* buf, int len) {
    int got = 0;
    while (got < len) {
        int n = recv(s, (char*)buf + got, len - got, 0);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

// Convert MediaCodec's raw output (already Annex-B start-code delimited on
// most devices) — feed as-is; the MFT expects Annex-B.
static void StreamLoop(SOCKET sock, H264Decoder& dec, FrameWriter& fw) {
    BYTE header[5];
    static BYTE payload[1 << 20];
    UINT64 audioBytes = 0;
    int pkts = 0;
    // A live tablet sends audio every 100ms even with the camera dark, so
    // 15s of silence is a dead peer. Without this the host blocked in recv()
    // forever on a socket the tablet had leaked (stopped streaming without
    // closing the accepted client), and never reconnected to the next session.
    DWORD rcvMs = 15000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&rcvMs, sizeof(rcvMs));
    EnterCriticalSection(&g_cmdCs); g_cmdSock = sock; LeaveCriticalSection(&g_cmdCs);
    UINT32 lastTabletState = g_ctrl ? g_ctrl->tabletState : 0;
    for (;;) {
        if (!RecvAll(sock, header, 5)) {
            // Zero packets = adb false-accept with nothing behind it (idle
            // tablet). Not a disconnect, not worth a line every retry.
            if (pkts > 0) {
                logts();
                printf(WSAGetLastError() == WSAETIMEDOUT
                    ? "[NET] tablet silent 15s, dropping connection\n"
                    : "[NET] tablet disconnected\n");
            }
            break;
        }
        UINT32 len = (header[1] << 24) | (header[2] << 16) | (header[3] << 8) | header[4];
        if (len > sizeof(payload)) { printf("[NET] oversize packet %u\n", len); break; }
        if (!RecvAll(sock, payload, len)) break;
        if (pkts++ < 12)
            printf("[NET] pkt type=%d len=%u head=%02x %02x %02x %02x %02x\n",
                header[0], len, payload[0], payload[1], payload[2], payload[3], len > 4 ? payload[4] : 0);
        switch (header[0]) {
        case 0x01: { // config: [w:4][h:4][SPS+PPS]
            if (len >= 8) {
                UINT32 w = (payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3];
                UINT32 h = (payload[4] << 24) | (payload[5] << 16) | (payload[6] << 8) | payload[7];
                fw.SetDimensions(w, h);
                dec.Feed(payload + 8, len - 8);
            }
            dec.Drain(fw);
            break;
        }
        case 0x02: // video AU
            if (dec.Feed(payload, len)) dec.Drain(fw);
            break;
        case 0x03: // audio PCM: cable feed + local monitor + optional WAV tee
            audioBytes += len;
            g_wavDump.Write(payload, len);
            g_cableMon.Feed(payload, len);
            if (g_audioEnabled) g_audioMon.Feed(payload, len);
            break;
        case 0x04: // hello/heartbeat: dims + state flags
            NoteHello(payload, len);
            if (g_ctrl && g_ctrl->tabletState != lastTabletState) {
                logts(); printf("[NET] tablet %s%s\n",
                    g_ctrl->tabletState == 2 ? "camera LIVE" : "READY, camera off",
                    g_ctrl->tabletOnDemand ? " (on demand)" : "");
                // Camera just went off: publish black so a consumer that
                // starts later does not get a frozen picture from the last
                // session while the tablet spins up.
                if (lastTabletState == 2 && g_ctrl->tabletState == 1) fw.PublishBlack();
                lastTabletState = g_ctrl->tabletState;
            }
            break;
        }
    }
    EnterCriticalSection(&g_cmdCs); g_cmdSock = INVALID_SOCKET; LeaveCriticalSection(&g_cmdCs);
    if (g_ctrl) g_ctrl->tabletState = 0;
}

// Read packets until the first config (0x01) arrives; set dims from it.
// The caller then feeds the same config to the decoder via StreamLoop's
// normal path — so we PUSH it back by processing it here too.
static bool WaitForConfig(SOCKET sock, FrameWriter& fw, H264Decoder& dec) {
    BYTE header[5];
    static BYTE payload[65536];
    for (int i = 0; i < 200; i++) { // bounded: ~200 packets max
        if (!RecvAll(sock, header, 5)) return false;
        UINT32 len = (header[1] << 24) | (header[2] << 16) | (header[3] << 8) | header[4];
        if (len > sizeof(payload)) return false;
        if (!RecvAll(sock, payload, len)) return false;
        if (header[0] == 0x03) { // audio arriving before the first config
            g_wavDump.Write(payload, len);
            g_cableMon.Feed(payload, len);
            if (g_audioEnabled) g_audioMon.Feed(payload, len);
            continue;
        }
        if (header[0] == 0x01 && len >= 8) {
            UINT32 w = (payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3];
            UINT32 h = (payload[4] << 24) | (payload[5] << 16) | (payload[6] << 8) | payload[7];
            fw.SetDimensions(w, h);
            dec.Feed(payload + 8, len - 8); // SPS/PPS so the decoder starts ready
            printf("[OK] camera locked to %ux%u\n", w, h);
            return true;
        }
        if (header[0] == 0x04 && len >= 9) {
            // Hello carries the dims before any frame exists: enough to
            // register the virtual camera, which is what lets Teams select
            // it and thereby ask for the camera to be turned on.
            UINT32 w = (payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3];
            UINT32 h = (payload[4] << 24) | (payload[5] << 16) | (payload[6] << 8) | payload[7];
            fw.SetDimensions(w, h);
            NoteHello(payload, len);
            printf("[OK] camera locked to %ux%u (tablet %s%s)\n", w, h,
                (payload[8] & 2) ? "live" : "READY, camera off",
                (payload[8] & 1) ? ", on demand" : "");
            return true;
        }
    }
    return false;
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: log file sees output live
    const char* host = "127.0.0.1"; // USB: adb forward tcp:9000
    int port = 9000;
    bool mute = false;
    const wchar_t* wavPath = nullptr;
    for (int i = 1; i < argc; i++) {
        // Flags anywhere; the first bare argument is the tablet IP (WiFi).
        if (!_wcsicmp(argv[i], L"--mute") || !_wcsicmp(argv[i], L"-nosound")) mute = true;
        else if (!_wcsicmp(argv[i], L"--dumpwav") && i + 1 < argc) wavPath = argv[++i];
        else { char h[64]; wcstombs(h, argv[i], sizeof(h)); host = _strdup(h); }
    }

    printf("[CatCam] init\n");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    g_audioEnabled = !mute;
    if (mute) printf("[AUD] local monitor disabled (--mute)\n");
    else if (!g_audioMon.Start()) {
        printf("[ERR] audio monitor thread failed to start\n");
        g_audioEnabled = false;
    }
    // The cable feed runs regardless of --mute: capture continues, only the
    // speakers are opt-in. Waits quietly until VB-Cable's device exists.
    g_cableMon.targetName = L"VB-Audio Virtual Cable";
    g_cableMon.targetExcl = L"16 Ch";
    g_cableMon.label = "cable";
    if (!g_cableMon.Start()) printf("[ERR] cable feed thread failed to start\n");
    if (wavPath) {
        if (g_wavDump.Open(wavPath)) printf("[AUD] dumping PCM to %ls\n", wavPath);
        else printf("[ERR] --dumpwav: cannot open %ls\n", wavPath);
    }

    BOOL supported = FALSE;
    MFIsVirtualCameraTypeSupported(MFVirtualCameraType_SoftwareCameraSource, &supported);
    if (!supported) { printf("[ERR] no virtual camera support\n"); return 1; }

    FrameWriter fw;
    if (!fw.Init()) return 1;
    printf("[OK] shared memory ready\n");
    InitializeCriticalSection(&g_cmdCs);
    ControlInit();

    H264Decoder dec;
    dec.Init();

    // Connect FIRST and wait for the tablet's config packet so we know the
    // real stream dimensions BEFORE the camera enumerates. A frame-server
    // camera must keep one resolution for its lifetime.
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = INVALID_SOCKET;
    // Wait for the tablet IN-PROCESS, indefinitely. Giving up here (old
    // behaviour: exit 1 after one failed config wait) handed the waiting to
    // the tray watchdog, which respawned the entire host every 3s for as
    // long as the tablet was asleep - MF decoder init, virtual camera probe
    // and a VB-Cable endpoint open on every attempt, ~1200 process launches
    // an hour, a 14MB host.log of one repeated stanza, and a tray icon
    // flapping red/yellow the whole time. Waiting is cheap, respawning is not.
    // Note: adb's forward ACCEPTS the local connect even when nothing is
    // listening on the device, so a successful connect() is NOT proof of a
    // tablet. Only the config packet is - so retry the pair, not the connect.
    for (long long attempt = 0;; attempt++) {
        // Quiet after the first few tries: this loop can now run for days.
        const bool loud = attempt < 3 || attempt % 60 == 0;
        s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET; addr.sin_port = htons(port);
        inet_pton(AF_INET, host, &addr.sin_addr);
        if (loud) { logts(); printf("[NET] connecting to %s:%d ...\n", host, port); }
        if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
            // Bound the config wait: the stream socket has no timeout, so a
            // peer that connects and then says nothing would hang us here.
            DWORD rcvMs = 10000;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&rcvMs, sizeof(rcvMs));
            if (WaitForConfig(s, fw, dec)) { // StreamLoop sets its own timeout
                logts(); printf("[NET] connected to tablet\n");
                break;
            }
            if (loud) { logts(); printf("[WAIT] no config (tablet not streaming yet)\n"); }
        }
        closesocket(s); s = INVALID_SOCKET;
        Sleep(attempt < 10 ? 2000 : 15000);
    }

    ComPtr<IMFVirtualCamera> cam;
    HRESULT hr = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_Session,
        MFVirtualCameraAccess_CurrentUser,
        L"CatCam", SOURCE_CLSID, nullptr, 0, &cam);
    if (FAILED(hr)) { printf("[ERR] MFCreateVirtualCamera 0x%08lx (run as admin, regsvr32 CatCamSource.dll)\n", hr); return 1; }
    hr = cam->Start(nullptr);
    if (FAILED(hr)) { printf("[ERR] virtual camera Start 0x%08lx\n", hr); return 1; }
    printf("[OK] 'CatCam' virtual camera is LIVE\n");
    CreateThread(nullptr, 0, DemandThread, nullptr, 0, nullptr);

    StreamLoop(s, dec, fw); // runs until disconnect
    // Reconnect loop. Same adb false-accept caveat as the initial wait, so
    // throttle both the logging and the retry rate: a tablet that sleeps
    // overnight must not fill host.log with a stanza every 2 seconds.
    for (long long attempt = 0;; attempt++) {
        const bool loud = attempt < 3 || attempt % 60 == 0;
        s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET; addr.sin_port = htons(port);
        inet_pton(AF_INET, host, &addr.sin_addr);
        if (loud) { logts(); printf("[NET] connecting to %s:%d ...\n", host, port); }
        if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
            if (loud) { logts(); printf("[NET] connected to tablet\n"); }
            ULONGLONG began = GetTickCount64();
            StreamLoop(s, dec, fw);
            // Only a session that actually ran resets the throttle. A
            // false-accept returns from StreamLoop immediately, and must
            // NOT look like a fresh start or the loop never backs off.
            if (GetTickCount64() - began >= 10000) attempt = -1;
        }
        closesocket(s);
        Sleep(attempt < 10 ? 2000 : 15000);
    }
    return 0;
}
