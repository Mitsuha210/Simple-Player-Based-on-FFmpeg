# Sim Player

这是一个基于 FFmpeg 和 C++17 的简易播放器骨架，目标是把播放器最核心的职责先拆清楚，再逐步补齐渲染和音频输出。

## 目录结构

```text
.
├─ CMakeLists.txt
├─ README.md
├─ include/player
│  ├─ BlockingQueue.h
│  ├─ Clock.h
│  ├─ Decoder.h
│  ├─ Demuxer.h
│  ├─ FfmpegUtils.h
│  ├─ Player.h
│  └─ Renderer.h
└─ src
   ├─ Decoder.cpp
   ├─ Demuxer.cpp
   ├─ Player.cpp
   ├─ Renderer.cpp
   └─ main.cpp
```

## 播放器主链路

1. `Demuxer` 打开媒体文件，读取流信息，并通过 `av_read_frame` 将压缩包分发到音频/视频包队列。
2. `Decoder` 分别运行在音频线程和视频线程中，将压缩包送入 `AVCodecContext`，再持续调用 `avcodec_receive_frame` 取出解码后的帧。
3. `AudioRenderer` 消费音频帧，完成重采样和送声卡，随后更新音频主时钟。
4. `VideoRenderer` 消费视频帧，根据视频 PTS 和音频时钟计算延时，控制显示节奏。
5. `Player` 负责生命周期、线程启动/停止、异常传播和同步策略。

## 推荐的后续扩展顺序

1. 接入 SDL2，先实现音频回调和 YUV 纹理显示。
2. 为 `AudioRenderer` 添加 `SwrContext`，统一输出到 `s16`/`fltp` 目标格式。
3. 为 `VideoRenderer` 添加 `SwsContext`，统一转换成 `YUV420P` 或 SDL 纹理格式。
4. 加入暂停、步进、拖动和 EOF flush。
5. 引入 `master clock` 策略切换，例如音频主时钟、视频主时钟、外部时钟。

## 构建假设

- 已安装 FFmpeg 开发库：`avformat`、`avcodec`、`avutil`、`swresample`、`swscale`
- 可选安装 SDL2，用于真正的窗口和音频设备输出
- Windows 上如果没有 `pkg-config`，建议改为手动指定 FFmpeg include/lib 路径，或换成 vcpkg + CMake toolchain

## 关键设计点

- 队列使用阻塞模型，避免空转轮询。
- 音频时钟优先，因为声卡回放更稳定，视频应向音频对齐。
- 所有线程停止时都通过 `abort()` 广播，避免线程卡死在队列等待。
- `AVPacket` 和 `AVFrame` 使用 RAII 封装，减少释放遗漏。
