#include "esp.hpp"
#include "../config.hpp"
#include "../driver/driver.hpp"
#include "../sdk/offsets.hpp"
#include "../sdk/vector.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "colors.h"

#include <windows.h>
#include <cmath>

namespace Features {
    namespace {
        ImU32 accent_u32(float alpha = 1.0f) {
            ImVec4 c = colors::accent;
            c.w = alpha;
            return ImGui::GetColorU32(c);
        }

        void draw_corner_box(ImDrawList* dl, float left, float top, float right, float bottom, ImU32 col) {
            const float w = right - left;
            const float h = bottom - top;
            const float len = (w < h ? w : h) * 0.25f;

            // top-left
            dl->AddLine({ left, top }, { left + len, top }, col, 1.6f);
            dl->AddLine({ left, top }, { left, top + len }, col, 1.6f);
            // top-right
            dl->AddLine({ right, top }, { right - len, top }, col, 1.6f);
            dl->AddLine({ right, top }, { right, top + len }, col, 1.6f);
            // bottom-left
            dl->AddLine({ left, bottom }, { left + len, bottom }, col, 1.6f);
            dl->AddLine({ left, bottom }, { left, bottom - len }, col, 1.6f);
            // bottom-right
            dl->AddLine({ right, bottom }, { right - len, bottom }, col, 1.6f);
            dl->AddLine({ right, bottom }, { right, bottom - len }, col, 1.6f);
        }

        void draw_bar(ImDrawList* dl, float x, float bottom, float height, float fraction, ImU32 frame) {
            const float barH = height;
            const float fillH = barH * ImClamp(fraction, 0.0f, 1.0f);

            dl->AddRectFilled({ x, bottom - barH }, { x + 4.0f, bottom }, ImColor(0.f, 0.f, 0.f, 0.55f));
            if (fillH > 0.0f)
                dl->AddRectFilled({ x, bottom - fillH }, { x + 4.0f, bottom }, frame);
        }

        void draw_hud(int w, int h) {
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            const ImVec2 center = { w / 2.0f, h / 2.0f };
            const ImU32 accent = accent_u32();

            if (g_Config.hud_crosshair) {
                const float gap = 5.0f, size = 6.0f;
                dl->AddLine({ center.x - gap - size, center.y }, { center.x - gap, center.y }, accent, 1.5f);
                dl->AddLine({ center.x + gap, center.y }, { center.x + gap + size, center.y }, accent, 1.5f);
                dl->AddLine({ center.x, center.y - gap - size }, { center.x, center.y - gap }, accent, 1.5f);
                dl->AddLine({ center.x, center.y + gap }, { center.x, center.y + gap + size }, accent, 1.5f);
            }

            if (g_Config.hud_fov_circle)
                dl->AddCircle(center, g_Config.hud_fov_radius, accent_u32(0.55f), 64, 1.4f);

            if (g_Config.hud_hitmarker) {
                const float s = 7.0f, o = 4.0f;
                dl->AddLine({ center.x - o - s, center.y - o - s }, { center.x - o, center.y - o }, accent, 1.6f);
                dl->AddLine({ center.x + o + s, center.y - o - s }, { center.x + o, center.y - o }, accent, 1.6f);
                dl->AddLine({ center.x - o - s, center.y + o + s }, { center.x - o, center.y + o }, accent, 1.6f);
                dl->AddLine({ center.x + o + s, center.y + o + s }, { center.x + o, center.y + o }, accent, 1.6f);
            }

            if (g_Config.hud_watermark) {
                dl->AddText({ 12.0f, 12.0f }, ImColor(0.f, 0.f, 0.f, 0.55f), "evolution | gta5");
                dl->AddText({ 11.0f, 11.0f }, accent, "evolution | gta5");
            }
        }
    }

