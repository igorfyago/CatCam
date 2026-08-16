// ============================================================================
// FrameServer.h — Cross-session shared memory reader
// Defines the protocol for reading NV12 frames sent from the companion script.
// ============================================================================
#pragma once
#include <windows.h>

// We use the Global\ namespace so the shared memory is accessible across 
// session boundaries (from Session 0 where Frame Server runs, to the user session).
#define SHARED_MEM_NAME L"Global\\CatCam_SharedMem"
#define MUTEX_NAME      L"Global\\CatCam_Mutex"
#define CONTROL_MEM_NAME L"Global\\CatCam_Control"

// Small side channel next to the frame buffer (its own mapping so the frame
// layout and the stale-mapping rules around it are untouched). Everyone
// writes only their own fields; ticks are GetTickCount64 (system-wide).
//   consumerBeat  : this DLL, on every sample it serves = "an app is pulling
//                   frames from the virtual camera right now"
//   previewBeat   : the tray, while its preview window is open
//   tabletState   : the host, 0 = no tablet, 1 = READY (camera off), 2 = live
//   tabletOnDemand: the host, 1 = the tablet lets the PC drive its camera
//   hostBeat      : the host, alive
// The host turns demand (either beat fresh) into start/stop commands.
#pragma pack(push, 1)
struct ControlBlock
{
    UINT32  magic;          // 'CCTL'
    UINT32  version;        // 1
    UINT64  consumerBeat;
    UINT64  previewBeat;
    UINT32  tabletState;
    UINT32  tabletOnDemand;
    UINT64  hostBeat;
    // Tray -> host command mailbox + tablet tuning (host/tray only; this
    // DLL touches nothing past hostBeat).
    UINT32  cmdSeq;
    char    cmd[11];
    UINT8   tuneFlags;
    INT16   zoomX100;
    INT8    ev, tone;
    UINT8   wb, focusMode, focusPos, micLevel;
};
#pragma pack(pop)
#define CONTROL_MAGIC 0x4C544343u   // "CCTL" little-endian

// Data layout of the shared memory mapping
#pragma pack(push, 1)
struct SharedMemHeader
{
    UINT32  width;
    UINT32  height;
    UINT32  stride;
    UINT32  frameSize;
    UINT64  frameIndex; // Monotonically increasing index for each new frame
    UINT8   data[1];    // Start of the NV12 frame buffer
};
#pragma pack(pop)

class FrameServer
{
public:
    FrameServer();
    ~FrameServer();

    HRESULT Initialize();
    // Copies the newest frame into dst UNDER the mutex (the old
    // GetLatestFrame returned a pointer into shared memory and released the
    // mutex before the caller copied: torn frames while the host writes).
    // Returns S_OK with *length/*frameIndex set only when a frame NEWER than
    // the last copied one exists; E_PENDING when there is nothing new or the
    // mutex is busy.
    HRESULT CopyLatestFrame(BYTE* dst, DWORD cap, DWORD* length, UINT64* frameIndex);

    UINT32 GetWidth() const;
    UINT32 GetHeight() const;

    // Called on every served sample while the stream is active: stamps
    // consumerBeat so the host knows an app is actually watching. Cheap
    // (one store); opens the control mapping lazily and retries quietly if
    // the host has not created it yet.
    void TouchConsumer();

private:
    HANDLE _hMapFile;
    HANDLE _hMutex;
    SharedMemHeader* _header;
    UINT64 _lastIndex;
    HANDLE _hCtrlMap = nullptr;
    ControlBlock* _ctrl = nullptr;
    UINT32 _ctrlRetry = 0;
};
