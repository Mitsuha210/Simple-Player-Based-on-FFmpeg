#pragma once

#include "Clock.h"
#include "FfmpegUtils.h"

#include <chrono>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>

struct SwrContext;
struct SwsContext;
typedef uint32_t SDL_AudioDeviceID;
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace sim_player {

class AudioRenderer {
public:
    AudioRenderer() = default;
    virtual ~AudioRenderer();

    virtual void open(const AVCodecContext& codec_context);
    virtual void close();
    virtual void request_stop();
    virtual void set_paused(bool paused);
    virtual void flush();
    virtual void render(FramePtr frame, Clock& audio_clock, AVRational time_base);

private:
    static void ensure_sdl_initialized();

    SDL_AudioDeviceID device_id_ = 0;
    SwrContext* swr_context_ = nullptr;
    AVChannelLayout target_layout_ {};
    int target_sample_rate_ = 0;
    AVSampleFormat target_sample_format_ = AV_SAMPLE_FMT_NONE;
    int bytes_per_sample_ = 0;
    bool started_ = false;
    bool sdl_ready_ = false;
    std::atomic<bool> stop_requested_ {false};
    bool paused_ = false;
    mutable std::mutex mutex_;
    std::vector<std::uint8_t> pcm_buffer_;
};

class VideoRenderer {
public:
    VideoRenderer() = default;
    virtual ~VideoRenderer();

    virtual void open(const AVCodecContext& codec_context);
    virtual void close();
    virtual void request_stop();
    virtual void flush();
    virtual void render(FramePtr frame, const Clock& master_clock, AVRational time_base);
    virtual void present();
    virtual void set_progress(double position_seconds, double duration_seconds, bool dragging);
    virtual bool is_progress_bar_hit(int x, int y) const;
    virtual double progress_from_x(int x) const;
    virtual double current_pts() const;

private:
    static void ensure_sdl_initialized();
    std::chrono::milliseconds compute_delay(double video_pts, double master_pts);
    void draw_scene_locked();

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    SwsContext* sws_context_ = nullptr;
    FramePtr yuv_frame_;
    std::vector<std::uint8_t> yuv_buffer_;
    int width_ = 0;
    int height_ = 0;
    double last_video_pts_ = 0.0;
    bool has_last_video_pts_ = false;
    double displayed_pts_ = 0.0;
    double progress_position_seconds_ = 0.0;
    double progress_duration_seconds_ = 0.0;
    bool dragging_progress_ = false;
    bool sdl_ready_ = false;
    std::atomic<bool> stop_requested_ {false};
    mutable std::mutex mutex_;
};

}  // namespace sim_player
