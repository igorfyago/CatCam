// ============================================================================
// CatCamTray.exe · system tray launcher for CatCamHost.exe.
//
// Runs CatCamHost.exe as a hidden child (stdout appended to host.log),
// auto-restarts it if it dies, and shows a webcam tray icon with a status
// dot read from Global\CatCam_SharedMem:
//   green  = frameIndex advancing (streaming)
//   yellow = host alive, frames stalled (waiting for tablet)
//   red    = host process not running (restart pending)
//   gray   = starting up / shared memory not available yet
// Menu: Live preview / Flip camera / Start / Stop / Open host log /
// Hide tray icon / Exit.
// "Live preview" opens a small window rendering the shared-mem NV12
// frames (BT.709 -> RGB32, letterboxed StretchDIBits, up to 30fps).
// Flip/Start/Stop run camctl.bat, which drives the REAL tablet app
// buttons over adb (wake, foreground MainActivity, tap): the Android UI
// stays in sync because its labels re-read service state every 500ms.
// "Hide" removes the icon but keeps everything running; launching
// CatCamTray.exe again re-shows the icon (second instance signals the
// first and exits, single instance via mutex).
// If a CatCamHost.exe already runs at tray start it is ADOPTED, never
// duplicated: a second host would re-register the virtual camera and
// corrupt Frame Server state for the boot session (HANDOFF lesson 16).
// Manifest requireAdministrator: the one UAC prompt happens at tray start.
// ============================================================================
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <objidl.h>
#include <gdiplus.h>
#include <stdio.h>
#include <vector>

#define WM_TRAYICON      (WM_USER + 1)
#define WM_SHOWICON      (WM_APP + 2)   // posted by a second instance
#define WM_EXITKEEPHOST  (WM_APP + 3)   // "/exit": quit tray, leave host running
#define WM_WIFITOGGLE    (WM_APP + 4)   // "/wifi": toggle Wi-Fi mode remotely
#define ID_TRAY_OPENLOG  1001
#define ID_TRAY_EXIT     1002
#define ID_TRAY_HIDE     1003
#define ID_TRAY_PREVIEW  1004
#define ID_TRAY_FLIP     1005
#define ID_TRAY_CAMSTART 1006
#define ID_TRAY_CAMSTOP  1007
#define ID_TRAY_MONITOR  1008
#define ID_TRAY_WIFI     1009
#define IDT_TICK         42
#define IDT_PREVIEW      43

enum class CamState { Gray, Red, Yellow, Green };

static Gdiplus::Bitmap* LoadMascot();   // defined in the icon section

#pragma pack(push, 1)
struct SharedMemHeader {                 // must match CatCamHost.cpp
    UINT32 width, height, stride, frameSize;
    UINT64 frameIndex;
    UINT8  data[1];
};
#pragma pack(pop)

static NOTIFYICONDATAW nid{};
static PROCESS_INFORMATION pi{};
static HWND hwnd;
static wchar_t dir[MAX_PATH];
static wchar_t hostLogPath[MAX_PATH];
static bool iconHidden = false;
static CamState shownState = CamState::Gray;
static bool stateEverShown = false;
static ULONGLONG nextStartAllowed = 0;   // host restart cooldown

static HANDLE hMap = nullptr;
static volatile SharedMemHeader* shm = nullptr;
static UINT64 lastIdx = 0;
static ULONGLONG lastIdxChange = 0;

// Tablet discovery: the app broadcasts "CATCAM1 <port>" on UDP :9001 every
// 2s while streaming. A background thread records the freshest sender.
static CRITICAL_SECTION beaconCs;
static char beaconIp[64] = "";
static ULONGLONG beaconAtTick = 0;
static wchar_t hostIpArg[64] = L"";     // IP the running host was spawned with ("" = USB)

// Preview window state
static HWND prevWnd = nullptr;
static HANDLE hShmMutex = nullptr;
static std::vector<BYTE> nv12Buf, rgbBuf;
static UINT64 prevShownIdx = 0;
static UINT32 prevW = 0, prevH = 0;

