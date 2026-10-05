// SmartDial - turn the dial on CMF Buds Pro 2 (or similar earbuds) into seek / scroll / reels controls.
// Native Win32 desktop app: settings window, tray icon, global hotkeys. No runtime needed.
//
// Build: run build.bat (needs MinGW-w64 g++ and windres on PATH).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0A00
#endif

#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <commctrl.h>
#include <shellapi.h>
#include <cmath>
#include <cwctype>
#include <string>

// ============================================================================
// Constants
// ============================================================================
enum Mode { MODE_VOLUME = 0, MODE_SEEK, MODE_SCROLL, MODE_REELS };

enum {
    ID_MODE_VOLUME = 100, ID_MODE_SEEK, ID_MODE_SCROLL, ID_MODE_REELS,   // consecutive on purpose
    ID_HK_SEEK = 110, ID_HK_SCROLL, ID_HK_REELS,
    ID_USECUR = 130,
    ID_DEVICE = 120, ID_NOTCH, ID_NOTCH_SPIN, ID_REVERSE, ID_CENTER, ID_NOTIFY, ID_AUTOSTART, ID_CLOSETRAY,
    ID_STATUS = 140,
    ID_TRAY_OPEN = 200, ID_TRAY_MODE0 = 210, ID_TRAY_EXIT = 220
};

#define HKID_SEEK   1
#define HKID_SCROLL 2
#define HKID_REELS  3

#define WM_TRAY     (WM_APP + 1)
#define WM_DIAL     (WM_APP + 2)
#define WM_AUDIODEV (WM_APP + 3)
#define WM_HKDONE   (WM_APP + 4)
#define TIMER_DEV   1

static const wchar_t* CLS_NAME = L"SmartDialMainWnd";
static const float    PARK     = 0.5f;   // volume parked here while a dial mode is active

// Tag for our own volume writes so their notifications can be ignored.
static const GUID CTX = {0x5ee0d1a1, 0x1b2c, 0x4c3d,
                         {0x9a, 0x8b, 0x7c, 0x6d, 0x5e, 0x4f, 0x3a, 0x2b}};

// ============================================================================
// Settings
// ============================================================================
static const WORD DEF_HK_SEEK   = MAKEWORD('S', HOTKEYF_CONTROL | HOTKEYF_ALT);
static const WORD DEF_HK_SCROLL = MAKEWORD('D', HOTKEYF_CONTROL | HOTKEYF_ALT);
static const WORD DEF_HK_REELS  = MAKEWORD('R', HOTKEYF_CONTROL | HOTKEYF_ALT);

struct Config {
    std::wstring deviceMatch   = L"CMF, Buds";
    int          scrollNotches = 3;
    bool         reverseScroll = false;   // false: dial up = scroll down
    bool         keepCentered  = true;    // re-center volume after every click
    bool         notifications = true;    // silent tray balloons on mode change
    bool         closeToTray   = true;
    WORD         hkSeek   = DEF_HK_SEEK;
    WORD         hkScroll = DEF_HK_SCROLL;
    WORD         hkReels  = DEF_HK_REELS;
};

// ============================================================================
// Globals
// ============================================================================
static HINSTANCE g_hInst;
static HWND      g_hwnd;
static int       g_dpi = 96;
static HFONT     g_font;
static bool      g_loading;          // true while we fill the controls (ignore their notifications)
static UINT      g_msgShow, g_msgTaskbar;
static std::wstring g_ini;
static Config    g_cfg;

static IMMDeviceEnumerator*  g_enum;
static IAudioEndpointVolume* g_vol;
static Mode          g_mode = MODE_VOLUME;   // mode the user asked for
static bool          g_active;               // attached to the buds and volume parked
static float         g_saved = 0.5f;         // real volume to restore
static float         g_ref   = 0.5f;         // volume the next change is compared against
static std::wstring  g_devName, g_devId, g_warn;
static ULONGLONG     g_ignoreUntil;
static int           g_clicks;               // dial clicks handled since activation (shown in status)
static HHOOK         g_hook;
static NOTIFYICONDATAW g_nid;
static bool          g_toldTray;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t");
    return s.substr(a, b - a + 1);
}

