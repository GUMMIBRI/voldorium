#pragma once
#include <stdbool.h>
#include <stdint.h>
enum {
    SC1_UP=1u<<0, SC1_DOWN=1u<<1, SC1_LEFT=1u<<2, SC1_RIGHT=1u<<3,
    SC1_ACCEPT=1u<<4, SC1_BACK=1u<<5, SC1_START=1u<<6,
    SC1_TAB_LEFT=1u<<7, SC1_TAB_RIGHT=1u<<8, SC1_L3=1u<<9, SC1_R3=1u<<10
};
enum Sc1MenuAction {
    SC1_MENU_NONE, SC1_MENU_OPEN, SC1_MENU_CLOSE, SC1_MENU_WIDE,
    SC1_MENU_FILL, SC1_MENU_MOVIE, SC1_MENU_SCALE, SC1_MENU_FSR, SC1_MENU_RETRY_SAVE, SC1_MENU_CONTINUE_UNSAVED, SC1_MENU_VOLUME_DOWN, SC1_MENU_VOLUME_UP, SC1_MENU_EXIT
};
typedef struct {
    bool open, confirm_exit, save_error, pending_exit;
    int page, row[3], confirm_yes;
    uint32_t previous;
} Sc1MenuState;
enum Sc1MenuAction sc1_menu_step(Sc1MenuState *state, uint32_t input);
