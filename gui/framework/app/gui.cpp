#include "gui.h"

#include "config.hpp"
#include "sdk/offsets.hpp"
#include "core/glow_renderer.h"
#include "helpers/fonts.h"
#include "helpers/icons.h"
#include "helpers/screenshot.h"
#include "helpers/window_drag.h"
#include "pages/aim_page.h"
#include "pages/config_page.h"
#include "pages/settings_page.h"
#include "pages/visuals_page.h"
#include "theme/colors.h"
#include "theme/layout.h"
#include "theme/ui_scale.h"

#include "imgui.h"
#include "imgui_internal.h"

namespace
{
    icons::tab g_selected_tab = icons::tab::aim;
    icons::tab g_prev_tab     = icons::tab::aim;
    float g_tab_fade          = 1.f;
    ImGuiStyle g_base_style   = {};
    bool g_base_style_saved   = false;

    aim_page::state      g_aim;
    visuals_page::state  g_visuals;
    config_page::state   g_config;
    settings_page::state g_settings;

    void apply_style()
    {
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding   = 0.f;
        s.WindowBorderSize = 0.f;
        s.WindowPadding    = { 0.f, 0.f };
        s.AntiAliasedLines = true;
        s.AntiAliasedFill  = true;
        s.ChildBorderSize  = 0.f;
        s.FrameBorderSize  = 0.f;
        s.ScrollbarSize    = 0.f;

        ImVec4* c = s.Colors;
        c[ImGuiCol_WindowBg]         = { 0.f, 0.f, 0.f, 0.f };
        c[ImGuiCol_ChildBg]          = { 0.f, 0.f, 0.f, 0.f };
        c[ImGuiCol_Text]             = colors::text;
        c[ImGuiCol_ScrollbarBg]      = { 0.f, 0.f, 0.f, 0.f };
        c[ImGuiCol_ScrollbarGrab]    = { 0.f, 0.f, 0.f, 0.f };
        c[ImGuiCol_ScrollbarGrabHovered] = { 0.f, 0.f, 0.f, 0.f };
        c[ImGuiCol_ScrollbarGrabActive]  = { 0.f, 0.f, 0.f, 0.f };

        if (!g_base_style_saved)
        {
            g_base_style = s;
            g_base_style_saved = true;
        }
    }

    void apply_scaled_style()
    {
        ImGui::GetStyle() = g_base_style;
        ImGui::GetStyle().ScaleAllSizes(ui_scale::factor);
        ImGui::GetIO().FontGlobalScale = 1.f;
    }

    void draw_text_centered(ImDrawList* dl, ImFont* font, const ImRect& rect,
        ImU32 col, const char* text)
    {
        if (!font || !text)
            return;

        const float render = fonts::size(font);
        const ImVec2 ts = font->CalcTextSizeA(render, FLT_MAX, 0.f, text);
        const ImVec2 pos = {
            rect.Min.x + (rect.GetWidth() - ts.x) * 0.5f,
            rect.Min.y + (rect.GetHeight() - ts.y) * 0.5f };
        dl->AddText(font, render, pos, col, text);
    }

    void draw_brand_logo(ImDrawList* dl, const ImRect& area)
    {
        const ImVec2 c = area.GetCenter();
        const float s  = ui_scale::s(layout::sidebar_logo_size) * 0.5f;
        const ImU32 accent = ImGui::GetColorU32(colors::accent);
        const ImU32 glow   = ImGui::GetColorU32(ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, 0.18f));

        dl->AddCircleFilled(c, s + 1.f, glow, 32);
        dl->AddCircle(c, s, accent, 32, ui_scale::s(1.5f));

