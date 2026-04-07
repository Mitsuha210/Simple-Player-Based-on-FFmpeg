#include "player/Renderer.h"

#include <algorithm>
#include <iostream>
#include <thread>

namespace sim_player {

void AudioRenderer::open(const AVCodecContext& codec_context) {
    std::cout << "[audio] sample_rate=" << codec_context.sample_rate
              << " channels=" << codec_context.ch_layout.nb_channels << '\n';
}

void AudioRenderer::render(FramePtr frame, Clock& audio_clock, AVRational time_base) {
    const int64_t pts = frame->best_effort_timestamp;
    if (pts != AV_NOPTS_VALUE) {
        audio_clock.set(pts * av_q2d(time_base));
    }

    // 这里先保留为占位实现。实际项目里应在这里做 swr_convert 和设备写入。
    std::cout << "[audio] nb_samples=" << frame->nb_samples
              << " pts=" << audio_clock.get() << '\n';
}

void VideoRenderer::open(const AVCodecContext& codec_context) {
    std::cout << "[video] width=" << codec_context.width
              << " height=" << codec_context.height << '\n';
}

void VideoRenderer::render(FramePtr frame, const Clock& master_clock, AVRational time_base) {
    const int64_t pts = frame->best_effort_timestamp;
    const double video_pts = pts == AV_NOPTS_VALUE ? 0.0 : pts * av_q2d(time_base);
    std::this_thread::sleep_for(compute_delay(video_pts, master_clock.get()));

    // 这里先保留为占位实现。实际项目里应在这里做 sws_scale 和纹理/窗口显示。
    std::cout << "[video] pts=" << video_pts
              << " sync_to_audio=" << master_clock.get() << '\n';
}

std::chrono::milliseconds VideoRenderer::compute_delay(double video_pts, double master_pts) {
    double delta = video_pts - master_pts;
    delta = std::clamp(delta, 0.0, 0.1);
    return std::chrono::milliseconds(static_cast<int>(delta * 1000.0));
}

}  // namespace sim_player
