// src/tools/ui_tools.cpp
//
// Stage 5 (goal 4): ui.list / ui.layout / ui.build. See ui_tools.h for the
// contract summary. Layout data lives in ui_layout.h (pure), raster helpers in
// ui_render.h (pure); this file is the only place that touches MPQ/decoders/json.

#include "tools/ui_tools.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/archive.h"
#include "tools/base64.h"
#include "tools/ui_layout.h"
#include "tools/ui_render.h"
#include "decoders/cel.h"
#include "decoders/cl2.h"
#include "decoders/clx.h"
#include "decoders/decoders_common.h"
#include "decoders/palette.h"
#include "decoders/pcx.h"
#include "decoders/png.h"

namespace {

using mcp::json;
using mcp::ToolResult;
namespace ul = ui_layout;
namespace ur = ui_render;

// ---- param helpers (same shape as scene_tools.cpp) ------------------------
std::string reqStr(const json& p, const char* key) {
    auto it = p.find(key);
    if (it == p.end() || !it->is_string())
        throw std::invalid_argument(std::string("missing string param: ") + key);
    return it->get<std::string>();
}

std::vector<std::string> archiveStack(const json& params) {
    std::vector<std::string> archives;
    if (params.contains("archive") && params["archive"].is_string())
        archives.push_back(params["archive"].get<std::string>());
    if (params.contains("archives") && params["archives"].is_array())
        for (const auto& a : params["archives"])
            if (a.is_string()) archives.push_back(a.get<std::string>());
    if (archives.empty())
        throw std::invalid_argument("provide `archive` (and/or `archives`)");
    return archives;
}

// Try to extract `name` from any archive in the stack. Returns the archive path
// that actually contained the file (for reporting) via `hitArchive`.
bool extractAny(const std::vector<std::string>& archives, const std::string& name,
                std::vector<uint8_t>& out, std::string* hitArchive = nullptr) {
    for (const auto& a : archives) {
        engine::Archive arc;
        if (!arc.open(a)) continue;
        if (arc.extract(name, out)) {
            if (hitArchive) *hitArchive = a;
            return true;
        }
    }
    return false;
}

std::string sanitize(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        if (c == '\\' || c == '/' || c == ':' || c == '.') c = '_';
    return r;
}

bool endsWith(const std::string& s, const char* suf) {
    const size_t n = std::string(suf).size();
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

// ---- JSON serialization of the pure catalog ------------------------------
json rectJson(const ul::Rect& r) {
    return json::array({r.x, r.y, r.w, r.h});
}

json artJson(const ul::ArtRef& a) {
    return json{
        {"name", a.name}, {"format", a.format}, {"width", a.width},
        {"frame", a.frame}, {"frames", a.frames}, {"palette", a.palette},
        {"archive_hint", a.archive_hint}, {"transparent_index", a.transparent_index}};
}

json flagsJson(const ul::Flags& f) {
    return json{{"align_center", f.align_center}, {"align_right", f.align_right},
                {"vertical_center", f.vertical_center}};
}

// Base element serialization (shared by ui.layout and ui.build). `extra` fields
// (decode_status / decoded_size / placed_rect) are merged in by the caller.
json elementJson(const ul::Element& e, int index) {
    json j = json{
        {"index", index}, {"id", e.id}, {"type", ul::typeName(e.type)},
        {"label", e.label}, {"rect", rectJson(e.rect)},
        {"panel", e.panel}, {"panel_rect", rectJson(e.panel_rect)},
        {"z", e.z}, {"size_from_art", e.size_from_art},
        {"art", e.has_art ? artJson(e.art) : json(nullptr)},
        {"grid", nullptr}, {"text", nullptr}, {"font_size", e.font_size},
        {"color", nullptr}, {"align", nullptr}, {"flags", flagsJson(e.flags)},
        {"source_ref", e.source_ref}};
    if (e.grid_cols > 0) {
        j["grid"] = json{{"cols", e.grid_cols}, {"rows", e.grid_rows},
                         {"cell", json::array({e.cell_w, e.cell_h})},
                         {"step", json::array({e.step_x, e.step_y})}};
    }
    if (!e.text.empty()) {
        j["text"] = e.text;
        j["color"] = e.color.empty() ? json(nullptr) : json(e.color);
        j["align"] = e.align.empty() ? json(nullptr) : json(e.align);
    }
    return j;
}

json screenSummaryJson(const ul::Screen& s) {
    json needs = json::array();
    for (const auto& n : ul::needsWidthList(s)) needs.push_back(n);
    return json{
        {"id", s.id}, {"title", s.title}, {"category", s.category},
        {"canvas", json{{"width", s.canvas_w}, {"height", s.canvas_h}}},
        {"element_count", ul::elementCount(s)},
        {"text_element_count", ul::textElementCount(s)},
        {"art_ref_count", ul::artRefCount(s)},
        {"panel_count", static_cast<int>(s.panels.size())},
        {"palette", s.palette}, {"needs_width", needs},
        {"source_ref", s.source_ref}};
}

// ---- toolUiList -----------------------------------------------------------
ToolResult toolUiList(const json& params) {
    std::string filter;
    if (params.contains("category") && params["category"].is_string())
        filter = params["category"].get<std::string>();

    json arr = json::array();
    for (const auto& s : ul::screens()) {
        if (!filter.empty() && s.category != filter) continue;
        arr.push_back(screenSummaryJson(s));
    }
    json cats = json::array();
    for (const auto& c : ul::categories()) cats.push_back(c);

    ToolResult r;
    r.value = json{{"count", arr.size()},
                   {"canvas_default", json{{"width", 640}, {"height", 480}}},
                   {"categories", cats}, {"screens", arr}};
    return r;
}

// ---- toolUiLayout ---------------------------------------------------------
ToolResult toolUiLayout(const json& params) {
    const std::string id = reqStr(params, "id");
    const ul::Screen* s = ul::findScreen(id);
    if (!s) throw std::runtime_error("unknown ui screen id: " + id);

    json panels = json::array();
    for (const auto& p : s->panels)
        panels.push_back(json{{"id", p.id}, {"rect", rectJson(p.rect)}, {"note", p.note}});

    json elements = json::array();
    for (size_t i = 0; i < s->elements.size(); ++i)
        elements.push_back(elementJson(s->elements[i], static_cast<int>(i)));

    json artRefs = json::array();
    for (const auto& a : ul::uniqueArtRefs(*s)) artRefs.push_back(artJson(a));
    json needs = json::array();
    for (const auto& n : ul::needsWidthList(*s)) needs.push_back(n);

    ToolResult r;
    r.value = json{
        {"id", s->id}, {"title", s->title}, {"category", s->category},
        {"canvas", json{{"width", s->canvas_w}, {"height", s->canvas_h}}},
        {"logical_origin", "screen"}, {"palette", s->palette},
        {"panels", panels}, {"elements", elements}, {"art_refs", artRefs},
        {"needs_width", needs},
        {"overlay_box_count", static_cast<int>(ul::overlayBoxes(*s).size())},
        {"source_ref", s->source_ref}};
    return r;
}

// ---- decode one referenced art resource into RGBA frames -----------------
// palRgb: 768-byte RGB (loadPalette output). PCX uses its embedded palette.
std::vector<decoders::DecodedFrame> decodeUiArt(
    const std::vector<uint8_t>& buf, const ul::ArtRef& art,
    const std::vector<uint8_t>& palRgb) {
    std::vector<decoders::DecodedFrame> frames;
    std::string lower = art.name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });

