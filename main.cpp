#include <Windows.h>
#include <TlHelp32.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <urlmon.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <atomic>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#define CURRENT_VERSION "1.0.0"
#define VERSION_URL "https://raw.githubusercontent.com/essde342-bit/cs2-cheat/main/version.json"

#define DW_ENTITY_LIST          0x2709010
#define DW_LOCAL_PLAYER_PAWN    0x2553D38
#define DW_VIEW_MATRIX          0x2559640
#define M_H_PLAYER_PAWN         0x92C
#define M_I_HEALTH              0x344
#define M_I_TEAM_NUM            0x3E3
#define M_P_GAME_SCENE_NODE     0x328

#define HOTKEY_MENU_ID 1
#define HOTKEY_MENU_KEY VK_INSERT

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct Mat4 { float m[4][4]; };

struct Settings {
    bool espEnabled = true, box = true, skeleton = true, healthBar = true, hpText = true;
    bool snaplines = false, showTeam = false;
    float maxDistance = 5000.f, boxThickness = 2.f;
    float boxColor[4] = { 1,0,0,1 }, skelColor[4] = { 1,1,1,1 }, snapColor[4] = { 1,1,0,1 };
    bool menuOpen = false;
};

static std::atomic<bool> g_Running{ true };
static Settings g_Settings;
static ID3D11Device* g_Device = nullptr;
static ID3D11DeviceContext* g_Context = nullptr;
static IDXGISwapChain* g_SwapChain = nullptr;
static ID3D11RenderTargetView* g_RTV = nullptr;

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
    if (on) ex |= WS_EX_TRANSPARENT; else ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(h, GWL_EXSTYLE, ex);
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

        // Stage 1: query+read only long enough to locate client.dll
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
        CloseHandle(hq);   // drop query rights immediately
        if (!clientBase) { std::cout << "[!] client.dll not found\n"; return false; }

        // Stage 2: minimal read-only handle for the whole session
        handle = OpenProcess(PROCESS_VM_READ, FALSE, pid);
        if (!handle) { std::cout << "[!] OpenProcess(read) failed\n"; return false; }
        std::cout << "[+] client.dll @ 0x" << std::hex << clientBase << std::dec << " (VM_READ only)\n";
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
};

static bool W2S(const Vec3& w, Vec2& s, const Mat4& m, int sw, int sh) {
    float ww = m.m[3][0]*w.x + m.m[3][1]*w.y + m.m[3][2]*w.z + m.m[3][3];
    if (ww < 0.01f) return false;
    float inv = 1.f / ww;
    s.x = sw*0.5f + (m.m[0][0]*w.x + m.m[0][1]*w.y + m.m[0][2]*w.z + m.m[0][3]) * inv * sw*0.5f;
    s.y = sh*0.5f - (m.m[1][0]*w.x + m.m[1][1]*w.y + m.m[1][2]*w.z + m.m[1][3]) * inv * sh*0.5f;
    return true;
}

static void RenderMenu() {
    ImGui::SetNextWindowSize(ImVec2(380, 460), ImGuiCond_FirstUseEver);
    ImGui::Begin("Void DLC", &g_Settings.menuOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);
    ImGui::TextColored(ImVec4(0.6f,0.8f,1,1), "INSERT - show/hide menu"); ImGui::Separator();
    ImGui::Checkbox("Enable ESP", &g_Settings.espEnabled); ImGui::Separator();
    ImGui::Checkbox("Box", &g_Settings.box);
    if (g_Settings.box) { ImGui::Indent(16); ImGui::ColorEdit4("Box color", g_Settings.boxColor, ImGuiColorEditFlags_NoInputs); ImGui::SliderFloat("Box thickness", &g_Settings.boxThickness, 1, 5, "%.1f"); ImGui::Unindent(16); }
    ImGui::Checkbox("Skeleton", &g_Settings.skeleton);
    if (g_Settings.skeleton) { ImGui::Indent(16); ImGui::ColorEdit4("Skeleton color", g_Settings.skelColor, ImGuiColorEditFlags_NoInputs); ImGui::Unindent(16); }
    ImGui::Checkbox("Health bar", &g_Settings.healthBar); ImGui::Checkbox("HP text", &g_Settings.hpText);
    ImGui::Checkbox("Snaplines", &g_Settings.snaplines);
    if (g_Settings.snaplines) { ImGui::Indent(16); ImGui::ColorEdit4("Snapline color", g_Settings.snapColor, ImGuiColorEditFlags_NoInputs); ImGui::Unindent(16); }
    ImGui::Separator(); ImGui::Checkbox("Show teammates", &g_Settings.showTeam);
    ImGui::SliderFloat("Max distance (0 = inf)", &g_Settings.maxDistance, 0, 10000, "%.0f");
    ImGui::End();
}

