/* Modified by JackerGamer (2026): keyboard + mouse controls, analog anti-dead zone,
   Simplified Chinese menu, Square latch, ov_bbox fix.  See README.md. */
#define IMGUI_DEFINE_MATH_OPERATORS
#include "ps2_ui.h"
#include "ps2_settings.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void ps2_log(const char *fmt, ...);

namespace {

bool g_init, g_visible;
SDL_Window *g_window;
float g_max_aniso;
float g_dpi = 1.0f;
VkFormat g_color_format;

enum { CAP_NONE, CAP_KEY, CAP_PAD };
int g_cap = CAP_NONE, g_cap_act, g_cap_slot;
bool g_cap_armed;
Uint64 g_cap_start;
Sint16 g_cap_base[SDL_GAMEPAD_AXIS_COUNT];

enum { TAB_NONE = -1, TAB_KEYBOARD = 4, TAB_CONTROLLER = 5, TAB_ANALOG = 6 };
int g_want_tab = TAB_NONE;

const ImVec4 ACCENT(0.40f, 0.86f, 0.50f, 1.0f);
const ImVec4 WARN(1.00f, 0.76f, 0.32f, 1.0f);

/* Simplified Chinese.  The CJK glyphs come from a Windows system font: they
   are merged into the default font, so a Chinese word shows up in either
   language, and the same font is also loaded on its own to draw the whole
   menu while Chinese is selected. */
bool g_zh;
ImFont *g_font_zh;
void *g_cjk_data;

const char *L(const char *en, const char *zh) { return g_zh ? zh : en; }

struct Tr { const char *en, *zh; };

bool combo(const char *label, int *v, const Tr *items, int n) {
    const char *it[8];
    for (int i = 0; i < n && i < 8; i++) it[i] = L(items[i].en, items[i].zh);
    return ImGui::Combo(label, v, it, n < 8 ? n : 8);
}

void load_cjk_font(ImGuiIO &io) {
    static const char *const NAMES[] = { "msyh.ttc", "Deng.ttf", "simhei.ttf", "simsun.ttc" };
    const char *windir = getenv("WINDIR");
    io.Fonts->AddFontDefault();
    for (const char *name : NAMES) {
        char path[512];
        snprintf(path, sizeof path, "%s\\Fonts\\%s", windir && *windir ? windir : "C:\\Windows", name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        long size = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
        void *data = size > 0 ? IM_ALLOC((size_t)size) : nullptr;
        bool ok = data && fseek(f, 0, SEEK_SET) == 0 && fread(data, 1, (size_t)size, f) == (size_t)size;
        fclose(f);
        if (!ok) { if (data) IM_FREE(data); continue; }
        g_cjk_data = data;
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.FontDataOwnedByAtlas = false;
        io.Fonts->AddFontFromMemoryTTF(data, (int)size, 0.0f, &merge);
        ImFontConfig own;
        own.FontDataOwnedByAtlas = false;
        g_font_zh = io.Fonts->AddFontFromMemoryTTF(data, (int)size, 0.0f, &own);
        ps2_log("ui: Chinese text from %s", path);
        return;
    }
    ps2_log("ui: no Chinese system font found; the menu stays in English");
}

const char *act_name(int act) {
    static const char *const ZH[PS2_ACT_COUNT] = {
        "选择键", "L3", "R3", "开始键",
        "方向键 上", "方向键 右", "方向键 下", "方向键 左",
        "L2", "R2", "L1", "R1",
        "三角键", "圆圈键", "交叉键", "方块键",
        "左摇杆 上", "左摇杆 下", "左摇杆 左", "左摇杆 右",
        "右摇杆 上", "右摇杆 下", "右摇杆 左", "右摇杆 右",
    };
    return g_zh && act >= 0 && act < PS2_ACT_COUNT ? ZH[act] : ps2_action_name(act);
}

const char *pad_name(int code) {
    static const char *const BUTTONS[] = {
        "下键 (A / 交叉)", "右键 (B / 圆圈)", "左键 (X / 方块)", "上键 (Y / 三角)",
        "Back / 选择", "Guide 键", "Start", "左摇杆按下", "右摇杆按下", "左肩键", "右肩键",
        "十字键 上", "十字键 下", "十字键 左", "十字键 右", "Misc",
        "右背键 1", "左背键 1", "右背键 2", "左背键 2",
        "触摸板", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6",
    };
    static char buf[4][48];
    static unsigned next;
    if (!g_zh || code <= 0) return ps2_padbind_name(code);
    char *out = buf[next++ % 4];
    if (code & PS2_PADBIND_AXIS) {
        int axis = (code & 0xFF) >> 1, pos = code & 1;
        switch (axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:  return pos ? "左摇杆 右" : "左摇杆 左";
        case SDL_GAMEPAD_AXIS_LEFTY:  return pos ? "左摇杆 下" : "左摇杆 上";
        case SDL_GAMEPAD_AXIS_RIGHTX: return pos ? "右摇杆 右" : "右摇杆 左";
        case SDL_GAMEPAD_AXIS_RIGHTY: return pos ? "右摇杆 下" : "右摇杆 上";
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:  return "左扳机";
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "右扳机";
        default: snprintf(out, 48, "轴 %d%c", axis, pos ? '+' : '-'); return out;
        }
    }
    if (code - 1 < (int)(sizeof BUTTONS / sizeof BUTTONS[0])) return BUTTONS[code - 1];
    snprintf(out, 48, "按钮 %d", code - 1);
    return out;
}

void check_vk(VkResult r) {
    if (r < 0) ps2_log("ui: Vulkan call failed (%d)", (int)r);
}

void changed(int dirty) { ps2_settings_touch(dirty | PS2_CFG_SAVE); }

void gfx_changed(int dirty) {
    ps2_cfg.preset = ps2_settings_preset_matches(&ps2_cfg);
    changed(dirty);
}

void help(const char *text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void env_note(const char *var, const char *what) {
    const char *v = getenv(var);
    if (v && *v) ImGui::TextColored(WARN, L("%s=%s is set and %s.", "已设置 %s=%s，%s。"), var, v, what);
}

bool slider_pct(const char *label, float *v, float lo, float hi) {
    float pct = *v * 100.0f;
    if (ImGui::SliderFloat(label, &pct, lo * 100.0f, hi * 100.0f, "%.1f%%")) {
        *v = pct / 100.0f;
        return true;
    }
    return false;
}

void apply_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 8.0f;
    s.ChildRounding = 6.0f;
    s.FrameRounding = 5.0f;
    s.PopupRounding = 6.0f;
    s.GrabRounding = 5.0f;
    s.TabRounding = 5.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowPadding = ImVec2(16.0f, 14.0f);
    s.FramePadding = ImVec2(9.0f, 5.0f);
    s.ItemSpacing = ImVec2(10.0f, 8.0f);
    s.WindowBorderSize = 1.0f;
    s.SeparatorTextBorderSize = 2.0f;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg]         = ImVec4(0.07f, 0.09f, 0.10f, 0.96f);
    c[ImGuiCol_ChildBg]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_PopupBg]          = ImVec4(0.08f, 0.10f, 0.11f, 0.98f);
    c[ImGuiCol_Border]           = ImVec4(0.30f, 0.55f, 0.36f, 0.45f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.13f, 0.17f, 0.16f, 1.00f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.19f, 0.27f, 0.22f, 1.00f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.22f, 0.34f, 0.26f, 1.00f);
    c[ImGuiCol_TitleBg]          = ImVec4(0.07f, 0.12f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgActive]    = ImVec4(0.10f, 0.22f, 0.14f, 1.00f);
    c[ImGuiCol_Button]           = ImVec4(0.16f, 0.26f, 0.19f, 1.00f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.24f, 0.42f, 0.29f, 1.00f);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.30f, 0.55f, 0.36f, 1.00f);
    c[ImGuiCol_Header]           = ImVec4(0.16f, 0.30f, 0.20f, 1.00f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.22f, 0.40f, 0.27f, 1.00f);
    c[ImGuiCol_HeaderActive]     = ImVec4(0.28f, 0.50f, 0.33f, 1.00f);
    c[ImGuiCol_Tab]              = ImVec4(0.11f, 0.18f, 0.13f, 1.00f);
    c[ImGuiCol_TabHovered]       = ImVec4(0.24f, 0.44f, 0.30f, 1.00f);
    c[ImGuiCol_TabSelected]      = ImVec4(0.19f, 0.36f, 0.24f, 1.00f);
    c[ImGuiCol_CheckMark]        = ACCENT;
    c[ImGuiCol_SliderGrab]       = ImVec4(0.36f, 0.70f, 0.44f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ACCENT;
    c[ImGuiCol_SeparatorHovered] = ACCENT;
    c[ImGuiCol_TextSelectedBg]   = ImVec4(0.30f, 0.55f, 0.36f, 0.45f);
    c[ImGuiCol_NavCursor]        = ACCENT;
    c[ImGuiCol_PlotHistogram]    = ImVec4(0.36f, 0.70f, 0.44f, 1.00f);
}

