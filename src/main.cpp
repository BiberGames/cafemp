#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <vpad/input.h>
#include <coreinit/debug.h>
#include <gx2/context.h>

#include "config.hpp"
#include "menu.hpp"
#include "video_player.hpp"

enum AppState {
    STATE_MENU,
    STATE_PLAYING
};

AppState app_state = STATE_MENU;

std::string format_time(int seconds) {
    int mins = seconds / 60;
    int secs = seconds % 60;
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%02d:%02d", mins, secs);
    return std::string(buffer);
}

int init_sdl() {
    printf("Starting SDL...\n");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) < 0) {
        printf("Failed to init SDL: %s\n", SDL_GetError());
        return -1;
    }

    wanted_spec = create_audio_spec();

    if (SDL_OpenAudio(&wanted_spec, NULL) < 0) {
        printf("SDL_OpenAudio error: %s\n", SDL_GetError());
        return -1;
    }

    audio_mutex = SDL_CreateMutex();
    avformat_network_init();

    TTF_Init();
    font = TTF_OpenFont(fontpath, 24);

    return 0;
}

int init_video_player(const char* filepath) {
    printf("Starting Video Player...\n");

    if (avformat_open_input(&fmt_ctx, filepath, NULL, NULL) != 0) {
        printf("Could not open file: %s\n", filepath);
        return -1;
    }

    avformat_find_stream_info(fmt_ctx, NULL);

    audio_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    video_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);

    if (audio_stream_index < 0 || video_stream_index < 0) {
        printf("Could not find both audio and video streams.\n");
        return -1;
    }

    audio_codec_ctx = create_codec_context(fmt_ctx, audio_stream_index);
    video_codec_ctx = create_codec_context(fmt_ctx, video_stream_index);

    if (!audio_codec_ctx || !video_codec_ctx)
        return -1;

    window = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, screen_width, screen_height, 0);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(renderer, screen_width, screen_height);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, video_codec_ctx->width, video_codec_ctx->height);

    framerate = fmt_ctx->streams[video_stream_index]->r_frame_rate;
    double frameRate = av_q2d(framerate);
    printf("FPS: %f\n", frameRate);

    // Audio resampler
    swr_ctx = swr_alloc_set_opts(NULL,
        AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, audio_sample_rate,
        audio_codec_ctx->channel_layout, audio_codec_ctx->sample_fmt, audio_codec_ctx->sample_rate,
        0, NULL);
    swr_init(swr_ctx);

    pkt = av_packet_alloc();
    frame = av_frame_alloc();

    SDL_PauseAudio(0);

    return 0;
}

int video_player_cleanup() {
    SDL_CloseAudio();

    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr_ctx);
    avcodec_free_context(&audio_codec_ctx);
    avcodec_free_context(&video_codec_ctx);
    avformat_close_input(&fmt_ctx);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_CloseFont(font);
    TTF_Quit();
    SDL_Quit();

    return 0;
}

void handle_vpad_input() {
    VPADStatus buf;
    int key_press = VPADRead(VPAD_CHAN_0, &buf, 1, nullptr);
    if (key_press == 1) {
        if (buf.trigger == DRC_BUTTON_A) {
            playing_video = !playing_video;
            SDL_PauseAudio(playing_video ? 0 : 1);
        }
        else if(buf.trigger == DRC_BUTTON_START) {
            
        }
        else if(buf.trigger) {
            printf("pressed  = %08x\n", buf.trigger);
        }
    }
}

void render_timer(int current_pts_seconds, int duration_seconds) {
    SDL_RenderCopy(renderer, texture, NULL, NULL);

    std::string time_str = format_time(current_pts_seconds) + " / " + format_time(duration_seconds);
    SDL_Color white = {255, 255, 255};

    SDL_Surface* text_surface = TTF_RenderText_Blended(font, time_str.c_str(), white);
    SDL_Texture* text_texture = SDL_CreateTextureFromSurface(renderer, text_surface);

    int text_w, text_h;
    SDL_QueryTexture(text_texture, NULL, NULL, &text_w, &text_h);
    SDL_Rect dst_rect = {10, screen_height - text_h - 10, text_w, text_h};

    SDL_RenderCopy(renderer, text_texture, NULL, &dst_rect);

    SDL_FreeSurface(text_surface);
    SDL_DestroyTexture(text_texture);
}

int main(int argc, char **argv) {
    WHBProcInit();

    if (init_sdl() != 0) return -1;
    if (init_video_player(filename) != 0) return -1;

    uint64_t ticks_per_frame = OSMillisecondsToTicks(1000) / av_q2d(framerate);
    uint64_t last_frame_ticks = OSGetSystemTime();

    while (WHBProcIsRunning()) {
        handle_vpad_input();

        if (!playing_video) {
            SDL_RenderPresent(renderer);
            SDL_Delay(50);
            continue;
        }

        if((av_read_frame(fmt_ctx, pkt)) >= 0) {
            int64_t duration_seconds = fmt_ctx->duration / AV_TIME_BASE;

            if (pkt->stream_index == audio_stream_index) {
                if (avcodec_send_packet(audio_codec_ctx, pkt) == 0) {
                    while (avcodec_receive_frame(audio_codec_ctx, frame) == 0) {
                        play_audio_frame(frame, swr_ctx, 2);
                    }
                }
            }
        
            static int64_t current_pts_seconds = 0;
            if (pkt->stream_index == video_stream_index) {
                if (avcodec_send_packet(video_codec_ctx, pkt) == 0) {
                    while (avcodec_receive_frame(video_codec_ctx, frame) == 0) {
                        if (frame->format == AV_PIX_FMT_YUV420P) {
                            AVRational time_base = fmt_ctx->streams[video_stream_index]->time_base;
                            current_pts_seconds = frame->pts * av_q2d(time_base);

                            SDL_UpdateYUVTexture(texture, NULL,
                                frame->data[0], frame->linesize[0],
                                frame->data[1], frame->linesize[1],
                                frame->data[2], frame->linesize[2]);
        
                            uint64_t now_ticks = OSGetSystemTime();
                            uint64_t elapsed_ticks = now_ticks - last_frame_ticks;
        
                            if (elapsed_ticks < ticks_per_frame) {
                                OSSleepTicks(ticks_per_frame - elapsed_ticks);
                            }
        
                            last_frame_ticks = OSGetSystemTime();
        
                            render_timer(current_pts_seconds, duration_seconds);
                            SDL_RenderPresent(renderer);
                        }
                    }
                }
            }
        
            av_packet_unref(pkt);
        }
    }

    avcodec_send_packet(audio_codec_ctx, NULL);
    while (avcodec_receive_frame(audio_codec_ctx, frame) == 0) {
        play_audio_frame(frame, swr_ctx, 2);
    }
    while (ring_buffer_fill > 0) {
        SDL_Delay(100);
    }

    video_player_cleanup();

    WHBProcShutdown();
    return 0;
}
