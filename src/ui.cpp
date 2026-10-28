#include "ui.hpp"
#include "offsets.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

namespace ui {
namespace {
std::filesystem::path diagnostic_output;
void checkbox(const char* label,std::atomic<bool>& value) {
    bool current=value.load();if(ImGui::Checkbox(label,&current))value.store(current);
}
void slider(const char* label,std::atomic<float>& value,float low,float high,const char* format) {
    float current=value.load();if(ImGui::SliderFloat(label,&current,low,high,format))value.store(current);
}
const char* aim_reason(unsigned value){
    constexpr const char* reasons[]={"Waiting for callbacks","Aim disabled","Game not focused","Close menu with Insert to aim",
        "Hold right mouse to aim","Local player dead","No eligible targets","Waiting for local player",
        "Rendered camera unavailable","Targets outside aim FOV","Applying aim","Invalid saved aim angles",
        "Silent aim armed for supported hitscan guns","Silent aim: no target inside aim FOV"};
    return value<std::size(reasons)?reasons[value]:"Unknown aim state";
}
void menu(State& state,const Snapshot& snapshot,const std::string& status,RECT& bounds) {
    const auto display=ImGui::GetIO().DisplaySize;
    const float width=std::max(260.0f,std::min(660.0f,display.x-24.0f));
    const float height=std::max(250.0f,std::min(675.0f,display.y-24.0f));
    ImGui::SetNextWindowSize({width,height},ImGuiCond_Always);
    ImGui::SetNextWindowPos({std::max(12.0f,(display.x-width)*0.5f),std::max(12.0f,(display.y-height)*0.5f)},ImGuiCond_FirstUseEver);
    bool open=true;
    if(ImGui::Begin("Banana / Control panel",&open,ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoResize)){
        const ImVec2 point=ImGui::GetWindowPos(),size=ImGui::GetWindowSize();
        bounds={static_cast<LONG>(point.x),static_cast<LONG>(point.y),static_cast<LONG>(point.x+size.x),static_cast<LONG>(point.y+size.y)};
        ImGui::TextColored({0.25f,0.91f,0.76f,1},"BANANA");
        ImGui::SameLine();ImGui::TextDisabled("INTERNAL  /  %s","LIVE SESSION");
        ImGui::TextColored({0.25f,0.91f,0.76f,1},"FEATURE BUILD: aim / movement / weapons");
        ImGui::TextDisabled("Insert menu  |  F1 ESP  |  F2 aim  |  F3 teams  |  End deactivate");
        ImGui::Separator();
        if(ImGui::BeginTabBar("features")){
        if(ImGui::BeginTabItem("Visuals and aim")){
        ImGui::TextColored({0.96f,0.68f,0.38f,1},"Visuals");
        checkbox("Box ESP",state.esp);ImGui::SameLine(260);checkbox("Player outlines",state.outlines);
        checkbox("Filter teammates",state.team_filter);
        ImGui::Separator();
        ImGui::TextColored({0.96f,0.68f,0.38f,1},"Aim");
        const bool previous_aim=state.aimbot.load();
        checkbox("Aimbot - hold right mouse",state.aimbot);
        if(!previous_aim&&state.aimbot.load())state.silent_aim=false;
        ImGui::BeginDisabled(!state.silent_available.load());
        const bool previous_silent=state.silent_aim.load();
        checkbox("Silent aim - hitscan guns",state.silent_aim);
        if(!previous_silent&&state.silent_aim.load())state.aimbot=false;
        ImGui::EndDisabled();
        if(state.silent_aim.load())ImGui::TextDisabled("Camera stays unchanged; spread is preserved.");
        ImGui::SetNextItemWidth(-140);slider("Aim FOV",state.fov,1.0f,60.0f,"%.1f deg");
        ImGui::SetNextItemWidth(-140);slider("Smoothing",state.smooth,1.0f,30.0f,"%.1f");
        ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Movement")){
        if(ImGui::CollapsingHeader("Movement and camera",ImGuiTreeNodeFlags_DefaultOpen)){
            checkbox("Fly / noclip",state.fly);
            ImGui::SameLine();ImGui::TextDisabled("%s",state.flying.load()?"ACTIVE":"off");
            ImGui::SetNextItemWidth(-160);slider("Fly speed",state.fly_speed,1.0f,100.0f,"%.1f");
            ImGui::TextDisabled("WASD move, Space up, crouch down, Shift boost.");
            checkbox("Override move speed",state.speed_override);
            ImGui::SetNextItemWidth(-160);slider("Speed multiplier",state.speed_multiplier,0.25f,5.0f,"%.2fx");
            checkbox("Override jump height",state.jump_override);
            ImGui::SetNextItemWidth(-160);slider("Jump multiplier",state.jump_multiplier,0.25f,5.0f,"%.2fx");
            checkbox("Override camera FOV",state.fov_override);
            ImGui::SetNextItemWidth(-160);slider("Camera FOV",state.camera_fov,40.0f,130.0f,"%.0f deg");
            if(ImGui::Button("Reset movement and camera")){
                state.fly=false;state.speed_override=false;state.jump_override=false;state.fov_override=false;
            }
        }
        ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Weapons")){
        if(ImGui::CollapsingHeader("Weapons",ImGuiTreeNodeFlags_DefaultOpen)){
            ImGui::BeginDisabled(!state.weapon_available.load());
            checkbox("Rapid fire",state.rapid_fire);
            ImGui::SetNextItemWidth(-160);slider("Rate multiplier",state.rapid_multiplier,1.0f,10.0f,"%.1fx");
            checkbox("Infinite ammo",state.infinite_ammo);
            ImGui::EndDisabled();
            ImGui::TextDisabled("Ammo %d | local host updates %u",state.weapon_ammo.load(),state.host_weapon_updates.load());
            ImGui::TextWrapped("A remote server still controls accepted fire rate and ammo.");
        }
        ImGui::EndTabItem();}
        ImGui::EndTabBar();}
        ImGui::Separator();
        ImGui::TextColored({0.25f,0.91f,0.76f,1},"Session diagnostics");
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(status.c_str());ImGui::PopTextWrapPos();
        ImGui::TextColored({0.96f,0.68f,0.38f,1},"%s",aim_reason(snapshot.aim_reason));
        ImGui::TextDisabled("Camera %dx%d | nearest %.1f deg | aim updates %u",snapshot.width,snapshot.height,snapshot.nearest_angle,state.aim_updates.load());
        ImGui::TextDisabled("Silent redirects %u",state.silent_shots.load());
        if(ImGui::BeginTable("diagnostics",4,ImGuiTableFlags_SizingStretchSame)){
            const auto cell=[](const char* label,unsigned value){ImGui::TableNextColumn();ImGui::TextDisabled("%s",label);ImGui::Text("%u",value);};
            cell("Callbacks",state.frames.load());cell("Players",state.players.load());
            cell("Candidates",snapshot.candidates);cell("Boxes",snapshot.count);
            cell("Team skipped",snapshot.same_team);cell("No head",snapshot.missing_head);
            cell("Faults",state.fault.load());
            const auto age=snapshot.timestamp?GetTickCount64()-snapshot.timestamp:0;
            cell("Snapshot ms",static_cast<unsigned>(std::min<ULONGLONG>(age,999999)));
            ImGui::EndTable();
        }
        ImGui::TextDisabled("GameAssembly: %.16s",offsets::GameAssemblySha256);
        ImGui::TextDisabled("Build timestamp 0x%08X  /  image 0x%08X",offsets::GameAssemblyTimestamp,offsets::GameAssemblySizeOfImage);
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button,{0.38f,0.18f,0.13f,1});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,{0.56f,0.25f,0.16f,1});
        if(ImGui::Button("Deactivate",{-1,34}))state.stop.store(true);
        ImGui::PopStyleColor(2);
    }
    ImGui::End();
    if(!open)state.menu.store(false);
}
void draw_aim_indicator(const Snapshot& snapshot,const ImVec2& display,float angle){
    if(!snapshot.camera_ready||!snapshot.timestamp||GetTickCount64()-snapshot.timestamp>300||
       !std::isfinite(snapshot.vertical_fov)||snapshot.vertical_fov<1||snapshot.vertical_fov>175)return;
    constexpr float radians=0.017453292519943295f;
    const float radius=std::tan(std::clamp(angle,1.0f,60.0f)*radians)/std::tan(snapshot.vertical_fov*0.5f*radians)*display.y*0.5f;
    if(!std::isfinite(radius)||radius<1||radius>display.y*4)return;
    auto* draw=ImGui::GetBackgroundDrawList();
    draw->AddCircle({display.x*0.5f,display.y*0.5f},radius,IM_COL32(245,184,95,180),96,1.0f);
    if(snapshot.target_selected&&snapshot.width>0&&snapshot.height>0&&std::isfinite(snapshot.target_x)&&std::isfinite(snapshot.target_y)){
        const ImVec2 point{snapshot.target_x*display.x/static_cast<float>(snapshot.width),snapshot.target_y*display.y/static_cast<float>(snapshot.height)};
        draw->AddCircle(point,5,IM_COL32(255,100,90,255),16,2);
    }
}
void draw_boxes(const Snapshot& snapshot,const ImVec2& display) {
    if(snapshot.width<=0||snapshot.height<=0||!snapshot.timestamp||GetTickCount64()-snapshot.timestamp>300)return;
    auto* draw=ImGui::GetBackgroundDrawList();
    const float sx=display.x/static_cast<float>(snapshot.width);
    const float sy=display.y/static_cast<float>(snapshot.height);
    const unsigned count=std::min(snapshot.count,static_cast<unsigned>(snapshot.boxes.size()));
    for(unsigned index=0;index<count;++index){
        const auto& box=snapshot.boxes[index];
        if(!std::isfinite(box.left)||!std::isfinite(box.right)||!std::isfinite(box.top)||!std::isfinite(box.bottom))continue;
        const ImVec2 low{box.left*sx,box.top*sy},high{box.right*sx,box.bottom*sy};
        if(high.x<=low.x||high.y<=low.y||high.x<0||high.y<0||low.x>display.x||low.y>display.y)continue;
        if(high.x-low.x>display.x*4||high.y-low.y>display.y*4)continue;
        draw->AddRect({low.x-1,low.y-1},{high.x+1,high.y+1},IM_COL32(8,12,15,255),0,0,3);
        draw->AddRect(low,high,IM_COL32(63,226,190,255),0,0,1.5f);
        const float fraction=std::clamp(static_cast<float>(box.health)/static_cast<float>(std::max(box.max_health,1)),0.0f,1.0f);
        draw->AddRectFilled({low.x-7,low.y},{low.x-3,high.y},IM_COL32(20,29,32,255));
        draw->AddRectFilled({low.x-6,high.y-(high.y-low.y)*fraction},{low.x-4,high.y},IM_COL32(86,231,132,255));
        const auto end=std::find(box.name.begin(),box.name.end(),'\0');
        const char* text_end=box.name.data()+std::distance(box.name.begin(),end);
        const auto size=ImGui::CalcTextSize(box.name.data(),text_end);
        const ImVec2 text_position{(low.x+high.x-size.x)*0.5f,low.y-size.y-5};
        draw->AddText({text_position.x+1,text_position.y+1},IM_COL32(8,12,15,255),box.name.data(),text_end);
        draw->AddText(text_position,IM_COL32(235,246,246,255),box.name.data(),text_end);
    }
}
}

