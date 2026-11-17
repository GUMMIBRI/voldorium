/* Native controller menu. Original SC1 integration, following PE's navigation
   conventions. Font: IBM Plex Sans, SIL OFL; see fonts/OFL-IBMPlex.txt. */
#include "menu.h"
#include "font-atlas.h"
#include "menu-shaders.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *config_get(const char *, const char *);
void config_set(const char *, const char *, const char *);
int config_save_checked(void);
int audio_volume(void);
void sc1_audio_set_volume(int value);

static Sc1MenuState state;
static bool loaded, wide, fill, dirty, save_failed, movie_blur=true, fsr_setting;
static int render_scale=1, start_scale=1;
static bool start_fsr;
Sc1MenuState *sc1_menu_state(void) { return &state; }
bool sc1_menu_visible(void) { return state.open; }
bool sc1_menu_widescreen(void) { return wide; }
bool sc1_menu_fill_screen(void) { return fill; }
bool sc1_menu_movie_blur(void) { return movie_blur; }
int sc1_menu_render_scale(void) { return render_scale; }
bool sc1_menu_fsr_enabled(void) { return fsr_setting; }

void sc1_menu_init(void)
{
    if (loaded) return;
    loaded = true;
    const char *aspect = config_get("video", "ps5_aspect");
    const char *fullscreen = config_get("video", "ps5_fill");
    wide = aspect && !strcmp(aspect, "wide");
    fill = fullscreen && atoi(fullscreen) != 0;
    const char *movie=config_get("video","ps5_movie_framing");
    movie_blur=!movie || strcmp(movie,"fill");
    const char *scale=config_get("video","ps5_scale"), *upscale=config_get("video","ps5_fsr");
    render_scale=scale && !strcmp(scale,"2") ? 2 : 1;
    fsr_setting=upscale && !strcmp(upscale,"1");
    start_scale=render_scale; start_fsr=fsr_setting;
    fprintf(stderr, "SC1 settings: L3+R3 menu, framing=%s fill=%d\n", wide ? "16:9" : "4:3", fill);
}

enum Sc1MenuAction sc1_menu_action(enum Sc1MenuAction action)
{
    if (action == SC1_MENU_CONTINUE_UNSAVED) {
        enum Sc1MenuAction outcome=state.pending_exit ? SC1_MENU_EXIT : SC1_MENU_CLOSE;
        state.save_error=false;state.pending_exit=false;state.open=false;return outcome;
    }
    if (action == SC1_MENU_WIDE) { wide = !wide; dirty = true; }
    else if (action == SC1_MENU_FILL) { fill = !fill; dirty = true; }
    else if (action == SC1_MENU_MOVIE) { movie_blur = !movie_blur; dirty = true; }
    else if (action == SC1_MENU_SCALE) { render_scale = render_scale==1 ? 2 : 1; dirty = true; }
    else if (action == SC1_MENU_FSR) { fsr_setting = !fsr_setting; dirty = true; }
    else if (action == SC1_MENU_VOLUME_UP || action == SC1_MENU_VOLUME_DOWN) {
        sc1_audio_set_volume(audio_volume() + (action == SC1_MENU_VOLUME_UP ? 10 : -10));
        dirty = true;
    }
    if ((action == SC1_MENU_CLOSE || action == SC1_MENU_EXIT || action == SC1_MENU_RETRY_SAVE) && dirty) {
        bool exiting=action==SC1_MENU_EXIT || (action==SC1_MENU_RETRY_SAVE && state.pending_exit);
        config_set("video", "ps5_aspect", wide ? "wide" : "original");
        config_set("video", "ps5_fill", fill ? "1" : "0");
        config_set("video", "ps5_movie_framing", movie_blur ? "blur" : "fill");
        config_set("video", "ps5_scale", render_scale==2 ? "2" : "1");
        config_set("video", "ps5_fsr", fsr_setting ? "1" : "0");
        save_failed = !config_save_checked();
        dirty = save_failed;
        if (save_failed) { state.open=true;state.save_error=true;state.pending_exit=exiting; }
        else { state.open=false;state.save_error=false;state.pending_exit=false; }
        fprintf(stderr, "SC1 settings: framing=%s fill=%d volume=%d saved=%d\n", wide ? "16:9" : "4:3", fill, audio_volume(),!save_failed);
        if (save_failed) return SC1_MENU_NONE;
        return exiting ? SC1_MENU_EXIT : SC1_MENU_CLOSE;
    }
    return action;
}

