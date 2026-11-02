#include "friend_pick_ui.h"
#include "attach_xfer.h"

#include <algorithm>
#include <filesystem>

namespace hubui {

void FriendPicker::open(const std::string& title, const std::string& hint, int max_friends) {
    title_ = title; hint_ = hint; max_ = std::max(1, max_friends);
    sel_.clear(); chosen_.clear();
    want_open_ = true;
}

bool FriendPicker::draw(const char* id) {
    if (want_open_) { ImGui::OpenPopup(id); want_open_ = false; open_ = true; focus_ = true; }
    if (!open_) return false;
    ImVec2 vs = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(sp(54.f), vs.x * 0.9f), std::min(sp(58.f), vs.y * 0.8f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(2.f), sp(2.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.f));
    bool visible = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleVar(2);
    if (!visible) { open_ = false; return false; }
    bool sent = false;
    if (ui::modal_back()) { open_ = false; ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return false; }

    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted(title_.c_str());
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
    ImGui::TextWrapped("%s", hint_.c_str());
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    /* online friends first, then by name */
    std::vector<const hubnet::Friend*> fl;
    for (const auto& f : fr_friends()) fl.push_back(&f);
    std::stable_sort(fl.begin(), fl.end(), [](const hubnet::Friend* a, const hubnet::Friend* b) {
        if (a->online() != b->online()) return a->online();
        return a->name < b->name;
    });
    const float foot = sp(5.5f);
    ImGui::BeginChild("##fpicklist", ImVec2(0, -foot), ImGuiChildFlags_NavFlattened);
    if (fl.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
        ImGui::TextWrapped("You don't have any friends to send to yet. Add one from the friends panel.");
        ImGui::PopStyleColor();
    }
    for (size_t i = 0; i < fl.size(); ++i) {
        const hubnet::Friend& f = *fl[i];
        const bool on = sel_.count(f.uid) > 0;
        std::string label = std::string(on ? "[x]  " : "[  ]  ") + f.name + (f.online() ? "   (online)" : "   (offline - gets it when back)");
        ImGui::PushID((int)i);
        if (focus_ && i == 0) { ImGui::SetKeyboardFocusHere(0); focus_ = false; }
        if (ui::button(label.c_str(), ImVec2(-FLT_MIN, sp(4.25f)), on)) {
            if (on) sel_.erase(f.uid);
            else if ((int)sel_.size() < max_) sel_.insert(f.uid);
            else set_toast("You can pick up to " + std::to_string(max_) + " friends at a time.", false, 3.0);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    const std::string go = sel_.empty() ? std::string("Send") : "Send to " + std::to_string(sel_.size());
    float bw = (ImGui::GetContentRegionAvail().x - sp(1.f)) * 0.5f;
    if (ui::button(go.c_str(), ImVec2(bw, sp(4.5f)), true, !sel_.empty())) {
        chosen_.assign(sel_.begin(), sel_.end());
        sent = true;
    }
    ImGui::SameLine(0, sp(1.f));
    if (ui::button("Cancel", ImVec2(bw, sp(4.5f)))) { open_ = false; ImGui::CloseCurrentPopup(); }
    if (sent) { open_ = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
    return sent;
}

/* ---------- upload + DM ---------- */
namespace {
struct SendJob { int job = 0; std::vector<std::string> uids; std::string text, name; };
std::vector<SendJob> g_jobs;
int g_pct = 0;
}  // namespace

bool attach_send_start(const std::string& path, const std::string& kind, const std::string& display_name, const std::vector<std::string>& uids,
                       const std::string& text) {
    if (uids.empty()) return false;
    std::error_code ec;
    auto sz = std::filesystem::file_size(std::filesystem::u8path(path), ec);
    if (ec) { set_toast("Couldn't read that file.", false, 4.0); return false; }
    std::string why = hub::attach::attach_size_problem((int64_t)sz, kind);
    if (!why.empty()) { set_toast(why, false, 7.0); return false; }
    if (FR.fake) {   // screenshot / QA aid: no server, the DM lines appear at once
        hubnet::Attach a;
        a.id = "fake" + std::to_string(g_jobs.size() + 1); a.name = hub::attach::sanitize_name(display_name); a.kind = kind; a.size = (int64_t)sz;
        a.expires = wall_ms() + 7 * 86400 * 1000LL;
        std::string names;
        for (const auto& u : uids) {
            fr_send_attach_dm(u, text, a);
            if (const hubnet::Friend* f = fr_find(u)) { if (!names.empty()) names += ", "; names += f->name; }
        }
        set_toast("Sent to " + names + ".", true, 4.0);
        return true;
    }
    SendJob j;
    j.job = hub::attach::upload(path, kind, display_name, uids);
    j.uids = uids; j.text = text; j.name = display_name;
    g_jobs.push_back(j);
    g_pct = 0;
    return true;
}

void attach_send_tick() {
    for (size_t i = 0; i < g_jobs.size();) {
        hub::attach::Result r;
        if (!hub::attach::poll(g_jobs[i].job, &r)) { g_jobs.erase(g_jobs.begin() + i); continue; }
        if (!r.done) { g_pct = r.percent; ++i; continue; }
        if (r.ok) {
            std::string names;
            for (const auto& u : g_jobs[i].uids) {
                fr_send_attach_dm(u, g_jobs[i].text, r.ref);
                if (const hubnet::Friend* f = fr_find(u)) { if (!names.empty()) names += ", "; names += f->name; }
            }
            set_toast("Sent to " + names + ".", true, 4.0);
        } else {
            set_toast(r.error.empty() ? std::string("Couldn't send that file.") : r.error, false, 8.0);
        }
        hub::attach::forget(g_jobs[i].job);
        g_jobs.erase(g_jobs.begin() + i);
    }
}

std::string attach_send_status() {
    if (g_jobs.empty()) return std::string();
    return "Sending... " + std::to_string(g_pct) + "%";
}

} // namespace hubui
