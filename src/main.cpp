#include <SDL2/SDL.h>
#include <whb/proc.h>
#include "config.hpp"
#include "menu.hpp"

AppState main_app_state = STATE_MENU;
SDL_Window* main_window;
SDL_Renderer* main_renderer;
SDL_Texture* main_texture;

int init_sdl() {
    printf("Starting SDL...\n");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        printf("Failed to init SDL: %s\n", SDL_GetError());
        return -1;
    }

    main_window = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_WIDTH, SCREEN_HEIGHT, 0);
    main_renderer = SDL_CreateRenderer(main_window, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(main_renderer, SCREEN_WIDTH, SCREEN_HEIGHT);

    return 0;
}

int main(int argc, char **argv) {
    WHBProcInit();

    if (init_sdl() != 0) return -1;

    ui_init(main_window, main_renderer, main_texture, &main_app_state);

    while (WHBProcIsRunning()) {
        ui_render();
        SDL_RenderPresent(main_renderer);
    }
    ui_shutodwn();

    SDL_DestroyTexture(main_texture);
    SDL_DestroyRenderer(main_renderer);
    SDL_DestroyWindow(main_window);
    SDL_Quit();

    WHBProcShutdown();
    return 0;
}
