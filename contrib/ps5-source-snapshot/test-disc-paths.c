#include <assert.h>
#include "sc1_disc_paths.h"
struct sample { const char *found; const char *denied; };
static int mock(const char *p, void *v) {
    struct sample *s=v;
    if (s->found && !strcmp(p,s->found)) return 0;
    if (s->denied && !strcmp(p,s->denied)) return EACCES;
    return ENOENT;
}
int main(void) {
    char chosen[256], err[512];
    const char *paths[]={"/app0/Soulcalibur (USA).gdi", "/data/PPSA99841/Soulcalibur (USA).gdi", "/mnt/usb7/homebrew/PPSA99841/Soulcalibur (USA).gdi"};
    for (int i=0;i<3;i++) { struct sample s={paths[i],0}; assert(sc1_choose_disc(mock,&s,chosen,sizeof chosen,err,sizeof err)); assert(!strcmp(chosen,paths[i])); }
    struct sample missing={0,0}; assert(!sc1_choose_disc(mock,&missing,chosen,sizeof chosen,err,sizeof err)); assert(!err[0]);
    struct sample denied={0,paths[1]}; assert(!sc1_choose_disc(mock,&denied,chosen,sizeof chosen,err,sizeof err)); assert(strstr(err,paths[1]));
    puts("disc path resolver tests passed");
}
