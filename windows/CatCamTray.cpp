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
#include <windowsx.h>
#include <tlhelp32.h>
#include <objidl.h>
#include <gdiplus.h>
#include <stdio.h>
#include <vector>
#include <string>

#define WM_TRAYICON      (WM_USER + 1)
#define WM_SHOWICON      (WM_APP + 2)   // posted by a second instance
#define WM_EXITKEEPHOST  (WM_APP + 3)   // "/exit": quit tray, leave host running
#define WM_WIFITOGGLE    (WM_APP + 4)   // "/wifi": toggle Wi-Fi mode remotely
#define WM_TRANSPORTAPPLY (WM_APP + 5)  // beacon thread: registry already set, restart host
#define ID_TRAY_OPENLOG  1001
#define ID_TRAY_EXIT     1002
#define ID_TRAY_HIDE     1003
#define ID_TRAY_PREVIEW  1004
#define ID_TRAY_FLIP     1005
#define ID_TRAY_CAMSTART 1006
#define ID_TRAY_CAMSTOP  1007
#define ID_TRAY_MONITOR  1008
#define ID_TRAY_WIFI     1009
#define ID_CAM_ZOOMIN    1101
#define ID_CAM_ZOOMOUT   1102
#define ID_CAM_EVUP      1103
#define ID_CAM_EVDOWN    1104
#define ID_CAM_WARMER    1105
#define ID_CAM_COOLER    1106
#define ID_CAM_DAY       1107
#define ID_CAM_NIGHT     1108
#define ID_CAM_FOCUSLOCK 1109
#define ID_CAM_FOCUSAUTO 1110
#define ID_CAM_FOCUSNEAR 1111
#define ID_CAM_FOCUSFAR  1112
#define ID_CAM_WB        1113
#define ID_CAM_TORCH     1114
#define ID_CAM_MIRROR    1115
#define ID_CAM_FLIP      1116
#define ID_CAM_POWER     1117
#define ID_CAM_LBL_ZOOM  1120
#define ID_CAM_LBL_EV    1121
#define ID_CAM_LBL_TONE  1122
#define ID_CAM_LBL_FOCUS 1123
#define ID_CAM_LBL_STATE 1124
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
static ULONGLONG hostStartedAt = 0;      // for "did it run a real session?"
static DWORD     restartDelayMs = 3000;  // escalates while the host keeps dying
static int       hostExitStreak = 0;     // consecutive short-lived hosts
static DWORD     healIntervalMs = 30000; // yellow-heal backoff (30s -> 10min)
static int       healCount = 0;

static HANDLE hMap = nullptr;
static volatile SharedMemHeader* shm = nullptr;

// Demand side channel (must match FrameServer.h). Tray writes previewBeat
// while the preview window is open (the preview is a consumer too), reads
// tabletState/tabletOnDemand for the tooltip and to skip the adb heal in
// the normal on-demand idle state.
#pragma pack(push, 1)
struct ControlBlock {
    UINT32 magic, version;
    UINT64 consumerBeat, previewBeat;
    UINT32 tabletState;      // 0 none, 1 READY (camera off), 2 live
    UINT32 tabletOnDemand;
    UINT64 hostBeat;
    UINT32 cmdSeq;           // tray -> host mailbox
    char   cmd[11];          // longest verb "focus auto"/"focus near" + NUL
    UINT8  tuneFlags;        // bit0 day, bit1 focus locked, bit2 AF camera, bit3 EV ok,
                             // bit4 torch on, bit5 torch ok, bit6 mirror on, bit7 manual focus ok
    INT16  zoomX100;
    INT8   ev, tone;
    UINT8  wb;               // CONTROL_AWB_MODE value (1 auto, 2 incandescent, 3 fluorescent, 5 daylight, 6 cloudy)
    UINT8  focusMode;        // 0 auto, 1 locked, 2 manual
    UINT8  focusPos;         // 0 near .. 100 far
    UINT8  spare;
};
#pragma pack(pop)
static HANDLE hCtrlMap = nullptr;
static volatile ControlBlock* ctrl = nullptr;
static bool EnsureControl() {
    if (ctrl) return true;
    if (!hCtrlMap) hCtrlMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"Global\\CatCam_Control");
    if (!hCtrlMap) return false;
    ctrl = (volatile ControlBlock*)MapViewOfFile(hCtrlMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ControlBlock));
    return ctrl != nullptr;
}
static bool TabletReadyOnDemand() {
    return EnsureControl() && ctrl->tabletOnDemand && ctrl->tabletState == 1;
}
static bool TabletConnected() { return EnsureControl() && ctrl->tabletState != 0; }
static void Log(const char* fmt, ...);   // defined below

