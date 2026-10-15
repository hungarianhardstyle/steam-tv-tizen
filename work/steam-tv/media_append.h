#pragma once
#include <mutex>

template<class Append>
static auto AppendWithoutConfigLock(std::unique_lock<std::mutex>& config,
                                    std::mutex& track, Append append) -> decltype(append()) {
    std::unique_lock<std::mutex> active(track);
    config.unlock();
    auto result = append();
    // Close holds config while waiting for track; release track before reacquiring config.
    active.unlock();
    config.lock();
    return result;
}
