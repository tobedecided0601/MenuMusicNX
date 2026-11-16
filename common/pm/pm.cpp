#include "pm.hpp"

#include "config/config.hpp"
#include "sdmc/sdmc.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr u64 QLAUNCH_TITLE_ID{pm::QlaunchTitleId};
constexpr s32 EVENT_COUNT = 32;

/* qlaunch and every applet (library applets, the overlay applet, "starter",
   ...) live in the reserved 0x0100000000001xxx title-id range. */
constexpr u64 APPLET_TITLE_ID_BASE{0x0100000000001000ULL};
constexpr u64 APPLET_TITLE_ID_MASK{0xFFFFFFFFFFFFF000ULL};

/* The overlay applet is used by Tesla/nx-ovlloader. Opening the MenuMusicNX
   overlay must not be treated as leaving the HOME Menu. */
constexpr u64 OVERLAY_APPLET_TITLE_ID{0x010000000000100CULL};

/* Terminated applets whose older events must be skipped while scanning. */
constexpr u32 CLOSED_PROGRAM_MAX = 4;

constexpr u32 FOCUS_LOG_MAX_LINES = 12;
constexpr const char FOCUS_LOG_PATH[] = "/config/sys-tune/focus.log";

alignas(0x10) PdmAppletEvent g_applet_events[EVENT_COUNT]{};
alignas(0x10) PdmPlayEvent g_play_events[EVENT_COUNT]{};
/* Index of the newest play event seen. The log is a ring buffer: once it is
   full its total entry count stops changing, but the end index keeps moving. */
s32 g_last_play_event_end{-1};
bool g_cached_home_foreground{};
bool g_cached_home_valid{};
bool g_pdmqry_initialized{};
bool g_pmdmnt_initialized{};
bool g_pminfo_initialized{};
u64 g_cached_app_tid{};
pm::FocusOwner g_focus_owner{pm::FocusOwner::Unknown};
pm::FocusOwner g_cached_focus_owner{pm::FocusOwner::Unknown};
/* Music keeps playing in applets opened from the HOME Menu (see AppletKeepsMusic). */
bool g_home_applet_music{true};
bool g_focus_log{false};
/* True from the start of a sleep/shutdown transition until wake: no SD card
   access (log writes, config reads) in that window. */
bool g_log_suspended{false};
bool g_focus_log_dir_ready{false};
s32 g_last_logged_entry_index{-1};

auto IsSystemAppletTitleId(u64 program_id) -> bool {
    return (program_id & APPLET_TITLE_ID_MASK) == APPLET_TITLE_ID_BASE;
}

enum class FocusTarget : u8 {
    Ignore,
    Home,
    Application,
    Applet,
};

enum class FocusEventKind : u8 {
    Ignore,
    InFocus,
    OutOfFocus,
    Terminated,
};

auto ClassifyFocusEventKind(u8 event_type) -> FocusEventKind {
    switch (event_type) {
        case PdmAppletEventType_InFocus:
            return FocusEventKind::InFocus;
        case PdmAppletEventType_OutOfFocus:
            return FocusEventKind::OutOfFocus;
        /* The process is gone, so its older events no longer describe the
           foreground owner. */
        case PdmAppletEventType_Exit:
        case PdmAppletEventType_OutOfFocus4:
        case PdmAppletEventType_Exit5:
        case PdmAppletEventType_Exit6:
            return FocusEventKind::Terminated;
        default: /* PdmAppletEventType_Launch */
            return FocusEventKind::Ignore;
    }
}

auto PdmProgramIdToU64(const u32 program_id[2]) -> u64 {
    /* pdm stores the two u32 halves swapped (see libnx pdm.h); on hardware
       qlaunch was logged as 0x0000100001000000 with the opposite order. */
    return (static_cast<u64>(program_id[0]) << 32) | program_id[1];
}

auto TitleIdsMatch(u64 lhs, u64 rhs) -> bool {
    if (lhs == rhs) {
        return true;
    }

    constexpr u64 base_mask = ~0xFFULL;
    if ((lhs & base_mask) == (rhs & base_mask)) {
        return true;
    }

    constexpr u64 update_mask = ~0xFFFULL;
    return (lhs & update_mask) == (rhs & update_mask);
}

