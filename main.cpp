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

#define DW_ENTITY_LIST        0x2709010
#define DW_LOCAL_PLAYER_PAWN  0x2553D38
#define DW_VIEW_MATRIX        0x2559640
#define DW_VIEW_ANGLES        0x256A4B8
#define DW_SENSITIVITY        0x2551038
#define DW_SENS_OFF           0x58
#define M_H_PLAYER_PAWN       0x92C
#define M_I_HEALTH            0x344
#define M_I_TEAM_NUM          0x3E3
#define M_P_GAME_SCENE_NODE   0x328
#define M_ENTITY_SPOTTED_STATE 0x1438

static const float PI = 3.14159265358979f;
static const float R2D = 180.f / PI;

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct Mat4 { float m[4][4]; };

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
    int menuKeyIdx = 5;
    bool menuOpen = true;
};

static const char* KEY_NAMES[] = { "Всегда", "Mouse4", "Mouse5", "Alt", "Shift", "Ctrl", "CapsLock" };
static const int KEY_VKS[] = { 0, VK_XBUTTON1, VK_XBUTTON2, VK_MENU, VK_SHIFT, VK_CONTROL, VK_CAPITAL };
static const char* MENU_KEY_NAMES[] = { "Правый Shift", "Правый Ctrl", "Правый Alt", "Insert", "Home", "End", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
static const int MENU_KEY_VKS[] = { VK_RSHIFT, VK_RCONTROL, VK_RMENU, VK_INSERT, VK_HOME, VK_END, VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12 };
static const char* BONE_NAMES[] = { "Голова", "Шея", "Грудь", "Таз" };
static const int BONE_IDS[] = { 6, 5, 4, 0 };

static std::atomic<bool> g_Running{ true };
static Settings g;
static ID3D11Device* g_Device = nullptr;
static ID3D11DeviceContext* g_Context = nullptr;
static IDXGISwapChain* g_SwapChain = nullptr;
static ID3D11RenderTargetView* g_RTV = nullptr;
static HWND g_GameWnd = nullptr;
static std::string g_CfgPath;
static bool g_VisOk = false;

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
    WI("menu", "key", g.menuKeyIdx);
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
    g.menuKeyIdx = RI("menu", "key", g.menuKeyIdx);
    if (g.aimKeyIdx < 0 || g.aimKeyIdx > 6) g.aimKeyIdx = 2;
    if (g.aimBoneIdx < 0 || g.aimBoneIdx > 3) g.aimBoneIdx = 0;
    if (g.menuKeyIdx < 0 || g.menuKeyIdx >= (int)(sizeof(MENU_KEY_VKS)/sizeof(MENU_KEY_VKS[0]))) g.menuKeyIdx = 0;
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
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return true;
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
static void SetClickThrough(HWND h, bool on) {
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (on) {
        ex |= WS_EX_TRANSPARENT;
    } else {
        ex &= ~WS_EX_TRANSPARENT;
    }
    SetWindowLongPtrW(h, GWL_EXSTYLE, ex);
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
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
    HANDLE handle = nullptr; uintptr_t clientBase = 0;
    bool Init() {
        DWORD pid = FindProcessId("cs2.exe");
        if (!pid) { std::cout << "[!] cs2.exe not found\n"; return false; }
        HANDLE hq = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!hq) { std::cout << "[!] OpenProcess(query) failed\n"; return false; }
        HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (s != INVALID_HANDLE_VALUE) {
            MODULEENTRY32 me; me.dwSize = sizeof(me);
            if (Module32First(s, &me)) do {
                if (!_stricmp(me.szModule, "client.dll")) { clientBase = (uintptr_t)me.modBaseAddr; break; }
            } while (Module32Next(s, &me));
            CloseHandle(s);
        }
        CloseHandle(hq);
        if (!clientBase) { std::cout << "[!] client.dll not found\n"; return false; }
        handle = OpenProcess(PROCESS_VM_READ, FALSE, pid);
        if (!handle) { std::cout << "[!] OpenProcess(read) failed\n"; return false; }
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
        Vec3 a = Read<Vec3>(clientBase + DW_VIEW_ANGLES);
        if (std::isfinite(a.x) && std::isfinite(a.y) && fabsf(a.x) <= 90.f && fabsf(a.y) <= 360.f) { out = a; return true; }
        uintptr_t p = Read<uintptr_t>(clientBase + DW_VIEW_ANGLES);
        if (p) { a = Read<Vec3>(p); if (std::isfinite(a.x) && std::isfinite(a.y) && fabsf(a.x) <= 90.f && fabsf(a.y) <= 360.f) { out = a; return true; } }
        return false;
    }
    float ReadSens() const {
        uintptr_t p = Read<uintptr_t>(clientBase + DW_SENSITIVITY);
        if (p) { float v = Read<float>(p + DW_SENS_OFF); if (std::isfinite(v) && v > 0.05f && v < 20.f) return v; }
        float v2 = Read<float>(clientBase + DW_SENSITIVITY);
        if (std::isfinite(v2) && v2 > 0.05f && v2 < 20.f) return v2;
        return -1.f;
    }
    bool ReadVisible(uintptr_t pawn, int localIndex, bool& out) const {
        if (localIndex < 0 || localIndex > 63) return false;
        uint8_t spotted = Read<uint8_t>(pawn + M_ENTITY_SPOTTED_STATE + 8);
        if (spotted > 1) return false;
        uint32_t m0 = Read<uint32_t>(pawn + M_ENTITY_SPOTTED_STATE + 12);
        uint32_t m1 = Read<uint32_t>(pawn + M_ENTITY_SPOTTED_STATE + 16);
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
    ImGui::Begin("Void DLC", nullptr, ImGuiWindowFlags_NoCollapse);
    ImGui::TextColored(ImVec4(0.6f,0.8f,1,1), "%s - меню | v%s", MENU_KEY_NAMES[g.menuKeyIdx], CURRENT_VERSION);
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
            ImGui::TextUnformatted("Кнопка меню:");
            ImGui::Combo("##menukey", &g.menuKeyIdx, MENU_KEY_NAMES, IM_ARRAYSIZE(MENU_KEY_NAMES)); ImGui::SameLine(); Help("Кнопка для открытия/закрытия этого меню. Правый Shift по умолчанию.");
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
    
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT, cls, L"", WS_POPUP, rc.left, rc.top, winW, winH, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { system("pause"); return 1; }
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    MARGINS mg{ -1,-1,-1,-1 }; DwmExtendFrameIntoClientArea(hwnd, &mg);
    ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd);
    SetClickThrough(hwnd, false);

    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Width = winW; sd.BufferDesc.Height = winH;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferDesc.RefreshRate = { 60,1 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1;
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
    ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_Device, g_Context);

    const float clear[4] = { 0,0,0,0 };
    static const int kSkel[][2] = { {6,5},{5,4},{4,2},{2,0},{4,8},{8,9},{9,10},{4,13},{13,14},{14,15},{0,22},{22,23},{23,24},{0,25},{25,26},{26,27} };
    bool lastMenu = false, prevAimKey = false, aimToggled = false, prevMenuKey = false;
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

        int menuVK = MENU_KEY_VKS[g.menuKeyIdx];
        bool menuKeyDown = (GetAsyncKeyState(menuVK) & 0x8000) != 0;
        if (menuKeyDown && !prevMenuKey) g.menuOpen = !g.menuOpen;
        prevMenuKey = menuKeyDown;

        if (g.menuOpen != lastMenu) { SetClickThrough(hwnd, !g.menuOpen); lastMenu = g.menuOpen; }

        DWORD now = GetTickCount();
        bool foreground = (GetForegroundWindow() == g_GameWnd);
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

        uintptr_t el = mem.Read<uintptr_t>(mem.clientBase + DW_ENTITY_LIST);
        uintptr_t lp = mem.Read<uintptr_t>(mem.clientBase + DW_LOCAL_PLAYER_PAWN);
        int lt = lp ? mem.Read<int>(lp + M_I_TEAM_NUM) : 0;
        Vec3 lo{};
        if (lp) { uintptr_t ls = mem.Read<uintptr_t>(lp + M_P_GAME_SCENE_NODE); if (ls) mem.ReadOriginSmart(ls, lo); }
        Vec3 eye{ lo.x, lo.y, lo.z + g.eyeHeight };
        Mat4 vm = mem.Read<Mat4>(mem.clientBase + DW_VIEW_MATRIX);
        bool needBones = g.skeleton || g.aimEnabled || g.trigEnabled;
        g_VisOk = false; localIndex = -1;

        if (g.espEnabled || g.aimEnabled || g.trigEnabled) {
            for (int i = 1; i <= 256; ++i) {
                uintptr_t entity = mem.GetListEntity(el, i);
                if (!entity) continue;

                uintptr_t pw = 0;
                bool isBot = false;

                uint32_t ph = mem.Read<uint32_t>(entity + M_H_PLAYER_PAWN);
                if (ph) {
                    pw = mem.GetListEntity(el, ph & 0x7FFF);
                }

                if (!pw) {
                    int testHP = mem.Read<int>(entity + M_I_HEALTH);
                    if (testHP > 0 && testHP <= 100) {
                        uintptr_t testSN = mem.Read<uintptr_t>(entity + M_P_GAME_SCENE_NODE);
                        if (testSN) {
                            pw = entity;
                            isBot = true;
                        }
                    }
                }

                if (!pw) continue;
                if (pw == lp) { localIndex = i; continue; }

                int hp = mem.Read<int>(pw + M_I_HEALTH);
                if (hp <= 0 || hp > 100) continue;

                int tm = mem.Read<int>(pw + M_I_TEAM_NUM);
                if (!g.showTeam && tm == lt) continue;

                if (isBot && !g.showBots) continue;

                uintptr_t sn = mem.Read<uintptr_t>(pw + M_P_GAME_SCENE_NODE);
                if (!sn) continue;
                PInfo p{}; p.pawn = pw; p.hp = hp; p.visible = true;
                if (!mem.ReadOriginSmart(sn, p.origin)) continue;
                if (g.maxDistance > 0) {
                    float dx = p.origin.x-lo.x, dy = p.origin.y-lo.y, dz = p.origin.z-lo.z;
                    if (sqrtf(dx*dx+dy*dy+dz*dz) > g.maxDistance) continue;
                }
                if (g.visEnabled && !isBot) { bool ok = mem.ReadVisible(pw, localIndex, p.visible); if (ok) g_VisOk = true; }
                p.hasBones = needBones ? mem.ReadBonesSmart(sn, p.origin, p.bones) : false;
                int bid = BONE_IDS[g.aimBoneIdx];
                p.bonePos = p.hasBones ? Vec3{ p.bones[bid][0][3], p.bones[bid][1][3], p.bones[bid][2][3] }
                                       : Vec3{ p.origin.x, p.origin.y, p.origin.z + 60.f };
                Vec3 d{ p.bonePos.x-eye.x, p.bonePos.y-eye.y, p.bonePos.z-eye.z };
                float hyp = sqrtf(d.x*d.x + d.y*d.y);
                p.pitch = atan2f(-d.z, hyp) * R2D;
                p.yaw = atan2f(d.y, d.x) * R2D;
                p.angDist = 9999.f;
                if (haveAngles) { float dyaw = NormYaw(p.yaw - curAngles.y); float dp = p.pitch - curAngles.x; p.angDist = sqrtf(dyaw*dyaw + dp*dp); }
                Vec3 hw = p.hasBones ? Vec3{ p.bones[6][0][3], p.bones[6][1][3], p.bones[6][2][3] } : Vec3{ p.origin.x, p.origin.y, p.origin.z + 75.f };
                Vec3 fw = p.origin; fw.z -= 2.f;
                if (W2S(hw, p.sHead, vm, winW, winH) && W2S(fw, p.sFeet, vm, winW, winH)) players.push_back(p);
            }
        }

        PInfo* target = nullptr;
        if (haveAngles) for (auto& p : players) {
            if (g.visEnabled && g_VisOk && !p.visible) continue;
            if (p.angDist < g.aimFov && (!target || p.angDist < target->angDist)) target = &p;
        }
        if (target && target->pawn != lastTarget) { lastTarget = target->pawn; acquireAt = now; }
        if (!target) lastTarget = 0;

        bool canAct = foreground && !g.menuOpen;
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

        if (g.menuOpen) RenderMenu();

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
    DestroyWindow(hwnd); UnregisterClassW(cls, wc.hInstance);
    if (mem.handle) CloseHandle(mem.handle);
    return 0;
}
