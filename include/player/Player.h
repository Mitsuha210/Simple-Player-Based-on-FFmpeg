#pragma once

#include "BlockingQueue.h"
#include "Clock.h"
#include "Decoder.h"
#include "Demuxer.h"
#include "Renderer.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace sim_player {

class Player {
public:
    Player();
    ~Player();

    void open(const std::string& input_path);
    void play();
    void pause();
    void resume();
    void toggle_pause();
    void stop();
    bool is_opened() const { return opened_.load(); }
    bool is_playing() const { return playing_.load(); }
    bool is_paused() const { return paused_.load(); }
    bool is_finished() const;
    bool has_error() const { return error_.load(); }
    std::string last_error() const;
    void pump_video();
    void refresh_video();
    void seek_to(double position_seconds);
    void seek_relative(double delta_seconds);
    double duration_seconds() const;
    double current_position_seconds() const;
    void set_progress_preview(double position_seconds, bool dragging);
    bool is_progress_bar_hit(int x, int y) const;
    double progress_position_from_x(int x) const;

private:
    void report_error(const std::string& message);
    void seek_internal(double position_seconds, bool resume_after_seek);
    void restart_workers();
    void stop_workers();

    BlockingQueue<PacketPtr> audio_packets_ {256};
    BlockingQueue<PacketPtr> video_packets_ {256};
    BlockingQueue<VideoFrameItem> video_frames_ {32};
    Clock audio_clock_;
    Demuxer demuxer_;
    AudioRenderer audio_renderer_;
    VideoRenderer video_renderer_;
    std::unique_ptr<Decoder> audio_decoder_;
    std::unique_ptr<Decoder> video_decoder_;
    std::atomic<bool> opened_ {false};
    std::atomic<bool> playing_ {false};
    std::atomic<bool> paused_ {false};
    std::atomic<bool> stopping_ {false};
    std::atomic<bool> error_ {false};
    mutable std::mutex error_mutex_;
    mutable std::mutex state_mutex_;
    std::string last_error_;
};

}  // namespace sim_player