auto ClassifyFocusTarget(u64 program_id, u8 applet_id, u64 app_tid) -> FocusTarget {
    if (applet_id != AppletId_None) {
        /* The play log's applet id names the applet itself. Its program id can
           be the program that opened it: on hardware an Album (photoViewer, 21)
           event was logged with qlaunch's program id. So the applet id wins. */
        if (applet_id == AppletId_SystemAppletMenu) {
            return FocusTarget::Home;
        }
        if (applet_id == AppletId_OverlayApplet) {
            return FocusTarget::Ignore;
        }
        if (applet_id == AppletId_application) {
            return TitleIdsMatch(program_id, app_tid) ? FocusTarget::Application : FocusTarget::Applet;
        }
        return FocusTarget::Applet;
    }

    /* The applet event log only reports the program id. */
    if (program_id == 0) {
        return FocusTarget::Ignore;
    }
    if (program_id == QLAUNCH_TITLE_ID) {
        return FocusTarget::Home;
    }
    if (program_id == OVERLAY_APPLET_TITLE_ID) {
        return FocusTarget::Ignore;
    }
    if (!IsSystemAppletTitleId(program_id) && TitleIdsMatch(program_id, app_tid)) {
        return FocusTarget::Application;
    }

    return FocusTarget::Applet;
}

struct FocusEventView {
    u32 entry_index;
    u64 program_id;
    u8 applet_id;
    u8 event_type;
};

FocusEventView g_focus_view[EVENT_COUNT]{};

struct FocusQuery {
    bool decided{false};
    bool home{false};

    /* Diagnostics for the decisive event. */
    u64 program_id{};
    u8 applet_id{};
    u8 event_type{};
    u32 entry_index{};
    FocusTarget target{FocusTarget::Ignore};
};

/* Applets are identified by (program id, applet id): the play log reports a
   library applet with the program id of whoever opened it (usually qlaunch),
   so the program id alone would also match the HOME Menu's own events. */
struct ClosedApplet {
    u64 program_id;
    u8 applet_id;
};

struct FocusScan {
    ClosedApplet closed[CLOSED_PROGRAM_MAX]{};
    u32 closed_count{};
};

auto IsClosedApplet(const FocusScan& scan, const FocusEventView& event) -> bool {
    for (u32 i = 0; i < scan.closed_count; ++i) {
        if (scan.closed[i].program_id == event.program_id && scan.closed[i].applet_id == event.applet_id) {
            return true;
        }
    }

    return false;
}

void MarkAppletClosed(FocusScan* scan, const FocusEventView& event) {
    if (scan->closed_count >= CLOSED_PROGRAM_MAX || IsClosedApplet(*scan, event)) {
        return;
    }

    scan->closed[scan->closed_count++] = ClosedApplet{event.program_id, event.applet_id};
}

/* Applets that always silence the music: they have their own audio (Mii
   Maker, eShop), run homebrew (Album/hbmenu), or are web browsers. */
auto IsSilentAppletId(u8 applet_id) -> bool {
    switch (applet_id) {
        case AppletId_LibraryAppletMiiEdit:
        case AppletId_LibraryAppletPhotoViewer:
        case AppletId_LibraryAppletWeb:
        case AppletId_LibraryAppletShop:
        case AppletId_LibraryAppletOfflineWeb:
        case AppletId_LibraryAppletLoginShare:
        case AppletId_LibraryAppletWifiWebAuth:
            return true;
        default:
            return false;
    }
}

auto IsSilentAppletProgram(u64 program_id) -> bool {
    switch (program_id) {
        case 0x0100000000001009ULL: // miiEdit
        case 0x010000000000100DULL: // photoViewer
        case 0x010000000000100AULL: // web
        case 0x010000000000100BULL: // shop
        case 0x010000000000100FULL: // offlineWeb
        case 0x0100000000001010ULL: // loginShare
        case 0x0100000000001011ULL: // wifiWebAuth
            return true;
        default:
            return false;
    }
}

/* Whether the music keeps playing while this applet has focus: only for
   applets opened by the HOME Menu itself (Settings dialogs, keyboard, user
   page, controllers, profile selector, ...), never for ones a game opened. */
auto AppletKeepsMusic(const FocusEventView& event) -> bool {
    if (!g_home_applet_music) {
        return false;
    }

    if (event.applet_id != AppletId_None) {
        if (IsSilentAppletId(event.applet_id)) {
            return false;
        }
        /* program_id is the program that opened the applet. */
        return event.program_id == QLAUNCH_TITLE_ID;
    }

    /* Applet event log: only the program id is known. */
    return !IsSilentAppletProgram(event.program_id);
}

/* Applies a single focus event, newest first. Returns true when the event
   decides whether the HOME Menu owns the foreground. */