        ImVec2 diamond[4] = {
            { c.x, c.y - s * 0.58f },
            { c.x + s * 0.50f, c.y },
            { c.x, c.y + s * 0.58f },
            { c.x - s * 0.50f, c.y },
        };
        dl->AddConvexPolyFilled(diamond, 4, accent);
        dl->AddCircleFilled(c, ui_scale::s(2.2f), ImGui::GetColorU32(colors::text), 12);
    }

    ImRect sidebar_tab_rect(const ImRect& sidebar, float y)
    {
        const float tab_w = ui_scale::s(layout::sidebar_tab_w);
        const float tab_h = ui_scale::s(layout::sidebar_tab_h);
        const float x = sidebar.GetCenter().x - tab_w * 0.5f;
        return { { x, y }, { x + tab_w, y + tab_h } };
    }

    void draw_sidebar_tab(ImDrawList* dl, ImFont* icon_font, const ImRect& tab_bb, icons::tab tab,
        int tab_index, bool selected, bool& blocks_window_drag)
    {
        const char* glyph = icons::to_utf8(tab);

        ImGuiID id = ImGui::GetID((void*)(intptr_t)tab);
        ImGui::ItemAdd(tab_bb, id);

        bool hovered = false;
        bool held    = false;
        const bool pressed = ImGui::ButtonBehavior(tab_bb, id, &hovered, &held);

        if (hovered || held)
            blocks_window_drag = true;

        if (pressed)
            g_selected_tab = tab;

        static float sel_anim[4]   = {};
        static float hover_anim[4] = {};
        const float dt = ImGui::GetIO().DeltaTime;
        const int idx = ImClamp(tab_index, 0, 3);
        sel_anim[idx]   = ImLerp(sel_anim[idx],   selected ? 1.f : 0.f, ImMin(1.f, dt * 22.f));
        hover_anim[idx] = ImLerp(hover_anim[idx], hovered && !selected ? 1.f : 0.f, ImMin(1.f, dt * 24.f));

        if (hover_anim[idx] > 0.01f)
        {
            dl->AddRectFilled(tab_bb.Min, tab_bb.Max,
                ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 0.05f * hover_anim[idx])), ui_scale::s(layout::sidebar_tab_round));
        }

        const ImVec4 icon_col = ImLerp(
            ImLerp(colors::text_muted, colors::text, hover_anim[idx] * 0.35f),
            colors::accent,
            sel_anim[idx]);

        draw_text_centered(dl, icon_font, tab_bb,
            ImGui::GetColorU32(icon_col), glyph);
    }

    void draw_sidebar(ImDrawList* dl, const ImRect& sidebar, float alpha, bool& blocks_window_drag)
    {
        using namespace layout;

        ImVec4 sidebar_col = colors::sidebar;
        sidebar_col.w *= alpha;
        dl->AddRectFilled(sidebar.Min, sidebar.Max, ImGui::GetColorU32(sidebar_col),
            ui_scale::s(shell_round), ImDrawFlags_RoundCornersLeft);

        const float tab_w = ui_scale::s(sidebar_tab_w);
        const float tab_x = sidebar.GetCenter().x - tab_w * 0.5f;
        const float logo_y = sidebar.Min.y + ui_scale::s(sidebar_pad);
        const ImRect logo_area(
            { tab_x, logo_y },
            { tab_x + tab_w, logo_y + ui_scale::s(sidebar_logo_size) });
        draw_brand_logo(dl, logo_area);

        const float sep_y = logo_area.Max.y + ui_scale::s(sidebar_logo_gap) * 0.5f;
        dl->AddLine(
            { sidebar.Min.x + ui_scale::s(14.f), sep_y },
            { sidebar.Max.x - ui_scale::s(14.f), sep_y },
            ImGui::GetColorU32(colors::sidebar_sep));

        static const icons::tab tabs[] = {
            icons::tab::visuals,
            icons::tab::aim,
            icons::tab::config,
            icons::tab::settings,
        };

        ImFont* icon_font = fonts::icon(layout::sidebar_tab_icon);
        if (!icon_font)
            icon_font = ImGui::GetFont();

        const int tab_count = (int)IM_ARRAYSIZE(tabs);
        const float tab_h = ui_scale::s(sidebar_tab_h);
        const float tab_gap = ui_scale::s(sidebar_tabs_gap);
        const float nav_h = (float)tab_count * tab_h + (float)(tab_count - 1) * tab_gap;
        const float zone_top = sep_y + ui_scale::s(8.f);
        const float zone_bottom = sidebar.Max.y - ui_scale::s(sidebar_bottom_pad + sidebar_exit_h + 8.f);
        float y = zone_top + ImMax(0.f, (zone_bottom - zone_top - nav_h) * 0.5f);

        ImRect tab_rects[4] = {};
        int selected_index = 0;
        for (int i = 0; i < tab_count; ++i)
        {
            tab_rects[i] = sidebar_tab_rect(sidebar, y);
            if (g_selected_tab == tabs[i])
                selected_index = i;
            y += tab_h + tab_gap;
        }

        static float indicator_y = 0.f;
        static bool indicator_init = false;
        static float last_sidebar_scale = 1.f;
        if (fabsf(last_sidebar_scale - ui_scale::factor) > 0.001f)
        {
            indicator_init = false;
            last_sidebar_scale = ui_scale::factor;
        }
        const ImRect& target = tab_rects[selected_index];
        if (!indicator_init)
        {
            indicator_y = target.Min.y;
            indicator_init = true;
        }
        indicator_y = ImLerp(indicator_y, target.Min.y, ImMin(1.f, ImGui::GetIO().DeltaTime * 20.f));

        const ImRect indicator_bb(
            { tab_rects[0].Min.x, indicator_y },
            { tab_rects[0].Max.x, indicator_y + tab_h });
        dl->AddRectFilled(indicator_bb.Min, indicator_bb.Max,
            ImGui::GetColorU32(ImVec4(colors::accent.x, colors::accent.y, colors::accent.z, 0.16f)),
            ui_scale::s(sidebar_tab_round));

        for (int i = 0; i < tab_count; ++i)
        {
            draw_sidebar_tab(dl, icon_font, tab_rects[i], tabs[i], i,
                g_selected_tab == tabs[i], blocks_window_drag);
        }

        const ImRect exit_bb = sidebar_tab_rect(sidebar,
            sidebar.Max.y - ui_scale::s(sidebar_bottom_pad + sidebar_exit_h));

        ImGuiID exit_id = ImGui::GetID("##sidebar_exit");
        ImGui::ItemAdd(exit_bb, exit_id);

        bool exit_hovered = false;
        bool exit_held    = false;
        const bool exit_pressed = ImGui::ButtonBehavior(exit_bb, exit_id, &exit_hovered, &exit_held);

        if (exit_hovered || exit_held)
            blocks_window_drag = true;

        static float exit_hover = 0.f;
        exit_hover = ImLerp(exit_hover, exit_hovered ? 1.f : 0.f, ImMin(1.f, ImGui::GetIO().DeltaTime * 18.f));
        if (exit_hover > 0.01f)
        {
            dl->AddRectFilled(exit_bb.Min, exit_bb.Max,
                ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 0.05f * exit_hover)), ui_scale::s(sidebar_exit_round));
        }

        if (ImFont* exit_font = fonts::icon(layout::sidebar_tab_icon))
        {
            const ImVec4 exit_base = ImLerp(colors::text_muted, colors::accent, exit_hover);
            draw_text_centered(dl, exit_font, exit_bb,
                ImGui::GetColorU32(exit_base), icons::exit);
        }

        if (exit_pressed)
            ::PostQuitMessage(0);
    }

    void draw_active_page(bool& blocks_window_drag)
    {
        switch (g_selected_tab)
        {
        case icons::tab::visuals:  visuals_page::draw(g_visuals, blocks_window_drag); break;
        case icons::tab::aim:      aim_page::draw(g_aim, blocks_window_drag); break;
        case icons::tab::config:   config_page::draw(g_config, blocks_window_drag); break;
        case icons::tab::settings:
            settings_page::draw(g_settings, blocks_window_drag);
            break;
        default: break;
        }
    }

    // Pushes the menu state into the cheat config that the features (aimbot,
    // ESP, misc - see gui\cheat) read every tick.
}

