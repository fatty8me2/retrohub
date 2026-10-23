# Generates <gen>/libretro.c (patched copy of SameBoy's libretro.c) and the five *_boot.c arrays from BootROMs/prebuilt/*.bin.
function(sameboy_rb_prepare src gen)
    foreach(n agb cgb dmg sgb sgb2)
        file(READ ${src}/BootROMs/prebuilt/${n}_boot.bin hex HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " arr "${hex}")
        file(WRITE ${gen}/${n}_boot.c
            "const unsigned char ${n}_boot[] = {${arr}};\nconst unsigned ${n}_boot_length = sizeof(${n}_boot);\n")
    endforeach()
    file(READ ${src}/libretro/libretro.c c)
    # 1) no joypad bounce, no random power-on RAM (both depend on state the savestate does not hold)
    set(anchor "    GB_set_boot_rom_load_callback(&gameboy[i], boot_rom_load);")
    string(FIND "${c}" "${anchor}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "sameboy_rb: patch anchor not found in libretro.c (SameBoy source changed?)")
    endif()
    string(REPLACE "${anchor}" "    /* RetroHub rollback netplay: deterministic across savestate loads */\n    GB_set_emulate_joypad_bouncing(&gameboy[i], false);\n    GB_random_set_enabled(false);\n${anchor}" c "${c}")
    # 1b) power-on RAM/OAM/HRAM/palettes are filled by GB_random() inside GB_init/GB_reset: the generator must be off BEFORE the first
    #     GB_init, otherwise every PC starts with different unwritten memory (extra_oam, uninitialised WRAM) and games that read it diverge.
    set(a_init "static void init_for_current_model(unsigned id)\n{\n")
    string(FIND "${c}" "${a_init}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "sameboy_rb: init_for_current_model anchor not found")
    endif()
    string(REPLACE "${a_init}" "${a_init}    GB_random_set_enabled(false);   /* RetroHub: before GB_init */\n" c "${c}")
    # 2) distinct library name so retrorun can tell this build from the stock core
    string(REPLACE "info->library_name     = \"SameBoy\";" "info->library_name     = \"SameBoy (rollback)\";" c "${c}")
    # 3) Host-side state that is not in SameBoy's savestate but changes what a re-simulated frame looks like or does:
    #    - bit_to_send1/2: the in-flight serial bits of the dual-GB link cable (a rollback landing mid-transfer re-simulated it differently);
    #    - the two LCD-off "retained" frames and each GB's pixel buffer: frames the PPU does not fully repaint (LCD just turned on,
    #      fades) show whatever the buffer held before, so the picture depended on the timeline that preceded the load.
    #    Appended after the GB states: 16 header bytes (the two bits) + retained1 + retained2 + pixels0 + pixels1 (160x144 RGBA each).
    string(REPLACE "return maximum_save_size * 2;" "return maximum_save_size * 2 + RB_EXTRA_SIZE;" c "${c}")
    string(REPLACE "static size_t retro_serialize_size_unused;" "" c "${c}")
    set(a_hdr "#include \"libretro_core_options.h\"")
    string(REPLACE "${a_hdr}" "${a_hdr}\n#define RB_PIX (160 * 144 * 4)\n#define RB_EXTRA_SIZE (16 + 4 * RB_PIX)" c "${c}")
    set(a_ser "        offset += state_size;\n        size -= state_size;\n    }\n\n    return true;\n}")
    string(FIND "${c}" "${a_ser}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "sameboy_rb: retro_serialize anchor not found")
    endif()
    string(REPLACE "${a_ser}" "        offset += state_size;\n        size -= state_size;\n    }\n\n    if (size >= RB_EXTRA_SIZE) {   /* RetroHub: host-side state */\n        uint8_t *x = ((uint8_t *) data) + offset;\n        x[0] = bit_to_send1;\n        x[1] = bit_to_send2;\n        memcpy(x + 16, retained_frame_1, RB_PIX);\n        memcpy(x + 16 + RB_PIX, retained_frame_2, RB_PIX);\n        for (unsigned i = 0; i < emulated_devices; i++) memcpy(x + 16 + 2 * RB_PIX + i * RB_PIX, GB_get_pixels_output(&gameboy[i]), RB_PIX);\n    }\n\n    return true;\n}" c "${c}")
    set(a_uns "        size -= state_size;\n        data = ((uint8_t *)data) + state_size;\n    }\n\n    return true;")
    string(FIND "${c}" "${a_uns}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "sameboy_rb: retro_unserialize anchor not found")
    endif()
    string(REPLACE "${a_uns}" "        size -= state_size;\n        data = ((uint8_t *)data) + state_size;\n    }\n\n    if (size >= RB_EXTRA_SIZE) {   /* RetroHub: host-side state */\n        const uint8_t *x = (const uint8_t *)data;\n        bit_to_send1 = x[0] != 0;\n        bit_to_send2 = x[1] != 0;\n        memcpy(retained_frame_1, x + 16, RB_PIX);\n        memcpy(retained_frame_2, x + 16 + RB_PIX, RB_PIX);\n        for (unsigned i = 0; i < emulated_devices; i++) memcpy(GB_get_pixels_output(&gameboy[i]), x + 16 + 2 * RB_PIX + i * RB_PIX, RB_PIX);\n    }\n\n    return true;" c "${c}")
    # 4) GB_gameboy_t.joypad_is_stable is not in the savestate: a load that lands inside the 24-cycle JOYP select delay would keep the stale
    #    "stable" flag and GB_joypad_run would never finish the switch (rare lossy-network desync at a game-dependent frame).
    set(a_load "        if (GB_load_state_from_buffer(&gameboy[i], data, state_size)) {\n            return false;\n        }\n")
    string(FIND "${c}" "${a_load}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "sameboy_rb: load anchor not found")
    endif()
    string(REPLACE "${a_load}" "${a_load}        gameboy[i].joypad_is_stable = false;   /* RetroHub: unsaved flag */\n" c "${c}")
    file(WRITE ${gen}/libretro.c "${c}")
endfunction()
