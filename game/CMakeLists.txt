# Set a more convenient ARM flag
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    set(ARM64_ARCH TRUE)
    message(STATUS "ARM64 architecture detected")
else()
    set(ARM64_ARCH FALSE)
    message(STATUS "Non-ARM64 architecture detected")
endif()

if(ARM64_ARCH)
    # Add your ARM64-specific configuration or build options here
    set(OG_ASM_FUNCS_FILE kernel/asm_funcs_arm64.s)
    enable_language(ASM)
    set(CMAKE_ASM_SOURCE_FILE_EXTENSIONS ${CMAKE_ASM_SOURCE_FILE_EXTENSIONS} s)
    set_source_files_properties(${OG_ASM_FUNCS_FILE} PROPERTIES COMPILE_FLAGS "-arch arm64 -g")
else()
    set(OG_ASM_FUNCS_FILE kernel/asm_funcs_x86_64.asm)
    enable_language(ASM_NASM)
    set(CMAKE_ASM_NASM_SOURCE_FILE_EXTENSIONS ${CMAKE_ASM_NASM_SOURCE_FILE_EXTENSIONS} asm)
    set(CMAKE_ASM_NASM_COMPILE_OBJECT "<CMAKE_ASM_NASM_COMPILER> <INCLUDES> -f ${CMAKE_ASM_NASM_OBJECT_FORMAT} -o <OBJECT> <SOURCE>")
    set_source_files_properties(${OG_ASM_FUNCS_FILE} PROPERTIES COMPILE_FLAGS "-g")
endif()

