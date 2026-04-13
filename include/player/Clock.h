#pragma once

#include <atomic>

namespace sim_player {

class Clock {
public:
    void set(double pts_seconds) { //更新
        pts_seconds_.store(pts_seconds, std::memory_order_relaxed);
        //memory_order_relaxed means it only guarantee that it is safe for the variable itself on read&write,but not guarantee synchronizing with other variables
    }

    double get() const { //读取当前时钟值
        return pts_seconds_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<double> pts_seconds_ {0.0};
};

}  // namespace sim_player
