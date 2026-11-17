#pragma once
#include "menu-state.h"
#include <SDL3/SDL.h>

Sc1MenuState *sc1_menu_state(void);
void sc1_menu_init(void);
enum Sc1MenuAction sc1_menu_action(enum Sc1MenuAction action);
bool sc1_menu_widescreen(void);
bool sc1_menu_fill_screen(void);
bool sc1_menu_movie_blur(void);
int sc1_menu_render_scale(void);
bool sc1_menu_fsr_enabled(void);
bool sc1_menu_visible(void);
bool sc1_menu_gpu_create(SDL_GPUDevice *device, SDL_GPUTextureFormat format);
void sc1_menu_gpu_destroy(void);
bool sc1_menu_gpu_upload(SDL_GPUCommandBuffer *commands);
bool sc1_menu_gpu_draw(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, uint32_t width, uint32_t height);
bool sc1_present_settings(void);
void sc1_audio_menu_pause(bool paused);

typedef struct { unsigned width,height; int fsr_status; bool override_scale,override_fsr; } Sc1RenderStatus;
enum { SC1_FSR_OFF, SC1_FSR_PENDING, SC1_FSR_ACTIVE, SC1_FSR_BYPASS, SC1_FSR_FAILED };
Sc1RenderStatus sc1_render_status(void);
