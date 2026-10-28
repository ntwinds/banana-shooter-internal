#pragma once
#include <cstdint>
namespace ui {struct State;}
namespace weapon_features {
bool install(std::uintptr_t,ui::State&);
void tick();
bool before_attack(void* firearm);
void after_attack(void* firearm);
void restore();
void shutdown();
}