static const wchar_t* ModeName(Mode m) {
    switch (m) {
    case MODE_SEEK:   return L"Seek";
    case MODE_SCROLL: return L"Scroll";
    case MODE_REELS:  return L"Reels";
    default:          return L"Volume";
    }
}

// ============================================================================
// Audio: volume-change and device-change notifications
// ============================================================================
class VolCb : public IAudioEndpointVolumeCallback {
    LONG ref = 1;
public:
    virtual ~VolCb() {}
    STDMETHODIMP QueryInterface(REFIID r, void** p) override {
        if (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, IID_IAudioEndpointVolumeCallback)) {
            *p = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG n = InterlockedDecrement(&ref);
        if (n == 0) delete this;
        return n;
    }
    STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA d) override {
        // Audio thread: never touch the volume here, hand off to the UI thread.
        if (d && !IsEqualGUID(d->guidEventContext, CTX))
            PostMessageW(g_hwnd, WM_DIAL, 0, (LPARAM)(d->fMasterVolume * 10000.0f + 0.5f));
        return S_OK;
    }
};
static VolCb* g_cb;

class DevNotify : public IMMNotificationClient {
    LONG ref = 1;
    static void Poke() { if (g_hwnd) PostMessageW(g_hwnd, WM_AUDIODEV, 0, 0); }
public:
    virtual ~DevNotify() {}
    STDMETHODIMP QueryInterface(REFIID r, void** p) override {
        if (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, IID_IMMNotificationClient)) {
            *p = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG n = InterlockedDecrement(&ref);
        if (n == 0) delete this;
        return n;
    }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { Poke(); return S_OK; }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { Poke(); return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { Poke(); return S_OK; }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow f, ERole, LPCWSTR) override {
        if (f == eRender) Poke();
        return S_OK;
    }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};
static DevNotify* g_notif;

static std::wstring FriendlyName(IMMDevice* dev) {
    std::wstring out;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
            out = v.pwszVal;
        PropVariantClear(&v);
        ps->Release();
    }
    return out;
}

static std::wstring DeviceId(IMMDevice* dev) {
    std::wstring out;
    LPWSTR id = nullptr;
    if (SUCCEEDED(dev->GetId(&id)) && id) { out = id; CoTaskMemFree(id); }
    return out;
}

