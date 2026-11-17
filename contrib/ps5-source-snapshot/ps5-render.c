/* Direct SDL GPU submission for Voldorium TA lists. Private development source.
   This replaces SDL_RenderGeometry so reciprocal-W and depth reach the GPU. */
#include "ta-pipeline.h"
#include "texture-state.h"
#include "menu.h"
#include "movie-blur.h"
#include "hd-texture.h"
#include "pc-piece.h"
#include "texture-hotcache.h"
#include "rtt-trace.h"
void sc1_audio_rtt_snapshot(uint64_t sequence, const char *phase);
#include "fsr1/cpe_fsr1.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SDL_GPUDevice *sc1_create_vulkan_device(void);
u32 tex_decode_rgba(u32 tcw, u32 tsp, u32 *out, u32 *w, u32 *h);
u32 hw_reg(u32 addr);
void hw_vram_touch(u32 offset, u32 len);

static SDL_Window *window;
static SDL_GPUDevice *device;
static CpeFsr1 *fsr;
static u32 screen_width=640, screen_height=480;
static bool fsr_requested;
static Sc1RenderStatus render_status={640,480,SC1_FSR_OFF,false,false};
Sc1RenderStatus sc1_render_status(void) { return render_status; }
static Sc1TaPipelines *pipelines;
static SDL_GPUTexture *color_target, *depth_target, *white;
static SDL_GPUTexture *rtt_color, *rtt_depth;
static SDL_GPUTextureFormat depth_format;
static SDL_GPUTransferBuffer *readback;
static u32 rtt_width, rtt_height, readback_capacity;
static SDL_GPUBuffer *vertices, *indices;
static SDL_GPUTransferBuffer *geometry_upload, *texture_upload;
static u32 vertex_capacity, index_capacity, upload_capacity;
static u32 texture_upload_capacity;
static u64 frames, last_present, rtt_frames;
static int initialized;
static Sc1MoviePolicy movie_policy;
static unsigned movie_logs;

typedef struct {
    u32 tcw, size, bytes;
    u64 last_used, uploaded;
    SDL_GPUTexture *texture;
    Sc1TextureState state;
} Texture;
typedef struct { const TaPoly *poly; u32 first, count; SDL_GPUTexture *texture; float depth; } Draw;
static Texture *textures;
static Draw *draws, *sort_scratch;
static size_t texture_capacity, draw_capacity, sort_capacity;
static u32 texture_count, draw_count;
static u64 texture_serial, texture_bytes;
static Sc1HotTextureCache hot_textures;
static u32 texture_hits, texture_uploads;
typedef struct { SDL_GPUTexture *texture; u32 tcw,tsp,width,height,offset; unsigned char *replacement; } TextureUpload;
static TextureUpload *texture_jobs;
static size_t texture_job_capacity;
static u32 texture_job_count, texture_job_bytes;
static u32 transition_mode, transition_generations[VRAM_SIZE >> 12];
static u8 transition_pages[VRAM_SIZE >> 12];
static bool transition_active, transition_held, transition_picture;

static void report(const char *operation)
{
    static unsigned errors;
    if (errors++ < 8)
        fprintf(stderr, "SC1 GPU depth: %s failed: %s\n", operation, SDL_GetError());
}

