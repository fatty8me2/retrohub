# RetroHub patches to the fetched Beetle PSX tree, applied in place at configure time (idempotent: every replacement carries a
# RETROHUB-RB-<id> marker and is skipped when it is already there).
# Purpose: make savestates exact enough for rollback netplay (docs/CORES.md, "PS1 determinism"). Every patch states the state it
# restores or the nondeterminism it removes. Called from fetch.cmake right after the tree is populated.

# beetle_patch(src rel id anchor replacement): replace the first occurrence of `anchor` (must exist unless already patched).
function(beetle_patch src rel id anchor repl)
    set(f ${src}/${rel})
    file(READ ${f} c)
    string(FIND "${c}" "RETROHUB-RB-${id}" done)
    if(NOT done EQUAL -1)
        return()
    endif()
    string(FIND "${c}" "${anchor}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "beetle_psx patch ${id}: anchor not found in ${rel}")
    endif()
    string(REPLACE "${anchor}" "${repl}" c "${c}")
    file(WRITE ${f} "${c}")
endfunction()

function(beetle_apply_patches src)
    # 1) The SW scanout cache ("this surface row's margins are already zero") describes the surface as the previous frames of THIS
    #    timeline left it. A state load switches timelines without touching the surface, so a frame whose width differs from the
    #    cached one kept stale margin pixels: re-simulated frames showed different borders than the first run. Drop it on load.
    beetle_patch(${src} mednafen/psx/gpu.c scanout
        "      gpu_restore_state_p3(!(diff_load && ret));\n"
        "      GPU_InvalidateScanoutCache();   /* RETROHUB-RB-scanout: the cache is not part of the state */\n      gpu_restore_state_p3(!(diff_load && ret));\n")

    # 2) The software surface is not part of the state: rows a frame does not rewrite (just after a display-mode change, outside the
    #    visible scanline range) keep whatever the PREVIOUS frames of this timeline left, so a state load showed other pixels than the
    #    first run (rollback netplay then sees false video mismatches). Serialize the surface with the rest of MAIN.
    beetle_patch(${src} libretro.c surface
        "int StateAction(StateMem *sm, int load, int data_only)\n{\n"
        "static uint8_t rb_surf_dummy[4];   /* RETROHUB-RB-surface */\nint StateAction(StateMem *sm, int load, int data_only)\n{\n")
    beetle_patch(${src} libretro.c surface2
        "      SFARRAYN(MainRAM->data8, 1024 * 2048, \"MainRAM.data8\"),\n"
        "      SFARRAYN(MainRAM->data8, 1024 * 2048, \"MainRAM.data8\"),\n      SFARRAYN(surf ? (uint8_t *)surf->pixels : rb_surf_dummy, surf ? (size_t)surf->h * (size_t)surf->pitchinpix * 4 : 4, \"RHSurf\"),   /* RETROHUB-RB-surface2 */\n")

    # 3) Distinct library name so retrorun can tell this build (verified for rollback: SyncTest exact) from a stock core.
    beetle_patch(${src} libretro.c libname
        "   info->library_name     = MEDNAFEN_CORE_NAME;
"
        "   info->library_name     = MEDNAFEN_CORE_NAME \" (rollback)\";   /* RETROHUB-RB-libname */
")
endfunction()