    if (endsWith(lower, ".pcx")) {
        decoders::DecodedFrame fr;
        std::vector<uint8_t> embedded;
        if (!decoders::decodePcx(buf, fr, &embedded, nullptr, art.transparent_index))
            return frames;
        frames.push_back(std::move(fr));
    } else if (endsWith(lower, ".cel")) {
        std::vector<uint8_t> clx = decoders::celToClx(buf.data(), buf.size(),
                                                       static_cast<uint16_t>(art.width));
        frames = decoders::decodeClx(clx, palRgb);
    } else if (endsWith(lower, ".cl2")) {
        std::vector<uint8_t> clx = decoders::cl2ToClx(buf.data(), buf.size(),
                                                       static_cast<uint16_t>(art.width));
        frames = decoders::decodeClx(clx, palRgb);
    } else if (endsWith(lower, ".clx")) {
        frames = decoders::decodeClx(buf, palRgb);
    }
    return frames;
}

// Per-element runtime decode result (parallel to screen.elements).
struct ElemStatus {
    std::string decode_status = "text"; // ok|text|missing|width_unknown|decode_error
    json decoded_size = nullptr;
    json placed_rect  = nullptr;
};

// Per-art-file status (keyed by art name).
struct ArtStatus {
    std::string name, format, palette, archive, status = "missing";
    int width = 0, height = 0, frame_used = 0;
    size_t bytes = 0;
};

