#include "ui.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>
#include <wrl/client.h>
#include <cstdio>
#include <thread>
using Microsoft::WRL::ComPtr;
namespace {
unsigned clicks{};
LRESULT CALLBACK host_procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam){
    if(message==WM_LBUTTONDOWN){++clicks;return 0;}
    return DefWindowProcW(window,message,wparam,lparam);
}
void send_test_click(HWND window){
    // Call the host procedure directly; no desktop input is sent.
    const auto procedure=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window,GWLP_WNDPROC));
    CallWindowProcW(procedure,window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(20,20));
}
}
int main(){
    const HINSTANCE instance=GetModuleHandleW(nullptr);
    WNDCLASSEXW registration{};registration.cbSize=sizeof(registration);
    registration.hInstance=instance;registration.lpfnWndProc=host_procedure;registration.lpszClassName=L"UnityWndClass";
    if(!RegisterClassExW(&registration))return 1;
    // Remains hidden: this test cannot intercept the user's mouse or keyboard.
    const HWND window=CreateWindowExW(0,registration.lpszClassName,L"Renderer regression host",WS_OVERLAPPEDWINDOW,
        0,0,800,600,nullptr,nullptr,instance,nullptr);
    if(!window)return 1;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> chain;
    DXGI_SWAP_CHAIN_DESC desc{};desc.BufferCount=1;desc.BufferDesc.Width=800;desc.BufferDesc.Height=600;
    desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow=window;desc.SampleDesc.Count=1;desc.Windowed=TRUE;
    const HRESULT created=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&desc,chain.GetAddressOf(),device.GetAddressOf(),nullptr,context.GetAddressOf());
    if(FAILED(created)){DestroyWindow(window);return 1;}
    ComPtr<ID3D11Texture2D> buffer;ComPtr<ID3D11RenderTargetView> target;
    if(FAILED(chain->GetBuffer(0,IID_PPV_ARGS(buffer.GetAddressOf())))||
       FAILED(device->CreateRenderTargetView(buffer.Get(),nullptr,target.GetAddressOf())))return 1;
    if(MH_Initialize()!=MH_OK)return 1;
    ui::State state;state.menu=false;state.esp=false;state.aimbot=false;
    int renderer_result=-1;
    std::thread worker([&]{renderer_result=ui::run_in_game(state,instance);});
    bool checked=false;int failed=0;
    const auto check=[&](bool good,const char* label){if(!good){++failed;std::printf("FAILED: %s\n",label);}};
    const ULONGLONG began=GetTickCount64();
    while(!state.stop.load()&&GetTickCount64()-began<12000){
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        constexpr float background[]={0.1f,0.3f,0.55f,1};
        ID3D11RenderTargetView* view=target.Get();context->OMSetRenderTargets(1,&view,nullptr);
        context->ClearRenderTargetView(view,background);chain->Present(0,0);
        if(state.render_frames.load()>3&&!checked){
            ID3D11RenderTargetView* restored{};context->OMGetRenderTargets(1,&restored,nullptr);
            check(restored==target.Get(),"game render target preserved");if(restored)restored->Release();
            state.menu=false;send_test_click(window);
            check(clicks==1,"game click forwarded when menu hidden");
            state.menu=true;send_test_click(window);
            check(clicks==1,"menu captures its mouse click");
            state.menu=false;send_test_click(window);
            check(clicks==2,"game clicks resume after menu closes");
            checked=true;state.stop=true;
        }
        Sleep(10);
    }
    state.stop=true;worker.join();
    check(checked,"real D3D11 Present hook rendered frames");
    check(renderer_result==0,"renderer shuts down cleanly");
    check(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window,GWLP_WNDPROC))==host_procedure,"original game procedure restored");
    send_test_click(window);check(clicks==3,"game click forwarded after deactivation");
    std::printf("Renderer/input regression: frames=%u failures=%d\n",state.render_frames.load(),failed);
    context->OMSetRenderTargets(0,nullptr,nullptr);target.Reset();buffer.Reset();chain.Reset();context.Reset();device.Reset();
    DestroyWindow(window);UnregisterClassW(registration.lpszClassName,instance);
    return failed?1:0;
}
