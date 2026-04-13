#pragma once

#include <memory>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace sim_player {

    //把 FFmpeg 返回的负数错误码转成可读字符串
inline std::string ffmpeg_error_to_string(int errnum) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errnum, buffer, sizeof(buffer));
    return std::string(buffer);
}

//throw an exception if code < 0
inline void throw_on_ffmpeg_error(int code, const std::string& what) {
    if (code < 0) {
        throw std::runtime_error(what + ": " + ffmpeg_error_to_string(code));
    }
}

//free AVPacket automatically
struct PacketDeleter {
    void operator()(AVPacket* packet) const {
        av_packet_free(&packet);
    }
};

//free AVFrame automatically
//many objects of FFmpeg can not use function 'delete',and must use its own *_free function,otherwise,there will be a crash or leak
struct FrameDeleter {
    void operator()(AVFrame* frame) const {
        av_frame_free(&frame);
    }
};

using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

inline PacketPtr make_packet() {
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        throw std::bad_alloc();
    }
    return PacketPtr(packet);
}

inline FramePtr make_frame() {
    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        throw std::bad_alloc();
    }
    return FramePtr(frame);
}

}  // namespace sim_player
