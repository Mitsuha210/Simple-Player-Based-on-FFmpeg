#include "player/Renderer.h"

#include <SDL2/SDL.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h> //image memory-related
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <thread>

namespace sim_player {

namespace { //other files cannot use these functions,like private

std::once_flag g_sdl_once;//used for ensure SDL is initialized only once
//UI drawing parameters
constexpr int kProgressBarHeight = 20;
constexpr int kProgressBarMargin = 12;
constexpr int kGlyphScale = 2;
constexpr int kGlyphWidth = 3;
constexpr int kGlyphHeight = 5;
constexpr int kGlyphSpacing = 2;

void throw_sdl_error(const char* what) {
    throw std::runtime_error(std::string(what) + ": " + SDL_GetError());
}

//Character dot matrix font table
//为什么要自己画点阵数字
std::array<const char*, kGlyphHeight> glyph_for_char(char c) {
    switch (c) {
    case '0':
        return {"111", "101", "101", "101", "111"};
    case '1':
        return {"010", "110", "010", "010", "111"};
    case '2':
        return {"111", "001", "111", "100", "111"};
    case '3':
        return {"111", "001", "111", "001", "111"};
    case '4':
        return {"101", "101", "111", "001", "001"};
    case '5':
        return {"111", "100", "111", "001", "111"};
    case '6':
        return {"111", "100", "111", "101", "111"};
    case '7':
        return {"111", "001", "001", "001", "001"};
    case '8':
        return {"111", "101", "111", "101", "111"};
    case '9':
        return {"111", "101", "111", "001", "111"};
    case ':':
        return {"000", "010", "000", "010", "000"};
    case '/':
        return {"001", "001", "010", "100", "100"};
    case ' ':
    default:
        return {"000", "000", "000", "000", "000"};
    }
}

//convert second to the format'm:ss'
std::string format_time_label(double seconds) {
    //Round to the nearest whole second
    const int total_seconds = std::max(0, static_cast<int>(seconds + 0.5));
    const int minutes = total_seconds / 60;
    const int remainder = total_seconds % 60;

    std::string label = std::to_string(minutes);
    if (remainder < 10) {
        label += ":0";
    } else {
        label += ":";
    }
    label += std::to_string(remainder);
    return label;
}

//draw texts a square a time using SDL
void draw_text(SDL_Renderer* renderer, int x, int y, const std::string& text) {
    for (char c : text) {
        const auto glyph = glyph_for_char(c);
        for (int row = 0; row < kGlyphHeight; ++row) {
            for (int col = 0; col < kGlyphWidth; ++col) {
                if (glyph[row][col] != '1') {
                    continue;
                }
                //draw a solid rectangle
                SDL_Rect pixel {x + col * kGlyphScale, y + row * kGlyphScale, kGlyphScale, kGlyphScale};
                SDL_RenderFillRect(renderer, &pixel);
            }
        }
        x += kGlyphWidth * kGlyphScale + kGlyphSpacing;
    }
}

}  // namespace

AudioRenderer::~AudioRenderer() {
    close();
}

//ensure SDL is initialized only once
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
    paused_ = false;
    //specify target output format
    target_sample_format_ = AV_SAMPLE_FMT_S16;
    target_sample_rate_ = codec_context.sample_rate > 0 ? codec_context.sample_rate : 48000;

    const int desired_channels = codec_context.ch_layout.nb_channels > 2
                                     ? 2
                                     : (codec_context.ch_layout.nb_channels > 0 ? codec_context.ch_layout.nb_channels : 2);
    //generate default layout depends on channel_nb
    av_channel_layout_default(&target_layout_, desired_channels);
    bytes_per_sample_ = av_get_bytes_per_sample(target_sample_format_);

    SDL_AudioSpec desired {};
    desired.freq = target_sample_rate_;
    desired.format = AUDIO_S16SYS;
    desired.channels = static_cast<Uint8>(target_layout_.nb_channels);
    desired.samples = 2048;
    desired.callback = nullptr; //use queue mode instead of callback mode

    //open a specific audio device 
    SDL_AudioSpec obtained {};
    device_id_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (device_id_ == 0) {
        throw_sdl_error("SDL_OpenAudioDevice failed");
    }

