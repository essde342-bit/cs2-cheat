#include <Windows.h>
#include <TlHelp32.h>
#include <urlmon.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <map>
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
#pragma comment(lib, "dwmapi.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#define ID_PAYLOAD 1
#define ID_LOGO    2
#define CURRENT_VERSION "2.0.0"
#define VERSION_URL "https://raw.githubusercontent.com/essde342-bit/cs2-cheat/main/version.json"

static ID3D11Device* g_Device = nullptr;
static ID3D11DeviceContext* g_Context = nullptr;
static IDXGISwapChain* g_SwapChain = nullptr;
static ID3D11RenderTargetView* g_RTV = nullptr;
static ID3D11ShaderResourceView* g_LogoSRV = nullptr;
static int g_LogoW = 0, g_LogoH = 0;
static HWND g_Hwnd = nullptr;
static HANDLE g_PayloadProc = nullptr;
static std::string g_PayloadPath;
static std::atomic<bool> g_Running{ true };
static std::atomic<bool> g_VersionChecked{ false };
static std::atomic<bool> g_VersionOk{ true };
static std::string g_VersionMessage, g_DownloadUrl;
static ImFont* g_FontTitle = nullptr;
static ImFont* g_FontUI = nullptr;

static const ImVec4 ACCENT(0.35f, 0.62f, 1.0f, 1.0f);
static const ImVec4 ACCENT_HI(0.55f, 0.78f, 1.0f, 1.0f);
static const ImVec4 PURPLE(0.55f, 0.35f, 0.95f, 1.0f);

struct Star { float x, y, r, phase, speed; };
static std::vector<Star> g_Stars;
static std::map<std::string, float> g_HoverAnim;
static ImVec4 g_StatusCol(1, 0.85f, 0.3f, 1);

static float Lerp(float a, float b, float t) { return a + (b - a) * t; }
static ImVec4 LerpCol(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(Lerp(a.x,b.x,t), Lerp(a.y,b.y,t), Lerp(a.z,b.z,t), Lerp(a.w,b.w,t));
}
static ImU32 U32(const ImVec4& c) { return IM_COL32((int)(c.x*255),(int)(c.y*255),(int)(c.z*255),(int)(c.w*255)); }

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
    int w=0,h=0,c=0; unsigned char* px = stbi_load_from_memory((const stbi_uc*)d,(int)sz,&w,&h,&c,4);
    if (!px) return false;
    D3D11_TEXTURE2D_DESC td{}; td.Width=w; td.Height=h; td.MipLevels=1; td.ArraySize=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count=1; td.Usage=D3D11_USAGE_DEFAULT; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem=px; sd.SysMemPitch=w*4;
    ID3D11Texture2D* tex=nullptr; HRESULT hr=g_Device->CreateTexture2D(&td,&sd,&tex); stbi_image_free(px);
    if (FAILED(hr)) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format=td.Format; vd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels=1;
    HRESULT hr2=g_Device->CreateShaderResourceView(tex,&vd,&g_LogoSRV); tex->Release();
    if (FAILED(hr2)) return false;
    g_LogoW=w; g_LogoH=h; return true;
}