auto ApplyFocusEvent(FocusQuery* query, FocusScan* scan, const FocusEventView& event, u64 app_tid) -> bool {
    const auto kind = ClassifyFocusEventKind(event.event_type);
    if (kind == FocusEventKind::Ignore || IsClosedApplet(*scan, event)) {
        return false;
    }

    const auto target = ClassifyFocusTarget(event.program_id, event.applet_id, app_tid);
    if (target == FocusTarget::Ignore) {
        return false;
    }

    query->program_id = event.program_id;
    query->applet_id = event.applet_id;
    query->event_type = event.event_type;
    query->entry_index = event.entry_index;
    query->target = target;

    switch (target) {
        case FocusTarget::Home:
            /* HOME lost focus, but the applet that took it has already closed
               (seen newer in this scan): pdm logs the applet's exit a moment
               before HOME's InFocus, so keep scanning for HOME's previous state
               instead of pausing for ~100 ms. */
            if (kind != FocusEventKind::InFocus && scan->closed_count > 0) {
                return false;
            }
            /* qlaunch only owns the foreground while it has focus. */
            query->home = kind == FocusEventKind::InFocus;
            query->decided = true;
            return true;

        case FocusTarget::Application:
            /* A game/homebrew that loses focus or terminates hands the
               foreground back to the HOME Menu. */
            query->home = kind != FocusEventKind::InFocus;
            query->decided = true;
            return true;

        case FocusTarget::Applet:
            if (kind == FocusEventKind::InFocus) {
                query->home = AppletKeepsMusic(event);
                query->decided = true;
                return true;
            }
            /* An applet that terminated is gone: keep scanning for the applet
               underneath it (or for the HOME Menu). */
            if (kind == FocusEventKind::Terminated) {
                MarkAppletClosed(scan, event);
            }
            return false;

        case FocusTarget::Ignore:
            break;
    }

    return false;
}

auto ResolveFocusQuery(u64 app_tid, s32 count, FocusQuery* query) -> bool {
    FocusScan scan{};

    for (s32 i = count - 1; i >= 0; --i) {
        if (ApplyFocusEvent(query, &scan, g_focus_view[i], app_tid)) {
            return true;
        }
    }

    return false;
}

void FillFocusViewFromAppletEvents(s32 start_entry, s32 count) {
    for (s32 i = 0; i < count; ++i) {
        g_focus_view[i] = FocusEventView{
            static_cast<u32>(start_entry + i),
            g_applet_events[i].program_id,
            AppletId_None,
            g_applet_events[i].event_type,
        };
    }
}

void FillFocusViewFromPlayEvents(s32 start_entry, s32 count, s32* out_count) {
    s32 entries = 0;

    for (s32 i = 0; i < count; ++i) {
        if (g_play_events[i].play_event_type != PdmPlayEventType_Applet) {
            continue;
        }

        g_focus_view[entries] = FocusEventView{
            static_cast<u32>(start_entry + i),
            PdmProgramIdToU64(g_play_events[i].event_data.applet.program_id),
            g_play_events[i].event_data.applet.applet_id,
            g_play_events[i].event_data.applet.event_type,
        };
        ++entries;
    }

    *out_count = entries;
}

auto FocusOwnerFromTarget(FocusTarget target) -> pm::FocusOwner {
    switch (target) {
        case FocusTarget::Home:
            return pm::FocusOwner::Home;
        case FocusTarget::Application:
            return pm::FocusOwner::Application;
        case FocusTarget::Applet:
            return pm::FocusOwner::Applet;
        case FocusTarget::Ignore:
            break;
    }

    return pm::FocusOwner::Unknown;
}

auto FocusTargetName(FocusTarget target) -> const char* {
    switch (target) {
        case FocusTarget::Home:
            return "home";
        case FocusTarget::Application:
            return "app";
        case FocusTarget::Applet:
            return "applet";
        case FocusTarget::Ignore:
            break;
    }

    return "ignore";
}

/* Optional breadcrumb trail for diagnosing focus behaviour on-device.
   Enabled with "[config] focus_log=1"; nothing is written otherwise. */
void LogFocusEvents(s32 count, u64 app_tid, const FocusQuery& query, bool decided) {
    if (!g_focus_log || count <= 0) {
        return;
    }

    if (!g_focus_log_dir_ready) {
        sdmc::CreateFolder("/config");
        sdmc::CreateFolder("/config/sys-tune");
        g_focus_log_dir_ready = true;
    }

    char buffer[FOCUS_LOG_MAX_LINES * 104 + 168];
    int used = 0;

    const auto append = [&](const char* format, auto... args) {
        const auto room = static_cast<int>(sizeof(buffer)) - used;
        if (room <= 1) {
            return;
        }

        const auto written = std::snprintf(buffer + used, static_cast<size_t>(room), format, args...);
        if (written <= 0) {
            return;
        }

        used += std::min(written, room - 1);
    };

    /* Oldest first, so the log reads chronologically. */
    for (s32 i = 0; i < count && i < static_cast<s32>(FOCUS_LOG_MAX_LINES); ++i) {
        const FocusEventView& event = g_focus_view[count - 1 - i];
        if (static_cast<s32>(event.entry_index) <= g_last_logged_entry_index) {
            continue;
        }

        append("ev idx=%u pid=%016llX applet=%u type=%u kind=%u target=%s\n",
            event.entry_index,
            static_cast<unsigned long long>(event.program_id),
            static_cast<unsigned>(event.applet_id),
            static_cast<unsigned>(event.event_type),
            static_cast<unsigned>(ClassifyFocusEventKind(event.event_type)),
            FocusTargetName(ClassifyFocusTarget(event.program_id, event.applet_id, app_tid)));

        g_last_logged_entry_index = static_cast<s32>(event.entry_index);
    }

    append("decision decided=%d home=%d target=%s pid=%016llX applet=%u type=%u\n",
        decided ? 1 : 0,
        query.home ? 1 : 0,
        FocusTargetName(query.target),
        static_cast<unsigned long long>(query.program_id),
        static_cast<unsigned>(query.applet_id),
        static_cast<unsigned>(query.event_type));

    if (used > 0) {
        /* Drop the trailing newline; pm::Log adds its own. */
        if (buffer[used - 1] == '\n') {
            buffer[used - 1] = '\0';
        }
        pm::Log("focus scan\n%s", buffer);
    }
}

