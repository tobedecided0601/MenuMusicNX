#pragma once

#include <switch.h>

namespace pm {

/// Applet id seen on hardware for the offline web applet (010000000000100F)
/// when System Settings shows the EULA / IP notices / Wireless E-label.
/// Newer than libnx's AppletId list.
constexpr u8 AppletIdOfflineWebNew = 51;

/// Short name for an AppletId as logged by pdm (shared by sysmodule + overlay).
inline const char* AppletIdName(u8 applet_id) {
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
        case AppletIdOfflineWebNew: return "offlineWeb(new)";
        default: return "unknown";
    }
}

/// Short name for a PdmAppletEventType.
inline const char* AppletEventName(u8 event_type) {
    switch (event_type) {
        case PdmAppletEventType_Launch: return "launch";
        case PdmAppletEventType_Exit: return "exit";
        case PdmAppletEventType_InFocus: return "focus";
        case PdmAppletEventType_OutOfFocus: return "unfocus";
        case PdmAppletEventType_OutOfFocus4: return "background";
        case PdmAppletEventType_Exit5: return "exit5";
        case PdmAppletEventType_Exit6: return "exit6";
        default: return "?";
    }
}

/// pdm stores the two u32 halves of a program id swapped (see libnx pdm.h).
inline u64 PdmProgramId(const u32 program_id[2]) {
    return (static_cast<u64>(program_id[0]) << 32) | program_id[1];
}

} // namespace pm
