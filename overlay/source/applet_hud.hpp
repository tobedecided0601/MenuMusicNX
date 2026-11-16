#pragma once

#include <tesla.hpp>

/// Debug HUD: the latest applet changes from the pdm play event log, drawn in
/// the top-left corner while the overlay menu is closed (see tsl::setHudEnabled).
class AppletHud {
  public:
    void Initialize();
    void Exit();

    /// Called once per frame by the overlay while the HUD is shown.
    void Draw(tsl::gfx::Renderer *renderer);

  private:
    static constexpr size_t HistorySize = 10;

    struct Entry {
        s32 index;
        u64 program_id;
        u64 seen_ms; ///< uptime when the HUD first saw it; 0 = before the HUD started
        u8 applet_id;
        u8 event_type;
        u8 mode;
    };

    void Poll();
    void Push(const Entry &entry);

    bool m_pdm_ready{};
    Result m_pdm_rc{};
    s32 m_last_end{-1};
    u32 m_frame{};
    bool m_music_playing{};
    bool m_music_known{};

    Entry m_history[HistorySize]{};
    size_t m_count{};
    size_t m_head{}; ///< next write position
};
