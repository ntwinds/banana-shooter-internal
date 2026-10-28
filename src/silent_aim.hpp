#pragma once
#include <cstdint>
#include "aim_math.hpp"
namespace ui {struct State;}
namespace silent_aim {
// Install after the module hash check.
bool install(std::uintptr_t module_base,ui::State& state);
// Caller supplies an eligible target already selected within its FOV policy.
void update_target(aim::Vec3 world,bool valid);
void shutdown();
}
