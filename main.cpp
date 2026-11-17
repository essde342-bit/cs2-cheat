#include <Windows.h>
#include <TlHelp32.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <urlmon.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <atomic>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#define CURRENT_VERSION "2.0.0"
#define VERSION_URL "https://raw.githubusercontent.com/essde342-bit/cs2-cheat/main/version.json"

struct DumpedOffsets {
    uintptr_t dwEntityList = 0;
    uintptr_t dwLocalPlayerPawn = 0;
    uintptr_t dwViewMatrix = 0;
    uintptr_t dwViewAngles = 0;
    uintptr_t dwSensitivity = 0;
    uintptr_t dwSensOffset = 0x58;
    uintptr_t m_hPlayerPawn = 0x92C;
    uintptr_t m_iHealth = 0x344;
    uintptr_t m_iTeamNum = 0x3E3;
    uintptr_t m_pGameSceneNode = 0x328;
    uintptr_t m_entitySpottedState = 0x1438;
    bool valid = false;
};

static DumpedOffsets g_Offsets;
static const float PI = 3.14159265358979f;
static const float R2D = 180.f / PI;

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct Mat4 { float m[4][4]; };

static ImVec2 g_MenuPos(0, 0);
static ImVec2 g_MenuSize(0, 0);

struct Settings {
    bool espEnabled = true, box = true, skeleton = true, healthBar = true, hpText = true;
    bool snaplines = false, showTeam = false, showBots = true;
    float maxDistance = 5000.f, boxThickness = 2.f;
    float boxColor[4] = { 1,0,0,1 }, skelColor[4] = { 1,1,1,1 }, snapColor[4] = { 1,1,0,1 };
    bool aimEnabled = false, aimToggle = false, visEnabled = true;
    int aimKeyIdx = 2;
    float aimFov = 5.f, aimSmooth = 8.f, aimJitter = 0.3f;
    int aimReactionMs = 120, aimBoneIdx = 0;
    float eyeHeight = 64.05f;
    bool manualSens = false; float sensOverride = 2.5f;
    bool trigEnabled = false, trigRequireAimKey = true;
    float trigFov = 1.5f;
    int trigDelayMs = 80, trigCooldownMs = 120, trigHoldMs = 40;
    bool menuOpen = true;
};

static const char* KEY_NAMES[] = { "Всегда", "Mouse4", "Mouse5", "Alt", "Shift", "Ctrl", "CapsLock" };
static const int KEY_VKS[] = { 0, VK_XBUTTON1, VK_XBUTTON2, VK_MENU, VK_SHIFT, VK_CONTROL, VK_CAPITAL };
static const char* BONE_NAMES[] = { "Голова", "Шея", "Грудь", "Таз" };
static const int BONE_IDS[] = { 6, 5, 4, 0 };

static std::atomic<bool> g_Running{ true };
static Settings g;
static ID3D11Device* g_Device = nullptr;
static ID3D11DeviceContext* g_Context = nullptr;
static IDXGISwapChain* g_SwapChain = nullptr;
static ID3D11RenderTargetView* g_RTV = nullptr;
static HWND g_GameWnd = nullptr;
static HWND g_OverlayWnd = nullptr;
static std::string g_CfgPath;
static bool g_VisOk = false;

class OffsetDumper {
public:
    static bool ScanModule(HANDLE hProcess, uintptr_t moduleBase, size_t moduleSize) {
        std::cout << "[*] Starting offset dump...\n";
        struct Signature { const char* pattern; const char* mask; int type; int offset; };
        std::vector<Signature> signatures = {
            {"\x48\x8B\x0D\x00\x00\x00\x00\x48\x89\x7C\x24", "xxx????xxxx", 1, 3},
            {"\x48\x8B\x05\x00\x00\x00\x00\x48\x85\xC0\x74\x00\x8B\x88", "xxx????xxxx?xx", 2, 3},
            {"\x48\x8D\x0D\x00\x00\x00\x00\x48\xC1\xE0\x06", "xxx????xxxx", 3, 3},
            {"\x48\x8B\x0D\x00\x00\x00\x00\xE8\x00\x00\x00\x00\xF3\x0F\x10\x05", "xxx????x????xxxx", 4, 3},
            {"\x48\x8B\x05\x00\x00\x00\x00\xF3\x0F\x10\x88\x00\x00\x00\x00", "xxx????xxxx????", 5, 3}
        };
        std::vector<uint8_t> moduleData(moduleSize);
        SIZE_T bytesRead;
        if (!ReadProcessMemory(hProcess, (LPCVOID)moduleBase, moduleData.data(), moduleSize, &bytesRead)) return false;
        for (const auto& sig : signatures) {
            uintptr_t found = FindPattern(moduleData.data(), moduleSize, sig.pattern, sig.mask);
            if (found) {
                int32_t relativeOffset = *(int32_t*)(moduleData.data() + found + sig.offset);
                uintptr_t finalOffset = moduleBase + found + sig.offset + 4 + relativeOffset - moduleBase;
                switch (sig.type) {
                    case 1: g_Offsets.dwEntityList = finalOffset; break;
                    case 2: g_Offsets.dwLocalPlayerPawn = finalOffset; break;
                    case 3: g_Offsets.dwViewMatrix = finalOffset; break;
                    case 4: g_Offsets.dwViewAngles = finalOffset; break;
                    case 5: g_Offsets.dwSensitivity = finalOffset; break;
                }
                std::cout << "[+] Found offset type " << sig.type << " at 0x" << std::hex << finalOffset << std::dec << "\n";
            }
        }
        g_Offsets.valid = (g_Offsets.dwEntityList != 0 && g_Offsets.dwLocalPlayerPawn != 0);
        if (g_Offsets.valid) {
            std::cout << "[+] Offset dump successful!\n";
        } else {
            std::cout << "[!] Offset dump failed - using fallback values\n";
            g_Offsets.dwEntityList = 0x2709010;
            g_Offsets.dwLocalPlayerPawn = 0x2553D38;
            g_Offsets.dwViewMatrix = 0x2559640;
            g_Offsets.dwViewAngles = 0x256A4B8;
            g_Offsets.dwSensitivity = 0x2551038;
        }
        return g_Offsets.valid;
    }
private:
    static uintptr_t FindPattern(const uint8_t* data, size_t size, const char* pattern, const char* mask) {
        size_t patternLen = strlen(mask);
        for (size_t i = 0; i <= size - patternLen; i++) {
            bool found = true;
            for (size_t j = 0; j < patternLen; j++) {
                if (mask[j] != '?' && pattern[j] != data[i + j]) { found = false; break; }
            }
            if (found) return i;
        }
        return 0;
    }
};