auto QueryHomeForegroundFromPdm(u64 app_tid, bool* home_foreground) -> Result {
    if (!g_pdmqry_initialized) {
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    s32 total_entries{};
    s32 start_entry_index{};
    s32 end_entry_index{};
    Result rc = pdmqryGetAvailablePlayEventRange(&total_entries, &start_entry_index, &end_entry_index);
    if (R_FAILED(rc)) {
        return rc;
    }

    if (end_entry_index == g_last_play_event_end && g_cached_home_valid && app_tid == g_cached_app_tid) {
        *home_foreground = g_cached_home_foreground;
        g_focus_owner = g_cached_focus_owner;
        return 0;
    }

    g_last_play_event_end = end_entry_index;

    /* The ini is re-read whenever the focus event log changed, so a setting
       toggled in the overlay applies the moment it matters. */
    if (!g_log_suspended) {
        g_home_applet_music = config::get_home_applet_music();
        g_focus_log = config::get_focus_log();
    }

    s32 start_entry = end_entry_index - (EVENT_COUNT - 1);
    if (start_entry < 0) {
        start_entry = 0;
    }

    s32 out{};
    s32 count{};
    rc = pdmqryQueryAppletEvent(start_entry, hosversionAtLeast(10, 0, 0), g_applet_events, EVENT_COUNT, &out);
    if (R_SUCCEEDED(rc) && out > 0) {
        count = std::min(out, EVENT_COUNT);
        FillFocusViewFromAppletEvents(start_entry, count);
    }

    FocusQuery query{};
    bool decided = ResolveFocusQuery(app_tid, count, &query);

    if (!decided) {
        /* Event types vary with firmware, so fall back to the play event log
           before reporting "unknown". */
        out = 0;
        rc = pdmqryQueryPlayEvent(start_entry, g_play_events, EVENT_COUNT, &out);
        if (R_FAILED(rc)) {
            g_cached_home_valid = false;
            return rc;
        }

        count = std::min(out, EVENT_COUNT);
        FillFocusViewFromPlayEvents(start_entry, count, &count);
        decided = ResolveFocusQuery(app_tid, count, &query);
    }

    LogFocusEvents(count, app_tid, query, decided);

    if (!decided) {
        g_cached_home_valid = false;
        return 1;
    }

    *home_foreground = query.home;
    g_focus_owner = FocusOwnerFromTarget(query.target);
    g_cached_home_foreground = query.home;
    g_cached_focus_owner = g_focus_owner;
    g_cached_home_valid = true;
    g_cached_app_tid = app_tid;
    return 0;
}

/* ---- From Fimochi/MenuMusicNX#3 (CLAPD): extra gates that keep the music
   silent at boot, while the console sleeps/shuts down, while a game is being
   launched, and while certain applets have a live process. ---- */

/// Programs for system applets that are NOT the HOME Menu itself.
/// While any of these has a live process, music must stay paused, even if no
/// game is running (e.g. user select at game launch) or the game is still the
/// "application" (e.g. software keyboard opened from inside a game).
/// Add more title IDs here if you want other applets to silence the music.
struct BlockingApplet {
    u64 title_id;
    bool only_with_game; // true: only silence music if a game is running
};

/* Focus events already decide user select, keyboard and controllers (by who
   opened them). The process check stays only for the Album, as a backstop
   for homebrew in applet mode. */
constexpr BlockingApplet BLOCKING_APPLETS[] = {
    {0x010000000000100DULL, false}, // photoViewer (Album). Homebrew in applet mode runs inside this applet
};
constexpr size_t BLOCKING_APPLET_COUNT = sizeof(BLOCKING_APPLETS) / sizeof(BLOCKING_APPLETS[0]);

/// True while an application has been launched (pdm "Launch" event) but has
/// not exited yet. Right after you pick a game there is a window with no
/// application process (user select screen, loading) where the old logic
/// wrongly reported HOME.
/// Event types seen on hardware: 0 = Launch, 1 = Exit (applet_id 1 = application).
auto ApplicationLaunchPending() -> bool {
    if (!g_pdmqry_initialized) {
        return false;
    }

    s32 total{}, start{}, end{};
    if (R_FAILED(pdmqryGetAvailablePlayEventRange(&total, &start, &end))) {
        return false;
    }

    /* Polled every 50 ms: only rescan when the event log grew. */
    static s32 cached_end{-1};
    static bool cached_pending{};
    static u64 pending_since_ms{};
    /* A launch that was cancelled (e.g. backing out of user select) may never
       log an Exit event: stop treating it as pending after a while. */
    constexpr u64 LAUNCH_PENDING_MAX_MS = 30'000;
    const u64 now_ms = armTicksToNs(armGetSystemTick()) / 1000000ULL;
    if (end == cached_end) {
        return cached_pending && now_ms - pending_since_ms < LAUNCH_PENDING_MAX_MS;
    }
    cached_end = end;
    cached_pending = false;

    s32 first = end - (EVENT_COUNT - 1);
    if (first < 0) {
        first = 0;
    }

    s32 out{};
    if (R_FAILED(pdmqryQueryPlayEvent(first, g_play_events, EVENT_COUNT, &out))) {
        return false;
    }

    for (s32 i = out - 1; i >= 0; --i) {
        const PdmPlayEvent* e = &g_play_events[i];
        if (e->play_event_type != PdmPlayEventType_Applet ||
            e->event_data.applet.applet_id != AppletId_application) {
            continue;
        }

        const u8 t = e->event_data.applet.event_type;
        if (t == 0) {
            cached_pending = true; // Launch is the newest application lifecycle event
            pending_since_ms = now_ms;
            return true;
        }
        if (t == 1) {
            return false; // Exit
        }
    }

    return false;
}

// ---- Boot gate: keep music off until HOME has really appeared this boot ----
constexpr u64 BOOT_RESTART_UPTIME_MS = 90'000; // first poll later than this = manual restart, not a boot
constexpr u64 BOOT_FALLBACK_MS       = 60'000; // never hold the music longer than this after the first poll

bool g_boot_gate_open{};
bool g_boot_gate_init{};
s32 g_boot_baseline_end{-1};
u64 g_boot_first_poll_ms{};

auto NowMs() -> u64 {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

auto BootGateOpen() -> bool {
    if (g_boot_gate_open) {
        return true;
    }

    const u64 now = NowMs();

    if (!g_boot_gate_init) {
        g_boot_gate_init = true;
        g_boot_first_poll_ms = now;

        s32 total{}, start{}, end{};
        if (g_pdmqry_initialized && R_SUCCEEDED(pdmqryGetAvailablePlayEventRange(&total, &start, &end))) {
            g_boot_baseline_end = end;
        } else {
            g_boot_baseline_end = -1;
        }

        if (g_boot_baseline_end < 0 || now > BOOT_RESTART_UPTIME_MS) {
            g_boot_gate_open = true;
            return true;
        }
    }

    if (now - g_boot_first_poll_ms > BOOT_FALLBACK_MS) {
        g_boot_gate_open = true;
        return true;
    }

    // Open once a NEW qlaunch InFocus event shows up (history from previous
    // sessions is ignored: it is older than the baseline index).
    s32 total{}, start{}, end{};
    if (!g_pdmqry_initialized || R_FAILED(pdmqryGetAvailablePlayEventRange(&total, &start, &end))) {
        return false;
    }
    if (end <= g_boot_baseline_end) {
        return false;
    }

    s32 first = end - (EVENT_COUNT - 1);
    if (first < 0) {
        first = 0;
    }

    s32 out{};
    if (R_FAILED(pdmqryQueryPlayEvent(first, g_play_events, EVENT_COUNT, &out))) {
        return false;
    }

    for (s32 i = 0; i < out; ++i) {
        const PdmPlayEvent* e = &g_play_events[i];
        if (first + i > g_boot_baseline_end &&
            e->play_event_type == PdmPlayEventType_Applet &&
            (e->event_data.applet.applet_id == AppletId_SystemAppletMenu ||
             (e->event_data.applet.applet_id == AppletId_None &&
              PdmProgramIdToU64(e->event_data.applet.program_id) == QLAUNCH_TITLE_ID)) &&
            e->event_data.applet.event_type == PdmAppletEventType_InFocus) {
            g_boot_gate_open = true;
            return true;
        }
    }

    return false;
}
// ---- end boot gate ----

// ---- Power-down hold: silence music when the console sleeps or shuts down ----
// pdm logs power-state events (play event type 2). On hardware the first raw
// byte of the event was 2 on sleep and 3 on wake; the shutdown value was not
// observed, so any sub-type other than "on" (0) and "wake" (3) counts as the
// console going down. The hold clears on wake/on, or after a timeout in case an
// unrelated power event slips through, so the music can never stay off for good.
constexpr u64 POWER_DOWN_HOLD_MS = 30'000;
constexpr u8  PLAY_EVENT_POWER_STATE = 2;
constexpr u8  POWER_EVENT_ON = 0;
constexpr u8  POWER_EVENT_WAKE = 3;

s32 g_power_last_end{-1};
u64 g_power_down_until_ms{};

auto AppletName(u8 applet_id) -> const char* {
    switch (applet_id) {
        case AppletId_None: return "none";
        case AppletId_application: return "application";
        case AppletId_OverlayApplet: return "overlay";
        case AppletId_SystemAppletMenu: return "qlaunch";
        case AppletId_SystemApplication: return "starter";
        case AppletId_LibraryAppletAuth: return "auth";
        case AppletId_LibraryAppletCabinet: return "cabinet";
        case AppletId_LibraryAppletController: return "controller";
        case AppletId_LibraryAppletDataErase: return "dataErase";
        case AppletId_LibraryAppletError: return "error";
        case AppletId_LibraryAppletNetConnect: return "netConnect";
        case AppletId_LibraryAppletPlayerSelect: return "playerSelect";
        case AppletId_LibraryAppletSwkbd: return "swkbd";
        case AppletId_LibraryAppletMiiEdit: return "miiEdit";
        case AppletId_LibraryAppletWeb: return "web";
        case AppletId_LibraryAppletShop: return "shop";
        case AppletId_LibraryAppletPhotoViewer: return "photoViewer";
        case AppletId_LibraryAppletSet: return "set";
        case AppletId_LibraryAppletOfflineWeb: return "offlineWeb";
        case AppletId_LibraryAppletLoginShare: return "loginShare";
        case AppletId_LibraryAppletWifiWebAuth: return "wifiWebAuth";
        case AppletId_LibraryAppletMyPage: return "myPage";
        default: return "unknown";
    }
}

/* Lists every live process in the system applet title range
   (0x0100000000001000-0x01000000000010FF). */
void LogRunningSystemApplets(u8 applet_id) {
    if (!g_pmdmnt_initialized) {
        return;
    }

    char list[512];
    int used = 0;
    list[0] = '\0';
    for (u64 tid = 0x0100000000001000ULL; tid <= 0x01000000000010FFULL; ++tid) {
        u64 pid{};
        if (R_SUCCEEDED(pmdmntGetProcessId(&pid, tid))) {
            const int room = static_cast<int>(sizeof(list)) - used;
            if (room <= 1) {
                break;
            }
            const int written = std::snprintf(list + used, room, " %016llX(pid %llu)",
                static_cast<unsigned long long>(tid), static_cast<unsigned long long>(pid));
            if (written > 0) {
                used += std::min(written, room - 1);
            }
        }
    }
    pm::Log("unknown applet_id=%u, live system applets:%s", static_cast<unsigned>(applet_id), list);
}

/* Dumps every new pdm play event (applet, account, power, operation mode,
   ...) so unknown signals such as unlocking the lock screen can be found. */
void LogRawPlayEvent(s32 index, const PdmPlayEvent& e) {
    if (!g_focus_log) {
        return;
    }

    char hex[sizeof(e.event_data.data) * 2 + 1]{};
    for (size_t i = 0; i < sizeof(e.event_data.data); ++i) {
        std::snprintf(hex + i * 2, 3, "%02X", e.event_data.data[i]);
    }

    if (e.play_event_type == PdmPlayEventType_Applet) {
        const u8 applet_id = e.event_data.applet.applet_id;
        pm::Log("play idx=%d applet pid=%016llX applet_id=%u(%s) event=%u mode=%u steady=%llu raw=%s",
            index,
            static_cast<unsigned long long>(PdmProgramIdToU64(e.event_data.applet.program_id)),
            static_cast<unsigned>(applet_id), AppletName(applet_id),
            static_cast<unsigned>(e.event_data.applet.event_type),
            static_cast<unsigned>(e.event_data.applet.unk_x8.applet.mode),
            static_cast<unsigned long long>(e.timestamp_steady),
            hex);

        /* Applet ids newer than libnx's list: name the program by listing the
           system applet processes that are alive right now. */
        const u8 event_type = e.event_data.applet.event_type;
        if (applet_id > AppletId_LibraryAppletMyPage &&
            (event_type == PdmAppletEventType_Launch || event_type == PdmAppletEventType_InFocus)) {
            LogRunningSystemApplets(applet_id);
        }
    } else {
        pm::Log("play idx=%d type=%u value=%u steady=%llu raw=%s",
            index,
            static_cast<unsigned>(e.play_event_type),
            static_cast<unsigned>(e.event_data.data[0]),
            static_cast<unsigned long long>(e.timestamp_steady),
            hex);
    }
}

auto PowerDownActive() -> bool {
    const u64 now = NowMs();

    s32 total{}, start{}, end{};
    if (g_pdmqry_initialized && R_SUCCEEDED(pdmqryGetAvailablePlayEventRange(&total, &start, &end))) {
        if (g_power_last_end < 0) {
            g_power_last_end = end; // ignore history from before we started
        } else if (end > g_power_last_end) {
            s32 first = end - (EVENT_COUNT - 1);
            if (first < 0) {
                first = 0;
            }

            s32 out{};
            if (R_SUCCEEDED(pdmqryQueryPlayEvent(first, g_play_events, EVENT_COUNT, &out))) {
                for (s32 i = 0; i < out; ++i) {
                    const PdmPlayEvent* e = &g_play_events[i];
                    if (first + i <= g_power_last_end) {
                        continue;
                    }

                    LogRawPlayEvent(first + i, *e);

                    if (static_cast<u8>(e->play_event_type) != PLAY_EVENT_POWER_STATE) {
                        continue;
                    }

                    const u8 sub_type = reinterpret_cast<const u8*>(e)[0];
                    if (sub_type == POWER_EVENT_WAKE || sub_type == POWER_EVENT_ON) {
                        g_power_down_until_ms = 0;
                        pm::Log("power event value=%u -> hold cleared", sub_type);
                    } else {
                        g_power_down_until_ms = now + POWER_DOWN_HOLD_MS;
                        pm::Log("power event value=%u -> hold %llu ms", sub_type,
                            static_cast<unsigned long long>(POWER_DOWN_HOLD_MS));
                    }
                }
            }
            g_power_last_end = end;
        }
    }

    return now < g_power_down_until_ms;
}
// ---- end power-down hold ----

} // namespace

namespace pm {

auto Initialize() -> Result {
    g_pmdmnt_initialized = R_SUCCEEDED(pmdmntInitialize());
    g_pminfo_initialized = R_SUCCEEDED(pminfoInitialize());
    g_pdmqry_initialized = R_SUCCEEDED(pdmqryInitialize());

    g_last_play_event_end = -1;
    g_cached_home_valid = false;
    g_cached_app_tid = 0;
    g_cached_focus_owner = pm::FocusOwner::Unknown;
    g_focus_owner = pm::FocusOwner::Unknown;
    g_last_logged_entry_index = -1;

    g_home_applet_music = config::get_home_applet_music();
    g_focus_log = config::get_focus_log();

    return 0;
}

void Exit() {
    if (g_pdmqry_initialized) {
        pdmqryExit();
        g_pdmqry_initialized = false;
    }
    if (g_pminfo_initialized) {
        pminfoExit();
        g_pminfo_initialized = false;
    }
    if (g_pmdmnt_initialized) {
        pmdmntExit();
        g_pmdmnt_initialized = false;
    }
}

void getCurrentPidTid(u64* pid_out, u64* tid_out) {
    if (pid_out == nullptr || tid_out == nullptr) {
        return;
    }

    *pid_out = UINT64_MAX;
    *tid_out = 0;
    if (!g_pmdmnt_initialized) {
        return;
    }

    Result rc{};
    if (R_SUCCEEDED(rc = pmdmntGetApplicationProcessId(pid_out))) {
        if (g_pminfo_initialized && 0x20f == pminfoGetProgramId(tid_out, *pid_out)) {
            *tid_out = QLAUNCH_TITLE_ID;
        }
    } else if (rc == 0x20f) {
        *pid_out = 0;
        *tid_out = QLAUNCH_TITLE_ID;
    }
}

auto IsHomeMenuForeground() -> bool {
    u64 pid{};
    u64 app_tid{};
    getCurrentPidTid(&pid, &app_tid);

    const bool no_application = pid == 0 || app_tid == QLAUNCH_TITLE_ID;

    /* QueryHomeForegroundFromPdm returns 0 only when the focus events actually
       decided the answer; "no application runs" is the fallback for every
       other case (no pdm:qry, undecidable or stale log). */
    bool home = no_application;
    const bool pdm_decided = QueryHomeForegroundFromPdm(app_tid, &home) == 0;
    if (!pdm_decided) {
        home = no_application;
    }
    const bool pdm_home = home;

    /* No application process yet, but a game was just picked (user select,
       loading screen): that is not the HOME Menu. */
    const bool launching = home && no_application && ApplicationLaunchPending();
    if (launching) {
        home = false;
    }

    /* Applets with a live process that must silence the music. User select
       always blocks (it belongs to launching a game); the others follow the
       pause_on_applet setting. */
    const bool game_running = pid != 0 && pid != UINT64_MAX && app_tid != QLAUNCH_TITLE_ID;
    bool blocked = false;
    u64 blocked_by = 0;
    if (home && g_pmdmnt_initialized) {
        for (size_t i = 0; i < BLOCKING_APPLET_COUNT && !blocked; ++i) {
            const auto& applet = BLOCKING_APPLETS[i];
            if (applet.only_with_game && !game_running) {
                continue;
            }
            u64 bpid{};
            if (R_SUCCEEDED(pmdmntGetProcessId(&bpid, applet.title_id))) {
                blocked = true;
                blocked_by = applet.title_id;
            }
        }
    }

    /* Evaluate both every poll so their event-log baselines stay current. */
    const bool boot_ready = BootGateOpen();
    const bool powering_down = PowerDownActive();

    const bool result = home && !blocked && boot_ready && !powering_down;

    /* Log every change of any input, so focus.log explains each decision. */
    struct Snapshot {
        u64 pid, app_tid, blocked_by;
        bool pdm_decided, pdm_home, launching, blocked, boot_ready, powering_down, result;
        pm::FocusOwner owner;
        bool operator==(const Snapshot&) const = default;
    };
    static Snapshot last{};
    static bool have_last = false;
    const Snapshot now{pid, app_tid, blocked_by, pdm_decided, pdm_home, launching, blocked,
        boot_ready, powering_down, result, g_focus_owner};
    if (!have_last || !(now == last)) {
        Log("state play=%d | pdm_decided=%d pdm_home=%d owner=%u launching=%d blocked=%d(%016llX) "
            "boot_ready=%d power_down=%d | app_pid=%lld app_tid=%016llX home_applet_music=%d",
            result, pdm_decided, pdm_home, static_cast<unsigned>(g_focus_owner), launching,
            blocked, static_cast<unsigned long long>(blocked_by), boot_ready, powering_down,
            static_cast<long long>(pid), static_cast<unsigned long long>(app_tid), g_home_applet_music);
        last = now;
        have_last = true;
    }

    return result;
}

auto GetFocusOwner() -> FocusOwner {
    return g_focus_owner;
}

auto UptimeMs() -> u64 {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

#ifndef MMNX_BUILD
#define MMNX_BUILD "dev"
#endif

namespace {
    Mutex g_log_mutex{};
    /* Lines logged while the console sleeps; written out on wake. */
    char g_log_pending[8 * 1024];
    size_t g_log_pending_size{};
    bool g_log_pending_overflow{};
}

void SetLogSuspended(bool suspended) {
    mutexLock(&g_log_mutex);
    g_log_suspended = suspended;
    if (!suspended && g_focus_log) {
        if (g_log_pending_size > 0) {
            sdmc::AppendFile(FOCUS_LOG_PATH, g_log_pending, g_log_pending_size);
        }
        if (g_log_pending_overflow) {
            constexpr char note[] = "[log] lines dropped while asleep (buffer full)\n";
            sdmc::AppendFile(FOCUS_LOG_PATH, note, sizeof(note) - 1);
        }
    }
    if (!suspended) {
        g_log_pending_size = 0;
        g_log_pending_overflow = false;
    }
    mutexUnlock(&g_log_mutex);
}

void Log(const char* format, ...) {
    if (!g_focus_log) {
        return;
    }

    char line[2048];
    int used = std::snprintf(line, sizeof(line), "[%8llu] ", static_cast<unsigned long long>(UptimeMs()));
    if (used < 0) {
        return;
    }

    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(line + used, sizeof(line) - used, format, args);
    va_end(args);
    if (written > 0) {
        used = std::min<int>(used + written, sizeof(line) - 2);
    }
    line[used++] = '\n';

    mutexLock(&g_log_mutex);

    /* The log is never truncated: it survives restarts and crashes (each line
       is flushed). Mark every boot so sessions can be told apart. */
    static bool session_marked = false;
    if (!session_marked && !g_log_suspended) {
        session_marked = true;
        if (!g_focus_log_dir_ready) {
            sdmc::CreateFolder("/config");
            sdmc::CreateFolder("/config/sys-tune");
            g_focus_log_dir_ready = true;
        }
        char marker[160];
        const int len = std::snprintf(marker, sizeof(marker),
            "\n==== MenuMusicNX %s started, uptime %llu ms ====\n", MMNX_BUILD,
            static_cast<unsigned long long>(UptimeMs()));
        if (len > 0) {
            sdmc::AppendFile(FOCUS_LOG_PATH, marker, std::min<size_t>(len, sizeof(marker) - 1));
        }
    }

    if (g_log_suspended) {
        /* No SD access while the console goes to sleep. */
        if (g_log_pending_size + used <= sizeof(g_log_pending)) {
            std::memcpy(g_log_pending + g_log_pending_size, line, used);
            g_log_pending_size += used;
        } else {
            g_log_pending_overflow = true;
        }
        mutexUnlock(&g_log_mutex);
        return;
    }
    if (!g_focus_log_dir_ready) {
        sdmc::CreateFolder("/config");
        sdmc::CreateFolder("/config/sys-tune");
        g_focus_log_dir_ready = true;
    }
    sdmc::AppendFile(FOCUS_LOG_PATH, line, static_cast<size_t>(used));
    mutexUnlock(&g_log_mutex);
}

} // namespace pm
