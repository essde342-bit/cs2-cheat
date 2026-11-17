#include <Windows.h>
#include <TlHelp32.h>
#include <urlmon.h>
#include <d3d11.h>
#include <cstdio>
#include <string>
#include <atomic>
#include <thread>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#define ID_PAYLOAD 1
#define ID_LOGO    2
#define CURRENT_VERSION "1.0.0"
#define VERSION_URL "https://raw.githubusercontent.com/essde342-bit/cs2-cheat/main/version.json"

static ID3D11Device* g_Device = nullptr;
static ID3D11DeviceContext* g_Context = nullptr;
static IDXGISwapChain* g_SwapChain = nullptr;
static ID3D11RenderTargetView* g_RTV = nullptr;
static ID3D11ShaderResourceView* g_LogoSRV = nullptr;
static int g_LogoW = 0, g_LogoH = 0;
static HANDLE g_PayloadProc = nullptr;
static std::string g_PayloadPath;
static std::atomic<bool> g_Running{ true };
static std::atomic<bool> g_VersionChecked{ false };
static std::atomic<bool> g_VersionOk{ true };
static std::string g_VersionMessage, g_DownloadUrl;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static DWORD FindProcessId(const char* n) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe; pe.dwSize = sizeof(pe); DWORD pid = 0;
    if (Process32First(s, &pe)) do { if (!_stricmp(pe.szExeFile, n)) { pid = pe.th32ProcessID; break; } } while (Process32Next(s, &pe));
    CloseHandle(s); return pid;
}

static bool LoadEmbeddedResource(int id, void** d, DWORD* sz) {
    HRSRC hr = FindResourceA(nullptr, MAKEINTRESOURCEA(id), RT_RCDATA); if (!hr) return false;
    HGLOBAL hg = LoadResource(nullptr, hr); if (!hg) return false;
    *d = LockResource(hg); *sz = SizeofResource(nullptr, hr);
    return *d && *sz;
}

static bool CreateLogoTexture() {
    void* d = nullptr; DWORD sz = 0; if (!LoadEmbeddedResource(ID_LOGO, &d, &sz)) return false;
    int w=0,h=0,c=0; unsigned char* px = stbi_load_from_memory((const stbi_uc*)d, (int)sz, &w, &h, &c, 4);
    if (!px) return false;
    D3D11_TEXTURE2D_DESC td{}; td.Width=w; td.Height=h; td.MipLevels=1; td.ArraySize=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count=1; td.Usage=D3D11_USAGE_DEFAULT; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem=px; sd.SysMemPitch=w*4;
    ID3D11Texture2D* tex=nullptr; HRESULT hr = g_Device->CreateTexture2D(&td,&sd,&tex); stbi_image_free(px);
    if (FAILED(hr)) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format=td.Format; vd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels=1;
    HRESULT hr2 = g_Device->CreateShaderResourceView(tex,&vd,&g_LogoSRV); tex->Release();
    if (FAILED(hr2)) return false;
    g_LogoW=w; g_LogoH=h; return true;
}

// Random payload filename per launch: no fixed-path signature
static std::string GetTempPayloadPath() {
    char t[MAX_PATH]; GetTempPathA(MAX_PATH, t);
    char n[64]; sprintf_s(n, "void_%llu.exe", (unsigned long long)(GetTickCount64() ^ ((unsigned long long)GetCurrentProcessId() << 16)));
    return std::string(t) + n;
}

static bool LaunchPayload() {
    void* d = nullptr; DWORD sz = 0; if (!LoadEmbeddedResource(ID_PAYLOAD, &d, &sz)) return false;
    g_PayloadPath = GetTempPayloadPath();
    HANDLE f = CreateFileA(g_PayloadPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0; WriteFile(f, d, sz, &wr, nullptr); CloseHandle(f);
    if (wr != sz) return false;
    STARTUPINFOA si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, (char*)g_PayloadPath.c_str(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread); g_PayloadProc = pi.hProcess; return true;
}

static void UnloadPayload() {
    if (g_PayloadProc) { TerminateProcess(g_PayloadProc, 0); CloseHandle(g_PayloadProc); g_PayloadProc = nullptr; }
    DeleteFileA(g_PayloadPath.c_str());
}

static bool PayloadAlive() {
    if (!g_PayloadProc) return false;
    DWORD c = 0;
    if (GetExitCodeProcess(g_PayloadProc, &c) && c == STILL_ACTIVE) return true;
    CloseHandle(g_PayloadProc); g_PayloadProc = nullptr; return false;
}

static void CenterTextColored(const char* t, const ImVec4& c) {
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(t).x) * 0.5f);
    ImGui::TextColored(c, "%s", t);
}

static std::string ExtractJsonString(const std::string& j, const std::string& k) {
    size_t p = j.find("\"" + k + "\""); if (p == std::string::npos) return "";
    p = j.find(':', p); if (p == std::string::npos) return "";
    p = j.find('"', p + 1); if (p == std::string::npos) return "";
    size_t e = j.find('"', p + 1); if (e == std::string::npos) return "";
    return j.substr(p + 1, e - p - 1);
}