int main() {
    SetConsoleTitleA("console");
    if (!CheckPayloadVersion()) { std::cout << "[!] outdated\n"; system("pause"); return 1; }

    Memory mem; if (!mem.Init()) { system("pause"); return 1; }
    DWORD pid = FindProcessId("cs2.exe");
    HWND gw = FindWindowByPid(pid); if (!gw) { system("pause"); return 1; }
    RECT rc; GetWindowRect(gw, &rc);
    int winW = rc.right-rc.left, winH = rc.bottom-rc.top;

    // Random window class per launch: no fixed class-name signature
    wchar_t cls[64]; swprintf_s(cls, L"VD_%u_%llu", GetCurrentProcessId(), (unsigned long long)GetTickCount64());

    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW|CS_VREDRAW; wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = cls; RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST|WS_EX_TRANSPARENT, cls, L"", WS_POPUP, rc.left, rc.top, winW, winH, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { system("pause"); return 1; }
    MARGINS mg = { -1,-1,-1,-1 }; DwmExtendFrameIntoClientArea(hwnd, &mg);
    ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd);
    RegisterHotKey(nullptr, HOTKEY_MENU_ID, MOD_NOREPEAT, HOTKEY_MENU_KEY);

    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Width = winW; sd.BufferDesc.Height = winH;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferDesc.RefreshRate = { 60,1 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_SwapChain, &g_Device, &fl, &g_Context) != S_OK) { system("pause"); return 1; }
    ID3D11Texture2D* bb = nullptr; g_SwapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_Device->CreateRenderTargetView(bb, nullptr, &g_RTV); bb->Release();

    ImGui::CreateContext(); ImGui::StyleColorsDark(); ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_Device, g_Context);
    const float clear[4] = { 0,0,0,0 };
    static const int kSkel[][2] = { {6,5},{5,4},{4,2},{2,0},{4,8},{8,9},{9,10},{4,13},{13,14},{14,15},{0,22},{22,23},{23,24},{0,25},{25,26},{26,27} };
    bool lastMenu = false;

    while (g_Running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_Running = false;
            if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_MENU_ID) g_Settings.menuOpen = !g_Settings.menuOpen;
        }
        if (!g_Running) break;
        if (g_Settings.menuOpen != lastMenu) { SetClickThrough(hwnd, !g_Settings.menuOpen); lastMenu = g_Settings.menuOpen; }

        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        auto* dl = ImGui::GetBackgroundDrawList();
        int drawn = 0;

        if (g_Settings.espEnabled) {
            uintptr_t el = mem.Read<uintptr_t>(mem.clientBase + DW_ENTITY_LIST);
            uintptr_t lp = mem.Read<uintptr_t>(mem.clientBase + DW_LOCAL_PLAYER_PAWN);
            int lt = lp ? mem.Read<int>(lp + M_I_TEAM_NUM) : 0;
            Vec3 lo{};
            if (lp) { uintptr_t ls = mem.Read<uintptr_t>(lp + M_P_GAME_SCENE_NODE); if (ls) mem.ReadOriginSmart(ls, lo); }
            Mat4 vm = mem.Read<Mat4>(mem.clientBase + DW_VIEW_MATRIX);
            ImU32 bc = ToU32(g_Settings.boxColor), sc = ToU32(g_Settings.skelColor), nc = ToU32(g_Settings.snapColor);

            for (int i = 1; i <= 64; ++i) {
                uintptr_t ctl = mem.GetListEntity(el, i); if (!ctl) continue;
                uint32_t ph = mem.Read<uint32_t>(ctl + M_H_PLAYER_PAWN); if (!ph) continue;
                uintptr_t pw = mem.GetListEntity(el, ph & 0x7FFF); if (!pw || pw == lp) continue;
                int hp = mem.Read<int>(pw + M_I_HEALTH); if (hp <= 0 || hp > 100) continue;
                int tm = mem.Read<int>(pw + M_I_TEAM_NUM); if (!g_Settings.showTeam && tm == lt) continue;
                uintptr_t sn = mem.Read<uintptr_t>(pw + M_P_GAME_SCENE_NODE); if (!sn) continue;
                Vec3 org{}; if (!mem.ReadOriginSmart(sn, org)) continue;
                if (g_Settings.maxDistance > 0) {
                    float dx = org.x-lo.x, dy = org.y-lo.y, dz = org.z-lo.z;
                    if (sqrtf(dx*dx+dy*dy+dz*dz) > g_Settings.maxDistance) continue;
                }
                // read bones ONLY when skeleton is on: less read volume
                float bones[64][3][4];
                bool hb = g_Settings.skeleton ? mem.ReadBonesSmart(sn, org, bones) : false;
                Vec3 hw = hb ? Vec3{ bones[6][0][3], bones[6][1][3], bones[6][2][3] } : Vec3{ org.x, org.y, org.z + 75.f };
                Vec3 fw = org; fw.z -= 2.f;
                Vec2 sh{}, sf{};
                if (!W2S(hw, sh, vm, winW, winH) || !W2S(fw, sf, vm, winW, winH)) continue;
                float h = sf.y - sh.y; if (h < 5) continue;
                float w = h * 0.35f, bt = sh.y - h * 0.06f;
                drawn++;
                if (g_Settings.snaplines) dl->AddLine(ImVec2(winW*0.5f,(float)winH), ImVec2(sf.x,sf.y), nc, 1);
                if (g_Settings.box) dl->AddRect(ImVec2(sh.x-w*0.5f,bt), ImVec2(sh.x+w*0.5f,sf.y), bc, 0, 0, g_Settings.boxThickness);
                if (g_Settings.healthBar) {
                    float f = hp / 100.f;
                    dl->AddRectFilled(ImVec2(sh.x-w*0.5f-6, bt+(sf.y-bt)*(1-f)), ImVec2(sh.x-w*0.5f-2, sf.y), IM_COL32(0,255,0,255));
                    dl->AddRect(ImVec2(sh.x-w*0.5f-6, bt), ImVec2(sh.x-w*0.5f-2, sf.y), IM_COL32(255,255,255,255), 0, 0, 1);
                }
                if (g_Settings.skeleton && hb) for (auto& b : kSkel) {
                    Vec3 p1{ bones[b[0]][0][3], bones[b[0]][1][3], bones[b[0]][2][3] };
                    Vec3 p2{ bones[b[1]][0][3], bones[b[1]][1][3], bones[b[1]][2][3] };
                    Vec2 s1{}, s2{};
                    if (W2S(p1,s1,vm,winW,winH) && W2S(p2,s2,vm,winW,winH)) dl->AddLine(ImVec2(s1.x,s1.y), ImVec2(s2.x,s2.y), sc, 1.5f);
                }
                if (g_Settings.hpText) { char t[16]; sprintf_s(t, "%d", hp); dl->AddText(ImVec2(sh.x-8, bt-16), IM_COL32(255,255,255,255), t); }
            }
        }

        char dbg[96]; sprintf_s(dbg, "Void DLC v%s | drawn: %d", CURRENT_VERSION, drawn);
        dl->AddText(ImVec2(8,8), IM_COL32(0,255,128,200), dbg);
        if (g_Settings.menuOpen) RenderMenu();

        ImGui::Render();
        g_Context->OMSetRenderTargets(1, &g_RTV, nullptr);
        g_Context->ClearRenderTargetView(g_RTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_SwapChain->Present(1, 0);
    }

    UnregisterHotKey(nullptr, HOTKEY_MENU_ID);
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    if (g_RTV) g_RTV->Release(); if (g_SwapChain) g_SwapChain->Release();
    if (g_Context) g_Context->Release(); if (g_Device) g_Device->Release();
    DestroyWindow(hwnd); UnregisterClassW(cls, wc.hInstance);
    if (mem.handle) CloseHandle(mem.handle);
    return 0;
}
