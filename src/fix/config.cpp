#include "config.h"

#include <windows.h>

#include <cstdio>

namespace ersc_fix::config {

namespace {

std::wstring g_path;

constexpr const char* kDefaultIni =
    "; ersc_fix settings. Set a hook to 0 to disable it (e.g. if the game misbehaves),\r\n"
    "; then restart the game. The log lists which hooks were installed.\r\n"
    "[hooks]\r\n"
    "; bug-001: stop the crash dump on quit (data write only, no code patch).\r\n"
    "exit_abort = 1\r\n"
    "; Steam-layer hook (two pointers in Steam's own vtable; no code patched) that the message fixes below work\r\n"
    "; through; also logs message traffic. Required by block_reclaim, summon_replay, mount_probe, mount_replay, storm_breaker.\r\n"
    "steam_log = 1\r\n"
    "; bug-004: partner-copy repairs. This key runs the 250 ms loop behind dead_flag_repair and away_hide; also logs\r\n"
    "; each partner's character state on change.\r\n"
    "ride_probe = 1\r\n"
    "; bug-004 fix: when the partner dies, the game marks our copy of them dead; after they respawn nothing revives\r\n"
    "; the copy, so it stays a corpse (frozen, sliding, later invisible). Clears the two dead bits once the copy is restored.\r\n"
    "dead_flag_repair = 1\r\n"
    "; bug-004b fix: when the partner warps to another map (e.g. Roundtable), their copy is faded out by the game but\r\n"
    "; the model sometimes stays drawn, clipped into the ground. Holds the copy's draw alpha at 0 while it is in that state.\r\n"
    "away_hide = 1\r\n"
    "; bug-004c fix: while the partner is at the Roundtable, their position stream is rewritten so their copy stands\r\n"
    "; where the Roundtable is drawn: nameplate in the far-away list, map icon and compass marker there. See [away_relocate].\r\n"
    "away_relocate = 1\r\n"
    "; bug-008 fix: a partner's spirit-ash summon announced far away is never created here. Re-delivers the announcement\r\n"
    "; while the summon is still missing, then resends the partner's packets about it. Needs steam_log.\r\n"
    "summon_replay = 1\r\n"
    "; Tracks every non-player character in the synced sets (per-block sets included). The disabled-set sync and the\r\n"
    "; bullet guard depend on it; also logs state changes.\r\n"
    "enemy_probe = 1\r\n"
    "; bug-005 fix: a death reload rebuilds the dying player's world, and an enemy the game had disabled (a dragon that\r\n"
    "; flew off) comes back enabled on the rebuilt game only. Each game sends its peers the enemies it holds disabled;\r\n"
    "; a game that just rebuilt after its own death disables its fresh copies to match, through the game's own\r\n"
    "; ChrIns::SetDisabled on the game thread. Needs enemy_probe and steam_log. _sync exchanges the lists; _apply acts on them.\r\n"
    "disabled_set_sync = 1\r\n"
    "disabled_set_apply = 1\r\n"
    "; bug-005: event-flag sync. Reads the current map block's 10,000 event flags every second and exchanges the bitmap\r\n"
    "; with peers; the flag guard and handoff ([flag_probe]) depend on it. Also logs every flag that differs between the games.\r\n"
    "flag_probe = 1\r\n"
    "; bug-005 fix: Farum Azula's lightning is a map event-script loop that fires bolts from invisible bullet dummies until\r\n"
    "; the player nears the perched dragon, which then flies off and is disabled. A death reload restarts the loop on the\r\n"
    "; dying game only; while a continuous peer holds the dragon disabled, this game's bullets from the dummies are dropped.\r\n"
    "; Inline hook on the game's bullet spawn. Options in [bullet_guard].\r\n"
    "bullet_guard = 1\r\n"
    "; bug-003 fix: the mod reloads a partner's Torrent whenever their horse-type message arrives; while they ride that\r\n"
    "; leaves the horse invisible. A redundant message (same type as before) is dropped while their copy rides. Needs steam_log.\r\n"
    "mount_probe = 1\r\n"
    "; bug-003 fix (mount replay): keeps each partner's mount state from their ride records and hands their horse burst\r\n"
    "; back to the game when their character is created here. Needs steam_log.\r\n"
    "mount_replay = 1\r\n"
    "; bug-003 fix (spawn repair): when a partner's character is created here after you fast-travelled while they were\r\n"
    "; mounted, this game thinks they are on foot and their Torrent stays invisible; corrects that at creation. Needs\r\n"
    "; mount_replay. Inline hook on game code; if it cannot be installed the game closes with a message (set 0 to run without).\r\n"
    "spawn_repair = 1\r\n"
    "; bug-003 fix: at that creation, if the partner's Torrent already exists here with no map block, give it the block the\r\n"
    "; partner is in before the game activates it (activated without a block, the horse is never drawn until a remount).\r\n"
    "spawn_preplace = 1\r\n"
    "; bug-005 (block reclaim, host side): after a death the game hands every map block (= enemy authority) to the partner\r\n"
    "; and never takes it back. The host takes its loaded blocks back through the game's own ownership messages. Needs steam_log.\r\n"
    "block_reclaim = 1\r\n"
    "; bug-007 fix: break event-flag echo storms (two games fighting over one flag at 50+ msg/s). Drops our replies for a\r\n"
    "; ping-ponging flag for 2 s. Needs steam_log.\r\n"
    "storm_breaker = 1\r\n"
    "\r\n[away_relocate]\r\n"
    "; Where the Roundtable is drawn on the world map, as a map tile (0x3C XX ZZ 00) plus coordinates in the mod's\r\n"
    "; position frame for that tile.\r\n"
    "roundtable_block = 0x3C2B2500\r\n"
    "roundtable_x = -3226\r\n"
    "roundtable_y = 92\r\n"
    "roundtable_z = -1422\r\n"
    "; The rewrite is skipped while you are at the Roundtable yourself, so two players there see each other normally.\r\n"
    "; Underground (Siofra, Ainsel, Nokron, Deeproot, Mohgwyn): 1 = show the partner at the well/lift they took while only\r\n"
    "; one player is there; 0 = the mod's stock behaviour.\r\n"
    "underground = 0\r\n"
    "; Minor dungeons are placed at their true world position and tracked correctly by the mod. Leave at 0.\r\n"
    "dungeons = 0\r\n"
    "\r\n"
    "[bullet_guard]\r\n"
    "; 1 = drop the hazard's bullets under the rule above; 0 = log only.\r\n"
    "drop = 1\r\n"
    "; Model numbers (cXXXX) of the bullet dummies whose bullets are dropped, and of the dragon whose disabled state on\r\n"
    "; the peer means the peer's hazard loop has ended. Farum Azula: c0120 dummies, c4510 dragon.\r\n"
    "dummy_models = 120\r\n"
    "anchor_models = 4510\r\n"
    "; Map blocks (hex ids) the rule applies to; empty = any block. Default: Crumbling Farum Azula only.\r\n"
    "blocks = 0D000000\r\n"
    "\r\n"
    "[flag_probe]\r\n"
    "; bug-005 fix (flag handoff): the game pulls a block's character state from the block owner whenever a game (re)builds\r\n"
    "; that block, but nothing carries event flags, and a reload resets the volatile ones. When a peer requests a block we\r\n"
    "; are settled in, we answer with our flags for it; when we request a block, the answer's set volatile flags are written\r\n"
    "; here through the game's setter on the game thread, never clearing. 0 = log what would be adopted, write nothing.\r\n"
    "reconcile_after_reload = 1\r\n"
    "; Thousands digit of the flag tail that may be adopted (default: the game's volatile classes).\r\n"
    "reconcile_classes = 2,3,5\r\n"
    "; Flag ids never adopted, comma-separated.\r\n"
    "reconcile_deny =\r\n"
    "; bug-005 fix (flag guard): a reload zeroes the map's volatile flag blocks twice (the game's volatile reset and the copy\r\n"
    "; of the block owner's blocks out of the 0x43 reply). Both writers and the flag setter are hooked; a guarded flag they\r\n"
    "; clear is set again before the writer returns. Guarded = every volatile flag the continuous peer holds set in the block.\r\n"
    "guard = 1\r\n"
    "; When the peer clears a guarded flag this game wrote, clear it here as well (the flag is the peer's). 0 = release only.\r\n"
    "guard_mirror_clear = 1\r\n"
    "; The game packs a block's flags into its 0x43 reply from a stale copy when it is a session guest. 1 = replace the\r\n"
    "; outgoing bytes with what this game reads.\r\n"
    "pack_fix = 1\r\n"
    ;

}

const std::wstring& ini_path() {
    return g_path;
}

void load(const std::filesystem::path& dir) {
    g_path = (dir / L"ersc_fix.ini").wstring();
    if (GetFileAttributesW(g_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfsopen(g_path.c_str(), L"wb", _SH_DENYWR)) {
            std::fputs(kDefaultIni, f);
            std::fclose(f);
        }
    }
}

bool hook_enabled(const wchar_t* name) {
    return GetPrivateProfileIntW(L"hooks", name, 0, g_path.c_str()) != 0;
}

}
