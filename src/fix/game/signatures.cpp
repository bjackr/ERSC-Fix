#include "game/signatures.h"

#include "game/mem.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <vector>

namespace ersc_fix::game::signatures {

namespace {

struct Pattern {
    const char* name;
    size_t length;
    const char* bytes;
    const char* mask;
};

constexpr Pattern kPatterns[] = {
#include "game/signatures_data.inc"
};

struct Section {
    uintptr_t begin = 0, end = 0;
};

Section section(uintptr_t base, const char* name) {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::strncmp(reinterpret_cast<const char*>(sec->Name), name, 8) == 0) {
            return {base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize};
        }
    }
    return {};
}

std::vector<uintptr_t> find_all(const Pattern& p, Section s, size_t limit = 3) {
    std::vector<uintptr_t> hits;
    const auto* pat = reinterpret_cast<const uint8_t*>(p.bytes);
    const auto* msk = reinterpret_cast<const uint8_t*>(p.mask);
    constexpr size_t kChunk = 1 << 20;
    std::vector<uint8_t> buf(kChunk + 128);
    for (uintptr_t at = s.begin; at < s.end && hits.size() < limit; at += kChunk) {
        const size_t want = static_cast<size_t>(std::min<uintptr_t>(kChunk + p.length, s.end - at));
        if (!mem::copy(at, buf.data(), want)) continue;
        for (size_t i = 0; i + p.length <= want && i < kChunk; ++i) {
            if (buf[i] != pat[0]) continue;
            bool ok = true;
            for (size_t k = 1; k < p.length; ++k) {
                if (msk[k] && buf[i + k] != pat[k]) { ok = false; break; }
            }
            if (ok) {
                hits.push_back(at + i);
                if (hits.size() >= limit) break;
            }
        }
    }
    return hits;
}

const Pattern* pattern(const char* name) {
    for (const auto& p : kPatterns) if (std::strcmp(p.name, name) == 0) return &p;
    return nullptr;
}

uintptr_t rip_target(uintptr_t at) {
    uint8_t b[8];
    if (!mem::copy(at, b, sizeof(b)) || b[0] != 0x48) return 0;
    const size_t length = b[1] == 0x83 ? 8 : 7;
    int32_t disp;
    std::memcpy(&disp, b + 3, 4);
    return at + length + static_cast<intptr_t>(disp);
}

}

Resolved resolve(uintptr_t base) {
    Resolved r;
    const Section text = section(base, ".text");
    const Section rdata = section(base, ".rdata");
    const Section data = section(base, ".data");
    if (!text.begin || !rdata.begin || !data.begin) {
        r.report = "could not read the game's section table";
        return r;
    }
    auto unique = [&](const char* name) -> uintptr_t {
        const Pattern* p = pattern(name);
        if (!p) return 0;
        auto hits = find_all(*p, text);
        r.report += std::format("{}: {} match{}{}; ", name, hits.size(), hits.size() == 1 ? "" : "es",
                                hits.size() == 1 ? std::format(" at +0x{:X}", hits[0] - base) : std::string());
        return hits.size() == 1 ? hits[0] : 0;
    };
    auto in_data = [&](uintptr_t a) { return a >= data.begin && a < data.end; };
    auto rva = [&](uintptr_t a) { return static_cast<uint32_t>(a - base); };

    if (uintptr_t f = unique("type3_sender")) {
        const uintptr_t wcm = rip_target(f + 0x8E), sess = rip_target(f + 0x47);
        if (in_data(wcm)) r.world_chr_man = rva(wcm);
        if (in_data(sess)) r.session_man = rva(sess);
    }
    if (uintptr_t site = unique("evflag_site")) {
        const uintptr_t ev = rip_target(site + 0x10);
        if (in_data(ev)) r.cs_event_flag_man = rva(ev);
    }
    if (uintptr_t f = unique("get_flag")) r.get_event_flag = rva(f);

    auto vtable_of = [&](const char* name) -> uint32_t {
        const uintptr_t fn = unique(name);
        if (!fn) return 0;
        std::vector<uintptr_t> where;
        constexpr size_t kChunk = 1 << 20;
        std::vector<uint8_t> buf(kChunk + 8);
        for (uintptr_t at = rdata.begin; at < rdata.end && where.size() < 2; at += kChunk) {
            const size_t want = static_cast<size_t>(std::min<uintptr_t>(kChunk + 8, rdata.end - at));
            if (!mem::copy(at, buf.data(), want)) continue;
            for (size_t i = 0; i + 8 <= want && i < kChunk; i += 8) {
                uintptr_t v;
                std::memcpy(&v, buf.data() + i, 8);
                if (v == fn) where.push_back(at + i);
            }
        }
        return where.size() == 1 ? rva(where[0] - 8 * 10) : 0;
    };
    r.vt_network = vtable_of("netman_update");
    r.vt_netai = vtable_of("netai_update");
    r.vt_pad = vtable_of("pad_update");
    if (uintptr_t f = unique("ride_mount")) r.ride_mount = rva(f);
    if (uintptr_t f = unique("ride_dismount")) r.ride_dismount = rva(f);
    if (uintptr_t f = unique("net_player_spawn")) r.net_player_spawn = rva(f);
    if (uintptr_t f = unique("chr_set_disabled")) r.chr_set_disabled = rva(f);
    if (uintptr_t f = unique("set_flag")) r.set_event_flag = rva(f);
    if (uintptr_t f = unique("volatile_reset")) r.volatile_reset = rva(f);
    if (uintptr_t f = unique("flag_block_apply")) r.flag_block_apply = rva(f);
    if (uintptr_t f = unique("flag_slot_legacy")) r.flag_slot_legacy = rva(f);
    if (uintptr_t f = unique("bullet_spawn")) r.bullet_spawn = rva(f);
    if (uintptr_t f = unique("field_ins_resolve")) r.field_ins_resolve = rva(f);
    if (const Pattern* p = pattern("pack_flags_pair")) {
        auto hits = find_all(*p, text);
        r.report += std::format("pack_flags_pair: {} match{}; ", hits.size(), hits.size() == 1 ? "" : "es");
        if (hits.size() == 2) {
            std::sort(hits.begin(), hits.end());
            r.pack_block_flags = rva(hits[0]);
            r.pack_volatile_flags = rva(hits[1]);
        }
    }

    r.complete = r.world_chr_man && r.cs_event_flag_man && r.session_man;
    r.report += std::format("-> WorldChrMan 0x{:X} SessionMan 0x{:X} CSEventFlagMan 0x{:X} GetFlag 0x{:X} vtables 0x{:X}/0x{:X}/0x{:X} "
                            "RideMount 0x{:X} RideDismount 0x{:X} NetPlayerSpawn 0x{:X}",
                            r.world_chr_man, r.session_man, r.cs_event_flag_man, r.get_event_flag, r.vt_network, r.vt_pad,
                            r.vt_netai, r.ride_mount, r.ride_dismount, r.net_player_spawn);
    return r;
}

}