static void InitCfgPath() {
    char ad[MAX_PATH]; GetEnvironmentVariableA("APPDATA", ad, MAX_PATH);
    std::string dir = std::string(ad) + "\\VoidDLC";
    CreateDirectoryA(dir.c_str(), nullptr);
    g_CfgPath = dir + "\\config.ini";
}
static void WS(const char* s, const char* k, const std::string& v) { WritePrivateProfileStringA(s, k, v.c_str(), g_CfgPath.c_str()); }
static void WF(const char* s, const char* k, float v) { char b[32]; sprintf_s(b, "%.4f", v); WS(s, k, b); }
static void WI(const char* s, const char* k, int v) { char b[32]; sprintf_s(b, "%d", v); WS(s, k, b); }
static float RF(const char* s, const char* k, float d) { char b[64]; sprintf_s(b, "%.4f", d); GetPrivateProfileStringA(s, k, b, b, 64, g_CfgPath.c_str()); return (float)atof(b); }
static int RI(const char* s, const char* k, int d) { return GetPrivateProfileIntA(s, k, d, g_CfgPath.c_str()); }
static void WC(const char* s, const char* k, const float c[4]) { for (int i = 0; i < 4; i++) { char key[32]; sprintf_s(key, "%s%d", k, i); WF(s, key, c[i]); } }
static void RC(const char* s, const char* k, float c[4]) { for (int i = 0; i < 4; i++) { char key[32]; sprintf_s(key, "%s%d", k, i); c[i] = RF(s, key, c[i]); } }

static void SaveConfig() {
    WF("vis", "maxdist", g.maxDistance); WF("vis", "boxthick", g.boxThickness);
    WI("vis", "esp", g.espEnabled); WI("vis", "box", g.box); WI("vis", "skel", g.skeleton);
    WI("vis", "hpbar", g.healthBar); WI("vis", "hptext", g.hpText); WI("vis", "snap", g.snaplines);
    WI("vis", "team", g.showTeam); WI("vis", "bots", g.showBots);
    WC("vis", "boxcol", g.boxColor); WC("vis", "skelcol", g.skelColor); WC("vis", "snapcol", g.snapColor);
    WI("aim", "on", g.aimEnabled); WI("aim", "toggle", g.aimToggle); WI("aim", "key", g.aimKeyIdx);
    WI("aim", "vis", g.visEnabled);
    WF("aim", "fov", g.aimFov); WF("aim", "smooth", g.aimSmooth); WF("aim", "jitter", g.aimJitter);
    WI("aim", "react", g.aimReactionMs); WI("aim", "bone", g.aimBoneIdx);
    WF("aim", "eye", g.eyeHeight); WI("aim", "mansens", g.manualSens); WF("aim", "sens", g.sensOverride);
    WI("trig", "on", g.trigEnabled); WI("trig", "reqkey", g.trigRequireAimKey);
    WF("trig", "fov", g.trigFov); WI("trig", "delay", g.trigDelayMs);
    WI("trig", "cool", g.trigCooldownMs); WI("trig", "hold", g.trigHoldMs);
}
static void LoadConfig() {
    g.maxDistance = RF("vis", "maxdist", g.maxDistance); g.boxThickness = RF("vis", "boxthick", g.boxThickness);
    g.espEnabled = RI("vis", "esp", g.espEnabled); g.box = RI("vis", "box", g.box); g.skeleton = RI("vis", "skel", g.skeleton);
    g.healthBar = RI("vis", "hpbar", g.healthBar); g.hpText = RI("vis", "hptext", g.hpText); g.snaplines = RI("vis", "snap", g.snaplines);
    g.showTeam = RI("vis", "team", g.showTeam); g.showBots = RI("vis", "bots", g.showBots);
    RC("vis", "boxcol", g.boxColor); RC("vis", "skelcol", g.skelColor); RC("vis", "snapcol", g.snapColor);
    g.aimEnabled = RI("aim", "on", g.aimEnabled); g.aimToggle = RI("aim", "toggle", g.aimToggle); g.aimKeyIdx = RI("aim", "key", g.aimKeyIdx);
    g.visEnabled = RI("aim", "vis", g.visEnabled);
    g.aimFov = RF("aim", "fov", g.aimFov); g.aimSmooth = RF("aim", "smooth", g.aimSmooth); g.aimJitter = RF("aim", "jitter", g.aimJitter);
    g.aimReactionMs = RI("aim", "react", g.aimReactionMs); g.aimBoneIdx = RI("aim", "bone", g.aimBoneIdx);
    g.eyeHeight = RF("aim", "eye", g.eyeHeight); g.manualSens = RI("aim", "mansens", g.manualSens); g.sensOverride = RF("aim", "sens", g.sensOverride);
    g.trigEnabled = RI("trig", "on", g.trigEnabled); g.trigRequireAimKey = RI("trig", "reqkey", g.trigRequireAimKey);
    g.trigFov = RF("trig", "fov", g.trigFov); g.trigDelayMs = RI("trig", "delay", g.trigDelayMs);
    g.trigCooldownMs = RI("trig", "cool", g.trigCooldownMs); g.trigHoldMs = RI("trig", "hold", g.trigHoldMs);
    if (g.aimKeyIdx < 0 || g.aimKeyIdx > 6) g.aimKeyIdx = 2;
    if (g.aimBoneIdx < 0 || g.aimBoneIdx > 3) g.aimBoneIdx = 0;
}
static void Preset(int mode) {
    if (mode == 0) { g.aimFov = 3; g.aimSmooth = 12; g.aimJitter = 0.4f; g.aimReactionMs = 180; g.aimBoneIdx = 1; g.visEnabled = true; g.trigFov = 1.0f; g.trigDelayMs = 120; g.trigCooldownMs = 200; }
    else if (mode == 1) { g.aimFov = 6; g.aimSmooth = 8; g.aimJitter = 0.3f; g.aimReactionMs = 120; g.aimBoneIdx = 0; g.visEnabled = true; g.trigFov = 1.5f; g.trigDelayMs = 80; g.trigCooldownMs = 140; }
    else { g.aimFov = 15; g.aimSmooth = 3; g.aimJitter = 0.1f; g.aimReactionMs = 40; g.aimBoneIdx = 0; g.visEnabled = false; g.trigFov = 3; g.trigDelayMs = 30; g.trigCooldownMs = 80; }
}

