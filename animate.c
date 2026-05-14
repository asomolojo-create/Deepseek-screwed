#include "animate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#define PACKED __attribute__((packed))
#else
#define PACKED
#endif

struct PACKED bitmap_header {
    uint8_t magic[2];
    uint32_t file_size;
    uint32_t reserved;
    uint32_t pixel_offset;
};

struct PACKED bitmap5_header {
    uint32_t size;
    int32_t  bV5Width;
    int32_t  bV5Height;
    uint16_t planes;
    uint16_t bits_per_pixel;
    uint32_t compression;
    uint32_t image_size;
    int32_t  x_pixels_per_meter;
    int32_t  y_pixels_per_meter;
    uint32_t colors_used;
    uint32_t colors_important;
};


enum sprite_type { SPRITE_BITMAP, SPRITE_RECT, SPRITE_CIRCLE };

struct sprite {
    enum sprite_type type;
    size_t width, height;
    color_t* pixels;   // ARGB32
    int ref_count;
};

struct canvas {
    size_t height, width;
    color_t background_color;
    struct sprite_placement* top_layer;
    struct sprite_placement* bottom_layer;
};

struct sprite_placement {
    struct canvas* canvas;
    struct sprite* sprite;
    ssize_t x, y;
    ssize_t vx, vy, ax, ay;
    animate_fn custom_fn;
    void* priv;
    struct sprite_placement* next; // above
    struct sprite_placement* prev; // below
};

// Helpers -----------------------------------------------------

static void sprite_ref(struct sprite* s) { if (s) s->ref_count++; }

static bool sprite_unref(struct sprite* s) {
    if (!s || s->ref_count <= 0) return false;
    if (--s->ref_count == 0) { free(s->pixels); free(s); return true; }
    return false;
}

// Loads BMP (ARGB32), reversing rows
static struct sprite* load_bmp(const char* file) {
    FILE* fp = fopen(file, "rb");
    if (!fp) return NULL;

    struct bitmap_header bmp_header;
    if (fread(&bmp_header, sizeof(bmp_header), 1, fp) != 1 ||
        bmp_header.magic[0] != 'B' || bmp_header.magic[1] != 'M') {
        fclose(fp);
        return NULL;
    }

    struct bitmap5_header b5;
    if (fread(&b5, sizeof(b5), 1, fp) != 1) {
        fclose(fp);
        return NULL;
    }

    if (b5.size < 40 || b5.planes != 1 || b5.bits_per_pixel != 32) {
        fclose(fp);
        return NULL;
    }
    if (b5.bV5Width <= 0 || b5.bV5Height == 0) {
        fclose(fp);
        return NULL;
    }

    size_t w = (size_t)b5.bV5Width;
    size_t h = (size_t)(b5.bV5Height < 0 ? -(int64_t)b5.bV5Height : (int64_t)b5.bV5Height);
    if (w == 0 || h == 0 || h > SIZE_MAX / w || w * h > SIZE_MAX / sizeof(color_t)) {
        fclose(fp);
        return NULL;
    }

    struct sprite* s = malloc(sizeof(*s));
    if (!s) { fclose(fp); return NULL; }

    s->type = SPRITE_BITMAP;
    s->width = w;
    s->height = h;
    s->ref_count = 1; // owner reference
    s->pixels = malloc(w * h * sizeof(color_t));
    if (!s->pixels) { free(s); fclose(fp); return NULL; }

    if (fseek(fp, (long)bmp_header.pixel_offset, SEEK_SET) != 0) {
        free(s->pixels); free(s); fclose(fp); return NULL;
    }

    bool bottom_up = (b5.bV5Height > 0);
    for (size_t y = 0; y < h; y++) {
        size_t dst_row = bottom_up ? (h - 1 - y) : y;
        if (fread(&s->pixels[dst_row * w], sizeof(color_t), w, fp) != w) {
            free(s->pixels); free(s); fclose(fp); return NULL;
        }
    }

    fclose(fp);
    return s;
}