namespace gui
{
    void sync_cheat_config()
    {
        static const int kAimBones[] = {
            Offsets::Bones::SKEL_HEAD,
            Offsets::Bones::SKEL_NECK,
            Offsets::Bones::SKEL_SPINE3,
            Offsets::Bones::SKEL_SPINE3,
            Offsets::Bones::SKEL_PELVIS,
        };

        // Aim page -> vector aim / silent aim / triggerbot
        g_Config.vaim_enable       = g_aim.enabled;
        g_Config.vaim_smooth       = ImMax(1.0f, g_aim.smoothing * 0.1f);
        g_Config.vaim_bone         = kAimBones[ImClamp(g_aim.aimbone, 0, 4)];
        g_Config.vaim_key          = g_aim.keybind; // 0 = unbound
        g_Config.vaim_fov          = g_aim.fov;
        g_Config.vaim_max_dist     = g_aim.max_distance;
        g_Config.vaim_visible_only = g_aim.visible_only;
        g_Config.vaim_ignore_dead  = g_aim.ignore_knocked;
        g_Config.vaim_ignore_friends = g_aim.ignore_team;
        g_Config.saim_enable       = g_aim.silent_aim;
        g_Config.trg_enable        = g_aim.auto_shoot;

        // Visuals page -> ESP / HUD
        g_Config.esp_master         = g_visuals.esp_enabled;
        g_Config.esp_box            = g_visuals.box_esp;
        g_Config.esp_corner         = g_visuals.corner_box;
        g_Config.esp_box_style      = g_visuals.box_style;
        g_Config.esp_health_bar     = g_visuals.health_bar;
        g_Config.esp_skeleton       = g_visuals.skeleton;
        g_Config.esp_head_dot       = g_visuals.head_dot;
        g_Config.esp_snaplines      = g_visuals.tracers;
        g_Config.esp_tracer_origin  = g_visuals.tracer_origin;
        g_Config.esp_render_distance = g_visuals.max_distance;
        g_Config.hud_crosshair      = g_visuals.crosshair;
        g_Config.hud_fov_circle     = g_visuals.fov_circle;
        g_Config.hud_fov_radius     = g_visuals.fov_radius;
        g_Config.hud_watermark      = g_visuals.watermark;
        g_Config.hud_hitmarker      = g_visuals.hitmarker;
        g_Config.obs_bypass         = g_visuals.streamproof || g_settings.streamproof;

        // Settings page
        g_Config.menu_key = g_settings.menu_key ? g_settings.menu_key : 0x2D;
    }
}

