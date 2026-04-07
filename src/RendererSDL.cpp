#include "player/Renderer.h"

#include <SDL2/SDL.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <mutex>
#include <thread>

namespace sim_player {

namespace {

std::once_flag g_sdl_once;

void throw_sdl_error(const char* what) {
    throw std::runtime_error(std::string(what) + ": " + SDL_GetError());
}

}  // namespace

AudioRenderer::~AudioRenderer() {
    close();
}

void AudioRenderer::ensure_sdl_initialized() {
    std::call_once(g_sdl_once, [] {
        if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO) != 0) {
            throw_sdl_error("SDL_Init failed");
        }
    });
}

void AudioRenderer::open(const AVCodecContext& codec_context) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_sdl_initialized();
    sdl_ready_ = true;
    stop_requested_.store(false);
    target_sample_format_ = AV_SAMPLE_FMT_S16;
    target_sample_rate_ = codec_context.sample_rate > 0 ? codec_context.sample_rate : 48000;

    const int desired_channels = codec_context.ch_layout.nb_channels > 2
                                     ? 2
                                     : (codec_context.ch_layout.nb_channels > 0 ? codec_context.ch_layout.nb_channels : 2);
    av_channel_layout_default(&target_layout_, desired_channels);
    bytes_per_sample_ = av_get_bytes_per_sample(target_sample_format_);

    SDL_AudioSpec desired {};
    desired.freq = target_sample_rate_;
    desired.format = AUDIO_S16SYS;
    desired.channels = static_cast<Uint8>(target_layout_.nb_channels);
    desired.samples = 2048;
    desired.callback = nullptr;

    SDL_AudioSpec obtained {};
    device_id_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (device_id_ == 0) {
        throw_sdl_error("SDL_OpenAudioDevice failed");
    }

    av_channel_layout_uninit(&target_layout_);
    av_channel_layout_default(&target_layout_, obtained.channels);
    target_sample_rate_ = obtained.freq;
    bytes_per_sample_ = SDL_AUDIO_BITSIZE(obtained.format) / 8;

    if (swr_alloc_set_opts2(&swr_context_,
                            &target_layout_,
                            target_sample_format_,
                            target_sample_rate_,
                            &codec_context.ch_layout,
                            static_cast<AVSampleFormat>(codec_context.sample_fmt),
                            codec_context.sample_rate,
                            0,
                            nullptr) < 0) {
        throw std::runtime_error("swr_alloc_set_opts2 failed");
    }
    throw_on_ffmpeg_error(swr_init(swr_context_), "swr_init failed");

    pcm_buffer_.clear();
    started_ = false;
}

void AudioRenderer::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_.store(true);
    if (device_id_ != 0) {
        SDL_ClearQueuedAudio(device_id_);
        SDL_CloseAudioDevice(device_id_);
        device_id_ = 0;
    }
    if (swr_context_ != nullptr) {
        swr_free(&swr_context_);
    }
    av_channel_layout_uninit(&target_layout_);
    pcm_buffer_.clear();
    started_ = false;
    sdl_ready_ = false;
}

void AudioRenderer::request_stop() {
    stop_requested_.store(true);
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_id_ != 0) {
        SDL_ClearQueuedAudio(device_id_);
    }
}

void AudioRenderer::render(FramePtr frame, Clock& audio_clock, AVRational time_base) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_.load() || device_id_ == 0 || swr_context_ == nullptr) {
        return;
    }

    const int channel_count = target_layout_.nb_channels > 0 ? target_layout_.nb_channels : 2;
    const int max_output_samples = swr_get_out_samples(swr_context_, frame->nb_samples);
    if (max_output_samples <= 0) {
        return;
    }

    const int buffer_size =
        av_samples_get_buffer_size(nullptr, channel_count, max_output_samples, target_sample_format_, 1);
    throw_on_ffmpeg_error(buffer_size, "av_samples_get_buffer_size failed");

    pcm_buffer_.resize(static_cast<std::size_t>(buffer_size));
    std::uint8_t* output[] = {pcm_buffer_.data()};
    const std::uint8_t** input_data = const_cast<const std::uint8_t**>(frame->extended_data);
    const int converted_samples =
        swr_convert(swr_context_, output, max_output_samples, input_data, frame->nb_samples);
    throw_on_ffmpeg_error(converted_samples, "swr_convert failed");

    const int converted_size = converted_samples * channel_count * bytes_per_sample_;
    if (converted_size <= 0) {
        return;
    }

    const std::uint32_t max_queued_bytes =
        static_cast<std::uint32_t>(target_sample_rate_ * channel_count * bytes_per_sample_);
    while (!stop_requested_.load() && SDL_GetQueuedAudioSize(device_id_) > max_queued_bytes) {
        SDL_Delay(5);
    }
    if (stop_requested_.load()) {
        return;
    }

    // SDL_QueueAudio matches the current push model: the decode thread produces PCM and submits it directly.
    if (SDL_QueueAudio(device_id_, pcm_buffer_.data(), static_cast<Uint32>(converted_size)) != 0) {
        throw_sdl_error("SDL_QueueAudio failed");
    }
    if (!started_) {
        SDL_PauseAudioDevice(device_id_, 0);
        started_ = true;
    }

    // swr_convert is needed because codec output formats are not stable across media streams.
    const int64_t pts = frame->best_effort_timestamp;
    if (pts != AV_NOPTS_VALUE) {
        audio_clock.set(pts * av_q2d(time_base));
    }
}