static void CheckVersion() {
    char t[MAX_PATH]; GetTempPathA(MAX_PATH, t);
    std::string path = std::string(t) + "void_ver.json";
    if (FAILED(URLDownloadToFileA(nullptr, VERSION_URL, path.c_str(), 0, nullptr))) { g_VersionOk = true; g_VersionChecked = true; return; }
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { g_VersionOk = true; g_VersionChecked = true; return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::string buf(sz, '\0'); fread(&buf[0], 1, sz, f); fclose(f); DeleteFileA(path.c_str());
    std::string rv = ExtractJsonString(buf, "version");
    g_VersionMessage = ExtractJsonString(buf, "message");
    g_DownloadUrl = ExtractJsonString(buf, "download_url");
    g_VersionOk = rv.empty() || rv == CURRENT_VERSION;
    g_VersionChecked = true;
}

int main() {
    CreateMutexA(nullptr, TRUE, "VoidDLC_Single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    std::thread(CheckVersion).detach();

    int winW = 480, winH = 380;
    int px = (GetSystemMetrics(SM_CXSCREEN)-winW)/2, py = (GetSystemMetrics(SM_CYSCREEN)-winH)/2;
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW|CS_VREDRAW; wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"VoidDLC_Loader"; RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Void DLC", WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX, px, py, winW, winH, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd);

    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount=2; sd.BufferDesc.Width=winW; sd.BufferDesc.Height=winH;
    sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferDesc.RefreshRate={60,1};
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow=hwnd; sd.SampleDesc.Count=1; sd.Windowed=TRUE;
    D3D_FEATURE_LEVEL fl;
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_SwapChain, &g_Device, &fl, &g_Context) != S_OK) return 1;
    ID3D11Texture2D* bb=nullptr; g_SwapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_Device->CreateRenderTargetView(bb, nullptr, &g_RTV); bb->Release();

    ImGui::CreateContext(); ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle(); st.WindowRounding=0; st.WindowPadding=ImVec2(20,20);
    st.Colors[ImGuiCol_WindowBg]=ImVec4(0,0,0,1);
    st.Colors[ImGuiCol_Button]=ImVec4(0.15f,0.35f,0.55f,1);
    st.Colors[ImGuiCol_ButtonHovered]=ImVec4(0.2f,0.45f,0.7f,1);
    st.Colors[ImGuiCol_ButtonActive]=ImVec4(0.1f,0.3f,0.5f,1);
    ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_Device, g_Context);
    CreateLogoTexture();

    const float clear[4] = { 0,0,0,1 };
    while (g_Running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_Running = false;
        }
        if (!g_Running) break;
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("##l", nullptr, ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoSavedSettings);
        if (g_LogoSRV) {
            float th=130, tw=th*((float)g_LogoW/g_LogoH);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth()-tw)*0.5f);
            ImGui::Image((ImTextureID)g_LogoSRV, ImVec2(tw,th)); ImGui::Spacing();
        }
        CenterTextColored("V O I D   D L C", ImVec4(0.85f,0.9f,1,1));
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); ImGui::Spacing();
        if (!g_VersionChecked) {
            CenterTextColored("status: checking for updates...", ImVec4(1,0.85f,0.3f,1));
        } else if (!g_VersionOk) {
            CenterTextColored("VERSION OUTDATED", ImVec4(1,0.2f,0.2f,1)); ImGui::Spacing();
            CenterTextColored(g_VersionMessage.empty() ? "Please update." : g_VersionMessage.c_str(), ImVec4(1,0.7f,0.7f,1));
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::SetCursorPosX((ImGui::GetWindowWidth()-220)*0.5f);
            if (ImGui::Button("DOWNLOAD UPDATE", ImVec2(220,38)) && !g_DownloadUrl.empty())
                ShellExecuteA(nullptr, "open", g_DownloadUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::Spacing(); CenterTextColored("current: v" CURRENT_VERSION, ImVec4(0.5f,0.5f,0.5f,1));
        } else {
            bool cs2 = FindProcessId("cs2.exe") != 0;
            bool alive = PayloadAlive();
            const char* s = alive ? "status: PAYLOAD RUNNING" : cs2 ? "status: CS2 running - ready" : "status: CS2 not detected";
            CenterTextColored(s, alive ? ImVec4(0.3f,1,0.4f,1) : cs2 ? ImVec4(1,0.85f,0.3f,1) : ImVec4(1,0.35f,0.35f,1));
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::SetCursorPosX((ImGui::GetWindowWidth()-316)*0.5f);
            if (ImGui::Button("LAUNCH", ImVec2(150,36)) && !alive && cs2) LaunchPayload();
            ImGui::SameLine();
            if (ImGui::Button("UNLOAD", ImVec2(150,36))) UnloadPayload();
            ImGui::Spacing(); ImGui::Spacing();
            CenterTextColored("closing loader does NOT stop the cheat", ImVec4(0.45f,0.45f,0.5f,1));
            CenterTextColored("v" CURRENT_VERSION, ImVec4(0.35f,0.35f,0.4f,1));
        }
        ImGui::End();
        ImGui::Render();
        g_Context->OMSetRenderTargets(1, &g_RTV, nullptr);
        g_Context->ClearRenderTargetView(g_RTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_SwapChain->Present(1, 0);
    }
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    if (g_LogoSRV) g_LogoSRV->Release(); if (g_RTV) g_RTV->Release();
    if (g_SwapChain) g_SwapChain->Release(); if (g_Context) g_Context->Release(); if (g_Device) g_Device->Release();
    DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
