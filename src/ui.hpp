#pragma once
#include <Windows.h>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
namespace ui {
struct Box {
    float left{},top{},right{},bottom{};
    int health{},max_health{100};
    std::array<char,96> name{};
};
struct Snapshot {
    std::array<Box,128> boxes{};
    unsigned count{};
    int width{},height{};
    unsigned candidates{},same_team{},missing_head{};
    unsigned in_front{},bad_projection{};
    float nearest_angle{180},vertical_fov{75};
    float target_x{},target_y{};
    bool target_selected{},camera_ready{};
    unsigned aim_reason{};
    ULONGLONG timestamp{};
};
struct State {
    std::atomic<bool> esp{true},aimbot{true},team_filter{false},outlines{false};
    std::atomic<bool> menu{true},stop{},restored{};
    std::atomic<float> fov{12},smooth{8};
    std::atomic<unsigned> frames{},players{},fault{};
    std::atomic<unsigned> aim_updates{};
    std::atomic<unsigned> render_frames{};
    std::atomic<bool> silent_aim{},fly{},speed_override{},jump_override{},fov_override{};
    std::atomic<float> fly_speed{15},speed_multiplier{1.5f},jump_multiplier{1.5f},camera_fov{90};
    std::atomic<unsigned> silent_shots{};
    std::atomic<bool> flying{};
    std::atomic<bool> silent_available{};
    std::atomic<bool> rapid_fire{},infinite_ammo{},weapon_available{};
    std::atomic<float> rapid_multiplier{3};
    std::atomic<int> weapon_ammo{-1};
    std::atomic<unsigned> host_weapon_updates{};
    std::mutex mutex;
    Snapshot snapshot;
    std::string status="Starting...";
    void set_status(const char* text){std::lock_guard<std::mutex> lock(mutex);status=text;}
};
int run(State& state,HMODULE module);
int run_in_game(State& state,HMODULE module);
void draw_frame(State& state,RECT& menu_bounds);
}