// Shapes as pixel sprites
static struct sprite* make_rect(size_t w, size_t h, color_t c, bool filled) {
    struct sprite* s = malloc(sizeof(*s));
    if (!s) return NULL;
    s->type = SPRITE_RECT;
    s->width = w;
    s->height = h;
    s->ref_count = 1; // owner reference
    s->pixels = malloc(w * h * sizeof(color_t));
    if (!s->pixels) { free(s); return NULL; }

    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            bool border = (x == 0 || y == 0 || x == w - 1 || y == h - 1);
            bool draw = filled || border;
            s->pixels[y * w + x] = draw ? c : 0;
        }
    }
    return s;
}

static struct sprite* make_circle(size_t r, color_t c) {
    size_t d = 2 * r + 1;
    struct sprite* s = malloc(sizeof(*s));
    if (!s) return NULL;
    s->type = SPRITE_CIRCLE;
    s->width = d;
    s->height = d;
    s->ref_count = 1; // owner reference
    s->pixels = malloc(d * d * sizeof(color_t));
    if (!s->pixels) { free(s); return NULL; }

    for (size_t y = 0; y < d; y++) {
        for (size_t x = 0; x < d; x++) {
            ssize_t dx = (ssize_t)x - (ssize_t)r;
            ssize_t dy = (ssize_t)y - (ssize_t)r;
            bool inside = dx * dx + dy * dy <= (ssize_t)(r * r);
            s->pixels[y * d + x] = inside ? c : 0;
        }
    }
    return s;
}

// Core API ----------------------------------------------------

struct canvas* animate_create_canvas(size_t height, size_t width, color_t background_color)
{
    struct canvas* canvas = malloc(sizeof(struct canvas));
    if (!canvas) return NULL;
    canvas->height = height;
    canvas->width = width;
    canvas->background_color = background_color;
    canvas->top_layer = NULL;
    canvas->bottom_layer = NULL;
    return canvas;
}

void animate_destroy_canvas(struct canvas* canvas)
{
    if (!canvas) return;
    struct sprite_placement* p = canvas->bottom_layer;
    while (p) {
        struct sprite_placement* next = p->next;
        sprite_unref(p->sprite);
        free(p);
        p = next;
    }
    free(canvas);
}

struct sprite* animate_create_sprite(const char* file) 
{
    return load_bmp(file);
}

struct sprite* animate_create_circle(size_t radius, color_t c, bool filled) 
{
    (void)filled; // reserved
    return make_circle(radius, c);
}

struct sprite* animate_create_rectangle(size_t width, size_t height, color_t c, bool filled)
{
    return make_rect(width, height, c, filled);
}

bool animate_destroy_sprite(struct sprite* sprite) 
{
    if (!sprite) return true;          // error
    if (sprite->ref_count > 1) return true; // still in use by placements
    (void)sprite_unref(sprite);        // release owner ref
    return false;                      // success
}

// Add safe accessors for sprite dimensions
size_t animate_sprite_width(const struct sprite* s) {
    return s ? s->width : 0;
}
size_t animate_sprite_height(const struct sprite* s) {
    return s ? s->height : 0;
}

struct sprite_placement* animate_place_sprite(struct canvas* canvas, struct sprite* sprite, ssize_t x, ssize_t y) 
{
    if (!canvas || !sprite) return NULL;
    struct sprite_placement* placement = malloc(sizeof(struct sprite_placement));
    if (!placement) return NULL;
    placement->canvas = canvas;
    placement->sprite = sprite;
    placement->x = x;
    placement->y = y;
    placement->vx = placement->vy = placement->ax = placement->ay = 0;
    placement->custom_fn = NULL;
    placement->priv = NULL;
    placement->next = NULL;
    placement->prev = canvas->top_layer;

    sprite_ref(sprite);

    if (canvas->top_layer) canvas->top_layer->next = placement;
    else canvas->bottom_layer = placement;
    canvas->top_layer = placement;
    return placement;
}

static void swap_with_next(struct sprite_placement* p) {
    struct canvas* c = p->canvas;
    struct sprite_placement* n = p->next;
    if (!n) return;
    struct sprite_placement* before = p->prev;
    struct sprite_placement* after = n->next;

    if (before) before->next = n; else c->bottom_layer = n;
    if (after) after->prev = p;   else c->top_layer = p;

    n->prev = before;
    n->next = p;
    p->prev = n;
    p->next = after;
}