set(RUNTIME_SOURCE
        external/discord_jak1.cpp
        external/discord_jak2.cpp
        external/discord_jak3.cpp
        external/discord_jakx.cpp
        external/discord.cpp
        graphics/display.cpp
        graphics/gfx_test.cpp
        graphics/gfx.cpp
        graphics/jak2_texture_remap.cpp
        graphics/jak3_texture_remap.cpp
        graphics/jakx_texture_remap.cpp
        graphics/screenshot.cpp
        graphics/opengl_renderer/background/background_common.cpp
        graphics/opengl_renderer/background/Hfrag.cpp
        graphics/opengl_renderer/background/Shrub.cpp
        graphics/opengl_renderer/background/TFragment.cpp
        graphics/opengl_renderer/background/Tie3.cpp
        graphics/opengl_renderer/BlitDisplays.cpp
        graphics/opengl_renderer/BucketRenderer.cpp
        graphics/opengl_renderer/CollideMeshRenderer.cpp
        graphics/opengl_renderer/debug_gui.cpp
        graphics/opengl_renderer/DepthCue.cpp
        graphics/opengl_renderer/DirectRenderer.cpp
        graphics/opengl_renderer/DirectRenderer2.cpp
        graphics/opengl_renderer/dma_helpers.cpp
        graphics/opengl_renderer/EyeRenderer.cpp
        graphics/opengl_renderer/foreground/Generic2_Build.cpp
        graphics/opengl_renderer/foreground/Generic2_DMA.cpp
        graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp
        graphics/opengl_renderer/foreground/Generic2.cpp
        graphics/opengl_renderer/foreground/Generic2BucketRenderer.cpp
        graphics/opengl_renderer/foreground/Merc2.cpp
        graphics/opengl_renderer/foreground/Merc2BucketRenderer.cpp
        graphics/opengl_renderer/foreground/Shadow2.cpp
        graphics/opengl_renderer/loader/Loader.cpp
        graphics/opengl_renderer/loader/LoaderStages.cpp
        graphics/opengl_renderer/ocean/CommonOceanRenderer.cpp
        graphics/opengl_renderer/ocean/OceanEnvmap.cpp
        graphics/opengl_renderer/ocean/OceanMid_PS2.cpp
        graphics/opengl_renderer/ocean/OceanMid.cpp
        graphics/opengl_renderer/ocean/OceanMidAndFar.cpp
        graphics/opengl_renderer/ocean/OceanNear_PS2.cpp
        graphics/opengl_renderer/ocean/OceanNear.cpp
        graphics/opengl_renderer/ocean/OceanTexture_PC.cpp
        graphics/opengl_renderer/ocean/OceanTexture.cpp
        graphics/opengl_renderer/opengl_utils.cpp
        graphics/opengl_renderer/OpenGLRenderer.cpp
        graphics/opengl_renderer/Profiler.cpp
        graphics/opengl_renderer/ProgressRenderer.cpp
        graphics/opengl_renderer/Shader.cpp
        graphics/opengl_renderer/Shadow_PS2.cpp
        graphics/opengl_renderer/ShadowRenderer.cpp
        graphics/opengl_renderer/SkyBlendCPU.cpp
        graphics/opengl_renderer/SkyBlendGPU.cpp
        graphics/opengl_renderer/SkyRenderer.cpp
        graphics/opengl_renderer/SlowTimeEffect.cpp
        graphics/opengl_renderer/sprite/GlowRenderer.cpp
        graphics/opengl_renderer/sprite/Sprite3_Distort.cpp
        graphics/opengl_renderer/sprite/Sprite3_Glow.cpp
        graphics/opengl_renderer/sprite/Sprite3.cpp
        graphics/opengl_renderer/TextureAnimator.cpp
        graphics/opengl_renderer/TextureAnimatorDefs.cpp
        graphics/opengl_renderer/TextureUploadHandler.cpp
        graphics/opengl_renderer/VisDataHandler.cpp
        graphics/opengl_renderer/Warp.cpp
        graphics/pipelines/opengl.cpp
        graphics/sceGraphicsInterface.cpp
        graphics/texture/jak1_tpage_dir.cpp
        graphics/texture/jak2_tpage_dir.cpp
        graphics/texture/jak3_tpage_dir.cpp
        graphics/texture/TextureConverter.cpp
        graphics/texture/TexturePool.cpp
        ${OG_ASM_FUNCS_FILE}
        kernel/common/fileio.cpp
        kernel/common/kboot.cpp
        kernel/common/kdgo.cpp
        kernel/common/kdsnetm.cpp
        kernel/common/klink.cpp
        kernel/common/klisten.cpp
        kernel/common/custom_audio.cpp
        kernel/common/kmachine.cpp
        kernel/common/kmalloc.cpp
        kernel/common/kmemcard.cpp
        kernel/common/kprint.cpp
        kernel/common/kscheme.cpp
        kernel/common/ksocket.cpp
        kernel/common/ksound.cpp
        kernel/jak1/fileio.cpp
        kernel/jak1/kboot.cpp
        kernel/jak1/kdgo.cpp
        kernel/jak1/klink.cpp
        kernel/jak1/klisten.cpp
        kernel/jak1/kmachine.cpp
        kernel/jak1/kprint.cpp
        kernel/jak1/kscheme.cpp
        kernel/jak1/ksound.cpp
        kernel/jak2/fileio.cpp
        kernel/jak2/kboot.cpp
        kernel/jak2/kdgo.cpp
        kernel/jak2/klink.cpp
        kernel/jak2/klisten.cpp
        kernel/jak2/kmachine.cpp
        kernel/jak2/kmachine_extras.cpp
        kernel/jak2/kmalloc.cpp
        kernel/jak2/kprint.cpp
        kernel/jak2/kscheme.cpp
        kernel/jak2/ksound.cpp
        kernel/jak3/fileio.cpp
        kernel/jak3/kboot.cpp
        kernel/jak3/kdgo.cpp
        kernel/jak3/kdsnetm.cpp
        kernel/jak3/klink.cpp
        kernel/jak3/klisten.cpp
        kernel/jak3/kmachine.cpp
        kernel/jak3/kmachine_extras.cpp
        kernel/jak3/kmalloc.cpp
        kernel/jak3/kmemcard.cpp
        kernel/jak3/kprint.cpp
        kernel/jak3/kscheme.cpp
        kernel/jak3/ksocket.cpp
        kernel/jak3/ksound.cpp
        kernel/jakx/fileio.cpp
        kernel/jakx/kboot.cpp
        kernel/jakx/kdgo.cpp
        kernel/jakx/kdsnetm.cpp
        kernel/jakx/klink.cpp
        kernel/jakx/klisten.cpp
        kernel/jakx/kmachine.cpp
        kernel/jakx/kmachine_extras.cpp
        kernel/jakx/kmalloc.cpp
        kernel/jakx/kmemcard.cpp
        kernel/jakx/kprint.cpp
        kernel/jakx/kscheme.cpp
        kernel/jakx/ksocket.cpp
        kernel/jakx/ksound.cpp
        mips2c/jak1_functions/bones.cpp
        mips2c/jak1_functions/collide_cache.cpp
        mips2c/jak1_functions/collide_edge_grab.cpp
        mips2c/jak1_functions/collide_func.cpp
        mips2c/jak1_functions/collide_mesh.cpp
        mips2c/jak1_functions/collide_probe.cpp
        mips2c/jak1_functions/draw_string.cpp
        mips2c/jak1_functions/generic_effect.cpp
        mips2c/jak1_functions/generic_effect2.cpp
        mips2c/jak1_functions/generic_merc.cpp
        mips2c/jak1_functions/generic_tie.cpp
        mips2c/jak1_functions/joint.cpp
        mips2c/jak1_functions/merc_blend_shape.cpp
        mips2c/jak1_functions/ocean_vu0.cpp
        mips2c/jak1_functions/ocean.cpp
        mips2c/jak1_functions/ripple.cpp
        mips2c/jak1_functions/shadow.cpp
        mips2c/jak1_functions/sky_tng.cpp
        mips2c/jak1_functions/sparticle_launcher.cpp
        mips2c/jak1_functions/sparticle.cpp
        mips2c/jak1_functions/test_func.cpp
        mips2c/jak1_functions/texture.cpp
        mips2c/jak1_functions/tfrag.cpp
        mips2c/jak1_functions/tie_methods.cpp
        mips2c/jak1_functions/time_of_day.cpp
        mips2c/jak2_functions/bones.cpp
        mips2c/jak2_functions/collide_cache.cpp
        mips2c/jak2_functions/collide_edge_grab.cpp
        mips2c/jak2_functions/collide_func.cpp
        mips2c/jak2_functions/collide_hash.cpp
        mips2c/jak2_functions/collide_mesh.cpp
        mips2c/jak2_functions/debug.cpp
        mips2c/jak2_functions/font.cpp
        mips2c/jak2_functions/foreground.cpp
        mips2c/jak2_functions/generic_effect.cpp
        mips2c/jak2_functions/generic_merc.cpp
        mips2c/jak2_functions/joint.cpp
        mips2c/jak2_functions/lights.cpp
        mips2c/jak2_functions/merc_blend_shape.cpp
        mips2c/jak2_functions/nav_control.cpp
        mips2c/jak2_functions/nav_mesh.cpp
        mips2c/jak2_functions/ocean_vu0.cpp
        mips2c/jak2_functions/ocean.cpp
        mips2c/jak2_functions/ripple.cpp
        mips2c/jak2_functions/shadow.cpp
        mips2c/jak2_functions/sky.cpp
        mips2c/jak2_functions/sparticle_launcher.cpp
        mips2c/jak2_functions/sparticle.cpp
        mips2c/jak2_functions/spatial_hash.cpp
        mips2c/jak2_functions/squid.cpp
        mips2c/jak2_functions/texture.cpp
        mips2c/jak2_functions/vortex.cpp
        mips2c/jak3_functions/collide_func.cpp
        mips2c/jak3_functions/debug.cpp
        mips2c/jak3_functions/font.cpp
        mips2c/jak3_functions/foreground.cpp
        mips2c/jak3_functions/generic_effect.cpp
        mips2c/jak3_functions/joint.cpp
        mips2c/jak3_functions/lights.cpp
        mips2c/jak3_functions/sky.cpp
        mips2c/jak3_functions/texture.cpp
        mips2c/jak3_functions/particle_curves.cpp
        mips2c/jak3_functions/cloth.cpp
        mips2c/jak3_functions/collide_cache.cpp
        mips2c/jak3_functions/collide_hash.cpp
        mips2c/jak3_functions/collide_edge_grab.cpp
        mips2c/jak3_functions/collide_mesh.cpp
        mips2c/jak3_functions/spatial_hash.cpp
        mips2c/jak3_functions/sparticle.cpp
        mips2c/jak3_functions/sparticle_launcher.cpp
        mips2c/jak3_functions/nav_control.cpp
        mips2c/jak3_functions/nav_engine.cpp
        mips2c/jak3_functions/merc_blend_shape.cpp
        mips2c/jak3_functions/wvehicle_part.cpp
        mips2c/jak3_functions/ripple.cpp
        mips2c/jak3_functions/shadow.cpp
        mips2c/jak3_functions/ocean.cpp
        mips2c/jak3_functions/ocean_vu0.cpp
        mips2c/jak3_functions/generic_merc.cpp
        mips2c/mips2c_table.cpp
        overlord/common/dma.cpp
        overlord/common/fake_iso.cpp
        overlord/common/iso_api.cpp
        overlord/common/iso.cpp
        overlord/common/isocommon.cpp
        overlord/common/989_plugins/plugin_strv.cpp
        overlord/common/overlord.cpp
        overlord/common/sbank.cpp
        overlord/common/soundcommon.cpp
        overlord/common/srpc.cpp
        overlord/common/ssound.cpp
        overlord/jak1/dma.cpp
        overlord/jak1/fake_iso.cpp
        overlord/jak1/iso_api.cpp
        overlord/jak1/iso_queue.cpp
        overlord/jak1/iso.cpp
        overlord/jak1/isocommon.cpp
        overlord/jak1/overlord.cpp
        overlord/jak1/ramdisk.cpp
        overlord/jak1/srpc.cpp
        overlord/jak1/ssound.cpp
        overlord/jak1/stream.cpp
        overlord/jak2/dma.cpp
        overlord/jak2/iso_api.cpp
        overlord/jak2/iso_cd.cpp
        overlord/jak2/iso_queue.cpp
        overlord/jak2/iso.cpp
        overlord/jak2/list.cpp
        overlord/jak2/overlord.cpp
        overlord/jak2/pages.cpp
        overlord/jak2/989_plugins/plugin_strv.cpp
        overlord/jak2/custom_audio_stream.cpp
        overlord/jak2/spustreams.cpp
        overlord/jak2/srpc.cpp
        overlord/jak2/ssound.cpp
        overlord/jak2/stream.cpp
        overlord/jak2/streamlfo.cpp
        overlord/jak2/streamlist.cpp
        overlord/jak2/vag.cpp
        overlord/jak3/overlord.cpp
        overlord/jak3/pagemanager.cpp
        overlord/jak3/iso_cd.cpp
        overlord/jak3/dma.cpp
        overlord/jak3/iso.cpp
        overlord/jak3/iso_queue.cpp
        overlord/jak3/srpc.cpp
        overlord/jak3/vag.cpp
        overlord/jak3/ssound.cpp
        overlord/jak3/iso_api.cpp
        overlord/jak3/spustreams.cpp
        overlord/jak3/list.cpp
        overlord/jak3/vblank_handler.cpp
        overlord/jak3/dvd_driver.cpp
        overlord/jak3/basefile.cpp
        overlord/jak3/basefilesystem.cpp
        overlord/jak3/ramdisk.cpp
        overlord/jak3/isocommon.cpp
        overlord/jak3/init.cpp
        overlord/jak3/stream.cpp
        overlord/jak3/sbank.cpp
        overlord/jak3/soundcommon.cpp
        overlord/jak3/streamlist.cpp
        overlord/jak3/989_plugins/plugin_strv.cpp
        overlord/jakx/overlord.cpp
        overlord/jakx/pagemanager.cpp
        overlord/jakx/iso_cd.cpp
        overlord/jakx/dma.cpp
        overlord/jakx/iso.cpp
        overlord/jakx/iso_queue.cpp
        overlord/jakx/srpc.cpp
        overlord/jakx/vag.cpp
        overlord/jakx/ssound.cpp
        overlord/jakx/iso_api.cpp
        overlord/jakx/spustreams.cpp
        overlord/jakx/list.cpp
        overlord/jakx/vblank_handler.cpp
        overlord/jakx/dvd_driver.cpp
        overlord/jakx/basefile.cpp
        overlord/jakx/basefilesystem.cpp
        overlord/jakx/ramdisk.cpp
        overlord/jakx/isocommon.cpp
        overlord/jakx/init.cpp
        overlord/jakx/stream.cpp
        overlord/jakx/sbank.cpp
        overlord/jakx/soundcommon.cpp
        overlord/jakx/streamlist.cpp
        runtime.cpp
        sce/deci2.cpp
        sce/iop.cpp
        sce/libcdvd_ee.cpp
        sce/libdma.cpp
        sce/libgraph.cpp
        sce/libpad.cpp
        sce/libscf.cpp
        sce/sif_ee_memcard.cpp
        sce/sif_ee.cpp
        sce/stubs.cpp
        settings/settings.cpp
        system/background_worker.cpp
        system/Deci2Server.cpp
        system/hid/devices/dualsense_effects.cpp
        system/hid/devices/game_controller.cpp
        system/hid/devices/keyboard.cpp
        system/hid/devices/mouse.cpp
        system/hid/display_manager.cpp
        system/hid/input_bindings.cpp
        system/hid/input_manager.cpp
        system/hid/text_input_editor.cpp
        system/hid/sdl_util.cpp
        system/IOP_Kernel.cpp
        system/iop_thread.cpp
        system/SystemThread.cpp
        tools/filter_menu/filter_menu.cpp
        tools/network_debug/network_debug_panel.cpp
        tools/subtitle_editor/subtitle_editor_db.cpp
        tools/subtitle_editor/subtitle_editor_repl_client.cpp
        tools/subtitle_editor/subtitle_editor.cpp
        )

