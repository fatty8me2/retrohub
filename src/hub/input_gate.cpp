#include "input_gate.h"

#include "imgui.h"
#include "imgui_internal.h"

namespace inputgate {

int purge_pad_presses(ImGuiContext* ctx) {
    if (!ctx) return 0;
    ImVector<ImGuiInputEvent>& q = ctx->InputEventsQueue;
    int removed = 0, w = 0;
    for (int r = 0; r < q.Size; ++r) {
        const ImGuiInputEvent& e = q[r];
        if (e.Type == ImGuiInputEventType_Key && e.Key.Down && e.Key.Key >= ImGuiKey_GamepadStart && e.Key.Key <= ImGuiKey_GamepadRStickDown) { ++removed; continue; }
        if (w != r) q[w] = q[r];
        ++w;
    }
    q.resize(w);
    return removed;
}

}  // namespace inputgate