VideoRenderer::~VideoRenderer() {
    close();
}

void VideoRenderer::ensure_sdl_initialized() {
    std::call_once(g_sdl_once, [] {
        if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO) != 0) {
            throw_sdl_error("SDL_Init failed");
        }
    });
}

void VideoRenderer::open(const AVCodecContext& codec_context) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_sdl_initialized();
    sdl_ready_ = true;
    stop_requested_.store(false);
    width_ = codec_context.width;
    height_ = codec_context.height;
    if (width_ <= 0 || height_ <= 0) {
        throw std::runtime_error("invalid video size");
    }

    window_ =
        SDL_CreateWindow("sim_player", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width_, height_, SDL_WINDOW_SHOWN);
    if (window_ == nullptr) {
        throw_sdl_error("SDL_CreateWindow failed");
    }

    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer_ == nullptr) {
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    }
    if (renderer_ == nullptr) {
        throw_sdl_error("SDL_CreateRenderer failed");
    }

    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width_, height_);
    if (texture_ == nullptr) {
        throw_sdl_error("SDL_CreateTexture failed");
    }

    sws_context_ = sws_getContext(width_,
                                  height_,
                                  static_cast<AVPixelFormat>(codec_context.pix_fmt),
                                  width_,
                                  height_,
                                  AV_PIX_FMT_YUV420P,
                                  SWS_BILINEAR,
                                  nullptr,
                                  nullptr,
                                  nullptr);
    if (sws_context_ == nullptr) {
        throw std::runtime_error("sws_getContext failed");
    }

    yuv_frame_ = make_frame();
    const int buffer_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, width_, height_, 1);
    throw_on_ffmpeg_error(buffer_size, "av_image_get_buffer_size failed");
    yuv_buffer_.resize(static_cast<std::size_t>(buffer_size));
    throw_on_ffmpeg_error(
        av_image_fill_arrays(yuv_frame_->data, yuv_frame_->linesize, yuv_buffer_.data(), AV_PIX_FMT_YUV420P, width_, height_, 1),
        "av_image_fill_arrays failed");

    last_video_pts_ = 0.0;
    has_last_video_pts_ = false;
}

void VideoRenderer::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_.store(true);
    yuv_frame_.reset();
    yuv_buffer_.clear();
    if (sws_context_ != nullptr) {
        sws_freeContext(sws_context_);
        sws_context_ = nullptr;
    }
    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    if (renderer_ != nullptr) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
    has_last_video_pts_ = false;
    sdl_ready_ = false;
}

void VideoRenderer::request_stop() {
    stop_requested_.store(true);
}

void VideoRenderer::render(FramePtr frame, const Clock& master_clock, AVRational time_base) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_.load() || renderer_ == nullptr || texture_ == nullptr || sws_context_ == nullptr) {
        return;
    }

    const int64_t pts = frame->best_effort_timestamp;
    const double video_pts = pts == AV_NOPTS_VALUE ? 0.0 : pts * av_q2d(time_base);

    // Audio is the master clock because the sound device advances more steadily than display refresh.
    const auto delay = compute_delay(video_pts, master_clock.get());
    if (stop_requested_.load()) {
        return;
    }
    std::this_thread::sleep_for(delay);
    if (stop_requested_.load()) {
        return;
    }

    AVFrame* display_frame = frame.get();
    if (frame->format != AV_PIX_FMT_YUV420P) {
        throw_on_ffmpeg_error(
            sws_scale(sws_context_, frame->data, frame->linesize, 0, height_, yuv_frame_->data, yuv_frame_->linesize),
            "sws_scale failed");
        display_frame = yuv_frame_.get();
    }

    // SDL_UpdateYUVTexture works directly with YUV420P planes, so we normalize frames to that layout first.
    if (SDL_UpdateYUVTexture(texture_,
                             nullptr,
                             display_frame->data[0],
                             display_frame->linesize[0],
                             display_frame->data[1],
                             display_frame->linesize[1],
                             display_frame->data[2],
                             display_frame->linesize[2]) != 0) {
        throw_sdl_error("SDL_UpdateYUVTexture failed");
    }

    if (SDL_RenderClear(renderer_) != 0) {
        throw_sdl_error("SDL_RenderClear failed");
    }
    if (SDL_RenderCopy(renderer_, texture_, nullptr, nullptr) != 0) {
        throw_sdl_error("SDL_RenderCopy failed");
    }
    SDL_RenderPresent(renderer_);
}

std::chrono::milliseconds VideoRenderer::compute_delay(double video_pts, double master_pts) {
    double delta = 0.0;
    if (master_pts > 0.0) {
        delta = video_pts - master_pts;
    } else if (has_last_video_pts_) {
        delta = video_pts - last_video_pts_;
    }

    last_video_pts_ = video_pts;
    has_last_video_pts_ = true;
    delta = std::clamp(delta, 0.0, 0.1);
    return std::chrono::milliseconds(static_cast<int>(delta * 1000.0));
}

}  // namespace sim_player
