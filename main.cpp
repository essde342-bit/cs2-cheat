#include <Windows.h>
#include <TlHelp32.h>
#include <d3d11.h>
#include <cstdio>
#include <cmath>
#include <iostream>
#include <atomic>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// Твои найденные глобальные оффсеты
#define DW_ENTITY_LIST          0x2709010
#define DW_LOCAL_PLAYER_PAWN    0x2553D38
#define DW_VIEW_MATRIX          0x2559640

// Структурные оффсеты (из схемы классов, меняются редко)
#define M_I_HEALTH              0x344
#define M_I_TEAM_NUM            0x3E3
#define M_LIFE_STATE            0x348
#define M_P_GAME_SCENE_NODE     0x328
#define M_H_PLAYER_PAWN         0x7FC

#define HOTKEY_MENU_ID 1
#define HOTKEY_MENU_KEY VK_INSERT

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct Mat4 { float m[4][4]; };

struct PlayerInfo {
    Vec3 origin;
    float bones[64][3][4];
    int health;
    int team;
};

struct Settings {
    bool espEnabled = true;
    bool box = true;
    bool skeleton = true;
    bool healthBar = true;
    bool hpText = true;
    bool snaplines = false;
    bool showTeam = false;
    float maxDistance = 5000.f;
    float boxThickness = 2.f;
    float boxColor[4] = { 1.f, 0.f, 0.f, 1.f };
    float skelColor[4] = { 1.f, 1.f, 1.f, 1.f };
    float snapColor[4] = { 1.f, 1.f, 0.f, 1.f };
    bool menuOpen = false;
};

static std::atomic<bool> g_Running{ true };
static Settings g_Settings;
static ID3D11Device* g_Device = nullptr;
static ID3D11DeviceContext* g_Context = nullptr;
static IDXGISwapChain* g_SwapChain = nullptr;
static ID3D11RenderTargetView* g_RTV = nullptr;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return 1;
    switch (msg) {
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_SIZE:
        if (g_Device && g_SwapChain && wParam != SIZE_MINIMIZED) {
            if (g_RTV) { g_RTV->Release(); g_RTV = nullptr; }
            g_SwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            ID3D11Texture2D* backBuf = nullptr;
            g_SwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf));
            g_Device->CreateRenderTargetView(backBuf, nullptr, &g_RTV);
            backBuf->Release();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void SetClickThrough(HWND hwnd, bool enabled) {
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (enabled) ex |= WS_EX_TRANSPARENT; else ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
}

static ImU32 ToU32(const float c[4]) {
    return IM_COL32((int)(c[0]*255), (int)(c[1]*255), (int)(c[2]*255), (int)(c[3]*255));
}

static DWORD FindProcessId(const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe; pe.dwSize = sizeof(pe); DWORD pid = 0;
    if (Process32First(snap, &pe)) {
        do { if (_stricmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; } } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap); return pid;
}

struct WindowData { DWORD pid; HWND hwnd; };
static BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    auto* wd = reinterpret_cast<WindowData*>(lParam); DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == wd->pid && IsWindowVisible(hwnd)) { wd->hwnd = hwnd; return FALSE; }
    return TRUE;
}

static HWND FindWindowByPid(DWORD pid) {
    WindowData wd{ pid, nullptr };
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&wd));
    return wd.hwnd;
}

class Memory {
public:
    HANDLE handle = nullptr; uintptr_t clientBase = 0; size_t clientSize = 0;

    bool Init() {
        DWORD pid = FindProcessId("cs2.exe"); if (!pid) return false;
        handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid); if (!handle) return false;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) return false;
        MODULEENTRY32 me; me.dwSize = sizeof(me);
        if (Module32First(snap, &me)) {
            do { if (_stricmp(me.szModule, "client.dll") == 0) { clientBase = reinterpret_cast<uintptr_t>(me.modBaseAddr); clientSize = me.modBaseSize; break; } } while (Module32Next(snap, &me));
        }
        CloseHandle(snap); return clientBase != 0;
    }

    template<typename T> T Read(uintptr_t addr) const {
        T val{}; ReadProcessMemory(handle, reinterpret_cast<LPCVOID>(addr), &val, sizeof(T), nullptr); return val;
    }

    bool ReadRaw(uintptr_t addr, void* buf, size_t size) const {
        return ReadProcessMemory(handle, reinterpret_cast<LPCVOID>(addr), buf, size, nullptr) == TRUE;
    }

    uintptr_t GetListEntity(uintptr_t listBase, int index) const {
        uintptr_t chunk = Read<uintptr_t>(listBase + 8 * ((index & 0x7FFF) >> 9) + 16);
        if (!chunk) return 0; return Read<uintptr_t>(chunk + 120 * (index & 0x3FF));
    }
};