// ---- toolUiBuild ----------------------------------------------------------
ToolResult toolUiBuild(const json& params) {
    const std::string id = reqStr(params, "id");
    const ul::Screen* screen = ul::findScreen(id);
    if (!screen) throw std::runtime_error("unknown ui screen id: " + id);
    const std::vector<std::string> archives = archiveStack(params);

    const int W = screen->canvas_w, H = screen->canvas_h;
    const std::string outDir = params.value("out", std::string("out/ui"));
    // Optional explicit override; when absent, each ArtRef.palette wins (this is
    // essential: a screen mixes in-game CELs (town.pal) and dialog CLX
    // (diablo.pal), so a single forced palette would corrupt one of them).
    const std::string palName = params.value("palette", std::string(""));
    const bool wantOverlay = params.value("overlay", true);
    const bool labelNumbers = params.value("label_numbers", true);
    const bool skipMissing = params.value("skip_missing_art", true);

    // Background: array [r,g,b,a], default opaque black -> 0 transparent pixels.
    uint8_t bgR = 0, bgG = 0, bgB = 0, bgA = 255;
    if (params.contains("background") && params["background"].is_array() &&
        params["background"].size() >= 3) {
        const auto& b = params["background"];
        bgR = static_cast<uint8_t>(std::max(0, std::min(255, b[0].get<int>())));
        bgG = static_cast<uint8_t>(std::max(0, std::min(255, b[1].get<int>())));
        bgB = static_cast<uint8_t>(std::max(0, std::min(255, b[2].get<int>())));
        if (b.size() >= 4)
            bgA = static_cast<uint8_t>(std::max(0, std::min(255, b[3].get<int>())));
    }
    ur::Canvas canvas(W, H, ur::packRGBA(bgR, bgG, bgB, bgA));

    // Palette BANK: each ArtRef carries its own authoritative palette name
    // (ui_art\\diablo.pal for menu/dialog .clx, levels\\towndata\\town.pal for
    // in-game .cel/.clx, "embedded" for PCX). Load each distinct 768-byte RGB
    // table once and cache it. `palette` param, if given, overrides all of them.
    struct PalEntry {
        std::string name, status, archive;
        std::vector<uint8_t> rgb;   // 768-byte RGB (empty if unavailable)
        int colors = 0;
    };
    std::vector<PalEntry> palBank;
    auto paletteFor = [&](const std::string& name) -> PalEntry& {
        for (auto& p : palBank) if (p.name == name) return p;
        PalEntry p; p.name = name;
        std::vector<uint8_t> buf;
        if (extractAny(archives, name, buf, &p.archive)) {
            p.rgb = decoders::loadPalette(buf);
            if (p.rgb.size() >= 256 * 3) { p.status = "ok"; p.colors = 256; }
            else { p.rgb.clear(); p.status = "decode_error"; }
        } else {
            p.status = "missing";
        }
        palBank.push_back(std::move(p));
        return palBank.back();
    };
    auto resolvePaletteName = [&](const ul::ArtRef& art) -> std::string {
        if (!palName.empty() && palName != "embedded") return palName; // explicit override
        return art.palette;
    };
    // Pre-resolve palettes for all non-PCX art so reporting includes every one.
    for (const auto& e : screen->elements)
        if (e.has_art && e.art.format != "pcx")
            paletteFor(resolvePaletteName(e.art));

    // Draw order: z ascending, then original index.
    std::vector<int> order(screen->elements.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return screen->elements[a].z < screen->elements[b].z;
    });

    std::vector<ElemStatus> statuses(screen->elements.size());
    std::vector<ArtStatus> artStatuses;
    auto artStatusFor = [&](const std::string& n) -> ArtStatus& {
        for (auto& a : artStatuses) if (a.name == n) return a;
        artStatuses.push_back(ArtStatus{});
        artStatuses.back().name = n;
        return artStatuses.back();
    };
    // frame cache keyed by "name|frame|palette" (same art could be decoded with
    // different palettes across screens; keep entries separate).
    struct CacheKey { std::string name; int frame; std::string palette; };
    std::vector<std::pair<CacheKey, std::vector<decoders::DecodedFrame>>> cache;

    for (int idx : order) {
        const ul::Element& e = screen->elements[idx];
        ElemStatus& st = statuses[idx];
        if (!e.has_art) {
            st.decode_status = "text"; // no backend art; frontend draws text/box
            continue;
        }
        ArtStatus& as = artStatusFor(e.art.name);
        as.format = e.art.format; as.palette = e.art.palette;
        as.frame_used = e.art.frame;
        if ((e.art.format == "cel" || e.art.format == "cl2") && e.art.width <= 0) {
            st.decode_status = "width_unknown"; as.status = "width_unknown"; continue;
        }
        // Resolve this element's palette (PCX uses its embedded one).
        const std::vector<uint8_t>* palptr = nullptr;
        if (e.art.format != "pcx") {
            PalEntry& pe = paletteFor(resolvePaletteName(e.art));
            as.palette = pe.name;
            if (pe.rgb.empty()) {
                st.decode_status = "decode_error"; as.status = "decode_error";
                if (!skipMissing)
                    throw std::runtime_error("palette unavailable: " + pe.name);
                continue;
            }
            palptr = &pe.rgb;
        }
        const std::vector<uint8_t> emptyPal;
        const std::vector<uint8_t>& palRgb = palptr ? *palptr : emptyPal;

        // Decode (cache by name+frame+palette; same art+palette may be reused).
        const std::string cachePal = as.palette.empty() ? std::string("embedded") : as.palette;
        std::vector<decoders::DecodedFrame>* frames = nullptr;
        for (auto& c : cache)
            if (c.first.name == e.art.name && c.first.frame == e.art.frame &&
                c.first.palette == cachePal) { frames = &c.second; break; }
        if (!frames) {
            std::vector<uint8_t> buf;
            std::string hit;
            if (!extractAny(archives, e.art.name, buf, &hit)) {
                st.decode_status = "missing"; as.status = "missing";
                if (!skipMissing) throw std::runtime_error("extract failed: " + e.art.name);
                continue;
            }
            as.archive = hit; as.bytes = buf.size();
            std::vector<decoders::DecodedFrame> decoded = decodeUiArt(buf, e.art, palRgb);
            cache.push_back({CacheKey{e.art.name, e.art.frame, cachePal}, std::move(decoded)});
            frames = &cache.back().second;
        }
        if (!frames || frames->empty()) {
            st.decode_status = "decode_error"; as.status = "decode_error"; continue;
        }
        const int fi = std::min(std::max(0, e.art.frame), static_cast<int>(frames->size()) - 1);
        const decoders::DecodedFrame& fr = (*frames)[fi];
        if (fr.width == 0 || fr.height == 0) {
            st.decode_status = "decode_error"; as.status = "decode_error"; continue;
        }

        ur::blit(canvas, e.rect.x, e.rect.y, fr.rgba.data(), fr.width, fr.height);
        st.decode_status = "ok";
        st.decoded_size = json::array({fr.width, fr.height});
        st.placed_rect = json::array({e.rect.x, e.rect.y, fr.width, fr.height});
        as.status = "ok"; as.width = fr.width; as.height = fr.height;
    }

    // Count transparent pixels (for verifiable "opaque background" assertion).
    size_t transparentPixels = 0;
    for (size_t i = 3; i < canvas.rgba.size(); i += 4)
        if (canvas.rgba[i] == 0) ++transparentPixels;

    // Base PNG.
    const std::vector<uint8_t> basePng =
        decoders::encodePNG(static_cast<uint16_t>(W), static_cast<uint16_t>(H), canvas.rgba);
    if (basePng.empty()) throw std::runtime_error("PNG encode failed (base)");

    // Overlay PNG (aligned element boxes + optional number tags).
    std::vector<uint8_t> overlayPng;
    std::string overlayBase64;
    int overlayBoxCount = 0;
    if (wantOverlay) {
        auto boxes = ul::overlayBoxes(*screen);
        overlayBoxCount = static_cast<int>(boxes.size());
        ur::Canvas ov = ur::rasterizeOverlay(W, H, boxes, labelNumbers, true);
        overlayPng = decoders::encodePNG(static_cast<uint16_t>(W),
                                          static_cast<uint16_t>(H), ov.rgba);
        if (overlayPng.empty()) throw std::runtime_error("PNG encode failed (overlay)");
        overlayBase64 = mcp::base64::encode(overlayPng);
    }

    // Write artifacts to disk.
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    const std::string base = sanitize(id);
    const std::string imagePath = outDir + "/" + base + ".png";
    const std::string overlayPath = outDir + "/" + base + "_overlay.png";
    const std::string metaPath = outDir + "/" + base + ".json";
    auto writeBytes = [](const std::string& p, const std::vector<uint8_t>& b) {
        std::ofstream f(p, std::ios::binary);
        if (f) f.write(reinterpret_cast<const char*>(b.data()),
                       static_cast<std::streamsize>(b.size()));
    };
    writeBytes(imagePath, basePng);
    if (wantOverlay) writeBytes(overlayPath, overlayPng);

    // Build elements[] (with runtime decode metadata) + art[] + counts.
    json elements = json::array();
    for (size_t i = 0; i < screen->elements.size(); ++i) {
        json ej = elementJson(screen->elements[i], static_cast<int>(i));
        ej["decode_status"] = statuses[i].decode_status;
        ej["decoded_size"] = statuses[i].decoded_size;
        ej["placed_rect"] = statuses[i].placed_rect;
        elements.push_back(std::move(ej));
    }
    json art = json::array();
    int artOk = 0, artMissing = 0, artError = 0;
    for (const auto& a : artStatuses) {
        art.push_back(json{{"name", a.name}, {"format", a.format},
                           {"archive", a.archive}, {"palette", a.palette},
                           {"status", a.status}, {"width", a.width},
                           {"height", a.height}, {"bytes", a.bytes},
                           {"frame_used", a.frame_used}});
        if (a.status == "ok") ++artOk;
        else if (a.status == "missing" || a.status == "width_unknown") ++artMissing;
        else ++artError;
    }
    json needs = json::array();
    for (const auto& n : ul::needsWidthList(*screen)) needs.push_back(n);

    // Palette bank as an array (one entry per distinct palette used by this
    // screen), plus a `palette` summary object kept for the documented contract.
    json palettes = json::array();
    int palOk = 0;
    for (const auto& p : palBank) {
        palettes.push_back(json{{"name", p.name}, {"status", p.status},
                                {"archive", p.archive}, {"colors", p.colors}});
        if (p.status == "ok") ++palOk;
    }
    std::string palSummaryStatus = palBank.empty() ? "unused"
        : (palOk == static_cast<int>(palBank.size()) ? "ok" : "partial");
    json paletteSummary = json{
        {"count", static_cast<int>(palBank.size())}, {"ok_count", palOk},
        {"status", palSummaryStatus},
        {"name", palBank.empty() ? std::string("(none)") : palBank.front().name},
        {"archive", palBank.empty() ? std::string("") : palBank.front().archive},
        {"colors", palBank.empty() ? 0 : palBank.front().colors}};

    json meta = json{
        {"id", id}, {"title", screen->title}, {"category", screen->category},
        {"width", W}, {"height", H}, {"element_count", ul::elementCount(*screen)},
        {"overlay_box_count", overlayBoxCount},
        {"transparent_pixel_count", transparentPixels},
        {"background_opaque", bgA == 255},
        {"elements", elements}, {"art", art}, {"needs_width", needs},
        {"palettes", palettes}, {"palette", paletteSummary},
        {"art_ok_count", artOk}, {"art_missing_count", artMissing},
        {"art_error_count", artError}};
    { std::ofstream f(metaPath); if (f) f << meta.dump(); }

    ToolResult r;
    r.value = json{
        {"id", id}, {"title", screen->title}, {"category", screen->category},
        {"canvas", json{{"width", W}, {"height", H}}},
        {"image_path", imagePath},
        {"overlay_path", wantOverlay ? overlayPath : std::string("")},
        {"meta_path", metaPath},
        {"image_base64", mcp::base64::encode(basePng)},
        {"overlay_base64", overlayBase64},
        {"width", W}, {"height", H},
        {"element_count", ul::elementCount(*screen)},
        {"overlay_box_count", overlayBoxCount},
        {"elements", elements}, {"art", art}, {"needs_width", needs},
        {"art_ok_count", artOk}, {"art_missing_count", artMissing},
        {"art_error_count", artError},
        {"palettes", palettes}, {"palette", paletteSummary},
        {"background_opaque", bgA == 255},
        {"transparent_pixel_count", transparentPixels},
        {"meta", meta}};
    return r;
}

} // namespace

