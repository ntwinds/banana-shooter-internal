#include <Windows.h>
#include <bcrypt.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <share.h>
#include "offsets.hpp"
#include "aim_math.hpp"
#include "ui.hpp"
#include "cursor_policy.hpp"
#include "silent_aim.hpp"
#include "weapon_features.hpp"

namespace {
using aim::Vec3;
struct Color { float r,g,b,a; };
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h):value(h){}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
};
HMODULE self_module{};
std::uintptr_t base{};
ui::State state;
auto& esp=state.esp;
auto& aimbot=state.aimbot;
auto& team_filter=state.team_filter;
auto& stop=state.stop;
auto& restored=state.restored;
auto& fov=state.fov;
auto& smooth=state.smooth;
auto& frames=state.frames;
auto& players=state.players;
auto& fault=state.fault;
FILE* log_file{};
void log(const char* text) {
    if(log_file){std::fprintf(log_file,"%s\n",text);std::fflush(log_file);}
    OutputDebugStringA(text);OutputDebugStringA("\n");
    state.set_status(text);
}
template<class T>T read(void* object,std::uintptr_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(object)+offset);
}
template<class T>void write(void* object,std::uintptr_t offset,T value) {
    *reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(object)+offset)=value;
}
// UnityEngine.Object's native cached pointer follows its two IL2CPP object words.
bool alive(void* object){return object&&read<void*>(object,0x10)!=nullptr;}
template<class F>F at(std::uintptr_t rva){return reinterpret_cast<F>(base+rva);}
using Look=void(*)(void*,void*);
Look original_look{};
Look original_camera_update{};
std::uint32_t movement_handle{};
void* movement_identity{};
using WeakNew=std::uint32_t(*)(void*,bool);
using WeakGet=void*(*)(std::uint32_t);
using WeakFree=void(*)(std::uint32_t);
WeakNew weak_new{};WeakGet weak_get{};WeakFree weak_free{};
struct SavedOutline {std::uint32_t handle{};int mode{};Color color{};float width{};bool enabled{};bool seen{};};
std::array<SavedOutline,512> outlines{};
bool enabled(void* o){return at<bool(*)(void*,void*)>(offsets::Behaviour::get_enabled)(o,nullptr);}
void enable(void* o,bool value){at<void(*)(void*,bool,void*)>(offsets::Behaviour::set_enabled)(o,value,nullptr);}
Vec3 position(void* t){Vec3 result{};at<Vec3*(*)(Vec3*,void*,void*)>(offsets::Transform::get_position)(&result,t,nullptr);return result;}
Vec3 forward(void* t){Vec3 result{};at<Vec3*(*)(Vec3*,void*,void*)>(offsets::Transform::get_forward)(&result,t,nullptr);return result;}
void rotate(void* t,Vec3 euler,bool local){at<void(*)(void*,Vec3,void*)>(local?offsets::Transform::set_localEulerAngles:offsets::Transform::set_eulerAngles)(t,euler,nullptr);}
struct MovementSettings {
    std::uint32_t handle{},camera_handle{};
    bool speed_saved{},jump_saved{},fly_saved{},fov_saved{},previous_noclip{};
    float old_speed{},old_jump{},old_fly_speed{},old_fov{};
} movement_settings;
void restore_movement(){
    auto& saved=movement_settings;
    void* movement=saved.handle?weak_get(saved.handle):nullptr;
    if(alive(movement)){
        if(saved.speed_saved)write(movement,offsets::PlayerMovement::moveSpeedFactor,saved.old_speed);
        if(saved.jump_saved)write(movement,offsets::PlayerMovement::jumpFactor,saved.old_jump);
        if(saved.fly_saved){
            void* noclip=read<void*>(movement,offsets::PlayerMovement::noClipMovement);
            if(noclip){
                write(noclip,offsets::NoClipMovement::moveSpeed,saved.old_fly_speed);
                const bool current=read<void*>(movement,offsets::PlayerMovement::_playerMovement)==noclip;
                if(current!=saved.previous_noclip)at<void(*)(void*,void*)>(offsets::PlayerMovement::ChangeNoClip)(movement,nullptr);
            }
        }
    }
    if(saved.handle)weak_free(saved.handle);
    void* camera=saved.camera_handle?weak_get(saved.camera_handle):nullptr;
    if(saved.fov_saved&&alive(camera))at<void(*)(void*,float,void*)>(offsets::Camera::set_fieldOfView)(camera,saved.old_fov,nullptr);
    if(saved.camera_handle)weak_free(saved.camera_handle);
    saved={};state.flying=false;
}
void apply_movement(void* movement,void* camera){
    auto& saved=movement_settings;
    if(saved.handle&&weak_get(saved.handle)!=movement)restore_movement();
    if(!saved.handle)saved.handle=weak_new(movement,false);
    if(state.speed_override.load()){
        if(!saved.speed_saved){saved.old_speed=read<float>(movement,offsets::PlayerMovement::moveSpeedFactor);saved.speed_saved=true;}
        write(movement,offsets::PlayerMovement::moveSpeedFactor,saved.old_speed*std::clamp(state.speed_multiplier.load(),0.25f,5.0f));
    }else if(saved.speed_saved){write(movement,offsets::PlayerMovement::moveSpeedFactor,saved.old_speed);saved.speed_saved=false;}
    if(state.jump_override.load()){
        if(!saved.jump_saved){saved.old_jump=read<float>(movement,offsets::PlayerMovement::jumpFactor);saved.jump_saved=true;}
        write(movement,offsets::PlayerMovement::jumpFactor,saved.old_jump*std::clamp(state.jump_multiplier.load(),0.25f,5.0f));
    }else if(saved.jump_saved){write(movement,offsets::PlayerMovement::jumpFactor,saved.old_jump);saved.jump_saved=false;}
    void* noclip=read<void*>(movement,offsets::PlayerMovement::noClipMovement);
    if(noclip&&state.fly.load()){
        if(!saved.fly_saved){
            saved.old_fly_speed=read<float>(noclip,offsets::NoClipMovement::moveSpeed);
            saved.previous_noclip=read<void*>(movement,offsets::PlayerMovement::_playerMovement)==noclip;
            saved.fly_saved=true;
        }
        if(read<void*>(movement,offsets::PlayerMovement::_playerMovement)!=noclip)
            at<void(*)(void*,void*)>(offsets::PlayerMovement::ChangeNoClip)(movement,nullptr);
        write(noclip,offsets::NoClipMovement::moveSpeed,std::clamp(state.fly_speed.load(),1.0f,100.0f));
        state.flying=read<void*>(movement,offsets::PlayerMovement::_playerMovement)==noclip;
    }else if(saved.fly_saved){
        if(noclip){
            write(noclip,offsets::NoClipMovement::moveSpeed,saved.old_fly_speed);
            if((read<void*>(movement,offsets::PlayerMovement::_playerMovement)==noclip)!=saved.previous_noclip)
                at<void(*)(void*,void*)>(offsets::PlayerMovement::ChangeNoClip)(movement,nullptr);
        }
        saved.fly_saved=false;state.flying=false;
    }
    if(saved.camera_handle&&weak_get(saved.camera_handle)!=camera){
        void* previous=weak_get(saved.camera_handle);
        if(saved.fov_saved&&alive(previous))at<void(*)(void*,float,void*)>(offsets::Camera::set_fieldOfView)(previous,saved.old_fov,nullptr);
        weak_free(saved.camera_handle);saved.camera_handle=0;saved.fov_saved=false;
    }
    if(state.fov_override.load()){
        if(!saved.camera_handle)saved.camera_handle=weak_new(camera,false);
        if(!saved.fov_saved){saved.old_fov=at<float(*)(void*,void*)>(offsets::Camera::get_fieldOfView)(camera,nullptr);saved.fov_saved=true;}
        at<void(*)(void*,float,void*)>(offsets::Camera::set_fieldOfView)(camera,std::clamp(state.camera_fov.load(),40.0f,130.0f),nullptr);
    }else if(saved.fov_saved){
        at<void(*)(void*,float,void*)>(offsets::Camera::set_fieldOfView)(camera,saved.old_fov,nullptr);saved.fov_saved=false;
    }
}
void release(SavedOutline& saved) {
    if(!saved.handle)return;
    void* o=weak_get(saved.handle);
    if(alive(o)){
        write(o,offsets::Outline::outlineMode,saved.mode);
        write(o,offsets::Outline::outlineColor,saved.color);
        write(o,offsets::Outline::outlineWidth,saved.width);
        write(o,offsets::Outline::needsUpdate,true);
        enable(o,saved.enabled);
    }
    weak_free(saved.handle);saved={};
}
void highlight(void* o) {
    if(!alive(o))return;
    SavedOutline* free_slot=nullptr;
    SavedOutline* selected=nullptr;
    for(auto& saved:outlines){
        if(!saved.handle){if(!free_slot)free_slot=&saved;continue;}
        if(weak_get(saved.handle)==o){selected=&saved;break;}
    }
    if(!selected){
        if(!free_slot)return;
        const auto handle=weak_new(o,false);
        if(!handle)return;
        *free_slot={handle,read<int>(o,offsets::Outline::outlineMode),
                    read<Color>(o,offsets::Outline::outlineColor),
                    read<float>(o,offsets::Outline::outlineWidth),enabled(o),true};
        selected=free_slot;
    }
    selected->seen=true;
    write(o,offsets::Outline::outlineMode,0); // OutlineAll
    write(o,offsets::Outline::outlineColor,Color{1.0f,0.15f,0.1f,1.0f});
    write(o,offsets::Outline::outlineWidth,3.0f);
    write(o,offsets::Outline::needsUpdate,true);
    if(!enabled(o))enable(o,true);
}
void restore_all(){for(auto& item:outlines)release(item);}
bool focused(){DWORD pid{};GetWindowThreadProcessId(GetForegroundWindow(),&pid);return pid==GetCurrentProcessId();}
Vec3 project(void* camera,Vec3 point){Vec3 result{};at<Vec3*(*)(Vec3*,void*,const Vec3*,void*)>(offsets::Camera::WorldToScreenPoint)(&result,camera,&point,nullptr);return result;}
void publish(ui::Snapshot& next){next.timestamp=GetTickCount64();std::lock_guard<std::mutex> lock(state.mutex);state.snapshot=next;}
void player_name(void* player,std::array<char,96>& output){
    void* text=read<void*>(player,offsets::PlayerState::Username);
    if(text){
        const int size=read<int>(text,0x10);
        if(size>0&&size<512){
            const auto* chars=reinterpret_cast<const wchar_t*>(reinterpret_cast<std::uintptr_t>(text)+0x14);
            const int result=WideCharToMultiByte(CP_UTF8,0,chars,std::min(size,28),output.data(),static_cast<int>(output.size()-1),nullptr,nullptr);
            if(result>0){output[static_cast<std::size_t>(result)]=0;return;}
        }
    }
    strcpy_s(output.data(),output.size(),"Player");
}
void tick(void* movement,void* move_camera) {
    ++frames;
    ui::Snapshot next{};
    silent_aim::update_target({},false);
    for(auto& item:outlines)item.seen=false;
    if(stop.load()||fault.load()){
        weapon_features::restore();
        restore_movement();
        restore_all();
        restored=true;publish(next);return;
    }
    void* local=read<void*>(movement,offsets::PlayerMovement::currentPlayer);
    void* camera=read<void*>(movement,offsets::PlayerMovement::playerCam);
    void* orientation=read<void*>(movement,offsets::PlayerMovement::orientation);
    if(!alive(local)||!alive(camera)||!alive(orientation)||!read<bool>(local,offsets::PlayerState::IsLocal)){next.aim_reason=7;restore_all();publish(next);return;}
    void* list=at<void*(*)(void*)>(offsets::PlayerState::get_PlayerStates)(nullptr);
    if(!list){next.aim_reason=7;restore_all();publish(next);return;}
    // List<T>: items +0x10, size +0x18; array data +0x20.
    void* array=read<void*>(list,0x10);
    const int count=read<int>(list,0x18);
    if(!array||count<0||count>512){next.aim_reason=7;restore_all();publish(next);return;}
    const auto capacity=read<std::uintptr_t>(array,0x18);
    if(static_cast<std::uintptr_t>(count)>capacity||capacity>65536){next.aim_reason=7;restore_all();publish(next);return;}
    players=static_cast<unsigned>(count);
    const bool active=focused();
    const bool key_down=(GetAsyncKeyState(VK_RBUTTON)&0x8000)!=0;
    const bool can_aim=active&&!state.menu.load()&&aimbot.load()&&key_down&&read<int>(local,offsets::PlayerState::Health)>0;
    next.aim_reason=!aimbot.load()?1:!active?2:state.menu.load()?3:!key_down?4:read<int>(local,offsets::PlayerState::Health)<=0?5:6;
    const int local_team=read<int>(local,offsets::PlayerState::Team);
    // Use the camera assigned to MoveCamera.
    void* render_camera=read<void*>(move_camera,offsets::MoveCamera::cam);
    if(!alive(render_camera)){next.aim_reason=8;restore_all();publish(next);return;}
    void* render_transform=at<void*(*)(void*,void*)>(offsets::Component::get_transform)(render_camera,nullptr);
    if(!alive(render_transform)){next.aim_reason=8;restore_all();publish(next);return;}
    apply_movement(movement,render_camera);
    next.width=at<int(*)(void*,void*)>(offsets::Camera::get_pixelWidth)(render_camera,nullptr);
    next.height=at<int(*)(void*,void*)>(offsets::Camera::get_pixelHeight)(render_camera,nullptr);
    next.vertical_fov=at<float(*)(void*,void*)>(offsets::Camera::get_fieldOfView)(render_camera,nullptr);
    next.camera_ready=next.width>0&&next.height>0;
    const Vec3 origin=position(render_transform),view=forward(render_transform);
    if(!aim::finite(origin)||!aim::finite(view)){next.aim_reason=8;restore_all();publish(next);return;}
    float best=fov.load();Vec3 target{},world_target{};bool found=false;
    for(int i=0;i<count;++i){
        void* p=read<void*>(array,0x20+static_cast<std::uintptr_t>(i)*sizeof(void*));
        if(!alive(p)||p==local||read<bool>(p,offsets::PlayerState::IsLocal)||read<int>(p,offsets::PlayerState::Health)<=0)continue;
        if(team_filter.load()&&read<int>(p,offsets::PlayerState::Team)==local_team){++next.same_team;continue;}
        if(esp.load()&&state.outlines.load()&&active)highlight(read<void*>(p,offsets::PlayerState::outline));
        void* head=read<void*>(p,offsets::PlayerState::head);
        if(!alive(head)){++next.missing_head;continue;}
        const Vec3 point=position(head);
        if(!aim::finite(point))continue;
        ++next.candidates;
        const Vec3 direction=aim::difference(point,origin);
        const float distance=aim::length(direction);
        const float degrees=aim::angle(view,direction);
        if(std::isfinite(degrees))next.nearest_angle=std::min(next.nearest_angle,degrees);
        if(esp.load()&&next.width>0&&next.height>0&&next.count<next.boxes.size()){
            Vec3 feet=point;feet.y-=1.75f;
            void* root=read<void*>(p,offsets::PlayerState::playerTransform);
            if(alive(root)){
                const Vec3 root_position=position(root);
                const float separation=aim::length(aim::difference(point,root_position));
                if(aim::finite(root_position)&&separation>0.25f&&separation<5.0f)feet=root_position;
            }
            Vec3 top_point=point;top_point.y+=0.2f;
            const Vec3 top=project(render_camera,top_point),bottom=project(render_camera,feet);
            if(aim::finite(top)&&aim::finite(bottom)&&top.z>0.05f&&bottom.z>0.05f){
                ++next.in_front;
                const float height=std::abs(top.y-bottom.y);
                if(height>3&&height<static_cast<float>(next.height)*2){
                    auto& box=next.boxes[next.count++];
                    const float center=(top.x+bottom.x)*0.5f;
                    box.left=center-height*0.22f;box.right=center+height*0.22f;
                    box.top=static_cast<float>(next.height)-std::max(top.y,bottom.y);
                    box.bottom=static_cast<float>(next.height)-std::min(top.y,bottom.y);
                    box.health=read<int>(p,offsets::PlayerState::Health);
                    box.max_health=std::max(1,read<int>(p,offsets::PlayerState::MaxHealth));
                    player_name(p,box.name);
                }else ++next.bad_projection;
            }else ++next.bad_projection;
        }
        if(distance<0.05f||distance>500.0f)continue;
        if(degrees<best){
            best=degrees;target=direction;world_target=point;found=true;
            const Vec3 screen=project(render_camera,point);
            next.target_x=screen.x;next.target_y=static_cast<float>(next.height)-screen.y;
        }
    }
    for(auto& item:outlines)if(item.handle&&!item.seen)release(item);
    next.target_selected=found;
    silent_aim::update_target(world_target,found&&active&&!state.menu.load()&&read<int>(local,offsets::PlayerState::Health)>0);
    static ULONGLONG last_time{};
    const ULONGLONG now=GetTickCount64();
    const float dt=last_time?static_cast<float>(now-last_time)/1000.0f:1.0f/60.0f;
    last_time=now;
    if(found&&can_aim&&!state.silent_aim.load()){
        const Vec3 desired=aim::angles(target);
        const float old_pitch=read<float>(movement,offsets::PlayerMovement::xRotation);
        const float old_yaw=read<float>(movement,offsets::PlayerMovement::desiredX);
        if(!std::isfinite(old_pitch)||!std::isfinite(old_yaw)){next.aim_reason=11;publish(next);return;}
        const float amount=aim::smoothing(smooth.load(),dt);
        const float pitch=std::clamp(old_pitch+(desired.x-old_pitch)*amount,-89.0f,89.0f);
        const float yaw=old_yaw+aim::wrap(desired.y-old_yaw)*amount;
        write(movement,offsets::PlayerMovement::xRotation,pitch);
        write(movement,offsets::PlayerMovement::desiredX,yaw);
        rotate(camera,{pitch,yaw,0},false);
        rotate(orientation,{0,yaw,0},true);
        rotate(render_transform,{pitch,yaw,0},false);
        ++state.aim_updates;next.aim_reason=10;
    }
    if(can_aim&&!found)next.aim_reason=next.candidates?9:6;
    if(state.silent_aim.load())next.aim_reason=found?12:13;
    publish(next);
}
// Keep SEH outside functions that own C++ objects requiring unwinding.
__declspec(noinline) void guarded_tick(void* movement,void* camera_controller) {
    __try {tick(movement,camera_controller);}
    __except(EXCEPTION_EXECUTE_HANDLER){fault=GetExceptionCode();esp=false;aimbot=false;}
}
// These values and Unity calls belong exclusively to the main-game Look thread.
bool cursor_saved{},cursor_broken{};
int saved_cursor_lock{};
bool saved_cursor_visible{};
DWORD cursor_exception{};
void update_cursor_unsafe(bool menu_open) {
    if(menu_open){
        if(!cursor_saved){
            saved_cursor_lock=at<int(*)(void*)>(offsets::Cursor::get_lockState)(nullptr);
            saved_cursor_visible=at<bool(*)(void*)>(offsets::Cursor::get_visible)(nullptr);
            cursor_saved=true;
        }
        // Game code may relock the cursor between callbacks; enforce both values.
        at<void(*)(int,void*)>(offsets::Cursor::set_lockState)(0,nullptr);
        at<void(*)(bool,void*)>(offsets::Cursor::set_visible)(true,nullptr);
    }else if(cursor_saved){
        void* manager=at<void*(*)(void*)>(offsets::NetworkManager::get_Instance)(nullptr);
        const bool blocked=alive(manager)&&at<bool(*)(void*,bool,void*)>(offsets::NetworkManager::CantPlay)(manager,false,nullptr);
        const auto desired=input_policy::restore({saved_cursor_lock,saved_cursor_visible},blocked);
        at<void(*)(int,void*)>(offsets::Cursor::set_lockState)(desired.lock,nullptr);
        at<void(*)(bool,void*)>(offsets::Cursor::set_visible)(desired.visible,nullptr);
        cursor_saved=false;
    }
}
__declspec(noinline) bool guarded_cursor(bool menu_open) {
    __try {update_cursor_unsafe(menu_open);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){cursor_exception=GetExceptionCode();return false;}
}
bool menu_cursor(bool requested) {
    if(cursor_broken)return false;
    if(guarded_cursor(requested))return requested;
    const DWORD error=cursor_exception;
    // A setter may have succeeded before its companion failed: restore once.
    guarded_cursor(false);
    cursor_saved=false;cursor_broken=true;
    fault=error;esp=false;aimbot=false;
    state.set_status("Cursor API failed. Features deactivated; original camera look resumed.");
    return false;
}
void hooked_look(void* movement,void* method){
    if(!stop.load()&&movement!=movement_identity){
        if(movement_handle)weak_free(movement_handle);
        movement_handle=weak_new(movement,false);movement_identity=movement;
    }
    const bool suppress_look=menu_cursor(state.menu.load()&&!stop.load()&&!fault.load());
    if(!suppress_look)original_look(movement,method);
    if(suppress_look&&(!state.menu.load()||stop.load()||fault.load())){
        // Also handle a tick fault or concurrent shutdown in this same callback.
        menu_cursor(false);
        original_look(movement,method);
    }
}
__declspec(noinline) void camera_tick(void* controller){
    __try {
        if(!stop.load()&&!fault.load())weapon_features::tick();
        else weapon_features::restore();
        // CantPlay(true) stops Look while our menu unlocks the cursor. Restore
        // from this independent callback so closing Insert cannot deadlock input.
        menu_cursor(state.menu.load()&&!stop.load()&&!fault.load()&&focused());
        void* movement=movement_handle?weak_get(movement_handle):nullptr;
        if(alive(movement))guarded_tick(movement,controller);
        else {
            ui::Snapshot empty{};empty.aim_reason=7;publish(empty);
            silent_aim::update_target({},false);
            if(stop.load()||fault.load()){restore_movement();restore_all();restored=true;}
        }
        if(stop.load()||fault.load())menu_cursor(false);
    }__except(EXCEPTION_EXECUTE_HANDLER){fault=GetExceptionCode();esp=false;aimbot=false;}
}
void hooked_camera_update(void* controller,void* method){original_camera_update(controller,method);camera_tick(controller);}
std::string hash_file(const wchar_t* path) {
    Handle file(CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(file.value==INVALID_HANDLE_VALUE)return {};
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return {};
    if(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0){BCryptCloseAlgorithmProvider(algorithm,0);return {};}
    std::array<unsigned char,65536> buffer{};DWORD bytes{};bool good=true;
    for(;;){
        if(!ReadFile(file.value,buffer.data(),static_cast<DWORD>(buffer.size()),&bytes,nullptr)){good=false;break;}
        if(!bytes)break;
        if(BCryptHashData(hash,buffer.data(),bytes,0)<0){good=false;break;}
    }
    std::array<unsigned char,32> digest{};
    if(good)good=BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0;
    BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);
    if(!good)return {};
    std::string result;result.reserve(64);
    constexpr char digits[]="0123456789abcdef";
    for(unsigned char byte:digest){result+=digits[byte>>4];result+=digits[byte&15];}
    return result;
}
bool bind(HMODULE game) {
    wchar_t path[32768]{};
    if(!GetModuleFileNameW(game,path,32768)||hash_file(path)!=offsets::GameAssemblySha256){log("Rejected: GameAssembly SHA-256 differs from analyzed build.");return false;}
    base=reinterpret_cast<std::uintptr_t>(game);
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+static_cast<std::uintptr_t>(dos->e_lfanew));
    if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.TimeDateStamp!=offsets::GameAssemblyTimestamp||nt->OptionalHeader.SizeOfImage!=offsets::GameAssemblySizeOfImage)return false;
    constexpr unsigned char prefix[]={0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x55,0x48,0x8d,0x68,0xb8,0x48,0x81,0xec,0x40,0x01};
    if(std::memcmp(reinterpret_cast<const void*>(base+offsets::PlayerMovement::Look),prefix,sizeof(prefix))!=0){log("Rejected: Look entry bytes differ from IDA evidence.");return false;}
    weak_new=reinterpret_cast<WeakNew>(GetProcAddress(game,"il2cpp_gchandle_new_weakref"));
    weak_get=reinterpret_cast<WeakGet>(GetProcAddress(game,"il2cpp_gchandle_get_target"));
    weak_free=reinterpret_cast<WeakFree>(GetProcAddress(game,"il2cpp_gchandle_free"));
    return weak_new&&weak_get&&weak_free;
}
DWORD WINAPI bootstrap(void*) {
    // Pin while callbacks exist: END deactivates, it does not unload code under game threads.
    HMODULE pinned{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&bootstrap),&pinned);
    wchar_t path[32768]{};GetModuleFileNameW(self_module,path,32768);
    std::wstring location(path);const auto slash=location.find_last_of(L"\\/");
    location=location.substr(0,slash+1)+L"BananaInternalUI.log";
    log_file=_wfsopen(location.c_str(),L"a",_SH_DENYNO);
    log("Initializing ImGui build...");
    HMODULE game=GetModuleHandleW(L"GameAssembly.dll");
    if(!game||!bind(game)){
        esp=false;aimbot=false;
        log("Initialization failed: missing or incompatible GameAssembly/API. Check BananaInternalUI.log.");
        ui::run(state,self_module);
        if(log_file){std::fclose(log_file);log_file=nullptr;}return 1;
    }
    void* target=reinterpret_cast<void*>(base+offsets::PlayerMovement::Look);
    void* camera_target=reinterpret_cast<void*>(base+offsets::MoveCamera::LateUpdate);
    MH_STATUS status=MH_Initialize();
    if(status==MH_OK)status=MH_CreateHook(target,reinterpret_cast<void*>(&hooked_look),reinterpret_cast<void**>(&original_look));
    if(status==MH_OK)status=MH_CreateHook(camera_target,reinterpret_cast<void*>(&hooked_camera_update),reinterpret_cast<void**>(&original_camera_update));
    if(status==MH_OK)status=MH_EnableHook(target);
    if(status==MH_OK)status=MH_EnableHook(camera_target);
    if(status==MH_OK){
        state.silent_available=silent_aim::install(base,state);
        if(!state.silent_available.load())log("Silent aim hooks unavailable; standard aim and movement remain available.");
        else state.weapon_available=weapon_features::install(base,state);
    }
    if(status!=MH_OK){MH_DisableHook(target);MH_DisableHook(camera_target);esp=false;aimbot=false;log(MH_StatusToString(status));ui::run(state,self_module);if(log_file){std::fclose(log_file);log_file=nullptr;}return 1;}
    log("Ready: final-camera update active. Insert closes menu; hold RMB to aim. Diagnostics show the reason when aim is idle.");
    const int ui_result=ui::run(state,self_module);
    silent_aim::shutdown();
    weapon_features::shutdown();
    if(ui_result)log("ImGui renderer failed. Features deactivated.");
    esp=false;aimbot=false;stop=true;
    log("Deactivated. Original outlines restore on the next Look callback. DLL stays loaded until game exit.");
    for(int i=0;i<120&&!restored.load();++i)Sleep(25);
    if(restored.load()){MH_DisableHook(target);MH_DisableHook(camera_target);log("Outlines restored and both game callbacks disabled.");}
    else log("No Look callback yet; inactive hook retained solely for deferred restoration.");
    if(log_file){std::fclose(log_file);log_file=nullptr;}
    return 0;
}
}
extern "C" __declspec(dllexport) void RequestStop(){esp=false;aimbot=false;stop=true;}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH){self_module=instance;DisableThreadLibraryCalls(instance);HANDLE thread=CreateThread(nullptr,0,bootstrap,nullptr,0,nullptr);if(thread)CloseHandle(thread);else return FALSE;}
    return TRUE;
}