namespace gui
{
    void initialize(ID3D11Device* device)
    {
        g_settings.accent_color = colors::accent;
        glow_renderer::init(device);
        fonts::init();
        apply_style();
    }

    void shutdown()
    {
        fonts::shutdown();
        glow_renderer::shutdown();
    }

    void prepare_frame()
    {
        ui_scale::set_percent(g_settings.ui_scale);
        fonts::sync_ui_scale(ui_scale::factor);
        apply_scaled_style();
    }

    void render(HWND hwnd)
    {
        using namespace layout;

        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, ImGuiInputFlags_RouteGlobal))
            screenshot::request();

        const ImVec2 display = ImGui::GetIO().DisplaySize;

        static ImVec2 panel_pos = { -1.f, -1.f };
        static float  last_scale  = 1.f;

        const float panel_w = ui_scale::s(window_w);
        const float panel_h = ui_scale::s(window_h);
        const float sb_w    = ui_scale::s(sidebar_w);

        if (panel_pos.x < 0.f || last_scale != ui_scale::factor)
        {
            panel_pos.x = (display.x - panel_w) * 0.5f;
            panel_pos.y = (display.y - panel_h) * 0.5f;
            last_scale  = ui_scale::factor;
        }

        const ImVec2 panel_size = { panel_w, panel_h };
        const ImRect panel_bb(panel_pos, panel_pos + panel_size);
        const float  panel_round = ui_scale::s(shell_round);

        if (screenshot::get().capture_frame)
        {
            const float outline = ui_scale::s(1.f);
            screenshot::set_bounds(
                panel_bb.Min - ImVec2(outline, outline),
                panel_bb.Max + ImVec2(outline, outline));
        }

        ImGui::SetNextWindowPos({ 0.f, 0.f }, ImGuiCond_Always);
        ImGui::SetNextWindowSize(display, ImGuiCond_Always);

        ImGui::Begin(main_window_id, nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBackground);

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // NOTE: no fullscreen background - the menu floats directly over the
        // game frame (see framework/core/present_hook.cpp). Menu opacity comes
        // from the settings page.
        const float shell_alpha = ImClamp(g_settings.menu_opacity * 0.01f, 0.05f, 1.0f);

        ImVec4 panel_col = colors::panel;
        panel_col.w *= shell_alpha;
        dl->AddRectFilled(panel_bb.Min, panel_bb.Max, ImGui::GetColorU32(panel_col), panel_round);

        bool blocks_window_drag = false;

        const ImRect sidebar(panel_bb.Min, { panel_bb.Min.x + sb_w, panel_bb.Max.y });
        draw_sidebar(dl, sidebar, shell_alpha, blocks_window_drag);

        if (g_selected_tab != g_prev_tab)
        {
            g_tab_fade  = 0.f;
            g_prev_tab  = g_selected_tab;
        }

        const float dt = ImGui::GetIO().DeltaTime;
        const float tab_ease = 1.f - ImPow(0.00001f, dt * 12.f);
        g_tab_fade = ImLerp(g_tab_fade, 1.f, tab_ease);

        ImGui::SetCursorScreenPos({ panel_bb.Min.x + sb_w, panel_bb.Min.y });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.f, 0.f });
        ImGui::BeginChild(layout::content_child_id,
            { panel_size.x - sb_w, panel_size.y },
            ImGuiChildFlags_None,
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_tab_fade);
        draw_active_page(blocks_window_drag);
        ImGui::PopStyleVar();

        ImGui::EndChild();
        ImGui::PopStyleVar();

        window_drag::handle_panel(panel_pos, panel_bb, blocks_window_drag);
        sync_cheat_config();
        (void)hwnd;
        ImGui::End();
    }
}
