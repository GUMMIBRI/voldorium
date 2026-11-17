#include "menu-state.h"

enum Sc1MenuAction sc1_menu_step(Sc1MenuState *s, uint32_t input)
{
    const uint32_t pressed = input & ~s->previous, chord = SC1_L3 | SC1_R3;
    const bool toggle = (input & chord) == chord && (s->previous & chord) != chord;
    s->previous = input;
    if (!s->open) {
        if (!toggle) return SC1_MENU_NONE;
        s->open = true;
        s->confirm_exit = false;
        return SC1_MENU_OPEN;
    }
    if (s->save_error) {
        if (pressed & SC1_ACCEPT) return SC1_MENU_RETRY_SAVE;
        if (pressed & SC1_BACK) return SC1_MENU_CONTINUE_UNSAVED;
        return SC1_MENU_NONE;
    }
    if (toggle || (pressed & SC1_START)) {
        s->open = false;
        s->confirm_exit = false;
        return SC1_MENU_CLOSE;
    }
    if (s->confirm_exit) {
        if (pressed & SC1_BACK) { s->confirm_exit = false; return SC1_MENU_NONE; }
        if (pressed & (SC1_UP | SC1_DOWN | SC1_LEFT | SC1_RIGHT)) s->confirm_yes ^= 1;
        if (pressed & SC1_ACCEPT) {
            s->confirm_exit = false;
            if (s->confirm_yes) { s->open = false; return SC1_MENU_EXIT; }
        }
        return SC1_MENU_NONE;
    }
    if (pressed & SC1_BACK) { s->open = false; return SC1_MENU_CLOSE; }
    if (pressed & (SC1_TAB_LEFT | SC1_TAB_RIGHT)) { s->page = (s->page + ((pressed & SC1_TAB_LEFT) ? 2 : 1)) % 3; return SC1_MENU_NONE; }
    const int settings = s->page == 0 ? 3 : s->page == 1 ? 1 : 2, rows = settings + 2;
    if (pressed & SC1_UP) s->row[s->page] = (s->row[s->page] + rows - 1) % rows;
    else if (pressed & SC1_DOWN) s->row[s->page] = (s->row[s->page] + 1) % rows;
    const int row = s->row[s->page];
    if (row >= settings) {
        if (!(pressed & SC1_ACCEPT)) return SC1_MENU_NONE;
        if (row == settings) { s->open = false; return SC1_MENU_CLOSE; }
        s->confirm_exit = true;
        s->confirm_yes = 0;
        return SC1_MENU_NONE;
    }
    if (!(pressed & (SC1_LEFT | SC1_RIGHT | SC1_ACCEPT))) return SC1_MENU_NONE;
    if (!s->page) return row == 0 ? SC1_MENU_WIDE : row == 1 ? SC1_MENU_FILL : SC1_MENU_MOVIE;
    if (s->page == 2) return row == 0 ? SC1_MENU_SCALE : SC1_MENU_FSR;
    return pressed & SC1_LEFT ? SC1_MENU_VOLUME_DOWN : SC1_MENU_VOLUME_UP;
}