    //rewrite format by actually obtained device
    av_channel_layout_uninit(&target_layout_);
    av_channel_layout_default(&target_layout_, obtained.channels);
    target_sample_rate_ = obtained.freq;
    bytes_per_sample_ = SDL_AUDIO_BITSIZE(obtained.format) / 8;

    //create audio resampling device
    //Decoder output format → Converted to the actual playback format required by SDL
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
    //initialize SwrContext
    throw_on_ffmpeg_error(swr_init(swr_context_), "swr_init failed");

    //clear status
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

void AudioRenderer::set_paused(bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = paused;
    if (device_id_ != 0) {
        SDL_PauseAudioDevice(device_id_, paused ? 1 : 0);
    }
}

void AudioRenderer::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_.store(false);
    if (device_id_ != 0) {
        SDL_ClearQueuedAudio(device_id_);
        if (!paused_) {
            SDL_PauseAudioDevice(device_id_, 0);
        }
    }
}

void AudioRenderer::render(FramePtr frame, Clock& audio_clock, AVRational time_base) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_.load() || device_id_ == 0 || swr_context_ == nullptr) {
        return;
    }

    const int channel_count = target_layout_.nb_channels > 0 ? target_layout_.nb_channels : 2;
    //Based on the number of input samples, estimate how many samples this conversion process will output at most.
    const int max_output_samples = swr_get_out_samples(swr_context_, frame->nb_samples);
    if (max_output_samples <= 0) {
        return;
    }

    const int buffer_size =
        av_samples_get_buffer_size(nullptr, channel_count, max_output_samples, target_sample_format_, 1);
    throw_on_ffmpeg_error(buffer_size, "av_samples_get_buffer_size failed");

    pcm_buffer_.resize(static_cast<std::size_t>(buffer_size));
    //a pointer for each plain
    std::uint8_t* output[] = {pcm_buffer_.data()};
    //frame->extended_data 指向 AVFrame 音频数据平面。
    const std::uint8_t** input_data = const_cast<const std::uint8_t**>(frame->extended_data);
    //convert input into target format,return actual output sample size
    const int converted_samples =
        swr_convert(swr_context_, output, max_output_samples, input_data, frame->nb_samples);
    throw_on_ffmpeg_error(converted_samples, "swr_convert failed");

    const int converted_size = converted_samples * channel_count * bytes_per_sample_;
    if (converted_size <= 0) {
        return;
    }

    //总字节数 = 样本数 × 声道数 × 每样本字节数
    //这里设置了队列中允许的最大字节数，超过是让线程延迟5ms
    const std::uint32_t max_queued_bytes =
        static_cast<std::uint32_t>(target_sample_rate_ * channel_count * bytes_per_sample_ * 0.1) ;
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
    if (!started_) { //第一次启动
        SDL_PauseAudioDevice(device_id_, paused_ ? 1 : 0);
        started_ = true;
    }

    // swr_convert is needed because codec output formats are not stable across media streams.
    const int64_t pts = frame->best_effort_timestamp;
    const double_t frame_pts_seconds = pts * av_q2d(time_base);
    const double frame_duration_seconds = 
    static_cast<double>(converted_samples) / static_cast<double>(target_sample_rate_);
    const double bytes_per_second =
        static_cast<double>(target_sample_rate_) *
        static_cast<double>(channel_count) *
        static_cast<double>(bytes_per_sample_);
    double_t queued_seconds = 
    SDL_GetQueuedAudioSize(device_id_) / bytes_per_second;
    //真实播放时间 = 队列末尾时间 - 队列剩余长度
    //audio_clock idicates current playtime
    double_t actual_audio_clock = 
    frame_pts_seconds + frame_duration_seconds - queued_seconds;
    if (pts != AV_NOPTS_VALUE) {
        audio_clock.set(actual_audio_clock);
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
        SDL_CreateWindow("sim_player", //title
            SDL_WINDOWPOS_CENTERED, //x position
            SDL_WINDOWPOS_CENTERED, //y position
            width_, height_, 
            SDL_WINDOW_SHOWN);//sign:display window
    if (window_ == nullptr) {
        throw_sdl_error("SDL_CreateWindow failed");
    }

    //create a 2d renderer for window 
    renderer_ = SDL_CreateRenderer(window_, //target window
        -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer_ == nullptr) {
        //back to software renderer if hardware accelerating failed
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    }
    if (renderer_ == nullptr) {
        throw_sdl_error("SDL_CreateRenderer failed");
    }

    //create an updatable texture
    texture_ = SDL_CreateTexture(renderer_, 
        SDL_PIXELFORMAT_IYUV, //Indicates the YUV420P three-plane format
        SDL_TEXTUREACCESS_STREAMING, //indicates it will be updated frequently
        width_, height_);
    if (texture_ == nullptr) {
        throw_sdl_error("SDL_CreateTexture failed");
    }

    //create pixel format converter
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

    //prepare a YUV target frame
    yuv_frame_ = make_frame();
    const int buffer_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, width_, height_, 1);
    throw_on_ffmpeg_error(buffer_size, "av_image_get_buffer_size failed");
    //allocate actual cache
    yuv_buffer_.resize(static_cast<std::size_t>(buffer_size));
    //let yuv_frame pointer point to this memory
    throw_on_ffmpeg_error(
        av_image_fill_arrays(yuv_frame_->data, 
            yuv_frame_->linesize, 
            yuv_buffer_.data(), 
            AV_PIX_FMT_YUV420P,
             width_, height_, 1),
        "av_image_fill_arrays failed");

    //initialize displaying status
    last_video_pts_ = 0.0;
    has_last_video_pts_ = false;
    displayed_pts_ = 0.0;
    progress_position_seconds_ = 0.0;
    progress_duration_seconds_ = 0.0;
    dragging_progress_ = false;
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
    displayed_pts_ = 0.0;
    progress_position_seconds_ = 0.0;
    progress_duration_seconds_ = 0.0;
    dragging_progress_ = false;
    sdl_ready_ = false;
}

