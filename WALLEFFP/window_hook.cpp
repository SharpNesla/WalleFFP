#include "window_hook.h"

#include <cstdio>

namespace {

// The bridge may re-subclass the window after us (e.g. on Reset), which pushes
// our proc below it. Each re-install takes a new slot with its own thunk so
// every link in the chain keeps its own "previous" proc. Only the outermost
// slot filters; inner slots pass everything through.
constexpr int kMaxSlots = 4;

HWND g_hwnd = nullptr;
bool g_unicode = false;
WNDPROC g_prev[kMaxSlots] = {};
int g_slotCount = 0;

int g_lastActivate = -1; // -1 unknown, 0 inactive, 1 active
unsigned g_dropped = 0;

LRESULT CallPrev(int slot, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return g_unicode ? CallWindowProcW(g_prev[slot], hwnd, msg, wParam, lParam)
                     : CallWindowProcA(g_prev[slot], hwnd, msg, wParam, lParam);
}

LRESULT FilterProc(int slot, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ACTIVATEAPP && slot == g_slotCount - 1) {
        const int active = wParam ? 1 : 0;
        if (active == g_lastActivate) {
            g_dropped++;
            if (g_dropped <= 5 || (g_dropped % 1000) == 0) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "[WALLEFFP] Dropped duplicate WM_ACTIVATEAPP(%d) (%u total)\n", active, g_dropped);
                OutputDebugStringA(buf);
            }
            return 0;
        }
        g_lastActivate = active;
    }
    return CallPrev(slot, hwnd, msg, wParam, lParam);
}

template <int Slot>
LRESULT CALLBACK SlotProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return FilterProc(Slot, hwnd, msg, wParam, lParam);
}

const WNDPROC kSlotProcs[kMaxSlots] = {SlotProc<0>, SlotProc<1>, SlotProc<2>, SlotProc<3>};

} // namespace

void InstallActivateAppFilter(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;

    if (g_hwnd && g_hwnd != hwnd) {
        OutputDebugStringA("[WALLEFFP] ActivateApp filter already bound to another window, skipping\n");
        return;
    }
    g_hwnd = hwnd;
    g_unicode = IsWindowUnicode(hwnd) != FALSE;

    const LONG_PTR cur = g_unicode ? GetWindowLongPtrW(hwnd, GWLP_WNDPROC) : GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
    if (g_slotCount > 0 && cur == reinterpret_cast<LONG_PTR>(kSlotProcs[g_slotCount - 1])) return; // still outermost

    if (g_slotCount == kMaxSlots) {
        OutputDebugStringA("[WALLEFFP] ActivateApp filter out of slots, not re-installing\n");
        return;
    }

    const int slot = g_slotCount;
    const LONG_PTR proc = reinterpret_cast<LONG_PTR>(kSlotProcs[slot]);
    const LONG_PTR prev = g_unicode ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, proc) : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, proc);
    if (!prev) {
        OutputDebugStringA("[WALLEFFP] ActivateApp filter: SetWindowLongPtr failed\n");
        return;
    }
    g_prev[slot] = reinterpret_cast<WNDPROC>(prev);
    g_slotCount = slot + 1;

    char buf[128];
    std::snprintf(buf, sizeof(buf), "[WALLEFFP] ActivateApp filter installed on hwnd=%p (slot %d, %s)\n",
                  static_cast<void*>(hwnd), slot, g_unicode ? "W" : "A");
    OutputDebugStringA(buf);
}