static void Log(const char* fmt, ...) {
    // tray.log lives next to the exe (portability: no baked user paths).
    static char logPath[MAX_PATH * 2] = "";
    if (!logPath[0]) {
        wchar_t mod[MAX_PATH];
        GetModuleFileNameW(nullptr, mod, MAX_PATH);
        wchar_t* s = wcsrchr(mod, L'\\'); if (s) *s = 0;
        snprintf(logPath, sizeof(logPath), "%ls\\tray.log", mod);
    }
    FILE* f = nullptr;
    if (fopen_s(&f, logPath, "a") == 0 && f) {
        va_list ap; va_start(ap, fmt);
        vfprintf(f, fmt, ap); fprintf(f, "\n");
        va_end(ap); fclose(f);
    }
}

// ---------------------------------------------------------------- host child

static DWORD FindHostPid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"CatCamHost.exe") == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// Speaker monitor preference (HKCU\Software\CatCam, Monitor DWORD).
// Default 0 = muted: the monitor plays the tablet's room out the PC
// speakers, wanted for remote cat-listening, startling when the tablet
// sits next to the PC. The host reads --mute at start, so toggling
// restarts the host (the tick watchdog brings it back in ~3s).
static bool MonitorEnabled() {
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\CatCam", L"Monitor",
            RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS)
        return v != 0;
    return false;
}
static void SetMonitorEnabled(bool on) {
    DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\CatCam", L"Monitor",
        REG_DWORD, &v, sizeof(v));
}

// Wi-Fi mode (HKCU\Software\CatCam, WifiMode DWORD, default 0 = USB).
// ON: the host is spawned against the tablet IP heard on the discovery
// beacon, direct TCP, no adb anywhere in the media path. The USB path
// stays exactly as it always was when OFF.
static bool WifiEnabled() {
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\CatCam", L"WifiMode",
            RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS)
        return v != 0;
    return false;
}
static void SetWifiEnabled(bool on) {
    DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\CatCam", L"WifiMode",
        REG_DWORD, &v, sizeof(v));
}

// Inbound UDP :9001 must pass the Windows firewall for beacons to arrive.
// One-time (registry-flagged), needs elevation, which the tray has.
static void EnsureFirewallRule() {
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\CatCam", L"FwBeaconRule",
            RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS && v) return;
    wchar_t cmd[512];
    swprintf_s(cmd, L"cmd.exe /c netsh advfirewall firewall add rule "
        L"name=\"CatCam discovery\" dir=in action=allow protocol=udp localport=9001");
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION cpi{};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &cpi)) {
        CloseHandle(cpi.hProcess); CloseHandle(cpi.hThread);
        DWORD one = 1;
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\CatCam", L"FwBeaconRule",
            REG_DWORD, &one, sizeof(one));
        Log("firewall rule for beacon port added");
    }
}

// Listens forever; cheap, runs in both modes so the IP is warm the moment
// Wi-Fi mode is switched on.
static DWORD WINAPI BeaconThread(LPVOID) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { Log("beacon: socket failed %d", WSAGetLastError()); return 0; }
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(9001);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        Log("beacon: bind 9001 failed %d", WSAGetLastError());
        closesocket(s);
        return 0;
    }
    char buf[128];
    for (;;) {
        sockaddr_in from{};
        int flen = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &flen);
        if (n <= 0) continue;
        buf[n] = 0;
        if (strncmp(buf, "CATCAM1 ", 8) != 0) continue;
        char ip[64];
        if (!inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip))) continue;
        EnterCriticalSection(&beaconCs);
        bool changed = strcmp(beaconIp, ip) != 0;
        strcpy_s(beaconIp, ip);
        beaconAtTick = GetTickCount64();
        LeaveCriticalSection(&beaconCs);
        if (changed) Log("beacon: tablet at %s", ip);
    }
}