static std::wstring CurrentDefaultId() {
    std::wstring id;
    IMMDevice* dev = nullptr;
    if (g_enum && SUCCEEDED(g_enum->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) {
        id = DeviceId(dev);
        dev->Release();
    }
    return id;
}

static std::wstring CurrentDefaultName() {
    std::wstring name;
    IMMDevice* dev = nullptr;
    if (g_enum && SUCCEEDED(g_enum->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) {
        name = FriendlyName(dev);
        dev->Release();
    }
    return name;
}

static bool NameMatches(const std::wstring& name) {
    std::wstring n = Lower(name), list = Lower(g_cfg.deviceMatch);
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t c = list.find(L',', pos);
        std::wstring tok = Trim(list.substr(pos, c == std::wstring::npos ? std::wstring::npos : c - pos));
        if (!tok.empty() && n.find(tok) != std::wstring::npos) return true;
        if (c == std::wstring::npos) break;
        pos = c + 1;
    }
    return false;
}

// ============================================================================
// Activate / deactivate (park the volume so the dial always has room to move)
// ============================================================================
static bool Activate() {
    IMMDevice* dev = nullptr;
    if (!g_enum || FAILED(g_enum->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) return false;
    std::wstring name = FriendlyName(dev);
    if (!NameMatches(name)) { dev->Release(); return false; }
    IAudioEndpointVolume* vol = nullptr;
    HRESULT hr = dev->Activate(IID_IAudioEndpointVolume, CLSCTX_ALL, nullptr, (void**)&vol);
    if (FAILED(hr) || !vol) { dev->Release(); return false; }
    g_devName = name;
    g_devId = DeviceId(dev);
    dev->Release();

    g_vol = vol;
    g_cb = new VolCb();
    g_vol->RegisterControlChangeNotify(g_cb);
    g_vol->GetMasterVolumeLevelScalar(&g_saved);
    g_vol->SetMasterVolumeLevelScalar(PARK, &CTX);
    g_ref = PARK;
    g_clicks = 0;
    g_active = true;
    return true;
}

static void Deactivate() {
    if (!g_active) return;
    if (g_vol) {
        g_vol->SetMasterVolumeLevelScalar(g_saved, &CTX);   // may fail if the device is gone; fine
        if (g_cb) g_vol->UnregisterControlChangeNotify(g_cb);
        g_vol->Release();
        g_vol = nullptr;
    }
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    g_active = false;
    g_devName.clear();
    g_devId.clear();
}

// ============================================================================
// Input injection
// ============================================================================
static void SendArrow(WORD vk) {
    INPUT in[2] = {};
    for (int i = 0; i < 2; i++) {
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = vk;
        in[i].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | (i ? KEYEVENTF_KEYUP : 0);
    }
    SendInput(2, in, sizeof(INPUT));
}

// positive = up, negative = down; delivered to the window under the cursor
static void SendWheel(int notches) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = (DWORD)(notches * WHEEL_DELTA);
    SendInput(1, &in, sizeof(INPUT));
}

static void DoAction(bool dialUp) {
    bool down = (dialUp != g_cfg.reverseScroll);   // default: dial up = move forward / down
    switch (g_mode) {
    case MODE_SEEK:
        SendArrow(dialUp ? VK_RIGHT : VK_LEFT);
        break;
    case MODE_SCROLL:
        SendWheel(down ? -g_cfg.scrollNotches : g_cfg.scrollNotches);
        break;
    case MODE_REELS:
        SendArrow(down ? VK_DOWN : VK_UP);
        break;
    default:
        break;
    }
}

// Keyboard volume keys are real volume changes, not dial turns.
static LRESULT CALLBACK KbHook(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        DWORD vk = ((KBDLLHOOKSTRUCT*)l)->vkCode;
        if (vk == VK_VOLUME_UP || vk == VK_VOLUME_DOWN || vk == VK_VOLUME_MUTE)
            g_ignoreUntil = GetTickCount64() + 400;
    }
    return CallNextHookEx(nullptr, code, w, l);
}

// ============================================================================
// Persistence (INI in %APPDATA%\SmartDial) and autostart (HKCU Run key)
// ============================================================================
static int GetI(const wchar_t* k, int def) {
    return (int)GetPrivateProfileIntW(L"Settings", k, def, g_ini.c_str());
}
static void PutI(const wchar_t* k, int v) {
    WritePrivateProfileStringW(L"Settings", k, std::to_wstring(v).c_str(), g_ini.c_str());
}

static void InitIniPath() {
    wchar_t dir[MAX_PATH] = L"";
    DWORD n = GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        wchar_t* slash = wcsrchr(dir, L'\\');
        if (slash) *slash = 0;
        g_ini = std::wstring(dir) + L"\\SmartDial.ini";
        return;
    }
    std::wstring folder = std::wstring(dir) + L"\\SmartDial";
    CreateDirectoryW(folder.c_str(), nullptr);
    g_ini = folder + L"\\settings.ini";
}

static void LoadConfig() {
    wchar_t buf[256];
    GetPrivateProfileStringW(L"Settings", L"DeviceMatch", L"CMF, Buds", buf, 256, g_ini.c_str());
    g_cfg.deviceMatch   = buf;
    g_cfg.scrollNotches = std::max(1, std::min(20, GetI(L"ScrollNotches", 3)));
    g_cfg.reverseScroll = GetI(L"ReverseScroll", 0) != 0;
    g_cfg.keepCentered  = GetI(L"KeepCentered", 1) != 0;
    g_cfg.notifications = GetI(L"Notifications", 1) != 0;
    g_cfg.closeToTray   = GetI(L"CloseToTray", 1) != 0;
    g_cfg.hkSeek   = (WORD)GetI(L"HotkeySeek",   DEF_HK_SEEK);
    g_cfg.hkScroll = (WORD)GetI(L"HotkeyScroll", DEF_HK_SCROLL);
    g_cfg.hkReels  = (WORD)GetI(L"HotkeyReels",  DEF_HK_REELS);
    if (!LOBYTE(g_cfg.hkSeek))   g_cfg.hkSeek   = DEF_HK_SEEK;     // an empty hotkey is never useful
    if (!LOBYTE(g_cfg.hkScroll)) g_cfg.hkScroll = DEF_HK_SCROLL;
    if (!LOBYTE(g_cfg.hkReels))  g_cfg.hkReels  = DEF_HK_REELS;
}

