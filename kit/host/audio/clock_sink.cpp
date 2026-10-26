// clock_sink.cpp - a sink with no device: it pulls the mix in real time and
// discards it. The mixer falls back to it when the platform's output will not
// open (a Mac whose session cannot reach Core Audio), so play cursors still
// move and a game that waits on its music or a stream keeps going in silence.
#include "sink.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

class ClockSink final : public AudioSink {
  public:
    ~ClockSink() override {
        stop();
    }
    bool start(double rate, const audio::Layout &,
               std::function<void(float *const *, uint32_t)> render) override {
        if (thread_.joinable())
            return true;
        if (rate <= 0)
            return false;
        render_ = std::move(render);
        rate_ = rate;
        stop_ = false;
        paused_ = false;
        thread_ = std::thread([this] { run(); });
        return true;
    }
    void stop() override {
        if (!thread_.joinable())
            return;
        stop_ = true;
        thread_.join();
    }
    bool running() const override {
        return thread_.joinable();
    }
    void pause(bool paused) override {
        paused_ = paused;
    }

  private:
    // Blocks of 512 frames, each released when the wall clock reaches it, so
    // the mix advances at the device rate without drifting.
    void run() {
        // The mixer writes six planes whatever the layout, into zeroed buffers.
        constexpr uint32_t kFrames = 512, kPlanes = 6;
        std::vector<std::vector<float>> planes(kPlanes, std::vector<float>(kFrames));
        std::vector<float *> pointers(kPlanes);
        for (unsigned c = 0; c < kPlanes; ++c)
            pointers[c] = planes[c].data();
        const auto period = std::chrono::duration<double>(kFrames / rate_);
        auto due = std::chrono::steady_clock::now();
        while (!stop_) {
            due += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
            std::this_thread::sleep_until(due);
            if (paused_)
                continue;
            for (auto &plane : planes)
                std::fill(plane.begin(), plane.end(), 0.0f);
            render_(pointers.data(), kFrames);
        }
    }
    std::function<void(float *const *, uint32_t)> render_;
    double rate_ = 48000;
    std::atomic<bool> stop_{false}, paused_{false};
    std::thread thread_;
};

} // namespace

std::unique_ptr<AudioSink> make_clock_sink() {
    return std::make_unique<ClockSink>();
}
