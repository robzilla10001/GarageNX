// source/ui/renderer.cpp

#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/font.hpp"
#include "ui/text_cache.hpp"

#include <unordered_map>
#include <cstdint>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Renderer {

static SDL_Window*   s_window   = nullptr;
static SDL_Renderer* s_renderer = nullptr;
static bool          s_docked   = false;

// ─── Lifecycle ────────────────────────────────────────────────────────────────

bool init(const std::string& asset_root) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
        SDL_Log("Renderer::init — SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    // On Switch, SDL2 always creates a 1280x720 window; docked scaling is
    // handled transparently by the compositor. On PC we create a true window.
#ifdef PLATFORM_SWITCH
    s_window = SDL_CreateWindow(
        "GarageNX",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        BASE_WIDTH, BASE_HEIGHT,
        SDL_WINDOW_FULLSCREEN
    );
#else
    s_window = SDL_CreateWindow(
        "GarageNX",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        BASE_WIDTH, BASE_HEIGHT,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
#endif

    if (!s_window) {
        SDL_Log("Renderer::init — SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    s_renderer = SDL_CreateRenderer(
        s_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );

    if (!s_renderer) {
        SDL_Log("Renderer::init — SDL_CreateRenderer failed: %s", SDL_GetError());
        return false;
    }

    // Always address the framebuffer in logical 1280x720 coordinates.
    // On docked (1080p) the SDL compositor scales up automatically.
    SDL_RenderSetLogicalSize(s_renderer, BASE_WIDTH, BASE_HEIGHT);

    // Enable alpha blending globally
    SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_BLEND);

    // Font system
    if (!Font::init(asset_root)) {
        SDL_Log("Renderer::init — Font::init failed");
        return false;
    }

    SDL_Log("Renderer::init — OK (logical %dx%d)", BASE_WIDTH, BASE_HEIGHT);
    return true;
}

void shutdown() {
    Font::shutdown();

    // Textures belong to s_renderer — free them before it is destroyed.
    text_cache_clear();
    if (s_renderer) { SDL_DestroyRenderer(s_renderer); s_renderer = nullptr; }
    if (s_window)   { SDL_DestroyWindow(s_window);     s_window   = nullptr; }

    SDL_Quit();
}

// ─── Frame lifecycle ──────────────────────────────────────────────────────────

void begin_frame() {
    // Detect docked state each frame — user may dock/undock mid-session.
#ifdef PLATFORM_SWITCH
    AppletOperationMode mode = appletGetOperationMode();
    s_docked = (mode == AppletOperationMode_Console);
#else
    s_docked = false;
#endif

    Theme::apply(s_renderer, Theme::Token::BgBase);
    SDL_RenderClear(s_renderer);

    // Age out unused cached text once per frame.
    text_cache_advance_frame();
}

void end_frame() {
    SDL_RenderPresent(s_renderer);
}

// ─── Accessors ────────────────────────────────────────────────────────────────

SDL_Renderer* get()      { return s_renderer; }
SDL_Window*   window()   { return s_window;   }
bool          is_docked(){ return s_docked;   }

// ─── Drawing helpers ──────────────────────────────────────────────────────────

void fill_rect(int x, int y, int w, int h) {
    SDL_Rect r{x, y, w, h};
    SDL_RenderFillRect(s_renderer, &r);
}

void draw_rect(int x, int y, int w, int h) {
    SDL_Rect r{x, y, w, h};
    SDL_RenderDrawRect(s_renderer, &r);
}

void hline(int x, int y, int w) {
    SDL_RenderDrawLine(s_renderer, x, y, x + w - 1, y);
}

void blit(SDL_Surface* surface, int x, int y) {
    if (!surface) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(s_renderer, surface);
    if (!tex) return;

    SDL_Rect dst{x, y, surface->w, surface->h};
    SDL_RenderCopy(s_renderer, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
}

void blit_centered(SDL_Surface* surface, int x, int y, int w, int h) {
    if (!surface) return;
    int dx = x + (w - surface->w) / 2;
    int dy = y + (h - surface->h) / 2;
    blit(surface, dx, dy);
}

void blit_right(SDL_Surface* surface, int x, int y, int w, int h) {
    if (!surface) return;
    int dx = x + w - surface->w;
    int dy = y + (h - surface->h) / 2;
    blit(surface, dx, dy);
}

// ─── Cached text ──────────────────────────────────────────────────────────────

namespace {
struct CachedTex {
    SDL_Texture* tex = nullptr;
    int          w = 0, h = 0;
    uint64_t     last_used = 0;
};
std::unordered_map<TextKey, CachedTex, TextKeyHash> s_text_cache;
EvictionPolicy s_evict;
uint64_t       s_frame = 0;

// Look up or build the texture for these params. Returns nullptr on failure
// (e.g. empty string or a font/render error) — callers must tolerate that.
CachedTex* get_or_build(const std::string& text, int size, int weight,
                        int family, SDL_Color color) {
    if (text.empty()) return nullptr;

    TextKey key{text, size, weight, family,
                pack_color(color.r, color.g, color.b, color.a)};

    auto it = s_text_cache.find(key);
    if (it != s_text_cache.end()) {
        it->second.last_used = s_frame;
        return &it->second;
    }

    // Miss: rasterise once, upload once, keep the texture.
    SDL_Surface* surf = Font::render(text, (Font::Size)size, (Font::Weight)weight,
                                     color, (Font::Family)family);
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(s_renderer, surf);
    const int w = surf->w, h = surf->h;
    SDL_FreeSurface(surf);
    if (!tex) return nullptr;

    CachedTex& slot = s_text_cache[key];
    slot.tex = tex; slot.w = w; slot.h = h; slot.last_used = s_frame;
    return &slot;
}
} // namespace

void draw_text(const std::string& text, int size, int weight, int family,
               SDL_Color color, int x, int y, int* out_w, int* out_h, int clip_w) {
    CachedTex* c = get_or_build(text, size, weight, family, color);
    if (!c) { if (out_w) *out_w = 0; if (out_h) *out_h = 0; return; }
    // clip_w > 0 truncates the drawn width (left-aligned) without a separate
    // texture — the cached full-width texture is sampled with a source rect.
    const int draw_w = (clip_w > 0 && clip_w < c->w) ? clip_w : c->w;
    SDL_Rect src{0, 0, draw_w, c->h};
    SDL_Rect dst{x, y, draw_w, c->h};
    SDL_RenderCopy(s_renderer, c->tex, &src, &dst);
    if (out_w) *out_w = draw_w;
    if (out_h) *out_h = c->h;
}

void measure_text(const std::string& text, int size, int weight, int family,
                  SDL_Color color, int* out_w, int* out_h) {
    CachedTex* c = get_or_build(text, size, weight, family, color);
    if (out_w) *out_w = c ? c->w : 0;
    if (out_h) *out_h = c ? c->h : 0;
}

void text_cache_advance_frame() {
    ++s_frame;

    // Age out entries untouched for a while, so scrolling away from a screen
    // frees its text rather than pinning it forever.
    for (auto it = s_text_cache.begin(); it != s_text_cache.end(); ) {
        if (s_evict.is_stale(it->second.last_used, s_frame)) {
            SDL_DestroyTexture(it->second.tex);
            it = s_text_cache.erase(it);
        } else {
            ++it;
        }
    }

    // Hard cap: if still over capacity after aging, drop the oldest entries.
    if (s_evict.over_capacity(s_text_cache.size())) {
        // Collect by last_used and evict the coldest down to the cap. This runs
        // rarely (only when >max_entries distinct strings are live at once).
        while (s_text_cache.size() > s_evict.max_entries) {
            auto oldest = s_text_cache.begin();
            for (auto it = s_text_cache.begin(); it != s_text_cache.end(); ++it)
                if (it->second.last_used < oldest->second.last_used) oldest = it;
            SDL_DestroyTexture(oldest->second.tex);
            s_text_cache.erase(oldest);
        }
    }
}

size_t text_cache_size() { return s_text_cache.size(); }

void text_cache_clear() {
    for (auto& [k, c] : s_text_cache) SDL_DestroyTexture(c.tex);
    s_text_cache.clear();
}

} // namespace Renderer