void bind_key(int act, int slot, int sc) {
    if (sc > 0)
        for (int a = 0; a < PS2_ACT_COUNT; a++)
            for (int k = 0; k < PS2_BIND_SLOTS; k++)
                if (ps2_cfg.key[a][k] == sc) ps2_cfg.key[a][k] = 0;
    ps2_cfg.key[act][slot] = sc;
    changed(0);
}

void bind_pad(int act, int slot, int code) {
    if (code > 0)
        for (int a = 0; a < PS2_ACT_COUNT; a++)
            for (int k = 0; k < PS2_BIND_SLOTS; k++)
                if (ps2_cfg.pad[a][k] == code) ps2_cfg.pad[a][k] = 0;
    ps2_cfg.pad[act][slot] = code;
    changed(0);
}

void start_capture(int kind, int act, int slot) {
    g_cap = kind;
    g_cap_act = act;
    g_cap_slot = slot;
    g_cap_armed = false;
    g_cap_start = SDL_GetTicks();
}

void end_capture() { g_cap = CAP_NONE; }

void update_capture_arming() {
    if (g_cap == CAP_NONE || g_cap_armed) return;
    if (SDL_GetTicks() - g_cap_start < 150) return;
    int nk = 0;
    const bool *ks = SDL_GetKeyboardState(&nk);
    for (int i = 0; ks && i < nk; i++)
        if (ks[i]) return;
    SDL_Gamepad *g = ps2_video_gamepad();
    memset(g_cap_base, 0, sizeof g_cap_base);
    if (g) {
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
            if (SDL_GetGamepadButton(g, (SDL_GamepadButton)b)) return;
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++)
            g_cap_base[a] = SDL_GetGamepadAxis(g, (SDL_GamepadAxis)a);
    }
    g_cap_armed = true;
}

bool capture_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: {
        if (e->key.repeat || !g_cap_armed) return true;
        SDL_Scancode sc = e->key.scancode;
        if (sc == SDL_SCANCODE_ESCAPE) { end_capture(); return true; }
        bool clear = sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_DELETE;
        if (g_cap == CAP_KEY) bind_key(g_cap_act, g_cap_slot, clear ? 0 : (int)sc);
        else if (clear) bind_pad(g_cap_act, g_cap_slot, 0);
        else return true;
        end_capture();
        return true;
    }
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (g_cap == CAP_PAD && g_cap_armed) {
            bind_pad(g_cap_act, g_cap_slot, PS2_PADBIND_BUTTON(e->gbutton.button));
            end_capture();
        }
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (g_cap == CAP_PAD && g_cap_armed && e->gaxis.axis < SDL_GAMEPAD_AXIS_COUNT) {
            int axis = e->gaxis.axis;
            int d = (int)e->gaxis.value - (int)g_cap_base[axis];
            bool trig = axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                     || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            if (d > 16000 || d < -16000) {
                bind_pad(g_cap_act, g_cap_slot,
                         PS2_PADBIND_AXISDIR(axis, trig ? 1 : d > 0));
                end_capture();
            }
        }
        return true;
    default:
        return false;
    }
}

struct Group { const char *title, *zh; int acts[4]; };
const Group GROUPS[] = {
    { "Face buttons", "面键", { PS2_ACT_CROSS, PS2_ACT_CIRCLE, PS2_ACT_SQUARE, PS2_ACT_TRIANGLE } },
    { "D-pad", "方向键", { PS2_ACT_UP, PS2_ACT_DOWN, PS2_ACT_LEFT, PS2_ACT_RIGHT } },
    { "Shoulders", "肩键", { PS2_ACT_L1, PS2_ACT_R1, PS2_ACT_L2, PS2_ACT_R2 } },
    { "Start, Select and stick clicks", "开始键、选择键和摇杆按下",
      { PS2_ACT_START, PS2_ACT_SELECT, PS2_ACT_L3, PS2_ACT_R3 } },
    { "Left stick", "左摇杆", { PS2_ACT_LS_UP, PS2_ACT_LS_DOWN, PS2_ACT_LS_LEFT, PS2_ACT_LS_RIGHT } },
    { "Right stick", "右摇杆", { PS2_ACT_RS_UP, PS2_ACT_RS_DOWN, PS2_ACT_RS_LEFT, PS2_ACT_RS_RIGHT } },
};

