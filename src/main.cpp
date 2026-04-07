#include "player/Player.h"
#include "player/FfmpegUtils.h"

#include <SDL2/SDL.h>

#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: sim_player <input-file>\n";
        return 1;
    }

    av_log_set_level(AV_LOG_INFO); //设置 FFmpeg 的日志输出等级为 INFO

    try {
        sim_player::Player player;
        player.open(argv[1]); //打开视频文件
        player.play();

        bool quit = false;
        while (!quit) {
            SDL_Event event {};
            while (SDL_PollEvent(&event) == 1) {
                if (event.type == SDL_QUIT) {
                    quit = true;
                    break;
                }
            }

            if (player.has_error()) {
                throw std::runtime_error(player.last_error());
            }
            player.pump_video(); //渲染并显示视频帧
            if (player.is_finished()) {
                quit = true;
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        player.stop();
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }

    return 0;
}