static bool W2S(const Vec3& w, Vec2& s, const Mat4& mat, int sw, int sh) {
    float ww = mat.m[3][0]*w.x + mat.m[3][1]*w.y + mat.m[3][2]*w.z + mat.m[3][3];
    if (ww < 0.01f) return false;
    float inv = 1.0f / ww;
    s.x = sw * 0.5f + (mat.m[0][0]*w.x + mat.m[0][1]*w.y + mat.m[0][2]*w.z + mat.m[0][3]) * inv * sw * 0.5f;
    s.y = sh * 0.5f - (mat.m[1][0]*w.x + mat.m[1][1]*w.y + mat.m[1][2]*w.z + mat.m[1][3]) * inv * sh * 0.5f;
    return true;
}

static void RenderMenu() {
    ImGui::SetNextWindowSize(ImVec2(380, 460), ImGuiCond_FirstUseEver);
    ImGui::Begin("CS2 ESP", &g_Settings.menuOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.f, 1.f), "INSERT - show/hide menu"); ImGui::Separator();
    ImGui::Checkbox("Enable ESP", &g_Settings.espEnabled); ImGui::Separator();
    ImGui::Checkbox("Box", &g_Settings.box);
    if (g_Settings.box) { ImGui::Indent(16.f); ImGui::ColorEdit4("Box color", g_Settings.boxColor, ImGuiColorEditFlags_NoInputs); ImGui::SliderFloat("Box thickness", &g_Settings.boxThickness, 1.f, 5.f, "%.1f"); ImGui::Unindent(16.f); }
    ImGui::Checkbox("Skeleton", &g_Settings.skeleton);
    if (g_Settings.skeleton) { ImGui::Indent(16.f); ImGui::ColorEdit4("Skeleton color", g_Settings.skelColor, ImGuiColorEditFlags_NoInputs); ImGui::Unindent(16.f); }
    ImGui::Checkbox("Health bar", &g_Settings.healthBar); ImGui::Checkbox("HP text", &g_Settings.hpText);
    ImGui::Checkbox("Snaplines", &g_Settings.snaplines);
    if (g_Settings.snaplines) { ImGui::Indent(16.f); ImGui::ColorEdit4("Snapline color", g_Settings.snapColor, ImGuiColorEditFlags_NoInputs); ImGui::Unindent(16.f); }
    ImGui::Separator(); ImGui::Checkbox("Show teammates", &g_Settings.showTeam);
    ImGui::SliderFloat("Max distance (0 = inf)", &g_Settings.maxDistance, 0.f, 10000.f, "%.0f");
    ImGui::End();
}

