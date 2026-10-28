#include "weapon_features.hpp"
#include "ui.hpp"
#include "offsets.hpp"
#include <MinHook.h>
#include <algorithm>
#include <cmath>
namespace weapon_features {
namespace {
struct Runtime {
    std::uintptr_t base{};ui::State* state{};std::atomic<bool> installed{};
    void*(*static_data)(void*){};
    std::uint32_t(*weak_new)(void*,bool){};
    void*(*weak_get)(std::uint32_t){};void(*weak_free)(std::uint32_t){};
    int(*host_attack)(void*,void*){};
    std::atomic<std::uint64_t> local_id{};
    std::uint32_t weapon{};bool saved_ammo{};int old_ammo{};ULONGLONG last_attempt{};
} runtime;
template<class T>T read(void* p,std::uintptr_t offset){return *reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(p)+offset);}
template<class T>void write(void* p,std::uintptr_t offset,T value){*reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(p)+offset)=value;}
template<class F>F at(std::uintptr_t rva){return reinterpret_cast<F>(runtime.base+rva);}
bool alive(void* p){return p&&read<void*>(p,0x10);}
void* local_weapon(){
    void* klass=*reinterpret_cast<void**>(runtime.base+offsets::WeaponManager::TypeInfoRva);
    if(!klass)return nullptr;
    void* fields=runtime.static_data(klass);if(!fields)return nullptr;
    void* manager=read<void*>(fields,offsets::WeaponManager::fields::Instance);
    if(!alive(manager))return nullptr;
    void* player=read<void*>(manager,offsets::WeaponManager::_currentPlayer);
    if(!alive(player)||!read<bool>(player,offsets::PlayerState::IsLocal))return nullptr;
    runtime.local_id=read<std::uint64_t>(player,offsets::PlayerState::SteamId);
    void* weapon=read<void*>(manager,offsets::WeaponManager::_currentWeapon);
    return alive(weapon)&&read<int>(weapon,offsets::Firearms::weaponType)==0?weapon:nullptr;
}
int ammo(void* weapon){void* value=read<void*>(weapon,offsets::Firearms::currentAmmo);return value?at<int(*)(void*,void*)>(offsets::SafeInt::GetValue)(value,nullptr):-1;}
void set_ammo(void* weapon,int count){void* value=read<void*>(weapon,offsets::Firearms::currentAmmo);if(value)at<int(*)(void*,int,void*)>(offsets::SafeInt::SetValue)(value,count,nullptr);}
void fill(void* weapon){const int capacity=read<int>(weapon,offsets::Firearms::maxAmmo);if(capacity>0&&capacity<=100000)set_ammo(weapon,capacity);}
void restore_unsafe(){
    const auto handle=runtime.weapon;runtime.weapon=0;
    void* weapon=handle?runtime.weak_get(handle):nullptr;
    if(runtime.saved_ammo&&alive(weapon))set_ammo(weapon,runtime.old_ammo);
    if(handle)runtime.weak_free(handle);
    runtime.saved_ammo=false;runtime.last_attempt=0;
}
void tick_unsafe(){
    if(!runtime.installed)return;
    void* weapon=local_weapon();
    void* previous=runtime.weapon?runtime.weak_get(runtime.weapon):nullptr;
    if(weapon!=previous)restore_unsafe();
    if(!weapon){runtime.state->weapon_ammo=-1;return;}
    if(!runtime.weapon)runtime.weapon=runtime.weak_new(weapon,false);
    if(runtime.state->infinite_ammo.load()){
        if(!runtime.saved_ammo){runtime.old_ammo=ammo(weapon);runtime.saved_ammo=runtime.old_ammo>=0;}
        fill(weapon);
    }else if(runtime.saved_ammo){set_ammo(weapon,runtime.old_ammo);runtime.saved_ammo=false;}
    runtime.state->weapon_ammo=ammo(weapon);
}
bool before_unsafe(void* weapon){
    if(!runtime.installed||runtime.state->stop.load()||runtime.state->fault.load()||local_weapon()!=weapon)return true;
    if(runtime.state->infinite_ammo.load())fill(weapon);
    if(!runtime.state->rapid_fire.load()||runtime.state->menu.load()||read<bool>(weapon,offsets::Firearms::isReload))return true;
    void* stat=read<void*>(weapon,offsets::Firearms::stat);
    if(!alive(stat))return true;
    const float normal=read<float>(stat,offsets::WeaponStat::fireRate);
    if(!std::isfinite(normal)||normal<=0)return true;
    const float desired=std::clamp(normal*std::clamp(runtime.state->rapid_multiplier.load(),1.0f,10.0f),1.0f,60.0f);
    const ULONGLONG now=GetTickCount64();
    if(runtime.last_attempt&&static_cast<float>(now-runtime.last_attempt)<1000.0f/desired)return false;
    runtime.last_attempt=now;
    // Native DoAttack checks this timestamp against Time.time - 1/stat.fireRate.
    write(weapon,offsets::Firearms::lastFireTime,-1000000.0f);return true;
}
struct HostSaved {void* weapon{};std::uint32_t ammo{};std::uint16_t rate{};bool changed_ammo{},changed_rate{};};
void prepare_host_unsafe(void* weapon,HostSaved& saved){
    auto& state=*runtime.state;
    if(!runtime.installed||state.stop.load()||state.fault.load())return;
    void* player=read<void*>(weapon,offsets::ActiveWeapon::_player);
    const auto local=runtime.local_id.load();
    if(!local||!alive(player)||read<std::uint64_t>(player,offsets::ServerPlayer::SteamId)!=local)return;
    saved.weapon=weapon;
    if(state.rapid_fire.load()){
        saved.rate=read<std::uint16_t>(weapon,offsets::ActiveWeapon::_tickFireRate);
        const auto rate=static_cast<std::uint16_t>(std::max(1.0f,static_cast<float>(saved.rate)/std::clamp(state.rapid_multiplier.load(),1.0f,10.0f)));
        saved.changed_rate=true;write(weapon,offsets::ActiveWeapon::_tickFireRate,rate);
    }
    if(state.infinite_ammo.load()){
        void* stat=read<void*>(weapon,offsets::ActiveWeapon::Stat);
        if(alive(stat)){
            const auto capacity=read<std::uint32_t>(stat,offsets::WeaponStat::maxAmmo);
            if(capacity&&capacity<=100000){saved.ammo=read<std::uint32_t>(weapon,offsets::ActiveWeapon::CurrentAmmo);saved.changed_ammo=true;write(weapon,offsets::ActiveWeapon::CurrentAmmo,std::max(saved.ammo,capacity));}
        }
    }
    if(saved.changed_rate||saved.changed_ammo)++state.host_weapon_updates;
}
void finish_host_unsafe(const HostSaved& saved){
    if(!saved.weapon)return;
    if(saved.changed_rate)write(saved.weapon,offsets::ActiveWeapon::_tickFireRate,saved.rate);
    if(saved.changed_ammo)write(saved.weapon,offsets::ActiveWeapon::CurrentAmmo,saved.ammo);
}
void prepare_host(void* weapon,HostSaved& saved){__try{prepare_host_unsafe(weapon,saved);}__except(EXCEPTION_EXECUTE_HANDLER){}}
void finish_host(const HostSaved& saved){__try{finish_host_unsafe(saved);}__except(EXCEPTION_EXECUTE_HANDLER){}}
int host_hook(void* weapon,void* method){
    HostSaved saved{};prepare_host(weapon,saved);int result{};
    __try{result=runtime.host_attack(weapon,method);}__finally{finish_host(saved);}
    return result;
}
}
bool install(std::uintptr_t base,ui::State& state){
    runtime.base=base;runtime.state=&state;
    const auto module=reinterpret_cast<HMODULE>(base);
    runtime.static_data=reinterpret_cast<decltype(runtime.static_data)>(GetProcAddress(module,"il2cpp_class_get_static_field_data"));
    runtime.weak_new=reinterpret_cast<decltype(runtime.weak_new)>(GetProcAddress(module,"il2cpp_gchandle_new_weakref"));
    runtime.weak_get=reinterpret_cast<decltype(runtime.weak_get)>(GetProcAddress(module,"il2cpp_gchandle_get_target"));
    runtime.weak_free=reinterpret_cast<decltype(runtime.weak_free)>(GetProcAddress(module,"il2cpp_gchandle_free"));
    if(!runtime.static_data||!runtime.weak_new||!runtime.weak_get||!runtime.weak_free)return false;
    void* target=reinterpret_cast<void*>(base+offsets::ActiveWeapon::DoAttack);
    if(MH_CreateHook(target,reinterpret_cast<void*>(&host_hook),reinterpret_cast<void**>(&runtime.host_attack))!=MH_OK)return false;
    if(MH_EnableHook(target)!=MH_OK)return false;
    runtime.installed=true;return true;
}
void tick(){__try{tick_unsafe();}__except(EXCEPTION_EXECUTE_HANDLER){runtime.state->rapid_fire=false;runtime.state->infinite_ammo=false;}}
bool before_attack(void* firearm){__try{return before_unsafe(firearm);}__except(EXCEPTION_EXECUTE_HANDLER){return true;}}
void after_attack(void* firearm){__try{if(runtime.installed&&runtime.state->infinite_ammo.load()&&!runtime.state->stop.load()&&local_weapon()==firearm)fill(firearm);}__except(EXCEPTION_EXECUTE_HANDLER){}}
void restore(){__try{restore_unsafe();}__except(EXCEPTION_EXECUTE_HANDLER){}}
void shutdown(){if(runtime.installed){runtime.installed=false;MH_DisableHook(reinterpret_cast<void*>(runtime.base+offsets::ActiveWeapon::DoAttack));}}
}