void VideoRenderer::request_stop() {
    stop_requested_.store(true);
}

void VideoRenderer::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_.store(false);
    last_video_pts_ = 0.0;
    has_last_video_pts_ = false;
    //update the display time to the target position before actually display
    displayed_pts_ = progress_position_seconds_;
    if (renderer_ == nullptr) {
        return;
    }
    if (SDL_SetRenderDrawColor(renderer_, 0, 0, 0, SDL_ALPHA_OPAQUE) != 0) {
        throw_sdl_error("SDL_SetRenderDrawColor failed");
    }
    if (SDL_RenderClear(renderer_) != 0) {
        throw_sdl_error("SDL_RenderClear failed");
    }
    draw_scene_locked();//?
}

void VideoRenderer::render(FramePtr frame, const Clock& master_clock, AVRational time_base) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_.load() || renderer_ == nullptr || texture_ == nullptr || sws_context_ == nullptr) {
        return;
    }

    const int64_t pts = frame->best_effort_timestamp;
    const double video_pts = pts == AV_NOPTS_VALUE ? 0.0 : pts * av_q2d(time_base);

    // Audio is the master clock because the sound device advances more steadily than display refresh.
    //key step for synchronization
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
            //update pixel format
            sws_scale(sws_context_, //contains info of how to convert 
                frame->data, 
                frame->linesize, 
                0, //start from row 0
                height_, //处理height_行
                yuv_frame_->data, //output to
                yuv_frame_->linesize),
            "sws_scale failed");
        //update display_frame as yuv_frame has been converted
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

    //clear screen first
    if (SDL_RenderClear(renderer_) != 0) {
        throw_sdl_error("SDL_RenderClear failed");
    }
    displayed_pts_ = video_pts;
    draw_scene_locked();//?
}

//redraw current frame
void VideoRenderer::present() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (renderer_ == nullptr || texture_ == nullptr) {
        return;
    }
    if (SDL_RenderClear(renderer_) != 0) {
        throw_sdl_error("SDL_RenderClear failed");
    }
    draw_scene_locked();
}

//set status of progress bar
void VideoRenderer::set_progress(double position_seconds, double duration_seconds, bool dragging) {
    std::lock_guard<std::mutex> lock(mutex_);
    //position->current position
    //duration->video total duration
    progress_position_seconds_ = std::max(0.0, position_seconds);
    progress_duration_seconds_ = std::max(0.0, duration_seconds);
    //set statu of if_dragging
    dragging_progress_ = dragging; 
}

