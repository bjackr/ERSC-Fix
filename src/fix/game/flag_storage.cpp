#include "game/flag_storage.h"

#include "game/mem.h"
#include "game/offsets.h"

namespace ersc_fix::game::flag_storage {

namespace {

using namespace ersc_fix::game::mem;

uintptr_t g_game_base = 0;

struct Storage {
    uintptr_t mgr = 0, arr = 0, header = 0;
    uint32_t fpb = 0, bpb = 0;
};

bool read_storage(Storage& s) {
    if (!g_game_base) return false;
    s.mgr = deref(g_game_base + kCsEventFlagManPtr);
    if (!s.mgr) return false;
    if (!read(s.mgr + 0x1C, s.fpb) || !read(s.mgr + 0x20, s.bpb) || !read(s.mgr + 0x28, s.arr) || !read(s.mgr + 0x38, s.header)) return false;
    return s.fpb && s.bpb && s.header;
}

bool find_block(const Storage& s, uint32_t key, BlockLoc& out) {
    uintptr_t node = deref(s.header + 8), result = s.header;
    for (int guard = 0; guard < 64 && node; ++guard) {
        uint8_t nil = 1;
        if (!read(node + 0x19, nil) || nil) break;
        uint32_t k = 0;
        if (!read(node + 0x20, k)) return false;
        if (k < key) {
            node = deref(node + 0x10);
        } else {
            result = node;
            node = deref(node);
        }
    }
    if (result == s.header) return false;
    uint32_t k = 0;
    if (!read(result + 0x20, k) || k != key) return false;
    int type = 0;
    if (!read(result + 0x28, type)) return false;
    out.type = type;
    if (type == 1) {
        int idx = 0;
        if (!read(result + 0x30, idx)) return false;
        out.slot = idx;
        out.storage = s.arr + static_cast<uint32_t>(idx) * static_cast<uint32_t>(s.bpb);
    } else if (type == 2) {
        out.slot = -1;
        if (!read(result + 0x30, out.storage)) return false;
    } else {
        return false;
    }
    return out.storage != 0;
}

}

void init(uintptr_t game_base) { g_game_base = game_base; }

bool info(Info& out) {
    Storage st;
    if (!read_storage(st)) return false;
    out.mgr = st.mgr;
    out.arr = st.arr;
    out.fpb = st.fpb;
    out.bpb = st.bpb;
    int mode = 0;
    uint8_t b7c = 0, b7d = 0;
    uintptr_t alt = 0;
    uint64_t n158 = 0, n170 = 0;
    read(st.mgr + 0x78, mode);
    read(st.mgr + 0x7C, b7c);
    read(st.mgr + 0x7D, b7d);
    read(st.mgr + 0x190, alt);
    read(st.mgr + 0x160, n158);
    read(st.mgr + 0x178, n170);
    out.mode = mode;
    out.multi = b7c;
    out.skip_reset = b7d;
    out.alt_storage = alt;
    out.alt_blocks_158 = n158;
    out.alt_blocks_170 = n170;
    return true;
}

bool block_of(uint32_t key, BlockLoc& out) {
    Storage st;
    return read_storage(st) && find_block(st, key, out);
}

bool read_block(uint32_t key, std::vector<uint8_t>& bytes) {
    Storage st;
    BlockLoc loc;
    if (!read_storage(st) || !find_block(st, key, loc)) return false;
    bytes.resize(st.bpb);
    return copy(loc.storage, bytes.data(), st.bpb);
}

std::map<int, uint32_t> slot_keys() {
    std::map<int, uint32_t> out;
    Storage st;
    if (!read_storage(st)) return out;
    const uintptr_t head = st.header;
    uintptr_t node = deref(head);
    auto nil = [](uintptr_t n) { uint8_t v = 1; return !n || !read(n + 0x19, v) || v; };
    for (int guard = 0; !nil(node) && node != head && guard < 60000; ++guard) {
        uint32_t key = 0;
        int type = 0, idx = 0;
        if (read(node + 0x20, key) && read(node + 0x28, type) && type == 1 && read(node + 0x30, idx)) out[idx] = key;
        uintptr_t right = deref(node + 0x10);
        if (!nil(right)) {
            node = right;
            for (int g2 = 0; g2 < 64; ++g2) {
                uintptr_t left = deref(node);
                if (nil(left)) break;
                node = left;
            }
        } else {
            uintptr_t parent = deref(node + 8);
            for (int g2 = 0; g2 < 64 && parent && parent != head && deref(parent + 0x10) == node; ++g2) {
                node = parent;
                parent = deref(node + 8);
            }
            node = parent;
        }
    }
    return out;
}

uint32_t read_range(uint32_t first, uint32_t count, std::vector<uint8_t>& bits) {
    bits.assign((count + 7) / 8, 0);
    Storage st;
    if (!count || !read_storage(st)) return 0;
    uint32_t resolved = 0;
    std::vector<uint8_t> block;
    uint32_t key = 0xFFFFFFFFu;
    bool have = false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t f = first + i, k = f / st.fpb, idx = f % st.fpb;
        if (k != key) {
            key = k;
            have = read_block(k, block) && block.size() == st.bpb;
        }
        if (!have) continue;
        ++resolved;
        if ((block[idx >> 3] >> (7 - (idx & 7))) & 1) bits[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
    }
    return resolved;
}

int set_cleared(const std::vector<uint32_t>& flags, std::vector<uint32_t>* restored) {
    Storage st;
    if (flags.empty() || !read_storage(st)) return 0;
    std::map<uint32_t, BlockLoc> blocks;
    int resolved = 0;
    for (uint32_t f : flags) {
        const uint32_t key = f / st.fpb;
        auto it = blocks.find(key);
        if (it == blocks.end()) {
            BlockLoc loc;
            if (!find_block(st, key, loc)) loc.storage = 0;
            it = blocks.emplace(key, loc).first;
        }
        if (!it->second.storage) continue;
        const uint32_t idx = f % st.fpb;
        const uintptr_t byte = it->second.storage + (idx >> 3);
        const uint8_t bit = static_cast<uint8_t>(1u << (7 - (idx & 7)));
        uint8_t v = 0;
        if (!read(byte, v)) continue;
        ++resolved;
        if (v & bit) continue;
        if (write(byte, static_cast<uint8_t>(v | bit)) && restored) restored->push_back(f);
    }
    return resolved;
}

}
