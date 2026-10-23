#pragma once
/* Process-wide state shared between the frontend (main) and the in-exe input plugin. */
#include <memory>

#include "lockstep.h"

namespace n64run {

struct RunState {
    int owner[kPorts] = {0, -1, -1, -1};   // port -> owning slot, -1 = not present
    bool mempak = false;                   // attach a Controller Pak to present ports (mempak_enabled)
    std::unique_ptr<Lockstep> lockstep;    // set by the frontend before EXECUTE
};

extern RunState g_run;

}  // namespace n64run
