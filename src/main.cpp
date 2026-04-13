#include "player/FfmpegUtils.h"
#include "player/Player.h"

#include <SDL2/SDL.h>

#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: sim_player <input-file>\n";
        return 1;
    }

    //set the log level of FFmpeg
    av_log_set_level(AV_LOG_INFO);

    try {
        sim_player::Player player; //main control object of player
        player.open(argv[1]);//open files and create resource needed
        player.play();//start threads

        bool quit = false; // pause
        bool dragging = false; // drag
        bool resume_after_drag = false; //resume
        double drag_target_seconds = 0.0;

        while (!quit) {
            SDL_Event event {};
            //retrieve events from queue continuously
            while (SDL_PollEvent(&event) == 1) {
                if (event.type == SDL_QUIT) {
                    quit = true;
                    break;
                }

                //press the keyboard
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                    switch (event.key.keysym.sym) {
                    case SDLK_SPACE: //pause or resume
                        player.toggle_pause();
                        break;
                    case SDLK_RIGHT:
                        player.seek_relative(10.0);
                        break;
                    case SDLK_LEFT:
                        player.seek_relative(-10.0);
                        break;
                    default:
                        break;
                    }
                }

                //click progress bar
                if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT &&
                    player.is_progress_bar_hit(event.button.x, event.button.y)) {
                    dragging = true;
                    resume_after_drag = player.is_playing() && !player.is_paused();
                    drag_target_seconds = player.progress_position_from_x(event.button.x);
                    //如果不暂停，后台线程还在继续推进，用户拖到哪和实际播放位置会打架
                    player.pause(); //pause the player
                    player.set_progress_preview(drag_target_seconds, true); //update the progress of preview but not refresh video here
                    player.refresh_video();
                }

                if (event.type == SDL_MOUSEMOTION && dragging) {
                    drag_target_seconds = player.progress_position_from_x(event.motion.x);
                    player.set_progress_preview(drag_target_seconds, true);
                    player.refresh_video();
                }

                //seek when mouse release
                if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT && dragging) {
                    dragging = false;
                    player.seek_to(drag_target_seconds);
                    player.set_progress_preview(drag_target_seconds, false);
                    //resume if it was playing before drag
                    if (resume_after_drag) {
                        player.resume();
                    } else {
                        player.pause();
                    }
                    player.refresh_video();
                }
            }

            if (quit) {
                break;
            }

            if (player.has_error()) {
                throw std::runtime_error(player.last_error());
            }

            if (!dragging) {
                player.pump_video();
                player.refresh_video();
            }

            //there may be frame of rest in queue after EOF
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