static void SaveConfig() {
    WritePrivateProfileStringW(L"Settings", L"DeviceMatch", g_cfg.deviceMatch.c_str(), g_ini.c_str());
    PutI(L"ScrollNotches", g_cfg.scrollNotches);
    PutI(L"ReverseScroll", g_cfg.reverseScroll);
    PutI(L"KeepCentered",  g_cfg.keepCentered);
    PutI(L"Notifications", g_cfg.notifications);
    PutI(L"CloseToTray",   g_cfg.closeToTray);
    PutI(L"HotkeySeek",    g_cfg.hkSeek);
    PutI(L"HotkeyScroll",  g_cfg.hkScroll);
    PutI(L"HotkeyReels",   g_cfg.hkReels);
}

static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static bool IsAutostart() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool r = RegQueryValueExW(k, L"SmartDial", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return r;
}

static void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(path) + L"\" /minimized";
        RegSetValueExW(k, L"SmartDial", 0, REG_SZ, (const BYTE*)v.c_str(),
                       (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"SmartDial");
    }
    RegCloseKey(k);
}

// ============================================================================
// Tray
// ============================================================================
static void UpdateTray() {
    if (!g_nid.hWnd) return;
    std::wstring tip = L"SmartDial \u2013 ";
    tip += ModeName(g_mode);
    if (g_mode != MODE_VOLUME && !g_active) tip += L" (waiting for earbuds)";
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_TIP;
    lstrcpynW(n.szTip, tip.c_str(), ARRAYSIZE(n.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// Silent balloon: NIIF_NOSOUND is what stops the Windows notification beep.
static void Notify(const std::wstring& text) {
    if (!g_cfg.notifications || !g_nid.hWnd) return;
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    lstrcpynW(n.szInfoTitle, L"SmartDial", ARRAYSIZE(n.szInfoTitle));
    lstrcpynW(n.szInfo, text.c_str(), ARRAYSIZE(n.szInfo));
    n.dwInfoFlags = NIIF_NONE | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void AddTray() {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(g_nid.szTip, L"SmartDial", ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void ShowMain() {
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
}

// ============================================================================
// UI state
// ============================================================================
static void UpdateUI() {
    if (!g_hwnd) return;
    CheckRadioButton(g_hwnd, ID_MODE_VOLUME, ID_MODE_REELS, ID_MODE_VOLUME + (int)g_mode);
    std::wstring s;
    if (g_mode == MODE_VOLUME)
        s = L"Volume mode \u2013 the dial controls volume as usual.";
    else if (g_active)
        s = std::wstring(L"Active: ") + ModeName(g_mode) + L" mode on " + g_devName +
            L"  (dial clicks seen: " + std::to_wstring(g_clicks) + L")";
    else {
        std::wstring cur = CurrentDefaultName();
        s = L"Waiting \u2013 the default output is \"" + (cur.empty() ? std::wstring(L"(none)") : cur) +
            L"\", which doesn't match \"" + g_cfg.deviceMatch + L"\". Set your earbuds as the default output, then click \"Use current\".";
    }
    if (!g_warn.empty()) s += L"\n" + g_warn;
    SetDlgItemTextW(g_hwnd, ID_STATUS, s.c_str());
    UpdateTray();
}

// Make the audio state match the requested mode and settings.
static void Apply() {
    if (g_active && (g_mode == MODE_VOLUME || !NameMatches(g_devName))) Deactivate();
    if (!g_active && g_mode != MODE_VOLUME) Activate();
    UpdateUI();
}

static void SetMode(Mode m) {
    g_mode = m;
    Apply();
    if (m == MODE_VOLUME) Notify(L"Volume mode");
    else if (g_active)    Notify(std::wstring(ModeName(m)) + L" mode");
    else                  Notify(std::wstring(ModeName(m)) + L" mode \u2013 waiting for earbuds");
}

static void ToggleMode(Mode m) { SetMode(g_mode == m ? MODE_VOLUME : m); }

static void HandleDeviceChange() {
    if (g_active && CurrentDefaultId() != g_devId) Deactivate();
    if (!g_active && g_mode != MODE_VOLUME) {
        if (Activate()) Notify(std::wstring(ModeName(g_mode)) + L" mode active");
    }
    UpdateUI();
}

// ============================================================================
// Hotkeys
// ============================================================================
static UINT ToMod(WORD hk) {
    BYTE m = HIBYTE(hk);
    UINT r = MOD_NOREPEAT;
    if (m & HOTKEYF_CONTROL) r |= MOD_CONTROL;
    if (m & HOTKEYF_ALT)     r |= MOD_ALT;
    if (m & HOTKEYF_SHIFT)   r |= MOD_SHIFT;
    return r;
}

static void UnregisterHotkeys() {
    UnregisterHotKey(g_hwnd, HKID_SEEK);
    UnregisterHotKey(g_hwnd, HKID_SCROLL);
    UnregisterHotKey(g_hwnd, HKID_REELS);
}

static void RegisterHotkeys() {
    UnregisterHotkeys();
    g_warn.clear();
    struct { int id; WORD hk; const wchar_t* name; } list[] = {
        {HKID_SEEK,   g_cfg.hkSeek,   L"Seek"},
        {HKID_SCROLL, g_cfg.hkScroll, L"Scroll"},
        {HKID_REELS,  g_cfg.hkReels,  L"Reels"},
    };
    for (auto& e : list) {
        if (!LOBYTE(e.hk)) continue;
        if (!RegisterHotKey(g_hwnd, e.id, ToMod(e.hk), LOBYTE(e.hk)))
            g_warn += std::wstring(e.name) + L" hotkey is in use by another app or mode. ";
    }
}

// While a hotkey box has focus, release the global hotkeys so combos can be recorded.
static LRESULT CALLBACK HkSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (m == WM_SETFOCUS)        UnregisterHotkeys();
    else if (m == WM_KILLFOCUS)  PostMessageW(g_hwnd, WM_HKDONE, 0, 0);
    else if (m == WM_NCDESTROY)  RemoveWindowSubclass(h, HkSub, id);
    return DefSubclassProc(h, m, w, l);
}

// ============================================================================
// Controls
// ============================================================================
static HWND Mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
               int id, DWORD ex = 0) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                           g_hwnd, (HMENU)(INT_PTR)id, g_hInst, nullptr);
}

static BOOL CALLBACK SetFontProc(HWND c, LPARAM f) {
    SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    return TRUE;
}

static void CreateControls() {
    // ---- Dial mode ----
    Mk(L"BUTTON", L"Dial mode", BS_GROUPBOX, 12, 8, 416, 132, 0);
    Mk(L"BUTTON", L"Volume  \u2013  the dial controls volume as usual",
       BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 28, 30, 386, 20, ID_MODE_VOLUME);
    Mk(L"BUTTON", L"Seek  \u2013  \u2190 / \u2192  back and forward in videos",
       BS_AUTORADIOBUTTON, 28, 55, 386, 20, ID_MODE_SEEK);
    Mk(L"BUTTON", L"Scroll  \u2013  mouse wheel, for feeds and pages",
       BS_AUTORADIOBUTTON, 28, 80, 386, 20, ID_MODE_SCROLL);
    Mk(L"BUTTON", L"Reels / Shorts  \u2013  \u2191 / \u2193  previous and next video",
       BS_AUTORADIOBUTTON, 28, 105, 386, 20, ID_MODE_REELS);

    // ---- Hotkeys ----
    Mk(L"BUTTON", L"Hotkeys (press again to return to volume)", BS_GROUPBOX | WS_GROUP, 12, 148, 416, 124, 0);
    const wchar_t* names[3] = {L"Seek mode", L"Scroll mode", L"Reels mode"};
    int ids[3] = {ID_HK_SEEK, ID_HK_SCROLL, ID_HK_REELS};
    for (int i = 0; i < 3; i++) {
        int y = 174 + i * 30;
        Mk(L"STATIC", names[i], SS_LEFT, 28, y + 3, 100, 18, 0);
        HWND hk = Mk(HOTKEY_CLASSW, L"", WS_TABSTOP, 140, y, 190, 24, ids[i], WS_EX_CLIENTEDGE);
        SendMessageW(hk, HKM_SETRULES, HKCOMB_NONE | HKCOMB_S,
                     MAKELPARAM(HOTKEYF_CONTROL | HOTKEYF_ALT, 0));
        SetWindowSubclass(hk, HkSub, (UINT_PTR)ids[i], 0);
    }

    // ---- Options ----
    Mk(L"BUTTON", L"Options", BS_GROUPBOX | WS_GROUP, 12, 280, 416, 218, 0);
    Mk(L"STATIC", L"Earbuds name contains:", SS_LEFT, 28, 308, 140, 18, 0);
    Mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 172, 305, 150, 24, ID_DEVICE, WS_EX_CLIENTEDGE);
    Mk(L"BUTTON", L"Use current", BS_PUSHBUTTON | WS_TABSTOP, 328, 304, 92, 26, ID_USECUR);
    Mk(L"STATIC", L"Scroll distance (wheel notches per click):", SS_LEFT, 28, 340, 230, 18, 0);
    HWND ed = Mk(L"EDIT", L"3", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 262, 337, 60, 24, ID_NOTCH, WS_EX_CLIENTEDGE);
    HWND ud = Mk(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                 0, 0, 0, 0, ID_NOTCH_SPIN);
    SendMessageW(ud, UDM_SETBUDDY, (WPARAM)ed, 0);
    SendMessageW(ud, UDM_SETRANGE32, 1, 20);
    Mk(L"BUTTON", L"Reverse scroll direction (dial up = scroll up)", BS_AUTOCHECKBOX | WS_TABSTOP, 28, 372, 386, 20, ID_REVERSE);
    Mk(L"BUTTON", L"Keep volume steady (re-center after each click)", BS_AUTOCHECKBOX | WS_TABSTOP, 28, 396, 386, 20, ID_CENTER);
    Mk(L"BUTTON", L"Show mode notifications (silent)", BS_AUTOCHECKBOX | WS_TABSTOP, 28, 420, 386, 20, ID_NOTIFY);
    Mk(L"BUTTON", L"Start SmartDial with Windows", BS_AUTOCHECKBOX | WS_TABSTOP, 28, 444, 386, 20, ID_AUTOSTART);
    Mk(L"BUTTON", L"Closing the window keeps SmartDial running in the tray", BS_AUTOCHECKBOX | WS_TABSTOP, 28, 468, 386, 20, ID_CLOSETRAY);

    // ---- Status ----
    Mk(L"STATIC", L"", SS_LEFT, 14, 506, 412, 58, ID_STATUS);

    EnumChildWindows(g_hwnd, SetFontProc, (LPARAM)g_font);
}

static void ControlsFromConfig() {
    g_loading = true;
    SetDlgItemTextW(g_hwnd, ID_DEVICE, g_cfg.deviceMatch.c_str());
    SetDlgItemInt(g_hwnd, ID_NOTCH, g_cfg.scrollNotches, FALSE);
    CheckDlgButton(g_hwnd, ID_REVERSE,   g_cfg.reverseScroll ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hwnd, ID_CENTER,    g_cfg.keepCentered  ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hwnd, ID_NOTIFY,    g_cfg.notifications ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hwnd, ID_CLOSETRAY, g_cfg.closeToTray   ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hwnd, ID_AUTOSTART, IsAutostart()       ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(g_hwnd, ID_HK_SEEK,   HKM_SETHOTKEY, g_cfg.hkSeek,   0);
    SendDlgItemMessageW(g_hwnd, ID_HK_SCROLL, HKM_SETHOTKEY, g_cfg.hkScroll, 0);
    SendDlgItemMessageW(g_hwnd, ID_HK_REELS,  HKM_SETHOTKEY, g_cfg.hkReels,  0);
    g_loading = false;
}

static void ConfigFromControls() {
    wchar_t buf[256];
    GetDlgItemTextW(g_hwnd, ID_DEVICE, buf, 256);
    g_cfg.deviceMatch = buf;
    BOOL ok = FALSE;
    UINT n = GetDlgItemInt(g_hwnd, ID_NOTCH, &ok, FALSE);
    if (ok) g_cfg.scrollNotches = (int)std::max(1u, std::min(20u, n));
    g_cfg.reverseScroll = IsDlgButtonChecked(g_hwnd, ID_REVERSE)   == BST_CHECKED;
    g_cfg.keepCentered  = IsDlgButtonChecked(g_hwnd, ID_CENTER)    == BST_CHECKED;
    g_cfg.notifications = IsDlgButtonChecked(g_hwnd, ID_NOTIFY)    == BST_CHECKED;
    g_cfg.closeToTray   = IsDlgButtonChecked(g_hwnd, ID_CLOSETRAY) == BST_CHECKED;
}

static void ReadHotkeys() {
    WORD* target[3] = {&g_cfg.hkSeek, &g_cfg.hkScroll, &g_cfg.hkReels};
    int   ids[3]    = {ID_HK_SEEK, ID_HK_SCROLL, ID_HK_REELS};
    WORD  defs[3]   = {DEF_HK_SEEK, DEF_HK_SCROLL, DEF_HK_REELS};
    for (int i = 0; i < 3; i++) {
        WORD v = (WORD)SendDlgItemMessageW(g_hwnd, ids[i], HKM_GETHOTKEY, 0, 0);
        if (!LOBYTE(v)) {                                   // cleared box -> back to the default
            v = defs[i];
            SendDlgItemMessageW(g_hwnd, ids[i], HKM_SETHOTKEY, v, 0);
        }
        *target[i] = v;
    }
}

// ============================================================================
// Tray menu
// ============================================================================
static void TrayCommand(int cmd) {
    if (cmd == ID_TRAY_OPEN) ShowMain();
    else if (cmd >= ID_TRAY_MODE0 && cmd <= ID_TRAY_MODE0 + 3) SetMode((Mode)(cmd - ID_TRAY_MODE0));
    else if (cmd == ID_TRAY_EXIT) DestroyWindow(g_hwnd);
}

static void ShowTrayMenu() {
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    const wchar_t* labels[4] = {L"Volume (normal)", L"Seek (\u2190 \u2192)", L"Scroll (wheel)", L"Reels (\u2191 \u2193)"};
    AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"Open SmartDial");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < 4; i++)
        AppendMenuW(m, MF_STRING | (g_mode == (Mode)i ? MF_CHECKED : 0), ID_TRAY_MODE0 + i, labels[i]);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"Exit");
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    if (cmd) TrayCommand(cmd);
}

