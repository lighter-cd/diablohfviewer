// src/tools/ui_layout.h
//
// Stage 5 (goal 4): Diablo 1 / Hellfire GUI LAYOUT catalog — header-only, pure
// data + pure queries (no nlohmann/json, no engine, no decoders). This lets
// out/unit_tests.cpp exercise the whole catalog by simply #include-ing it.
//
// The catalog is a faithful transcription of DevilutionX's UI layout constants
// (Source/DiabloUI/*, Source/control/*, Source/inv.cpp, Source/panels/*,
// Source/stores.cpp). Coordinates are the game's 640x480 logical UI space:
//   * Rect/Element.rect are ABSOLUTE [x,y,w,h] in that 640x480 space.
//   * Element.panel + Element.panel_rect keep the ORIGINAL panel-relative values
//     where DevilutionX defines them relative to a UiPanels origin.
//   * MainPanel {0,352,640,128}, LeftPanel {0,0,320,352},
//     RightPanel {320,0,320,352}.
//
// Each element records the MPQ art it references (ArtRef), its text/flags, and
// the DevilutionX source location (source_ref). Rendering (ui_render.h) and the
// MCP tools (ui_tools.cpp) consume this catalog; this header never touches MPQ.

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace ui_layout {

// ---------------------------------------------------------------------------
// Basic geometry
// ---------------------------------------------------------------------------
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

inline bool validateRect(const Rect& r, int W = 640, int H = 480) {
    return r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 &&
           r.x + r.w <= W && r.y + r.h <= H;
}

// DevilutionX text/alignment rects may legitimately overflow the 640x480
// logical UI frame (e.g. selok's body rect is x=140,w=560 -> right edge 700).
// They are text layout boxes, clipped at render time. For catalog validation we
// only require a positive size and a top-left anchor inside the canvas. Pixel
// (art) boxes are still fully inside via validateRect at render time (ui_render
// clips anything beyond the canvas anyway).
inline bool validateRectAnchor(const Rect& r, int W = 640, int H = 480) {
    return r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 && r.x < W && r.y < H;
}

// ---------------------------------------------------------------------------
// Element types
// ---------------------------------------------------------------------------
enum class ElementType {
    Image,       // static art (background, panel, logo, icon)
    Button,      // clickable button (art + text)
    Text,        // text-only element
    Grid,        // repeated cells (inventory 10x4, belt 8, store 10x9)
    Slot,        // single equipment slot
    Orb,         // health / mana flask
    PanelRect,   // panel partition outline (no art)
    Scrollbar,   // scrollbar
    Progress,    // progress bar
};

inline const char* typeName(ElementType t) {
    switch (t) {
        case ElementType::Image:      return "image";
        case ElementType::Button:     return "button";
        case ElementType::Text:       return "text";
        case ElementType::Grid:       return "grid";
        case ElementType::Slot:       return "slot";
        case ElementType::Orb:        return "orb";
        case ElementType::PanelRect: return "panel_rect";
        case ElementType::Scrollbar: return "scrollbar";
        case ElementType::Progress:  return "progress";
    }
    return "image";
}

// RGBA (r<<24 | g<<16 | b<<8 | a) used for overlay boxes per element type.
inline uint32_t overlayColor(ElementType t) {
    switch (t) {
        case ElementType::Image:      return 0x00C8FFDCu; // cyan
        case ElementType::Button:     return 0xFFA028E6u; // orange
        case ElementType::Text:       return 0xC878FFDCu; // violet
        case ElementType::Grid:       return 0x50DC78C8u; // green
        case ElementType::Slot:       return 0x50DC78C8u; // green
        case ElementType::Orb:        return 0xFF5050E6u; // red
        case ElementType::PanelRect: return 0xC8C8C88Cu; // gray (dashed)
        case ElementType::Scrollbar: return 0x5A96FFC8u; // blue
        case ElementType::Progress:  return 0x5A96FFC8u; // blue
    }
    return 0x00C8FFDCu;
}

inline uint8_t overlayR(uint32_t c) { return static_cast<uint8_t>(c >> 24); }
inline uint8_t overlayG(uint32_t c) { return static_cast<uint8_t>(c >> 16); }
inline uint8_t overlayB(uint32_t c) { return static_cast<uint8_t>(c >> 8); }
inline uint8_t overlayA(uint32_t c) { return static_cast<uint8_t>(c); }

// ---------------------------------------------------------------------------
// Art reference (an MPQ file + how to decode it)
// ---------------------------------------------------------------------------
struct ArtRef {
    std::string name;               // MPQ path, e.g. "ui_art\\mainmenu.pcx"
    std::string format;             // "pcx" | "cel" | "cl2" | "clx"
    int  width        = 0;          // CEL/CL2 frame width (0 = unknown -> needs_width)
    int  frame        = 0;          // which frame to blit
    int  frames       = 1;          // total frames in the file (catalog record)
    std::string palette;            // "embedded" (PCX) | "ui_art\\diablo.pal"
    std::string archive_hint;       // "DIABDAT.MPQ" | "devilutionx.mpq" (hint only)
    int  transparent_index = -1;    // PCX transparent palette index (-1 = none)
};

struct Flags {
    bool align_center    = false;
    bool align_right     = false;
    bool vertical_center = false;
};

// ---------------------------------------------------------------------------
// An element of one screen
// ---------------------------------------------------------------------------
struct Element {
    std::string id;
    ElementType type    = ElementType::Image;
    std::string label;              // Chinese description (frontend detail)
    Rect  rect;                      // ABSOLUTE 640x480 logical [x,y,w,h]
    std::string panel;              // owning panel id, "" = full screen
    Rect  panel_rect;               // panel-relative [x,y,w,h] (transcription)
    int   z = 0;                    // draw order (ascending)

    bool   has_art       = false;
    ArtRef art;

    bool   size_from_art = false;    // true: ui.build overwrites rect.w/h with decoded size

    // Grid/Slot repetition (cols x rows, cell size, step). 0 = not a grid.
    int grid_cols = 0, grid_rows = 0, cell_w = 0, cell_h = 0, step_x = 0, step_y = 0;

    // Text (type == Text or Button). Dynamic values use {PLACEHOLDER} tokens.
    std::string text;
    int  font_size = 0;             // UiFlags: 0 | 12 | 22 | 24 | 30 | 42 | 46
    std::string color;              // fonts\\*.trn basename: goldui/white/...
    std::string align;              // "left" | "center" | "right"
    Flags flags;
    std::string source_ref;         // DevilutionX source location
};

struct Panel {
    std::string id;                 // "MainPanel" | "LeftPanel" | "RightPanel"
    Rect  rect;                      // ABSOLUTE 640x480
    std::string note;
};

struct Screen {
    std::string id;
    std::string title;
    std::string category;           // menu|hud|panel|store|dialog
    int canvas_w = 640, canvas_h = 480;
    std::string palette;            // screen default; ArtRef.palette is authoritative
    std::vector<Panel>   panels;
    std::vector<Element> elements;
    std::string source_ref;
};

// ---------------------------------------------------------------------------
// Compact builders (used only inside buildScreens())
// ---------------------------------------------------------------------------
namespace detail {

// Two distinct 256-colour palettes are in play (verified against the real MPQs):
//  * MENU_PAL  "ui_art\\diablo.pal" (devilutionx.mpq): low indices are pure
//              primaries -> correct for DevilutionX-converted menu/dialog .clx,
//              WRONG (rainbow) for original in-game .cel.
//  * GAME_PAL  "levels\\towndata\\town.pal" (DIABDAT.MPQ): the in-game palette
//              loaded by LoadRndLvlPal(DTYPE_TOWN); all in-game HUD/panel .cel
//              and .clx are drawn with it (0% rainbow).
// PCX files carry an embedded palette and use "embedded".
const char* const MENU_PAL = "ui_art\\diablo.pal";
const char* const GAME_PAL = "levels\\towndata\\town.pal";

inline ArtRef pcx(const std::string& name, const std::string& hint = "DIABDAT.MPQ",
                  int transparent = -1, int frames = 1, int frame = 0) {
    ArtRef a; a.name = name; a.format = "pcx"; a.palette = "embedded";
    a.archive_hint = hint; a.transparent_index = transparent;
    a.frames = frames; a.frame = frame;
    return a;
}
inline ArtRef cel(const std::string& name, int width, const std::string& hint = "DIABDAT.MPQ",
                  int frames = 1, int frame = 0) {
    ArtRef a; a.name = name; a.format = "cel"; a.width = width;
    a.palette = GAME_PAL; a.archive_hint = hint;
    a.frames = frames; a.frame = frame;
    return a;
}
inline ArtRef clx(const std::string& name, const std::string& hint = "devilutionx.mpq",
                  int frames = 1, int frame = 0, const std::string& palette = MENU_PAL) {
    ArtRef a; a.name = name; a.format = "clx";
    a.palette = palette; a.archive_hint = hint;
    a.frames = frames; a.frame = frame;
    return a;
}

inline Element el(const std::string& id, ElementType t, const std::string& label,
                  const Rect& r, const std::string& src) {
    Element e; e.id = id; e.type = t; e.label = label; e.rect = r; e.source_ref = src;
    return e;
}
inline Element img(const std::string& id, const std::string& label,
                    const Rect& r, const ArtRef& a, int z, const std::string& src,
                    const std::string& panel = "", const Rect& pr = Rect{},
                    bool sizeFromArt = false) {
    Element e = el(id, ElementType::Image, label, r, src);
    e.z = z; e.has_art = true; e.art = a; e.size_from_art = sizeFromArt;
    e.panel = panel; e.panel_rect = pr;
    return e;
}
inline Element txt(const std::string& id, const std::string& label,
                   const Rect& r, const std::string& text, int font,
                   const std::string& color, const std::string& align,
                   int z, const std::string& src,
                   const std::string& panel = "", const Rect& pr = Rect{}) {
    Element e = el(id, ElementType::Text, label, r, src);
    e.z = z; e.text = text; e.font_size = font; e.color = color; e.align = align;
    e.flags.align_center = (align == "center");
    e.flags.align_right  = (align == "right");
    e.panel = panel; e.panel_rect = pr;
    return e;
}
inline Element btn(const std::string& id, const std::string& label,
                   const Rect& r, const std::string& text, int z,
                   const std::string& src, const ArtRef* a = nullptr) {
    Element e = el(id, ElementType::Button, label, r, src);
    e.z = z; e.text = text; e.font_size = 22; e.color = "whitegold";
    e.align = "center"; e.flags.align_center = true; e.flags.vertical_center = true;
    if (a) { e.has_art = true; e.art = *a; }
    return e;
}
inline Element orb(const std::string& id, const std::string& label,
                    const Rect& r, const ArtRef& a, int z, const std::string& src) {
    Element e = el(id, ElementType::Orb, label, r, src);
    e.z = z; e.has_art = true; e.art = a; e.panel = "MainPanel";
    return e;
}
inline Element grid(const std::string& id, const std::string& label,
                    const Rect& r, int cols, int rows, int cw, int ch,
                    int sx, int sy, int z, const std::string& src,
                    const std::string& panel = "", const Rect& pr = Rect{}) {
    Element e = el(id, ElementType::Grid, label, r, src);
    e.z = z; e.grid_cols = cols; e.grid_rows = rows; e.cell_w = cw; e.cell_h = ch;
    e.step_x = sx; e.step_y = sy; e.panel = panel; e.panel_rect = pr;
    return e;
}

} // namespace detail

// ---------------------------------------------------------------------------
// The catalog — 18 screens (menu 8 / hud 1 / panel 4 / store 2 / dialog 3)
// ---------------------------------------------------------------------------
inline std::vector<Screen> buildScreens() {
    using namespace detail;
    std::vector<Screen> v;
    const Rect MP{0, 352, 640, 128};
    const Rect LP{0, 0, 320, 352};
    const Rect RP{320, 0, 320, 352};

    // ---- menu: mainmenu ---------------------------------------------------
    {
        Screen s; s.id = "mainmenu"; s.title = "主菜单"; s.category = "menu";
        s.palette = "embedded";
        s.source_ref = "Source/DiabloUI/mainmenu.cpp; diabloui.cpp UiAddBackground/UiAddLogo";
        s.elements.push_back(img("mm.bg", "主菜单背景", {0, 0, 640, 480},
                                 pcx("ui_art\\mainmenu.pcx"), 0, "mainmenu.cpp:64"));
        const char* items[5] = {"Single Player", "Multi Player", "Replay Intro",
                                "Show Credits", "Exit Diablo"};
        for (int i = 0; i < 5; ++i) {
            s.elements.push_back(txt("mm.item" + std::to_string(i),
                "菜单项 " + std::to_string(i + 1), {64, 192 + i * 43, 510, 43},
                items[i], 42, "goldui", "center", 2, "mainmenu.cpp:79 UiList"));
        }
        s.elements.push_back(txt("mm.version", "版本信息行", {17, 444, 605, 21},
            "{VERSION}", 12, "grayuis", "left", 2, "mainmenu.cpp:81-82"));
        v.push_back(std::move(s));
    }

    // ---- menu: title ------------------------------------------------------
    {
        Screen s; s.id = "title"; s.title = "标题画面 / Logo"; s.category = "menu";
        s.palette = "embedded";
        s.source_ref = "Source/DiabloUI/title.cpp";
        s.elements.push_back(img("ti.bg", "标题背景", {0, 0, 640, 480},
                                 pcx("ui_art\\title.pcx"), 0, "title.cpp:42"));
        s.elements.push_back(img("ti.logo", "Diablo 旋转 Logo（DevilutionX 15 帧列表，取首帧）",
                                 {0, 182, 640, 100},
                                 pcx("ui_art\\logo.pcx", "DIABDAT.MPQ", 250, 1), 1,
                                 "title.cpp:43 LoadPcxSpriteList(logo,15)", "", Rect{}, true));
        s.elements.push_back(txt("ti.copyright", "版权行", {0, 410, 640, 26},
            "(C) 1996-2001 Blizzard Entertainment", 24, "grayui", "center", 2,
            "title.cpp:73-74"));
        v.push_back(std::move(s));
    }

    // ---- menu: credits ----------------------------------------------------
    {
        Screen s; s.id = "credits"; s.title = "制作人员名单"; s.category = "menu";
        s.palette = "embedded";
        s.source_ref = "Source/DiabloUI/credits.cpp";
        s.elements.push_back(img("cr.bg", "名单背景", {0, 0, 640, 480},
                                 pcx("ui_art\\credits.pcx"), 0, "credits.cpp:190"));
        s.elements.push_back(txt("cr.viewport", "滚动文字视口", {31, 114, 580, 251},
            "{CREDITS_LINES}", 24, "grayui", "center", 1, "credits.cpp:41 VIEWPORT"));
        v.push_back(std::move(s));
    }

    // ---- menu: selhero ----------------------------------------------------
    {
        Screen s; s.id = "selhero"; s.title = "选择英雄"; s.category = "menu";
        s.palette = "embedded";
        s.source_ref = "Source/DiabloUI/hero/selhero.cpp; scrollbar.cpp";
        s.elements.push_back(img("sh.bg", "选择英雄背景", {0, 0, 640, 480},
                                 pcx("ui_art\\selhero.pcx"), 0, "selhero.cpp:284"));
        s.elements.push_back(txt("sh.title", "标题", {24, 161, 590, 35},
            "Choose Class", 30, "grayui", "center", 1, "selhero.cpp:172"));
        s.elements.push_back(txt("sh.hero_label", "英雄属性标题", {242, 211, 365, 33},
            "Select Hero", 30, "grayui", "center", 1, "selhero.cpp:498"));
        s.elements.push_back(img("sh.portrait", "英雄头像（列表，取首帧）", {30, 211, 180, 76},
                                 pcx("ui_art\\heros.pcx", "DIABDAT.MPQ", -1, 1), 1,
                                 "diabloui.cpp:629 ArtHero"));
        const char* stats[5] = {"Level", "Strength", "Magic", "Dexterity", "Vitality"};
        for (int i = 0; i < 5; ++i) {
            s.elements.push_back(txt("sh.stat" + std::to_string(i),
                std::string("属性 ") + stats[i], {39, 323 + i * 21, 110, 21},
                stats[i], 24, "grayui", "left", 1, "selhero.cpp:513-526"));
        }
        s.elements.push_back(grid("sh.list", "英雄列表（6 行）", {265, 256, 320, 156},
                                  1, 6, 320, 26, 0, 26, 2, "selhero.cpp:537-562"));
        { Element sc = el("sh.scroll", ElementType::Scrollbar, "滚动条",
                          {585, 244, 25, 178}, "selhero.cpp:550");
          sc.z = 2; s.elements.push_back(std::move(sc)); }
        s.elements.push_back(btn("sh.ok", "OK 按钮", {239, 429, 120, 35}, "OK", 2,
                                 "selhero.cpp:203"));
        s.elements.push_back(btn("sh.delete", "Delete 按钮", {364, 429, 120, 35},
                                 "Delete", 2, "selhero.cpp:206"));
        s.elements.push_back(btn("sh.cancel", "Cancel 按钮", {489, 429, 144, 35},
                                 "Cancel", 2, "selhero.cpp:206"));
        v.push_back(std::move(s));
    }

    // ---- menu: selgame (difficulty) --------------------------------------
    {
        Screen s; s.id = "selgame"; s.title = "选择难度 / 游戏"; s.category = "menu";
        s.palette = "embedded";
        s.source_ref = "Source/DiabloUI/hero/selhero.cpp; scrollbar.cpp";
        s.elements.push_back(img("sg.bg", "难度选择背景", {0, 0, 640, 480},
                                 pcx("ui_art\\selgame.pcx"), 0, "selhero.cpp:387"));
        s.elements.push_back(txt("sg.title", "标题", {24, 161, 590, 35},
            "Select Difficulty", 30, "grayui", "center", 1, "selhero.cpp:387"));
        s.elements.push_back(grid("sg.list", "难度列表（3 行）", {265, 256, 320, 99},
                                  1, 3, 320, 33, 0, 33, 2, "selhero.cpp"));
        s.elements.push_back(btn("sg.ok", "OK 按钮", {279, 429, 140, 35}, "OK", 2,
                                 "selhero.cpp:314"));
        s.elements.push_back(btn("sg.cancel", "Cancel 按钮", {429, 429, 144, 35},
                                 "Cancel", 2, "selhero.cpp:317"));
        v.push_back(std::move(s));
    }

    // ---- menu: selok ------------------------------------------------------
    {
        Screen s; s.id = "selok"; s.title = "OK 确认对话框"; s.category = "menu";
        s.palette = "embedded";
        s.source_ref = "Source/DiabloUI/selok.cpp";
        s.elements.push_back(img("so.bg", "背景", {0, 0, 640, 480},
                                 pcx("ui_art\\mainmenu.pcx"), 0, "selok.cpp:64"));
        s.elements.push_back(txt("so.title", "标题", {24, 161, 590, 35},
            "{TITLE}", 30, "grayui", "center", 2, "selok.cpp:76-77"));
        s.elements.push_back(txt("so.body", "正文", {140, 210, 560, 168},
            "{BODY}", 24, "grayui", "left", 2, "selok.cpp:79-80"));
        s.elements.push_back(btn("so.ok", "OK 列表项", {230, 390, 180, 35}, "OK", 2,
                                 "selok.cpp:87"));
        v.push_back(std::move(s));
    }

    // ---- menu: selyesno --------------------------------------------------
    {
        Screen s; s.id = "selyesno"; s.title = "Yes / No 对话框"; s.category = "menu";
        s.palette = "embedded";
        s.source_ref = "Source/DiabloUI/selyesno.cpp";
        { Element bg = el("sy.bg", ElementType::PanelRect, "黑色背景",
                          {0, 0, 640, 480}, "selyesno.cpp UiLoadBlackBackground");
          s.elements.push_back(std::move(bg)); }
        s.elements.push_back(txt("sy.title", "标题", {24, 161, 590, 35},
            "{TITLE}", 30, "grayui", "center", 2, "selyesno.cpp:66-67"));
        s.elements.push_back(txt("sy.body", "正文", {120, 236, 400, 168},
            "{BODY}", 24, "grayui", "left", 2, "selyesno.cpp:69-70"));
        s.elements.push_back(btn("sy.yes", "Yes 列表项", {230, 390, 90, 35}, "Yes", 2,
                                 "selyesno.cpp:74"));
        s.elements.push_back(btn("sy.no", "No 列表项", {320, 390, 90, 35}, "No", 2,
                                 "selyesno.cpp:74"));
        v.push_back(std::move(s));
    }

    // ---- menu: settings --------------------------------------------------
    {
        Screen s; s.id = "settings"; s.title = "设置菜单"; s.category = "menu";
        s.palette = "embedded";
        s.source_ref = "Source/DiabloUI/settingsmenu.cpp";
        { Element bg = el("se.bg", ElementType::PanelRect, "黑色背景",
                          {0, 0, 640, 480}, "settingsmenu.cpp UiLoadBlackBackground");
          s.elements.push_back(std::move(bg)); }
        s.elements.push_back(txt("se.title", "标题", {0, 161, 640, 35},
            "Settings", 30, "grayui", "center", 2, "settingsmenu.cpp:401"));
        s.elements.push_back(grid("se.list", "设置项列表", {50, 204, 540, 208},
                                  1, 8, 540, 26, 0, 26, 2, "settingsmenu.cpp:557-567"));
        s.elements.push_back(txt("se.desc", "选项说明", {24, 412, 590, 64},
            "{DESCRIPTION}", 24, "grayui", "left", 2, "settingsmenu.cpp:564"));
        { Element sc = el("se.scroll", ElementType::Scrollbar, "滚动条",
                          {595, 204, 25, 208}, "settingsmenu.cpp:563");
          sc.z = 2; s.elements.push_back(std::move(sc)); }
        v.push_back(std::move(s));
    }

    // ---- hud: control_panel (blood/mana orbs) ----------------------------
    {
        Screen s; s.id = "control_panel"; s.title = "底部控制面板 / 血蓝球";
        s.category = "hud"; s.palette = "ui_art\\diablo.pal";
        s.source_ref = "Source/control/control_panel.cpp; control_flasks.cpp; mainpanel.cpp";
        s.panels.push_back(Panel{"MainPanel", MP, "UISize 分区 (0,352,640,128)"});

        s.elements.push_back(img("cp.bg", "控制面板底图", MP,
            cel("ctrlpan\\panel8.cel", 640), 0, "control_panel.cpp:367",
            "MainPanel", Rect{0, 0, 640, 128}));
        s.elements.push_back(img("cp.info_box", "信息框底", {177, 398, 288, 64},
            cel("data\\textbox2.cel", 271), 1, "control.hpp:40 InfoBoxRect",
            "MainPanel", Rect{177, 46, 288, 64}));
        s.elements.push_back(orb("cp.health_orb", "血球（p8bulbs 帧0）", {96, 360, 88, 88},
            cel("ctrlpan\\p8bulbs.cel", 88, "DIABDAT.MPQ", 2, 0), 2,
            "control_flasks.cpp:104 LifeFlask X 107/96"));
        s.elements.push_back(orb("cp.mana_orb", "蓝球（p8bulbs 帧1）", {464, 360, 88, 88},
            cel("ctrlpan\\p8bulbs.cel", 88, "DIABDAT.MPQ", 2, 1), 2,
            "control_flasks.cpp:110 ManaFlask X 475/464"));
        s.elements.push_back(txt("cp.health_text", "生命数值", {134, 380, 88, 20},
            "{HP}/{HP_MAX}", 12, "white", "center", 3, "scrollrt.cpp:1896",
            "MainPanel", Rect{134, 28, 88, 20}));
        s.elements.push_back(txt("cp.mana_text", "法力数值", {502, 380, 88, 20},
            "{MANA}/{MANA_MAX}", 12, "white", "center", 3, "scrollrt.cpp:1898",
            "MainPanel", Rect{502, 28, 88, 20}));
        s.elements.push_back(grid("cp.belt", "腰带 8 格", {205, 357, 232, 28},
            8, 1, 29, 28, 29, 0, 3, "control_panel.cpp:101 BeltRect",
            "MainPanel", Rect{205, 5, 232, 28}));
        // Wide panel buttons (relative MainPanel origin 0,352).
        const int bx[6] = {9, 9, 9, 9, 560, 560};
        const int by[6] = {9, 35, 75, 101, 9, 35};
        const char* bn[6] = {"CHAR", "QUESTS", "MAP", "MENU", "INV", "SPELLS"};
        for (int i = 0; i < 6; ++i) {
            s.elements.push_back(btn("cp.btn" + std::to_string(i),
                std::string("面板按钮 ") + bn[i], {bx[i], 352 + by[i], 71, 20},
                bn[i], 3, "control_panel.cpp:86-91 MainPanelButtonRect"));
        }
        // small multiplayer buttons
        s.elements.push_back(btn("cp.chat", "聊天按钮", {87, 443, 33, 32}, "CHAT", 3,
                                 "control_panel.cpp:92"));
        s.elements.push_back(btn("cp.friendly", "友军火力按钮", {527, 443, 33, 32},
                                 "FF", 3, "control_panel.cpp:93"));
        // current spell button + spell icon (spelicon width unknown -> needs_width)
        s.elements.push_back(btn("cp.spell_btn", "当前法术按钮", {565, 416, 56, 56},
                                 "{SPELL}", 3, "control_panel.cpp:103 SpellButtonRect"));
        { Element si = img("cp.spell_icon", "法术图标（spelicon 56×56）",
                           {565, 416, 56, 56}, cel("ctrlpan\\spelicon.cel", 56), 4,
                           "control_panel.cpp:406");
          s.elements.push_back(std::move(si)); }
        v.push_back(std::move(s));
    }

    // ---- panel: char_panel (Stats) ---------------------------------------
    {
        Screen s; s.id = "char_panel"; s.title = "角色属性面板"; s.category = "panel";
        s.palette = "ui_art\\diablo.pal";
        s.source_ref = "Source/panels/charpanel.cpp; Source/control/control_panel.cpp";
        s.panels.push_back(Panel{"LeftPanel", LP, "UISize 分区 (0,0,320,352)"});
        s.elements.push_back(img("ch.bg", "角色面板底图", LP,
            clx("data\\charbg.clx", "devilutionx.mpq", 1, 0, GAME_PAL), 0,
            "charpanel.cpp:274", "LeftPanel", Rect{0, 0, 320, 352}));
        s.elements.push_back(txt("ch.name", "玩家名", {9, 14, 150, 24},
            "{PLAYER_NAME}", 12, "white", "left", 2, "charpanel.cpp:127"));
        s.elements.push_back(txt("ch.class", "职业名", {161, 14, 149, 24},
            "{CLASS}", 12, "white", "left", 2, "charpanel.cpp:128"));
        const char* labels[4] = {"Strength", "Magic", "Dexterity", "Vitality"};
        for (int i = 0; i < 4; ++i) {
            s.elements.push_back(txt("ch.attr" + std::to_string(i),
                std::string("属性 ") + labels[i], {88, 135 + i * 28, 45, 24},
                labels[i], 24, "white", "left", 2, "charpanel.cpp:135-191"));
            s.elements.push_back(txt("ch.val" + std::to_string(i),
                std::string("属性值 ") + labels[i], {135, 135 + i * 28, 76, 24},
                "{VAL}", 24, "white", "left", 2, "charpanel.cpp:135-191"));
        }
        s.elements.push_back(txt("ch.gold", "金币", {211, 127, 99, 24},
            "{GOLD}", 24, "white", "left", 2, "charpanel.cpp Gold"));
        s.elements.push_back(txt("ch.life", "生命", {88, 284, 45, 24},
            "Life", 24, "white", "left", 2, "charpanel.cpp:284"));
        s.elements.push_back(txt("ch.mana", "法力", {88, 312, 45, 24},
            "Mana", 24, "white", "left", 2, "charpanel.cpp:312"));
        for (int i = 0; i < 4; ++i) {
            Element b = btn("ch.plus" + std::to_string(i), "属性 +按钮",
                {137, 138 + i * 29, 41, 22}, "+", 3, "control_panel.cpp:74-79");
            b.has_art = true; b.art = cel("data\\charbut.cel", 41);
            s.elements.push_back(std::move(b));
        }
        v.push_back(std::move(s));
    }

    // ---- panel: inventory (equipment + 10x4 backpack) -------------------
    {
        Screen s; s.id = "inventory"; s.title = "物品栏 / 装备槽"; s.category = "panel";
        s.palette = "ui_art\\diablo.pal";
        s.source_ref = "Source/inv.cpp InvRect[]; control_panel.cpp BeltRect";
        s.panels.push_back(Panel{"RightPanel", RP, "UISize 分区 (320,0,320,352)"});
        s.elements.push_back(img("in.bg", "物品栏底图（按职业）", RP,
            cel("data\\inv\\inv.cel", 320), 0, "inv.cpp:1181 LoadCel(class)",
            "RightPanel", Rect{0, 0, 320, 352}));
        // equipment slots (InvRect relative to RightPanel origin 320,0)
        struct SlotDef { const char* id; const char* label; int x, y, w, h; };
        const SlotDef slots[] = {
            {"in.head", "头盔", 132, 2, 58, 59}, {"in.ringL", "左戒指", 47, 177, 28, 29},
            {"in.ringR", "右戒指", 248, 177, 28, 29}, {"in.amulet", "护符", 205, 32, 28, 29},
            {"in.handL", "左手", 17, 75, 58, 86}, {"in.handR", "右手", 248, 75, 58, 87},
            {"in.chest", "胸甲", 132, 75, 58, 87},
        };
        for (const auto& d : slots) {
            Element e = el(d.id, ElementType::Slot, d.label,
                           {320 + d.x, d.y, d.w, d.h}, "inv.cpp:79-138 InvRect[]");
            e.z = 2; e.panel = "RightPanel"; e.panel_rect = Rect{d.x, d.y, d.w, d.h};
            s.elements.push_back(std::move(e));
        }
        s.elements.push_back(grid("in.pack", "背包 10x4", {320 + 17, 222, 290, 116},
            10, 4, 29, 29, 29, 29, 2, "inv.cpp:7-16 InvRect[7..46]",
            "RightPanel", Rect{17, 222, 290, 116}));
        s.elements.push_back(grid("in.belt", "腰带 8 格", {205, 357, 232, 28},
            8, 1, 29, 28, 29, 0, 2, "inv.cpp DrawInvBelt",
            "MainPanel", Rect{205, 5, 232, 28}));
        v.push_back(std::move(s));
    }

    // ---- panel: spellbook ------------------------------------------------
    {
        Screen s; s.id = "spellbook"; s.title = "法术书"; s.category = "panel";
        s.palette = "ui_art\\diablo.pal";
        s.source_ref = "Source/panels/spell_book.cpp; spell_icons.cpp";
        s.panels.push_back(Panel{"RightPanel", RP, "UISize 分区 (320,0,320,352)"});
        s.elements.push_back(img("sb.bg", "法术书底图", RP,
            cel("data\\spellbk.cel", 320), 0, "spell_book.cpp:123", "RightPanel",
            Rect{0, 0, 320, 352}));
        s.elements.push_back(img("sb.tabs", "法术书页签", {320, 0, 320, 40},
            cel("data\\spellbkb.cel", 76), 1, "spell_book.cpp:124"));
        s.elements.push_back(grid("sb.icons", "法术图标网格（4x3）", {337, 60, 296, 222},
            4, 3, 56, 56, 74, 74, 2, "spell_icons.cpp SPLICONLENGTH=56",
            "RightPanel", Rect{17, 60, 296, 222}));
        s.elements.push_back(img("sb.icon", "示例法术图标", {337, 60, 56, 56},
            cel("data\\spelli2.cel", 37), 3, "spell_icons.cpp:117 spelli2"));
        v.push_back(std::move(s));
    }

    // ---- panel: quest_log ------------------------------------------------
    {
        Screen s; s.id = "quest_log"; s.title = "任务日志"; s.category = "panel";
        s.palette = "ui_art\\diablo.pal";
        s.source_ref = "Source/control/control_panel.cpp; Source/quests.cpp";
        s.panels.push_back(Panel{"LeftPanel", LP, "UISize 分区 (0,0,320,352)"});
        s.elements.push_back(img("ql.bg", "任务日志底图", LP,
            cel("data\\quest.cel", 320), 0, "control_panel.cpp:421", "LeftPanel",
            Rect{0, 0, 320, 352}));
        s.elements.push_back(txt("ql.title", "任务标题", {20, 20, 280, 30},
            "{QUEST_TITLE}", 30, "goldui", "center", 2, "quests.cpp"));
        s.elements.push_back(grid("ql.lines", "任务文本行", {20, 60, 280, 270},
            1, 10, 280, 26, 0, 27, 2, "quests.cpp"));
        v.push_back(std::move(s));
    }

    // ---- store: store_list (classic text store) --------------------------
    {
        Screen s; s.id = "store_list"; s.title = "商店（经典文本列表）"; s.category = "store";
        s.palette = "ui_art\\diablo.pal";
        s.source_ref = "Source/stores.cpp; Source/panels/info_box.cpp";
        { Element bg = el("sl.dark", ElementType::PanelRect, "半透明暗底",
                          {347, 28, 265, 297}, "stores.cpp:274");
          bg.z = 0; s.elements.push_back(std::move(bg)); }
        s.elements.push_back(img("sl.bg", "商店文本框", {344, 327, 271, 100},
            cel("data\\textbox2.cel", 271), 1, "stores.cpp:272-273 pSTextBoxCels"));
        s.elements.push_back(grid("sl.items", "商品列表行", {346, 60, 255, 264},
            1, 20, 255, 12, 0, 13, 2, "stores.cpp:214 PaddingTop=32"));
        { Element sc = el("sl.scroll", ElementType::Scrollbar, "滚动条",
                          {601, 44, 12, 297}, "stores.cpp:277-303");
          sc.z = 2;
          sc.has_art = true; sc.art = cel("data\\textslid.cel", 12, "DIABDAT.MPQ", 14, 8);
          s.elements.push_back(std::move(sc)); }
        s.elements.push_back(txt("sl.gold", "持有金币", {346, 28, 255, 21},
            "Your gold: {GOLD}", 12, "whitegold", "right", 2, "stores.cpp:2473"));
        v.push_back(std::move(s));
    }

    // ---- store: store_grid (QoL grid store) ------------------------------
    {
        Screen s; s.id = "store_grid"; s.title = "商店（网格视图）"; s.category = "store";
        s.palette = "ui_art\\diablo.pal";
        s.source_ref = "Source/qol/visual_store.cpp; data\\store.clx";
        s.panels.push_back(Panel{"LeftPanel", LP, "UISize 分区 (0,0,320,352)"});
        s.elements.push_back(img("sgr.bg", "网格商店底图", LP,
            clx("data\\stash.clx", "devilutionx.mpq", 1, 0, GAME_PAL), 0,
            "visual_store.cpp:428 (store.clx 未恢复，用 stash 底图)",
            "LeftPanel", Rect{0, 0, 320, 352}));
        s.elements.push_back(grid("sgr.grid", "商品网格 10x9", {17, 44, 290, 261},
            10, 9, 29, 29, 29, 29, 2, "visual_store.cpp:390 GetVisualStoreSlotCoord",
            "LeftPanel", Rect{17, 44, 290, 261}));
        s.elements.push_back(btn("sgr.tab_basic", "Basic 页签", {14, 21, 72, 22},
                                  "Basic", 3, "visual_store.cpp:57"));
        s.elements.push_back(btn("sgr.tab_premium", "Premium 页签", {87, 21, 72, 22},
                                  "Premium", 3, "visual_store.cpp:58"));
        s.elements.push_back(btn("sgr.repair_all", "全部修理", {233, 315, 48, 24},
                                  "ALL", 3, "visual_store.cpp:59"));
        s.elements.push_back(btn("sgr.repair_one", "单个修理", {286, 315, 24, 24},
                                  "1", 3, "visual_store.cpp:60"));
        s.elements.push_back(txt("sgr.gold", "持有金币", {20, 320, 280, 20},
            "Gold: {GOLD}", 12, "whitegold", "left", 3, "visual_store.cpp:502"));
        v.push_back(std::move(s));
    }

    // ---- dialog: dialog_small (spopup) -----------------------------------
    {
        Screen s; s.id = "dialog_small"; s.title = "小对话框（spopup）"; s.category = "dialog";
        s.palette = "embedded";
        s.source_ref = "Source/DiabloUI/dialogs.cpp; button.cpp";
        s.elements.push_back(img("ds.bg", "小弹窗背景", {180, 168, 284, 148},
            pcx("ui_art\\spopup.pcx"), 0, "dialogs.cpp:63 spopup"));
        s.elements.push_back(txt("ds.body", "正文", {200, 211, 240, 80},
            "{BODY}", 12, "white", "center", 1, "dialogs.cpp:105"));
        { Element b = btn("ds.ok", "OK 按钮", {265, 265, 110, 28}, "OK", 2,
                          "dialogs.cpp:108");
          b.has_art = true; b.art = clx("ui_art\\dvl_but_sml.clx", "devilutionx.mpq", 2, 0);
          s.elements.push_back(std::move(b)); }
        v.push_back(std::move(s));
    }

    // ---- dialog: dialog_large (lrpopup) ----------------------------------
    {
        Screen s; s.id = "dialog_large"; s.title = "大对话框（lpopup/lrpopup）";
        s.category = "dialog"; s.palette = "embedded";
        s.source_ref = "Source/DiabloUI/dialogs.cpp; button.cpp";
        s.elements.push_back(img("dl.bg", "大弹窗背景", {127, 100, 385, 280},
            pcx("ui_art\\lrpopup.pcx"), 0, "dialogs.cpp:70 lrpopup"));
        s.elements.push_back(txt("dl.title", "标题", {147, 110, 345, 35},
            "{TITLE}", 30, "yellow", "center", 1, "dialogs.cpp:114"));
        s.elements.push_back(txt("dl.body", "正文", {147, 141, 345, 190},
            "{BODY}", 24, "white", "left", 1, "dialogs.cpp:117"));
        { Element b = btn("dl.ok", "OK 按钮", {264, 335, 110, 28}, "OK", 2,
                          "dialogs.cpp:120");
          b.has_art = true; b.art = clx("ui_art\\dvl_but_sml.clx", "devilutionx.mpq", 2, 0);
          s.elements.push_back(std::move(b)); }
        v.push_back(std::move(s));
    }

    // ---- dialog: dialog_large_dvl (CLX variant) -------------------------
    {
        Screen s; s.id = "dialog_large_dvl"; s.title = "大对话框（DevilutionX CLX）";
        s.category = "dialog"; s.palette = "ui_art\\diablo.pal";
        s.source_ref = "Source/DiabloUI/dialogs.cpp:65 dvl_lrpopup.clx";
        s.elements.push_back(img("dld.bg", "大弹窗背景（CLX）", {127, 100, 385, 280},
            clx("ui_art\\dvl_lrpopup.clx"), 0, "dialogs.cpp:65"));
        s.elements.push_back(txt("dld.title", "标题", {147, 110, 345, 35},
            "{TITLE}", 30, "yellow", "center", 1, "dialogs.cpp:114"));
        s.elements.push_back(txt("dld.body", "正文", {147, 141, 345, 190},
            "{BODY}", 24, "white", "left", 1, "dialogs.cpp:117"));
        { Element b = btn("dld.ok", "OK 按钮", {264, 335, 110, 28}, "OK", 2,
                          "dialogs.cpp:120");
          b.has_art = true; b.art = clx("ui_art\\dvl_but_sml.clx", "devilutionx.mpq", 2, 0);
          s.elements.push_back(std::move(b)); }
        v.push_back(std::move(s));
    }

    return v;
}

inline const std::vector<Screen>& screens() {
    static const std::vector<Screen> k = buildScreens();
    return k;
}

inline const Screen* findScreen(const std::string& id) {
    for (const auto& s : screens())
        if (s.id == id) return &s;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Pure queries / validation
// ---------------------------------------------------------------------------
inline int elementCount(const Screen& s) { return static_cast<int>(s.elements.size()); }

inline int textElementCount(const Screen& s) {
    int n = 0;
    for (const auto& e : s.elements) if (e.type == ElementType::Text) ++n;
    return n;
}

inline std::vector<ArtRef> uniqueArtRefs(const Screen& s) {
    std::vector<ArtRef> out;
    for (const auto& e : s.elements) {
        if (!e.has_art) continue;
        bool seen = false;
        for (const auto& a : out) if (a.name == e.art.name) { seen = true; break; }
        if (!seen) out.push_back(e.art);
    }
    return out;
}

inline int artRefCount(const Screen& s) {
    return static_cast<int>(uniqueArtRefs(s).size());
}

inline std::vector<std::string> needsWidthList(const Screen& s) {
    std::vector<std::string> out;
    for (const auto& e : s.elements) {
        if (!e.has_art) continue;
        if ((e.art.format == "cel" || e.art.format == "cl2") && e.art.width <= 0) {
            bool seen = false;
            for (const auto& n : out) if (n == e.art.name) { seen = true; break; }
            if (!seen) out.push_back(e.art.name);
        }
    }
    return out;
}

inline std::vector<std::string> categories() {
    std::vector<std::string> out;
    for (const auto& s : screens()) {
        bool seen = false;
        for (const auto& c : out) if (c == s.category) { seen = true; break; }
        if (!seen) out.push_back(s.category);
    }
    std::sort(out.begin(), out.end());
    return out;
}

inline bool knownColor(const std::string& c) {
    if (c.empty()) return true;
    static const char* k[] = {
        "goldui", "grayui", "golduis", "grayuis", "yellow", "white", "whitegold",
        "red", "blue", "orange", "black", "buttonface", "buttonpushed",
        "gamedialogwhite", "gamedialogyellow", "gamedialogred", nullptr};
    for (int i = 0; k[i]; ++i) if (c == k[i]) return true;
    return false;
}

inline bool knownFontSize(int f) {
    return f == 0 || f == 12 || f == 22 || f == 24 || f == 30 || f == 42 || f == 46;
}

inline bool validateScreen(const Screen& s, std::string& err) {
    if (s.id.empty() || s.title.empty() || s.category.empty() || s.source_ref.empty()) {
        err = "screen id/title/category/source_ref must be non-empty"; return false;
    }
    if (s.canvas_w != 640 || s.canvas_h != 480) {
        err = "canvas must be 640x480"; return false;
    }
    for (const auto& e : s.elements) {
        if (e.id.empty()) { err = "empty element id in " + s.id; return false; }
        for (const auto& o : s.elements)
            if (&o != &e && o.id == e.id) { err = "duplicate element id " + e.id; return false; }
        if (!validateRectAnchor(e.rect, s.canvas_w, s.canvas_h)) {
            err = "rect anchor out of canvas: " + e.id; return false;
        }
        if (e.source_ref.empty()) { err = "empty source_ref: " + e.id; return false; }
        if (e.z < 0) { err = "negative z: " + e.id; return false; }
        if (!knownFontSize(e.font_size)) { err = "bad font_size: " + e.id; return false; }
        if (!knownColor(e.color)) { err = "bad color: " + e.id; return false; }
        if (e.has_art) {
            if (e.art.name.empty()) { err = "empty art name: " + e.id; return false; }
            const std::string& f = e.art.format;
            if (f != "pcx" && f != "cel" && f != "cl2" && f != "clx") {
                err = "bad format: " + e.id; return false;
            }
            const std::string suffix = "." + f;
            if (e.art.name.size() < suffix.size() ||
                e.art.name.compare(e.art.name.size() - suffix.size(), suffix.size(), suffix) != 0) {
                err = "art name/format mismatch: " + e.id; return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Overlay boxes (panels first as dashed gray, then elements numbered 1..N)
// ---------------------------------------------------------------------------
struct OverlayBox {
    Rect     rect;
    int      index    = 0;      // 1..N for elements; 0 for panels
    uint32_t rgba     = 0;
    bool     is_panel = false;
    bool     is_text  = false;
    std::string tag;            // e.g. "3" or "3T"
};

inline std::vector<OverlayBox> overlayBoxes(const Screen& s) {
    std::vector<OverlayBox> out;
    for (const auto& p : s.panels) {
        OverlayBox b; b.rect = p.rect; b.index = 0;
        b.rgba = overlayColor(ElementType::PanelRect);
        b.is_panel = true; b.tag = "";
        out.push_back(b);
    }
    int n = 1;
    for (const auto& e : s.elements) {
        OverlayBox b; b.rect = e.rect; b.index = n;
        b.rgba = overlayColor(e.type);
        b.is_text = (e.type == ElementType::Text);
        b.tag = std::to_string(n) + (b.is_text ? "T" : "");
        out.push_back(b);
        ++n;
    }
    return out;
}

} // namespace ui_layout
