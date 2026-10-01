include(${CMAKE_CURRENT_LIST_DIR}/goalc/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/offline/CMakeLists.txt)

add_executable(goalc-test
        ${CMAKE_CURRENT_LIST_DIR}/emitter_util.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_main.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_reader.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_goos.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_listener_deci2.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_kernel_jak1.cpp
        ${CMAKE_CURRENT_LIST_DIR}/all_jak1_symbols.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_type_system.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_emitter_x86.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_emitter_avx.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_emitter_neon.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_emitter_arm64.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_common_util.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_text_input_editor.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_pretty_print.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_math.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_zstd.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_zydis.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/FormRegressionTest.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_AtomicOpBuilder.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_FormBeforeExpressions.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_FormExpressionBuild.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_FormExpressionBuild2.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_FormExpressionBuild3.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_FormExpressionBuildLong.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_InstructionDecode.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_InstructionParser.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_gkernel_jak1_decomp.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_math_decomp.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_DataParser.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_DisasmVifDecompile.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_VuDisasm.cpp
        ${CMAKE_CURRENT_LIST_DIR}/common/formatter/test_formatter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/handlers/lsp_router.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/handlers/initialize.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/handlers/text_document/completion.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/handlers/text_document/document_color.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/handlers/text_document/document_symbol.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/handlers/text_document/document_synchronization.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/handlers/text_document/formatting.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/handlers/text_document/go_to.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/handlers/text_document/hover.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/handlers/text_document/references.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/handlers/text_document/signature_help.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/handlers/text_document/type_hierarchy.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/protocol/completion.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/protocol/document_color.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/protocol/document_symbols.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/protocol/document_synchronization.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/protocol/formatting.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/protocol/hover.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/protocol/hover_formatting.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/protocol/progress_report.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/protocol/signature_help.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/protocol/type_hierarchy.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/state/data/mips_instruction.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/state/lsp_requester.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/state/workspace.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/transport/stdio.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/lsp_util.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/protocol/common_types.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/protocol/document_diagnostics.cpp
        ${CMAKE_CURRENT_LIST_DIR}/lsp/test_lsp_progress.cpp
        ${CMAKE_CURRENT_LIST_DIR}/lsp/test_lsp_completion.cpp
        ${CMAKE_CURRENT_LIST_DIR}/lsp/test_lsp_hover_formatting.cpp
        ${CMAKE_CURRENT_LIST_DIR}/lsp/test_lsp_hover_definition.cpp
        ${CMAKE_CURRENT_LIST_DIR}/lsp/test_lsp_references.cpp
        ${CMAKE_CURRENT_LIST_DIR}/lsp/test_lsp_signature_help.cpp
        ${GOALC_TEST_FRAMEWORK_SOURCES}
        ${GOALC_TEST_CASES}
        )

target_link_libraries(goalc-test common runtime compiler gtest decomp Zydis libzstd_static tree-sitter)

add_executable(multiplayer-platform-unit-test
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/platform/unit/test_connection_draft.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/platform/unit/test_snapshot_timeline.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/platform/unit/test_platform_core.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/platform/unit/test_platform_protocol.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/platform/unit/test_packet_handlers.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/platform/unit/test_frame_submission.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/platform/unit/test_runtime_commands.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/platform/unit/test_serialize_codec.cpp)
target_link_libraries(multiplayer-platform-unit-test multiplayer-platform gtest_main)

add_executable(multiplayer-platform-integration-test
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/platform/integration/test_gns_transport.cpp)
target_link_libraries(multiplayer-platform-integration-test multiplayer-platform
        GameNetworkingSockets::static gtest_main)

add_executable(multiplayer-jak2-unit-test
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/jak2/unit/test_core.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/jak2/unit/test_core_state.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/jak2/unit/test_vehicle_state.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/jak2/unit/test_traffic_state.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/jak2/unit/test_enemy_state.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/jak2/unit/test_player_codec.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/jak2/unit/test_wire_codecs.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/jak2/unit/test_wire_packets.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/jak2/unit/test_protocol.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/jak2/unit/test_traffic_codec.cpp)
target_link_libraries(multiplayer-jak2-unit-test multiplayer-jak2 gtest_main)

add_executable(multiplayer-jak2-integration-test
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/jak2/integration/runtime_stubs.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/jak2/integration/test_adapter_contract.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/jak2/integration/test_goal_bridge.cpp
        ${CMAKE_CURRENT_LIST_DIR}/multiplayer/jak2/integration/test_replication_dispatch.cpp)
target_link_libraries(multiplayer-jak2-integration-test multiplayer-jak2 gtest_main SDL3::SDL3)
target_compile_definitions(multiplayer-jak2-integration-test PRIVATE
        MP_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")

include(GoogleTest)
gtest_discover_tests(multiplayer-platform-unit-test TEST_PREFIX "platform.unit."
        PROPERTIES LABELS "platform;unit")
gtest_discover_tests(multiplayer-platform-integration-test TEST_PREFIX "platform.integration."
        PROPERTIES LABELS "platform;integration")
gtest_discover_tests(multiplayer-jak2-unit-test TEST_PREFIX "jak2.unit."
        PROPERTIES LABELS "jak2;unit")
gtest_discover_tests(multiplayer-jak2-integration-test TEST_PREFIX "jak2.integration."
        PROPERTIES LABELS "jak2;integration")

add_executable(test_image_resize ${CMAKE_CURRENT_LIST_DIR}/common/test_image_resize.cpp)
target_link_libraries(test_image_resize common)

if(WIN32)
  target_link_libraries(goalc-test mman)
endif()

#gtest_discover_tests(goalc-test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

if(UNIX AND CMAKE_COMPILER_IS_GNUCXX AND CODE_COVERAGE)
  set(BUILD_ARGS "-w" "dupbuild=warn")
  include(CodeCoverage)
  append_coverage_compiler_flags()
  setup_target_for_coverage_lcov(NAME goalc-test_coverage
                                 EXECUTABLE goalc-test --gtest_color=yes
                                 DEPENDENCIES goalc-test
                                 EXCLUDE "third-party/*" "/usr/include/*")
endif()