typedef struct { float x,y,u,v; uint32_t color; } MenuVertex;
enum { MAX_VERTICES=4096 };
static MenuVertex vertex[MAX_VERTICES];
static uint32_t count;
static bool overflow;
static SDL_GPUDevice *gpu;
static SDL_GPUShader *vs,*fs;
static SDL_GPUGraphicsPipeline *pipeline;
static SDL_GPUTexture *font;
static SDL_GPUSampler *sampler;
static SDL_GPUBuffer *buffer;
static SDL_GPUTransferBuffer *upload;

static void quad(float x,float y,float w,float h,float u0,float v0,float u1,float v1,uint32_t color)
{
    if (count+6>MAX_VERTICES) { overflow=true; return; }
    const MenuVertex corners[4] = {{x,y,u0,v0,color},{x+w,y,u1,v0,color},
        {x,y+h,u0,v1,color},{x+w,y+h,u1,v1,color}};
    const unsigned order[6]={0,1,2,2,1,3};
    for (unsigned i=0;i<6;++i) vertex[count++]=corners[order[i]];
}
static void rect(float x,float y,float w,float h,uint32_t color)
{
    quad(x,y,w,h,0.5f/512,0.5f/256,0.5f/512,0.5f/256,color);
}
static float text_width(const char *text,float size)
{
    float width=0;
    for (;*text;++text) {
        const unsigned c=(unsigned char)*text;
        if (c>=32 && c<128) width+=sc1_glyphs[c-32].advance*size/32;
    }
    return width;
}
static void text(float x,float y,const char *value,float size,uint32_t color)
{
    const float scale=size/32;
    for (;*value;++value) {
        const unsigned c=(unsigned char)*value;
        if (c<32 || c>=128) continue;
        const Sc1Glyph *g=&sc1_glyphs[c-32];
        if (g->x1>g->x0 && g->y1>g->y0)
            quad(x+g->xoff*scale,y+(26+g->yoff)*scale,(g->x1-g->x0)*scale,(g->y1-g->y0)*scale,
                g->x0/512.0f,g->y0/256.0f,g->x1/512.0f,g->y1/256.0f,color);
        x+=g->advance*scale;
    }
}
static void row(int index,const char *label,const char *value,bool selected,bool action)
{
    const float y=266+index*48+(action?14:0);
    if (selected) { rect(248,y-5,784,43,0xff243b57); rect(248,y-5,4,43,0xff459eff); }
    text(270,y,label,26,selected?0xfff0f3fa:0xffb9c1cf);
    if (value) text(1008-text_width(value,25),y,value,25,selected?0xff80baff:0xff9ba6ba);
}
static void compose(void)
{
    count=0; overflow=false;
    rect(0,0,1280,720,0x9e000000);
    rect(220,94,840,532,0xf5131820);
    rect(220,94,840,3,0xff459eff);
    text(252,118,"Soul Calibur",35,0xfff0f3fa);
    text(252,161,"Settings",22,0xff9ba6ba);
    if (save_failed) text(590,164,"Settings not saved.",20,0xffffa080);
    if (state.save_error) {
        text(252,224,"Could not save settings",29,0xffffbf80);
        text(252,292,"Restart settings may be lost if you continue.",23,0xfff0f3fa);
        text(252,347,"X: Retry saving",24,0xffb9c1cf);
        text(252,397,state.pending_exit ? "Circle: Exit without saving" : "Circle: Resume without saving",24,0xffb9c1cf);
        return;
    }
    if (state.confirm_exit) {
        text(252,224,"Exit to the launcher?",29,0xfff0f3fa);
        row(1,"No, keep playing",NULL,!state.confirm_yes,true);
        row(2,"Yes, exit game",NULL,state.confirm_yes,true);
        text(252,526,"Unsaved progress in the current match will be lost.",21,0xff9ba6ba);
        text(252,581,"D-pad: Choose     X: Confirm     Circle: Back",20,0xffb9c1cf);
        return;
    }
    const char *tabs[3]={"GRAPHICS","AUDIO","ENHANCE"};
    for (int i=0;i<3;++i) {
        const float x=252+i*185;
        if (state.page==i) rect(x,207,164,35,0xff243b57);
        text(x+12,209,tabs[i],22,state.page==i?0xff80baff:0xff9ba6ba);
    }
    text(854,210,"L1 / R1",20,0xff9ba6ba);
    const int selected=state.row[state.page];
    if (!state.page) {
        row(0,"Scene framing",wide?"< Widescreen 16:9 >":"< Classic 4:3 >",selected==0,false);
        row(1,"Fill the TV",fill?"< On >":"< Off >",selected==1,false);
        row(2,"Movie framing",movie_blur?"< Original + blur >":"< Fill screen >",selected==2,false);
        row(3,"Resume game",NULL,selected==3,true);
        row(4,"Exit game",NULL,selected==4,true);
    } else if (state.page==2) {
        row(0,"Internal resolution",render_scale==2?"< 2x >":"< 1x >",selected==0,false);
        row(1,"FSR 1 upscaling",fsr_setting?"< On >":"< Off >",selected==1,false);
        row(2,"Resume game",NULL,selected==2,true);
        row(3,"Exit game",NULL,selected==3,true);
    } else {
        char volume[32]; snprintf(volume,sizeof(volume),"< %d%% >",audio_volume());
        row(0,"Volume",volume,selected==0,false);
        row(1,"Resume game",NULL,selected==1,true);
        row(2,"Exit game",NULL,selected==2,true);
    }
    const char *help=state.page==2?"Graphics enhancements take effect after restarting the game.":
        state.page==1?"Adjust sound volume. The game is paused while this menu is open.":
        selected==0?"Choose the camera framing. Widescreen shows a wider 3D view.":
        selected==1?"On stretches the picture to the full display; Off preserves its aspect.":
        selected==2?"Keep movies sharp in 4:3 with blurred borders, or fill the screen.":
        "Circle, Options, or L3+R3 returns to the game.";
    if (state.page==2) {
        Sc1RenderStatus rs=sc1_render_status();char actual[128];
        const char *status=rs.fsr_status==SC1_FSR_ACTIVE?"active":rs.fsr_status==SC1_FSR_BYPASS?"bypassed":rs.fsr_status==SC1_FSR_FAILED?"failed":rs.fsr_status==SC1_FSR_PENDING?"waiting":"off";
        snprintf(actual,sizeof(actual),"Active %ux%u  FSR: %s%s",rs.width,rs.height,status,(rs.override_scale||rs.override_fsr)?" (override)":"");
        text(252,487,actual,19,0xff9ba6ba);
        help=(render_scale!=start_scale || fsr_setting!=start_fsr)?"Changed settings take effect after restart.":"Values above show the active renderer.";
    }
    text(252,532,help,20,0xff9ba6ba);
    text(252,581,"D-pad: Move    Left/Right: Change    X: Select    Circle: Resume",20,0xffb9c1cf);
}

