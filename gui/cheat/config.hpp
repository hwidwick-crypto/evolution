#pragma once

// Cheat configuration. Base fields come from the original pasta client, the
// "extras" section is bound to the Evolution GUI menu (see gui.cpp sync).
struct Config {
    // Vector Aim
    bool vaim_enable = true;
    int vaim_key = 0x02; // Right Mouse Button
    int vaim_mode = 0; // 0 = Hold, 1 = Toggle
    int vaim_bone = 0x796E; // Head
    float vaim_fov = 120.0f; // FOV radius in screen pixels
    float vaim_smooth = 3.5f; // delta divider, 1.0 = instant
    bool vaim_draw_fov = true;
    float vaim_max_dist = 250.0f;
    bool vaim_visible_only = true;
    bool vaim_ignore_dead = true;
    bool vaim_ignore_friends = true;
    bool vaim_ignore_npc = false;

    // Silent Aim & Wallshot
    bool saim_enable = false;
    int saim_key = 0x06; // Mouse Side Button 2
    int saim_mode = 0;
    bool saim_wallshot = false;
    float saim_fov = 300.0f;

    // Triggerbot
    bool trg_enable = false;
    int trg_key = 0x12; // Alt key
    int trg_mode = 0;
    int trg_delay_ms = 50;

    // Visuals (ESP) - base
    bool esp_master = true;
    bool esp_box = true;
    bool esp_skeleton = true;
    bool esp_health_bar = true;
    bool esp_armor_bar = true;
    bool esp_snaplines = false;
    bool esp_arrow_fov = true;
    float esp_arrow_size = 15.0f;
    float esp_render_distance = 400.0f;

    // Visuals (ESP) - menu extras
    bool esp_corner = false;
    int  esp_box_style = 0;     // 0 = 2D box, 1 = corner box, 2 = filled
    bool esp_head_dot = false;
    int  esp_tracer_origin = 0; // 0 = bottom, 1 = center, 2 = crosshair

    // HUD
    bool hud_crosshair = true;
    bool hud_fov_circle = false;
    float hud_fov_radius = 120.0f;
    bool hud_watermark = true;
    bool hud_hitmarker = false;

    // Misc
    bool misc_noclip = false;
    int misc_noclip_key = 0x56; // 'V' key
    float misc_noclip_speed = 2.5f;
    bool misc_godmode = false;
    int misc_suicide_key = 0x23; // End key
    bool obs_bypass = true;
    int menu_key = 0x2D; // Insert key
    int unload_key = 0x23; // End key
};

inline Config g_Config;