static std::string ExtractJsonString(const std::string& j, const std::string& k) {
    size_t p = j.find("\"" + k + "\""); if (p == std::string::npos) return "";
    p = j.find(':', p); if (p == std::string::npos) return "";
    p = j.find('"', p + 1); if (p == std::string::npos) return "";
    size_t e = j.find('"', p + 1); if (e == std::string::npos) return "";
    return j.substr(p + 1, e - p - 1);
}
static bool CheckPayloadVersion() {
    char t[MAX_PATH]; GetTempPathA(MAX_PATH, t);
    std::string path = std::string(t) + "void_chk.json";
    if (FAILED(URLDownloadToFileA(nullptr, VERSION_URL, path.c_str(), 0, nullptr))) return true;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return true;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::string buf(sz, '\0'); fread(&buf[0], 1, sz, f); fclose(f); DeleteFileA(path.c_str());
    std::string rv = ExtractJsonString(buf, "version");
    return rv.empty() || rv == CURRENT_VERSION;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (m == WM_SIZE && g_Device && g_SwapChain && w != SIZE_MINIMIZED) {
        if (g_RTV) { g_RTV->Release(); g_RTV = nullptr; }
        g_SwapChain->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0);
        ID3D11Texture2D* bb = nullptr; g_SwapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
        g_Device->CreateRenderTargetView(bb, nullptr, &g_RTV); bb->Release();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void UpdateHitbox(bool mouseOverMenu, bool menuOpen) {
    LONG_PTR ex = GetWindowLongPtrW(g_OverlayWnd, GWL_EXSTYLE);
    
    if (!menuOpen || !mouseOverMenu) {
        if (!(ex & WS_EX_TRANSPARENT)) {
            ex |= WS_EX_TRANSPARENT;
            SetWindowLongPtrW(g_OverlayWnd, GWL_EXSTYLE, ex);
        }
    } else {
        if (ex & WS_EX_TRANSPARENT) {
            ex &= ~WS_EX_TRANSPARENT;
            SetWindowLongPtrW(g_OverlayWnd, GWL_EXSTYLE, ex);
        }
    }
}

static ImU32 ToU32(const float c[4]) { return IM_COL32((int)(c[0]*255),(int)(c[1]*255),(int)(c[2]*255),(int)(c[3]*255)); }
static DWORD FindProcessId(const char* n) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe; pe.dwSize = sizeof(pe); DWORD pid = 0;
    if (Process32First(s, &pe)) do { if (!_stricmp(pe.szExeFile, n)) { pid = pe.th32ProcessID; break; } } while (Process32Next(s, &pe));
    CloseHandle(s); return pid;
}
struct WD { DWORD pid; HWND hwnd; };
static BOOL CALLBACK EWC(HWND h, LPARAM l) {
    WD* w = (WD*)l; DWORD p = 0; GetWindowThreadProcessId(h, &p);
    if (p == w->pid && IsWindowVisible(h)) { w->hwnd = h; return FALSE; } return TRUE;
}
static HWND FindWindowByPid(DWORD p) { WD w{ p, nullptr }; EnumWindows(EWC, (LPARAM)&w); return w.hwnd; }