void sc1_menu_gpu_destroy(void)
{
    if (!gpu) return;
    if (pipeline) SDL_ReleaseGPUGraphicsPipeline(gpu,pipeline);
    if (vs) SDL_ReleaseGPUShader(gpu,vs);
    if (fs) SDL_ReleaseGPUShader(gpu,fs);
    if (font) SDL_ReleaseGPUTexture(gpu,font);
    if (sampler) SDL_ReleaseGPUSampler(gpu,sampler);
    if (buffer) SDL_ReleaseGPUBuffer(gpu,buffer);
    if (upload) SDL_ReleaseGPUTransferBuffer(gpu,upload);
    gpu=NULL; pipeline=NULL; vs=fs=NULL; font=NULL; sampler=NULL; buffer=NULL; upload=NULL;
}

bool sc1_menu_gpu_create(SDL_GPUDevice *device,SDL_GPUTextureFormat format)
{
    if (pipeline) return true;
    gpu=device;
    SDL_GPUShaderCreateInfo shader={.code=(const Uint8 *)menu_vertex_spirv,.code_size=sizeof(menu_vertex_spirv),
        .entrypoint="main",.format=SDL_GPU_SHADERFORMAT_SPIRV,.stage=SDL_GPU_SHADERSTAGE_VERTEX,.num_uniform_buffers=1};
    vs=SDL_CreateGPUShader(gpu,&shader);
    shader.code=(const Uint8 *)menu_fragment_spirv; shader.code_size=sizeof(menu_fragment_spirv);
    shader.stage=SDL_GPU_SHADERSTAGE_FRAGMENT; shader.num_uniform_buffers=0; shader.num_samplers=1;
    fs=SDL_CreateGPUShader(gpu,&shader);
    if (!vs || !fs) goto fail;
    const SDL_GPUVertexBufferDescription vb={.slot=0,.pitch=sizeof(MenuVertex),.input_rate=SDL_GPU_VERTEXINPUTRATE_VERTEX};
    const SDL_GPUVertexAttribute va[]={
        {.location=0,.buffer_slot=0,.format=SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,.offset=offsetof(MenuVertex,x)},
        {.location=1,.buffer_slot=0,.format=SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,.offset=offsetof(MenuVertex,u)},
        {.location=2,.buffer_slot=0,.format=SDL_GPU_VERTEXELEMENTFORMAT_UINT,.offset=offsetof(MenuVertex,color)}};
    const SDL_GPUColorTargetDescription color={.format=format,.blend_state={.enable_blend=true,
        .src_color_blendfactor=SDL_GPU_BLENDFACTOR_SRC_ALPHA,.dst_color_blendfactor=SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        .src_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ONE,.dst_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        .color_blend_op=SDL_GPU_BLENDOP_ADD,.alpha_blend_op=SDL_GPU_BLENDOP_ADD}};
    const SDL_GPUGraphicsPipelineCreateInfo pi={.vertex_shader=vs,.fragment_shader=fs,
        .vertex_input_state={.vertex_buffer_descriptions=&vb,.num_vertex_buffers=1,.vertex_attributes=va,.num_vertex_attributes=3},
        .primitive_type=SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state={.fill_mode=SDL_GPU_FILLMODE_FILL,.cull_mode=SDL_GPU_CULLMODE_NONE},
        .multisample_state={.sample_count=SDL_GPU_SAMPLECOUNT_1},
        .target_info={.color_target_descriptions=&color,.num_color_targets=1}};
    pipeline=SDL_CreateGPUGraphicsPipeline(gpu,&pi);
    const SDL_GPUTextureCreateInfo ti={.type=SDL_GPU_TEXTURETYPE_2D,.format=SDL_GPU_TEXTUREFORMAT_R8_UNORM,
        .usage=SDL_GPU_TEXTUREUSAGE_SAMPLER,.width=512,.height=256,.layer_count_or_depth=1,.num_levels=1,.sample_count=SDL_GPU_SAMPLECOUNT_1};
    font=SDL_CreateGPUTexture(gpu,&ti);
    const SDL_GPUSamplerCreateInfo si={.min_filter=SDL_GPU_FILTER_LINEAR,.mag_filter=SDL_GPU_FILTER_LINEAR,
        .mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,.address_mode_u=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_v=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE};
    sampler=SDL_CreateGPUSampler(gpu,&si);
    const SDL_GPUBufferCreateInfo bi={.usage=SDL_GPU_BUFFERUSAGE_VERTEX,.size=sizeof(vertex)};
    buffer=SDL_CreateGPUBuffer(gpu,&bi);
    const SDL_GPUTransferBufferCreateInfo ui={.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,.size=sizeof(sc1_font_atlas)};
    upload=SDL_CreateGPUTransferBuffer(gpu,&ui);
    if (!pipeline || !font || !sampler || !buffer || !upload) goto fail;
    void *mapped=SDL_MapGPUTransferBuffer(gpu,upload,true);
    if (!mapped) goto fail;
    memcpy(mapped,sc1_font_atlas,sizeof(sc1_font_atlas));
    SDL_UnmapGPUTransferBuffer(gpu,upload);
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    if (!cmd) goto fail;
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);
    if (!copy) { SDL_CancelGPUCommandBuffer(cmd); goto fail; }
    const SDL_GPUTextureTransferInfo from={.transfer_buffer=upload,.pixels_per_row=512,.rows_per_layer=256};
    const SDL_GPUTextureRegion to={.texture=font,.w=512,.h=256,.d=1};
    SDL_UploadToGPUTexture(copy,&from,&to,false);
    SDL_EndGPUCopyPass(copy);
    if (!SDL_SubmitGPUCommandBuffer(cmd)) goto fail;
    return true;