static void StartHost() {
    if (pi.hProcess) return; // already running/adopted

    // Adopt an already-running host rather than spawning a duplicate.
    DWORD existing = FindHostPid();
    if (existing) {
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, existing);
        if (h) { pi = {}; pi.hProcess = h; Log("host adopted (pid %lu)", existing); return; }
        Log("host pid %lu found but OpenProcess failed (%lu)", existing, GetLastError());
    }

    // Wi-Fi mode: the host targets the beacon-discovered tablet IP. No
    // beacon fresher than 15s means nothing to connect to yet; stay down
    // and let the 1s watchdog retry (log at most every 30s).
    wchar_t wifiIp[64] = L"";
    if (WifiEnabled()) {
        EnterCriticalSection(&beaconCs);
        if (beaconIp[0] && GetTickCount64() - beaconAtTick < 15000)
            swprintf_s(wifiIp, L"%S", beaconIp);
        LeaveCriticalSection(&beaconCs);
        if (!wifiIp[0]) {
            static ULONGLONG lastNoBeacon = 0;
            ULONGLONG now = GetTickCount64();
            if (now - lastNoBeacon > 30000) {
                lastNoBeacon = now;
                Log("wifi mode: waiting for tablet beacon");
            }
            return;
        }
    }

    // Append host stdout/stderr to host.log so diagnostics survive the
    // hidden window (a tray-started host used to log nowhere).
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE hLog = CreateFileW(hostLogPath, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    wchar_t exe[MAX_PATH];
    swprintf_s(exe, L"%s\\CatCamHost.exe", dir);
    const bool monitor = MonitorEnabled();
    wchar_t cmd[MAX_PATH * 2];
    swprintf_s(cmd, L"\"%s\"%s%s%s", exe,
        wifiIp[0] ? L" " : L"", wifiIp,
        monitor ? L"" : L" --mute");
    wcscpy_s(hostIpArg, wifiIp);
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (hLog != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = hLog; si.hStdError = hLog; si.hStdInput = nullptr;
    }
    if (CreateProcessW(exe, cmd, nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, dir, &si, &pi)) {
        Log("host started hidden (pid %lu, monitor %s, target %s)", pi.dwProcessId,
            monitor ? "ON" : "muted", wifiIp[0] ? "wifi" : "usb");
    } else {
        Log("host start failed (%lu)", GetLastError());
    }
    if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog); // child keeps its copy
}

static void StopHost() {
    if (pi.hProcess) {
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
        pi = {};
        Log("host stopped");
    }
}

// ---------------------------------------------------------------- status

