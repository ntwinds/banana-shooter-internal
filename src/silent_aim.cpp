#include "silent_aim.hpp"
#include "ui.hpp"
#include "offsets.hpp"
#include "weapon_features.hpp"
#include <Windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <array>
#include <atomic>

namespace silent_aim {
namespace {
using aim::Vec3;
using Attack=void(*)(void*,void*);
using GunShoot=bool(*)(void*,bool,void*);
// Win64 return ABI: out RCX, Transform RDX, method R8.
using Forward=Vec3*(*)(Vec3*,void*,void*);
using Position=Vec3*(*)(Vec3*,void*,void*);
using StaticData=void*(*)(void*);
struct Target {Vec3 world{};ULONGLONG timestamp{};bool valid{};};
struct Scope {Vec3 world{};void* camera{};bool active{},counted{};};
struct Runtime {
    std::uintptr_t base{};
    ui::State* state{};
    std::atomic<bool> enabled{};
    Attack attack{};GunShoot gun{};Forward forward{};Position position{};StaticData static_data{};
    std::array<void*,3> created{};
    unsigned created_count{};
};
Runtime runtime;
SRWLOCK target_lock=SRWLOCK_INIT;
Target cached_target;
thread_local Scope scope;
constexpr std::uintptr_t outgoing_forward_return=0x41D74E;
constexpr std::uintptr_t hitscan_forward_return=0x42008F;
template<class T>T read(void* object,std::uintptr_t offset){return *reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(object)+offset);}
bool alive(void* object){return object&&read<void*>(object,0x10)!=nullptr;}
template<class F>F at(std::uintptr_t rva){return reinterpret_cast<F>(runtime.base+rva);}
bool focused(){DWORD pid{};GetWindowThreadProcessId(GetForegroundWindow(),&pid);return pid==GetCurrentProcessId();}
bool active(){
    const auto* state=runtime.state;
    return runtime.enabled.load()&&state&&state->silent_aim.load()&&!state->menu.load()&&
        !state->fault.load()&&!state->stop.load()&&focused();
}
bool target_copy(Target& target){
    AcquireSRWLockShared(&target_lock);target=cached_target;ReleaseSRWLockShared(&target_lock);
    return target.valid&&GetTickCount64()-target.timestamp<=100&&aim::finite(target.world);
}
bool local_firearm_unsafe(void* firearm,void** camera){
    // Firearms owns local viewmodels; still require exact current local weapon
    // identity, rather than assuming every call to a shared method is local.
    if(!alive(firearm)||read<int>(firearm,offsets::Firearms::weaponType)!=0)return false;
    void* klass=*reinterpret_cast<void**>(runtime.base+offsets::WeaponManager::TypeInfoRva);
    if(!klass)return false;
    void* statics=runtime.static_data(klass);
    if(!statics)return false;
    void* manager=read<void*>(statics,offsets::WeaponManager::fields::Instance);
    if(!alive(manager)||read<void*>(manager,offsets::WeaponManager::_currentWeapon)!=firearm)return false;
    void* local=read<void*>(manager,offsets::WeaponManager::_currentPlayer);
    if(!alive(local)||!read<bool>(local,offsets::PlayerState::IsLocal)||read<int>(local,offsets::PlayerState::Health)<=0)return false;
    void* controller=at<void*(*)(void*)>(offsets::MoveCamera::get_Instance)(nullptr);
    if(!alive(controller)||read<void*>(controller,offsets::MoveCamera::playerState)!=local)return false;
    void* transform=read<void*>(controller,offsets::MoveCamera::camTransform);
    if(!alive(transform))return false;
    if(at<void*(*)(void*,void*)>(offsets::Firearms::get_camera)(firearm,nullptr)!=transform)return false;
    *camera=transform;return true;
}
__declspec(noinline) bool local_firearm(void* firearm,void** camera){
    __try{return local_firearm_unsafe(firearm,camera);}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
Scope make_scope(void* firearm,bool permitted){
    Scope next{};
    if(!permitted||!active())return next;
    Target target{};if(!target_copy(target))return next;
    if(!local_firearm(firearm,&next.camera))return next;
    next.world=scope.active?scope.world:target.world;
    next.active=true;next.counted=scope.active&&scope.counted;return next;
}
struct ScopedShot {
    Scope previous;
    explicit ScopedShot(Scope next):previous(scope){scope=next;}
    ~ScopedShot(){scope=previous;}
    ScopedShot(const ScopedShot&)=delete;
    ScopedShot& operator=(const ScopedShot&)=delete;
};
void attack_hook(void* firearm,void* method){
    if(!weapon_features::before_attack(firearm))return;
    ScopedShot shot(make_scope(firearm,true));runtime.attack(firearm,method);
    weapon_features::after_attack(firearm);
}
bool gun_hook(void* firearm,bool visual_only,void* method){
    // Only inherit a visual replay when already inside this local DoAttack.
    // Independent replay/remote calls retain their original forward vector.
    ScopedShot shot(make_scope(firearm,!visual_only||scope.active));return runtime.gun(firearm,visual_only,method);
}
bool redirect_unsafe(Vec3* result,void* transform){
    if(!result||!alive(transform))return false;
    Vec3 origin{};runtime.position(&origin,transform,nullptr);
    const float maximum=runtime.state->fov.load();
    // Recheck at actual shot time instead of trusting a stale selection alone.
    return aim::shot_direction(*result,origin,scope.world,maximum,*result);
}
__declspec(noinline) bool redirect(Vec3* result,void* transform){
    __try{return redirect_unsafe(result,transform);}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
__declspec(noinline) Vec3* forward_hook(Vec3* result,void* transform,void* method){
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    Vec3* output=runtime.forward(result,transform,method);
    if(!scope.active||scope.camera!=transform||!active())return output;
    if(caller!=runtime.base+outgoing_forward_return&&caller!=runtime.base+hitscan_forward_return)return output;
    Target target{};if(!target_copy(target))return output;
    if(redirect(output,transform)&&!scope.counted){runtime.state->silent_shots.fetch_add(1);scope.counted=true;}
    return output;
}
}
bool install(std::uintptr_t module_base,ui::State& state){
    if(runtime.enabled.load())return true;
    if(!module_base||runtime.created_count)return false;
    runtime.base=module_base;runtime.state=&state;
    runtime.static_data=reinterpret_cast<StaticData>(GetProcAddress(reinterpret_cast<HMODULE>(module_base),"il2cpp_class_get_static_field_data"));
    runtime.position=at<Position>(offsets::Transform::get_position);
    if(!runtime.static_data)return false;
    const std::array<void*,3> targets={reinterpret_cast<void*>(module_base+offsets::Transform::get_forward),
        reinterpret_cast<void*>(module_base+offsets::Firearms::GunShoot),reinterpret_cast<void*>(module_base+offsets::Firearms::DoAttack)};
    const std::array<void*,3> detours={reinterpret_cast<void*>(&forward_hook),reinterpret_cast<void*>(&gun_hook),reinterpret_cast<void*>(&attack_hook)};
    const std::array<void**,3> originals={reinterpret_cast<void**>(&runtime.forward),reinterpret_cast<void**>(&runtime.gun),reinterpret_cast<void**>(&runtime.attack)};
    for(std::size_t index=0;index<targets.size();++index){
        if(MH_CreateHook(targets[index],detours[index],originals[index])!=MH_OK){shutdown();return false;}
        runtime.created[runtime.created_count++]=targets[index];
    }
    for(void* target:targets)if(MH_EnableHook(target)!=MH_OK){shutdown();return false;}
    runtime.enabled.store(true);return true;
}
void update_target(Vec3 world,bool valid){
    AcquireSRWLockExclusive(&target_lock);
    cached_target={world,GetTickCount64(),valid&&aim::finite(world)};
    ReleaseSRWLockExclusive(&target_lock);
}
void shutdown(){
    runtime.enabled.store(false);update_target({},false);
    // Disable only our hooks. Pinned module and disabled trampolines stay valid
    // for any already-dispatched call still returning through original code.
    for(unsigned index=0;index<runtime.created_count;++index)MH_DisableHook(runtime.created[index]);
}
}