// ============================================================================
// Window procedure
// ============================================================================
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_msgShow) { ShowMain(); return 0; }
    if (g_msgTaskbar && m == g_msgTaskbar) { Shell_NotifyIconW(NIM_ADD, &g_nid); UpdateTray(); return 0; }

    switch (m) {
    case WM_CREATE:
        g_hwnd = h;
        CreateControls();
        ControlsFromConfig();
        AddTray();
        RegisterHotkeys();
        UpdateUI();
        return 0;

    case WM_HOTKEY:
        if (w == HKID_SEEK)        ToggleMode(MODE_SEEK);
        else if (w == HKID_SCROLL) ToggleMode(MODE_SCROLL);
        else if (w == HKID_REELS)  ToggleMode(MODE_REELS);
        return 0;

    case WM_HKDONE:
        ReadHotkeys();
        SaveConfig();
        RegisterHotkeys();
        UpdateUI();
        return 0;

    case WM_DIAL: {
        if (!g_active || !g_vol) return 0;
        float v = (float)l / 10000.0f;
        if (GetTickCount64() < g_ignoreUntil) {      // keyboard volume key: accept as real volume
            g_ref = g_saved = v;
            return 0;
        }
        if (std::fabs(v - g_ref) < 0.001f) return 0;
        bool up = v > g_ref;
        if (g_cfg.keepCentered || v < 0.12f || v > 0.88f) {
            g_vol->SetMasterVolumeLevelScalar(PARK, &CTX);
            g_ref = PARK;
        } else {
            g_ref = v;
        }
        DoAction(up);
        g_clicks++;
        UpdateUI();
        return 0;
    }

    case WM_AUDIODEV:
        SetTimer(h, TIMER_DEV, 500, nullptr);        // coalesce bursts of device events
        return 0;

    case WM_TIMER:
        if (w == TIMER_DEV) { KillTimer(h, TIMER_DEV); HandleDeviceChange(); }
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if (g_loading) return 0;
        if (id >= ID_MODE_VOLUME && id <= ID_MODE_REELS) {
            if (code == BN_CLICKED) SetMode((Mode)(id - ID_MODE_VOLUME));
            return 0;
        }
        if (id == ID_USECUR && code == BN_CLICKED) {
            std::wstring n = CurrentDefaultName();
            if (!n.empty()) SetDlgItemTextW(h, ID_DEVICE, n.c_str());   // triggers EN_CHANGE below
            return 0;
        }
        if (id == ID_DEVICE && code == EN_CHANGE) {
            ConfigFromControls(); SaveConfig(); Apply();
            return 0;
        }
        if (id == ID_NOTCH && code == EN_CHANGE) {
            ConfigFromControls(); SaveConfig();
            return 0;
        }
        if ((id == ID_REVERSE || id == ID_CENTER || id == ID_NOTIFY || id == ID_CLOSETRAY) && code == BN_CLICKED) {
            ConfigFromControls(); SaveConfig();
            return 0;
        }
        if (id == ID_AUTOSTART && code == BN_CLICKED) {
            SetAutostart(IsDlgButtonChecked(h, ID_AUTOSTART) == BST_CHECKED);
            return 0;
        }
        if (id >= ID_TRAY_OPEN && id <= ID_TRAY_EXIT) { TrayCommand(id); return 0; }
        return 0;
    }

    case WM_TRAY:
        if (LOWORD(l) == WM_LBUTTONUP || LOWORD(l) == WM_LBUTTONDBLCLK) ShowMain();
        else if (LOWORD(l) == WM_RBUTTONUP) ShowTrayMenu();
        return 0;

    case WM_CLOSE:
        if (g_cfg.closeToTray) {
            ShowWindow(h, SW_HIDE);
            if (!g_toldTray) { g_toldTray = true; Notify(L"Still running in the tray. Right-click the icon to exit."); }
            return 0;
        }
        DestroyWindow(h);
        return 0;

    case WM_ENDSESSION:
        if (w) Deactivate();                          // put the real volume back on logoff/shutdown
        return 0;

    case WM_DESTROY:
        KillTimer(h, TIMER_DEV);
        Deactivate();                                 // restore the real volume
        UnregisterHotkeys();
        if (g_hook) UnhookWindowsHookEx(g_hook);
        if (g_enum && g_notif) g_enum->UnregisterEndpointNotificationCallback(g_notif);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ============================================================================