static bool EnsureSharedMem() {
    if (shm) return true;
    if (!hMap) hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\CatCam_SharedMem");
    if (!hMap) return false;
    // Full view: the status dot only needs the header, the preview window
    // needs the frame bytes too.
    shm = (volatile SharedMemHeader*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (shm && !hShmMutex)
        hShmMutex = OpenMutexW(MUTEX_MODIFY_STATE | SYNCHRONIZE, FALSE, L"Global\\CatCam_Mutex");
    return shm != nullptr;
}

// ---------------------------------------------------------------- preview

// BT.709 video-range NV12 -> BGRA, fixed point (matches the stream's
// advertised colorimetry).
static void Nv12ToRgb32(const BYTE* src, BYTE* dst, UINT32 w, UINT32 h) {
    const BYTE* yp = src;
    const BYTE* uv = src + (size_t)w * h;
    for (UINT32 r = 0; r < h; r++) {
        const BYTE* uvRow = uv + (size_t)(r / 2) * w;
        const BYTE* yRow = yp + (size_t)r * w;
        BYTE* out = dst + (size_t)r * w * 4;
        for (UINT32 c = 0; c < w; c++) {
            int y = ((int)yRow[c] - 16) * 298;
            int u = (int)uvRow[c & ~1u] - 128;
            int v = (int)uvRow[(c & ~1u) + 1] - 128;
            int rr = (y + 459 * v + 128) >> 8;
            int gg = (y - 55 * u - 136 * v + 128) >> 8;
            int bb = (y + 541 * u + 128) >> 8;
            out[(size_t)c * 4 + 0] = (BYTE)(bb < 0 ? 0 : bb > 255 ? 255 : bb);
            out[(size_t)c * 4 + 1] = (BYTE)(gg < 0 ? 0 : gg > 255 ? 255 : gg);
            out[(size_t)c * 4 + 2] = (BYTE)(rr < 0 ? 0 : rr > 255 ? 255 : rr);
            out[(size_t)c * 4 + 3] = 255;
        }
    }
}

static LRESULT CALLBACK PreviewWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1; // painted fully in WM_PAINT (no flicker)
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        const int cw = rc.right, ch = rc.bottom;
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        if (!rgbBuf.empty() && prevW && prevH && cw > 0 && ch > 0) {
            const double sc = min((double)cw / prevW, (double)ch / prevH);
            const int dw = (int)(prevW * sc), dh = (int)(prevH * sc);
            const int dx = (cw - dw) / 2, dy = (ch - dh) / 2;
            RECT s;
            s = { 0, 0, cw, dy };                FillRect(dc, &s, black); // top
            s = { 0, dy + dh, cw, ch };          FillRect(dc, &s, black); // bottom
            s = { 0, dy, dx, dy + dh };          FillRect(dc, &s, black); // left
            s = { dx + dw, dy, cw, dy + dh };    FillRect(dc, &s, black); // right
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = (LONG)prevW;
            bi.bmiHeader.biHeight = -(LONG)prevH; // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, dx, dy, dw, dh, 0, 0, prevW, prevH,
                rgbBuf.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(dc, &rc, black);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: prevWnd = nullptr; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void OpenPreview() {
    if (prevWnd) { ShowWindow(prevWnd, SW_SHOW); SetForegroundWindow(prevWnd); return; }
    UINT32 w = 720, h = 1280;
    if (EnsureSharedMem() && shm->width && shm->height) { w = shm->width; h = shm->height; }
    RECT rc{ 0, 0, (LONG)w / 2, (LONG)h / 2 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    prevWnd = CreateWindowW(L"CatCamPreviewWnd", L"CatCam preview",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    // Title-bar/taskbar mascot (built once from the tray art, no dot).
    static HICON prevIcon = nullptr;
    if (!prevIcon) {
        if (Gdiplus::Bitmap* mascot = LoadMascot()) {
            Gdiplus::Bitmap b(32, 32, PixelFormat32bppARGB);
            Gdiplus::Graphics g(&b);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(mascot, 0, 0, 32, 32);
            b.GetHICON(&prevIcon);
        }
    }
    if (prevIcon) {
        SendMessageW(prevWnd, WM_SETICON, ICON_SMALL, (LPARAM)prevIcon);
        SendMessageW(prevWnd, WM_SETICON, ICON_BIG, (LPARAM)prevIcon);
    }
    prevShownIdx = 0; // force first frame
}

static void PreviewTick() {
    if (!prevWnd || !EnsureSharedMem()) return;
    const UINT32 w = shm->width, h = shm->height;
    if (!w || !h) return;
    if (shm->frameIndex == prevShownIdx) return;
    const DWORD need = w * h * 3 / 2;
    if (nv12Buf.size() != need) {
        nv12Buf.resize(need);
        rgbBuf.resize((size_t)w * h * 4);
        prevW = w; prevH = h;
    }
    bool locked = false;
    if (hShmMutex) {
        DWORD r = WaitForSingleObject(hShmMutex, 5);
        if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) return; // host mid-write
        locked = true;
    }
    memcpy(nv12Buf.data(), (const void*)shm->data, need);
    prevShownIdx = shm->frameIndex;
    if (locked) ReleaseMutex(hShmMutex);
    Nv12ToRgb32(nv12Buf.data(), rgbBuf.data(), w, h);
    InvalidateRect(prevWnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- tablet ctl

// Fire-and-forget: camctl.bat wakes the tablet, foregrounds the CatCam
// activity and taps the requested REAL app button, so the Android UI is
// the single source of truth and stays aligned with what we triggered.
static void RunCamCtl(const wchar_t* verb) {
    wchar_t cmd[MAX_PATH * 2];
    swprintf_s(cmd, L"cmd.exe /c \"%s\\camctl.bat\" %s", dir, verb);
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION cpi{};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, dir, &si, &cpi)) {
        CloseHandle(cpi.hProcess); CloseHandle(cpi.hThread);
        Log("camctl %S", verb);
    } else {
        Log("camctl launch failed (%lu)", GetLastError());
    }
}

// Auto-heal (lesson 33): re-establish the adb port forward the host's TCP
// connect rides on. A USB replug kills the device's forwards and an adb
// server restart drops them all; until now only the logon task re-created
// them. Idempotent when the forward exists, harmless while the tablet is
// unplugged. Fire-and-forget, windowless. The adb path and device pinning
// live in adbfwd.bat next to the exe (catcam-env.bat defaults + optional
// catcam.env.bat overrides), so no machine paths are baked in here.
static void RunAdbForward() {
    wchar_t cmd[MAX_PATH * 2];
    swprintf_s(cmd, L"cmd.exe /c \"%s\\adbfwd.bat\"", dir);
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION cpi{};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, dir, &si, &cpi)) {
        CloseHandle(cpi.hProcess); CloseHandle(cpi.hThread);
        Log("frames stalled: re-established adb forward tcp:9000");
    } else {
        Log("adb forward heal failed to launch (%lu)", GetLastError());
    }
}

static int       stalledSecs = 0;
static ULONGLONG nextHealAt = 0;

static CamState ReadState() {
    bool hostAlive = pi.hProcess && WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT;
    if (!hostAlive) return CamState::Red;
    if (!EnsureSharedMem()) return CamState::Gray;
    UINT64 idx = shm->frameIndex;   // aligned 8-byte read, atomic on x64
    ULONGLONG now = GetTickCount64();
    if (idx != lastIdx) { lastIdx = idx; lastIdxChange = now; }
    if (now - lastIdxChange <= 3000) return CamState::Green;
    return CamState::Yellow;
}

// ---------------------------------------------------------------- icon

// Brand mascot (windows\catcam64.png, the cat face) loaded once via GDI+
// (PNG alpha survives, unlike the HICON round-trips). nullptr = fall back
// to the old runtime-drawn webcam glyph.
static Gdiplus::Bitmap* LoadMascot() {
    static Gdiplus::Bitmap* m = (Gdiplus::Bitmap*)(INT_PTR)-1;
    if (m == (Gdiplus::Bitmap*)(INT_PTR)-1) {
        wchar_t p[MAX_PATH];
        swprintf_s(p, L"%s\\catcam64.png", dir);
        m = new Gdiplus::Bitmap(p);
        if (!m || m->GetLastStatus() != Gdiplus::Ok) { delete m; m = nullptr; }
        Log(m ? "mascot icon loaded" : "catcam64.png missing, drawn glyph fallback");
    }
    return m;
}

// 32x32 mascot (or webcam glyph) with a status dot, composed with GDI+.
static HICON BuildIcon(CamState st) {
    using namespace Gdiplus;
    Bitmap bmp(32, 32, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));

    if (Bitmap* mascot = LoadMascot()) {
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.DrawImage(mascot, 0, 0, 32, 32);
    } else {
        // Camera body: circle, light gray with darker rim.
        SolidBrush body(Color(255, 225, 225, 228));
        Pen rim(Color(255, 90, 90, 96), 2.0f);
        g.FillEllipse(&body, 3, 1, 26, 26);
        g.DrawEllipse(&rim, 3, 1, 26, 26);
        // Lens: dark ring + blue-ish pupil with a highlight.
        SolidBrush lensOuter(Color(255, 45, 45, 52));
        g.FillEllipse(&lensOuter, 9, 7, 14, 14);
        SolidBrush pupil(Color(255, 70, 110, 160));
        g.FillEllipse(&pupil, 12, 10, 8, 8);
        SolidBrush glint(Color(200, 240, 240, 245));
        g.FillEllipse(&glint, 13, 11, 3, 3);
        // Stand.
        Pen stand(Color(255, 90, 90, 96), 3.0f);
        g.DrawLine(&stand, 16, 27, 16, 30);
        g.DrawLine(&stand, 10, 31, 22, 31);
    }

    // Status dot, bottom-right, with dark outline for contrast.
    Color dot;
    switch (st) {
    case CamState::Green:  dot = Color(255, 46, 204, 64);  break;
    case CamState::Yellow: dot = Color(255, 255, 196, 0);  break;
    case CamState::Red:    dot = Color(255, 229, 57, 53);  break;
    default:               dot = Color(255, 158, 158, 158); break;
    }
    SolidBrush dotBrush(dot);
    Pen dotRim(Color(255, 30, 30, 30), 1.5f);
    g.FillEllipse(&dotBrush, 20, 20, 11, 11);
    g.DrawEllipse(&dotRim, 20, 20, 11, 11);

    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

// Same status vocabulary as the tablet app's pill (LIVE / waiting).
static const wchar_t* StateTip(CamState st) {
    switch (st) {
    case CamState::Green:  return L"CatCam \u00b7 LIVE";
    case CamState::Yellow: return L"CatCam \u00b7 waiting for tablet";
    case CamState::Red:    return L"CatCam \u00b7 host down, restarting";
    default:               return L"CatCam \u00b7 starting";
    }
}

static void UpdateIcon(CamState st) {
    if (iconHidden) return;
    if (stateEverShown && st == shownState) return;
    HICON fresh = BuildIcon(st);
    if (!fresh) return;
    HICON old = nid.hIcon;
    nid.hIcon = fresh;
    wcscpy_s(nid.szTip, StateTip(st));
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(stateEverShown ? NIM_MODIFY : NIM_ADD, &nid);
    if (old) DestroyIcon(old);
    shownState = st;
    stateEverShown = true;
}

// ---------------------------------------------------------------- window

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) {
            POINT p; GetCursorPos(&p);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_TRAY_PREVIEW, L"Live preview");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, ID_TRAY_FLIP, L"Flip camera (front/back)");
            AppendMenuW(menu, MF_STRING, ID_TRAY_CAMSTART, L"Start tablet camera");
            AppendMenuW(menu, MF_STRING, ID_TRAY_CAMSTOP, L"Stop tablet camera");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING | (WifiEnabled() ? MF_CHECKED : 0),
                ID_TRAY_WIFI, L"Wi-Fi mode (no cable)");
            AppendMenuW(menu, MF_STRING | (MonitorEnabled() ? MF_CHECKED : 0),
                ID_TRAY_MONITOR, L"Speaker monitor (hear tablet room)");
            AppendMenuW(menu, MF_STRING, ID_TRAY_OPENLOG, L"Open host log");
            AppendMenuW(menu, MF_STRING, ID_TRAY_HIDE, L"Hide tray icon");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit CatCam");
            SetForegroundWindow(h);
            TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, p.x, p.y, 0, h, nullptr);
            DestroyMenu(menu);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_TRAY_PREVIEW)  OpenPreview();
        if (LOWORD(wp) == ID_TRAY_FLIP)     RunCamCtl(L"flip");
        if (LOWORD(wp) == ID_TRAY_CAMSTART) RunCamCtl(L"start");
        if (LOWORD(wp) == ID_TRAY_CAMSTOP)  RunCamCtl(L"stop");
        if (LOWORD(wp) == ID_TRAY_WIFI) {
            const bool on = !WifiEnabled();
            SetWifiEnabled(on);
            Log("wifi mode %s; restarting host", on ? "ON" : "OFF");
            if (on) EnsureFirewallRule();
            // Kill the host; the tick watchdog restarts it on the new transport.
            if (pi.hProcess) TerminateProcess(pi.hProcess, 0);
        }
        if (LOWORD(wp) == ID_TRAY_MONITOR) {
            const bool on = !MonitorEnabled();
            SetMonitorEnabled(on);
            Log("speaker monitor toggled %s; restarting host", on ? "ON" : "muted");
            // Kill the host; the tick watchdog restarts it with the new flag.
            if (pi.hProcess) TerminateProcess(pi.hProcess, 0);
        }
        if (LOWORD(wp) == ID_TRAY_OPENLOG) {
            ShellExecuteW(nullptr, L"open", L"notepad.exe", hostLogPath, nullptr, SW_SHOW);
        }
        if (LOWORD(wp) == ID_TRAY_HIDE) {
            Shell_NotifyIconW(NIM_DELETE, &nid);
            iconHidden = true; stateEverShown = false;
            Log("icon hidden (run CatCamTray.exe again to show)");
        }
        if (LOWORD(wp) == ID_TRAY_EXIT) {
            StopHost();
            if (!iconHidden) Shell_NotifyIconW(NIM_DELETE, &nid);
            PostQuitMessage(0);
        }
        return 0;
    case WM_SHOWICON:                    // second instance asked us to reappear
        if (iconHidden) {
            iconHidden = false; stateEverShown = false;
            UpdateIcon(ReadState());
            Log("icon re-shown");
        }
        return 0;
    case WM_EXITKEEPHOST:
        // Deploy verb ("CatCamTray.exe /exit"): quit the tray but LEAVE the
        // host running; the next tray adopts it (lesson 16: never duplicate,
        // never re-register). Makes tray swaps possible from an unelevated
        // shell with no UAC prompt.
        Log("exit requested (/exit), host left running");
        if (pi.hProcess) {
            CloseHandle(pi.hProcess);
            if (pi.hThread) CloseHandle(pi.hThread);
            pi = {};
        }
        if (!iconHidden) Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    case WM_WIFITOGGLE:
        // Remote verb ("CatCamTray.exe /wifi"): same as clicking the menu item.
        PostMessageW(h, WM_COMMAND, ID_TRAY_WIFI, 0);
        return 0;
    case WM_TIMER:
        if (wp == IDT_PREVIEW) { PreviewTick(); return 0; }
        if (wp == IDT_TICK) {
            // Watchdog: restart the host if it died (3s cooldown).
            if (pi.hProcess && WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
                CloseHandle(pi.hProcess);
                if (pi.hThread) CloseHandle(pi.hThread);
                pi = {};
                Log("host exited, restart in 3s");
                nextStartAllowed = GetTickCount64() + 3000;
            }
            if (!pi.hProcess && GetTickCount64() >= nextStartAllowed) StartHost();
            CamState st = ReadState();
            UpdateIcon(st);
            // Yellow = host alive, frames stalled. USB: the dominant cause
            // is a dead adb forward, re-add it (5s, then every 30s). WiFi:
            // there is no forward; the actionable cause is the tablet
            // having moved to a new IP (DHCP), so restart the host against
            // the fresh beacon; same-IP stalls are the host's own retry
            // loop's job.
            if (st == CamState::Yellow) {
                ULONGLONG now = GetTickCount64();
                if (++stalledSecs >= 5 && now >= nextHealAt) {
                    if (WifiEnabled()) {
                        wchar_t cur[64];
                        EnterCriticalSection(&beaconCs);
                        swprintf_s(cur, L"%S", beaconIp);
                        LeaveCriticalSection(&beaconCs);
                        if (cur[0] && hostIpArg[0] && _wcsicmp(cur, hostIpArg) != 0
                                && pi.hProcess) {
                            Log("wifi heal: tablet moved %S -> %S, restarting host",
                                hostIpArg, cur);
                            TerminateProcess(pi.hProcess, 0);
                        }
                    } else {
                        RunAdbForward();
                    }
                    nextHealAt = now + 30000;
                }
            } else {
                stalledSecs = 0;
            }
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Global\\CatCamTray_Once");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Second launch signals the running instance. Default: "show the
        // icon again". Verbs: /exit (quit, keep host: promptless tray
        // swaps) and /wifi (toggle Wi-Fi mode remotely). The main window
        // is MESSAGE-ONLY, and FindWindowW never sees those: search the
        // HWND_MESSAGE list explicitly (this was the
        // hide-then-cannot-unhide bug).
        HWND existing = FindWindowExW(HWND_MESSAGE, nullptr, L"CatCamTray", nullptr);
        if (existing) {
            if (cmdLine && wcsstr(cmdLine, L"/exit"))
                PostMessageW(existing, WM_EXITKEEPHOST, 0, 0);
            else if (cmdLine && wcsstr(cmdLine, L"/wifi"))
                PostMessageW(existing, WM_WIFITOGGLE, 0, 0);
            else
                PostMessageW(existing, WM_SHOWICON, 0, 0);
        }
        return 0;
    }

    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    wchar_t* sl = wcsrchr(dir, L'\\'); if (sl) *sl = 0;
    swprintf_s(hostLogPath, L"%s\\host.log", dir);

    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartupInput gdipIn;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipIn, nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"CatCamTray";
    RegisterClassW(&wc);
    hwnd = CreateWindowW(L"CatCamTray", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    // Let an UNELEVATED second launch reach this (usually elevated)
    // instance: UIPI silently drops app-range messages from lower
    // integrity unless explicitly allowed.
    ChangeWindowMessageFilterEx(hwnd, WM_SHOWICON, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, WM_EXITKEEPHOST, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, WM_WIFITOGGLE, MSGFLT_ALLOW, nullptr);

    // Tablet discovery listener (both modes; the IP is warm the moment
    // Wi-Fi mode is enabled). Firewall pass for the beacon port is ensured
    // up front when Wi-Fi mode is already on (registry-toggled starts).
    InitializeCriticalSection(&beaconCs);
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
        CreateThread(nullptr, 0, BeaconThread, nullptr, 0, nullptr);
    else
        Log("WSAStartup failed: no beacon listener");
    if (WifiEnabled()) EnsureFirewallRule();

    WNDCLASSW pc{};
    pc.lpfnWndProc = PreviewWndProc; pc.hInstance = inst;
    pc.lpszClassName = L"CatCamPreviewWnd";
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&pc);

    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd; nid.uID = 1;
    nid.uCallbackMessage = WM_TRAYICON;

    StartHost();
    UpdateIcon(ReadState());
    SetTimer(hwnd, IDT_TICK, 1000, nullptr);
    SetTimer(hwnd, IDT_PREVIEW, 33, nullptr); // no-op unless preview is open

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    StopHost();
    Gdiplus::GdiplusShutdown(gdipToken);
    return 0;
}