    void ESP::Render(const Matrix4x4& viewMatrix, int screenWidth, int screenHeight) {
        if (!g_Config.esp_master || screenWidth <= 0 || screenHeight <= 0) return;
        if (Driver::hDriver == INVALID_HANDLE_VALUE || !Driver::baseAddress) return;

        draw_hud(screenWidth, screenHeight);

        if (!g_Config.esp_box && !g_Config.esp_snaplines && !g_Config.esp_head_dot &&
            !g_Config.esp_health_bar && !g_Config.esp_armor_bar && !g_Config.esp_arrow_fov)
            return;

        uintptr_t world = Driver::ReadMemory<uintptr_t>(Driver::baseAddress + Offsets::World);
        if (!world) return;

        uintptr_t localPed = Driver::ReadMemory<uintptr_t>(world + Offsets::LocalPlayer);
        if (!localPed) return;

        uintptr_t replay = Driver::ReadMemory<uintptr_t>(Driver::baseAddress + Offsets::ReplayInterface);
        if (!replay) return;

        uintptr_t pedList = Driver::ReadMemory<uintptr_t>(replay + Offsets::ReplayPedList);
        int pedCount = Driver::ReadMemory<int>(replay + Offsets::ReplayPedCount);

        Vector3 localPos{};
        uintptr_t localNav = Driver::ReadMemory<uintptr_t>(localPed + Offsets::CPed::Navigation);
        if (localNav) {
            localPos = Driver::ReadMemory<Vector3>(localNav + Offsets::CNavigation::Position);
        }

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        const ImVec2 screenCenter = { screenWidth / 2.0f, screenHeight / 2.0f };
        const ImU32 boxCol = accent_u32();

        // Nearest target that is behind/outside the screen - used for the
        // off-screen direction arrow.
        Vector2 arrowScreen{};
        float arrowBestDist = 1e9f;

        for (int i = 0; i < pedCount; ++i) {
            uintptr_t ped = Driver::ReadMemory<uintptr_t>(pedList + (i * Offsets::PedStride));
            if (!ped || ped == localPed) continue;

            float hp = Driver::ReadMemory<float>(ped + Offsets::CPed::Health);
            if (hp <= 100.0f) continue; // Dead ped

            uintptr_t nav = Driver::ReadMemory<uintptr_t>(ped + Offsets::CPed::Navigation);
            if (!nav) continue;

            Vector3 pos = Driver::ReadMemory<Vector3>(nav + Offsets::CNavigation::Position);
            float distance = localPos.Distance(pos);
            if (distance > g_Config.esp_render_distance) continue;

            Vector3 headPos = { pos.x, pos.y, pos.z + 0.85f };
            Vector3 feetPos = { pos.x, pos.y, pos.z - 0.9f };

            Vector2 headScreen, feetScreen;
            if (!SDK::WorldToScreen(headPos, headScreen, viewMatrix, screenWidth, screenHeight) ||
                !SDK::WorldToScreen(feetPos, feetScreen, viewMatrix, screenWidth, screenHeight))
                continue;

            const bool onScreen =
                headScreen.x > -200.f && headScreen.x < screenWidth + 200.f &&
                feetScreen.x > -200.f && feetScreen.x < screenWidth + 200.f;

            if (!onScreen) {
                if (!g_Config.esp_arrow_fov) continue;

                const float dst = std::hypot(headScreen.x - screenCenter.x, headScreen.y - screenCenter.y);
                if (dst < arrowBestDist) {
                    arrowBestDist = dst;
                    arrowScreen = headScreen;
                }
                continue;
            }

            float height = std::abs(feetScreen.y - headScreen.y);
            if (height < 4.0f) continue;
            float width = height / 2.0f;

            float boxLeft = headScreen.x - (width / 2.0f);
            float boxTop = headScreen.y;
            float boxRight = headScreen.x + (width / 2.0f);
            float boxBottom = feetScreen.y;

            float armor = Driver::ReadMemory<float>(ped + Offsets::CPed::Armor);

            if (g_Config.esp_box) {
                const int style = g_Config.esp_corner ? 1 : g_Config.esp_box_style;
                if (style == 1) {
                    draw_corner_box(dl, boxLeft, boxTop, boxRight, boxBottom, boxCol);
                } else if (style == 2) {
                    dl->AddRectFilled({ boxLeft, boxTop }, { boxRight, boxBottom },
                        accent_u32(0.14f), 0.0f);
                    dl->AddRect({ boxLeft, boxTop }, { boxRight, boxBottom }, boxCol, 0.0f, 0, 1.6f);
                } else {
                    dl->AddRect({ boxLeft, boxTop }, { boxRight, boxBottom }, boxCol, 0.0f, 0, 1.6f);
                }
            }

            if (g_Config.esp_health_bar) {
                const float frac = (hp - 100.0f) / 100.0f;
                draw_bar(dl, boxLeft - 7.0f, boxBottom, height, frac,
                    ImColor::HSV(ImClamp(frac, 0.0f, 1.0f) * 0.33f, 0.85f, 0.95f));
            }

            if (g_Config.esp_armor_bar && armor > 0.0f) {
                draw_bar(dl, boxRight + 3.0f, boxBottom, height, armor / 100.0f,
                    ImColor::HSV(0.55f, 0.55f, 1.0f));
            }

            if (g_Config.esp_head_dot)
                dl->AddCircleFilled({ headScreen.x, headScreen.y }, 3.0f, boxCol);

            if (g_Config.esp_snaplines) {
                ImVec2 origin;
                switch (g_Config.esp_tracer_origin) {
                case 1:  origin = { screenWidth / 2.0f, screenHeight / 2.0f }; break;
                case 2:  origin = screenCenter; break;
                default: origin = { screenWidth / 2.0f, (float)screenHeight }; break;
                }
                dl->AddLine(origin, { (boxLeft + boxRight) * 0.5f, boxBottom },
                    accent_u32(0.45f), 1.0f);
            }
        }

        // Off-screen target arrow
        if (g_Config.esp_arrow_fov && arrowBestDist < 1e8f) {
            ImVec2 dir = { arrowScreen.x - screenCenter.x, arrowScreen.y - screenCenter.y };
            const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (len > 0.001f) {
                dir.x /= len; dir.y /= len;

                const ImVec2 pos = {
                    screenCenter.x + dir.x * (screenHeight * 0.42f),
                    screenCenter.y + dir.y * (screenHeight * 0.42f) };

                const ImVec2 normal = { -dir.y, dir.x };
                const float size = g_Config.esp_arrow_size;

                ImVec2 tip = { pos.x + dir.x * size, pos.y + dir.y * size };
                ImVec2 b1 = { pos.x - dir.x * size * 0.6f + normal.x * size * 0.7f,
                              pos.y - dir.y * size * 0.6f + normal.y * size * 0.7f };
                ImVec2 b2 = { pos.x - dir.x * size * 0.6f - normal.x * size * 0.7f,
                              pos.y - dir.y * size * 0.6f - normal.y * size * 0.7f };
                dl->AddTriangleFilled(tip, b1, b2, accent_u32(0.9f));
            }
        }
    }
}