static void swap_with_prev(struct sprite_placement* p) {
    struct canvas* c = p->canvas;
    struct sprite_placement* n = p->prev;
    if (!n) return;
    struct sprite_placement* before = n->prev;
    struct sprite_placement* after = p->next;

    if (before) before->next = p; else c->bottom_layer = p;
    if (after) after->prev = n;   else c->top_layer = n;

    p->prev = before;
    p->next = n;
    n->prev = p;
    n->next = after;
}

void animate_placement_up(struct sprite_placement* sprite_placement)
{
    if (sprite_placement) swap_with_next(sprite_placement);
}

void animate_placement_down(struct sprite_placement* sprite_placement){
    if (sprite_placement) swap_with_prev(sprite_placement);
}

void animate_placement_top(struct sprite_placement* sprite_placement)
{
    if (!sprite_placement) return;
    while (sprite_placement->next) swap_with_next(sprite_placement);
}

void animate_placement_bottom(struct sprite_placement* sprite_placement)
{
    if (!sprite_placement) return;
    while (sprite_placement->prev) swap_with_prev(sprite_placement);
}

void animate_destroy_placement(struct sprite_placement* sprite_placement)
{
    if (!sprite_placement) return;
    struct canvas* c = sprite_placement->canvas;
    if (sprite_placement->prev) sprite_placement->prev->next = sprite_placement->next;
    else c->bottom_layer = sprite_placement->next;
    if (sprite_placement->next) sprite_placement->next->prev = sprite_placement->prev;
    else c->top_layer = sprite_placement->prev;
    sprite_unref(sprite_placement->sprite);
    free(sprite_placement);
}

void animate_set_animation_params(struct sprite_placement* sprite_placement, ssize_t vx, ssize_t vy, ssize_t ax, ssize_t ay)
{
    if (!sprite_placement) return;
    sprite_placement->vx = vx;
    sprite_placement->vy = vy;
    sprite_placement->ax = ax;
    sprite_placement->ay = ay;
}

void animate_set_animation_function(struct sprite_placement* sprite_placement, animate_fn fn, void* priv) 
{
    if (!sprite_placement) return;
    sprite_placement->custom_fn = fn;
    sprite_placement->priv = priv;
}

size_t animate_frame_size_bytes(struct canvas* canvas)
{
    if (!canvas) return 0;
    return canvas->height * canvas->width * sizeof(color_t);
}

static void draw_sprite(color_t* buf, size_t bw, size_t bh, struct sprite* s, ssize_t ox, ssize_t oy) {
    for (size_t y = 0; y < s->height; y++) {
        for (size_t x = 0; x < s->width; x++) {
            ssize_t fx = ox + (ssize_t)x;
            ssize_t fy = oy + (ssize_t)y;
            if (fx < 0 || fy < 0 || fx >= (ssize_t)bw || fy >= (ssize_t)bh) continue;
            color_t c = s->pixels[y * s->width + x];
            if ((c >> 24) & 0xFF) buf[fy * bw + fx] = (c & 0x00FFFFFF) | 0xFF000000;
        }
    }
}

void animate_generate_frame(const struct canvas* canvas, size_t frame, size_t frame_rate, void* buf) 
{
    if (!canvas || !buf) return;
    color_t* out = buf;
    size_t total = canvas->height * canvas->width;
    for (size_t i = 0; i < total; i++) out[i] = (canvas->background_color & 0x00FFFFFF) | 0xFF000000;

    float t = (float)frame / (float)frame_rate;
    struct sprite_placement* p = canvas->bottom_layer;
    while (p) {
        ssize_t cx = p->x;
        ssize_t cy = p->y;
        if (p->custom_fn) {
            p->custom_fn(p->priv, &cx, &cy, t);
        } else {
            cx = p->x + (ssize_t)(p->vx * t + 0.5f * p->ax * t * t);
            cy = p->y + (ssize_t)(p->vy * t + 0.5f * p->ay * t * t);
        }
        draw_sprite(out, canvas->width, canvas->height, p->sprite, cx, cy);
        p = p->next;
    }
}



// Accessor functions for generate command
size_t animate_get_canvas_width(struct canvas* canvas) {
    if (!canvas) return 0;
    return canvas->width;
}

size_t animate_get_canvas_height(struct canvas* canvas) {
    if (!canvas) return 0;
    return canvas->height;
}