set(MULTIPLAYER_JAK2_SOURCE
        multiplayer/jak2/bridge/goal_bridge.cpp
        multiplayer/jak2/core/replication_state.cpp
        multiplayer/jak2/core/replication_state_traffic.cpp
        multiplayer/jak2/core/replication_state_entities.cpp
        multiplayer/jak2/core/validation.cpp
        multiplayer/jak2/application/presentation_runtime.cpp
        multiplayer/jak2/application/jak2_adapter.cpp
        multiplayer/jak2/api/multiplayer_api.cpp
        multiplayer/jak2/api/preferences.cpp
        multiplayer/jak2/wire/packets/traffic_authority_state_packet.cpp
        multiplayer/jak2/wire/packets/player_state_packet.cpp
        multiplayer/jak2/wire/packets/player_vehicle_state_packet.cpp
        multiplayer/jak2/wire/packets/turret_state_packet.cpp
        multiplayer/jak2/wire/packets/game_event_batch_packet.cpp
        multiplayer/jak2/wire/packets/world_state_packet.cpp
        multiplayer/jak2/wire/packets/bootstrap_state_packet.cpp
        multiplayer/jak2/wire/packets/airlock_state_batch_packet.cpp
        multiplayer/jak2/wire/packets/enemy_state_batch_packet.cpp
        multiplayer/jak2/wire/packets/pedestrian_state_batch_packet.cpp
        multiplayer/jak2/wire/packets/vehicle_state_batch_packet.cpp
        multiplayer/jak2/wire/packets/boss_state_packet.cpp
        multiplayer/jak2/application/packet_handlers.cpp)