//judge if the coordinate of mouse hit the bottom area of progress bar
bool VideoRenderer::is_progress_bar_hit(int x, int y) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (renderer_ == nullptr || width_ <= 0 || height_ <= 0) {
        return false;
    }
    //draw a rectangle area as progress bar
    const int left = kProgressBarMargin;
    const int right = width_ - kProgressBarMargin;
    const int top = height_ - kProgressBarHeight - kProgressBarMargin;
    const int bottom = height_ - kProgressBarMargin;
    return x >= left && x <= right && y >= top && y <= bottom;
}

//map the x-coordinate of mouse to the media time
double VideoRenderer::progress_from_x(int x) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (width_ <= 0 || progress_duration_seconds_ <= 0.0) {
        return 0.0;
    }
    const int left = kProgressBarMargin;
    const int right = width_ - kProgressBarMargin;
    const double ratio =
        std::clamp(static_cast<double>(x - left) / static_cast<double>(std::max(1, right - left)), 0.0, 1.0);
    return ratio * progress_duration_seconds_;
}

double VideoRenderer::current_pts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return displayed_pts_;
}

//draw the pictures and progress bar actually
void VideoRenderer::draw_scene_locked() {
    if (renderer_ == nullptr || texture_ == nullptr) {
        return;
    }
    //copy texture to current renderring target
    //draw picture in texture on window
    if (SDL_RenderCopy(renderer_, texture_, nullptr, nullptr) != 0) {
        throw_sdl_error("SDL_RenderCopy failed");
    }

    if (progress_duration_seconds_ > 0.0 && width_ > 0 && height_ > 0) {
        const int left = kProgressBarMargin;
        const int top = height_ - kProgressBarHeight - kProgressBarMargin;
        const int bar_width = std::max(1, width_ - 2 * kProgressBarMargin);
        //(x,y,width,height)
        SDL_Rect background {left, top, bar_width, kProgressBarHeight};
        SDL_Rect fill = background;
        const double ratio = std::clamp(progress_position_seconds_ / progress_duration_seconds_, 0.0, 1.0);
        fill.w = std::max(1, static_cast<int>(bar_width * ratio));

        //SDL_SetRenderDrawColor = 选画笔颜色
        //SDL_RenderDrawRect = 用画笔画一个空心框
        //SDL_RenderFillRect = 用油漆桶填一个实心块
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 20, 20, 20, 180);//set画笔颜色
        SDL_RenderFillRect(renderer_, &background);
        if (dragging_progress_) {
            SDL_SetRenderDrawColor(renderer_, 255, 180, 60, 230);
        } else {
            SDL_SetRenderDrawColor(renderer_, 80, 200, 120, 230);
        }
        SDL_RenderFillRect(renderer_, &fill); //画一个实心矩形
        SDL_SetRenderDrawColor(renderer_, 240, 240, 240, 255);
        SDL_RenderDrawRect(renderer_, &background); //只画矩形边框轮廓

        const std::string label =
            format_time_label(progress_position_seconds_) + " / " + format_time_label(progress_duration_seconds_);
        const int text_y = top + (kProgressBarHeight - kGlyphHeight * kGlyphScale) / 2;
        SDL_SetRenderDrawColor(renderer_, 250, 250, 250, 255);
        draw_text(renderer_, left + 8, text_y, label);
    }

    //把当前渲染结果提交到窗口显示(双缓冲交换)
    SDL_RenderPresent(renderer_);
}

std::chrono::milliseconds VideoRenderer::compute_delay(double video_pts, double master_pts) {
    double delta = 0.0;
    if (master_pts > 0.0) {
        delta = video_pts - master_pts;
    } else if (has_last_video_pts_) {
        //退化成按视频自身帧间隔播放 without master clock
        delta = video_pts - last_video_pts_;
    }

    last_video_pts_ = video_pts;
    has_last_video_pts_ = true;
    delta = std::clamp(delta, 0.0, 0.1);
    return std::chrono::milliseconds(static_cast<int>(delta * 1000.0));
}

}  // namespace sim_player
