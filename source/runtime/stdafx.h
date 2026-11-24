#pragma once

// Torchlight runtime precompiled header.
// Derived from UnleashedRecomp's stdafx.h; ImGui, SDL_mixer, zstd, stb, smol-v,
// D3D12/DXC and other Sonic-only dependencies removed.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__) || defined(__ANDROID__)
#include <unistd.h>
#include <pwd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <semaphore>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <xbox.h>
#include <xxhash.h>
#include <ankerl/unordered_dense.h>
#include <ppc_recomp_shared.h>
#include <toml++/toml.hpp>
#include <blockingconcurrentqueue.h>
#include <SDL.h>
#include <o1heap.h>
#include <fmt/core.h>

#include "framework.h"
#include "mutex.h"

#ifndef _WIN32
#include <sys/mman.h>
#endif
