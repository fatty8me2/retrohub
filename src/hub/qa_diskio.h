#pragma once
/* QA only: which file-system calls the UI thread makes while the game page draws (docs/QA_SHOTS.md, HUB_SHOT_DISKIO).
 *
 * Off unless a screenshot run (HUB_SCREENSHOT) sets HUB_SHOT_DISKIO=1. Then install() patches the import tables of the loaded modules
 * (not ntdll / kernel32 / kernelbase themselves) so CreateFile*, GetFileAttributes*, FindFirstFile*, CreateDirectory, DeleteFile and
 * MoveFileEx calls are counted before they run. Only calls on the UI thread count; device and pipe names (\\.\..., \\?\HID...) do not.
 * A Scope marks "the game page is drawing" (both hosts) and the desk's library health chip; calls inside one are the page's.
 * A Scope named "periodic" inside a page scope marks the full-window page's 4 s save-info refresh (kept as it always was): its calls are
 * reported apart. note_worker() counts a background worker the page started (the save-info and Wii U scans).
 *
 * Trace (HUB_SHOT_TRACE): a "diskio" line for every frame with page calls or workers (frame, time, counts, the first few paths), and at
 * exit one "diskio-sum" line: frames drawn, frames that drew the page, the time the page first drew (t0), and the counts from t0 + warm
 * (HUB_SHOT_DISKIO_WARM seconds, default 1) to the end ("late_*"): the steady state the QA check (qa\desk_idle_io.ps1) requires to be 0. */
namespace qadisk {

bool on();
void install();          // once the window is up (and again later for modules loaded since); no-op unless on()
void frame_begin();      // the main loop, before the frame's UI is built
void frame_end();        // after it: the per-frame trace line
void finish();           // at exit: the summary line
void note_worker(const char* what);

/* Calls made while one is alive (UI thread) belong to `what` ("page", "desk-health", "periodic" ...). */
struct Scope {
    explicit Scope(const char* what, bool page_drawn = true);   // page_drawn: counts this frame as one that drew the page
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    bool active = false;
};

/* QA's own writes (the trace file itself) are not counted while one is alive. */
struct Quiet {
    Quiet();
    ~Quiet();
    Quiet(const Quiet&) = delete;
    Quiet& operator=(const Quiet&) = delete;
};

}  // namespace qadisk
