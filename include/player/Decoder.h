#pragma once

#include "BlockingQueue.h"
#include "Clock.h"
#include "FfmpegUtils.h"
#include "Renderer.h"

#include <atomic>
#include <functional>
#include <thread>

namespace sim_player {

struct VideoFrameItem {
    FramePtr frame;
    AVRational time_base;
};

class Decoder {
public:
    enum class Kind {
        Audio,
        Video
    };

    Decoder(Kind kind,
            AVStream* stream,
            BlockingQueue<PacketPtr>& packet_queue,
            Clock& master_clock,
            AudioRenderer* audio_renderer,
            VideoRenderer* video_renderer,
            BlockingQueue<VideoFrameItem>* video_frames = nullptr);

    ~Decoder();

    void start();
    void stop();
    void flush();
    bool finished() const { return finished_.load(); }
    void set_error_callback(std::function<void(const std::string&)> callback);

private:
    void run();
    void decode_packet(AVPacket* packet);

    Kind kind_;
    AVStream* stream_;
    BlockingQueue<PacketPtr>& packet_queue_;
    Clock& master_clock_;
    AudioRenderer* audio_renderer_;
    VideoRenderer* video_renderer_;
    BlockingQueue<VideoFrameItem>* video_frames_;
    AVCodecContext* codec_context_ = nullptr;
    std::atomic<bool> running_ {false};
    std::atomic<bool> finished_ {false};
    std::function<void(const std::string&)> error_callback_;
    std::thread worker_;
    bool renderer_opened_ = false;
};

}  // namespace sim_player
