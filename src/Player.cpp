#include "player/Player.h"

namespace sim_player {

Player::Player() : demuxer_(audio_packets_, video_packets_) {}

Player::~Player() {
    stop();
}

void Player::open(const std::string& input_path) {
    //清空错误状态
    error_.store(false); 
    stopping_.store(false);
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_.clear();
    }

    demuxer_.open(input_path);//打开媒体文件
    demuxer_.set_error_callback([this](const std::string& message) { report_error(message); });

    //读取demuxer_找到音视频流
    const MediaStreams& streams = demuxer_.streams();
    if (streams.audio != nullptr) {
        //如果有音频流->创建音频解码器audio_decoder
        audio_decoder_ = std::make_unique<Decoder>(
            Decoder::Kind::Audio, streams.audio, audio_packets_, audio_clock_, &audio_renderer_, nullptr);
        //注册错误回调
        audio_decoder_->set_error_callback([this](const std::string& message) { report_error(message); });
    }

    if (streams.video != nullptr) {
        //如果有视频流->创建视频解码器video_decoder
        video_decoder_ = std::make_unique<Decoder>(
            Decoder::Kind::Video, streams.video, video_packets_, audio_clock_, nullptr, &video_renderer_, &video_frames_);
        video_decoder_->set_error_callback([this](const std::string& message) { report_error(message); });
    }

    opened_.store(true); //成功打开
}

void Player::play() {
    if (!opened_.load()) { //检查是否已成功打开文件
        throw std::runtime_error("player not opened");
    }

    if (audio_decoder_ != nullptr) {
        audio_decoder_->start(); //启动！
    }
    if (video_decoder_ != nullptr) {
        video_decoder_->start();
    }
    //不是先解码再解复用，而是先启动消费者线程再启动生产者线程
    //如果反过来写可能导致堵塞甚至丢帧
    //一旦 demuxer 开始往队列里塞数据，最好已经有消费者在等着处理，避免队列很快积压
    demuxer_.start(); //启动demuxer线程
    playing_.store(true);
}

void Player::stop() {
    //点击关闭关不掉的源头->阻塞中线程没清除导致一直阻塞->关不掉
    if (stopping_.exchange(true)) {
        return;
    }

    // Abort every blocking queue before joining workers, otherwise the video decoder
    // can stay stuck waiting to push frames while the main thread is already exiting.
    audio_packets_.abort();
    video_packets_.abort();
    video_frames_.abort();
    audio_renderer_.request_stop();
    video_renderer_.request_stop();
    demuxer_.stop();

    if (audio_decoder_ != nullptr) {
        audio_decoder_->stop();
    }
    if (video_decoder_ != nullptr) {
        video_decoder_->stop();
    }

    audio_renderer_.close();
    video_renderer_.close();
    playing_.store(false);
    stopping_.store(false);
}

bool Player::is_finished() const {
    if (!opened_.load()) {
        return false;
    }
    if (has_error()) {
        return true;
    }

    if (!demuxer_.finished()) {
        return false;
    }

    const bool audio_done = audio_decoder_ == nullptr || audio_decoder_->finished();
    const bool video_done = video_decoder_ == nullptr || video_decoder_->finished();

    //全部满足才关闭
    return audio_done && video_done && video_frames_.empty();
}

std::string Player::last_error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

void Player::pump_video() {
    auto item = video_frames_.try_pop();//try从frames中取出一帧
    if (!item.has_value()) {
        return;
    }
    //放入renderer中进行渲染显示
    //-->视频显示不再发生在视频解码线程，而是发生在主线程。
    video_renderer_.render(std::move(item->frame), audio_clock_, item->time_base);
}

void Player::report_error(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = message;
    }
    error_.store(true);
    audio_packets_.abort();
    video_packets_.abort();
}

}  // namespace sim_player
