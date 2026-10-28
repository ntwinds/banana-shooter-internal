#pragma once
#include <algorithm>
#include <cmath>
namespace aim {
constexpr float pi = 3.14159265358979323846f;
struct Vec3 { float x{}, y{}, z{}; };
inline Vec3 difference(Vec3 a, Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
inline float length(Vec3 v) { return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); }
inline bool finite(Vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
inline float wrap(float degrees) { return std::remainder(degrees,360.0f); }
inline float angle(Vec3 a, Vec3 b) {
    const float product=length(a)*length(b);
    if (!std::isfinite(product)||product<0.00001f) return 180.0f;
    return std::acos(std::clamp((a.x*b.x+a.y*b.y+a.z*b.z)/product,-1.0f,1.0f))*180.0f/pi;
}
inline Vec3 angles(Vec3 direction) {
    return {-std::atan2(direction.y,std::hypot(direction.x,direction.z))*180.0f/pi,
             std::atan2(direction.x,direction.z)*180.0f/pi,0.0f};
}
inline bool shot_direction(Vec3 original,Vec3 origin,Vec3 target,float fov,Vec3& output){
    const Vec3 delta=difference(target,origin);
    const float distance=length(delta);
    if(!finite(original)||!finite(origin)||!finite(target)||!std::isfinite(fov)||fov<=0||
       !std::isfinite(distance)||distance<0.001f||angle(original,delta)>fov)return false;
    output={delta.x/distance,delta.y/distance,delta.z/distance};return true;
}
inline float smoothing(float strength,float dt) {
    return 1.0f-std::exp(-std::clamp(dt,0.001f,0.1f)*60.0f/std::max(strength,1.0f));
}
}
