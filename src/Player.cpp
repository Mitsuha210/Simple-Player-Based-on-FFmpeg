#include "player/Player.h"

#include <algorithm>
#include <stdexcept>

namespace sim_player {

Player::Player() : demuxer_(audio_packets_, video_packets_) {}

Player::~Player() {
    stop();
}

void Player::open(const std::string& input_path) {
    //clear status
    error_.store(false);
    stopping_.store(false);
    paused_.store(false);
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_.clear();
    }

    demuxer_.open(input_path);
    demuxer_.set_error_callback([this](const std::string& message) { report_error(message); });

    //create stream decoder
    const MediaStreams& streams = demuxer_.streams();
    if (streams.audio != nullptr) {
        audio_decoder_ = std::make_unique<Decoder>(
            Decoder::Kind::Audio, streams.audio, audio_packets_, audio_clock_, &audio_renderer_, nullptr);
        audio_decoder_->set_error_callback([this](const std::string& message) { report_error(message); });
    }

    if (streams.video != nullptr) {
        video_decoder_ = std::make_unique<Decoder>(
            Decoder::Kind::Video, streams.video, video_packets_, audio_clock_, nullptr, &video_renderer_, &video_frames_);
        video_decoder_->set_error_callback([this](const std::string& message) { report_error(message); });
    }

    opened_.store(true);
    //initialize progress bar
    set_progress_preview(0.0, false);
}

void Player::play() {
    if (!opened_.load()) {
        throw std::runtime_error("player not opened");
    }

    //?????? seek ?????????????
    seek_internal(current_position_seconds(), true);
}

void Player::pause() { //stop the work thread
    if (!opened_.load()) {
        return;
    }
    if (paused_.load()) {
        refresh_video();
        return;
    }

    //retrieve current position
    const double paused_position = current_position_seconds();
    paused_.store(true);
    playing_.store(false);
    audio_renderer_.set_paused(true);
    set_progress_preview(paused_position, false);
    audio_clock_.set(paused_position);
    stop_workers();
    refresh_video();
}

void Player::resume() {
    if (!opened_.load()) {
        return;
    }
    seek_internal(current_position_seconds(), true);
}

void Player::toggle_pause() {
    if (paused_.load()) {
        resume();
    } else {
        pause();
    }
}

void Player::stop() {
    if (stopping_.exchange(true)) {
        return;
    }

    playing_.store(false);
    paused_.store(false);
    stop_workers();
    audio_renderer_.close();
    video_renderer_.close();
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
    return audio_done && video_done && video_frames_.empty();
}

std::string Player::last_error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

void Player::pump_video() {
    if (paused_.load()) {
        return;
    }

    //try to retrieve only one frame each time 
    auto item = video_frames_.try_pop();
    if (item.has_value()) {
        video_renderer_.render(std::move(item->frame), audio_clock_, item->time_base);
    }
    //?????
    set_progress_preview(current_position_seconds(), false);
}

void Player::refresh_video() {
    //redraw current texture
    video_renderer_.present();
}

//jump to position_seconds
void Player::seek_to(double position_seconds) {
    seek_internal(position_seconds, !paused_.load());
}

//jump to current_sencode + dalta_seconds(relative_seconds)
void Player::seek_relative(double delta_seconds) {
    seek_to(current_position_seconds() + delta_seconds);
}

double Player::duration_seconds() const {
    return demuxer_.duration_seconds();
}

double Player::current_position_seconds() const {
    const double audio_position = audio_clock_.get();
    const double video_position = video_renderer_.current_pts();
    return std::max(audio_position, video_position);
}

void Player::set_progress_preview(double position_seconds, bool dragging) {
    video_renderer_.set_progress(position_seconds, duration_seconds(), dragging);
}

//Delegate the mouse event logic to the renderer for processing
bool Player::is_progress_bar_hit(int x, int y) const {
    return video_renderer_.is_progress_bar_hit(x, y);
}

double Player::progress_position_from_x(int x) const {
    return video_renderer_.progress_from_x(x);
}

void Player::report_error(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = message;
    }
    error_.store(true);
    audio_packets_.abort();
    video_packets_.abort();
    video_frames_.abort();
}

void Player::seek_internal(double position_seconds, bool resume_after_seek) {
    if (!opened_.load()) {
        return;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    const double duration = duration_seconds();
    const double clamped = duration > 0.0 ? std::clamp(position_seconds, 0.0, duration) : std::max(0.0, position_seconds);

    //stop threads before updating status
    stop_workers();
    audio_packets_.reset();
    video_packets_.reset();
    video_frames_.reset();

    //not only clear waiting queue,but also clear out the inside cache of decoder
    if (audio_decoder_ != nullptr) {
        audio_decoder_->flush();
    }
    if (video_decoder_ != nullptr) {
        video_decoder_->flush();
    }

    //change reading position
    demuxer_.seek(clamped);

    //update clock and states of renderer
    audio_clock_.set(clamped);
    audio_renderer_.set_paused(!resume_after_seek);
    audio_renderer_.flush();
    set_progress_preview(clamped, false);
    video_renderer_.flush();

    paused_.store(!resume_after_seek);
    playing_.store(resume_after_seek);
    if (resume_after_seek) {
        restart_workers();
    }
}

void Player::restart_workers() {
    audio_packets_.reset();
    video_packets_.reset();
    video_frames_.reset();

    if (audio_decoder_ != nullptr) {
        audio_decoder_->start();
    }
    if (video_decoder_ != nullptr) {
        video_decoder_->start();
    }
    demuxer_.start();
}

void Player::stop_workers() {
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

    audio_renderer_.flush();
    video_renderer_.flush();
}

}  // namespace sim_player