void draw_frame(State& state,RECT& bounds){
    Snapshot snapshot;std::string status;
    {std::lock_guard<std::mutex> lock(state.mutex);snapshot=state.snapshot;status=state.status;}
    const ULONGLONG now=GetTickCount64();
    if(state.esp.load())draw_boxes(snapshot,ImGui::GetIO().DisplaySize);
    if(state.aimbot.load()||state.silent_aim.load())draw_aim_indicator(snapshot,ImGui::GetIO().DisplaySize,state.fov.load());
    if(state.menu.load())menu(state,snapshot,status,bounds);
    static ULONGLONG last_write{};
    if(!diagnostic_output.empty()&&now-last_write>=1000){
        const auto temporary=diagnostic_output.wstring()+L".tmp";
        {
            std::ofstream file(temporary,std::ios::trunc);
            if(file)file<<"{\n\"pid\":"<<GetCurrentProcessId()<<",\"callbacks\":"<<state.frames.load()
                <<",\"render_frames\":"<<state.render_frames.load()<<",\"players\":"<<state.players.load()
                <<",\"candidates\":"<<snapshot.candidates<<",\"boxes\":"<<snapshot.count
                <<",\"camera_width\":"<<snapshot.width<<",\"camera_height\":"<<snapshot.height
                <<",\"nearest_angle\":"<<snapshot.nearest_angle<<",\"aim_reason\":"<<snapshot.aim_reason
                <<",\"aim_updates\":"<<state.aim_updates.load()<<",\"silent_redirects\":"<<state.silent_shots.load()
                <<",\"silent_available\":"<<(state.silent_available.load()?"true":"false")
                <<",\"flying\":"<<(state.flying.load()?"true":"false")<<",\"menu_open\":"<<(state.menu.load()?"true":"false")
                <<",\"fault\":"<<state.fault.load()<<",\"snapshot_age_ms\":"<<(snapshot.timestamp?now-snapshot.timestamp:0)<<"\n}\n";
        }
        MoveFileExW(temporary.c_str(),diagnostic_output.c_str(),MOVEFILE_REPLACE_EXISTING);last_write=now;
    }
}
int run(State& state,HMODULE module) {
    wchar_t path[32768]{};
    GetModuleFileNameW(module,path,32768);
    diagnostic_output=std::filesystem::path(path).parent_path()/
        (L"BananaDiagnostics-"+std::to_wstring(GetCurrentProcessId())+L".json");
    state.menu=false;
    return run_in_game(state,module);
}
}
