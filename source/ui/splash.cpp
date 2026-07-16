// source/ui/splash.cpp

#include "ui/splash.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

namespace Splash {
namespace {

// Clear to the menu's background colour, then draw the splash over it at the
// given alpha. At alpha 0 only the menu backdrop remains, so the fade ends
// exactly on the colour the main menu is about to paint.
void draw_frame(SDL_Renderer* r, SDL_Texture* tex, Uint8 alpha) {
    Theme::apply(r, Theme::Token::BgBase);
    SDL_RenderClear(r);
    SDL_SetTextureAlphaMod(tex, alpha);
    // The asset is authored at the full 1280x720 target, so a null dst rect
    // maps it 1:1 over the render target.
    SDL_RenderCopy(r, tex, nullptr, nullptr);
    SDL_RenderPresent(r);
}

// Returns true if the user asked to move things along.
bool poll_skip() {
    bool skip = false;
    SDL_Event e;
    // Keep pumping regardless: a window that stops responding for seconds
    // looks hung, and this is also how the skip is detected.
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT ||
            e.type == SDL_JOYBUTTONDOWN ||
            e.type == SDL_KEYDOWN) {
            skip = true;
        }
    }
    return skip;
}

} // namespace

void show(const std::string& asset_root, int hold_ms, int fade_ms) {
    SDL_Renderer* r = Renderer::get();
    if (!r || (hold_ms <= 0 && fade_ms <= 0)) return;

    // The image is bundled in RomFS on hardware (assets/bg -> romfs:/bg) and
    // read from ./assets on the PC stub; asset_root already encodes which.
    const std::string path = asset_root + "/bg/garageNX.png";

    SDL_Surface* surf = IMG_Load(path.c_str());
    if (!surf) {
        // Decorative only — log and carry on rather than holding up startup.
        SDL_Log("Splash: could not load %s (%s) — skipping", path.c_str(), IMG_GetError());
        return;
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
    SDL_FreeSurface(surf);
    if (!tex) {
        SDL_Log("Splash: CreateTextureFromSurface failed (%s) — skipping", SDL_GetError());
        return;
    }
    // Required for SDL_SetTextureAlphaMod to have any effect.
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    // ── Hold ──────────────────────────────────────────────────────────────────
    const Uint32 hold_start = SDL_GetTicks();
    while ((SDL_GetTicks() - hold_start) < (Uint32)hold_ms) {
        if (poll_skip()) break;   // skip to the fade, not straight to the menu
        draw_frame(r, tex, 255);
        SDL_Delay(16);            // ~60fps; keeps the loop off a busy core
    }

    // ── Fade out to the menu background ──────────────────────────────────────
    if (fade_ms > 0) {
        const Uint32 fade_start = SDL_GetTicks();
        for (;;) {
            const Uint32 elapsed = SDL_GetTicks() - fade_start;
            if (elapsed >= (Uint32)fade_ms) break;
            const float t = (float)elapsed / (float)fade_ms;   // 0 -> 1
            draw_frame(r, tex, (Uint8)(255.0f * (1.0f - t)));
            SDL_Delay(16);
        }
    }

    // Land on the exact backdrop the menu will paint, so its first frame does
    // not flash.
    Theme::apply(r, Theme::Token::BgBase);
    SDL_RenderClear(r);
    SDL_RenderPresent(r);

    SDL_DestroyTexture(tex);
}

} // namespace Splash
