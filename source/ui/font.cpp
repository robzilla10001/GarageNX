// source/ui/font.cpp

#include "ui/font.hpp"
#include <SDL2/SDL.h>
#include <map>
#include <string>
#include <tuple>

namespace Font {

// ─── Cache key ────────────────────────────────────────────────────────────────
using CacheKey = std::tuple<int, Weight, Family>;

static std::map<CacheKey, TTF_Font*> s_cache;
static std::string s_regular_path;
static std::string s_bold_path;
static std::string s_mono_path;
static bool s_initialized = false;

// ─── Lifecycle ────────────────────────────────────────────────────────────────

bool init(const std::string& asset_root) {
    if (TTF_Init() != 0) {
        SDL_Log("Font::init — TTF_Init failed: %s", TTF_GetError());
        return false;
    }

    s_regular_path = asset_root + "/fonts/Inter-Regular.ttf";
    s_bold_path    = asset_root + "/fonts/Inter-Bold.ttf";
    s_mono_path    = asset_root + "/fonts/DejaVuSansMono.ttf";
    s_initialized  = true;

    // Pre-warm the most common sizes so the first frame doesn't stutter
    get(Size::Body,   Weight::Regular);
    get(Size::Body,   Weight::Bold);
    get(Size::Small,  Weight::Regular);
    get(Size::Medium, Weight::Bold);
    get(Size::Title,  Weight::Bold);
    get(Size::Small,  Weight::Regular, Family::Mono);

    return true;
}

void shutdown() {
    for (auto& [key, font] : s_cache) {
        if (font) TTF_CloseFont(font);
    }
    s_cache.clear();
    TTF_Quit();
    s_initialized = false;
}

// ─── Access ───────────────────────────────────────────────────────────────────

TTF_Font* get(Size size, Weight weight, Family family) {
    if (!s_initialized) {
        SDL_Log("Font::get — font system not initialized");
        return nullptr;
    }

    int pt = static_cast<int>(size);
    CacheKey key{pt, weight, family};

    auto it = s_cache.find(key);
    if (it != s_cache.end()) {
        return it->second;
    }

    std::string path;
    if (family == Family::Mono) {
        path = s_mono_path;   // DejaVu Sans Mono ships as a single weight
    } else {
        path = (weight == Weight::Bold) ? s_bold_path : s_regular_path;
    }

    TTF_Font* font = TTF_OpenFont(path.c_str(), pt);
    if (!font) {
        SDL_Log("Font::get — TTF_OpenFont(%s, %d) failed: %s",
                path.c_str(), pt, TTF_GetError());
        return nullptr;
    }

    TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
    s_cache[key] = font;
    return font;
}

SDL_Surface* render(const std::string& text, Size size, Weight weight,
                    SDL_Color color, Family family) {
    TTF_Font* font = get(size, weight, family);
    if (!font) return nullptr;
    return TTF_RenderUTF8_Blended(font, text.c_str(), color);
}

int mono_advance(Size size) {
    TTF_Font* f = get(size, Weight::Regular, Family::Mono);
    if (!f) return 0;
    int adv = 0;
    // All glyphs in a monospace font share the same advance; measure '0'.
    TTF_GlyphMetrics(f, '0', nullptr, nullptr, nullptr, nullptr, &adv);
    return adv;
}

} // namespace Font
