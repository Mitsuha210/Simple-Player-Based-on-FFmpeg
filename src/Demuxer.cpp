#include "player/Demuxer.h"

namespace sim_player {

Demuxer::Demuxer(BlockingQueue<PacketPtr>& audio_packets, BlockingQueue<PacketPtr>& video_packets)
    : audio_packets_(audio_packets), video_packets_(video_packets) {}

Demuxer::~Demuxer() {
    stop();
    if (format_context_ != nullptr) {
        avformat_close_input(&format_context_);
    }
}

void Demuxer::open(const std::string& input_path) {
    finished_.store(false);
    AVFormatContext* context = nullptr;
    throw_on_ffmpeg_error(avformat_open_input(&context, input_path.c_str(), nullptr, nullptr),
                          "avformat_open_input failed");
    throw_on_ffmpeg_error(avformat_find_stream_info(context, nullptr),
                          "avformat_find_stream_info failed");

    format_context_ = context;

    //自动搜寻音视频流编号
    int audio_index = av_find_best_stream(context, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    int video_index = av_find_best_stream(context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    if (audio_index >= 0) {
        streams_.audio = context->streams[audio_index];
    }
    if (video_index >= 0) {
        streams_.video = context->streams[video_index];
    }
    if (streams_.audio == nullptr && streams_.video == nullptr) {
        throw std::runtime_error("no playable audio/video stream found");
    }
}

void Demuxer::start() {
    finished_.store(false);
    running_.store(true);
    //创造线程worker_,以this为参数执行run
    worker_ = std::thread(&Demuxer::run, this);
}

void Demuxer::stop() {
    running_.store(false);
    audio_packets_.abort();//中断队列
    video_packets_.abort();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

void Demuxer::run() {
    try {
        while (running_.load()) {
            PacketPtr packet = make_packet(); // 创建一个pkt指针
            int ret = av_read_frame(format_context_, packet.get());
            //读到文件结尾时返回AVERROR_EOF(End Of File)
            if (ret == AVERROR_EOF) { //读到文件末尾
                // A null packet tells each decoder to flush delayed frames before exiting.
                push_eof_packets();
                break;
            }
            throw_on_ffmpeg_error(ret, "av_read_frame failed");

            if (streams_.audio != nullptr && packet->stream_index == streams_.audio->index) {
                if (!audio_packets_.push(std::move(packet))) {
                    break;
                }
            } else if (streams_.video != nullptr && packet->stream_index == streams_.video->index) {
                if (!video_packets_.push(std::move(packet))) {
                    break;
                }
            }
        }
        finished_.store(true);
    } catch (const std::exception& ex) {
        finished_.store(true);
        running_.store(false);
        audio_packets_.abort();
        video_packets_.abort();
        if (error_callback_) {
            error_callback_(ex.what());
        }
    }
}

void Demuxer::push_eof_packets() {
    if (streams_.audio != nullptr) {
        audio_packets_.push(PacketPtr {});
        //PacketPtr {}表示“值初始化”，得到一个空指针对象(这里==一个特殊记号)
        //告诉线程pkt已经读完了
    }
    if (streams_.video != nullptr) {
        video_packets_.push(PacketPtr {});
    }
}

void Demuxer::set_error_callback(std::function<void(const std::string&)> callback) {
    error_callback_ = std::move(callback);
}

}  // namespace sim_player
