#pragma once
// source/ui/layout.hpp
// Shared layout constants and region definitions.
// All screens reference these — no magic numbers in rendering code.

namespace Layout {

// ─── Screen regions (1280x720 logical) ───────────────────────────────────────

static constexpr int SCREEN_W = 1280;
static constexpr int SCREEN_H = 720;

static constexpr int TITLE_BAR_H  = 40;
static constexpr int STATUS_BAR_H = 40;

// Content area (between title bar and status bar)
static constexpr int CONTENT_Y = TITLE_BAR_H;
static constexpr int CONTENT_H = SCREEN_H - TITLE_BAR_H - STATUS_BAR_H;
static constexpr int CONTENT_W = SCREEN_W;

// Status bar Y position
static constexpr int STATUS_BAR_Y = SCREEN_H - STATUS_BAR_H;

// ─── Padding & spacing ────────────────────────────────────────────────────────

static constexpr int PAD_XS  = 4;
static constexpr int PAD_SM  = 8;
static constexpr int PAD_MD  = 16;
static constexpr int PAD_LG  = 24;
static constexpr int PAD_XL  = 32;

static constexpr int LIST_ROW_H      = 40;   // standard list row height
static constexpr int LIST_ROW_H_LG   = 52;   // large row (title list with metadata)
static constexpr int DIVIDER_H       = 1;

// ─── File browser column widths ───────────────────────────────────────────────
// Three-column ranger-style layout (default, non-split)

static constexpr int FB_COL1_W = 230;   // nav menu / breadcrumb (~18%)
static constexpr int FB_COL2_W = 512;   // directory listing    (~40%)
static constexpr int FB_COL3_W = 538;   // details / preview    (~42%)

static constexpr int FB_COL1_X = 0;
static constexpr int FB_COL2_X = FB_COL1_W;
static constexpr int FB_COL3_X = FB_COL1_W + FB_COL2_W;

// Split-view: col1 stays the same, col2 and col3 each get half the remainder
static constexpr int FB_SPLIT_HALF_W = (SCREEN_W - FB_COL1_W) / 2;  // 525
static constexpr int FB_SPLIT_SRC_X  = FB_COL1_W;
static constexpr int FB_SPLIT_DST_X  = FB_COL1_W + FB_SPLIT_HALF_W;

// ─── Main menu ────────────────────────────────────────────────────────────────

static constexpr int MENU_ITEM_H    = 52;
static constexpr int MENU_INDENT_X  = PAD_XL;

// ─── Modal dialog ─────────────────────────────────────────────────────────────

static constexpr int MODAL_W        = 640;
static constexpr int MODAL_MIN_H    = 180;
static constexpr int MODAL_X        = (SCREEN_W - MODAL_W) / 2;
static constexpr int MODAL_CORNER_R = 8;   // rounded corner radius (drawn manually)

// ─── Status bar segments ──────────────────────────────────────────────────────

static constexpr int STATUS_PAD_X   = PAD_MD;
static constexpr int BATTERY_BAR_W  = 36;
static constexpr int BATTERY_BAR_H  = 16;

} // namespace Layout