set(MULTIPLAYER_PLATFORM_SOURCE
        multiplayer/platform/session/connection_draft.cpp
        multiplayer/platform/session/connection_input.cpp
        multiplayer/platform/core/compatibility_identity.cpp
        multiplayer/platform/core/message_policy.cpp
        multiplayer/platform/core/sequence.cpp
        multiplayer/platform/profile/profile_store.cpp
        multiplayer/platform/protocol/session_protocol.cpp
        multiplayer/platform/discovery/discovery_protocol.cpp
        multiplayer/platform/discovery/invite.cpp
        multiplayer/platform/discovery/discovery_service.cpp
        multiplayer/platform/session/player_registry.cpp
        multiplayer/platform/session/packet_registry.cpp
        multiplayer/platform/session/cadence_scheduler.cpp
        multiplayer/platform/session/reconnect_policy.cpp
        multiplayer/platform/protocol/message_frame.cpp
        multiplayer/platform/runtime/multiplayer_runtime.cpp
        multiplayer/platform/runtime/multiplayer_runtime_commands.cpp
        multiplayer/platform/runtime/multiplayer_runtime_worker.cpp
        multiplayer/platform/session/session_controller.cpp
        multiplayer/platform/session/session_state.cpp
        multiplayer/platform/transport/session_platform.cpp
        multiplayer/platform/nat/port_mapping.cpp
        multiplayer/platform/nat/port_mapping_coordinator.cpp)