namespace mcp {
namespace tools {

void registerUiTools(ToolRegistry& reg) {
    reg.registerTool(ToolDef{
        "ui.list",
        "List all Diablo 1 / Hellfire UI screens whose layout can be inspected "
        "(main menu, title, credits, hero/difficulty select, OK/YesNo dialogs, "
        "settings, in-game control panel with health/mana orbs, character/inventory/"
        "spellbook/quest panels, stores, and small/large popup dialogs). Pure "
        "static catalog: NO MPQ needed. Returns id/title/category/canvas/"
        "element_count/text_element_count/art_ref_count/palette/needs_width per "
        "screen. Optional `category` filter (menu|hud|panel|store|dialog).",
        json{{"type", "object"},
             {"properties", json{
                 {"category", json{{"type", "string"},
                     {"description", "optional filter: menu|hud|panel|store|dialog"}}}}}},
        toolUiList});

    reg.registerTool(ToolDef{
        "ui.layout",
        "Return the FULL structured layout of one UI screen (from ui.list): "
        "panel partitions, every element with absolute 640x480 rect, type, "
        "label, art reference (MPQ name/format/width/frame/palette), text/"
        "font_size/color/align/flags, and DevilutionX source_ref. Text is NOT "
        "rendered server-side; these fields drive the frontend approximate-font "
        "text layer. NO MPQ needed. `id` from ui.list.",
        json{{"type", "object"},
             {"properties", json{
                 {"id", json{{"type", "string"}, {"description", "screen id from ui.list"}}}}},
             {"required", json::array({"id"})}},
        toolUiLayout});

    reg.registerTool(ToolDef{
        "ui.build",
        "Decode REAL MPQ UI art and composite a 640x480 screen PNG (opaque "
        "background, 0 transparent pixels): backgrounds (.pcx embedded palette), "
        "panel CEL, health/mana orbs, buttons and CLX overlays are blitted at "
        "their transcribed DevilutionX coordinates. Also writes an aligned "
        "element-rectangle overlay PNG (per-type boxes + number tags) and a JSON "
        "sidecar. Text is NOT burned into the backend PNG (see ui.layout). "
        "Returns image_base64/overlay_base64 (PNG), per-element decode_status/"
        "decoded_size/placed_rect, per-art decode status, palette status, and "
        "out/ui/ artifact paths. UI art spans DIABDAT.MPQ (.pcx/.cel) and "
        "devilutionx.mpq (ui_art\\diablo.pal/.clx): pass BOTH via `archive` + "
        "`archives` so cross-archive extraction works. CEL/CL2 frame width comes "
        "from the layout catalog (not auto-lookup); unknown widths are reported "
        "in needs_width and skipped, never faked.",
        json{{"type", "object"},
             {"properties", json{
                 {"id", json{{"type", "string"}, {"description", "screen id from ui.list"}}},
                 {"archive", json{{"type", "string"}, {"description", "primary MPQ path"}}},
                 {"archives", json{{"type", "array"}, {"items", json{{"type", "string"}}},
                     {"description", "additional MPQ search paths (e.g. devilutionx.mpq for ui_art\\diablo.pal and .clx art)"}}},
                 {"out", json{{"type", "string"}, {"description", "output dir (default out/ui)"}}},
                 {"palette", json{{"type", "string"}, {"description", "override CEL/CLX palette name (default per screen, usually ui_art\\diablo.pal)"}}},
                 {"overlay", json{{"type", "boolean"}, {"description", "also produce the element-box overlay PNG (default true)"}}},
                 {"label_numbers", json{{"type", "boolean"}, {"description", "draw number tags on overlay boxes (default true)"}}},
                 {"background", json{{"type", "array"}, {"items", json{{"type", "integer"}}},
                     {"description", "[r,g,b,a] canvas fill (default [0,0,0,255] opaque black)"}}},
                 {"skip_missing_art", json{{"type", "boolean"}, {"description", "skip unresolvable art instead of failing (default true)"}}},
             }},
             {"required", json::array({"id", "archive"})}},
        toolUiBuild});
}

} // namespace tools
} // namespace mcp