class Memory {
public:
    HANDLE handle = nullptr; uintptr_t clientBase = 0; size_t clientSize = 0;
    bool Init() {
        DWORD pid = FindProcessId("cs2.exe");
        if (!pid) { std::cout << "[!] cs2.exe not found\n"; return false; }
        HANDLE hq = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!hq) { std::cout << "[!] OpenProcess(query) failed\n"; return false; }
        HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (s != INVALID_HANDLE_VALUE) {
            MODULEENTRY32 me; me.dwSize = sizeof(me);
            if (Module32First(s, &me)) do {
                if (!_stricmp(me.szModule, "client.dll")) { clientBase = (uintptr_t)me.modBaseAddr; clientSize = me.modBaseSize; break; }
            } while (Module32Next(s, &me));
            CloseHandle(s);
        }
        CloseHandle(hq);
        if (!clientBase) { std::cout << "[!] client.dll not found\n"; return false; }
        handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!handle) { std::cout << "[!] OpenProcess(read) failed\n"; return false; }
        if (!OffsetDumper::ScanModule(handle, clientBase, clientSize)) {
            std::cout << "[!] Offset dump failed, using fallback\n";
        }
        return true;
    }
    template<typename T> T Read(uintptr_t a) const { T v{}; ReadProcessMemory(handle, (LPCVOID)a, &v, sizeof(T), nullptr); return v; }
    bool ReadRaw(uintptr_t a, void* b, size_t n) const { return ReadProcessMemory(handle, (LPCVOID)a, b, n, nullptr) == TRUE; }
    uintptr_t GetListEntity(uintptr_t lb, int i) const {
        uintptr_t c = Read<uintptr_t>(lb + 8 * ((i & 0x7FFF) >> 9) + 16);
        return c ? Read<uintptr_t>(c + 120 * (i & 0x3FF)) : 0;
    }
    bool ReadOriginSmart(uintptr_t sn, Vec3& out) const {
        static const uintptr_t cand[] = { 0xD0, 0x90, 0xC0 };
        for (uintptr_t o : cand) {
            Vec3 v = Read<Vec3>(sn + o);
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) continue;
            if (v.x == 0 && v.y == 0 && v.z == 0) continue;
            if (fabsf(v.x) > 65536 || fabsf(v.y) > 65536 || fabsf(v.z) > 65536) continue;
            out = v; return true;
        }
        return false;
    }
    bool ReadBonesSmart(uintptr_t sn, const Vec3& org, float out[64][3][4]) const {
        static const uintptr_t cand[] = { 0x80, 0x1E0, 0x160 };
        for (uintptr_t o : cand) {
            uintptr_t p = Read<uintptr_t>(sn + o); if (!p) continue;
            float tmp[64][3][4]; if (!ReadRaw(p, tmp, sizeof(tmp))) continue;
            Vec3 h{ tmp[6][0][3], tmp[6][1][3], tmp[6][2][3] };
            if (!std::isfinite(h.x) || !std::isfinite(h.y) || !std::isfinite(h.z)) continue;
            float dx = h.x-org.x, dy = h.y-org.y, dz = h.z-org.z;
            float d = sqrtf(dx*dx+dy*dy+dz*dz);
            if (d > 1.f && d < 120.f) { memcpy(out, tmp, sizeof(tmp)); return true; }
        }
        return false;
    }
    bool ReadAngles(Vec3& out) const {
        Vec3 a = Read<Vec3>(clientBase + g_Offsets.dwViewAngles);
        if (std::isfinite(a.x) && std::isfinite(a.y) && fabsf(a.x) <= 90.f && fabsf(a.y) <= 360.f) { out = a; return true; }
        uintptr_t p = Read<uintptr_t>(clientBase + g_Offsets.dwViewAngles);
        if (p) { a = Read<Vec3>(p); if (std::isfinite(a.x) && std::isfinite(a.y) && fabsf(a.x) <= 90.f && fabsf(a.y) <= 360.f) { out = a; return true; } }
        return false;
    }
    float ReadSens() const {
        uintptr_t p = Read<uintptr_t>(clientBase + g_Offsets.dwSensitivity);
        if (p) { float v = Read<float>(p + g_Offsets.dwSensOffset); if (std::isfinite(v) && v > 0.05f && v < 20.f) return v; }
        float v2 = Read<float>(clientBase + g_Offsets.dwSensitivity);
        if (std::isfinite(v2) && v2 > 0.05f && v2 < 20.f) return v2;
        return -1.f;
    }
    bool ReadVisible(uintptr_t pawn, int localIndex, bool& out) const {
        if (localIndex < 0 || localIndex > 63) return false;
        uint8_t spotted = Read<uint8_t>(pawn + g_Offsets.m_entitySpottedState + 8);
        if (spotted > 1) return false;
        uint32_t m0 = Read<uint32_t>(pawn + g_Offsets.m_entitySpottedState + 12);
        uint32_t m1 = Read<uint32_t>(pawn + g_Offsets.m_entitySpottedState + 16);
        uint64_t mask = ((uint64_t)m1 << 32) | m0;
        out = (mask >> localIndex) & 1ULL;
        return true;
    }
};

static bool W2S(const Vec3& w, Vec2& s, const Mat4& m, int sw, int sh) {
    float ww = m.m[3][0]*w.x + m.m[3][1]*w.y + m.m[3][2]*w.z + m.m[3][3];
    if (ww < 0.01f) return false;
    float inv = 1.f / ww;
    s.x = sw*0.5f + (m.m[0][0]*w.x + m.m[0][1]*w.y + m.m[0][2]*w.z + m.m[0][3]) * inv * sw*0.5f;
    s.y = sh*0.5f - (m.m[1][0]*w.x + m.m[1][1]*w.y + m.m[1][2]*w.z + m.m[1][3]) * inv * sh*0.5f;
    return true;
}
static float NormYaw(float d) { while (d > 180.f) d -= 360.f; while (d < -180.f) d += 360.f; return d; }

struct PInfo {
    uintptr_t pawn; Vec3 origin, bonePos; float pitch, yaw, angDist;
    Vec2 sHead, sFeet; bool hasBones, visible; float bones[64][3][4]; int hp;
};

static void Help(const char* txt) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(300); ImGui::TextUnformatted(txt); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
}
static void SliderFHelp(const char* label, float* v, float mn, float mx, const char* fmt, const char* help) { ImGui::SliderFloat(label, v, mn, mx, fmt); ImGui::SameLine(); Help(help); }
static void SliderIHelp(const char* label, int* v, int mn, int mx, const char* help) { ImGui::SliderInt(label, v, mn, mx); ImGui::SameLine(); Help(help); }

