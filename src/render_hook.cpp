#include "ui.hpp"
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cwchar>
#include <mutex>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

namespace ui {
namespace {
using Microsoft::WRL::ComPtr;
using PresentFunction=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
using ResizeFunction=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT);
struct Renderer {
    State* state{};
    std::recursive_mutex mutex;
    std::atomic<bool> stopping{true},initialized{},failed{};
    std::atomic<unsigned> callbacks{};
    std::atomic<HWND> window{};
    std::atomic<WNDPROC> previous_procedure{};
    ComPtr<IDXGISwapChain> chain;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11RenderTargetView> target;
    ImGuiContext* imgui{};
    bool win32_ready{},dx11_ready{},focused{};
    std::array<bool,5> mouse_down{};
    std::atomic<int> wheel{},horizontal_wheel{};
    std::mutex text_mutex;
    std::array<unsigned short,512> text{};
    unsigned text_count{};
    PresentFunction present{};
    ResizeFunction resize{};
    void* present_address{};
    void* resize_address{};
};
// The DLL is pinned by main.cpp. Keep trampoline pointers/context storage valid
// for a callback that was already dispatched when hooks were disabled.
Renderer renderer;
struct Callback {
    Callback(){renderer.callbacks.fetch_add(1);}
    ~Callback(){renderer.callbacks.fetch_sub(1);}
    Callback(const Callback&)=delete;
    Callback& operator=(const Callback&)=delete;
};
struct ImGuiScope {
    ImGuiContext* previous=ImGui::GetCurrentContext();
    explicit ImGuiScope(ImGuiContext* current){ImGui::SetCurrentContext(current);}
    ~ImGuiScope(){ImGui::SetCurrentContext(previous);}
    ImGuiScope(const ImGuiScope&)=delete;
    ImGuiScope& operator=(const ImGuiScope&)=delete;
};
void fail(const char* text) {
    if(!renderer.failed.exchange(true)&&renderer.state){
        std::string existing;
        {std::lock_guard<std::mutex> lock(renderer.state->mutex);existing=renderer.state->status;}
        if(existing.find("failed")!=std::string::npos||existing.find("Rejected")!=std::string::npos||renderer.state->fault.load())
            existing+=" | ";
        else existing.clear();
        existing+=text;renderer.state->set_status(existing.c_str());
        renderer.state->stop.store(true);
    }
}
bool game_window(HWND window) {
    if(!window||!IsWindow(window))return false;
    DWORD pid{};GetWindowThreadProcessId(window,&pid);
    if(pid!=GetCurrentProcessId())return false;
    wchar_t name[128]{};
    GetClassNameW(window,name,static_cast<int>(std::size(name)));
    return std::wcscmp(name,L"UnityWndClass")==0;
}
BOOL CALLBACK find_window(HWND window,LPARAM parameter) {
    if(game_window(window)){*reinterpret_cast<HWND*>(parameter)=window;return FALSE;}return TRUE;
}
HWND find_game_window(){HWND window{};EnumWindows(find_window,reinterpret_cast<LPARAM>(&window));return window;}
LRESULT CALLBACK window_procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    // ImGui lives on the render thread. This path never calls an ImGui API.
    const auto previous=renderer.previous_procedure.load();
    State* state=renderer.state;
    if(!renderer.stopping.load()&&state&&message==WM_KEYDOWN&&(lparam&(1LL<<30))==0){
        if(wparam==VK_INSERT){state->menu.store(!state->menu.load());return 0;}
        if(wparam==VK_END&&!state->menu.load()){state->stop.store(true);return 0;}
        if(wparam==VK_ESCAPE&&state->menu.load()){state->menu.store(false);return 0;}
        if(wparam==VK_F1){state->esp.store(!state->esp.load());return 0;}
        if(wparam==VK_F2){state->aimbot.store(!state->aimbot.load());if(state->aimbot.load())state->silent_aim=false;return 0;}
        if(wparam==VK_F3){state->team_filter.store(!state->team_filter.load());return 0;}
        if(wparam==VK_F4){state->fly.store(!state->fly.load());return 0;}
    }
    if(!renderer.stopping.load()&&state&&state->menu.load()){
        if(message==WM_CHAR){std::lock_guard<std::mutex> lock(renderer.text_mutex);if(renderer.text_count<renderer.text.size())renderer.text[renderer.text_count++]=static_cast<unsigned short>(wparam);return 0;}
        if(message==WM_KEYDOWN||message==WM_KEYUP||message==WM_SYSKEYDOWN||message==WM_SYSKEYUP)return 0;
        if(message==WM_MOUSEWHEEL){renderer.wheel.fetch_add(GET_WHEEL_DELTA_WPARAM(wparam));return 0;}
        if(message==WM_MOUSEHWHEEL){renderer.horizontal_wheel.fetch_add(GET_WHEEL_DELTA_WPARAM(wparam));return 0;}
        if(message==WM_INPUT){
            // DefWindowProc performs foreground raw-input cleanup without
            // delivering the click/movement to the game's original procedure.
            return DefWindowProcW(window,message,wparam,lparam);
        }
        if(message>=WM_MOUSEFIRST&&message<=WM_MOUSELAST)return 0;
        if(message==WM_SETCURSOR&&LOWORD(lparam)==HTCLIENT){SetCursor(LoadCursorW(nullptr,IDC_ARROW));return TRUE;}
    }
    return previous?CallWindowProcW(previous,window,message,wparam,lparam):DefWindowProcW(window,message,wparam,lparam);
}
void restore_procedure() {
    const HWND window=renderer.window.load();const auto previous=renderer.previous_procedure.load();
    if(window&&previous&&IsWindow(window)){
        const auto current=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window,GWLP_WNDPROC));
        // Do not remove another component's newer subclass. Our dormant entry
        // remains a forwarding link if another subclass has chained through it.
        if(current==window_procedure)SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(previous));
    }
}
bool create_target() {
    ComPtr<ID3D11Texture2D> buffer;
    if(FAILED(renderer.chain->GetBuffer(0,IID_PPV_ARGS(buffer.GetAddressOf()))))return false;
    return SUCCEEDED(renderer.device->CreateRenderTargetView(buffer.Get(),nullptr,renderer.target.ReleaseAndGetAddressOf()));
}
void set_theme() {
    ImGui::StyleColorsDark();auto& style=ImGui::GetStyle();
    style.WindowRounding=12;style.ChildRounding=8;style.FrameRounding=5;style.GrabRounding=5;
    style.WindowPadding={20,18};style.FramePadding={10,7};style.ItemSpacing={10,10};
    auto* colors=style.Colors;
    colors[ImGuiCol_WindowBg]={0.055f,0.070f,0.085f,0.98f};
    colors[ImGuiCol_ChildBg]={0.075f,0.095f,0.110f,1};
    colors[ImGuiCol_Border]={0.16f,0.22f,0.25f,1};
    colors[ImGuiCol_Text]={0.91f,0.95f,0.96f,1};
    colors[ImGuiCol_TextDisabled]={0.46f,0.58f,0.63f,1};
    colors[ImGuiCol_FrameBg]={0.12f,0.17f,0.19f,1};
    colors[ImGuiCol_FrameBgHovered]={0.16f,0.25f,0.28f,1};
    colors[ImGuiCol_FrameBgActive]={0.18f,0.31f,0.34f,1};
    colors[ImGuiCol_CheckMark]={0.23f,0.88f,0.73f,1};
    colors[ImGuiCol_SliderGrab]={0.23f,0.78f,0.68f,1};
    colors[ImGuiCol_SliderGrabActive]={0.36f,0.96f,0.83f,1};
    colors[ImGuiCol_Button]={0.11f,0.31f,0.29f,1};
    colors[ImGuiCol_ButtonHovered]={0.15f,0.43f,0.38f,1};
    colors[ImGuiCol_ButtonActive]={0.18f,0.53f,0.45f,1};
    colors[ImGuiCol_Header]={0.12f,0.31f,0.30f,1};
    colors[ImGuiCol_TitleBgActive]=colors[ImGuiCol_WindowBg];
}
bool initialize(IDXGISwapChain* chain,HWND window) {
    if(FAILED(chain->GetDevice(IID_PPV_ARGS(renderer.device.GetAddressOf()))))return false;
    renderer.device->GetImmediateContext(renderer.context.GetAddressOf());
    if(!renderer.context)return false;
    renderer.chain=chain;
    if(!create_target())return false;
    const auto previous_context=ImGui::GetCurrentContext();
    renderer.imgui=ImGui::CreateContext();
    if(!renderer.imgui){ImGui::SetCurrentContext(previous_context);return false;}
    ImGui::GetIO().IniFilename=nullptr;ImGui::GetIO().LogFilename=nullptr;
    ImGui::GetIO().ConfigFlags|=ImGuiConfigFlags_NoMouseCursorChange;
    set_theme();
    renderer.win32_ready=ImGui_ImplWin32_Init(window);
    if(renderer.win32_ready)renderer.dx11_ready=ImGui_ImplDX11_Init(renderer.device.Get(),renderer.context.Get());
    ImGui::SetCurrentContext(previous_context);
    if(!renderer.dx11_ready)return false;
    renderer.window.store(window);
    renderer.previous_procedure.store(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window,GWLP_WNDPROC)));
    SetLastError(0);
    const LONG_PTR previous=SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&window_procedure));
    if(!previous&&GetLastError()!=0)return false;
    if(previous)renderer.previous_procedure.store(reinterpret_cast<WNDPROC>(previous));
    renderer.initialized.store(true);
    // Main initialization/fault diagnostics must survive graphics readiness.
    {
        std::lock_guard<std::mutex> lock(renderer.state->mutex);
        if(renderer.state->status.empty()||renderer.state->status=="Starting...")
            renderer.state->status="Game renderer ready. Insert opens the menu.";
    }
    return true;
}
void poll_mouse(HWND window) {
    auto& io=ImGui::GetIO();
    const bool focused=GetForegroundWindow()==window;
    if(focused!=renderer.focused){io.AddFocusEvent(focused);renderer.focused=focused;}
    const bool active=focused&&renderer.state->menu.load();
    POINT cursor{};
    if(active&&GetCursorPos(&cursor)&&ScreenToClient(window,&cursor))
        io.AddMousePosEvent(static_cast<float>(cursor.x),static_cast<float>(cursor.y));
    else io.AddMousePosEvent(-FLT_MAX,-FLT_MAX);
    constexpr int keys[]={VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2};
    for(std::size_t index=0;index<renderer.mouse_down.size();++index){
        const bool down=active&&(GetAsyncKeyState(keys[index])&0x8000)!=0;
        if(down!=renderer.mouse_down[index]){io.AddMouseButtonEvent(static_cast<int>(index),down);renderer.mouse_down[index]=down;}
    }
    const int wheel=renderer.wheel.exchange(0),horizontal=renderer.horizontal_wheel.exchange(0);
    if(active&&(wheel||horizontal))io.AddMouseWheelEvent(static_cast<float>(horizontal)/WHEEL_DELTA,static_cast<float>(wheel)/WHEEL_DELTA);
    io.AddKeyEvent(ImGuiMod_Ctrl,active&&(GetAsyncKeyState(VK_CONTROL)&0x8000)!=0);
    io.AddKeyEvent(ImGuiMod_Shift,active&&(GetAsyncKeyState(VK_SHIFT)&0x8000)!=0);
    io.AddKeyEvent(ImGuiMod_Alt,active&&(GetAsyncKeyState(VK_MENU)&0x8000)!=0);
    struct Key {int native;ImGuiKey imgui;};
    constexpr Key keyboard[]={{VK_BACK,ImGuiKey_Backspace},{VK_DELETE,ImGuiKey_Delete},{VK_TAB,ImGuiKey_Tab},{VK_RETURN,ImGuiKey_Enter},
        {VK_LEFT,ImGuiKey_LeftArrow},{VK_RIGHT,ImGuiKey_RightArrow},{VK_UP,ImGuiKey_UpArrow},{VK_DOWN,ImGuiKey_DownArrow},
        {VK_HOME,ImGuiKey_Home},{VK_END,ImGuiKey_End},{'A',ImGuiKey_A},{'C',ImGuiKey_C},{'V',ImGuiKey_V},{'X',ImGuiKey_X},{'Y',ImGuiKey_Y},{'Z',ImGuiKey_Z}};
    for(const auto& key:keyboard)io.AddKeyEvent(key.imgui,active&&(GetAsyncKeyState(key.native)&0x8000)!=0);
    {std::lock_guard<std::mutex> lock(renderer.text_mutex);if(active)for(unsigned i=0;i<renderer.text_count;++i)io.AddInputCharacterUTF16(renderer.text[i]);renderer.text_count=0;}
}
struct OutputTargets {
    ID3D11DeviceContext* context;
    std::array<ID3D11RenderTargetView*,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> views{};
    ID3D11DepthStencilView* depth{};
    explicit OutputTargets(ID3D11DeviceContext* value):context(value){context->OMGetRenderTargets(static_cast<UINT>(views.size()),views.data(),&depth);}
    ~OutputTargets(){
        context->OMSetRenderTargets(static_cast<UINT>(views.size()),views.data(),depth);
        for(auto* view:views)if(view)view->Release();if(depth)depth->Release();
    }
    OutputTargets(const OutputTargets&)=delete;
    OutputTargets& operator=(const OutputTargets&)=delete;
};
void render(IDXGISwapChain* chain) {
    DXGI_SWAP_CHAIN_DESC description{};
    if(FAILED(chain->GetDesc(&description))||!game_window(description.OutputWindow))return;
    if(!renderer.initialized.load()){
        if(!initialize(chain,description.OutputWindow)){fail("Renderer initialization failed: game D3D11/ImGui unavailable.");return;}
    }
    if(chain!=renderer.chain.Get())return;
    if(!renderer.target&&!create_target()){fail("Renderer failed to acquire the game backbuffer.");return;}
    ImGuiScope scope(renderer.imgui);
    ImGui_ImplDX11_NewFrame();ImGui_ImplWin32_NewFrame();poll_mouse(description.OutputWindow);
    ImGui::NewFrame();
    RECT bounds{};draw_frame(*renderer.state,bounds);
    ImGui::Render();
    // The ImGui DX11 backend restores shader, viewport, scissor, blend and
    // input-assembler state; preserve OM targets separately around its draw.
    OutputTargets saved(renderer.context.Get());
    ID3D11RenderTargetView* target=renderer.target.Get();
    renderer.context->OMSetRenderTargets(1,&target,nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    renderer.state->render_frames.fetch_add(1);
}
HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* chain,UINT interval,UINT flags) {
    Callback callback;
    if(!renderer.stopping.load()&&renderer.state&&!renderer.state->stop.load()&&(flags&DXGI_PRESENT_TEST)==0){
        try{
            std::lock_guard<std::recursive_mutex> lock(renderer.mutex);
            if(!renderer.stopping.load())render(chain);
        }catch(...){fail("Renderer failed with a C++ exception; game graphics preserved.");}
    }
    const HRESULT result=renderer.present(chain,interval,flags);
    if(!renderer.stopping.load()&&(result==DXGI_ERROR_DEVICE_REMOVED||result==DXGI_ERROR_DEVICE_RESET)){
        std::lock_guard<std::recursive_mutex> lock(renderer.mutex);
        if(chain==renderer.chain.Get())fail("Game D3D11 device lost; UI stopped for safe graphics recovery.");
    }
    return result;
}
HRESULT STDMETHODCALLTYPE resize_hook(IDXGISwapChain* chain,UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags) {
    Callback callback;
    std::lock_guard<std::recursive_mutex> lock(renderer.mutex);
    const bool selected=chain==renderer.chain.Get();
    if(selected)renderer.target.Reset();
    const HRESULT result=renderer.resize(chain,count,width,height,format,flags);
    // Reacquire lazily in Present after successful resize. Do not bind targets
    // or clear any game-owned device state during ResizeBuffers.
    if(selected&&FAILED(result)&&result!=DXGI_ERROR_INVALID_CALL&&!renderer.stopping.load())
        fail("Game swapchain resize/device lost; UI stopped.");
    return result;
}
LRESULT CALLBACK dummy_procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam){return DefWindowProcW(window,message,wparam,lparam);}
bool discover(HMODULE module) {
    constexpr wchar_t class_name[]=L"BananaDxgiDiscoveryWindow";
    WNDCLASSEXW registration{};registration.cbSize=sizeof(registration);
    registration.hInstance=module;registration.lpfnWndProc=dummy_procedure;registration.lpszClassName=class_name;
    if(!RegisterClassExW(&registration))return false;
    // This tiny window is never shown, activated, or used as a live overlay.
    const HWND window=CreateWindowExW(0,class_name,L"",WS_OVERLAPPED,0,0,8,8,nullptr,nullptr,module,nullptr);
    if(!window){UnregisterClassW(class_name,module);return false;}
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> chain;
    DXGI_SWAP_CHAIN_DESC description{};description.BufferCount=1;description.BufferDesc.Width=8;description.BufferDesc.Height=8;
    description.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;description.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow=window;description.SampleDesc.Count=1;description.Windowed=TRUE;
    constexpr D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_0};
    const HRESULT result=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,
        static_cast<UINT>(std::size(levels)),D3D11_SDK_VERSION,&description,chain.GetAddressOf(),device.GetAddressOf(),nullptr,context.GetAddressOf());
    if(SUCCEEDED(result)){
        const auto table=*reinterpret_cast<void***>(chain.Get());
        renderer.present_address=table[8];renderer.resize_address=table[13];
    }
    chain.Reset();context.Reset();device.Reset();DestroyWindow(window);UnregisterClassW(class_name,module);
    return SUCCEEDED(result)&&renderer.present_address&&renderer.resize_address;
}
void shutdown() {
    renderer.stopping.store(true);
    if(renderer.present_address)MH_DisableHook(renderer.present_address);
    if(renderer.resize_address)MH_DisableHook(renderer.resize_address);
    // Disabled hooks admit no new renderer work. Acquire its mutex before
    // releasing graphics resources, then drain already-dispatched originals.
    {
        std::lock_guard<std::recursive_mutex> lock(renderer.mutex);
        // A Present already inside initialization may have installed the
        // subclass after shutdown began; restore only after that work drains.
        restore_procedure();
        if(renderer.imgui){
            ImGuiScope scope(renderer.imgui);
            if(renderer.dx11_ready)ImGui_ImplDX11_Shutdown();
            if(renderer.win32_ready)ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(renderer.imgui);renderer.imgui=nullptr;
        }
        renderer.dx11_ready=renderer.win32_ready=false;
        renderer.target.Reset();renderer.chain.Reset();renderer.context.Reset();renderer.device.Reset();
        renderer.initialized.store(false);
    }
    const ULONGLONG began=GetTickCount64();
    while(renderer.callbacks.load()&&GetTickCount64()-began<3000)Sleep(5);
    // Keep disabled trampolines allocated: a game Present can block in DXGI.
    // Removing that trampoline under its pending return would invalidate code.
}
}
int run_in_game(State& state,HMODULE module) {
    renderer.state=&state;state.menu.store(false);
    renderer.failed.store(false);renderer.stopping.store(false);
    if(!discover(module)){fail("Renderer discovery failed: no D3D11 swapchain available.");shutdown();return 1;}
    MH_STATUS result=MH_CreateHook(renderer.present_address,reinterpret_cast<void*>(&present_hook),reinterpret_cast<void**>(&renderer.present));
    if(result==MH_OK)result=MH_CreateHook(renderer.resize_address,reinterpret_cast<void*>(&resize_hook),reinterpret_cast<void**>(&renderer.resize));
    if(result==MH_OK)result=MH_EnableHook(renderer.resize_address);
    if(result==MH_OK)result=MH_EnableHook(renderer.present_address);
    if(result!=MH_OK){fail(MH_StatusToString(result));shutdown();return 1;}
    const ULONGLONG began=GetTickCount64();
    std::array<bool,256> keys{};
    const auto pressed=[&keys](int key){const bool down=(GetAsyncKeyState(key)&0x8000)!=0;const bool result=down&&!keys[static_cast<std::size_t>(key)];keys[static_cast<std::size_t>(key)]=down;return result;};
    HWND window=find_game_window();
    while(!state.stop.load()){
        const HWND selected=renderer.window.load();if(selected)window=selected;
        else if(!IsWindow(window))window=find_game_window();
        const bool active=GetForegroundWindow()==window;
        const bool insert=pressed(VK_INSERT),end=pressed(VK_END),f1=pressed(VK_F1),f2=pressed(VK_F2),f3=pressed(VK_F3);
        if(active&&!renderer.initialized.load()){
            if(insert)state.menu.store(!state.menu.load());
            if(end){state.stop.store(true);break;}
            if(f1)state.esp.store(!state.esp.load());
            if(f2)state.aimbot.store(!state.aimbot.load());
            if(f3)state.team_filter.store(!state.team_filter.load());
        }
        if(!renderer.initialized.load()&&GetTickCount64()-began>20000){fail("Renderer timed out waiting for the game's D3D11 Present.");break;}
        Sleep(15);
    }
    state.menu.store(false);shutdown();return renderer.failed.load()?1:0;
}
}
