#include "aim_math.hpp"
#include "cursor_policy.hpp"
#include <cstdio>
#include <limits>
int main() {
    int failed=0;
    auto check=[&](bool good,const char* name){if(!good){std::printf("FAILED: %s\n",name);++failed;}};
    check(std::abs(aim::angles({0,0,1}).y)<0.001f,"forward yaw");
    check(std::abs(aim::angles({1,0,0}).y-90)<0.001f,"right yaw");
    check(std::abs(aim::angles({0,1,0}).x+90)<0.001f,"up pitch");
    check(std::abs(aim::wrap(-358)-2)<0.001f,"yaw wrap boundary");
    check(std::abs(aim::angle({0,0,1},{1,0,0})-90)<0.001f,"angular FOV");
    check(aim::angle({0,0,0},{0,0,1})==180.0f,"zero direction rejected");
    const float a=aim::smoothing(8,1.0f/60),b=aim::smoothing(8,1.0f/120);
    check(std::abs(a-(1-(1-b)*(1-b)))<0.00001f,"frame rate independent smoothing");
    const auto paused=input_policy::restore({1,false},true);
    check(paused.lock==0&&paused.visible,"pause menu remains clickable after closing ImGui");
    const auto playing=input_policy::restore({1,false},false);
    check(playing.lock==1&&!playing.visible,"gameplay cursor restored after closing ImGui");
    aim::Vec3 redirected{3,4,5};
    check(aim::shot_direction({0,0,1},{2,0,0},{2,0,10},12,redirected)&&std::abs(redirected.z-1)<0.00001f,"silent direction uses shot origin");
    const auto before=redirected;
    check(!aim::shot_direction({0,0,1},{0,0,0},{10,0,0},12,redirected)&&redirected.z==before.z,"outside silent FOV preserves original output");
    check(!aim::shot_direction({0,0,1},{0,0,0},{0,0,0},12,redirected),"zero length shot rejected");
    check(!aim::shot_direction({0,0,1},{0,0,0},{0,0,1},std::numeric_limits<float>::quiet_NaN(),redirected),"invalid silent FOV rejected");
    std::printf("Aim math: %d failures\n",failed);
    return failed?1:0;
}
