#include "applet_hud.hpp"

#include "tune.h"
#include "pm/applet_names.hpp"

#include <cstdio>

namespace {

    constexpr s32 QueryCount = 32;
    alignas(0x10) PdmPlayEvent g_events[QueryCount];

    auto UptimeMs() -> u64 {
        return armTicksToNs(armGetSystemTick()) / 1000000ULL;
    }

}

void AppletHud::Initialize() {
    m_pdm_rc    = pdmqryInitialize();
    m_pdm_ready = R_SUCCEEDED(m_pdm_rc);
}

void AppletHud::Exit() {
    if (m_pdm_ready) {
        pdmqryExit();
        m_pdm_ready = false;
    }
}

void AppletHud::Push(const Entry &entry) {
    m_history[m_head] = entry;
    m_head = (m_head + 1) % HistorySize;
    if (m_count < HistorySize)
        m_count++;
}

void AppletHud::Poll() {
    if (!m_pdm_ready)
        return;

    s32 total{}, start{}, end{};
    if (R_FAILED(pdmqryGetAvailablePlayEventRange(&total, &start, &end)) || end == m_last_end)
        return;

    /* First poll: back-fill with the most recent events so changes that
       happened just before the HUD was switched on are visible too. */
    const bool backfill = m_last_end < 0;
    s32 first = backfill ? end - (QueryCount - 1) : m_last_end + 1;
    if (first < end - (QueryCount - 1))
        first = end - (QueryCount - 1);
    if (first < 0)
        first = 0;

    s32 out{};
    if (R_FAILED(pdmqryQueryPlayEvent(first, g_events, QueryCount, &out)))
        return;

    const u64 now = backfill ? 0 : UptimeMs();
    for (s32 i = 0; i < out; ++i) {
        const PdmPlayEvent &e = g_events[i];
        if (e.play_event_type != PdmPlayEventType_Applet)
            continue;

        const u8 applet_id = e.event_data.applet.applet_id;
        this->Push(Entry{
            first + i,
            pm::PdmProgramId(e.event_data.applet.program_id),
            now,
            applet_id,
            e.event_data.applet.event_type,
            applet_id == AppletId_application ? static_cast<u8>(0) : e.event_data.applet.unk_x8.applet.mode,
        });
    }

    m_last_end = end;
}

void AppletHud::Draw(tsl::gfx::Renderer *renderer) {
    /* Poll ~10 times per second, music state ~4 times per second. */
    if ((m_frame % 6) == 0)
        this->Poll();
    if ((m_frame % 15) == 0)
        m_music_known = R_SUCCEEDED(tuneGetStatus(&m_music_playing));
    m_frame++;

    constexpr s32 x = 8, y = 8, w = 432, line = 19, font = 15;
    const s32 lines = 2 + static_cast<s32>(m_count ? m_count : 1);
    const s32 h = 10 + lines * line + 6;

    renderer->drawRect(x, y, w, h, renderer->a({0x0, 0x0, 0x0, 0xC}));

    const tsl::Color white{0xF, 0xF, 0xF, 0xF};
    const tsl::Color grey{0xA, 0xA, 0xA, 0xF};
    const tsl::Color green{0x4, 0xF, 0x6, 0xF};
    const tsl::Color orange{0xF, 0xA, 0x3, 0xF};
    const tsl::Color cyan{0x5, 0xD, 0xF, 0xF};

    s32 ty = y + 6 + font;
    renderer->drawString("Applet HUD", false, x + 8, ty, font, renderer->a(white));
    renderer->drawString("combo: menu", false, x + w - 100, ty, 13, renderer->a(grey));

    ty += line;
    if (!m_music_known) {
        renderer->drawString("Music: ?", false, x + 8, ty, font, renderer->a(grey));
    } else {
        renderer->drawString(m_music_playing ? "Music: playing" : "Music: paused", false, x + 8, ty, font,
                             renderer->a(m_music_playing ? green : orange));
    }

    if (!m_pdm_ready) {
        char msg[64];
        std::snprintf(msg, sizeof(msg), "pdm:qry unavailable (0x%X)", m_pdm_rc);
        renderer->drawString(msg, false, x + 8, ty + line, font, renderer->a(orange));
        return;
    }

    if (m_count == 0) {
        renderer->drawString("no applet events yet", false, x + 8, ty + line, font, renderer->a(grey));
        return;
    }

    /* Newest first. */
    for (size_t n = 0; n < m_count; ++n) {
        const Entry &e = m_history[(m_head + HistorySize - 1 - n) % HistorySize];
        ty += line;

        char time[16];
        if (e.seen_ms == 0)
            std::snprintf(time, sizeof(time), "earlier");
        else
            std::snprintf(time, sizeof(time), "%.1fs", static_cast<double>(e.seen_ms) / 1000.0);

        char id[8];
        std::snprintf(id, sizeof(id), "%u", static_cast<unsigned>(e.applet_id));

        const char *name = pm::AppletIdName(e.applet_id);
        const bool focus = e.event_type == PdmAppletEventType_InFocus;
        const tsl::Color text = n == 0 ? white : grey;

        renderer->drawString(time, false, x + 8, ty, font, renderer->a(text));
        renderer->drawString(id, false, x + 88, ty, font, renderer->a(cyan));
        renderer->drawString(name, false, x + 122, ty, font, renderer->a(text));
        renderer->drawString(pm::AppletEventName(e.event_type), false, x + 272, ty, font,
                             renderer->a(focus ? green : text));
        if (e.mode == 1)
            renderer->drawString("layered", false, x + 362, ty, 13, renderer->a(grey));
    }
}
