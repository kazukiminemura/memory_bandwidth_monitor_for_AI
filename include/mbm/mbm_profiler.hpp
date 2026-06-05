#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace mbm {

struct FunctionWindowStat {
    std::string name;
    std::uint64_t calls = 0;
    std::uint64_t bytes = 0;
    std::uint64_t total_ns = 0;
};

struct WindowSnapshot {
    std::vector<FunctionWindowStat> functions;
    std::chrono::steady_clock::time_point from;
    std::chrono::steady_clock::time_point to;
};

class Profiler {
public:
    static Profiler& instance();

    void enterScope(const char* function_name);
    void addBytes(std::uint64_t bytes);
    void leaveScope();

    WindowSnapshot consumeWindow();

private:
    Profiler();
    ~Profiler();
    Profiler(const Profiler&) = delete;
    Profiler& operator=(const Profiler&) = delete;
};

class ScopeGuard {
public:
    explicit ScopeGuard(const char* function_name);
    ~ScopeGuard();

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

private:
    bool active_ = false;
};

} // namespace mbm

#define MBM_SCOPE(name_literal) ::mbm::ScopeGuard mbm_scope_guard_##__LINE__(name_literal)
#define MBM_ADD_BYTES(byte_count) ::mbm::Profiler::instance().addBytes(static_cast<std::uint64_t>(byte_count))