// Camera menu -> tablet, over the wire (works on Wi-Fi, no adb): drop the
// verb in the mailbox, the host forwards it. Same words the tablet's
// 0x10 command packet understands.
static void SendTablet(const char* verb) {
    if (!EnsureControl()) return;
    char buf[11] = {};
    strncpy_s(buf, verb, _TRUNCATE);
    memcpy((void*)ctrl->cmd, buf, 11);
    InterlockedIncrement((volatile LONG*)&ctrl->cmdSeq);
    Log("camera: %s", verb);
}
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
        va_end(ap);
        // Append mode: the post-write position is the size. Keep one
        // generation so a runaway loop costs 8MB, not the disk.
        const long size = ftell(f);
        fclose(f);
        if (size > 4 * 1024 * 1024) {
            char old[MAX_PATH * 2];
            snprintf(old, sizeof(old), "%s.1", logPath);
            MoveFileExA(logPath, old, MOVEFILE_REPLACE_EXISTING);
        }
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

        // Extended beacon: "CATCAM1 <port> <usb|wifi> <gen>". The tablet's
        // transport switch rides here; gen counts user toggles, so a gen we
        // have not seen = a fresh user action on the tablet, and it wins
        // over the local menu state (last touch wins, either surface).
        // gen 0 = never toggled; != not > so a reinstalled app still works.
        char mode[16] = "";
        unsigned gen = 0;
        if (sscanf_s(buf + 8, "%*u %15s %u", mode, (unsigned)_countof(mode), &gen) == 2
                && gen > 0) {
            DWORD lastGen = 0, cb = sizeof(lastGen);
            RegGetValueW(HKEY_CURRENT_USER, L"Software\\CatCam", L"TabletGen",
                RRF_RT_REG_DWORD, nullptr, &lastGen, &cb);
            if (gen != lastGen) {
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\CatCam", L"TabletGen",
                    REG_DWORD, &gen, sizeof(gen));
                const bool wantWifi = strcmp(mode, "wifi") == 0;
                if (wantWifi != WifiEnabled()) {
                    SetWifiEnabled(wantWifi);
                    Log("tablet switched transport -> %s", mode);
                    // Host restart happens on the main thread (it owns pi).
                    PostMessageW(hwnd, WM_TRANSPORTAPPLY, 0, 0);
                }
            }
        }
    }
}

