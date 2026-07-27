// ============================================================================
// FrameServer.cpp — Implementation of the shared memory reader
// ============================================================================
#include "FrameServer.h"

// 24 bytes header + NV12 frame size (1280x720)
static const DWORD TOTAL_MEM_SIZE = 24 + 1280 * 720 * 3 / 2;

FrameServer::FrameServer()
    : _hMapFile(nullptr)
    , _hMutex(nullptr)
    , _header(nullptr)
    , _lastIndex(0)
{
}

FrameServer::~FrameServer()
{
    if (_header)
        UnmapViewOfFile(_header);
    if (_hMapFile)
        CloseHandle(_hMapFile);
    if (_hMutex)
        CloseHandle(_hMutex);
}

HRESULT FrameServer::Initialize()
{
    // Create a security descriptor with a NULL DACL to allow full access.
    // This is necessary because this DLL runs in Session 0 (LOCAL SERVICE), 
    // and the companion script runs in the interactive user session.
    SECURITY_DESCRIPTOR sd;
    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION))
        return HRESULT_FROM_WIN32(GetLastError());

    if (!SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE))
        return HRESULT_FROM_WIN32(GetLastError());

    SECURITY_ATTRIBUTES sa;
    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle       = FALSE;

    // Create the global file mapping object
    _hMapFile = CreateFileMappingW(
        INVALID_HANDLE_VALUE,   // Backed by the system paging file
        &sa,
        PAGE_READWRITE,
        0,
        TOTAL_MEM_SIZE,
        SHARED_MEM_NAME         // L"Global\\CatCam_SharedMem"
    );

    if (!_hMapFile)
        return HRESULT_FROM_WIN32(GetLastError());

    // Map the view into our process address space
    _header = (SharedMemHeader*)MapViewOfFile(
        _hMapFile, 
        FILE_MAP_READ | FILE_MAP_WRITE, 
        0, 
        0, 
        0
    );

    // Map the view into our process address space.
    // NOTE: never zero the header here — whoever probes first (this DLL,
    // inside Frame Server's mfpmp.exe) may create the mapping BEFORE the
    // host writes dims; zeroing on every DLL init would erase them.
    // Dimensions are owned by the host (from the tablet's config packet);
    // the DLL only reads them. If the mapping already exists, GetWidth/
    // GetHeight see the host's values; on a brand-new mapping the pages
    // are already zeroed by the OS.
    if (!_header)
        return HRESULT_FROM_WIN32(GetLastError());

    // Create the cross-process mutex (non-fatal if this fails, we can run lockless)
    _hMutex = CreateMutexW(&sa, FALSE, MUTEX_NAME);

    return S_OK;
}

HRESULT FrameServer::CopyLatestFrame(BYTE* dst, DWORD cap, DWORD* length, UINT64* frameIndex)
{
    if (!_header || !dst)
        return E_FAIL;

    bool locked = false;
    if (_hMutex) {
        DWORD r = WaitForSingleObject(_hMutex, 5);
        // WAIT_ABANDONED still grants ownership (previous owner died holding
        // it, e.g. host killed mid-Publish); the header is about to be fully
        // re-read so proceed. A timeout means the host is mid-write: treat
        // as "no new frame" rather than reading torn data.
        if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED)
            return E_PENDING;
        locked = true;
    }

    HRESULT hr;
    if (_header->frameIndex == _lastIndex) {
        hr = E_PENDING; // no new frame
    } else {
        DWORD len = _header->frameSize;
        if (len == 0 || len > cap) {
            hr = E_PENDING; // header mid-initialization or size mismatch
        } else {
            memcpy(dst, (const void*)_header->data, len);
            *length     = len;
            *frameIndex = _header->frameIndex;
            _lastIndex  = _header->frameIndex;
            hr = S_OK;
        }
    }

    if (locked) ReleaseMutex(_hMutex);
    return hr;
}

UINT32 FrameServer::GetWidth()  const { return _header ? _header->width  : 0; }
UINT32 FrameServer::GetHeight() const { return _header ? _header->height : 0; }