fail:
    sc1_menu_gpu_destroy();
    return false;
}

bool sc1_menu_gpu_upload(SDL_GPUCommandBuffer *commands)
{
    compose();
    if (overflow) return SDL_SetError("Settings geometry exceeds its bounded buffer");
    void *mapped=SDL_MapGPUTransferBuffer(gpu,upload,true);
    if (!mapped) return false;
    memcpy(mapped,vertex,count*sizeof(*vertex));
    SDL_UnmapGPUTransferBuffer(gpu,upload);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(commands);
    if (!copy) return false;
    const SDL_GPUTransferBufferLocation from={.transfer_buffer=upload};
    const SDL_GPUBufferRegion to={.buffer=buffer,.size=count*sizeof(*vertex)};
    SDL_UploadToGPUBuffer(copy,&from,&to,true);
    SDL_EndGPUCopyPass(copy);
    return true;
}

bool sc1_menu_gpu_draw(SDL_GPUCommandBuffer *commands,SDL_GPUTexture *target,uint32_t width,uint32_t height)
{
    const SDL_GPUColorTargetInfo color={.texture=target,.load_op=SDL_GPU_LOADOP_LOAD,.store_op=SDL_GPU_STOREOP_STORE};
    SDL_GPURenderPass *pass=SDL_BeginGPURenderPass(commands,&color,1,NULL);
    if (!pass) return false;
    float scale=width/1280.0f;
    if (height/720.0f<scale) scale=height/720.0f;
    const SDL_GPUViewport viewport={(width-1280*scale)/2,(height-720*scale)/2,1280*scale,720*scale,0,1};
    SDL_SetGPUViewport(pass,&viewport);
    SDL_BindGPUGraphicsPipeline(pass,pipeline);
    const SDL_GPUBufferBinding vb={.buffer=buffer};
    const SDL_GPUTextureSamplerBinding texture={font,sampler};
    SDL_BindGPUVertexBuffers(pass,0,&vb,1);
    SDL_BindGPUFragmentSamplers(pass,0,&texture,1);
    const float dimensions[4]={1280,720,0,0};
    SDL_PushGPUVertexUniformData(commands,0,dimensions,sizeof(dimensions));
    SDL_DrawGPUPrimitives(pass,count,1,0,0);
    SDL_EndGPURenderPass(pass);
    return true;
}
