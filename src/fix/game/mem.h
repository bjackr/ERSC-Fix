#pragma once

#include "game/offsets.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ersc_fix::game::mem {

inline bool copy(uintptr_t address, void* out, size_t size) {
    if (address < 0x10000 || !size) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool readable(uintptr_t address, size_t size) {
    if (address < 0x10000 || !size) return false;
    __try {
        volatile const uint8_t* p = reinterpret_cast<const uint8_t*>(address);
        (void)p[0];
        (void)p[size - 1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
inline bool read(uintptr_t address, T& out) {
    return copy(address, &out, sizeof(T));
}

template <class T>
inline bool write(uintptr_t address, const T& value) {
    if (address < 0x10000) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline uintptr_t deref(uintptr_t address) {
    uintptr_t v = 0;
    return read(address, v) ? v : 0;
}

inline bool tree_is_nil(uintptr_t node) {
    uint8_t nil = 1;
    return !read(node + kTreeNodeIsNil, nil) || nil != 0;
}

template <class F>
inline void walk_tree(uintptr_t head_ptr, F&& visit) {
    uintptr_t head = deref(head_ptr);
    if (!head) return;
    uintptr_t node = deref(head + kTreeNodeLeft);
    for (size_t guard = 0; node && node != head && !tree_is_nil(node) && guard < 512; ++guard) {
        visit(node);
        uintptr_t right = deref(node + kTreeNodeRight);
        if (right && !tree_is_nil(right)) {
            node = right;
            for (;;) {
                uintptr_t left = deref(node + kTreeNodeLeft);
                if (!left || tree_is_nil(left)) break;
                node = left;
            }
        } else {
            uintptr_t parent = deref(node + kTreeNodeParent);
            while (parent && parent != head && deref(parent + kTreeNodeRight) == node) {
                node = parent;
                parent = deref(node + kTreeNodeParent);
            }
            node = parent;
        }
    }
}

template <class F>
inline void walk_list(uintptr_t head_ptr, F&& visit) {
    uintptr_t head = deref(head_ptr);
    if (!head) return;
    uintptr_t node = deref(head);
    for (size_t guard = 0; node && node != head && guard < 64; ++guard) {
        visit(node);
        node = deref(node);
    }
}

}