static void RenderMenu() {
    ImGui::SetNextWindowSize(ImVec2(470, 560), ImGuiCond_FirstUseEver);
    ImGui::Begin("Void DLC", &g.menuOpen, ImGuiWindowFlags_NoCollapse);
    g_MenuPos = ImGui::GetWindowPos();
    g_MenuSize = ImGui::GetWindowSize();
    ImGui::TextColored(ImVec4(0.6f,0.8f,1,1), "Правый Shift - меню | v%s", CURRENT_VERSION);
    if (g_Offsets.valid) {
        ImGui::TextColored(ImVec4(0.3f,1,0.3f,1), "Оффсеты: OK (auto-dumped)");
    } else {
        ImGui::TextColored(ImVec4(1,0.5f,0.3f,1), "Оффсеты: Fallback");
    }
    ImGui::Separator();
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Визуал")) {
            ImGui::Checkbox("Включить ESP", &g.espEnabled); ImGui::Separator();
            ImGui::Checkbox("Рамки вокруг врагов", &g.box);
            if (g.box) { ImGui::Indent(16); ImGui::ColorEdit4("Цвет рамок", g.boxColor, ImGuiColorEditFlags_NoInputs); ImGui::SliderFloat("Толщина рамок", &g.boxThickness, 1, 5, "%.1f"); ImGui::Unindent(16); }
            ImGui::Checkbox("Скелет (кости)", &g.skeleton);
            if (g.skeleton) { ImGui::Indent(16); ImGui::ColorEdit4("Цвет скелета", g.skelColor, ImGuiColorEditFlags_NoInputs); ImGui::Unindent(16); }
            ImGui::Checkbox("Полоска здоровья", &g.healthBar);
            ImGui::Checkbox("Число здоровья", &g.hpText);
            ImGui::Checkbox("Линии к врагам", &g.snaplines);
            if (g.snaplines) { ImGui::Indent(16); ImGui::ColorEdit4("Цвет линий", g.snapColor, ImGuiColorEditFlags_NoInputs); ImGui::Unindent(16); }
            ImGui::Separator();
            ImGui::Checkbox("Показывать своих", &g.showTeam);
            ImGui::Checkbox("Показывать ботов", &g.showBots); ImGui::SameLine(); Help("Включает ESP на ботов. Работает и на игроков, и на ботов одновременно.");
            SliderFHelp("Дистанция (0=все)", &g.maxDistance, 0, 10000, "%.0f", "Дальше этого расстояния враги не рисуются. 0 = без ограничения.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Аим")) {
            ImGui::Checkbox("Включить аимбот", &g.aimEnabled); ImGui::Separator();
            ImGui::Combo("Кнопка аима", &g.aimKeyIdx, KEY_NAMES, 7); ImGui::SameLine(); Help("Какой кнопкой включать аим. 'Всегда' = аим активен постоянно.");
            ImGui::Checkbox("Переключать (иначе держать)", &g.aimToggle); ImGui::SameLine(); Help("Если включено - нажал один раз и аим работает, нажал ещё раз - выключился. Если выключено - аим работает пока держишь кнопку.");
            ImGui::Combo("Куда целиться", &g.aimBoneIdx, BONE_NAMES, 4); ImGui::SameLine(); Help("Голова = больше урона но заметнее. Шея/Грудь = незаметнее и стабильнее.");
            SliderFHelp("Радиус (FOV)", &g.aimFov, 1, 30, "%.1f", "Насколько далеко от прицела аим может брать цель. Меньше = незаметнее.");
            SliderFHelp("Плавность", &g.aimSmooth, 1, 25, "%.0f", "Чем больше - тем медленнее и человечнее движение. Меньше = резче.");
            SliderFHelp("Дрожание", &g.aimJitter, 0, 1.5f, "%.2f", "Мелкая случайная тряска прицела, чтобы движение не было идеальным.");
            SliderIHelp("Задержка реакции (мс)", &g.aimReactionMs, 0, 350, "Пауза перед началом наводки на новую цель, как у человека.");
            ImGui::Separator();
            ImGui::Checkbox("Только видимые враги (vis check)", &g.visEnabled); ImGui::SameLine(); Help("Аим берёт цель только если ты её реально видишь.");
            ImGui::Separator();
            SliderFHelp("Высота глаз", &g.eyeHeight, 40, 72, "%.1f", "Стоя ~64, сидя ~46. Влияет на вертикальную наводку.");
            ImGui::Checkbox("Своя чувствительность", &g.manualSens);
            if (g.manualSens) { SliderFHelp("Чувствительность", &g.sensOverride, 0.1f, 8.f, "%.2f", "Впиши свою sens из игры если авто-определение неверное."); }
            ImGui::TextDisabled("Отдача компенсируется автоматически.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Триггер")) {
            ImGui::Checkbox("Включить триггербот", &g.trigEnabled); ImGui::Separator();
            ImGui::Checkbox("Только с кнопкой аима", &g.trigRequireAimKey); ImGui::SameLine(); Help("Стрелять только когда держишь кнопку аима.");
            SliderFHelp("Зона срабатывания", &g.trigFov, 0.3f, 6.f, "%.1f", "Насколько точно прицел должен быть на враге чтобы выстрелить. Меньше = точнее.");
            SliderIHelp("Задержка выстрела (мс)", &g.trigDelayMs, 0, 300, "Пауза перед выстрелом когда прицел попал на врага.");
            SliderIHelp("Перерыв между выстрелами (мс)", &g.trigCooldownMs, 40, 500, "Чтобы не спамить кликами.");
            SliderIHelp("Длительность клика (мс)", &g.trigHoldMs, 15, 120, "Сколько держится кнопка мыши.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Настройки")) {
            ImGui::TextUnformatted("Готовые пресеты:");
            if (ImGui::Button("Legit (незаметно)", ImVec2(140, 30))) Preset(0); ImGui::SameLine(); Help("Медленно, малый радиус, только видимые. Для игры без палева.");
            if (ImGui::Button("Balanced", ImVec2(110, 30))) Preset(1); ImGui::SameLine(); Help("Среднее между незаметностью и скоростью.");
            if (ImGui::Button("Rage (быстро)", ImVec2(120, 30))) Preset(2); ImGui::SameLine(); Help("Быстро и резко, большой радиус. Заметно, для дезматча/ботов.");
            ImGui::Separator();
            if (ImGui::Button("Сохранить", ImVec2(180, 30))) SaveConfig(); ImGui::SameLine();
            if (ImGui::Button("Загрузить", ImVec2(180, 30))) LoadConfig();
            ImGui::Spacing();
            ImGui::TextDisabled("Конфиг: %s", g_CfgPath.c_str());
            ImGui::TextDisabled("Автозагрузка при старте, автосохранение при выходе.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

int main() {
    SetConsoleTitleA("console");
    srand(GetTickCount());
    if (!CheckPayloadVersion()) { std::cout << "[!] outdated\n"; system("pause"); return 1; }
    InitCfgPath(); LoadConfig();

    Memory mem; if (!mem.Init()) { system("pause"); return 1; }
    DWORD pid = FindProcessId("cs2.exe");
    g_GameWnd = FindWindowByPid(pid); if (!g_GameWnd) { system("pause"); return 1; }
    
    RECT rc; GetWindowRect(g_GameWnd, &rc);
    int winW = rc.right - rc.left, winH = rc.bottom - rc.top;

    wchar_t cls[64]; swprintf_s(cls, L"VD_%u_%llu", GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW; wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = cls; RegisterClassExW(&wc);
    
    g_OverlayWnd = CreateWindowExW(WS_EX_TOPMOST, cls, L"", WS_POPUP, rc.left, rc.top, winW, winH, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_OverlayWnd) { system("pause"); return 1; }
    
    MARGINS mg{ -1,-1,-1,-1 }; DwmExtendFrameIntoClientArea(g_OverlayWnd, &mg);
    
    ShowWindow(g_OverlayWnd, SW_SHOW); UpdateWindow(g_OverlayWnd);

    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Width = winW; sd.BufferDesc.Height = winH;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferDesc.RefreshRate = { 60,1 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = g_OverlayWnd; sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_SwapChain, &g_Device, &fl, &g_Context) != S_OK) { system("pause"); return 1; }
    ID3D11Texture2D* bb = nullptr; g_SwapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_Device->CreateRenderTargetView(bb, nullptr, &g_RTV); bb->Release();

    ImGui::CreateContext(); ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    ImFont* f = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    if (!f) f = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 16.f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    if (!f) io.Fonts->AddFontDefault();
    ImGuiStyle& pst = ImGui::GetStyle();
    pst.WindowRounding = 8.f; pst.FrameRounding = 5.f; pst.GrabRounding = 4.f; pst.TabRounding = 5.f;
    pst.Colors[ImGuiCol_Header] = ImVec4(0.15f, 0.30f, 0.52f, 0.6f);
    pst.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.40f, 0.65f, 0.8f);
    pst.Colors[ImGuiCol_Button] = ImVec4(0.13f, 0.30f, 0.52f, 1);
    pst.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.45f, 0.75f, 1);
    pst.Colors[ImGuiCol_SliderGrab] = ImVec4(0.35f, 0.62f, 1.0f, 1);
    pst.Colors[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.62f, 1.0f, 1);
    ImGui_ImplWin32_Init(g_OverlayWnd); ImGui_ImplDX11_Init(g_Device, g_Context);

    const float clear[4] = { 0,0,0,0 };
    static const int kSkel[][2] = { {6,5},{5,4},{4,2},{2,0},{4,8},{8,9},{9,10},{4,13},{13,14},{14,15},{0,22},{22,23},{23,24},{0,25},{25,26},{26,27} };
    bool lastMenu = g.menuOpen, prevAimKey = false, aimToggled = false, prevMenuKey = false;
    uintptr_t lastTarget = 0; DWORD acquireAt = 0;
    bool trigDown = false; DWORD trigUpAt = 0, lastShot = 0;
    float usedSens = 2.5f; int localIndex = -1;

    while (g_Running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_Running = false;
        }
        if (!g_Running) break;

        bool menuKeyDown = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
        if (menuKeyDown && !prevMenuKey) {
            g.menuOpen = !g.menuOpen;
        }
        prevMenuKey = menuKeyDown;

        DWORD now = GetTickCount();
        int vk = KEY_VKS[g.aimKeyIdx];
        bool keyDown = (vk == 0) ? true : (GetAsyncKeyState(vk) & 0x8000) != 0;
        bool aimActive;
        if (g.aimToggle && vk != 0) { if (keyDown && !prevAimKey) aimToggled = !aimToggled; aimActive = aimToggled; }
        else aimActive = keyDown;
        prevAimKey = keyDown;

        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        auto* dl = ImGui::GetBackgroundDrawList();

        std::vector<PInfo> players;
        Vec3 curAngles{}; bool haveAngles = mem.ReadAngles(curAngles);
        float sens = g.manualSens ? g.sensOverride : mem.ReadSens();
        if (sens <= 0.f) sens = g.sensOverride;
        usedSens = sens;

        uintptr_t el = mem.Read<uintptr_t>(mem.clientBase + g_Offsets.dwEntityList);
        uintptr_t lp = mem.Read<uintptr_t>(mem.clientBase + g_Offsets.dwLocalPlayerPawn);
        int lt = lp ? mem.Read<int>(lp + g_Offsets.m_iTeamNum) : 0;
        Vec3 lo{};
        if (lp) { uintptr_t ls = mem.Read<uintptr_t>(lp + g_Offsets.m_pGameSceneNode); if (ls) mem.ReadOriginSmart(ls, lo); }
        Vec3 eye{ lo.x, lo.y, lo.z + g.eyeHeight };
        Mat4 vm = mem.Read<Mat4>(mem.clientBase + g_Offsets.dwViewMatrix);
        bool needBones = g.skeleton || g.aimEnabled || g.trigEnabled;
        g_VisOk = false; localIndex = -1;

        if (g.espEnabled || g.aimEnabled || g.trigEnabled) {
            // ДИАГНОСТИКА: проверяем основные оффсеты
            static bool firstRun = true;
            if (firstRun) {
                std::cout << "[DEBUG] dwEntityList: 0x" << std::hex << g_Offsets.dwEntityList << std::dec << "\n";
                std::cout << "[DEBUG] m_hPlayerPawn: 0x" << std::hex << g_Offsets.m_hPlayerPawn << std::dec << "\n";
                std::cout << "[DEBUG] m_iHealth: 0x" << std::hex << g_Offsets.m_iHealth << std::dec << "\n";
                std::cout << "[DEBUG] m_iTeamNum: 0x" << std::hex << g_Offsets.m_iTeamNum << std::dec << "\n";
                std::cout << "[DEBUG] Entity list address: 0x" << std::hex << el << std::dec << "\n";
                std::cout << "[DEBUG] Local player pawn: 0x" << std::hex << lp << std::dec << "\n";
                firstRun = false;
            }
            
            int playersFound = 0;
            int controllersFound = 0;
            int botsFound = 0;
            
            for (int i = 1; i <= 256; ++i) {
                uintptr_t entity = mem.GetListEntity(el, i);
                if (!entity) continue;
                
                // ДИАГНОСТИКА: первые 5 сущностей
                if (i <= 5) {
                    std::cout << "[DEBUG] Entity " << i << ": 0x" << std::hex << entity << std::dec << "\n";
                }

                uintptr_t pw = 0;
                bool isBot = false;

                uint32_t ph = mem.Read<uint32_t>(entity + g_Offsets.m_hPlayerPawn);
                
                if (i <= 5) {
                    std::cout << "[DEBUG]   m_hPlayerPawn value: 0x" << std::hex << ph << std::dec << "\n";
                }
                
                if (ph && ph != 0xFFFFFFFF && ph != 0) {
                    uintptr_t pawnIndex = ph & 0x7FFF;
                    if (pawnIndex > 0 && pawnIndex < 0x7FFF) {
                        pw = mem.GetListEntity(el, pawnIndex);
                        if (i <= 5) {
                            std::cout << "[DEBUG]   Pawn index: " << pawnIndex << ", Pawn address: 0x" << std::hex << pw << std::dec << "\n";
                        }
                        if (pw) controllersFound++;
                    }
                }

                if (!pw) {
                    int testHP = mem.Read<int>(entity + g_Offsets.m_iHealth);
                    int testTeam = mem.Read<int>(entity + g_Offsets.m_iTeamNum);
                    
                    if (i <= 5) {
                        std::cout << "[DEBUG]   Test HP: " << testHP << ", Team: " << testTeam << "\n";
                    }
                    
                    if (testHP > 0 && testHP <= 100 && (testTeam == 2 || testTeam == 3)) {
                        uintptr_t testSN = mem.Read<uintptr_t>(entity + g_Offsets.m_pGameSceneNode);
                        if (testSN && testSN > 0x10000) {
                            Vec3 testOrigin{};
                            if (mem.ReadOriginSmart(testSN, testOrigin)) {
                                pw = entity;
                                isBot = true;
                                botsFound++;
                            }
                        }
                    }
                }

                if (!pw) continue;
                if (pw == lp) { 
                    localIndex = i; 
                    if (i <= 5) std::cout << "[DEBUG]   This is local player\n";
                    continue; 
                }

                int hp = mem.Read<int>(pw + g_Offsets.m_iHealth);
                if (hp <= 0 || hp > 100) continue;

                int tm = mem.Read<int>(pw + g_Offsets.m_iTeamNum);
                if (tm != 2 && tm != 3) continue;
                
                if (!g.showTeam && tm == lt) continue;
                if (isBot && !g.showBots) continue;

                uintptr_t sn = mem.Read<uintptr_t>(pw + g_Offsets.m_pGameSceneNode);
                if (!sn) continue;
                
                PInfo p{}; 
                p.pawn = pw; 
                p.hp = hp; 
                p.visible = true;
                
                if (!mem.ReadOriginSmart(sn, p.origin)) continue;
                
                if (g.maxDistance > 0) {
                    float dx = p.origin.x-lo.x, dy = p.origin.y-lo.y, dz = p.origin.z-lo.z;
                    if (sqrtf(dx*dx+dy*dy+dz*dz) > g.maxDistance) continue;
                }
                
                if (g.visEnabled && !isBot) { 
                    bool ok = mem.ReadVisible(pw, localIndex, p.visible); 
                    if (ok) g_VisOk = true; 
                }
                
                p.hasBones = needBones ? mem.ReadBonesSmart(sn, p.origin, p.bones) : false;
                int bid = BONE_IDS[g.aimBoneIdx];
                p.bonePos = p.hasBones ? Vec3{ p.bones[bid][0][3], p.bones[bid][1][3], p.bones[bid][2][3] }
                                       : Vec3{ p.origin.x, p.origin.y, p.origin.z + 60.f };
                
                Vec3 d{ p.bonePos.x-eye.x, p.bonePos.y-eye.y, p.bonePos.z-eye.z };
                float hyp = sqrtf(d.x*d.x + d.y*d.y);
                p.pitch = atan2f(-d.z, hyp) * R2D;
                p.yaw = atan2f(d.y, d.x) * R2D;
                p.angDist = 9999.f;
                if (haveAngles) { 
                    float dyaw = NormYaw(p.yaw - curAngles.y); 
                    float dp = p.pitch - curAngles.x; 
                    p.angDist = sqrtf(dyaw*dyaw + dp*dp); 
                }
                
                Vec3 hw = p.hasBones ? Vec3{ p.bones[6][0][3], p.bones[6][1][3], p.bones[6][2][3] } 
                                     : Vec3{ p.origin.x, p.origin.y, p.origin.z + 75.f };
                Vec3 fw = p.origin; fw.z -= 2.f;
                if (W2S(hw, p.sHead, vm, winW, winH) && W2S(fw, p.sFeet, vm, winW, winH)) {
                    players.push_back(p);
                    playersFound++;
                }
            }
            
            // ДИАГНОСТИКА: статистика
            static int frameCount = 0;
            if (frameCount % 60 == 0) {
                std::cout << "[STATS] Controllers: " << controllersFound 
                          << ", Bots: " << botsFound 
                          << ", Rendered: " << playersFound << "\n";
            }
            frameCount++;
        }

        PInfo* target = nullptr;
        if (haveAngles) for (auto& p : players) {
            if (g.visEnabled && g_VisOk && !p.visible) continue;
            if (p.angDist < g.aimFov && (!target || p.angDist < target->angDist)) target = &p;
        }
        if (target && target->pawn != lastTarget) { lastTarget = target->pawn; acquireAt = now; }
        if (!target) lastTarget = 0;

        bool canAct = !g.menuOpen;
        if (g.aimEnabled && aimActive && canAct && target && haveAngles && (now - acquireAt) >= (DWORD)g.aimReactionMs) {
            float dyaw = NormYaw(target->yaw - curAngles.y);
            float dpitch = target->pitch - curAngles.x;
            if (g.aimJitter > 0) {
                dyaw += ((rand() % 1000) / 1000.f - 0.5f) * 2.f * g.aimJitter;
                dpitch += ((rand() % 1000) / 1000.f - 0.5f) * 2.f * g.aimJitter;
            }
            float base = 1.f / g.aimSmooth;
            float ease = 0.35f + 0.65f * fminf(1.f, target->angDist / 8.f);
            float frac = fminf(1.f, base * ease);
            float mx = -(dyaw * frac) / (0.022f * sens);
            float my = (dpitch * frac) / (0.022f * sens);
            if (fabsf(mx) >= 1.f || fabsf(my) >= 1.f) mouse_event(MOUSEEVENTF_MOVE, (LONG)mx, (LONG)my, 0, 0);
        }

        if (trigDown && now >= trigUpAt) { mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0); trigDown = false; }
        if (g.trigEnabled && canAct && target && target->angDist < g.trigFov && (!g.trigRequireAimKey || aimActive) && !trigDown &&
            (now - acquireAt) >= (DWORD)g.trigDelayMs && (now - lastShot) >= (DWORD)g.trigCooldownMs) {
            mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            trigDown = true; trigUpAt = now + g.trigHoldMs; lastShot = now;
        }

        ImU32 bc = ToU32(g.boxColor), sc = ToU32(g.skelColor), nc = ToU32(g.snapColor);
        if (g.espEnabled) for (auto& p : players) {
            float h = p.sFeet.y - p.sHead.y; if (h < 5) continue;
            float w = h * 0.35f, bt = p.sHead.y - h * 0.06f;
            if (g.snaplines) dl->AddLine(ImVec2(winW*0.5f, (float)winH), ImVec2(p.sFeet.x, p.sFeet.y), nc, 1);
            if (g.box) dl->AddRect(ImVec2(p.sHead.x-w*0.5f, bt), ImVec2(p.sHead.x+w*0.5f, p.sFeet.y), bc, 0, 0, g.boxThickness);
            if (g.healthBar) {
                float f = p.hp / 100.f;
                dl->AddRectFilled(ImVec2(p.sHead.x-w*0.5f-6, bt+(p.sFeet.y-bt)*(1-f)), ImVec2(p.sHead.x-w*0.5f-2, p.sFeet.y), IM_COL32(0,255,0,255));
                dl->AddRect(ImVec2(p.sHead.x-w*0.5f-6, bt), ImVec2(p.sHead.x-w*0.5f-2, p.sFeet.y), IM_COL32(255,255,255,255), 0, 0, 1);
            }
            if (g.skeleton && p.hasBones) for (auto& b : kSkel) {
                Vec3 p1{ p.bones[b[0]][0][3], p.bones[b[0]][1][3], p.bones[b[0]][2][3] };
                Vec3 p2{ p.bones[b[1]][0][3], p.bones[b[1]][1][3], p.bones[b[1]][2][3] };
                Vec2 s1{}, s2{};
                if (W2S(p1, s1, vm, winW, winH) && W2S(p2, s2, vm, winW, winH)) dl->AddLine(ImVec2(s1.x, s1.y), ImVec2(s2.x, s2.y), sc, 1.5f);
            }
            if (g.hpText) { char t[16]; sprintf_s(t, "%d", p.hp); dl->AddText(ImVec2(p.sHead.x-8, bt-16), IM_COL32(255,255,255,255), t); }
        }

        if (g.menuOpen) {
            RenderMenu();
            
            POINT mousePos;
            GetCursorPos(&mousePos);
            ScreenToClient(g_OverlayWnd, &mousePos);
            
            bool mouseOverMenu = (mousePos.x >= g_MenuPos.x && mousePos.x <= g_MenuPos.x + g_MenuSize.x &&
                                  mousePos.y >= g_MenuPos.y && mousePos.y <= g_MenuPos.y + g_MenuSize.y);
            
            UpdateHitbox(mouseOverMenu, g.menuOpen);
        } else {
            UpdateHitbox(false, g.menuOpen);
        }

        if (g.menuOpen != lastMenu) {
            lastMenu = g.menuOpen;
        }

        ImGui::Render();
        g_Context->OMSetRenderTargets(1, &g_RTV, nullptr);
        g_Context->ClearRenderTargetView(g_RTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_SwapChain->Present(1, 0);
    }

    SaveConfig();
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    if (g_RTV) g_RTV->Release(); if (g_SwapChain) g_SwapChain->Release();
    if (g_Context) g_Context->Release(); if (g_Device) g_Device->Release();
    DestroyWindow(g_OverlayWnd); UnregisterClassW(cls, wc.hInstance);
    if (mem.handle) CloseHandle(mem.handle);
    return 0;
}