void binding_table(bool pad) {
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
                             | ImGuiTableFlags_SizingStretchProp;
    for (const Group &grp : GROUPS) {
        ImGui::SeparatorText(L(grp.title, grp.zh));
        ImGui::PushID(grp.title);
        if (ImGui::BeginTable("binds", 3, tf)) {
            ImGui::TableSetupColumn(L("Action", "动作"), ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn(L("Primary", "主绑定"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn(L("Secondary", "副绑定"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
            for (int act : grp.acts) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(act_name(act));
                for (int slot = 0; slot < PS2_BIND_SLOTS; slot++) {
                    ImGui::TableSetColumnIndex(1 + slot);
                    ImGui::PushID(act * 4 + slot);
                    int code = pad ? ps2_cfg.pad[act][slot] : ps2_cfg.key[act][slot];
                    bool capturing = g_cap == (pad ? CAP_PAD : CAP_KEY)
                                  && g_cap_act == act && g_cap_slot == slot;
                    const char *label = capturing ? "..."
                                      : pad ? pad_name(code) : ps2_key_name(code);
                    bool dim = !code && !capturing;
                    if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (ImGui::Button(label, ImVec2(-FLT_MIN, 0.0f)))
                        start_capture(pad ? CAP_PAD : CAP_KEY, act, slot);
                    if (dim) ImGui::PopStyleColor();
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        if (pad) bind_pad(act, slot, 0);
                        else bind_key(act, slot, 0);
                    }
                    ImGui::SetItemTooltip("%s", L("Click to rebind, right-click to clear.",
                                                  "点击重新绑定，右键清除。"));
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
}

void draw_capture_popup() {
    const char *id = L("Rebind###capture", "重新绑定###capture");
    bool open = ImGui::IsPopupOpen(id);
    if (g_cap == CAP_NONE && !open) return;
    if (g_cap != CAP_NONE && !open) ImGui::OpenPopup(id);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize
                                | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
        return;
    if (g_cap == CAP_NONE) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::Text("%s  (%s)", act_name(g_cap_act),
                g_cap_slot ? L("secondary", "副绑定") : L("primary", "主绑定"));
    ImGui::Spacing();
    ImGui::TextColored(ACCENT, "%s", g_cap == CAP_KEY
                       ? L("Press a key...", "请按一个键……")
                       : L("Press a button, or push a stick or trigger...",
                           "请按一个按钮，或推动摇杆、扳机……"));
    if (!g_cap_armed)
        ImGui::TextDisabled("%s", L("Release all keys and buttons first.", "请先松开所有按键和按钮。"));
    ImGui::TextDisabled("%s", L("Esc cancels, Backspace clears.", "Esc 取消，Backspace 清除。"));
    ImGui::Spacing();
    if (ImGui::Button(L("Clear binding", "清除绑定"))) {
        if (g_cap == CAP_KEY) bind_key(g_cap_act, g_cap_slot, 0);
        else bind_pad(g_cap_act, g_cap_slot, 0);
        end_capture();
    }
    ImGui::SameLine();
    if (ImGui::Button(L("Cancel", "取消"))) end_capture();
    if (g_cap == CAP_NONE) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

const Tr WINDOW_MODES[] = {
    { "Windowed", "窗口" }, { "Borderless fullscreen", "无边框全屏" },
    { "Exclusive fullscreen", "独占全屏" },
};

struct WinSize { int w, h; const char *tag; };
const WinSize WIN_SIZES[] = {
    { 640, 448, "1x" }, { 1280, 896, "2x" }, { 1920, 1344, "3x" }, { 2560, 1792, "4x" },
    { 1280, 720, "720p" }, { 1600, 900, "" }, { 1920, 1080, "1080p" },
    { 2560, 1440, "1440p" }, { 3840, 2160, "4K" },
};

void fs_changed() {
    changed(ps2_cfg.window_mode == PS2_WIN_EXCLUSIVE ? PS2_CFG_WINDOW : 0);
}

void resolution_controls() {
    if (combo(L("Window mode", "窗口模式"), &ps2_cfg.window_mode, WINDOW_MODES, 3)) {
        if (ps2_cfg.window_mode != PS2_WIN_WINDOWED) ps2_cfg.fullscreen_type = ps2_cfg.window_mode;
        changed(PS2_CFG_WINDOW);
    }
    help(L("Borderless covers the screen at the desktop resolution and switches instantly. "
           "Exclusive changes the display mode itself. F11 or Alt+Enter toggles fullscreen "
           "using whichever of the two you last picked.",
           "无边框全屏以桌面分辨率铺满屏幕，切换即时完成；独占全屏会更改显示器本身的显示模式。"
           "F11 或 Alt+Enter 按你上次选择的那种方式切换全屏。"));

    if (ps2_cfg.window_mode == PS2_WIN_WINDOWED) {
        char preview[48];
        snprintf(preview, sizeof preview, "%d x %d", ps2_cfg.window_w, ps2_cfg.window_h);
        if (ImGui::BeginCombo(L("Window size", "窗口大小"), preview)) {
            for (const WinSize &s : WIN_SIZES) {
                char label[64];
                if (s.w == 640) snprintf(label, sizeof label, "%d x %d  (%s)", s.w, s.h,
                                         L("1x, native", "1x，原生"));
                else if (s.tag[0]) snprintf(label, sizeof label, "%d x %d  (%s)", s.w, s.h, s.tag);
                else snprintf(label, sizeof label, "%d x %d", s.w, s.h);
                bool sel = ps2_cfg.window_w == s.w && ps2_cfg.window_h == s.h;
                if (ImGui::Selectable(label, sel)) {
                    ps2_cfg.window_w = s.w;
                    ps2_cfg.window_h = s.h;
                    changed(PS2_CFG_WINDOW);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        help(L("The window's size in pixels. Dragging the window edge also works and is remembered.",
               "窗口大小（像素）。直接拖动窗口边缘也可以，并且会被记住。"));
    } else if (ps2_cfg.window_mode == PS2_WIN_EXCLUSIVE) {
        SDL_DisplayID d = SDL_GetDisplayForWindow(g_window);
        char preview[64];
        if (ps2_cfg.fs_w <= 0) snprintf(preview, sizeof preview, "%s", L("Desktop resolution", "桌面分辨率"));
        else snprintf(preview, sizeof preview, "%d x %d @ %.0f Hz", ps2_cfg.fs_w, ps2_cfg.fs_h,
                      (double)ps2_cfg.fs_hz);
        if (ImGui::BeginCombo(L("Fullscreen resolution", "全屏分辨率"), preview)) {
            if (ImGui::Selectable(L("Desktop resolution", "桌面分辨率"), ps2_cfg.fs_w <= 0)) {
                ps2_cfg.fs_w = ps2_cfg.fs_h = 0;
                ps2_cfg.fs_hz = 0.0f;
                fs_changed();
            }
            int n = 0;
            SDL_DisplayMode **modes = d ? SDL_GetFullscreenDisplayModes(d, &n) : nullptr;
            for (int i = 0; modes && i < n; i++) {
                const SDL_DisplayMode *m = modes[i];
                if (m->pixel_density != 1.0f) continue;
                char label[64];
                snprintf(label, sizeof label, "%d x %d @ %.0f Hz", m->w, m->h, (double)m->refresh_rate);
                bool sel = ps2_cfg.fs_w == m->w && ps2_cfg.fs_h == m->h
                        && std::fabs(ps2_cfg.fs_hz - m->refresh_rate) < 0.5f;
                ImGui::PushID(i);
                if (ImGui::Selectable(label, sel)) {
                    ps2_cfg.fs_w = m->w;
                    ps2_cfg.fs_h = m->h;
                    ps2_cfg.fs_hz = m->refresh_rate;
                    fs_changed();
                }
                ImGui::PopID();
            }
            SDL_free(modes);
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextDisabled("%s", L("Borderless fullscreen always runs at the desktop resolution.",
                                    "无边框全屏始终使用桌面分辨率。"));
    }
}

const Tr ASPECTS[] = {
    { "Stretch to fill the window", "拉伸铺满窗口" },
    { "Auto (4:3, or 16:9 with widescreen)", "自动（4:3，开启宽屏时为 16:9）" },
    { "4:3", "4:3" }, { "16:9", "16:9" }, { "Custom", "自定义" },
};

void aspect_controls() {
    if (combo(L("Aspect ratio", "画面比例"), &ps2_cfg.aspect, ASPECTS, 5)) changed(0);
    help(L("The shape of the picture inside the window. Auto keeps the PS2's 4:3 picture "
           "undistorted and becomes 16:9 when the widescreen patch is on. Stretch fills the "
           "window whatever its shape, which is how the runtime always behaved.",
           "画面在窗口中的形状。“自动”保持 PS2 原本的 4:3 画面不变形，开启宽屏补丁时变为 16:9。"
           "“拉伸”无论窗口是什么形状都铺满，这是运行时一直以来的做法。"));
    env_note("PS2_ASPECT", L("overrides the aspect ratio", "它会覆盖画面比例"));
    if (ps2_cfg.aspect == PS2_ASPECT_CUSTOM
        && ImGui::SliderFloat(L("Custom ratio", "自定义比例"), &ps2_cfg.aspect_custom, 1.0f, 3.0f,
                              "%.3f : 1"))
        changed(0);
}

void widescreen_control() {
    bool ws = ps2_cfg.widescreen != 0;
    if (ImGui::Checkbox(L("Widescreen (16:9)", "宽屏（16:9）"), &ws)) {
        ps2_cfg.widescreen = ws;
        if (ws && ps2_cfg.aspect == PS2_ASPECT_4_3) ps2_cfg.aspect = PS2_ASPECT_AUTO;
        changed(0);
    }
    help(L("Applies the 16:9 patch from PCSX2's patch database (by nemesis2000) to the game's "
           "own camera, so the 3D view really is wider instead of stretched. Use Auto or 16:9 "
           "aspect with it.",
           "把 PCSX2 补丁库中的 16:9 补丁（作者 nemesis2000）应用到游戏自身的镜头上，3D 视野会真正"
           "变宽，而不是被拉伸。请配合“自动”或“16:9”画面比例使用。"));
    if (ws) {
        static const Tr HUD_LAYOUTS[] = { { "Stretched with the picture", "随画面拉伸" },
                                          { "4:3 in the centre", "4:3 居中" } };
        if (combo(L("HUD layout", "HUD 布局"), &ps2_cfg.hud_layout, HUD_LAYOUTS, 2)) changed(0);
        help(L("The game's HUD is laid out for 4:3. Centred draws the screen-fixed parts -- "
               "tapes, readouts, radar, captions -- at their own proportions in the middle of "
               "the screen; markers placed on things in the world stay on them. Needs the "
               "native 2D layer (on unless PS2_RN_2D=0).",
               "游戏的 HUD 是按 4:3 设计的。“居中”会把固定在屏幕上的部分（刻度带、读数、雷达、字幕）"
               "按原比例画在屏幕中间；标在世界中物体上的标记仍然跟随物体。需要原生 2D 图层"
               "（除非设置了 PS2_RN_2D=0，否则默认开启）。"));
    }
    if (ws && ps2_cfg.aspect == PS2_ASPECT_STRETCH && g_window) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(g_window, &w, &h);
        if (h > 0 && std::fabs((float)w / (float)h - 16.0f / 9.0f) > 0.05f)
            ImGui::TextColored(WARN, "%s", L("This window is not 16:9, so Stretch distorts the picture. Pick Auto.",
                                             "当前窗口不是 16:9，“拉伸”会使画面变形，请选择“自动”。"));
    }
}

const Tr PRESENT_MODES[] = {
    { "Mailbox - low latency, no tearing", "Mailbox - 低延迟，无撕裂" },
    { "VSync (FIFO)", "垂直同步（FIFO）" },
    { "Immediate - tearing, lowest latency", "Immediate - 有撕裂，延迟最低" },
    { "Adaptive VSync (FIFO relaxed)", "自适应垂直同步（FIFO relaxed）" },
};

void present_controls() {
    if (combo(L("Presentation", "画面呈现方式"), &ps2_cfg.present_mode, PRESENT_MODES, 4))
        changed(PS2_CFG_SWAPCHAIN);
    help(L("The game always runs at the PS2's 59.94 fields per second; this only decides how "
           "finished frames reach the screen. A mode your display does not offer falls back "
           "to VSync.",
           "游戏始终以 PS2 的每秒 59.94 场运行，这里只决定画好的帧如何送到屏幕上。"
           "显示器不支持的模式会退回垂直同步。"));
    env_note("PS2_PRESENT_MODE", L("overrides the presentation mode", "它会覆盖画面呈现方式"));
    bool lim = ps2_cfg.fps_limit > 0;
    if (ImGui::Checkbox(L("Limit frame rate", "限制帧率"), &lim)) {
        ps2_cfg.fps_limit = lim ? 60 : 0;
        changed(PS2_CFG_FPS);
    }
    if (lim) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::SliderInt("##fpslimit", &ps2_cfg.fps_limit, 20, 240, "%d FPS"))
            changed(PS2_CFG_FPS);
    }
    help(L("Shows fewer frames. Below 60, frames are skipped evenly -- every other one at 30 -- "
           "while the game keeps running at the PS2's 59.94 fields a second underneath. It saves "
           "only the final presentation, so it cannot make a slow machine run the game faster; "
           "leave it off unless you want an even, lower frame rate.",
           "显示更少的帧。低于 60 时会均匀跳帧（30 时每两帧显示一帧），而游戏本身仍以 PS2 的每秒 "
           "59.94 场运行。它只省下最后的画面呈现，无法让慢的电脑把游戏跑得更快；除非你想要稳定的"
           "较低帧率，否则请保持关闭。"));
    env_note("PS2_FPS_CAP", L("overrides the frame-rate limit", "它会覆盖帧率限制"));
}

struct PresetInfo { const char *name, *blurb, *zh_name, *zh_blurb; };
const PresetInfo PRESETS[4] = {
    { "Low",    "The PS2's native 640x448 with plain bilinear scaling to the window -- the "
                "renderer as it was before this menu. Lightest on the GPU; the one for "
                "integrated graphics.",
      "低", "PS2 原生 640x448，用普通双线性缩放到窗口，也就是有这个菜单之前的渲染方式。"
            "GPU 负担最轻，适合集成显卡。" },
    { "Medium", "2x internal resolution (1280x896) for clean edges, lines and effects, and "
                "4x anisotropic filtering.",
      "中", "2 倍内部分辨率（1280x896），边缘、线条和特效更干净，4 倍各向异性过滤。" },
    { "High",   "3x internal resolution (1920x1344), 8x anisotropic filtering, sharp-bilinear "
                "scaling and light sharpening.",
      "高", "3 倍内部分辨率（1920x1344），8 倍各向异性过滤，锐利双线性缩放，轻度锐化。" },
    { "Ultra",  "4x internal resolution (2560x1792), averaged down to the window for "
                "anti-aliasing, 16x anisotropic filtering and light sharpening. Uses the "
                "most video memory.",
      "极高", "4 倍内部分辨率（2560x1792），缩小到窗口时平均采样以抗锯齿，16 倍各向异性过滤，"
              "轻度锐化。占用显存最多。" },
};

const char *preset_name(int i) { return L(PRESETS[i].name, PRESETS[i].zh_name); }
const char *preset_blurb(int i) { return L(PRESETS[i].blurb, PRESETS[i].zh_blurb); }

void internal_res_status() {
    ps2_render_info ri;
    ps2_video_render_info(&ri);
    ImGui::TextDisabled(L("Drawing at %u x %u (%ux the PS2's %u x %u), about %.0f MB of video memory.",
                          "当前绘制分辨率 %u x %u（PS2 的 %u 倍，原始 %u x %u），约占 %.0f MB 显存。"),
                        ri.guest_w * ri.scale, ri.guest_h * ri.scale, ri.scale,
                        ri.guest_w, ri.guest_h,
                        (double)(ri.bytes_at_1x * ri.scale * ri.scale) / (1024.0 * 1024.0));
    if (ri.failed_scale)
        ImGui::TextColored(WARN, L("%ux did not fit in video memory, so it stays at %ux.",
                                   "%u 倍超出了显存容量，因此保持在 %u 倍。"),
                           ri.failed_scale, ri.scale);
}

void internal_res_control() {
    ps2_render_info ri;
    ps2_video_render_info(&ri);
    static const int VALUES[] = { 0, 1, 2, 3, 4, 5, 6, 8 };
    auto label = [&ri](int v, char *buf, size_t n) {
        double mb = (double)(ri.bytes_at_1x * (uint64_t)(v * v)) / (1024.0 * 1024.0);
        if (v <= 0)
            snprintf(buf, n, L("Auto - cover the window (%ux now)", "自动 - 覆盖窗口（当前 %u 倍）"),
                     ri.auto_scale < ri.max_scale ? ri.auto_scale : ri.max_scale);
        else if (v == 1)
            snprintf(buf, n, L("Native - %u x %u", "原生 - %u x %u"), ri.guest_w, ri.guest_h);
        else
            snprintf(buf, n, L("%dx - %u x %u  (about %.0f MB)", "%d 倍 - %u x %u（约 %.0f MB）"),
                     v, ri.guest_w * v, ri.guest_h * v, mb);
    };
    char preview[96];
    label(ps2_cfg.internal_res, preview, sizeof preview);
    if (ImGui::BeginCombo(L("Internal resolution", "内部分辨率"), preview)) {
        for (int v : VALUES) {
            char buf[96];
            label(v, buf, sizeof buf);
            bool too_big = v > (int)ri.max_scale;
            if (too_big) ImGui::BeginDisabled();
            if (ImGui::Selectable(buf, ps2_cfg.internal_res == v)) {
                ps2_cfg.internal_res = v;
                gfx_changed(0);
            }
            if (too_big) ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    help(L("How many pixels the game is drawn with before it is scaled to the window. Native is "
           "the PS2's own 640x448. 2x and up draw the same frame with that many times the pixels "
           "in each direction, so polygon edges, HUD lines and the effects the game renders into "
           "its own buffers come out sharp; the textures themselves keep the game's resolution. "
           "Above the window's size the extra pixels are averaged down, which anti-aliases. Video "
           "memory and GPU time grow with the square of the factor. Auto picks the smallest factor "
           "that covers the window.",
           "游戏缩放到窗口之前用多少像素绘制。“原生”即 PS2 自己的 640x448。2 倍及以上会在每个方向上"
           "用相应倍数的像素绘制同一帧，因此多边形边缘、HUD 线条以及游戏渲染到自身缓冲区的特效都会"
           "变清晰；贴图本身仍保持游戏原有的分辨率。超出窗口尺寸的多余像素会被平均缩小，起到抗锯齿"
           "作用。显存和 GPU 耗时随倍数的平方增长。“自动”会选择刚好覆盖窗口的最小倍数。"));
    env_note("PS2_UPSCALE", L("overrides the internal resolution", "它会覆盖内部分辨率"));
    internal_res_status();
}

void language_control() {
    static const char *const LANGS[] = { "English", "简体中文" };
    if (!g_font_zh) {
        ImGui::TextDisabled("Language: English (no Chinese font in the Windows Fonts folder)");
        return;
    }
    if (ImGui::Combo("Language / 语言", &ps2_cfg.language, LANGS, 2)) changed(0);
}

void tab_quick() {
    ImGuiStyle &st = ImGui::GetStyle();
    language_control();
    ImGui::SeparatorText(L("Graphics quality", "画质"));
    float w = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x * 3.0f) / 4.0f;
    float h = ImGui::GetFrameHeight() * 2.2f;
    for (int i = PS2_PRESET_ULTRA; i >= PS2_PRESET_LOW; i--) {
        bool sel = ps2_cfg.preset == i;
        if (sel) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.52f, 0.32f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.60f, 0.38f, 1.0f));
        }
        if (ImGui::Button(preset_name(i), ImVec2(w, h))) {
            ps2_settings_apply_preset(&ps2_cfg, i);
            changed(PS2_CFG_SAMPLER);
        }
        if (sel) ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("%s", preset_blurb(i));
        if (i > PS2_PRESET_LOW) ImGui::SameLine();
    }
    if (ps2_cfg.preset == PS2_PRESET_CUSTOM)
        ImGui::TextWrapped("%s", L("Custom: options have been changed individually on the other tabs.",
                                   "自定义：已在其他页面单独修改过选项。"));
    else
        ImGui::TextWrapped(L("%s: %s", "%s：%s"), preset_name(ps2_cfg.preset),
                           preset_blurb(ps2_cfg.preset));
    internal_res_status();

    ImGui::SeparatorText(L("Display", "显示"));
    resolution_controls();
    aspect_controls();
    widescreen_control();
    present_controls();
    bool fps = ps2_cfg.show_fps != 0;
    if (ImGui::Checkbox(L("Show FPS counter", "显示帧率"), &fps)) { ps2_cfg.show_fps = fps; changed(0); }

    ImGui::SeparatorText(L("Controls", "操作"));
    if (ImGui::Button(L("Keyboard & mouse", "键盘和鼠标"))) g_want_tab = TAB_KEYBOARD;
    ImGui::SameLine();
    if (ImGui::Button(L("Controller bindings", "手柄按键"))) g_want_tab = TAB_CONTROLLER;
    ImGui::SameLine();
    if (ImGui::Button(L("Dead zones", "死区"))) g_want_tab = TAB_ANALOG;

    ImGui::Spacing();
    ImGui::Separator();
    const char *reset_id = L("Reset all settings?###resetall", "恢复全部默认设置？###resetall");
    if (ImGui::Button(L("Reset all settings to defaults", "恢复全部默认设置"))) ImGui::OpenPopup(reset_id);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(reset_id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(L("Graphics, display, bindings and dead zones all return to their defaults.",
                                 "画质、显示、按键绑定和死区都会恢复为默认值。"));
        if (ImGui::Button(L("Reset", "恢复"), ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f))) {
            int lang = ps2_cfg.language;
            ps2_settings_defaults(&ps2_cfg);
            ps2_cfg.language = lang;
            changed(PS2_CFG_SWAPCHAIN | PS2_CFG_WINDOW | PS2_CFG_SAMPLER | PS2_CFG_FPS);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(L("Cancel", "取消"), ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void tab_display() {
    ImGui::SeparatorText(L("Window", "窗口"));
    resolution_controls();

    ImGui::SeparatorText(L("Picture", "画面"));
    aspect_controls();
    widescreen_control();
    bool is = ps2_cfg.integer_scale != 0;
    if (ImGui::Checkbox(L("Integer scaling", "整数倍缩放"), &is)) { ps2_cfg.integer_scale = is; changed(0); }
    help(L("Scale only by whole multiples of the game's 448 lines, leaving a border instead of "
           "resampling unevenly. Combine with Nearest for exact pixels.",
           "只按游戏 448 行的整数倍缩放，留出黑边而不做不均匀的重采样。配合“最近邻”可得到精确像素。"));
    static const Tr SCALE[] = {
        { "Nearest - blocky, exact pixels", "最近邻 - 方块感，像素精确" },
        { "Bilinear - smooth", "双线性 - 平滑" },
        { "Sharp bilinear - crisp, even pixels", "锐利双线性 - 清晰，像素均匀" },
    };
    if (combo(L("Scaling filter", "缩放滤镜"), &ps2_cfg.scale_filter, SCALE, 3)) gfx_changed(0);
    help(L("How the rendered picture is resized to the window. Sharp bilinear keeps pixels crisp "
           "at any size without the uneven pixel widths of Nearest. A picture drawn larger than "
           "the window is averaged down with any filter except Nearest.",
           "渲染好的画面如何缩放到窗口。“锐利双线性”在任何尺寸下都保持像素清晰，又没有“最近邻”那种"
           "像素宽窄不一的问题。画面比窗口大时，除“最近邻”外的滤镜都会平均缩小。"));

    ImGui::SeparatorText(L("Presentation", "画面呈现"));
    present_controls();

    ImGui::SeparatorText(L("Interface", "界面"));
    language_control();
    bool fps = ps2_cfg.show_fps != 0;
    if (ImGui::Checkbox(L("Show FPS counter", "显示帧率"), &fps)) { ps2_cfg.show_fps = fps; changed(0); }
    if (ImGui::SliderFloat(L("Menu text size", "菜单文字大小"), &ps2_cfg.ui_scale, 0.75f, 2.0f, "%.2fx"))
        changed(0);
}

void tab_rendering() {
    ImGui::SeparatorText(L("Resolution", "分辨率"));
    internal_res_control();

    ImGui::SeparatorText(L("Textures", "贴图"));
    static const Tr TF[] = {
        { "As the game requests (accurate)", "按游戏要求（准确）" },
        { "Force nearest (pixelated)", "强制最近邻（像素化）" },
        { "Force bilinear (smooth)", "强制双线性（平滑）" },
    };
    if (combo(L("Texture filtering", "贴图过滤"), &ps2_cfg.tex_filter, TF, 3)) gfx_changed(0);
    help(L("The PS2 picks nearest or bilinear filtering per draw. Forcing one applies to the 3D "
           "world's textures. The HUD, menus and text keep what the game asked for: they are 2D "
           "art laid out texel by texel, and smoothing them pulls in the neighbouring pictures "
           "(lines across the minimap). So do effects such as the clouds.",
           "PS2 每次绘制时自行选择最近邻或双线性过滤。强制设置只作用于 3D 世界的贴图。HUD、菜单和"
           "文字保持游戏的要求：它们是逐纹素排布的 2D 图像，平滑处理会把相邻的图案混进来（小地图上"
           "会出现线条）。云等特效也是如此。"));

    static const int AF_VALUES[] = { 0, 2, 4, 8, 16 };
    const char *const AF[] = { L("Off", "关"), "2x", "4x", "8x", "16x" };
    int idx = 0;
    for (int i = 0; i < 5; i++)
        if (AF_VALUES[i] == ps2_cfg.anisotropy) idx = i;
    bool unsupported = g_max_aniso < 2.0f;
    if (unsupported) ImGui::BeginDisabled();
    if (ImGui::BeginCombo(L("Anisotropic filtering", "各向异性过滤"), AF[idx])) {
        for (int i = 0; i < 5; i++) {
            if (AF_VALUES[i] > 0 && (float)AF_VALUES[i] > g_max_aniso) continue;
            if (ImGui::Selectable(AF[i], i == idx)) {
                ps2_cfg.anisotropy = AF_VALUES[i];
                gfx_changed(PS2_CFG_SAMPLER);
            }
        }
        ImGui::EndCombo();
    }
    if (unsupported) ImGui::EndDisabled();
    help(unsupported ? L("This GPU does not support anisotropic filtering.", "这块 GPU 不支持各向异性过滤。")
                     : L("Sharpens textures seen at a steep angle -- mostly the ground running "
                         "towards the horizon. Costs a little GPU time.",
                         "让以倾斜角度看到的贴图更清晰，主要是延伸到地平线的地面。会占用少量 GPU 时间。"));

    env_note("PS2_NO_MIP", L("turns the game's mipmaps off, which garbles distant terrain",
                             "它会关闭游戏的 mipmap，导致远处地形显示错乱"));

    ImGui::SeparatorText(L("Frames", "帧"));
    bool di = ps2_cfg.deinterlace != 0;
    if (ImGui::Checkbox(L("Deinterlace", "反交错"), &di)) { ps2_cfg.deinterlace = di; gfx_changed(0); }
    help(L("The game draws alternate lines each field, as an interlaced TV expects. On weaves "
           "the two back into one steady frame; off shows the raw fields, which bob by a line.",
           "游戏每一场只画隔行的线条，这是为隔行扫描电视设计的。开启时把两场合成一帧稳定的画面；"
           "关闭时显示原始的场，画面会上下跳动一行。"));
}

void tab_post() {
    ImGui::SeparatorText(L("Anti-aliasing and sharpness", "抗锯齿与锐度"));
    bool fx = ps2_cfg.fxaa != 0;
    if (ImGui::Checkbox("FXAA", &fx)) { ps2_cfg.fxaa = fx; gfx_changed(0); }
    help(L("Fast approximate anti-aliasing on the final picture: softens jagged edges on "
           "aircraft and the horizon, and can soften fine HUD text a little.",
           "对最终画面做快速近似抗锯齿：柔化飞机和地平线上的锯齿，也可能让细小的 HUD 文字略微变糊。"));
    if (ImGui::SliderFloat(L("Sharpening", "锐化"), &ps2_cfg.sharpen, 0.0f, 1.0f, "%.2f")) gfx_changed(0);
    help(L("Contrast-limited sharpening of the final picture. Counteracts the softness of "
           "scaling 640x448 up to a large window.",
           "对最终画面做限制对比度的锐化，抵消把 640x448 放大到大窗口时的模糊感。"));

    ImGui::SeparatorText(L("Colour", "色彩"));
    bool c = false;
    c |= ImGui::SliderFloat(L("Brightness", "亮度"), &ps2_cfg.brightness, -0.5f, 0.5f, "%+.2f");
    c |= ImGui::SliderFloat(L("Contrast", "对比度"), &ps2_cfg.contrast, 0.5f, 1.5f, "%.2f");
    c |= ImGui::SliderFloat(L("Gamma", "伽马"), &ps2_cfg.gamma, 0.5f, 2.0f, "%.2f");
    c |= ImGui::SliderFloat(L("Saturation", "饱和度"), &ps2_cfg.saturation, 0.0f, 2.0f, "%.2f");
    if (c) changed(0);
    if (ImGui::Button(L("Reset colour", "重置色彩"))) {
        ps2_cfg.brightness = 0.0f;
        ps2_cfg.contrast = 1.0f;
        ps2_cfg.gamma = 1.0f;
        ps2_cfg.saturation = 1.0f;
        changed(0);
    }
}

void mouse_controls() {
    ImGui::SeparatorText(L("Mouse", "鼠标"));
    ImGui::TextWrapped("%s", L(
        "While the game has focus and this menu is closed, the mouse is captured. "
        "Mouse left/right: L2 / R2. Mouse up/down: left stick up/down. Hold Space: "
        "the mouse moves the right stick instead. Left button: Cross, right button: "
        "Triangle, mouse 4: Circle, mouse 5: Square, wheel: Select. Alt + the "
        "left-stick keys press the D-pad. A key held for an axis wins over the mouse. "
        "Square, from any key, button or controller, latches: each press steps it from "
        "released to held at half pressure, to held at full pressure, to released.",
        "游戏窗口在前台且本菜单关闭时，鼠标会被锁定在窗口内。鼠标左右：L2 / R2。鼠标上下：左摇杆"
        "上下。按住空格：鼠标改为控制右摇杆。左键：交叉键，右键：三角键，侧键 4：圆圈键，侧键 5："
        "方块键，滚轮：选择键。Alt + 左摇杆按键：方向键。同一个轴上，按住的键盘按键优先于鼠标。"
        "方块键（无论来自键盘、鼠标还是手柄）每按一次切换一档：50% 力度按住 → 100% 力度按住 → 松开。"));
    bool ch = false;
    bool en = ps2_cfg.mouse_enabled != 0;
    if (ImGui::Checkbox(L("Use the mouse", "使用鼠标"), &en)) { ps2_cfg.mouse_enabled = en; ch = true; }
    ch |= ImGui::SliderFloat(L("Mouse sensitivity", "鼠标灵敏度"), &ps2_cfg.mouse_sens, 0.1f, 5.0f, "%.2fx");
    ch |= ImGui::SliderFloat(L("Return to centre", "回中速度"), &ps2_cfg.mouse_recenter, 0.0f, 30.0f,
                             L("%.1f / s", "%.1f / 秒"));
    help(L("How fast L2/R2 and the left stick settle back once the mouse stops. High: the stick "
           "follows how fast you move the mouse. 0: it stays where the mouse put it, like a real "
           "stick you let go of. The right stick (Space) always holds until Space is released.",
           "鼠标停下后，L2/R2 和左摇杆回到中间的速度。数值高：摇杆推多远取决于鼠标移动得多快。0："
           "鼠标推到哪里摇杆就停在哪里，像一根松手后不回弹的摇杆。右摇杆（按住空格时）始终停在原处，"
           "直到松开空格。"));
    ch |= slider_pct(L("Mouse dead zone", "鼠标死区"), &ps2_cfg.mouse_deadzone, 0.0f, 0.5f);
    help(L("Small mouse movement inside this is ignored, so a resting hand does not nudge the "
           "aircraft or tap L2/R2.",
           "小于这个范围的鼠标移动会被忽略，手放在鼠标上的轻微晃动不会推动飞机或误触 L2/R2。"));
    bool inv = ps2_cfg.mouse_invert_y != 0;
    if (ImGui::Checkbox(L("Invert up/down (mouse up = stick down, nose up)",
                          "上下反向（鼠标向上 = 摇杆向下 = 机头抬起）"), &inv)) {
        ps2_cfg.mouse_invert_y = inv;
        ch = true;
    }
    if (ch) changed(0);
}

void tab_keyboard() {
    mouse_controls();
    ImGui::SeparatorText(L("Keys", "按键"));
    ImGui::TextWrapped("%s", L("Click a binding, then press the key you want. Esc cancels; Backspace or a "
                               "right-click clears it. Each key drives one action, so binding a key "
                               "again moves it here.",
                               "点击一个绑定，然后按下想要的键。Esc 取消，Backspace 或右键清除。每个键只对应"
                               "一个动作，所以把已用的键绑到这里会把它从原来的动作上移走。"));
    if (ImGui::Button(L("Reset keyboard to defaults", "恢复默认键盘按键"))) {
        ps2_settings_default_bindings(&ps2_cfg, 1, 0);
        changed(0);
    }
    binding_table(false);
}

void tab_controller() {
    SDL_Gamepad *cur = ps2_video_gamepad();
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    const char *preview = cur ? SDL_GetGamepadName(cur) : L("No controller connected", "未连接手柄");
    if (!preview) preview = L("Controller", "手柄");
    if (ImGui::BeginCombo(L("Controller", "手柄"), preview)) {
        for (int i = 0; ids && i < n; i++) {
            const char *name = SDL_GetGamepadNameForID(ids[i]);
            char label[192];
            snprintf(label, sizeof label, "%s##%u", name ? name : L("Controller", "手柄"), (unsigned)ids[i]);
            bool sel = cur && SDL_GetGamepadID(cur) == ids[i];
            if (ImGui::Selectable(label, sel)) ps2_video_select_gamepad(ids[i]);
        }
        ImGui::EndCombo();
    }
    SDL_free(ids);
    bool block = ps2_cfg.block_input_in_menu != 0;
    if (ImGui::Checkbox(L("Hold the game's controls while this menu is open",
                          "打开本菜单时暂停向游戏发送操作"), &block)) {
        ps2_cfg.block_input_in_menu = block;
        changed(0);
    }
    ImGui::TextWrapped("%s", L("Click a binding, then press a button, or push a stick or trigger in the "
                               "direction you want. Esc cancels; Backspace or a right-click clears it.",
                               "点击一个绑定，然后按下按钮，或把摇杆、扳机推向想要的方向。Esc 取消，"
                               "Backspace 或右键清除。"));
    if (ImGui::Button(L("Reset controller to defaults", "恢复默认手柄按键"))) {
        ps2_settings_default_bindings(&ps2_cfg, 0, 1);
        changed(0);
    }
    binding_table(true);
}

void stick_view(int s, float size) {
    SDL_Gamepad *g = ps2_video_gamepad();
    int ax = 0, ay = 0;
    if (g) {
        ax = SDL_GetGamepadAxis(g, s ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX);
        ay = SDL_GetGamepadAxis(g, s ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY);
    }
    const float v[4] = { ps2_pad_axis_value(ay, 0), ps2_pad_axis_value(ay, 1),
                         ps2_pad_axis_value(ax, 0), ps2_pad_axis_value(ax, 1) };
    unsigned char bx, by;
    ps2_pad_stick(ps2_cfg.deadzone, ps2_cfg.anti_deadzone, ps2_cfg.axis_scale, ps2_cfg.invert[s],
                  v, &bx, &by);
    float rx = v[3] - v[2], ry = v[1] - v[0];
    float ox = bx < 127 ? -(127 - bx) / 127.0f : (bx - 127) / 128.0f;
    float oy = by < 127 ? -(127 - by) / 127.0f : (by - 127) / 128.0f;

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = size * 0.5f - 4.0f;
    ImVec2 ctr = p + ImVec2(size * 0.5f, size * 0.5f);
    dl->AddRectFilled(p, p + ImVec2(size, size), IM_COL32(18, 24, 22, 255), 6.0f);
    float scale = ps2_cfg.axis_scale > 0.01f ? ps2_cfg.axis_scale : 0.01f;
    float dzr = ps2_cfg.deadzone / scale;
    if (dzr > 0.0f)
        dl->AddCircleFilled(ctr, r * (dzr < 1.0f ? dzr : 1.0f), IM_COL32(190, 70, 70, 80), 48);
    dl->AddLine(ctr - ImVec2(r, 0.0f), ctr + ImVec2(r, 0.0f), IM_COL32(60, 72, 66, 255));
    dl->AddLine(ctr - ImVec2(0.0f, r), ctr + ImVec2(0.0f, r), IM_COL32(60, 72, 66, 255));
    dl->AddCircle(ctr, r, IM_COL32(130, 150, 140, 255), 64, 1.5f);
    if (scale > 1.0f) dl->AddCircle(ctr, r / scale, IM_COL32(100, 120, 110, 200), 64, 1.0f);
    if (ps2_cfg.anti_deadzone > 0.0f)
        dl->AddCircle(ctr, r * ps2_cfg.anti_deadzone, IM_COL32(90, 150, 230, 200), 48, 1.5f);
    dl->AddCircleFilled(ctr + ImVec2(rx * r, ry * r), 4.0f, IM_COL32(210, 210, 210, 220));
    dl->AddCircleFilled(ctr + ImVec2(ox * r, oy * r), 5.5f, IM_COL32(100, 235, 130, 255));
    ImGui::Text("X %3u  Y %3u", (unsigned)bx, (unsigned)by);
}

void tab_analog() {
    ImGui::TextWrapped("%s", L("These are PCSX2's DualShock 2 analog settings, and the game receives what "
                               "PCSX2 would send it for the same controller: sensitivity is applied first, "
                               "a stick inside the dead zone's circle reads as centred, and movement "
                               "outside it is passed on as it is, not rescaled. The defaults are PCSX2's: "
                               "no dead zone and 133% sensitivity. They apply to the mouse's stick "
                               "movement too.",
                               "这些是 PCSX2 的 DualShock 2 模拟摇杆设置，游戏收到的数值与同一个手柄在 PCSX2 "
                               "上发出的相同：先乘以灵敏度，落在死区圆内的摇杆视为回中，圆外的移动原样传递，"
                               "不重新缩放。默认值与 PCSX2 相同：无死区，灵敏度 133%。鼠标控制的摇杆也受这些"
                               "设置影响。"));
    env_note("PS2_PAD_DEADZONE", L("overrides the analog dead zone", "它会覆盖摇杆死区"));
    if (!ps2_video_gamepad())
        ImGui::TextDisabled("%s", L("Connect a controller to see the live preview.",
                                    "连接手柄即可看到实时预览。"));
    else
        ImGui::TextDisabled("%s", L("Preview: grey is the raw stick, green is what the game receives.",
                                    "预览：灰点是摇杆原始位置，绿点是游戏收到的位置。"));

    static const Tr INVERT[] = {
        { "Not inverted", "不反向" }, { "Invert left/right", "左右反向" },
        { "Invert up/down", "上下反向" }, { "Invert left/right + up/down", "左右和上下都反向" },
    };
    bool ch = false;
    ch |= slider_pct(L("Analog dead zone", "摇杆死区"), &ps2_cfg.deadzone, 0.0f, 1.0f);
    help(L("PCSX2's Analog Deadzone: the fraction of stick movement that is ignored, for both "
           "sticks. It is a circle, measured after sensitivity. Raise it if the aircraft drifts "
           "with the stick released.",
           "PCSX2 的 Analog Deadzone：被忽略的摇杆行程比例，对两个摇杆都有效。它是一个圆，在乘以"
           "灵敏度之后计算。如果松开摇杆后飞机仍在漂移，就调高它。"));
    ch |= slider_pct(L("Analog anti-dead zone", "摇杆反死区"), &ps2_cfg.anti_deadzone, 0.0f, 0.9f);
    help(L("A stick pushed at all is moved out to at least this much of its travel, keeping its "
           "direction, so small movements get past the game's own dead zone. Full deflection is "
           "unchanged. A controller whose stick does not rest exactly at the centre also needs "
           "an analog dead zone, or the aircraft will drift. Shown as the blue circle below.",
           "摇杆只要被推动，就至少输出这个比例的行程（方向不变），让细小的移动越过游戏自带的死区。"
           "推到底时不受影响。手柄摇杆松开后不能精确回中时，还需要配合设置摇杆死区，否则飞机会漂移。"
           "下方预览中的蓝色圆圈表示这个范围。"));
    ch |= slider_pct(L("Analog sensitivity", "摇杆灵敏度"), &ps2_cfg.axis_scale, 0.01f, 2.0f);
    help(L("PCSX2's Analog Sensitivity: the stick axis scaling factor. PCSX2 recommends 130% to "
           "140% for recent controllers such as the DualShock 4 or Xbox One controller.",
           "PCSX2 的 Analog Sensitivity：摇杆轴的缩放系数。PCSX2 建议 DualShock 4、Xbox One 等较新"
           "的手柄使用 130% 到 140%。"));
    ch |= slider_pct(L("Button / trigger dead zone", "按键 / 扳机死区"), &ps2_cfg.button_deadzone, 0.0f, 1.0f);
    help(L("PCSX2's Button/Trigger Deadzone: how far a trigger, or a stick bound to a button, "
           "travels before it counts as pressed at all.",
           "PCSX2 的 Button/Trigger Deadzone：扳机（或绑定到按键的摇杆）要推多远才算按下。"));
    ch |= combo(L("Invert left stick", "左摇杆反向"), &ps2_cfg.invert[0], INVERT, 4);
    ch |= combo(L("Invert right stick", "右摇杆反向"), &ps2_cfg.invert[1], INVERT, 4);
    if (ImGui::Button(L("Reset to PCSX2 defaults", "恢复 PCSX2 默认值"))) {
        ps2_settings d;
        ps2_settings_defaults(&d);
        ps2_cfg.deadzone = d.deadzone;
        ps2_cfg.anti_deadzone = d.anti_deadzone;
        ps2_cfg.axis_scale = d.axis_scale;
        ps2_cfg.button_deadzone = d.button_deadzone;
        ps2_cfg.invert[0] = d.invert[0];
        ps2_cfg.invert[1] = d.invert[1];
        ch = true;
    }
    if (ch) changed(0);

    float view = ImGui::GetFontSize() * 11.0f;
    if (ImGui::BeginTable("sticks", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (int s = 0; s < 2; s++) {
            ImGui::TableNextColumn();
            ImGui::PushID(s);
            ImGui::TextUnformatted(s ? L("Right stick", "右摇杆") : L("Left stick", "左摇杆"));
            stick_view(s, view);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (SDL_Gamepad *g = ps2_video_gamepad()) {
        ImGui::SeparatorText(L("Triggers", "扳机"));
        for (int t = 0; t < 2; t++) {
            int raw = SDL_GetGamepadAxis(g, t ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
                                              : SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
            float v = ps2_pad_axis_value(raw, 1);
            unsigned char pressure;
            int held = ps2_pad_trigger(ps2_cfg.button_deadzone, v, &pressure);
            char overlay[96];
            snprintf(overlay, sizeof overlay, L("%s raw %.0f%%  %s, pressure %u", "%s 原始 %.0f%%  %s，压力 %u"),
                     t ? "R2" : "L2", (double)(v * 100.0f),
                     held ? L("held", "按下") : L("released", "松开"), (unsigned)pressure);
            ImGui::ProgressBar(v, ImVec2(-FLT_MIN, 0.0f), overlay);
        }
    }
}

void draw_fps() {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(10.0f * g_dpi, 10.0f * g_dpi), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                             | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
                             | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##fps", nullptr, f))
        ImGui::Text("%.0f FPS  %.1f ms", (double)io.Framerate,
                    io.Framerate > 0.0f ? 1000.0 / (double)io.Framerate : 0.0);
    ImGui::End();
}

void draw_menu() {
    ImGuiIO &io = ImGui::GetIO();
    float s = g_dpi * ps2_cfg.ui_scale;
    ImVec2 size(std::fmin(io.DisplaySize.x * 0.94f, 880.0f * s),
                std::fmin(io.DisplaySize.y * 0.92f, 700.0f * s));
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::Begin(L("Ace Combat 5 - Settings###ps2settings", "皇牌空战5 - 设置###ps2settings"),
                     &open, ImGuiWindowFlags_NoCollapse)) {
        float footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
        if (ImGui::BeginTabBar("tabs")) {
            /* "###" keeps each tab's id the same in both languages */
            static const Tr TABS[] = {
                { "Quick###t0", "快速设置###t0" }, { "Display###t1", "显示###t1" },
                { "Rendering###t2", "渲染###t2" }, { "Post-processing###t3", "后期处理###t3" },
                { "Keyboard & mouse###t4", "键盘和鼠标###t4" }, { "Controller###t5", "手柄###t5" },
                { "Analog & dead zones###t6", "摇杆与死区###t6" },
            };
            static void (*const PAGES[])() = {
                tab_quick, tab_display, tab_rendering, tab_post,
                tab_keyboard, tab_controller, tab_analog,
            };
            for (int i = 0; i < 7; i++) {
                ImGuiTabItemFlags f = g_want_tab == i ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(L(TABS[i].en, TABS[i].zh), nullptr, f)) {
                    if (ImGui::BeginChild("page", ImVec2(0.0f, -footer))) PAGES[i]();
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
            g_want_tab = TAB_NONE;
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        ImGui::TextDisabled(L("F4 or Esc closes  |  F11 or Alt+Enter toggles fullscreen  |  %s",
                              "F4 或 Esc 关闭  |  F11 或 Alt+Enter 切换全屏  |  %s"),
                            ps2_settings_path());
    }
    ImGui::End();
    if (!open) ps2_ui_set_visible(0);
}

}

int ps2_ui_init(const ps2_ui_init_info *info) {
    if (g_init) return 0;
    if (!info || !info->window) return -1;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad
                    | ImGuiConfigFlags_NoMouseCursorChange;
    load_cjk_font(io);
    g_window = info->window;
    g_max_aniso = info->max_anisotropy;
    g_dpi = SDL_GetWindowDisplayScale(info->window);
    if (!(g_dpi > 0.0f)) g_dpi = 1.0f;
    apply_style();
    ImGui::GetStyle().ScaleAllSizes(g_dpi);
    ImGui::GetStyle().FontScaleDpi = g_dpi;

    if (!ImGui_ImplSDL3_InitForVulkan(info->window)) {
        ps2_log("ui: the Dear ImGui SDL3 backend did not initialise");
        ImGui::DestroyContext();
        return -1;
    }
    g_color_format = info->color_format;
    ImGui_ImplVulkan_InitInfo vi = {};
    vi.ApiVersion = VK_API_VERSION_1_3;
    vi.Instance = info->instance;
    vi.PhysicalDevice = info->phys;
    vi.Device = info->device;
    vi.QueueFamily = info->queue_family;
    vi.Queue = info->queue;
    vi.DescriptorPoolSize = 32;
    vi.MinImageCount = info->image_count >= 2 ? info->image_count : 2;
    vi.ImageCount = vi.MinImageCount;
    vi.UseDynamicRendering = true;
    vi.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    vi.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    vi.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_color_format;
    vi.CheckVkResultFn = check_vk;
    if (!ImGui_ImplVulkan_Init(&vi)) {
        ps2_log("ui: the Dear ImGui Vulkan backend did not initialise");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return -1;
    }
    g_init = true;
    ps2_log("ui: Dear ImGui %s ready -- F4 opens the settings menu", IMGUI_VERSION);
    if (const char *e = getenv("PS2_UI_OPEN")) {
        ps2_ui_set_visible(1);
        g_want_tab = atoi(e);
    }
    return 0;
}

void ps2_ui_shutdown(void) {
    if (!g_init) return;
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (g_cjk_data) { IM_FREE(g_cjk_data); g_cjk_data = nullptr; }
    g_font_zh = nullptr;
    g_init = false;
    g_visible = false;
}

int ps2_ui_visible(void) { return g_init && g_visible; }

void ps2_ui_set_visible(int on) {
    if (!g_init || (on != 0) == g_visible) return;
    g_visible = on != 0;
    ImGuiIO &io = ImGui::GetIO();
    io.ClearEventsQueue();
    io.ClearInputKeys();
    g_cap = CAP_NONE;
    if (g_visible) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        SDL_ShowCursor();
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    }
}

int ps2_ui_blocks_game_input(void) {
    return g_init && g_visible && ps2_cfg.block_input_in_menu;
}

int ps2_ui_event(const SDL_Event *e) {
    if (!g_init) return 0;
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_F4 && !e->key.repeat) {
        ps2_ui_set_visible(!g_visible);
        return 1;
    }
    if (g_cap != CAP_NONE && capture_event(e)) return 1;
    if (!g_visible) {
        if (e->type == SDL_EVENT_GAMEPAD_ADDED || e->type == SDL_EVENT_GAMEPAD_REMOVED)
            ImGui_ImplSDL3_ProcessEvent(e);
        return 0;
    }
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE) {
        ps2_ui_set_visible(0);
        return 1;
    }
    ImGui_ImplSDL3_ProcessEvent(e);
    return 0;
}

void ps2_ui_draw(VkCommandBuffer cmd) {
    if (!g_init || (!g_visible && !ps2_cfg.show_fps)) return;
    ImGuiIO &io = ImGui::GetIO();
    if (g_cap == CAP_PAD) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    ImGui::GetStyle().FontScaleMain = ps2_cfg.ui_scale;
    update_capture_arming();

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (ps2_cfg.show_fps) draw_fps();
    if (g_visible) {
        g_zh = ps2_cfg.language == 1 && g_font_zh;
        if (g_zh) ImGui::PushFont(g_font_zh, ImGui::GetStyle().FontSizeBase * 1.15f);
        draw_menu();
        draw_capture_popup();
        if (g_zh) ImGui::PopFont();
    }
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

void ps2_ui_swapchain_changed(uint32_t image_count) {
    if (g_init && image_count >= 2) ImGui_ImplVulkan_SetMinImageCount(image_count);
}
