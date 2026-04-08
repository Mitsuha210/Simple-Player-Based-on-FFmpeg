#pragma once

#include "BlockingQueue.h"
#include "FfmpegUtils.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace sim_player {

struct MediaStreams {
    AVStream* audio = nullptr;
    AVStream* video = nullptr;
};

class Demuxer {
public:
    Demuxer(BlockingQueue<PacketPtr>& audio_packets, BlockingQueue<PacketPtr>& video_packets);
    ~Demuxer();

    void open(const std::string& input_path);
    void start();
    void stop();
    void seek(double position_seconds);
    void set_error_callback(std::function<void(const std::string&)> callback);

    AVFormatContext* format_context() const { return format_context_; }
    const MediaStreams& streams() const { return streams_; }
    bool finished() const { return finished_.load(); }
    double duration_seconds() const;

private:
    void run();
    void push_eof_packets();

    BlockingQueue<PacketPtr>& audio_packets_;
    BlockingQueue<PacketPtr>& video_packets_;
    AVFormatContext* format_context_ = nullptr;
    MediaStreams streams_;
    std::atomic<bool> running_ {false};
    std::atomic<bool> finished_ {false};
    std::function<void(const std::string&)> error_callback_;
    std::thread worker_;
};

}  // namespace sim_player
