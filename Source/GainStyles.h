#pragma once

#include <array>
#include <cstddef>

namespace nx
{
enum class GainStyle { universal, worship, gospel, pop, popRock, acoustic };
enum class StyleLane { drums, bass, harmony, backing, ambience, other };

struct GainStyleProfile
{
    const char* name;
    std::array<float, 6> offsets;
};

inline constexpr std::array<GainStyleProfile, 6> gainStyles {{
    {"Universal", {0, 0, 0, 0, 0, 0}},
    {"Worship", {-0.5f, -0.5f, -1.5f, 0.5f, 0.75f, -1.0f}},
    {"Gospel", {0.75f, 1.0f, -0.75f, 1.0f, -0.5f, 0.0f}},
    {"Pop", {0.5f, 0.75f, -1.0f, -0.5f, -0.75f, -0.5f}},
    {"Pop Rock", {1.0f, 0.5f, 0.75f, -0.75f, -1.0f, 0.0f}},
    {"Acoustic", {-1.5f, -0.5f, 0.75f, -0.75f, -1.5f, -0.5f}}
}};

constexpr float gainStyleOffset(GainStyle style, StyleLane lane)
{
    return gainStyles[static_cast<std::size_t>(style)].offsets[static_cast<std::size_t>(lane)];
}
}