if(ENABLE_NETWORK_SIMULATION)
    list(APPEND RUNTIME_SOURCE
            tools/network_simulation/network_simulation_panel.cpp)
    list(APPEND MULTIPLAYER_PLATFORM_SOURCE
            multiplayer/platform/transport/network_simulation.cpp)
endif()

add_subdirectory(sound)

add_library(multiplayer-platform STATIC ${MULTIPLAYER_PLATFORM_SOURCE})
if(ENABLE_NETWORK_SIMULATION)
    target_compile_definitions(multiplayer-platform PUBLIC ENABLE_NETWORK_SIMULATION)
endif()
target_link_libraries(multiplayer-platform PRIVATE GameNetworkingSockets::static common
        "$<TARGET_FILE:libminiupnpc-static>" "$<TARGET_FILE:natpmp>")
add_dependencies(multiplayer-platform libminiupnpc-static natpmp)
target_include_directories(multiplayer-platform PUBLIC "${PROJECT_SOURCE_DIR}")
if(WIN32)
    target_link_libraries(multiplayer-platform PRIVATE iphlpapi ws2_32)
elseif(UNIX)
    target_link_libraries(multiplayer-platform PRIVATE pthread)
endif()

add_library(multiplayer-jak2 STATIC ${MULTIPLAYER_JAK2_SOURCE})
target_include_directories(multiplayer-jak2 PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(multiplayer-jak2 PUBLIC multiplayer-platform PRIVATE common fmt)

# we build the runtime as a static library.
add_library(runtime STATIC ${RUNTIME_SOURCE} "../third-party/glad/src/glad.c")

option(MULTIPLAYER_COVERAGE "Instrument multiplayer production sources with LLVM coverage" OFF)
if(MULTIPLAYER_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MULTIPLAYER_COVERAGE requires Clang and matching llvm-cov/llvm-profdata")
    endif()
    foreach(source IN LISTS RUNTIME_SOURCE)
        if(source MATCHES "^multiplayer/.*\\.cpp$")
            set_property(SOURCE ${source} APPEND PROPERTY COMPILE_OPTIONS
                    -fprofile-instr-generate -fcoverage-mapping -O0)
        endif()
    endforeach()
    target_link_options(runtime INTERFACE -fprofile-instr-generate)
endif()

set_property(SOURCE multiplayer/platform/nat/port_mapping.cpp APPEND PROPERTY
        INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/third-party/miniupnpc/include"
        "${PROJECT_SOURCE_DIR}/third-party/libnatpmp")
set_property(SOURCE multiplayer/platform/nat/port_mapping.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS
        MINIUPNP_STATICLIB
        NATPMP_STATICLIB
        ENABLE_STRNATPMPERR
        NATPMP_MAX_RETRIES=3)

target_link_libraries(runtime common fmt multiplayer-platform multiplayer-jak2 SDL3::SDL3 imgui discord-rpc sound stb_image libco
        libcurl "$<TARGET_FILE:libminiupnpc-static>" "$<TARGET_FILE:natpmp>")
add_dependencies(runtime libminiupnpc-static natpmp)
if(WIN32)
    target_link_libraries(runtime mman ole32 iphlpapi ws2_32)
else()
    target_link_libraries(runtime pthread dl)
endif()

add_executable(gk main.cpp)
target_link_libraries(gk runtime)
if(WIN32)
    set_target_properties(gk PROPERTIES VS_DPI_AWARE "PerMonitor")
endif()
