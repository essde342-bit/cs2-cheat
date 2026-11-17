#include <Windows.h>
#include <TlHelp32.h>
#include <d3d11.h>
#include <cstdio>
#include <string>
#include <atomic>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#define ID_PAYLOAD 1
#define ID_LOGO    2

static ID3D11Device* g_Device = nullptr;
static ID3D11DeviceContext* g_Context = nullptr;
static IDXGISwapChain* g_SwapChain = nullptr;
static ID3D11RenderTargetView* g_RTV = nullptr;
static ID3D11ShaderResourceView* g_LogoSRV = nullptr;
static int g_LogoW = 0, g_LogoH = 0;

static HANDLE g_PayloadProc = nullptr;
static std::string g_PayloadPath;
static std::atomic<bool> g_Running{ true };

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return 1;
    switch (msg) {
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
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

static bool LoadEmbeddedResource(int id, void** data, DWORD* size) {
    HRSRC hr = FindResourceA(nullptr, MAKEINTRESOURCEA(id), RT_RCDATA);
    if (!hr) return false;
    HGLOBAL hg = LoadResource(nullptr, hr);
    if (!hg) return false;
    *data = LockResource(hg);
    *size = SizeofResource(nullptr, hr);
    return *data != nullptr && *size > 0;
}

static bool CreateLogoTexture() {
    void* data = nullptr; DWORD size = 0;
    if (!LoadEmbeddedResource(ID_LOGO, &data, &size)) return false;
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory((const stbi_uc*)data, (int)size, &w, &h, &comp, 4);
    if (!px) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = px; sd.SysMemPitch = w * 4;
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = g_Device->CreateTexture2D(&td, &sd, &tex);
    stbi_image_free(px);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = td.Format;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    HRESULT hr2 = g_Device->CreateShaderResourceView(tex, &vd, &g_LogoSRV);
    tex->Release();
    if (FAILED(hr2)) return false;

    g_LogoW = w; g_LogoH = h;
    return true;
}

static std::string GetTempPayloadPath() {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    return std::string(tmp) + "cs2esp_payload.exe";
}

static bool LaunchPayload() {
    void* data = nullptr; DWORD size = 0;
    if (!LoadEmbeddedResource(ID_PAYLOAD, &data, &size)) return false;

    g_PayloadPath = GetTempPayloadPath();
    HANDLE f = CreateFileA(g_PayloadPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(f, data, size, &written, nullptr);
    CloseHandle(f);
    if (written != size) return false;

    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, (char*)g_PayloadPath.c_str(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) return false;

    CloseHandle(pi.hThread);
    g_PayloadProc = pi.hProcess;
    return true;
}

static void UnloadPayload() {
    if (g_PayloadProc) {
        TerminateProcess(g_PayloadProc, 0);
        CloseHandle(g_PayloadProc);
        g_PayloadProc = nullptr;
    }
    DeleteFileA(g_PayloadPath.c_str());
}

static bool PayloadAlive() {
    if (!g_PayloadProc) return false;
    DWORD code = 0;
    if (GetExitCodeProcess(g_PayloadProc, &code) && code == STILL_ACTIVE) return true;
    CloseHandle(g_PayloadProc);
    g_PayloadProc = nullptr;
    return false;
}

static void CenterTextColored(const char* txt, const ImVec4& col) {
    float w = ImGui::CalcTextSize(txt).x;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - w) * 0.5f);
    ImGui::TextColored(col, "%s", txt);
}

int main() {
    // Single instance
    CreateMutexA(nullptr, TRUE, "CS2Loader_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    int winW = 480, winH = 380;
    int posX = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int posY = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW; wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"CS2Loader_Class"; RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"CS2 Loader", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        posX, posY, winW, winH, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd);

    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Width = winW; sd.BufferDesc.Height = winH;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferDesc.RefreshRate = { 60, 1 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    D3D_FEATURE_LEVEL fl;
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_SwapChain, &g_Device, &fl, &g_Context) != S_OK) return 1;

    ID3D11Texture2D* backBuf = nullptr; g_SwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf));
    g_Device->CreateRenderTargetView(backBuf, nullptr, &g_RTV); backBuf->Release();

    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.f;
    st.WindowPadding = ImVec2(20, 20);
    ImVec4* colors = st.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.15f, 0.35f, 0.55f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.45f, 0.70f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.10f, 0.30f, 0.50f, 1.0f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_Device, g_Context);

    CreateLogoTexture();

    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    char status[160] = "initializing...";

    while (g_Running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_Running = false;
        }
        if (!g_Running) break;

        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("##loader", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);

        // Logo
        if (g_LogoSRV) {
            float targetH = 130.f;
            float targetW = targetH * ((float)g_LogoW / (float)g_LogoH);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - targetW) * 0.5f);
            ImGui::Image((ImTextureID)g_LogoSRV, ImVec2(targetW, targetH));
            ImGui::Spacing();
        }

        CenterTextColored("C S 2   E X T E R N A L   L O A D E R", ImVec4(0.85f, 0.90f, 1.0f, 1.0f));
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); ImGui::Spacing();

        bool cs2 = FindProcessId("cs2.exe") != 0;
        bool alive = PayloadAlive();

        if (alive) snprintf(status, sizeof(status), "status: PAYLOAD RUNNING");
        else if (!cs2) snprintf(status, sizeof(status), "status: CS2 not detected - start the game first");
        else snprintf(status, sizeof(status), "status: CS2 running - ready to launch");

        CenterTextColored(status, alive ? ImVec4(0.3f, 1.0f, 0.4f, 1.0f)
            : cs2 ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f)
            : ImVec4(1.0f, 0.35f, 0.35f, 1.0f));

        ImGui::Spacing(); ImGui::Spacing();

        float bw = 150.f, bh = 36.f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - (bw * 2 + 16)) * 0.5f);
        if (ImGui::Button("LAUNCH", ImVec2(bw, bh))) {
            if (!alive && cs2) {
                if (LaunchPayload()) snprintf(status, sizeof(status), "status: PAYLOAD RUNNING");
                else snprintf(status, sizeof(status), "status: LAUNCH FAILED (extract error)");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("UNLOAD", ImVec2(bw, bh))) {
            UnloadPayload();
        }

        ImGui::Spacing(); ImGui::Spacing();
        CenterTextColored("closing loader does NOT stop the cheat - use UNLOAD", ImVec4(0.45f, 0.45f, 0.50f, 1.0f));

        ImGui::End();

        ImGui::Render();
        g_Context->OMSetRenderTargets(1, &g_RTV, nullptr);
        g_Context->ClearRenderTargetView(g_RTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_SwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    if (g_LogoSRV) g_LogoSRV->Release();
    if (g_RTV) g_RTV->Release();
    if (g_SwapChain) g_SwapChain->Release();
    if (g_Context) g_Context->Release();
    if (g_Device) g_Device->Release();
    DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
