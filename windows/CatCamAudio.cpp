// ============================================================================
// CatCamAudio.exe · makes the VB-Cable endpoints look like CatCam's.
//
// VB-Audio Virtual Cable ships four endpoints named after itself: "CABLE
// Input" (render) and "CABLE Output" (capture) for the 2ch cable, plus a
// "CABLE In 16ch" / "CABLE Out 16ch" pair nobody here uses. That is two
// unexplained entries in every picker and a name that says nothing about
// CatCam. This tool, run elevated by the installer:
//   setup    rename the 2ch capture endpoint  -> "CatCam Microphone"
//            rename the 2ch render endpoint   -> "CatCam Mic Feed"
//            disable both 16 Ch endpoints
//            pin the render endpoint ID under HKLM\SOFTWARE\CatCam so the
//            host opens exactly this device (name matching stays a fallback)
//   restore  undo exactly what setup did (uninstall): each endpoint gets
//            the name it had before setup, 16 Ch re-enabled only if setup
//            was the one that disabled it
//   list     print every endpoint (id, state, name) for diagnostics
//
// setup remembers, per endpoint ID, the original DeviceDesc and whether it
// disabled the endpoint (HKLM\SOFTWARE\CatCam\AudioOriginal). It writes
// that memory only once (first setup), so re-running setup on an already
// named machine, or after a logon (catcam-boot.bat re-runs it, for the
// fresh-install-then-reboot case), never records "CatCam Microphone" as the
// original. A user's own renames are what restore puts back, not VB's
// defaults. Endpoints already disabled before setup are left alone.
//
// Renaming = PKEY_Device_DeviceDesc on the endpoint (the same field the
// Sound control panel's Rename box edits; the adapter name in parentheses
// is untouched, so "CatCam Microphone (VB-Audio Virtual Cable)" is what apps
// show). Disabling = the control panel's Disable. Both go through the
// documented IPropertyStore first and fall back to IPolicyConfig, the
// (undocumented, stable since Vista) interface mmsys.cpl itself uses.
// The render endpoint is NOT disabled: the host renders the tablet's PCM
// into it, and a disabled endpoint cannot be opened.
//
// Ownership: a VB-Cable the user already had (OBS, Voicemeeter, Discord)
// is not CatCam's to rename, and Voicemeeter binds by NAME, so setup only
// renames/disables when HKLM\SOFTWARE\CatCam\CableInstalledByCatCam is 1
// (mic-setup.ps1 writes it when it actually ran VB's installer) or when
// called as "setup --force". Otherwise it only pins the render endpoint,
// which is all the host needs, and says so.
//
// Every setup/restore run appends to audio.log next to the exe (the
// installer runs it hidden and Inno ignores exit codes; this is where a
// failure becomes visible, and the diag zip picks it up).
//
// Nothing here modifies VB-Cable's driver or binaries: endpoint names and
// states are Windows-side per-endpoint properties, user-editable by design.
// ============================================================================
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "advapi32.lib")

using Microsoft::WRL::ComPtr;

// PKEY_Device_DeviceDesc is not in functiondiscoverykeys_devpkey.h.
static const PROPERTYKEY PKEY_Device_DeviceDesc_ = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 2 };

// ---- IPolicyConfig (mmsys.cpl's own interface; not in the SDK headers).
static const CLSID CLSID_PolicyConfigClient =
    { 0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };
static const IID IID_IPolicyConfig =
    { 0xf8679f50, 0x850a, 0x41cf, { 0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8 } };
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, struct DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, struct DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, BOOL) = 0;
};

struct Endpoint {
    std::wstring id, desc, friendly, adapter;
    EDataFlow flow = eRender;
    DWORD state = 0;   // 0 = unknown: never mistaken for ACTIVE
};

static std::wstring PropStr(IPropertyStore* ps, const PROPERTYKEY& key) {
    PROPVARIANT v; PropVariantInit(&v);
    std::wstring s;
    if (SUCCEEDED(ps->GetValue(key, &v)) && v.vt == VT_LPWSTR) s = v.pwszVal;
    PropVariantClear(&v);
    return s;
}

static bool ContainsI(const std::wstring& hay, const wchar_t* needle) {
    std::wstring h = hay, n = needle;
    for (auto& c : h) c = towlower(c);
    for (auto& c : n) c = towlower(c);
    return h.find(n) != std::wstring::npos;
}