static SDL_GPUTexture *create_texture(u32 width, u32 height,
    SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage)
{
    const SDL_GPUTextureCreateInfo info = { .type = SDL_GPU_TEXTURETYPE_2D,
        .format = format, .usage = usage, .width = width, .height = height,
        .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
    return SDL_CreateGPUTexture(device, &info);
}

static bool upload_texture(SDL_GPUCopyPass *copy, SDL_GPUTexture *texture,
    const u32 *data, u32 width, u32 height)
{
    void *mapped = SDL_MapGPUTransferBuffer(device, texture_upload, true);
    if (!mapped) return false;
    memcpy(mapped, data, (size_t)width * height * 4);
    SDL_UnmapGPUTransferBuffer(device, texture_upload);
    const SDL_GPUTextureTransferInfo source = { .transfer_buffer = texture_upload,
        .pixels_per_row = width, .rows_per_layer = height };
    const SDL_GPUTextureRegion destination = { .texture = texture,
        .w = width, .h = height, .d = 1 };
    SDL_UploadToGPUTexture(copy, &source, &destination, false);
    return true;
}

static void release_texture(Texture *entry)
{
    if (entry->texture) {
        SDL_ReleaseGPUTexture(device, entry->texture);
        texture_bytes -= entry->bytes;
    }
    sc1_texture_state_clear(&entry->state);
    memset(entry, 0, sizeof(*entry));
}

static void release_all_textures(void)
{
    for (u32 i = 0; i < texture_count; ++i)
        release_texture(&textures[i]);
    texture_count = 0;
}

static void begin_texture_frame(void)
{
    ++texture_serial;
    if(!texture_serial){memset(&hot_textures,0,sizeof(hot_textures));++texture_serial;}
    texture_hits = texture_uploads = 0;
    texture_job_count=texture_job_bytes=0;
    /* Keep the live working set. Reclaim cold textures under memory pressure,
       and age out unused scenes even when below the 128 MiB soft budget. */
    for (u32 i = 0; i < texture_count; ++i) {
        Texture *entry = &textures[i];
        if (entry->texture && (texture_serial - entry->last_used > 300 ||
                (texture_bytes > 128u * 1024 * 1024 && texture_serial - entry->last_used > 1)))
            release_texture(entry);
    }
}

static void finish_texture_frame(bool submitted)
{
    for(u32 i=0;i<texture_job_count;++i){ free(texture_jobs[i].replacement); texture_jobs[i].replacement=NULL; }
    for (u32 i = 0; i < texture_count; ++i)
        if (textures[i].uploaded == texture_serial) {
            /* A cancelled command buffer never performed its uploads. */
            if (!submitted) release_texture(&textures[i]);
            else textures[i].uploaded = 0;
        }
}

static void shutdown_video(void)
{
    if (!device) return;
    SDL_WaitForGPUIdle(device);
    sc1_movie_blur_destroy();
    CpeFsr1_Destroy(fsr); fsr=NULL;
    release_all_textures();
    sc1_menu_gpu_destroy();
    sc1_ta_pipelines_destroy(pipelines);
    pipelines = NULL;
    if (vertices) SDL_ReleaseGPUBuffer(device, vertices);
    if (indices) SDL_ReleaseGPUBuffer(device, indices);
    if (geometry_upload) SDL_ReleaseGPUTransferBuffer(device, geometry_upload);
    if (texture_upload) SDL_ReleaseGPUTransferBuffer(device, texture_upload);
    if (white) SDL_ReleaseGPUTexture(device, white);
    if (color_target) SDL_ReleaseGPUTexture(device, color_target);
    if (depth_target) SDL_ReleaseGPUTexture(device, depth_target);
    if (rtt_color) SDL_ReleaseGPUTexture(device, rtt_color);
    if (rtt_depth) SDL_ReleaseGPUTexture(device, rtt_depth);
    if (readback) SDL_ReleaseGPUTransferBuffer(device, readback);
    if (window) SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    device = NULL;
    if (window) SDL_DestroyWindow(window);
    window = NULL;
    initialized = 0;
}

int sc1_init_video(void)
{
    static int attempted;
    if (attempted) return initialized;
    attempted = 1;
    sc1_menu_init();
    const char *stage = "SDL video initialization";
    fprintf(stderr, "SC1 GPU depth: %s\n", stage);
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) goto fail;
    stage = "Vulkan device creation";
    fprintf(stderr, "SC1 GPU depth: %s\n", stage);
    device = sc1_create_vulkan_device();
    if (!device) goto fail;
    stage = "window and swapchain creation";
    fprintf(stderr, "SC1 GPU depth: %s\n", stage);
    window = SDL_CreateWindow("Soul Calibur", 640, 480, 0);
    if (!window || !SDL_ClaimWindowForGPUDevice(device, window)) goto fail;
    if (!SDL_SetGPUSwapchainParameters(device, window,
            SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC)) goto fail;
    stage = "color and depth attachments";
    fprintf(stderr, "SC1 GPU depth: %s\n", stage);
    depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    if (!SDL_GPUTextureSupportsFormat(device, depth_format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
        depth_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    if (!SDL_GPUTextureSupportsFormat(device, depth_format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
        SDL_SetError("TA modifier volumes require a supported depth-stencil target");
        goto fail;
    }
    const char *scale=getenv("M1_PS5_SCALE");
    if((scale && !strcmp(scale,"2")) || (!scale && sc1_menu_render_scale()==2)){screen_width=1280;screen_height=960;}
    else if(scale && strcmp(scale,"1")) fprintf(stderr,"SC1 scaling: unsupported scale %s, using 1x\n",scale);
    const char *fsr_setting=getenv("M1_PS5_FSR");
    fsr_requested=fsr_setting ? !strcmp(fsr_setting,"1") : sc1_menu_fsr_enabled();
    render_status.width=screen_width;render_status.height=screen_height;
    render_status.override_scale=scale!=NULL;render_status.override_fsr=fsr_setting!=NULL;
    render_status.fsr_status=fsr_requested?SC1_FSR_PENDING:SC1_FSR_OFF;
    fprintf(stderr,"SC1 scaling: internal %ux%u; FSR %s requested\n",screen_width,screen_height,fsr_requested?"on":"off");
    if(fsr_requested){fsr=CpeFsr1_Create(device);if(fsr)CpeFsr1_SetEnabled(fsr,true);else {render_status.fsr_status=SC1_FSR_FAILED;fprintf(stderr,"SC1 FSR unavailable: %s; using linear GPU scaling\n",SDL_GetError());}}
    color_target = create_texture(screen_width, screen_height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    depth_target = create_texture(screen_width, screen_height, depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    white = create_texture(1, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    const SDL_GPUTransferBufferCreateInfo upload = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 65536 };
    texture_upload = SDL_CreateGPUTransferBuffer(device, &upload);
    texture_upload_capacity=upload.size;
    if (!color_target || !depth_target || !white || !texture_upload) goto fail;
    stage = "perspective shaders and depth pipeline";
    fprintf(stderr, "SC1 GPU depth: %s\n", stage);
    pipelines = sc1_ta_pipelines_create(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, depth_format);
    if (!pipelines) goto fail;
    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    if (!commands) goto fail;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    const u32 pixel = 0xffffffff;
    const bool uploaded = copy && upload_texture(copy, white, &pixel, 1, 1);
    if (copy) SDL_EndGPUCopyPass(copy);
    if (!uploaded) {
        SDL_CancelGPUCommandBuffer(commands);
        goto fail;
    }
    if (!SDL_SubmitGPUCommandBuffer(commands)) goto fail;
    initialized = 1;
    fprintf(stderr, "SC1 GPU depth: ready on %s, depth format %u\n", SDL_GetGPUDeviceDriver(device), (unsigned)depth_format);
    return 1;
fail: {
    char error[512];
    SDL_strlcpy(error, SDL_GetError(), sizeof(error));
    shutdown_video();
    SDL_SetError("%s: %s", stage, error);
    report("startup");
    return 0;
}
}

static bool reserve(void **storage, size_t *capacity, size_t count, size_t size)
{
    if (count <= *capacity) return true;
    if (count > SIZE_MAX / size) return SDL_SetError("TA allocation overflow");
    void *next = realloc(*storage, count * size);
    if (!next) return SDL_OutOfMemory();
    *storage = next;
    *capacity = count;
    return true;
}

static bool reserve_geometry(u32 vbytes, u32 ibytes)
{
    if (vbytes > UINT32_MAX - ibytes) return SDL_SetError("TA upload exceeds GPU buffer size");
    if (vbytes > vertex_capacity) {
        const SDL_GPUBufferCreateInfo info = { .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = vbytes };
        SDL_GPUBuffer *next = SDL_CreateGPUBuffer(device, &info);
        if (!next) return false;
        if (vertices) SDL_ReleaseGPUBuffer(device, vertices);
        vertices = next;
        vertex_capacity = vbytes;
    }
    if (ibytes > index_capacity) {
        const SDL_GPUBufferCreateInfo info = { .usage = SDL_GPU_BUFFERUSAGE_INDEX, .size = ibytes };
        SDL_GPUBuffer *next = SDL_CreateGPUBuffer(device, &info);
        if (!next) return false;
        if (indices) SDL_ReleaseGPUBuffer(device, indices);
        indices = next;
        index_capacity = ibytes;
    }
    if (vbytes + ibytes > upload_capacity) {
        const SDL_GPUTransferBufferCreateInfo info = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = vbytes + ibytes };
        SDL_GPUTransferBuffer *next = SDL_CreateGPUTransferBuffer(device, &info);
        if (!next) return false;
        if (geometry_upload) SDL_ReleaseGPUTransferBuffer(device, geometry_upload);
        geometry_upload = next;
        upload_capacity = vbytes + ibytes;
    }
    return true;
}

static bool valid_polygon(const TaCtx *ctx, const TaPoly *p)
{
    return (p->list == 0 || p->list == 4 || p->list == 2) && p->count >= 3 &&
        p->first < ctx->nv && p->count <= ctx->nv - p->first;
}

static bool valid_vertex(const TaVert *v)
{
    return fabsf(v->x) < 1e20f && fabsf(v->y) < 1e20f && fabsf(v->z) < 1e20f;
}

/* Stable bottom-up merge: preserve submission order at equal reciprocal W. */
static void sort_translucent(u32 first)
{
    const u32 count = draw_count - first;
    Draw *source = draws + first, *destination = sort_scratch;
    for (u32 width = 1; width < count; width *= 2) {
        for (u32 base = 0; base < count; base += width * 2) {
            const u32 middle = base + width < count ? base + width : count;
            const u32 end = middle + width < count ? middle + width : count;
            u32 left = base, right = middle;
            for (u32 at = base; at < end; ++at) {
                if (right == end || (left < middle && source[left].depth <= source[right].depth))
                    destination[at] = source[left++];
                else
                    destination[at] = source[right++];
            }
        }
        Draw *swap = source; source = destination; destination = swap;
    }
    if (source != draws + first) memcpy(draws + first, source, count * sizeof(Draw));
}

static bool prepare_rtt(u32 width, u32 height)
{
    if (rtt_color && rtt_width == width && rtt_height == height) return true;
    SDL_GPUTexture *color = create_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    SDL_GPUTexture *depth = create_texture(width, height, depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    if (!color || !depth) {
        if (color) SDL_ReleaseGPUTexture(device, color);
        if (depth) SDL_ReleaseGPUTexture(device, depth);
        return false;
    }
    if (rtt_color) SDL_ReleaseGPUTexture(device, rtt_color);
    if (rtt_depth) SDL_ReleaseGPUTexture(device, rtt_depth);
    rtt_color = color; rtt_depth = depth;
    rtt_width = width; rtt_height = height;
    return true;
}

/* The GPU draws the image. The CPU only packs its completed RGBA readback into
   the game's framebuffer format so subsequent guest texture reads see it. */
static bool finish_rtt(SDL_GPUCommandBuffer *commands, u32 width, u32 height)
{
    static uint64_t trace_sequence;
    static unsigned trace_emitted;
    const uint64_t trace_id = ++trace_sequence;
    const bool trace = sc1_rtt_trace_sample(trace_id, trace_emitted);
    if (trace) { ++trace_emitted; sc1_audio_rtt_snapshot(trace_id, "before"); }
    const uint64_t trace_start = trace ? SDL_GetTicksNS() : 0;
    const u32 needed = width * height * 4;
    if (needed > readback_capacity) {
        const SDL_GPUTransferBufferCreateInfo info = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = needed };
        SDL_GPUTransferBuffer *next = SDL_CreateGPUTransferBuffer(device, &info);
        if (!next) { SDL_CancelGPUCommandBuffer(commands); return false; }
        if (readback) SDL_ReleaseGPUTransferBuffer(device, readback);
        readback = next;
        readback_capacity = needed;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) { SDL_CancelGPUCommandBuffer(commands); return false; }
    const SDL_GPUTextureRegion source = { .texture = rtt_color, .w = width, .h = height, .d = 1 };
    const SDL_GPUTextureTransferInfo destination = { .transfer_buffer = readback,
        .pixels_per_row = width, .rows_per_layer = height };
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    const uint64_t trace_submit = trace ? SDL_GetTicksNS() : 0;
    if (!fence) return false;
    const bool complete = SDL_WaitForGPUFences(device, true, &fence, 1);
    const uint64_t trace_wait = trace ? SDL_GetTicksNS() : 0;
    SDL_ReleaseGPUFence(device, fence);
    if (!complete) return false;
    const u8 *rgba = SDL_MapGPUTransferBuffer(device, readback, false);
    const uint64_t trace_map = trace ? SDL_GetTicksNS() : 0;
    if (!rgba) return false;
    const u32 ctrl = hw_reg(0x005f8048), pack = ctrl & 7, bpp = pack >= 4 ? 4 : 2;
    const u32 start = hw_reg(0x005f8060) & (VRAM_SIZE - 1), threshold = (ctrl >> 16) & 255;
    u32 stride = (hw_reg(0x005f804c) & 0x1ff) * 8;
    if (!stride) stride = width * bpp;
    u8 *vram = hw_vram();
    for (u32 y = 0; y < height; ++y) {
        const u8 *pixel = rgba + y * width * 4; // SDL texture rows are top to bottom.
        const u32 line = start + y * stride;
        for (u32 x = 0; x < width; ++x, pixel += 4) {
            const u32 r = pixel[0], g = pixel[1], b = pixel[2], a = pixel[3];
            u32 value;
            switch (pack) {
            case 0: value = (r >> 3) << 10 | (g >> 3) << 5 | b >> 3 | (ctrl & 0x8000); break;
            case 1: value = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3; break;
            case 2: value = (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4; break;
            case 3: value = (a >= threshold ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3; break;
            default: value = a << 24 | r << 16 | g << 8 | b; break;
            }
            for (u32 byte = 0; byte < bpp; ++byte)
                vram[(line + x * bpp + byte) & (VRAM_SIZE - 1)] = (u8)(value >> (byte * 8));
        }
        hw_vram_touch(line, width * bpp);
    }
    SDL_UnmapGPUTransferBuffer(device, readback);
    if (trace) {
        const uint64_t trace_end = SDL_GetTicksNS();
        fprintf(stderr, "SC1 RTT timing: id=%" PRIu64 " time_ns=%" PRIu64 " size=%ux%u pack=%u setup_submit_us=%" PRIu64 " fence_wait_us=%" PRIu64 " fence_release_map_us=%" PRIu64 " pack_unmap_us=%" PRIu64 "\n",
            trace_id, trace_end, width, height, pack, (trace_submit-trace_start)/1000,
            (trace_wait-trace_submit)/1000, (trace_map-trace_wait)/1000, (trace_end-trace_map)/1000);
        sc1_audio_rtt_snapshot(trace_id, "after");
    }
    ++rtt_frames;
    if (rtt_frames == 1)
        fprintf(stderr, "SC1 GPU depth: completed first render-to-texture %ux%u, pack=%u\n", width, height, pack);
    return true;
}

static bool draw_shadows(const TaCtx *ctx, SDL_GPUCommandBuffer *commands,
    SDL_GPURenderPass *pass, u32 width, u32 height)
{
    if (!ctx->nmv || !(hw_reg(0x005f8074) & 0x100)) return true;
    u32 volume_start = 0;
    for (u32 start = 0; start < ctx->nmv;) {
        u32 end = start + 1;
        while (end < ctx->nmv && !(ctx->mvmode[end] & 0x80)) ++end;
        const u32 mode = ctx->mvmode[start] & 7, last = (ctx->mvmode[start] >> 3) & 1;
        const bool open = !last && mode > 0;
        if (!sc1_ta_bind_shadow(pipelines, commands, pass, white,
                open ? SC1_SHADOW_SET : SC1_SHADOW_TOGGLE, open ? 1 : 0, width, height)) return false;
        SDL_DrawGPUPrimitives(pass, (end - start) * 3, 1, ctx->nv + start * 3, 0);
        if (mode == 1 || mode == 2) {
            if (!sc1_ta_bind_shadow(pipelines, commands, pass, white, SC1_SHADOW_RESOLVE,
                    mode == 2 ? 2 : 3, width, height)) return false;
            SDL_DrawGPUPrimitives(pass, (end - volume_start) * 3, 1, ctx->nv + volume_start * 3, 0);
            if (!sc1_ta_bind_shadow(pipelines, commands, pass, white, SC1_SHADOW_CLEAR, 0, width, height)) return false;
            SDL_DrawGPUPrimitives(pass, (end - volume_start) * 3, 1, ctx->nv + volume_start * 3, 0);
            volume_start = end;
        }
        start = end;
    }
    if (!sc1_ta_bind_shadow(pipelines, commands, pass, white, SC1_SHADOW_DARKEN, 0x82, width, height)) return false;
    SDL_DrawGPUPrimitives(pass, 6, 1, ctx->nv + ctx->nmv * 3, 0);
    return true;
}

static SDL_GPUTexture *texture_for(const TaPoly *poly)
{
    if (!(poly->isp & (1u << 25))) return white;
    const u32 size = poly->tsp & 63;
    SDL_GPUTexture *hot=sc1_hot_get(&hot_textures,texture_serial,poly->tcw,size);
    if(hot){++texture_hits;return hot;}
    u32 slot = texture_count;
    for (u32 i = 0; i < texture_count; ++i) {
        Texture *entry = &textures[i];
        if (entry->texture && entry->tcw == poly->tcw && entry->size == size) {
            if (entry->last_used == texture_serial || !sc1_texture_state_changed(&entry->state)) {
                entry->last_used = texture_serial;
                ++texture_hits;
                sc1_hot_put(&hot_textures,texture_serial,poly->tcw,size,entry->texture);
                return entry->texture;
            }
            slot = i;
            break;
        }
        if (!entry->texture && slot == texture_count) slot = i;
    }
    if (slot == texture_count) {
        if (!reserve((void **)&textures, &texture_capacity, (size_t)texture_count + 1, sizeof(*textures))) return NULL;
        memset(&textures[texture_count++], 0, sizeof(*textures));
    }
    u32 width=8u<<((poly->tsp>>3)&7),height=(poly->tcw>>31)?width:8u<<(poly->tsp&7);
    Texture *entry = &textures[slot];
    release_texture(entry);
    if (!sc1_texture_state_capture(&entry->state, poly->tcw, poly->tsp)) {
        sc1_texture_state_clear(&entry->state);
        SDL_OutOfMemory();
        return NULL;
    }
    const char *config_get(const char *,const char *);
    const char *keylog=config_get("debug","ps5_texture_keys");
    if(getenv("M1_TEXLOG") || (keylog && atoi(keylog))){
        static unsigned reported;
        if(reported++<256)fprintf(stderr,"SC1 texture key: %016" PRIx64 " %ux%u format=%u\n",
            entry->state.source_hash,width,height,(poly->tcw>>27)&7);
    }
    unsigned char *replacement=sc1_hd_texture(entry->state.source_hash,width,height,&width,&height);
    if(!replacement)replacement=sc1_pc_piece(entry->state.source_hash,poly->tcw,poly->tsp,entry->state.start,entry->state.span,&width,&height);
    const u32 bytes=width*height*4;
    if(texture_job_bytes>UINT32_MAX-bytes ||
        !reserve((void**)&texture_jobs,&texture_job_capacity,(size_t)texture_job_count+1,sizeof(*texture_jobs))) {
        free(replacement);sc1_texture_state_clear(&entry->state);return NULL;
    }
    SDL_GPUTexture *texture = create_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!texture) {free(replacement);sc1_texture_state_clear(&entry->state);return NULL;}
    entry->tcw = poly->tcw;
    entry->size = size;
    entry->bytes = width * height * 4;
    entry->last_used = entry->uploaded = texture_serial;
    entry->texture = texture;
    texture_bytes += entry->bytes;
    texture_jobs[texture_job_count++]=(TextureUpload){texture,poly->tcw,poly->tsp,width,height,texture_job_bytes,replacement};
    texture_job_bytes+=bytes;
    ++texture_uploads;
    sc1_hot_put(&hot_textures,texture_serial,poly->tcw,size,texture);
    return texture;
}

/* One map per frame, with non-overlapping offsets for all changed textures.
   Every decoded texture is a power-of-two RGBA image (minimum 8x8), so offsets
   are naturally multiples of 256 bytes. No fixed 4 MiB backing per texture. */
static bool stage_textures(void)
{
    for(u32 i=0;i<draw_count;++i){
        draws[i].texture=texture_for(draws[i].poly);
        if(!draws[i].texture)return false;
    }
    if(!texture_job_count)return true;
    if(texture_job_bytes>texture_upload_capacity){
        const SDL_GPUTransferBufferCreateInfo info={.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,.size=texture_job_bytes};
        SDL_GPUTransferBuffer *next=SDL_CreateGPUTransferBuffer(device,&info);if(!next)return false;
        SDL_ReleaseGPUTransferBuffer(device,texture_upload);texture_upload=next;texture_upload_capacity=texture_job_bytes;
    }
    u8 *mapped=SDL_MapGPUTransferBuffer(device,texture_upload,true);if(!mapped)return false;
    bool valid=true;
    for(u32 i=0;i<texture_job_count;++i){
        const TextureUpload *job=&texture_jobs[i];u32 width,height;
        if(job->replacement){memcpy(mapped+job->offset,job->replacement,(size_t)job->width*job->height*4);width=job->width;height=job->height;}
        else tex_decode_rgba(job->tcw,job->tsp,(u32*)(mapped+job->offset),&width,&height);
        if(width!=job->width||height!=job->height){valid=SDL_SetError("TA decoded texture size mismatch");break;}
    }
    SDL_UnmapGPUTransferBuffer(device,texture_upload);return valid;
}
static void upload_staged_textures(SDL_GPUCopyPass *copy)
{
    for(u32 i=0;i<texture_job_count;++i){
        const TextureUpload *job=&texture_jobs[i];
        const SDL_GPUTextureTransferInfo from={.transfer_buffer=texture_upload,.offset=job->offset,
            .pixels_per_row=job->width,.rows_per_layer=job->height};
        const SDL_GPUTextureRegion to={.texture=job->texture,.w=job->width,.h=job->height,.d=1};
        SDL_UploadToGPUTexture(copy,&from,&to,false);
    }
}

u64 render_count(void) { return frames; }
u64 render_rtt_count(void) { return rtt_frames; }

/* Match the existing PC VS -> fight transition: the guest reuses VS texture
   memory while still issuing its last VS lists. Preserve the completed GPU
   picture during that overwrite, then resume drawing when the mode advances. */
static bool hold_transition(void)
{
    const u32 mode = rd32(0x8c24a584);
    const bool opening = mode == 16 || (mode == 17 && transition_mode != 17);
    const bool was_held = transition_held;
    if (mode == 16 && transition_mode != 16) {
        memset(transition_pages, 0, sizeof(transition_pages));
        transition_active = true;
        transition_picture = false;
    }
    u32 changed = 0, overwritten = 0;
    for (u32 page = 0; page < (VRAM_SIZE >> 12); ++page) {
        const u32 generation = hw_vram_gen(page);
        if (generation != transition_generations[page]) {
            ++changed;
            overwritten += transition_pages[page];
            if (opening) transition_pages[page] = 1;
        }
        transition_generations[page] = generation;
    }
    if (mode != 17 && mode != 0) transition_held = false;
    else if (transition_active && transition_picture && !opening && (changed >= 256 || overwritten >= 16))
        transition_held = true;
    if (mode != 16 && mode != 17 && mode != 0) transition_active = transition_picture = false;
    if (was_held != transition_held) {
        static u32 reports;
        if (reports++ < 16)
            fprintf(stderr, "SC1 GPU depth: VS picture %s, mode=%u changed-pages=%u reused-pages=%u\n",
                transition_held ? "retained" : "released", mode, changed, overwritten);
    }
    transition_mode = mode;
    return transition_held;
}

/* Consumes commands on both success and failure. The retained 640x480 image
   remains a GPU texture, including frames held during the VS transition. */
static bool submit_screen(SDL_GPUCommandBuffer *commands, bool *presented)
{
    SDL_GPUTexture *swapchain = NULL;
    Uint32 width = 0, height = 0;
    *presented = false;
    if (sc1_menu_visible() && !sc1_menu_gpu_upload(commands)) {
        SDL_CancelGPUCommandBuffer(commands);
        return false;
    }
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, window, &swapchain, &width, &height)) {
        SDL_CancelGPUCommandBuffer(commands);
        return false;
    }
    if (swapchain && width && height) {
        sc1_movie_observe(&movie_policy,rd32(0x8c24a584),sc1_menu_movie_blur());
        const u32 numerator=sc1_menu_widescreen()?16:4, denominator=sc1_menu_widescreen()?9:3;
        u32 vw = width, vh = width * denominator / numerator;
        if (vh > height) { vh = height; vw = height * numerator / denominator; }
        if (sc1_menu_fill_screen()) { vw=width; vh=height; }
        bool backdrop=false;
        if(movie_policy.active){
            const Sc1MovieRect fit=sc1_movie_fit(width,height);
            vw=movie_policy.blur?fit.w:width;vh=movie_policy.blur?fit.h:height;
            if(movie_policy.blur && (vw!=width || vh!=height))
                backdrop=sc1_movie_backdrop(device,commands,color_target,screen_width,screen_height,swapchain,width,height,movie_policy.serial);
            static uint32_t logged_serial;
            if(logged_serial!=movie_policy.serial && movie_logs++<32){
                fprintf(stderr,"SC1 cinematic: mode=%u source=%u framing=%s backdrop=%d center=%ux%u output=%ux%u\n",
                    rd32(0x8c24a584),movie_policy.serial,movie_policy.blur?"original-4:3":"fill",backdrop,vw,vh,width,height);
                logged_serial=movie_policy.serial;
            }
        }
        SDL_GPUTexture *present_source=color_target;
        u32 source_width=screen_width,source_height=screen_height;
        if(fsr && vw>=screen_width && vh>=screen_height) {
            SDL_GPUTexture *upscaled=CpeFsr1_Record(fsr,commands,color_target,screen_width,screen_height,vw,vh,0.5f);
            if(upscaled){present_source=upscaled;source_width=vw;source_height=vh;render_status.fsr_status=SC1_FSR_ACTIVE;}
            else {render_status.fsr_status=SC1_FSR_FAILED;static unsigned failures;if(failures++<4)fprintf(stderr,"SC1 FSR pass failed: %s; using linear GPU scaling\n",SDL_GetError());}
        } else if(fsr) render_status.fsr_status=SC1_FSR_BYPASS;
        const SDL_GPUBlitInfo blit = {
            .source = { .texture = present_source, .w = source_width, .h = source_height },
            .destination = { .texture = swapchain, .x = (width - vw) / 2, .y = (height - vh) / 2, .w = vw, .h = vh },
            .load_op = backdrop ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR, .clear_color = { 0, 0, 0, 1 }, .filter = SDL_GPU_FILTER_LINEAR
        };
        SDL_BlitGPUTexture(commands, &blit);
        if (sc1_menu_visible() && !sc1_menu_gpu_draw(commands,swapchain,width,height)) {
            /* An acquired swapchain image must be submitted, not cancelled. */
            SDL_SubmitGPUCommandBuffer(commands);
            return false;
        }
        *presented = true;
    }
    return SDL_SubmitGPUCommandBuffer(commands);
}

bool sc1_present_settings(void)
{
    if (!initialized) return SDL_SetError("Renderer is not ready for settings");
    if (!sc1_menu_gpu_create(device,SDL_GetGPUSwapchainTextureFormat(device,window))) return false;
    SDL_GPUCommandBuffer *commands=SDL_AcquireGPUCommandBuffer(device);
    if (!commands) return false;
    bool presented;
    if (!submit_screen(commands,&presented)) return false;
    if (presented) { ++frames; last_present=SDL_GetTicksNS(); }
    return true;
}

void render_frame(TaCtx *ctx)
{
    if (!initialized || !ctx || !ctx->p || !ctx->v) return;
    if (ctx->np > TA_MAXP || ctx->nv > TA_MAXV || ctx->nmv > TA_MAXMV || (ctx->nmv && (!ctx->mv || !ctx->mvmode))) {
        SDL_SetError("TA context exceeds allocation bounds");
        report("context validation");
        return;
    }
    const u64 begin = SDL_GetTicksNS();
    begin_texture_frame();
    const bool rtt = (hw_reg(0x005f8060) & 0x1000000u) != 0;
    if (!rtt && hold_transition()) {
        SDL_GPUCommandBuffer *held = SDL_AcquireGPUCommandBuffer(device);
        bool presented;
        if (!held || !submit_screen(held, &presented)) { report("transition presentation"); return; }
        if (presented) { ++frames; last_present = SDL_GetTicksNS(); }
        return;
    }
    const u32 target_width = rtt ? ((hw_reg(0x005f8068) >> 16) & 0x7ff) + 1 : 640;
    const u32 target_height = rtt ? ((hw_reg(0x005f806c) >> 16) & 0x3ff) + 1 : 480;
    if (rtt && !prepare_rtt(target_width, target_height)) { report("RTT attachments"); return; }
    u64 total = 0;
    for (u32 i = 0; i < ctx->np; ++i)
        if (valid_polygon(ctx, &ctx->p[i])) total += (u64)(ctx->p[i].count - 2) * 3;
    if (total > UINT32_MAX / sizeof(u32)) {
        SDL_SetError("TA index buffer overflow");
        report("index validation");
        return;
    }
    const bool shadows = ctx->nmv && (hw_reg(0x005f8074) & 0x100);
    const u32 vertex_count = ctx->nv + (shadows ? 3 * ctx->nmv + 6 : 0);
    const u32 vbytes = (vertex_count ? vertex_count : 1) * sizeof(TaVert), ibytes = (u32)(total ? total : 1) * sizeof(u32);
    if (!reserve((void **)&draws, &draw_capacity, ctx->np, sizeof(*draws)) ||
        !reserve((void **)&sort_scratch, &sort_capacity, ctx->np, sizeof(*sort_scratch)) ||
        !reserve_geometry(vbytes, ibytes)) {
        report("frame allocation");
        return;
    }
    u8 *mapped = SDL_MapGPUTransferBuffer(device, geometry_upload, true);
    if (!mapped) { report("geometry mapping"); return; }
    if (ctx->nv) memcpy(mapped, ctx->v, ctx->nv * sizeof(TaVert));
    else memset(mapped, 0, vbytes);
    if (shadows) {
        TaVert *volume = (TaVert *)mapped + ctx->nv;
        memset(volume, 0, (3 * ctx->nmv + 6) * sizeof(TaVert));
        for (u32 i = 0; i < ctx->nmv * 3; ++i) {
            volume[i].x = ctx->mv[i * 3];
            volume[i].y = ctx->mv[i * 3 + 1];
            volume[i].z = ctx->mv[i * 3 + 2];
        }
        static const unsigned corner[6] = { 0, 1, 2, 2, 1, 3 };
        const u32 shade = hw_reg(0x005f8074) & 255;
        for (unsigned i = 0; i < 6; ++i) {
            TaVert *v = &volume[ctx->nmv * 3 + i];
            v->x = (corner[i] & 1) ? target_width : 0;
            v->y = (corner[i] & 2) ? target_height : 0;
            v->z = 1;
            v->col = 0xff000000u | shade << 16 | shade << 8 | shade;
        }
    }
    u32 *index = (u32 *)(mapped + vbytes);
    u32 ni = 0;
    draw_count = 0;
    u32 first_translucent = 0;
    const u8 lists[] = { 0, 4, 2 };
    for (unsigned layer = 0; layer < sizeof(lists); ++layer) {
        if (lists[layer] == 2) first_translucent = draw_count;
        for (u32 p = 0; p < ctx->np; ++p) {
            const TaPoly *poly = &ctx->p[p];
            if (poly->list != lists[layer] || !valid_polygon(ctx, poly)) continue;
            const u32 first = ni;
            for (u32 j = 2; j < poly->count; ++j) {
                const u32 base = poly->first + j - 2;
                if (!valid_vertex(&ctx->v[base]) || !valid_vertex(&ctx->v[base + 1]) || !valid_vertex(&ctx->v[base + 2])) continue;
                // A triangle strip reverses its winding on every odd triangle.
                index[ni++] = base + (j & 1);
                index[ni++] = base + 1 - (j & 1);
                index[ni++] = base + 2;
            }
            if (ni > first) {
                double sum = 0;
                u32 finite = 0;
                for (u32 j = 0; j < poly->count; ++j)
                    if (valid_vertex(&ctx->v[poly->first + j])) { sum += ctx->v[poly->first + j].z; ++finite; }
                draws[draw_count++] = (Draw){ .poly = poly, .first = first, .count = ni - first,
                    .depth = finite ? (float)(sum / finite) : 0 };
            }
        }
    }
    SDL_UnmapGPUTransferBuffer(device, geometry_upload);
    if (!(hw_reg(0x005f8098) & 1) && draw_count - first_translucent > 1) sort_translucent(first_translucent);
    if(!stage_textures()){report("packed texture staging");finish_texture_frame(false);return;}
    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    if (!commands) { report("command buffer"); finish_texture_frame(false); return; }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) { SDL_CancelGPUCommandBuffer(commands); report("copy pass"); finish_texture_frame(false); return; }
    const SDL_GPUTransferBufferLocation vsource = { .transfer_buffer = geometry_upload };
    const SDL_GPUTransferBufferLocation isource = { .transfer_buffer = geometry_upload, .offset = vbytes };
    const SDL_GPUBufferRegion vdest = { .buffer = vertices, .size = vbytes };
    const SDL_GPUBufferRegion idest = { .buffer = indices, .size = ni * sizeof(u32) };
    SDL_UploadToGPUBuffer(copy, &vsource, &vdest, true);
    if (ni) SDL_UploadToGPUBuffer(copy, &isource, &idest, true);
    bool ok = true;
    upload_staged_textures(copy);
    SDL_EndGPUCopyPass(copy);
    if (!ok) {
        SDL_CancelGPUCommandBuffer(commands);
        report("texture upload");
        finish_texture_frame(false);
        return;
    }
    const u64 uploaded = SDL_GetTicksNS();
    const u32 bgword = hw_reg(0x005f808c), param = hw_reg(0x005f8020) & 0x7fffff;
    const u32 bgaddr = param + ((bgword >> 3) & 0x1fffff) * 4;
    const u32 bgisp = vram_rd32_map32(bgaddr);
    const u32 bg = vram_rd32_map32(bgaddr + (6 + ((bgisp & (1u << 25)) ? 2 : 0)) * 4);
    const SDL_GPUColorTargetInfo color = { .texture = rtt ? rtt_color : color_target,
        .clear_color = { ((bg >> 16) & 255) / 255.0f, ((bg >> 8) & 255) / 255.0f, (bg & 255) / 255.0f, rtt ? (bg >> 24) / 255.0f : 1 },
        .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE, .cycle = true };
    const SDL_GPUDepthStencilTargetInfo depth = { .texture = rtt ? rtt_depth : depth_target, .clear_depth = 1.0f,
        .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_DONT_CARE,
        .stencil_load_op = SDL_GPU_LOADOP_CLEAR, .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE, .cycle = true };
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &color, 1, &depth);
    if (!pass) {
        SDL_CancelGPUCommandBuffer(commands);
        report("render pass");
        finish_texture_frame(false);
        return;
    }
    const SDL_GPUBufferBinding vb = { .buffer = vertices }, ib = { .buffer = indices };
    sc1_ta_begin_pass(pipelines);
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    const u32 alpha = hw_reg(0x005f811c), fog = hw_reg(0x005f80b4);
    for (u32 i = 0; i < draw_count; ++i) {
        if (i == first_translucent && !draw_shadows(ctx, commands, pass, target_width, target_height)) { ok = false; break; }
        const Draw *draw = &draws[i];
        if (!sc1_ta_bind(pipelines, commands, pass, draw->poly, draw->texture, target_width, target_height, alpha, fog)) {
            ok = false;
            break;
        }
        SDL_DrawGPUIndexedPrimitives(pass, draw->count, 1, draw->first, 0, 0);
    }
    if (ok && first_translucent == draw_count)
        ok = draw_shadows(ctx, commands, pass, target_width, target_height);
    SDL_EndGPURenderPass(pass);
    if (!ok) {
        SDL_CancelGPUCommandBuffer(commands);
        report("TA pipeline");
        finish_texture_frame(false);
        return;
    }
    const u64 drawn = SDL_GetTicksNS();
    if (rtt) {
        ok = finish_rtt(commands, target_width, target_height);
        finish_texture_frame(ok);
        if (!ok) report("render-to-texture readback");
        return;
    }
    bool presented;
    ok = submit_screen(commands, &presented);
    finish_texture_frame(ok);
    if (!ok) { report("presentation"); return; }
    if (transition_mode == 16 || transition_mode == 17) transition_picture = true;
    if (!presented) return;
    ++frames;
    const u64 end = SDL_GetTicksNS();
    if (frames == 1 || frames == 60 || frames == 300 || frames % 900 == 0) {
        u32 pipeline_binds, sampler_binds, uniform_pushes;
        sc1_ta_bind_counts(pipelines,&pipeline_binds,&sampler_binds,&uniform_pushes);
        fprintf(stderr, "SC1 GPU depth: frame %" PRIu64 ", %u triangles, texture uploads=%u hits=%u resident=%" PRIu64 " KiB; upload=%" PRIu64
            " us draw=%" PRIu64 " us present=%" PRIu64 " us interval=%" PRIu64 " us\n",
            frames, ni / 3, texture_uploads, texture_hits, texture_bytes / 1024, (uploaded - begin) / 1000, (drawn - uploaded) / 1000,
            (end - drawn) / 1000, last_present ? (end - last_present) / 1000 : 0);
        fprintf(stderr,"SC1 timing correlation: frame=%" PRIu64 " time_ns=%" PRIu64 " clock=SDL_GetTicksNS cpu_only=1\n", frames,end);
        fprintf(stderr,"SC1 GPU bindings: pipelines=%u samplers=%u uniform-pushes=%u\n",
            pipeline_binds,sampler_binds,uniform_pushes);
        fprintf(stderr,"SC1 GPU staging: jobs=%u bytes=%u arena=%u bytes movie=%d mode=%u\n",
            texture_job_count,texture_job_bytes,texture_upload_capacity,movie_policy.active,rd32(0x8c24a584));
    }
    last_present = end;
}
