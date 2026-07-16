// source/ui/renderer.cpp

#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/font.hpp"

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

} // namespace Renderer