static std::vector<Endpoint> Enumerate(IMMDeviceEnumerator* en) {
    std::vector<Endpoint> out;
    for (EDataFlow flow : { eRender, eCapture }) {
        ComPtr<IMMDeviceCollection> col;
        if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATEMASK_ALL, &col))) continue;
        UINT n = 0; col->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            ComPtr<IMMDevice> d;
            if (FAILED(col->Item(i, &d))) continue;
            Endpoint e; e.flow = flow;
            LPWSTR id = nullptr;
            if (SUCCEEDED(d->GetId(&id))) { e.id = id; CoTaskMemFree(id); }
            d->GetState(&e.state);
            ComPtr<IPropertyStore> ps;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                e.desc = PropStr(ps.Get(), PKEY_Device_DeviceDesc_);
                e.friendly = PropStr(ps.Get(), PKEY_Device_FriendlyName);
                e.adapter = PropStr(ps.Get(), PKEY_DeviceInterface_FriendlyName);
            }
            out.push_back(e);
        }
    }
    return out;
}

static const wchar_t* StateName(DWORD s) {
    switch (s) {
    case DEVICE_STATE_ACTIVE: return L"active";
    case DEVICE_STATE_DISABLED: return L"disabled";
    case DEVICE_STATE_NOTPRESENT: return L"not present";
    case DEVICE_STATE_UNPLUGGED: return L"unplugged";
    default: return L"?";
    }
}

// Rename: documented store first, IPolicyConfig second.
static HRESULT Rename(IMMDeviceEnumerator* en, IPolicyConfig* pc, const Endpoint& e, const wchar_t* name) {
    HRESULT hr = E_FAIL;
    ComPtr<IMMDevice> d;
    if (SUCCEEDED(en->GetDevice(e.id.c_str(), &d))) {
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READWRITE, &ps))) {
            PROPVARIANT v; InitPropVariantFromString(name, &v);
            hr = ps->SetValue(PKEY_Device_DeviceDesc_, v);
            if (SUCCEEDED(hr)) hr = ps->Commit();
            PropVariantClear(&v);
        }
    }
    if (FAILED(hr) && pc) {
        PROPVARIANT v; InitPropVariantFromString(name, &v);
        hr = pc->SetPropertyValue(e.id.c_str(), FALSE, PKEY_Device_DeviceDesc_, &v);
        PropVariantClear(&v);
    }
    return hr;
}

static HRESULT SetEnabled(IPolicyConfig* pc, const Endpoint& e, bool on) {
    if (!pc) return E_NOINTERFACE;
    return pc->SetEndpointVisibility(e.id.c_str(), on ? TRUE : FALSE);
}

static void PinRenderId(const wchar_t* id) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\CatCam", 0, nullptr, 0,
            KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        if (id) RegSetValueExW(hk, L"CableRenderId", 0, REG_SZ, (const BYTE*)id,
            (DWORD)((wcslen(id) + 1) * sizeof(wchar_t)));
        else RegDeleteValueW(hk, L"CableRenderId");
        RegCloseKey(hk);
    }
}

// Per-endpoint memory: HKLM\SOFTWARE\CatCam\AudioOriginal\<endpoint id>
//   Desc (REG_SZ)      the DeviceDesc before setup touched it
//   Disabled (REG_DWORD) 1 = setup disabled this endpoint (restore re-enables)
static const wchar_t* MEM_KEY = L"SOFTWARE\\CatCam\\AudioOriginal";

static bool MemGet(const std::wstring& id, std::wstring* desc, bool* weDisabled) {
    std::wstring sub = std::wstring(MEM_KEY) + L"\\" + id;
    wchar_t buf[512] = L""; DWORD cb = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, sub.c_str(), L"Desc", RRF_RT_REG_SZ, nullptr, buf, &cb) != ERROR_SUCCESS)
        return false;
    *desc = buf;
    DWORD d = 0; cb = sizeof(d);
    *weDisabled = RegGetValueW(HKEY_LOCAL_MACHINE, sub.c_str(), L"Disabled", RRF_RT_REG_DWORD, nullptr, &d, &cb) == ERROR_SUCCESS && d != 0;
    return true;
}
static void MemPut(const std::wstring& id, const std::wstring& desc, bool weDisabled) {
    std::wstring sub = std::wstring(MEM_KEY) + L"\\" + id;
    HKEY hk;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(hk, L"Desc", 0, REG_SZ, (const BYTE*)desc.c_str(), (DWORD)((desc.size() + 1) * sizeof(wchar_t)));
    DWORD d = weDisabled ? 1 : 0;
    RegSetValueExW(hk, L"Disabled", 0, REG_DWORD, (const BYTE*)&d, sizeof(d));
    RegCloseKey(hk);
}
static void MemDel(const std::wstring& id) {
    std::wstring sub = std::wstring(MEM_KEY) + L"\\" + id;
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, sub.c_str());
}

