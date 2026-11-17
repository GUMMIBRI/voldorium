#include "m1.h"
#include <SDL3/SDL.h>
#include "menu.h"

static SDL_Gamepad *pads[2];
static u32 buttons[2];
static bool release_guard;
static int stick_x, stick_y;

static void discover_pads(void)
{
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        if ((pads[0] && SDL_GetGamepadID(pads[0]) == ids[i]) ||
            (pads[1] && SDL_GetGamepadID(pads[1]) == ids[i]))
            continue;
        unsigned slot = pads[0] ? 1 : 0;
        if (pads[slot])
            break;
        pads[slot] = SDL_OpenGamepad(ids[i]);
    }
    SDL_free(ids);
}

static u32 read_buttons(SDL_Gamepad *pad)
{
    static const struct {
        SDL_GamepadButton button;
        u32 dreamcast;
    } mapping[] = {
        { SDL_GAMEPAD_BUTTON_SOUTH, 0x0004 },
        { SDL_GAMEPAD_BUTTON_EAST, 0x0002 },
        { SDL_GAMEPAD_BUTTON_WEST, 0x0400 },
        { SDL_GAMEPAD_BUTTON_NORTH, 0x0200 },
        { SDL_GAMEPAD_BUTTON_START, 0x0008 },
        { SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0010 },
        { SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0020 },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0040 },
        { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0080 },
        { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x1000 },
        { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x2000 },
    };
    u32 pressed = 0;
    for (unsigned i = 0; i < sizeof(mapping) / sizeof(mapping[0]); ++i)
        if (SDL_GetGamepadButton(pad, mapping[i].button))
            pressed |= mapping[i].dreamcast;
    return pressed;
}

static void poll_devices(void)
{
    static int initialized;
    static unsigned polls;
    if (!initialized) {
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            fprintf(stderr, "SC1: gamepad init failed: %s\n", SDL_GetError());
            initialized = -1;
            return;
        }
        initialized = 1;
    }
    if (initialized < 0)
        return;

    SDL_UpdateGamepads();
    for (unsigned i = 0; i < 2; ++i) {
        if (pads[i] && !SDL_GamepadConnected(pads[i])) {
            SDL_CloseGamepad(pads[i]);
            pads[i] = NULL;
        }
    }
    if (polls++ % 60 == 0 && (!pads[0] || !pads[1]))
        discover_pads();
}

static int direction(Sint16 value,int previous)
{
    if (value>16000) return 1;
    if (value<-16000) return -1;
    if (value>-8000 && value<8000) return 0;
    return previous;
}

static u32 menu_buttons(SDL_Gamepad *pad)
{
    if (!pad) { stick_x=stick_y=0; return 0; }
    static const struct { SDL_GamepadButton button; u32 bit; } map[]={
        {SDL_GAMEPAD_BUTTON_DPAD_UP,SC1_UP},{SDL_GAMEPAD_BUTTON_DPAD_DOWN,SC1_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT,SC1_LEFT},{SDL_GAMEPAD_BUTTON_DPAD_RIGHT,SC1_RIGHT},
        {SDL_GAMEPAD_BUTTON_SOUTH,SC1_ACCEPT},{SDL_GAMEPAD_BUTTON_EAST,SC1_BACK},
        {SDL_GAMEPAD_BUTTON_START,SC1_START},{SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,SC1_TAB_LEFT},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,SC1_TAB_RIGHT},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK,SC1_L3},{SDL_GAMEPAD_BUTTON_RIGHT_STICK,SC1_R3}};
    u32 result=0;
    for (unsigned i=0;i<sizeof(map)/sizeof(map[0]);++i)
        if (SDL_GetGamepadButton(pad,map[i].button)) result|=map[i].bit;
    stick_x=direction(SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_LEFTX),stick_x);
    stick_y=direction(SDL_GetGamepadAxis(pad,SDL_GAMEPAD_AXIS_LEFTY),stick_y);
    if (stick_x) result|=stick_x>0?SC1_RIGHT:SC1_LEFT;
    if (stick_y) result|=stick_y>0?SC1_DOWN:SC1_UP;
    return result;
}

void video_poll(void)
{
    poll_devices();
    sc1_menu_init();
    Sc1MenuState *menu=sc1_menu_state();
    unsigned controller=pads[0]?0:1;
    for (unsigned i=0;i<2;++i)
        if (pads[i] && SDL_GetGamepadButton(pads[i],SDL_GAMEPAD_BUTTON_LEFT_STICK) &&
                SDL_GetGamepadButton(pads[i],SDL_GAMEPAD_BUTTON_RIGHT_STICK)) controller=i;
    enum Sc1MenuAction action=sc1_menu_step(menu,menu_buttons(pads[controller]));
    action=sc1_menu_action(action);
    if (action==SC1_MENU_OPEN) {
        buttons[0]=buttons[1]=0;
        sc1_audio_menu_pause(true);
        while (menu->open) {
            if (!sc1_present_settings()) {
                fprintf(stderr,"SC1 settings: presentation failed: %s\n",SDL_GetError());
                menu->open=false;
                action=SC1_MENU_CLOSE;
                action=sc1_menu_action(action);
                /* Presentation failure cannot show the retry dialog. */
                menu->open=false;
                break;
            }
            SDL_Delay(1);
            poll_devices();
            if (!pads[controller]) controller=pads[0]?0:1;
            action=sc1_menu_step(menu,menu_buttons(pads[controller]));
            action=sc1_menu_action(action);
        }
        release_guard=true;
        if (action==SC1_MENU_EXIT) m1_stop("quit from settings menu");
        sc1_audio_menu_pause(false);
    }
    bool neutral=true;
    for (unsigned i=0;i<2;++i) {
        const u32 game=pads[i]?read_buttons(pads[i]):0;
        buttons[i]=release_guard?0:game;
        if (game || (pads[i] && (SDL_GetGamepadButton(pads[i],SDL_GAMEPAD_BUTTON_LEFT_STICK) ||
                SDL_GetGamepadButton(pads[i],SDL_GAMEPAD_BUTTON_RIGHT_STICK) ||
                SDL_abs(SDL_GetGamepadAxis(pads[i],SDL_GAMEPAD_AXIS_LEFTX))>8000 ||
                SDL_abs(SDL_GetGamepadAxis(pads[i],SDL_GAMEPAD_AXIS_LEFTY))>8000))) neutral=false;
    }
    if (release_guard && neutral) release_guard=false;
}

u32 video_buttons(void) { return buttons[0]; }
u32 video_buttons_b(void) { return buttons[1]; }
int video_port_b(void) { return pads[1] != NULL; }