int main() {
    Memory mem;
    if (!mem.Init()) { system("pause"); return 1; }

    DWORD pid = FindProcessId("cs2.exe");
    HWND gameWnd = FindWindowByPid(pid);
    if (!gameWnd) { system("pause"); return 1; }

    RECT rc; GetWindowRect(gameWnd, &rc);
    int winW = rc.right - rc.left, winH = rc.bottom - rc.top;

    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW; wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"CS2ESP_Class"; RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED, wc.lpszClassName, L"CS2 ESP", WS_POPUP, rc.left, rc.top, winW, winH, nullptr, nullptr, wc.hInstance, nullptr);
    SetLayeredWindowAttributes(hwnd, RGB(0,0,0), 255, LWA_ALPHA); ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd);
    RegisterHotKey(nullptr, HOTKEY_MENU_ID, MOD_NOREPEAT, HOTKEY_MENU_KEY);

    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Width = winW; sd.BufferDesc.Height = winH;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferDesc.RefreshRate = { 60, 1 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    D3D_FEATURE_LEVEL fl;
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_SwapChain, &g_Device, &fl, &g_Context) != S_OK) return 1;

    ID3D11Texture2D* backBuf = nullptr; g_SwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf));
    g_Device->CreateRenderTargetView(backBuf, nullptr, &g_RTV); backBuf->Release();

    ImGui::CreateContext(); ImGui::StyleColorsDark(); ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_Device, g_Context);
    const float clear[4] = { 0, 0, 0, 0 };
    static const int kSkeleton[][2] = { {6,5},{5,4},{4,2},{2,0}, {4,8},{8,9},{9,10}, {4,13},{13,14},{14,15}, {0,22},{22,23},{23,24}, {0,25},{25,26},{26,27} };
    bool lastMenuState = false;

    while (g_Running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_Running = false;
            if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_MENU_ID) g_Settings.menuOpen = !g_Settings.menuOpen;
        }
        if (!g_Running) break;

        if (g_Settings.menuOpen != lastMenuState) { SetClickThrough(hwnd, !g_Settings.menuOpen); lastMenuState = g_Settings.menuOpen; }

        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        auto* dl = ImGui::GetBackgroundDrawList();

        if (g_Settings.espEnabled) {
            uintptr_t entityList = mem.Read<uintptr_t>(mem.clientBase + DW_ENTITY_LIST);
            uintptr_t localPawn = mem.Read<uintptr_t>(mem.clientBase + DW_LOCAL_PLAYER_PAWN);
            int localTeam = localPawn ? mem.Read<int>(localPawn + M_I_TEAM_NUM) : 0;
            Vec3 localOrigin{}; Mat4 vm = mem.Read<Mat4>(mem.clientBase + DW_VIEW_MATRIX);
            ImU32 boxCol = ToU32(g_Settings.boxColor), skelCol = ToU32(g_Settings.skelColor), snapCol = ToU32(g_Settings.snapColor);

            for (int i = 1; i <= 64; ++i) {
                uintptr_t controller = mem.GetListEntity(entityList, i); if (!controller) continue;
                uint32_t pawnHandle = mem.Read<uint32_t>(controller + M_H_PLAYER_PAWN); if (!pawnHandle) continue;
                uintptr_t pawn = mem.GetListEntity(entityList, pawnHandle & 0x7FFF);
                if (!pawn || pawn == localPawn) continue;

                PlayerInfo p;
                p.health = mem.Read<int>(pawn + M_I_HEALTH);
                if (p.health <= 0 || mem.Read<uint8_t>(pawn + M_LIFE_STATE) != 0) continue;
                p.team = mem.Read<int>(pawn + M_I_TEAM_NUM);
                if (!g_Settings.showTeam && p.team == localTeam) continue;

                uintptr_t sceneNode = mem.Read<uintptr_t>(pawn + M_P_GAME_SCENE_NODE);
                if (!sceneNode) continue;
                
                mem.ReadRaw(sceneNode + 0x90, &p.origin, sizeof(p.origin));
                
                if (g_Settings.maxDistance > 0.f) {
                    float dx = p.origin.x - localOrigin.x, dy = p.origin.y - localOrigin.y, dz = p.origin.z - localOrigin.z;
                    if (sqrtf(dx*dx + dy*dy + dz*dz) > g_Settings.maxDistance) continue;
                }

                uintptr_t boneArray = mem.Read<uintptr_t>(sceneNode + 0x80);
                if (!boneArray) continue;
                mem.ReadRaw(boneArray, p.bones, sizeof(p.bones));

                Vec3 headWorld{ p.bones[6][0][3], p.bones[6][1][3], p.bones[6][2][3] };
                Vec3 feetWorld = p.origin; feetWorld.z -= 2.0f;
                Vec2 sHead{}, sFeet{};
                if (!W2S(headWorld, sHead, vm, winW, winH) || !W2S(feetWorld, sFeet, vm, winW, winH)) continue;
                float h = sFeet.y - sHead.y; if (h < 5.0f) continue;
                float w = h * 0.35f, boxTop = sHead.y - h * 0.06f;

                if (g_Settings.snaplines) dl->AddLine(ImVec2(winW*0.5f, (float)winH), ImVec2(sFeet.x, sFeet.y), snapCol, 1.f);
                if (g_Settings.box) dl->AddRect(ImVec2(sHead.x - w*0.5f, boxTop), ImVec2(sHead.x + w*0.5f, sFeet.y), boxCol, 0.f, 0, g_Settings.boxThickness);
                
                if (g_Settings.healthBar) {
                    float hpF = static_cast<float>(p.health) / 100.f; if (hpF > 1.f) hpF = 1.f;
                    dl->AddRectFilled(ImVec2(sHead.x - w*0.5f - 6, boxTop + (sFeet.y - boxTop) * (1.f - hpF)), ImVec2(sHead.x - w*0.5f - 2, sFeet.y), IM_COL32(0, 255, 0, 255));
                    dl->AddRect(ImVec2(sHead.x - w*0.5f - 6, boxTop), ImVec2(sHead.x - w*0.5f - 2, sFeet.y), IM_COL32(255, 255, 255, 255), 0.f, 0, 1.f);
                }

                if (g_Settings.skeleton) {
                    for (auto& b : kSkeleton) {
                        Vec3 p1{ p.bones[b[0]][0][3], p.bones[b[0]][1][3], p.bones[b[0]][2][3] };
                        Vec3 p2{ p.bones[b[1]][0][3], p.bones[b[1]][1][3], p.bones[b[1]][2][3] };
                        Vec2 s1{}, s2{};
                        if (W2S(p1, s1, vm, winW, winH) && W2S(p2, s2, vm, winW, winH))
                            dl->AddLine(ImVec2(s1.x, s1.y), ImVec2(s2.x, s2.y), skelCol, 1.5f);
                    }
                }
                if (g_Settings.hpText) { char txt[16]; sprintf_s(txt, "%d", p.health); dl->AddText(ImVec2(sHead.x - 8, boxTop - 16), IM_COL32(255, 255, 255, 255), txt); }
            }
        }

        if (g_Settings.menuOpen) RenderMenu();

        ImGui::Render(); g_Context->OMSetRenderTargets(1, &g_RTV, nullptr); g_Context->ClearRenderTargetView(g_RTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); g_SwapChain->Present(1, 0);
    }

    UnregisterHotKey(nullptr, HOTKEY_MENU_ID); ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    if (g_RTV) g_RTV->Release(); if (g_SwapChain) g_SwapChain->Release(); if (g_Context) g_Context->Release(); if (g_Device) g_Device->Release();
    DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, wc.hInstance); if (mem.handle) CloseHandle(mem.handle);
    return 0;
}