// audio.log next to the exe: every setup/restore line, timestamped.
static FILE* g_log = nullptr;
static void LogOpen() {
    wchar_t mod[MAX_PATH]; GetModuleFileNameW(nullptr, mod, MAX_PATH);
    wchar_t* sl = wcsrchr(mod, L'\\'); if (sl) *sl = 0;
    std::wstring path = std::wstring(mod) + L"\\audio.log";
    _wfopen_s(&g_log, path.c_str(), L"a, ccs=UTF-8");
}
static void Say(const wchar_t* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vwprintf(fmt, ap);
    va_end(ap);
    if (g_log) {
        SYSTEMTIME st; GetLocalTime(&st);
        fwprintf(g_log, L"[%04u-%02u-%02u %02u:%02u:%02u] ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        va_start(ap, fmt); vfwprintf(g_log, fmt, ap); va_end(ap);
        fflush(g_log);
    }
}

// mic-setup.ps1 records who installed the cable. Missing = not ours.
static bool CableIsOurs() {
    DWORD v = 0, cb = sizeof(v);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\CatCam", L"CableInstalledByCatCam",
        RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS && v == 1;
}

static const wchar_t* MIC_NAME  = L"CatCam Microphone";
static const wchar_t* FEED_NAME = L"CatCam Mic Feed";
static const wchar_t* VB_ADAPTER = L"VB-Audio Virtual Cable";

static bool IsCable(const Endpoint& e) {
    return ContainsI(e.adapter, VB_ADAPTER) || ContainsI(e.friendly, VB_ADAPTER);
}
static bool Is16ch(const Endpoint& e) {
    // The 16-channel cable is a separate adapter ("VB-Audio Virtual Cable
    // 16 Ch"?) or endpoint desc ("CABLE In 16ch"); match either.
    return ContainsI(e.desc, L"16ch") || ContainsI(e.desc, L"16 ch")
        || ContainsI(e.adapter, L"16 ch") || ContainsI(e.adapter, L"16ch");
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* verb = argc > 1 ? argv[1] : L"list";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
        wprintf(L"[ERR] MMDeviceEnumerator unavailable\n"); return 2;
    }
    ComPtr<IPolicyConfig> pc;
    CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL, IID_IPolicyConfig, (void**)pc.GetAddressOf());

    auto eps = Enumerate(en.Get());

    if (!_wcsicmp(verb, L"list")) {
        for (auto& e : eps)
            wprintf(L"%-7s %-11s %s\n        desc='%s' adapter='%s'\n        %s\n",
                e.flow == eRender ? L"render" : L"capture", StateName(e.state),
                e.friendly.c_str(), e.desc.c_str(), e.adapter.c_str(), e.id.c_str());
        return 0;
    }

    // Diagnostics: rename any endpoint by ID (what the Sound panel's Rename
    // box does), no memory involved. "CatCamAudio rename <id> <new name>".
    if (!_wcsicmp(verb, L"rename") && argc >= 4) {
        for (auto& e : eps) if (e.id == argv[2]) {
            HRESULT hr = Rename(en.Get(), pc.Get(), e, argv[3]);
            wprintf(L"rename '%s' -> '%s': 0x%08lx\n", e.desc.c_str(), argv[3], hr);
            return FAILED(hr) ? 4 : 0;
        }
        wprintf(L"no endpoint with that id\n"); return 3;
    }
    // Diagnostics: "CatCamAudio enable|disable <id>" (the Sound panel's
    // Enable/Disable), no memory involved.
    if ((!_wcsicmp(verb, L"enable") || !_wcsicmp(verb, L"disable")) && argc >= 3) {
        for (auto& e : eps) if (e.id == argv[2]) {
            HRESULT hr = SetEnabled(pc.Get(), e, !_wcsicmp(verb, L"enable"));
            wprintf(L"%s '%s': 0x%08lx\n", verb, e.friendly.c_str(), hr);
            return FAILED(hr) ? 4 : 0;
        }
        wprintf(L"no endpoint with that id\n"); return 3;
    }

    const bool setup = !_wcsicmp(verb, L"setup");
    const bool restore = !_wcsicmp(verb, L"restore");
    if (!setup && !restore) {
        wprintf(L"usage: CatCamAudio list | setup [--force] | restore | rename <id> <name> | enable|disable <id>\n"); return 1;
    }
    const bool force = argc > 2 && !_wcsicmp(argv[2], L"--force");
    LogOpen();
    Say(L"%s%s\n", verb, force ? L" --force" : L"");

    // Rename/disable only a cable that is CatCam's (or on --force). A cable
    // the user already had keeps its names and its 16ch; we still pin.
    const bool mayName = setup && (force || CableIsOurs());
    if (setup && !mayName)
        Say(L"VB-Cable was not installed by CatCam: names left alone (pick 'CABLE Output' as microphone, or run 'CatCamAudio setup --force')\n");

    int cables = 0, failures = 0, changed = 0;
    std::wstring renderId;
    for (auto& e : eps) {
        if (!IsCable(e)) continue;
        // Ghosts (VB reinstall leaves NOTPRESENT twins with the same names)
        // are never touched and never pinned; restore still sees DISABLED
        // ones so it can re-enable what it disabled.
        const bool real = e.state == DEVICE_STATE_ACTIVE || (restore && e.state == DEVICE_STATE_DISABLED);
        if (!real) continue;
        cables++;
        const wchar_t* kind = e.flow == eRender ? L"render" : L"capture";
        std::wstring orig; bool weDisabled = false;
        const bool remembered = MemGet(e.id, &orig, &weDisabled);
        HRESULT hr = S_OK;
        if (setup) {
            if (Is16ch(e)) {
                // Unused sibling: out of the pickers. Only if it is active
                // now (something already disabled = not ours to touch), and
                // remembered so restore re-enables exactly this.
                if (mayName && e.state == DEVICE_STATE_ACTIVE && !remembered) {
                    hr = SetEnabled(pc.Get(), e, false);
                    if (SUCCEEDED(hr)) { changed++; MemPut(e.id, e.desc, true); }
                    Say(L"disable 16ch %s: %s (0x%08lx)\n", kind, e.friendly.c_str(), hr);
                }
            } else {
                if (e.flow == eRender) renderId = e.id;   // pin: the active 2ch render endpoint
                const wchar_t* want = e.flow == eCapture ? MIC_NAME : FEED_NAME;
                // Rename on first touch, or when the name went back to what
                // we remembered (driver reinstall/reset). A name the user
                // gave it since (neither ours nor the remembered one) stays:
                // this runs at every logon and must not fight the user.
                const bool touch = mayName && e.desc != want && (!remembered || e.desc == orig);
                if (touch) {
                    if (!remembered) MemPut(e.id, e.desc, false);   // first touch: remember theirs
                    hr = Rename(en.Get(), pc.Get(), e, want);
                    if (SUCCEEDED(hr)) changed++;
                    Say(L"rename %s '%s' -> '%s': 0x%08lx\n", kind, e.desc.c_str(), want, hr);
                }
            }
        } else { // restore: only what setup remembered
            if (!remembered) continue;
            if (weDisabled) {
                hr = SetEnabled(pc.Get(), e, true);
                Say(L"re-enable %s: %s (0x%08lx)\n", kind, e.friendly.c_str(), hr);
            } else if (e.desc != orig) {
                hr = Rename(en.Get(), pc.Get(), e, orig.c_str());
                Say(L"rename %s '%s' -> '%s': 0x%08lx\n", kind, e.desc.c_str(), orig.c_str(), hr);
            }
            if (SUCCEEDED(hr)) { MemDel(e.id); changed++; }
        }
        if (FAILED(hr)) failures++;
    }
    if (restore) PinRenderId(nullptr);   // even if the cable is already gone
    if (!cables) { Say(L"no active VB-Audio Virtual Cable endpoints (VB-Cable not installed, or not until the next reboot)\n"); return 3; }
    // No active 2ch render endpoint seen this run: keep whatever pin exists
    // (a transient state must not erase a good pin; the host checks ACTIVE
    // itself and falls back by name).
    if (setup && !renderId.empty()) PinRenderId(renderId.c_str());
    Say(L"%s: %d cable endpoints, %d changed, %d failures%s\n", verb, cables, changed, failures,
        setup && !renderId.empty() ? L", render endpoint pinned for the host" : L"");
    return failures ? 4 : 0;
}