static std::string GetTempPayloadPath() {
    char t[MAX_PATH]; GetTempPathA(MAX_PATH, t);
    char n[64]; sprintf_s(n, "void_%llu.exe", (unsigned long long)(GetTickCount64() ^ ((unsigned long long)GetCurrentProcessId() << 16)));
    return std::string(t) + n;
}
static bool LaunchPayload() {
    void* d=nullptr; DWORD sz=0; if (!LoadEmbeddedResource(ID_PAYLOAD,&d,&sz)) return false;
    g_PayloadPath = GetTempPayloadPath();
    HANDLE f = CreateFileA(g_PayloadPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr=0; WriteFile(f,d,sz,&wr,nullptr); CloseHandle(f);
    if (wr != sz) return false;
    STARTUPINFOA si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr,(char*)g_PayloadPath.c_str(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) return false;
    CloseHandle(pi.hThread); g_PayloadProc=pi.hProcess; return true;
}
static void UnloadPayload() {
    if (g_PayloadProc) { TerminateProcess(g_PayloadProc,0); CloseHandle(g_PayloadProc); g_PayloadProc=nullptr; }
    DeleteFileA(g_PayloadPath.c_str());
}
static bool PayloadAlive() {
    if (!g_PayloadProc) return false;
    DWORD c=0;
    if (GetExitCodeProcess(g_PayloadProc,&c) && c==STILL_ACTIVE) return true;
    CloseHandle(g_PayloadProc); g_PayloadProc=nullptr; return false;
}

static std::string ExtractJsonString(const std::string& j, const std::string& k) {
    size_t p=j.find("\""+k+"\""); if(p==std::string::npos)return "";
    p=j.find(':',p); if(p==std::string::npos)return "";
    p=j.find('"',p+1); if(p==std::string::npos)return "";
    size_t e=j.find('"',p+1); if(e==std::string::npos)return "";
    return j.substr(p+1,e-p-1);
}
static void CheckVersion() {
    char t[MAX_PATH]; GetTempPathA(MAX_PATH,t);
    std::string path=std::string(t)+"void_ver.json";
    if (FAILED(URLDownloadToFileA(nullptr,VERSION_URL,path.c_str(),0,nullptr))) { g_VersionOk=true; g_VersionChecked=true; return; }
    FILE* f=fopen(path.c_str(),"rb");
    if (!f) { g_VersionOk=true; g_VersionChecked=true; return; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    std::string buf(sz,'\0'); fread(&buf[0],1,sz,f); fclose(f); DeleteFileA(path.c_str());
    std::string rv=ExtractJsonString(buf,"version");
    g_VersionMessage=ExtractJsonString(buf,"message");
    g_DownloadUrl=ExtractJsonString(buf,"download_url");
    g_VersionOk = rv.empty() || rv==CURRENT_VERSION;
    g_VersionChecked=true;
}

// ---------- animated background ----------
static void DrawBackground(ImDrawList* dl, float W, float H, float time, float dt) {
    dl->AddRectFilledMultiColor(ImVec2(0,0), ImVec2(W,H),
        IM_COL32(8,10,20,255), IM_COL32(10,8,24,255), IM_COL32(2,2,6,255), IM_COL32(4,4,10,255));
    // nebula glow blobs
    float p1 = 0.5f + 0.5f*sinf(time*0.7f);
    float p2 = 0.5f + 0.5f*sinf(time*0.5f+2.f);
    dl->AddCircleFilled(ImVec2(W*0.2f, H*0.35f), 150.f, U32(ImVec4(ACCENT.x,ACCENT.y,ACCENT.z,0.05f+0.03f*p1)), 48);
    dl->AddCircleFilled(ImVec2(W*0.85f, H*0.7f), 170.f, U32(ImVec4(PURPLE.x,PURPLE.y,PURPLE.z,0.05f+0.03f*p2)), 48);
    // stars
    if (g_Stars.empty()) {
        for (int i=0;i<110;i++) {
            Star s; s.x=(float)(rand()%1000)/1000.f*W; s.y=(float)(rand()%1000)/1000.f*H;
            s.r=0.6f+(rand()%100)/100.f*1.6f; s.phase=(rand()%628)/100.f; s.speed=0.4f+(rand()%100)/100.f*1.6f;
            g_Stars.push_back(s);
        }
    }
    for (auto& s : g_Stars) {
        s.y -= s.speed*dt*6.f; if (s.y < -2) { s.y = H+2; s.x = (float)(rand()%1000)/1000.f*W; }
        float tw = 0.25f + 0.75f*fabsf(sinf(time*s.speed + s.phase));
        dl->AddCircleFilled(ImVec2(s.x,s.y), s.r, IM_COL32(255,255,255,(int)(200*tw)), 6);
    }
}

// ---------- animated custom button ----------
static bool AnimButton(const char* id, ImVec2 size, bool accent, bool enabled) {
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered() && enabled;
    bool held = ImGui::IsItemActive() && enabled;
    float dt = ImGui::GetIO().DeltaTime;
    float& t = g_HoverAnim[id];
    t += ((hovered?1.f:0.f) - t) * fminf(1.f, dt*12.f);
    ImVec2 p = ImGui::GetItemRectMin();
    ImVec4 base = accent ? LerpCol(ImVec4(0.13f,0.30f,0.52f,1), ACCENT, t)
                         : LerpCol(ImVec4(0.10f,0.11f,0.15f,1), ImVec4(0.20f,0.22f,0.28f,1), t);
    if (!enabled) base = ImVec4(0.08f,0.08f,0.10f,1);
    if (held) base = LerpCol(base, ImVec4(0,0,0,1), 0.25f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // hover glow
    if (t > 0.02f && enabled) dl->AddRectFilled(ImVec2(p.x-3,p.y-3), ImVec2(p.x+size.x+3,p.y+size.y+3), U32(ImVec4(ACCENT.x,ACCENT.y,ACCENT.z,0.18f*t)), 10.f);
    dl->AddRectFilled(p, ImVec2(p.x+size.x,p.y+size.y), U32(base), 8.f);
    dl->AddRect(p, ImVec2(p.x+size.x,p.y+size.y), U32(ImVec4(1,1,1, enabled?0.10f+0.15f*t:0.04f)), 8.f);
    ImVec2 ts = ImGui::CalcTextSize(id);
    ImVec2 tp(p.x+(size.x-ts.x)*0.5f, p.y+(size.y-ts.y)*0.5f + (held?1.f:0.f));
    dl->AddText(tp, enabled?IM_COL32(255,255,255,255):IM_COL32(120,120,130,255), id);
    return clicked && enabled;
}

static void CenterDrawText(ImDrawList* dl, ImFont* font, float size, float y, const char* text, ImU32 col, float W) {
    float w = font->CalcTextSizeA(size, FLT_MAX, 0.f, text).x;
    dl->AddText(font, size, ImVec2((W-w)*0.5f, y), col, text);
}

int main() {
    CreateMutexA(nullptr, TRUE, "VoidDLC_Single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    std::thread(CheckVersion).detach();
    srand(GetTickCount());

    const int winW=520, winH=470;
    int px=(GetSystemMetrics(SM_CXSCREEN)-winW)/2, py=(GetSystemMetrics(SM_CYSCREEN)-winH)/2;
    WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc); wc.style=CS_HREDRAW|CS_VREDRAW; wc.lpfnWndProc=WndProc;
    wc.hInstance=GetModuleHandleW(nullptr); wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    wc.lpszClassName=L"VoidDLC_Loader2"; RegisterClassExW(&wc);
    g_Hwnd = CreateWindowExW(0, wc.lpszClassName, L"Void DLC", WS_POPUP, px, py, winW, winH, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(g_Hwnd, SW_SHOW); UpdateWindow(g_Hwnd);
    DWORD round = 2; DwmSetWindowAttribute(g_Hwnd, 33, &round, sizeof(round)); // Win11 rounded corners

    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount=2; sd.BufferDesc.Width=winW; sd.BufferDesc.Height=winH;
    sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferDesc.RefreshRate={60,1};
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow=g_Hwnd; sd.SampleDesc.Count=1; sd.Windowed=TRUE;
    D3D_FEATURE_LEVEL fl;
    if (D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&g_SwapChain,&g_Device,&fl,&g_Context)!=S_OK) return 1;
    ID3D11Texture2D* bb=nullptr; g_SwapChain->GetBuffer(0,IID_PPV_ARGS(&bb));
    g_Device->CreateRenderTargetView(bb,nullptr,&g_RTV); bb->Release();

    ImGui::CreateContext(); ImGui::StyleColorsDark();
    ImGuiIO& io=ImGui::GetIO();
    g_FontUI = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf",17.f,nullptr,io.Fonts->GetGlyphRangesCyrillic());
    if(!g_FontUI) g_FontUI = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf",16.f,nullptr,io.Fonts->GetGlyphRangesCyrillic());
    if(!g_FontUI) g_FontUI = io.Fonts->AddFontDefault();
    g_FontTitle = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf",34.f,nullptr,io.Fonts->GetGlyphRangesCyrillic());
    if(!g_FontTitle) g_FontTitle = g_FontUI;
    ImGuiStyle& st=ImGui::GetStyle(); st.WindowRounding=0; st.WindowPadding=ImVec2(0,0);
    ImGui_ImplWin32_Init(g_Hwnd); ImGui_ImplDX11_Init(g_Device,g_Context);
    CreateLogoTexture();

    const float clear[4]={0,0,0,1};
    double startTime = ImGui::GetTime();
    bool dragging=false; POINT lastCur{};

    while (g_Running) {
        MSG msg;
        while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (msg.message==WM_QUIT) g_Running=false;
        }
        if (!g_Running) break;
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        float time=(float)ImGui::GetTime();
        float dt=io.DeltaTime;
        float fade=fminf(1.f,(float)(ImGui::GetTime()-startTime)/0.6f);
        float W=(float)winW, H=(float)winH;

        ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(W,H));
        ImGui::Begin("##root",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground);
        ImDrawList* dl=ImGui::GetWindowDrawList();
        DrawBackground(dl,W,H,time,dt);

        // ---- titlebar ----
        dl->AddRectFilled(ImVec2(0,0),ImVec2(W,36),IM_COL32(255,255,255,8));
        dl->AddText(ImVec2(14,9),IM_COL32(200,210,230,255),"VOID DLC");
        dl->AddText(ImVec2(86,10),IM_COL32(120,130,150,255),"v" CURRENT_VERSION);
        ImGui::SetCursorPos(ImVec2(0,0));
        ImGui::InvisibleButton("##drag",ImVec2(W-70,36));
        if (ImGui::IsItemActive()) {
            POINT cur; GetCursorPos(&cur);
            if (!dragging) { dragging=true; lastCur=cur; }
            RECT r; GetWindowRect(g_Hwnd,&r);
            SetWindowPos(g_Hwnd,nullptr,r.left+cur.x-lastCur.x,r.top+cur.y-lastCur.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            lastCur=cur;
        } else dragging=false;
        ImGui::SetCursorPos(ImVec2(W-64,6));
        if (ImGui::InvisibleButton("##min",ImVec2(28,24))) ShowWindow(g_Hwnd,SW_MINIMIZE);
        if (ImGui::IsItemHovered()) dl->AddRectFilled(ImVec2(W-64,6),ImVec2(W-36,30),IM_COL32(255,255,255,20),6);
        dl->AddText(ImVec2(W-55,8),IM_COL32(200,200,210,255),"-");
        ImGui::SetCursorPos(ImVec2(W-32,6));
        if (ImGui::InvisibleButton("##close",ImVec2(28,24))) g_Running=false;
        if (ImGui::IsItemHovered()) dl->AddRectFilled(ImVec2(W-32,6),ImVec2(W-4,30),IM_COL32(220,60,60,180),6);
        dl->AddText(ImVec2(W-24,8),IM_COL32(230,230,240,255),"x");

        // ---- logo with glow + bob + fade ----
        float bob = sinf(time*1.2f)*3.f;
        float glow = 0.5f+0.5f*sinf(time*1.6f);
        float logoH=130.f, logoW=logoH*((float)g_LogoW/(float)g_LogoH);
        float lx=(W-logoW)*0.5f, ly=52.f+bob;
        if (g_LogoSRV) {
            dl->AddCircleFilled(ImVec2(W*0.5f,ly+logoH*0.5f),logoH*0.75f,U32(ImVec4(ACCENT.x,ACCENT.y,ACCENT.z,0.06f+0.05f*glow)),48);
            ImGui::SetCursorPos(ImVec2(lx,ly));
            ImGui::Image((ImTextureID)g_LogoSRV,ImVec2(logoW,logoH),ImVec2(0,0),ImVec2(1,1),ImVec4(1,1,1,fade));
        }
        CenterDrawText(dl,g_FontTitle,34.f,ly+logoH+6,"VOID DLC",IM_COL32(235,240,255,(int)(255*fade)),W);
        CenterDrawText(dl,g_FontUI,15.f,ly+logoH+44,"external suite  |  counter-strike 2",IM_COL32(130,140,165,(int)(255*fade)),W);

        float y = ly+logoH+78;
        bool cs2 = FindProcessId("cs2.exe")!=0;
        bool alive = PayloadAlive();

        if (!g_VersionChecked) {
            ImVec4 target(1,0.85f,0.3f,1); g_StatusCol=LerpCol(g_StatusCol,target,fminf(1,dt*6));
            const char* msg="проверка обновлений...";
            ImVec2 ts=ImGui::CalcTextSize(msg);
            float pw=ts.x+52, ph=30, pxx=(W-pw)*0.5f;
            dl->AddRectFilled(ImVec2(pxx,y),ImVec2(pxx+pw,y+ph),U32(ImVec4(g_StatusCol.x,g_StatusCol.y,g_StatusCol.z,0.12f)),15);
            dl->AddRect(ImVec2(pxx,y),ImVec2(pxx+pw,y+ph),U32(ImVec4(g_StatusCol.x,g_StatusCol.y,g_StatusCol.z,0.4f)),15);
            // spinner
            ImVec2 c(pxx+20,y+15);
            dl->PathArcTo(c,7.f,time*5.f,time*5.f+4.5f,24);
            dl->PathStroke(U32(g_StatusCol),0,2.f);
            dl->AddText(ImVec2(pxx+36,y+7),U32(g_StatusCol),msg);
            y+=ph+18;
        } else if (!g_VersionOk) {
            ImVec4 target(1,0.25f,0.25f,1); g_StatusCol=LerpCol(g_StatusCol,target,fminf(1,dt*6));
            CenterDrawText(dl,g_FontUI,18.f,y,"VERSION OUTDATED",U32(g_StatusCol),W); y+=28;
            const char* msg=g_VersionMessage.empty()?"Обновите клиент.":g_VersionMessage.c_str();
            CenterDrawText(dl,g_FontUI,15.f,y,msg,IM_COL32(255,170,170,255),W); y+=30;
            ImGui::SetCursorPos(ImVec2((W-230)*0.5f,y));
            if (AnimButton("DOWNLOAD UPDATE",ImVec2(230,40),true,true) && !g_DownloadUrl.empty())
                ShellExecuteA(nullptr,"open",g_DownloadUrl.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
            y+=52;
            CenterDrawText(dl,g_FontUI,13.f,y,"current: v" CURRENT_VERSION,IM_COL32(120,120,130,255),W);
        } else {
            ImVec4 target = alive?ImVec4(0.3f,1,0.45f,1): cs2?ImVec4(1,0.85f,0.3f,1):ImVec4(1,0.4f,0.4f,1);
            g_StatusCol=LerpCol(g_StatusCol,target,fminf(1,dt*6));
            const char* msg = alive?"payload running": cs2?"CS2 запущен - готов к запуску":"CS2 не найден - сначала запусти игру";
            ImVec2 ts=ImGui::CalcTextSize(msg);
            float pw=ts.x+40, ph=30, pxx=(W-pw)*0.5f;
            dl->AddRectFilled(ImVec2(pxx,y),ImVec2(pxx+pw,y+ph),U32(ImVec4(g_StatusCol.x,g_StatusCol.y,g_StatusCol.z,0.12f)),15);
            dl->AddRect(ImVec2(pxx,y),ImVec2(pxx+pw,y+ph),U32(ImVec4(g_StatusCol.x,g_StatusCol.y,g_StatusCol.z,0.4f)),15);
            dl->AddCircleFilled(ImVec2(pxx+18,y+15),4.f,U32(g_StatusCol),12);
            dl->AddText(ImVec2(pxx+30,y+7),U32(g_StatusCol),msg);
            y+=ph+20;
            ImGui::SetCursorPos(ImVec2((W-316)*0.5f,y));
            if (AnimButton("LAUNCH",ImVec2(150,42),true,!alive&&cs2)) LaunchPayload();
            ImGui::SameLine(0,16);
            if (AnimButton("UNLOAD",ImVec2(150,42),false,alive)) UnloadPayload();
            y+=56;
            CenterDrawText(dl,g_FontUI,13.f,y,"закрытие лоадера не останавливает чит - используй UNLOAD",IM_COL32(110,115,130,255),W);
        }

        // fade-in from black
        if (fade<1.f) ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0,0),ImVec2(W,H),IM_COL32(0,0,0,(int)(255*(1-fade))));
        ImGui::End();

        ImGui::Render();
        g_Context->OMSetRenderTargets(1,&g_RTV,nullptr);
        g_Context->ClearRenderTargetView(g_RTV,clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_SwapChain->Present(1,0);
    }

    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    if (g_LogoSRV) g_LogoSRV->Release(); if (g_RTV) g_RTV->Release();
    if (g_SwapChain) g_SwapChain->Release(); if (g_Context) g_Context->Release(); if (g_Device) g_Device->Release();
    DestroyWindow(g_Hwnd); UnregisterClassW(wc.lpszClassName,wc.hInstance);
    return 0;
}
