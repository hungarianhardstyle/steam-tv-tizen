#include "steam-tv/media_append.h"
#include <atomic>
#include <cassert>
#include <memory>
#include <thread>
#include <unistd.h>

static void Await(const std::atomic<bool>& flag) {
    for (unsigned i = 0; i < 2000 && !flag.load(); ++i) usleep(1000);
    assert(flag.load());
}

extern "C" void test_media_append(void) {
    std::mutex config;
    std::atomic<bool> audioInside{false}, releaseAudio{false}, videoDone{false};
    std::atomic<bool> closeWaiting{false}, closeDone{false};
    std::atomic<unsigned> destroyed{0};
    struct Tracks {
        std::mutex video, audio;
        std::atomic<unsigned>& destroyed;
        explicit Tracks(std::atomic<unsigned>& count) : destroyed(count) {}
        ~Tracks() { ++destroyed; }
    };
    auto player = std::make_shared<Tracks>(destroyed);
    std::thread audio([&] {
        std::unique_lock<std::mutex> lock(config);
        auto current = player;
        int result = AppendWithoutConfigLock(lock, current->audio, [&] {
            audioInside = true;
            Await(releaseAudio);
            return 17;
        });
        assert(lock.owns_lock() && result == 17 && destroyed == 0);
    });
    Await(audioInside);
    std::thread video([&] {
        std::unique_lock<std::mutex> lock(config);
        auto current = player;
        assert(AppendWithoutConfigLock(lock, current->video, [] { return 23; }) == 23);
        assert(lock.owns_lock());
        videoDone = true;
    });
    // Video must complete while the audio platform call remains blocked.
    Await(videoDone);
    video.join();
    std::thread close([&] {
        std::lock_guard<std::mutex> lock(config);
        auto closing = player;
        closeWaiting = true;
        std::lock_guard<std::mutex> videoLock(closing->video);
        std::lock_guard<std::mutex> audioLock(closing->audio);
        player.reset();
        closeDone = true;
    });
    Await(closeWaiting);
    assert(!closeDone && destroyed == 0);
    releaseAudio = true;
    // Close waits for append, but append must not need config before releasing its track lock.
    close.join();
    audio.join();
    assert(closeDone && destroyed == 1);
}