// Entry point
// ============================================================================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    g_hInst = hInst;
    g_msgShow = RegisterWindowMessageW(L"SmartDial.ShowMainWindow");
    g_msgTaskbar = RegisterWindowMessageW(L"TaskbarCreated");

    // Single instance: if already running, just bring it to the front.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"SmartDialSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(CLS_NAME, nullptr);
        if (other) PostMessageW(other, g_msgShow, 0, 0);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_HOTKEY_CLASS | ICC_UPDOWN_CLASS};
    InitCommonControlsEx(&icc);

    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);

    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    InitIniPath();
    LoadConfig();

    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                IID_IMMDeviceEnumerator, (void**)&g_enum))) {
        MessageBoxW(nullptr, L"Could not initialise Windows audio.", L"SmartDial", MB_ICONERROR);
        return 1;
    }
    g_notif = new DevNotify();
    g_enum->RegisterEndpointNotificationCallback(g_notif);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CLS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = {0, 0, S(440), S(574)};
    AdjustWindowRect(&rc, style, FALSE);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    HWND hwnd = CreateWindowExW(0, CLS_NAME, L"SmartDial", style, x, y, ww, wh,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KbHook, hInst, 0);

    bool minimized = wcsstr(GetCommandLineW(), L"/minimized") != nullptr;
    if (!minimized) { ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd); }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_notif) { g_notif->Release(); g_notif = nullptr; }
    if (g_enum) { g_enum->Release(); g_enum = nullptr; }
    if (g_font) DeleteObject(g_font);
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}
