#include "player/Decoder.h"

namespace sim_player {

namespace { //匿名命名空间

AVCodecContext* open_codec_context(AVStream* stream) {
    //find decoder by codec_id
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        throw std::runtime_error("decoder not found");
    }

    //codec_context is equvalent to an instance object of codec
    AVCodecContext* codec_context = avcodec_alloc_context3(codec);
    if (codec_context == nullptr) {
        throw std::bad_alloc();
    }

    //设置参数 by AVStream
    //Connect the "container layer" and the "encoding/decoding layer"
    int ret = avcodec_parameters_to_context(codec_context, stream->codecpar);
    if (ret < 0) {
        avcodec_free_context(&codec_context);
        throw std::runtime_error("avcodec_parameters_to_context failed: " + ffmpeg_error_to_string(ret));
    }

    //打开解码器
    ret = avcodec_open2(codec_context, codec, nullptr);
    if (ret < 0) {
        //free if opening failed
        avcodec_free_context(&codec_context);
        throw std::runtime_error("avcodec_open2 failed: " + ffmpeg_error_to_string(ret));
    }

    return codec_context;
}

}  // namespace

Decoder::Decoder(Kind kind,
                 AVStream* stream,
                 BlockingQueue<PacketPtr>& packet_queue,
                 Clock& master_clock,
                 AudioRenderer* audio_renderer,
                 VideoRenderer* video_renderer,
                 BlockingQueue<VideoFrameItem>* video_frames)
    : kind_(kind),
      stream_(stream),
      packet_queue_(packet_queue),
      master_clock_(master_clock),
      audio_renderer_(audio_renderer),
      video_renderer_(video_renderer),
      video_frames_(video_frames),
      codec_context_(open_codec_context(stream)) {}

Decoder::~Decoder() {
    stop();
    if (codec_context_ != nullptr) {
        avcodec_free_context(&codec_context_);
    }
}

void Decoder::start() {
    finished_.store(false);
    if (kind_ == Kind::Audio && audio_renderer_ != nullptr && !renderer_opened_) {
        audio_renderer_->open(*codec_context_);
        renderer_opened_ = true;
    }
    if (kind_ == Kind::Video && video_renderer_ != nullptr && !renderer_opened_) {
        video_renderer_->open(*codec_context_);
        renderer_opened_ = true;
    }

    running_.store(true);
    worker_ = std::thread(&Decoder::run, this); //启动decode线程
}

void Decoder::stop() {
    running_.store(false);
    packet_queue_.abort();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

//clear out internal states and cache of decoder
void Decoder::flush() {
    if (codec_context_ != nullptr) {
        avcodec_flush_buffers(codec_context_);
    }
}

void Decoder::run() {
    try {
        while (running_.load()) {
            auto packet = packet_queue_.pop();
            if (!packet.has_value()) {
                break;
            }
            if (packet->get() == nullptr) {
                break;
            }
            decode_packet(packet->get());
        }

        if (codec_context_ != nullptr) {
            decode_packet(nullptr);
        }
        finished_.store(true);
    } catch (const std::exception& ex) {
        finished_.store(true);
        running_.store(false);
        if (error_callback_) {
            error_callback_(ex.what());
        }
    }
}

void Decoder::decode_packet(AVPacket* packet) {
    int ret = avcodec_send_packet(codec_context_, packet);
    //EAGAIN indicates that cannot send now
    if (ret == AVERROR(EAGAIN)) {
        return;
    }
    throw_on_ffmpeg_error(ret, "avcodec_send_packet failed");

    //a packet may contains more than 1 frame
    while (running_.load()) {
        FramePtr frame = make_frame();//创建一个AVFrame的unique_ptr
        ret = avcodec_receive_frame(codec_context_, frame.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        throw_on_ffmpeg_error(ret, "avcodec_receive_frame failed");

        //音频直接输出
        if (kind_ == Kind::Audio && audio_renderer_ != nullptr) {
            audio_renderer_->render(std::move(frame), master_clock_, stream_->time_base);
        } 
        //存在帧队列时直接放入队列-->渲染由主线程执行
        else if (kind_ == Kind::Video && video_frames_ != nullptr) {
            if (!video_frames_->push(VideoFrameItem {std::move(frame), stream_->time_base})) {
                break;
            }
        }
        //frame queue不存在时直接放入渲染器进行渲染 
        else if (kind_ == Kind::Video && video_renderer_ != nullptr) {
            video_renderer_->render(std::move(frame), master_clock_, stream_->time_base);
        }
    }
}

void Decoder::set_error_callback(std::function<void(const std::string&)> callback) {
    error_callback_ = std::move(callback);
}

}  // namespace sim_player