static void StartHost() {
    if (pi.hProcess) return; // already running/adopted

    // Adopt an already-running host rather than spawning a duplicate.
    DWORD existing = FindHostPid();
    if (existing) {
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, existing);
        if (h) {
            pi = {}; pi.hProcess = h; hostStartedAt = GetTickCount64();
            Log("host adopted (pid %lu)", existing);
            return;
        }
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
    // Rotate first: no host is writing at this point (the adopt path above
    // already returned), and this file had grown to 14MB unattended.
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(hostLogPath, GetFileExInfoStandard, &fad)
            && (((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow) > 8ull * 1024 * 1024) {
        wchar_t old[MAX_PATH * 2];
        swprintf_s(old, L"%s.1", hostLogPath);
        MoveFileExW(hostLogPath, old, MOVEFILE_REPLACE_EXISTING);
        Log("host.log exceeded 8MB, rotated to host.log.1");
    }
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
        hostStartedAt = GetTickCount64();
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

// ---------------------------------------------------------------- overlay
// The preview IS the app: full-bleed video with the same overlay the tablet
// draws (Instagram/Snap camera idiom, tokens copied from res/drawable and
// activity_main.xml): status pill top centre; bottom stack of scrim pills
// (35% black, fully rounded; active segment = white pill, black text;
// inactive text 70% white), zoom in 44dp scrim circles, then the shutter
// row (76dp ring, 3dp white stroke, white circle off / red rounded square
// live) with the flip circle to its left. Extra rows the PC needs beyond
// the app (exposure, white balance, focus, torch, mirror) use the same
// pill vocabulary. Everything is drawn into one back buffer per frame and
// blitted once: no child windows, no flicker. Values shown are what the
// tablet reports back.
// Keys: +/- zoom, Up/Down exposure, Left/Right tone, F flip, Space shutter,
// A/L/N/M focus auto/lock/near/far, W white balance, D day/night, T torch,
// R mirror, Esc close.
enum class Kind { Seg, Circle, Shutter, Status, Label };
struct Item {
    int id; Kind kind; const wchar_t* def;   // default text
    wchar_t text[48]; bool enabled = true, active = false; RECT r{}; int row;
};
// Rows, top to bottom of the bottom stack; row 100 = status pill (top).
static Item g_items[] = {
    { ID_CAM_LBL_STATE, Kind::Status, L"no tablet",  L"", true, false, {}, 100 },
    { ID_CAM_COOLER,    Kind::Seg,    L"Cool",       L"", true, false, {}, 0 },
    { ID_CAM_LBL_TONE,  Kind::Label,  L"0",          L"", true, false, {}, 0 },
    { ID_CAM_WARMER,    Kind::Seg,    L"Warm",       L"", true, false, {}, 0 },
    { -1,               Kind::Label,  L"|",          L"", true, false, {}, 0 },   // group break
    { ID_CAM_DAY,       Kind::Seg,    L"Day",        L"", true, false, {}, 0 },
    { ID_CAM_NIGHT,     Kind::Seg,    L"Night",      L"", true, false, {}, 0 },
    { ID_CAM_EVDOWN,    Kind::Seg,    L"Darker",     L"", true, false, {}, 1 },
    { ID_CAM_LBL_EV,    Kind::Label,  L"0",          L"", true, false, {}, 1 },
    { ID_CAM_EVUP,      Kind::Seg,    L"Brighter",   L"", true, false, {}, 1 },
    { -1,               Kind::Label,  L"|",          L"", true, false, {}, 1 },
    { ID_CAM_WB,        Kind::Seg,    L"WB auto",    L"", true, false, {}, 1 },
    { ID_CAM_FOCUSAUTO, Kind::Seg,    L"Focus auto", L"", true, false, {}, 2 },
    { ID_CAM_FOCUSLOCK, Kind::Seg,    L"Lock",       L"", true, false, {}, 2 },
    { ID_CAM_FOCUSNEAR, Kind::Seg,    L"Nearer",     L"", true, false, {}, 2 },
    { ID_CAM_FOCUSFAR,  Kind::Seg,    L"Farther",    L"", true, false, {}, 2 },
    { -1,               Kind::Label,  L"|",          L"", true, false, {}, 2 },
    { ID_CAM_TORCH,     Kind::Seg,    L"Torch",      L"", true, false, {}, 2 },
    { -1,               Kind::Label,  L"|",          L"", true, false, {}, 2 },
    { ID_CAM_MIRROR,    Kind::Seg,    L"Mirror",     L"", true, false, {}, 2 },
    { ID_CAM_ZOOMOUT,   Kind::Circle, L"\u2212",     L"", true, false, {}, 3 },
    { ID_CAM_LBL_ZOOM,  Kind::Label,  L"1.0\u00d7",  L"", true, false, {}, 3 },
    { ID_CAM_ZOOMIN,    Kind::Circle, L"+",          L"", true, false, {}, 3 },
    { ID_CAM_FLIP,      Kind::Circle, L"\uE895",     L"", true, false, {}, 4 },   // Segoe MDL2 "Sync"
    { ID_CAM_POWER,     Kind::Shutter,L"",           L"", true, false, {}, 4 },
};
static int g_hot = -1, g_press = -1;
static bool g_trackingMouse = false;
static HDC g_backDc = nullptr; static HBITMAP g_backBmp = nullptr; static int g_backW = 0, g_backH = 0;
static COLORREF g_statusColor = 0x66000000;   // ARGB-ish: alpha in top byte for our own use
static bool g_live = false;

static Item* ItemById(int id) { for (auto& p : g_items) if (p.id == id) return &p; return nullptr; }
static void IText(int id, const wchar_t* t) { if (Item* p = ItemById(id)) wcscpy_s(p->text, t); }
static void IOn(int id, bool on) { if (Item* p = ItemById(id)) p->enabled = on; }
static void IActive(int id, bool a) { if (Item* p = ItemById(id)) p->active = a; }

static void RefreshModel() {
    for (auto& p : g_items) { wcscpy_s(p.text, p.def); p.enabled = true; p.active = false; }
    const bool on = TabletConnected();
    g_live = on && ctrl->tabletState == 2;
    if (!on) {
        IText(ID_CAM_LBL_STATE, L"no tablet"); g_statusColor = 0x66000000;
        for (auto& p : g_items) if (p.kind != Kind::Shutter && p.id != ID_CAM_FLIP) p.enabled = false;
        return;
    }
    const UINT8 f = ctrl->tuneFlags;
    wchar_t t[48];
    if (g_live) { IText(ID_CAM_LBL_STATE, L"LIVE"); g_statusColor = 0xFFE53935; }
    else { IText(ID_CAM_LBL_STATE, L"READY \u00b7 camera off"); g_statusColor = 0xFFDD9C10; }
    swprintf_s(t, L"%d.%d\u00d7", ctrl->zoomX100 / 100, (ctrl->zoomX100 % 100) / 10); IText(ID_CAM_LBL_ZOOM, t);
    swprintf_s(t, L"%+d", (int)ctrl->ev); IText(ID_CAM_LBL_EV, t);
    swprintf_s(t, L"%+d", (int)ctrl->tone); IText(ID_CAM_LBL_TONE, t);
    if (ctrl->tone == 0) IText(ID_CAM_LBL_TONE, L"0");
    if (ctrl->ev == 0) IText(ID_CAM_LBL_EV, L"0");
    IActive(ID_CAM_DAY, f & 1); IActive(ID_CAM_NIGHT, !(f & 1));
    IText(ID_CAM_WB, ctrl->wb == 2 ? L"WB incandescent" : ctrl->wb == 3 ? L"WB fluorescent"
        : ctrl->wb == 5 ? L"WB daylight" : ctrl->wb == 6 ? L"WB cloudy" : L"WB auto");
    IActive(ID_CAM_WB, ctrl->wb != 1);
    const bool af = f & 4, manual = f & 128;
    IActive(ID_CAM_FOCUSAUTO, af && ctrl->focusMode == 0);
    IActive(ID_CAM_FOCUSLOCK, af && ctrl->focusMode == 1);
    if (ctrl->focusMode == 2) { swprintf_s(t, L"Manual %u%%", (unsigned)ctrl->focusPos); IText(ID_CAM_FOCUSNEAR, L"Nearer"); }
    if (!af) IText(ID_CAM_FOCUSAUTO, L"Fixed focus");
    IOn(ID_CAM_FOCUSAUTO, af); IOn(ID_CAM_FOCUSLOCK, af);
    IOn(ID_CAM_FOCUSNEAR, manual); IOn(ID_CAM_FOCUSFAR, manual);
    IActive(ID_CAM_TORCH, f & 16); IOn(ID_CAM_TORCH, f & 32);
    IActive(ID_CAM_MIRROR, f & 64);
    IOn(ID_CAM_EVUP, f & 8); IOn(ID_CAM_EVDOWN, f & 8);
    if (!g_live)  // tuning only means something while the camera runs
        for (auto& p : g_items) if (p.kind == Kind::Seg || p.kind == Kind::Circle) if (p.id != ID_CAM_FLIP) p.enabled = false;
}

// Snapshot-diff so READY (no frames) still repaints on state changes.
static bool RefreshControls() {
    std::wstring before;
    for (auto& p : g_items) { before += p.text; before += p.enabled ? L'1' : L'0'; before += p.active ? L'1' : L'0'; }
    RefreshModel();
    std::wstring after;
    for (auto& p : g_items) { after += p.text; after += p.enabled ? L'1' : L'0'; after += p.active ? L'1' : L'0'; }
    return before != after;
}

static void RoundPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rf, float rad) {
    using namespace Gdiplus;
    if (rad * 2 > rf.Height) rad = rf.Height / 2;
    if (rad * 2 > rf.Width) rad = rf.Width / 2;
    path.AddArc(rf.X, rf.Y, rad * 2, rad * 2, 180, 90);
    path.AddArc(rf.X + rf.Width - rad * 2, rf.Y, rad * 2, rad * 2, 270, 90);
    path.AddArc(rf.X + rf.Width - rad * 2, rf.Y + rf.Height - rad * 2, rad * 2, rad * 2, 0, 90);
    path.AddArc(rf.X, rf.Y + rf.Height - rad * 2, rad * 2, rad * 2, 90, 90);
    path.CloseFigure();
}

// Layout + draw in one pass (text widths come from the same Graphics).
// dp: the app's dp, scaled so 1dp = 1px at 96 DPI.
static void DrawOverlay(Gdiplus::Graphics& g, HWND h, const RECT& client) {
    using namespace Gdiplus;
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    const float dp = GetDpiForWindow(h) / 96.f;
    Font f13(L"Segoe UI Semibold", 13 * dp * 96.f / 72.f, FontStyleRegular, UnitPixel);
    Font f14(L"Segoe UI", 14 * dp * 96.f / 72.f, FontStyleRegular, UnitPixel);
    Font f20(L"Segoe UI", 20 * dp * 96.f / 72.f, FontStyleRegular, UnitPixel);
    Font fIcon(L"Segoe MDL2 Assets", 20 * dp * 96.f / 72.f, FontStyleRegular, UnitPixel);
    StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    const Color scrim(0x59, 0, 0, 0), white(255, 255, 255, 255), white70(0xB3, 255, 255, 255), black(255, 0, 0, 0);
    SolidBrush bScrim(scrim), bWhite(white), bWhite70(white70), bBlack(black);
    const float cx = (client.left + client.right) / 2.f;

    // ---- status pill (top centre, 44dp down): solid pill in the state colour
    {
        Item* st = ItemById(ID_CAM_LBL_STATE);
        RectF m; g.MeasureString(st->text, -1, &f13, PointF(0, 0), &sf, &m);
        const float w = m.Width + 32 * dp, hh = m.Height + 14 * dp;
        RectF rf(cx - w / 2, client.top + 44 * dp, w, hh);
        GraphicsPath p; RoundPath(p, rf, hh / 2);
        SolidBrush b(Color((g_statusColor >> 24) & 0xFF, (g_statusColor >> 16) & 0xFF, (g_statusColor >> 8) & 0xFF, g_statusColor & 0xFF));
        g.FillPath(&b, &p);
        g.DrawString(st->text, -1, &f13, rf, &sf, &bWhite);
        st->r = { (LONG)rf.X, (LONG)rf.Y, (LONG)(rf.X + rf.Width), (LONG)(rf.Y + rf.Height) };
    }

    // ---- bottom stack: rows 0..2 pill groups, row 3 zoom, row 4 shutter.
    // Measure each row's width first, then place centred; rows stack upward
    // from 26dp above the bottom.
    const float segPadX = 14 * dp, segPadY = 6 * dp, groupPad = 4 * dp, gap = 12 * dp;
    const float rowGap = 14 * dp;
    float y = client.bottom - 26 * dp;
    // row 4: shutter row (80dp tall): shutter 76 centred, flip 52 at 64dp from the stack's left edge
    {
        const float rowH = 80 * dp; y -= rowH;
        Item* sh = ItemById(ID_CAM_POWER); Item* fl = ItemById(ID_CAM_FLIP);
        const float sd = 76 * dp, sx = cx - sd / 2, sy = y + (rowH - sd) / 2;
        // ring: 3dp white stroke over 15% black
        SolidBrush ringFill(Color(0x26, 0, 0, 0)); Pen ringPen(white, 3 * dp);
        g.FillEllipse(&ringFill, sx, sy, sd, sd);
        g.DrawEllipse(&ringPen, sx + 1.5f * dp, sy + 1.5f * dp, sd - 3 * dp, sd - 3 * dp);
        if (g_live) { // red rounded square 30dp
            const float d = 30 * dp; RectF rf(cx - d / 2, sy + sd / 2 - d / 2, d, d);
            GraphicsPath p; RoundPath(p, rf, 10 * dp);
            SolidBrush red(Color(255, 0xFF, 0x3B, 0x30)); g.FillPath(&red, &p);
        } else {      // white circle 60dp
            const float d = 60 * dp; g.FillEllipse(&bWhite, cx - d / 2, sy + sd / 2 - d / 2, d, d);
        }
        sh->r = { (LONG)sx, (LONG)sy, (LONG)(sx + sd), (LONG)(sy + sd) };
        // flip circle: 52dp, at 64dp from the left of the client (the app's
        // FrameLayout is full width), vertically centred on the shutter
        const float fd = 52 * dp, fx = cx - 110 * dp - fd / 2, fy = sy + sd / 2 - fd / 2;
        g.FillEllipse(&bScrim, fx, fy, fd, fd);
        RectF fr(fx, fy, fd, fd);
        SolidBrush* fb = fl->enabled ? &bWhite : &bWhite70;
        g.DrawString(fl->text, -1, &fIcon, fr, &sf, fb);
        fl->r = { (LONG)fx, (LONG)fy, (LONG)(fx + fd), (LONG)(fy + fd) };
        y -= 16 * dp;
    }
    // row 3: zoom (-) label (+)
    {
        Item* zo = ItemById(ID_CAM_ZOOMOUT); Item* zl = ItemById(ID_CAM_LBL_ZOOM); Item* zi = ItemById(ID_CAM_ZOOMIN);
        const float cd = 44 * dp; y -= cd;
        RectF m; g.MeasureString(zl->text, -1, &f14, PointF(0, 0), &sf, &m);
        const float lw = m.Width + 32 * dp, lh = 34 * dp, total = cd + 8 * dp + lw + 8 * dp + cd;
        float x = cx - total / 2;
        auto circle = [&](Item* it, float x0) {
            const bool hot = it == &g_items[g_hot >= 0 ? g_hot : 0] && g_hot >= 0, pr = g_press >= 0 && it == &g_items[g_press];
            SolidBrush b(Color(pr ? 0x90 : hot ? 0x70 : 0x59, 0, 0, 0));
            g.FillEllipse(&b, x0, y, cd, cd);
            RectF r(x0, y, cd, cd);
            g.DrawString(it->text, -1, &f20, r, &sf, it->enabled ? &bWhite : &bWhite70);
            it->r = { (LONG)x0, (LONG)y, (LONG)(x0 + cd), (LONG)(y + cd) };
        };
        circle(zo, x); x += cd + 8 * dp;
        RectF lr(x, y + (cd - lh) / 2, lw, lh); GraphicsPath p; RoundPath(p, lr, lh / 2);
        g.FillPath(&bScrim, &p); g.DrawString(zl->text, -1, &f14, lr, &sf, &bWhite);
        x += lw + 8 * dp;
        circle(zi, x);
        y -= rowGap;
    }
    // rows 2,1,0: pill groups (segments on a scrim pill; "|" = group break)
    for (int row = 2; row >= 0; row--) {
        // measure
        struct Seg { Item* it; float w; };
        std::vector<std::vector<Seg>> groups(1);
        float rowH = 0;
        for (auto& it : g_items) {
            if (it.row != row) continue;
            if (it.id == -1) { groups.emplace_back(); continue; }
            RectF m; g.MeasureString(it.text, -1, &f13, PointF(0, 0), &sf, &m);
            const float w = m.Width + (it.kind == Kind::Label ? 16 * dp : 2 * segPadX);
            if (m.Height + 2 * segPadY > rowH) rowH = m.Height + 2 * segPadY;
            groups.back().push_back({ &it, w });
        }
        float total = 0;
        for (auto& gr : groups) { float gw = 2 * groupPad; for (auto& s : gr) gw += s.w; total += gw; }
        total += gap * (groups.size() - 1);
        const float gh = rowH + 2 * groupPad;
        y -= gh;
        float x = cx - total / 2;
        for (auto& gr : groups) {
            float gw = 2 * groupPad; for (auto& s : gr) gw += s.w;
            RectF grf(x, y, gw, gh); GraphicsPath gp; RoundPath(gp, grf, gh / 2);
            g.FillPath(&bScrim, &gp);
            float sx = x + groupPad;
            for (auto& s : gr) {
                RectF rf(sx, y + groupPad, s.w, rowH);
                Item* it = s.it;
                it->r = { (LONG)rf.X, (LONG)rf.Y, (LONG)(rf.X + rf.Width), (LONG)(rf.Y + rf.Height) };
                const int idx = (int)(it - g_items);
                const bool hot = idx == g_hot, pr = idx == g_press;
                if (it->kind == Kind::Seg) {
                    if (it->active) {
                        GraphicsPath sp; RoundPath(sp, rf, rf.Height / 2);
                        SolidBrush b(Color(it->enabled ? 255 : 110, 255, 255, 255)); g.FillPath(&b, &sp);
                        SolidBrush tb(Color(it->enabled ? 255 : 160, 0, 0, 0));
                        g.DrawString(it->text, -1, &f13, rf, &sf, &tb);
                    } else {
                        if (hot || pr) { GraphicsPath sp; RoundPath(sp, rf, rf.Height / 2); SolidBrush b(Color(pr ? 0x40 : 0x22, 255, 255, 255)); g.FillPath(&b, &sp); }
                        SolidBrush tb(Color(it->enabled ? 0xB3 : 0x50, 255, 255, 255));
                        g.DrawString(it->text, -1, &f13, rf, &sf, &tb);
                    }
                } else { // value label
                    g.DrawString(it->text, -1, &f13, rf, &sf, &bWhite);
                }
                sx += s.w;
            }
            x += gw + gap;
        }
        y -= rowGap;
    }
}

static int ItemAt(POINT pt) {
    int i = 0;
    for (auto& p : g_items) { if (p.id > 0 && p.kind != Kind::Label && p.kind != Kind::Status && PtInRect(&p.r, pt)) return i; i++; }
    return -1;
}

static void EnsureBackBuffer(HDC dc, int w, int h) {
    if (g_backDc && g_backW == w && g_backH == h) return;
    if (g_backBmp) DeleteObject(g_backBmp);
    if (!g_backDc) g_backDc = CreateCompatibleDC(dc);
    g_backBmp = CreateCompatibleBitmap(dc, w, h);
    SelectObject(g_backDc, g_backBmp);
    g_backW = w; g_backH = h;
}

static LRESULT CALLBACK PreviewWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: RefreshControls(); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_hot >= 0 && g_items[g_hot].enabled) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    case WM_MOUSEMOVE: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const int i = ItemAt(pt);
        if (i != g_hot) { g_hot = i; InvalidateRect(h, nullptr, FALSE); }
        if (!g_trackingMouse) { TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 }; TrackMouseEvent(&tme); g_trackingMouse = true; }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_trackingMouse = false;
        if (g_hot >= 0) { g_hot = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        g_press = ItemAt(pt);
        if (g_press >= 0) { SetCapture(h); InvalidateRect(h, nullptr, FALSE); }
        SetFocus(h);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const int i = ItemAt(pt);
        if (GetCapture() == h) ReleaseCapture();
        if (i >= 0 && i == g_press && g_items[i].enabled)
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(g_items[i].id, 0), 0);
        g_press = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN: {
        int id = 0;
        switch (wp) {
        case VK_ADD: case VK_OEM_PLUS: id = ID_CAM_ZOOMIN; break;
        case VK_SUBTRACT: case VK_OEM_MINUS: id = ID_CAM_ZOOMOUT; break;
        case VK_UP: id = ID_CAM_EVUP; break;
        case VK_DOWN: id = ID_CAM_EVDOWN; break;
        case VK_RIGHT: id = ID_CAM_WARMER; break;
        case VK_LEFT: id = ID_CAM_COOLER; break;
        case 'F': id = ID_CAM_FLIP; break;
        case VK_SPACE: id = ID_CAM_POWER; break;
        case 'A': id = ID_CAM_FOCUSAUTO; break;
        case 'L': id = ID_CAM_FOCUSLOCK; break;
        case 'N': id = ID_CAM_FOCUSNEAR; break;
        case 'M': id = ID_CAM_FOCUSFAR; break;
        case 'W': id = ID_CAM_WB; break;
        case 'D': id = (TabletConnected() && (ctrl->tuneFlags & 1)) ? ID_CAM_NIGHT : ID_CAM_DAY; break;
        case 'T': id = ID_CAM_TORCH; break;
        case 'R': id = ID_CAM_MIRROR; break;
        case VK_ESCAPE: DestroyWindow(h); return 0;
        }
        if (id) { if (Item* p = ItemById(id)) if (p->enabled) SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(id, 0), 0); }
        return 0;
    }
    case WM_ERASEBKGND: return 1; // composed fully in WM_PAINT
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        if (rc.right <= 0 || rc.bottom <= 0) { EndPaint(h, &ps); return 0; }
        EnsureBackBuffer(dc, rc.right, rc.bottom);
        HDC bdc = g_backDc;
        const int cw = rc.right, ch = rc.bottom;
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        if (!rgbBuf.empty() && prevW && prevH && cw > 0 && ch > 0) {
            const double sc = min((double)cw / prevW, (double)ch / prevH);
            const int dw = (int)(prevW * sc), dh = (int)(prevH * sc);
            const int dx = (cw - dw) / 2, dy = (ch - dh) / 2;
            RECT s;
            s = { 0, 0, cw, dy };                FillRect(bdc, &s, black);
            s = { 0, dy + dh, cw, ch };          FillRect(bdc, &s, black);
            s = { 0, dy, dx, dy + dh };          FillRect(bdc, &s, black);
            s = { dx + dw, dy, cw, dy + dh };    FillRect(bdc, &s, black);
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = (LONG)prevW;
            bi.bmiHeader.biHeight = -(LONG)prevH; // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(bdc, HALFTONE);
            SetBrushOrgEx(bdc, 0, 0, nullptr);
            StretchDIBits(bdc, dx, dy, dw, dh, 0, 0, prevW, prevH,
                rgbBuf.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(bdc, &rc, black);
        }
        {
            Gdiplus::Graphics g(bdc);
            DrawOverlay(g, h, rc);
        }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, bdc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        prevWnd = nullptr; g_hot = g_press = -1;
        if (g_backBmp) { DeleteObject(g_backBmp); g_backBmp = nullptr; }
        if (g_backDc) { DeleteDC(g_backDc); g_backDc = nullptr; }
        g_backW = g_backH = 0;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void OpenPreview() {
    if (prevWnd) { ShowWindow(prevWnd, SW_SHOW); SetForegroundWindow(prevWnd); return; }
    // Open docked to the RIGHT HALF of the work area of the monitor the
    // cursor is on (the one the tray was clicked on): the preview lives
    // beside whatever is being worked on, no dragging. The frame is
    // letterboxed inside whatever shape that half is (WM_PAINT). Snap
    // and drag still work afterwards, this is only the opening geometry.
    POINT cur; GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    RECT wa{ 0, 0, 1280, 720 };
    if (GetMonitorInfoW(mon, &mi)) wa = mi.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const LONG half = (wa.right - wa.left) / 2;
    prevWnd = CreateWindowW(L"CatCamPreviewWnd", L"CatCam \u00b7 live preview & controls",
        // WS_CLIPCHILDREN: the video repaints 30x/s; without it the parent's
        // paint lands on top of the control-bar buttons (measured: empty bar).
        WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
        wa.left + half, wa.top, wa.right - (wa.left + half), wa.bottom - wa.top,
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
    // The preview is a consumer: an on-demand tablet turns its camera on
    // for it exactly like for Teams.
    if (prevWnd && EnsureControl()) ctrl->previewBeat = GetTickCount64();
    if (prevWnd && RefreshControls()) InvalidateRect(prevWnd, nullptr, FALSE);
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
        if (healCount < 2)
            Log("frames stalled: re-established adb forward tcp:9000 (next heals back off silently)");
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
    case CamState::Yellow: return TabletReadyOnDemand()
        ? L"CatCam \u00b7 ready, camera off until an app uses it"
        : L"CatCam \u00b7 waiting for tablet";
    case CamState::Red:    return L"CatCam \u00b7 host down, restarting";
    default:               return L"CatCam \u00b7 starting";
    }
}

static bool shownReadyTip = false;
static void UpdateIcon(CamState st) {
    if (iconHidden) return;
    const bool readyTip = (st == CamState::Yellow) && TabletReadyOnDemand();
    if (stateEverShown && st == shownState && readyTip == shownReadyTip) return;
    shownReadyTip = readyTip;
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
            AppendMenuW(menu, MF_STRING, ID_TRAY_PREVIEW, L"Live preview && controls");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, ID_TRAY_FLIP, L"Flip camera (front/back)");
            AppendMenuW(menu, MF_STRING, ID_TRAY_CAMSTART, L"Turn tablet camera on");
            AppendMenuW(menu, MF_STRING, ID_TRAY_CAMSTOP, L"Turn tablet camera off");
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
        // Flip/on/off: over the wire when a tablet is connected (Wi-Fi or
        // USB alike), the adb path only as the cold-start fallback.
        if (LOWORD(wp) == ID_TRAY_FLIP)     { if (TabletConnected()) SendTablet("flip");  else RunCamCtl(L"flip"); }
        if (LOWORD(wp) == ID_TRAY_CAMSTART) { if (TabletConnected()) SendTablet("start"); else RunCamCtl(L"start"); }
        if (LOWORD(wp) == ID_TRAY_CAMSTOP)  { if (TabletConnected()) SendTablet("stop");  else RunCamCtl(L"stop"); }
        if (LOWORD(wp) == ID_CAM_ZOOMIN)    SendTablet("zoom+");
        if (LOWORD(wp) == ID_CAM_ZOOMOUT)   SendTablet("zoom-");
        if (LOWORD(wp) == ID_CAM_EVUP)      SendTablet("ev+");
        if (LOWORD(wp) == ID_CAM_EVDOWN)    SendTablet("ev-");
        if (LOWORD(wp) == ID_CAM_WARMER)    SendTablet("tone+");
        if (LOWORD(wp) == ID_CAM_COOLER)    SendTablet("tone-");
        if (LOWORD(wp) == ID_CAM_DAY)       SendTablet("day");
        if (LOWORD(wp) == ID_CAM_NIGHT)     SendTablet("night");
        if (LOWORD(wp) == ID_CAM_FOCUSLOCK) SendTablet("focus lock");
        if (LOWORD(wp) == ID_CAM_FOCUSAUTO) SendTablet("focus auto");
        if (LOWORD(wp) == ID_CAM_FOCUSNEAR) SendTablet("focus near");
        if (LOWORD(wp) == ID_CAM_FOCUSFAR)  SendTablet("focus far");
        if (LOWORD(wp) == ID_CAM_WB)        SendTablet("wb next");
        if (LOWORD(wp) == ID_CAM_TORCH)     SendTablet("torch");
        if (LOWORD(wp) == ID_CAM_MIRROR)    SendTablet("mirror");
        if (LOWORD(wp) == ID_CAM_FLIP)      { if (TabletConnected()) SendTablet("flip"); else RunCamCtl(L"flip"); }
        if (LOWORD(wp) == ID_CAM_POWER) {
            if (!TabletConnected()) RunCamCtl(L"start");
            else SendTablet(ctrl->tabletState == 2 ? "stop" : "start");
        }
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
    case WM_TRANSPORTAPPLY:
        // Beacon thread already flipped the registry; act on it here (this
        // thread owns pi). Watchdog respawns the host on the new transport.
        if (WifiEnabled()) EnsureFirewallRule();
        if (pi.hProcess) TerminateProcess(pi.hProcess, 0);
        return 0;
    case WM_TIMER:
        if (wp == IDT_PREVIEW) { PreviewTick(); return 0; }
        if (wp == IDT_TICK) {
            // Watchdog: restart the host if it died, backing off while it
            // keeps dying young. A flat 3s retry here is what turned a
            // sleeping tablet into ~1200 host launches an hour and a tray
            // icon flapping red (cooling down) / yellow (briefly alive).
            // The host now waits for the tablet in-process, so a fast exit
            // means a real failure, and hammering it will not fix that.
            if (pi.hProcess && WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
                DWORD code = 0;
                GetExitCodeProcess(pi.hProcess, &code);
                ULONGLONG lived = hostStartedAt ? GetTickCount64() - hostStartedAt : 0;
                CloseHandle(pi.hProcess);
                if (pi.hThread) CloseHandle(pi.hThread);
                pi = {};
                if (lived >= 30000) {          // ran a real session: one-off death
                    restartDelayMs = 3000;
                    hostExitStreak = 0;
                } else if (restartDelayMs < 60000) {
                    restartDelayMs *= 2;
                    if (restartDelayMs > 60000) restartDelayMs = 60000;
                }
                hostExitStreak++;
                // Log the first few and then rarely: this line was 20k of
                // the 20,175 lines in tray.log.
                if (hostExitStreak <= 3 || hostExitStreak % 20 == 0)
                    Log("host exited (code %lu, ran %llus), restart in %lums%s",
                        code, lived / 1000, restartDelayMs,
                        hostExitStreak > 3 ? " [repeating]" : "");
                nextStartAllowed = GetTickCount64() + restartDelayMs;
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
            if (st == CamState::Yellow && !TabletReadyOnDemand()) {
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
                    // Yellow is now the NORMAL idle state (host waits for
                    // the tablet in-process), not a broken forward. A flat
                    // 30s heal spawned cmd+adb 2,880 times a day and logged
                    // every one. Back off: 30s doubling to 10min; reset the
                    // moment the state changes.
                    nextHealAt = now + healIntervalMs;
                    if (healIntervalMs < 600000) healIntervalMs *= 2;
                    healCount++;
                }
            } else {
                stalledSecs = 0;
                healIntervalMs = 30000;
                healCount = 0;
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
