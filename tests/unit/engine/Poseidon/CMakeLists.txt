set(OFPR_GAME_DATA_DIR "${CMAKE_SOURCE_DIR}/packages/Remaster")
set(OFPR_CATCH_TEST_SPEC "")
if(NOT EXISTS "${OFPR_GAME_DATA_DIR}")
    set(OFPR_CATCH_TEST_SPEC "~[GameData] ~[gamescan] ~[external-data]")
    message(STATUS "Remaster game data not found at ${OFPR_GAME_DATA_DIR}; excluding external-data Catch2 tests")
endif()
set(OFPR_CATCH_TEST_SPEC_ARGS)
if(NOT OFPR_CATCH_TEST_SPEC STREQUAL "")
    list(APPEND OFPR_CATCH_TEST_SPEC_ARGS TEST_SPEC ${OFPR_CATCH_TEST_SPEC})
endif()

set(POSEIDON_CORE_TEST_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Graphics/test_retail_page_instance_observation.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_compressed_model_source_birth.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_retail_rigid_asset_profile.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Graphics/test_geometry_page_rigid_odol7_cold_world_candidate.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Graphics/test_retail_world_instance_transaction.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Graphics/test_retail_world_stable_frame_observation.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Graphics/test_geometry_page_rigid_final_surface_certificate.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Support/test_stubs.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Asset/Addon/test_config_replace.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Asset/Formats/Common/test_format_detector.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/UI/Locale/Stringtable/test_stringtable.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/UI/Locale/Stringtable/test_codepage_transcode.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/UI/Locale/Stringtable/test_stringtable_encoding.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/UI/Locale/Stringtable/test_stringtable_switch.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/UI/Locale/Stringtable/test_localized_date.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/UI/Locale/test_mission_template_catalog.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Foundation/Time/test_clock_timeofday.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/PreprocC/test_preprocC.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_bank_mount_prefix.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_findbank_x64.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_qstream.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_qstream_banks.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_qstream_buffers.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_qstream_compression.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_compressed_model_member_lease.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_retail_pac_source_binding.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_retail_pac_pixel_stage.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_serializebin.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_qstream_file_input.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_qstream_file_output.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_qstream_path_resolution.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_qstream_pbo_game_scan.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_qstream_pbo_roundtrip.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams/test_qstream_pbo_content_hash.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Asset/Cache/test_ast004_producer_cost.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Asset/Cache/test_ast004_model_ddc.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/ParamFile/test_paramfile.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/ParamFile/test_paramfile_access.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/ParamFile/test_paramfile_api.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/ParamFile/test_paramfile_binary.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/ParamFile/test_paramfile_crc.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/ParamFile/test_paramfile_expressions.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/ParamFile/test_paramfile_inheritance.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/ParamFile/test_paramfile_localization.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/ParamFile/test_localized_string.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/ParamFile/test_paramfile_parsing.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/ParamFile/test_paramfile_preprocessing.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/ParamFile/test_paramfile_realworld.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Network/test_admin_login_auth.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Network/test_decode_robustness.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Network/test_network_predicate_parity.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Network/test_master_server_browser.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Network/test_net_transport_termination.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Network/test_net_server_teardown.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Evaluator/test_evaluator_complex.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Evaluator/test_evaluator_data.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Evaluator/test_evaluator_edge.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Evaluator/test_evaluator_functions.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Evaluator/test_evaluator_integration.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Evaluator/test_evaluator_nulars.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Evaluator/test_evaluator_operators.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Evaluator/test_evaluator_reentrancy.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Evaluator/test_evaluator_script_integration.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Evaluator/test_evaluator_simple.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Evaluator/test_evaluator_state_vars.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Evaluator/test_evaluator_syntax.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Evaluator/test_evaluator_value.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Evaluator/test_evaluator_varspace.cpp
)

set(POSEIDON_CORE_TEST_HEADERS
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Support/test_stubs.hpp
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Support/test_fixtures.hpp
)

add_executable(PoseidonCoreTests ${POSEIDON_CORE_TEST_SOURCES} ${POSEIDON_CORE_TEST_HEADERS})

target_link_libraries(PoseidonCoreTests PRIVATE
    Poseidon
    OpenPoseidon::MeshoptimizerClod
    Catch2::Catch2WithMain
    $<$<BOOL:${WIN32}>:ws2_32>
)

target_compile_definitions(PoseidonCoreTests PRIVATE
    GAME_DATA_DIR="${OFPR_GAME_DATA_DIR}"
)

target_include_directories(PoseidonCoreTests PRIVATE
    ${CMAKE_SOURCE_DIR}/engine
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Support
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Evaluator
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Asset/Formats/Common
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/Locale
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/Streams
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/ParamFile
    ${CMAKE_SOURCE_DIR}/tests/unit/engine/Poseidon/IO/PreprocC
)

source_group(TREE ${CMAKE_SOURCE_DIR}/tests/unit/engine FILES ${POSEIDON_CORE_TEST_SOURCES} ${POSEIDON_CORE_TEST_HEADERS})

set_target_properties(PoseidonCoreTests PROPERTIES
    OUTPUT_NAME "PoseidonCoreTests"
)

set(FIXTURES_ROOT ${CMAKE_SOURCE_DIR}/tests/fixtures)
add_custom_command(TARGET PoseidonCoreTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:PoseidonCoreTests>/fixtures/pbo
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:PoseidonCoreTests>/fixtures/qstream
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:PoseidonCoreTests>/fixtures/config-replace
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/config-replace $<TARGET_FILE_DIR:PoseidonCoreTests>/fixtures/config-replace
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/config      $<TARGET_FILE_DIR:PoseidonCoreTests>/fixtures
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/xml         $<TARGET_FILE_DIR:PoseidonCoreTests>/fixtures
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/stringtable $<TARGET_FILE_DIR:PoseidonCoreTests>/fixtures
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/pbo         $<TARGET_FILE_DIR:PoseidonCoreTests>/fixtures/pbo
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/qstream     $<TARGET_FILE_DIR:PoseidonCoreTests>/fixtures/qstream
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/evaluator   $<TARGET_FILE_DIR:PoseidonCoreTests>/fixtures
    COMMENT "Copying PoseidonCore test fixtures"
)

include(${CMAKE_SOURCE_DIR}/cmake/CatchWindowsSafe.cmake)
ofpr_catch_discover_tests(PoseidonCoreTests
    ${OFPR_CATCH_TEST_SPEC_ARGS}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    PROPERTIES
        ENVIRONMENT "UBSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/ubsan-suppressions.txt;LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/lsan-suppressions.txt"
)

set(POSEIDON_TEST_SOURCES
    # Test infrastructure
    test_main.cpp
    test_stubs.cpp
    
    # Memory tests
    Foundation/Memory/test_jimbo_allocator.cpp
    
    # Graphics tests
    Graphics/test_geometry_owner_ledger.cpp
    Graphics/test_geometry_source_revisions.cpp
    Graphics/test_parked_geometry_report.cpp
    Graphics/test_shared_retained_mesh_lifetime.cpp
    Graphics/test_standalone_mesh_retirement_witness.cpp
    Graphics/test_geometry_page_prototype.cpp
    Graphics/test_geometry_page_shape_export.cpp
    Graphics/test_geometry_page_controlled_mlod_asset.cpp
    Graphics/test_geometry_page_original_admission.cpp
    Graphics/test_geometry_page_camera_demand.cpp
    Graphics/test_geometry_page_projected_demand.cpp
    Tools/test_geometry_page_mlod_producer.cpp
    Tools/test_selected_cut_surface_certificate.cpp
    Tools/test_geometry_page_surface_admission.cpp
    Graphics/Rendering/test_geometry_page_projected_surface_bound.cpp
    World/Model/test_warm_material_stage_preflight.cpp
    Graphics/test_geometry_page_resident_decode.cpp
    Graphics/test_geometry_page_representation_decode.cpp
    Graphics/test_authored_geometry_page_worker.cpp
    Graphics/test_geometry_page_eviction_policy.cpp
    Graphics/test_geometry_page_clod_bake.cpp
    Graphics/test_geometry_page_clod_cut_decode.cpp
    Graphics/test_geometry_page_clod_ram_package.cpp
    Graphics/test_geometry_page_hierarchical.cpp
    Graphics/test_geometry_page_hierarchical_local_cut.cpp
    Graphics/test_geometry_page_hierarchical_projected_demand.cpp
    Graphics/test_geometry_page_hierarchical_combined_projection.cpp
    Graphics/test_geometry_page_hierarchical_original_outside.cpp
    Graphics/test_geometry_page_hierarchical_original_demand.cpp
    Graphics/test_geometry_page_hierarchical_binary_demand.cpp
    Graphics/test_geometry_page_hierarchical_release.cpp
    Graphics/test_geometry_page_hierarchical_disk.cpp
    Graphics/test_hierarchical_geometry_page_worker.cpp
    Graphics/test_geometry_page_clod_disk_codec.cpp
    Graphics/test_registration_cost_policy.cpp
    Graphics/test_resumable_model_admission.cpp
    Graphics/test_retained_material_role_declarations.cpp
    Graphics/test_deferred_procedural_admission.cpp
    Graphics/test_prepared_gpu_section_classification.cpp
    Graphics/test_owned_material_dependency_packet.cpp
    Graphics/test_geometry_page_rigid_odol7_candidate.cpp
    Graphics/test_texture_birth_before_use.cpp
    Graphics/test_texture_reuse_only_binding.cpp
    Graphics/test_geometry_page_rigid_odol7_final_shape.cpp
    Graphics/test_model_admission_receipt.cpp
    Graphics/test_geometry_page_rigid_final_hierarchy_product.cpp
    Graphics/test_retail_page_record_observation.cpp
    Graphics/test_alpha_classify.cpp
    Graphics/test_surface_decompose.cpp
    Graphics/test_shadowmap_math.cpp
    Graphics/test_ephemeris.cpp
    Graphics/test_paa_decode_fixtures.cpp
    Graphics/test_paa_decoder.cpp
    Graphics/test_texture_mip_chain.cpp
    Graphics/test_texture_alpha_block_scan.cpp
    Graphics/test_procedural_texture.cpp
    Graphics/test_shape_loader.cpp
    Graphics/test_pixel_format.cpp
    Graphics/test_image.cpp
    Graphics/test_dxt_compressor.cpp
    Graphics/test_paa_encoder.cpp
    Graphics/test_dds_converter.cpp
    Graphics/test_model_renderer.cpp
    Graphics/test_gl33_rendering.cpp
    Graphics/test_fan_decompose.cpp
    Graphics/test_grass_wind.cpp
    Graphics/test_zbias_math.cpp
    Graphics/test_mipmap_layout.cpp
    Graphics/test_texture_dummy_size.cpp
    Graphics/test_gl_state_cache_audit.cpp
    Graphics/test_ibo_bind_audit.cpp
    Graphics/test_buffer_usage_audit.cpp
    Graphics/test_graphics_engine_factory.cpp
    Graphics/test_shutdown_order_audit.cpp
    Graphics/test_gl_state_ownership_audit.cpp
    Graphics/test_debug_group_audit.cpp
    Graphics/test_spec_bit_backend_boundary.cpp
    Graphics/test_whole_shape_pass_routing.cpp
    Graphics/test_render_conventions_audit.cpp
    Graphics/test_vertex_color_byte_order.cpp
    Graphics/test_ubo_restore_guards.cpp
    Graphics/test_shader_named_outputs.cpp
    Graphics/test_jpg_import.cpp
    Graphics/test_custom_face_texture_cache.cpp
    Graphics/test_window_metrics.cpp
    Graphics/test_window_placement.cpp
)

if(WIN32)
    list(APPEND POSEIDON_TEST_SOURCES
        TcPbo/test_tc_pbo_api.cpp
    )
endif()

list(APPEND POSEIDON_TEST_SOURCES
    # Core tests
    UI/Locale/test_supported_languages.cpp
    Core/test_deterministic_commit.cpp
    Core/test_state_timeline_file.cpp
    Core/test_tick_state_hash.cpp
    Core/test_engine_config.cpp
    Core/test_application_exit.cpp
    Core/test_window_close.cpp
    Core/test_user_config.cpp
    Core/test_user_config_serialization.cpp
    Core/test_splash_gating.cpp
    Core/test_profile_manager.cpp
    Core/test_profile_service.cpp
    Core/test_fsm.cpp
    Core/test_globalAlive.cpp
    Core/test_legacy_logging.cpp
    Core/test_logging.cpp
    Core/test_version.cpp
    Asset/Cache/test_asset_cache.cpp
    Asset/Cache/test_handle.cpp
    Core/test_perf_trace.cpp
    Core/test_scoped_timer.cpp
    Core/test_task_pool.cpp
    Core/test_worker_determinism.cpp
    Foundation/Math/test_geometry_math.cpp
    
    # BIS Binary Framework tests (split by functionality)
    Asset/Formats/BISFramework/test_primitives.cpp
    Asset/Formats/BISFramework/test_strings.cpp
    Asset/Formats/BISFramework/test_structures.cpp
    Asset/Formats/BISFramework/test_arrays_uncompressed.cpp
    Asset/Formats/BISFramework/test_arrays_compressed.cpp
    Asset/Formats/BISFramework/test_errors.cpp
    Asset/Formats/BISFramework/test_stream.cpp
    Asset/Formats/BISFramework/test_p3d_integration.cpp
    
    # Cross-generation compatibility smoke (REN-GL33-002). Reads the local licensed
    # corpora where they exist and reports itself skipped where they do not.
    Asset/Compat/test_compat_smoke.cpp
    Asset/Compat/test_reforger_native.cpp
    # Formats tests (isolated format readers)
    Asset/Formats/Common/test_format_detector.cpp
    Asset/Formats/test_wrp.cpp
    Asset/Formats/World/test_oprw20.cpp
    Asset/Formats/World/test_oprw25.cpp
    Asset/Formats/RTM/test_rtm_reader.cpp
    Asset/Formats/test_rtm.cpp
    Asset/Formats/test_paa.cpp
    Asset/Formats/test_pac.cpp
    
    # World/Model tests (new architecture)
    World/Weapons/test_ballistics.cpp
    World/test_cargo_weapon_seats.cpp
    World/test_collision_query_math.cpp
    World/test_loose_objects.cpp
    World/Weapons/test_friendly_fire.cpp
    World/Weapons/test_grenade_fuse.cpp
    World/Weapons/test_penetration.cpp
    World/test_physics_world.cpp
    World/test_box3d_mass_update.cpp
    World/test_collider_flags.cpp
    World/test_physics_determinism.cpp
    World/test_first_divergent_boundary.cpp
    World/test_serial_task_graph.cpp
    World/test_stable_extract.cpp
    World/test_visibility_objects.cpp
    World/test_sim_stage_evidence.cpp
    Foundation/Threads/test_watchdog.cpp
    Foundation/Threads/test_precise_sleep.cpp
    World/Model/test_flag_bit_preservation.cpp
    World/Model/test_lod_purpose.cpp
    World/Model/test_shape_adapter_papauto.cpp
    World/Model/test_shape_adapter_odol_animation.cpp
    World/Model/test_shape_adapter_tangent_basis.cpp
    World/Model/test_shape_adapter_collision.cpp
    World/Model/test_shape_adapter_proxy.cpp
    World/Model/test_preparer_determinism.cpp
    World/Model/test_preparer_radius_certificate.cpp
    World/Model/test_preparer_static_source_envelope.cpp
    World/Model/test_loose_paa_preparation.cpp
    World/Model/test_preparer_native_bc3.cpp
    World/Model/test_paa_preparation_inflight.cpp
    World/Model/test_preparer_warm_textures.cpp
    World/Terrain/test_warm_texture_provenance.cpp
    World/Terrain/test_shape_cache_pressure_policy.cpp
    World/test_simulation_object_coverage.cpp
    World/test_simulation_residency.cpp
    World/test_simulation_fire_collision.cpp
    World/test_static_source_envelope.cpp
    World/test_static_plain_route_probe.cpp
    World/test_static_plain_source_summary.cpp
    World/Model/test_dds_publication_token.cpp
    World/Model/test_model_simulation_source_info.cpp
    World/Model/test_model_source_audit.cpp
    Graphics/Textures/test_bc3_prepared_sidecar.cpp
    World/Model/test_stream_texture_names.cpp
    World/Model/test_stream_tree_fixture.cpp
    World/Model/test_owned_model_read.cpp


    # P3D format tests
    Asset/Formats/P3D/test_mlod_loader.cpp
    Asset/Formats/P3D/test_mlod_writer.cpp
    Asset/Formats/P3D/MLOD/test_mlod_lod_dispatch.cpp
    Asset/Formats/P3D/MLOD/test_odol_revision.cpp
    Asset/Formats/P3D/test_odol_named_selections.cpp
    Asset/Formats/P3D/test_odol40_arma1.cpp
    Asset/Formats/P3D/test_odol40_material_layout.cpp
    Asset/Formats/P3D/test_model_proxy_bounds.cpp
    Asset/Formats/P3D/test_p3d_model_load.cpp
    Asset/Addon/test_virtual_path.cpp
    Graphics/Textures/test_paa_taggs.cpp
    Graphics/Textures/test_prepared_textures.cpp
    Graphics/Textures/test_edds_reader.cpp
    Graphics/Textures/test_dds_texture_source.cpp
    Graphics/Textures/test_sinkhole_texture_formats.cpp
    Graphics/Textures/test_block_compression.cpp
    Asset/Formats/Enfusion/test_pak_archive.cpp
    Asset/Formats/Enfusion/test_ebin_world.cpp
    Asset/Formats/Enfusion/test_resource_database.cpp
    Asset/Formats/Enfusion/test_xob_model.cpp
    Asset/Formats/Enfusion/test_xob_collision.cpp
    Foundation/Algorithms/test_lzo1x.cpp
    Graphics/Rendering/test_paa_lzo_replay_cache.cpp
    Graphics/Rendering/test_paa_dxt_bounds.cpp
    Graphics/Rendering/test_archive_source_binding.cpp
    Foundation/Algorithms/test_lz4block.cpp
    Foundation/Algorithms/test_radixsort.cpp
    Foundation/Algorithms/test_sha256.cpp
    Asset/Cache/test_derived_asset_key.cpp
    Asset/Cache/test_derived_blob_store.cpp
    Asset/Formats/Material/test_rv_material_source.cpp
    Asset/Formats/Material/test_emat_source.cpp
    Asset/Formats/Material/test_emat_material_adapter.cpp
    Asset/Formats/Material/test_shader_schema.cpp
    Asset/Formats/Material/test_terrain_family.cpp
    Asset/Formats/Material/test_material_channels.cpp

    # PoseidonFormats C API tests
    Asset/Formats/test_poseidon_formats_capi.cpp

    # Foundation primitives (formerly Utility/)
    Foundation/Algorithms/test_crc.cpp
    Foundation/Math/test_interpol.cpp
    Foundation/Math/test_statistics.cpp
    Foundation/Math/test_fixed.cpp
    Foundation/Strings/test_strformat.cpp
    Foundation/Strings/test_serial_mbcs.cpp
    Foundation/Time/test_time.cpp

    # Rendering tests (batch-02)
    Graphics/Rendering/test_primitives.cpp

    # Simulation tests (batch-03)
    World/Simulation/test_animation.cpp
    World/Simulation/test_corpse_pose.cpp
    World/Simulation/test_cave_bounds.cpp
    Dev/test_cave_mesh.cpp
    Dev/test_trench_mesh.cpp
    Dev/test_cave_editor_controls.cpp

    # Rendering tests (batch-06)
    Graphics/Rendering/test_shape.cpp
    Graphics/Rendering/test_odol_v7_wire_format.cpp
    Graphics/Rendering/test_shape_section_inheritance.cpp
    Graphics/Rendering/test_shape_logical_residency.cpp
    Graphics/Rendering/test_render_flags.cpp
    Graphics/Rendering/test_light_visibility.cpp
    Graphics/Rendering/test_render_pass_descriptor.cpp
    Graphics/Rendering/test_build_render_pass_descriptor.cpp
    Graphics/Rendering/test_ascii_search.cpp
    Graphics/Rendering/test_validate_render_pass_descriptor.cpp
    Graphics/Rendering/test_render_frame_invariants.cpp
    Graphics/Rendering/test_render_frame_buffers.cpp
    Graphics/Rendering/test_render_frame_geometry.cpp
    Graphics/Rendering/test_render_frame_emission_timing.cpp
    Graphics/Rendering/test_spec_bit_unambiguity.cpp
    Graphics/Rendering/test_complete_material_descriptor.cpp
    Graphics/Rendering/test_render_frame_texture_desc.cpp
    Graphics/Rendering/test_render_frame_coordinates.cpp
    Graphics/Rendering/test_render_frame_build_frame.cpp
    Graphics/Rendering/test_render_frame_stats.cpp
    Graphics/Rendering/test_render_frame_perf_budget.cpp
    Graphics/Rendering/test_render_frame_runtime_checks.cpp
    Graphics/Rendering/test_presentation_snapshot_ring.cpp
    Graphics/Rendering/test_cloudlet_depth_contract.cpp

    # Rendering tests (batch-07)
    Graphics/Rendering/test_colors_materials.cpp

    # World tests (batch-08)
    World/test_terrain.cpp
    World/Terrain/test_water_surface_query.cpp
    World/Terrain/test_water_bodies.cpp
    World/Terrain/test_terrain_surface_catalog.cpp
    World/test_terrain_gpu_interp.cpp
    World/test_terrain_cdlod.cpp
    World/test_terrain_background.cpp
    World/test_object_animated_bounds.cpp
    World/test_face_rig.cpp
    World/test_landscape_object_ids.cpp
    World/test_occlusion_contour.cpp
    World/test_occlusion_sphere_edge.cpp
    World/test_landscape_layout.cpp
    World/test_landscape_layout_no_pch.cpp
    World/Weather/test_wind_model.cpp
    World/Weather/test_airflow_field.cpp
    World/Effects/test_building_interior.cpp

    # Audio tests (batch-09)
    Audio/test_pcm_cache.cpp
    Audio/test_sound.cpp
    Audio/test_wave_file.cpp
    Audio/test_wss_loader.cpp
    Audio/test_audio_init.cpp
    Audio/test_hrtf_mode.cpp
    Audio/test_voice_handle.cpp
    Audio/test_voice_handle_oal.cpp
    Audio/test_volume_pipeline.cpp
    Audio/test_audio_math.cpp
    Audio/test_dx8_reference.cpp
    Audio/test_eax_efx.cpp
    Audio/test_environment_blend.cpp
    Audio/test_capture.cpp
    Audio/test_smoke.cpp
    Audio/test_oal_loopback.cpp
    Audio/test_von_integration.cpp
    Audio/test_mixing_parity.cpp
    Audio/test_campaign_sound_voice_suffix.cpp
    Audio/test_voice_lang_path.cpp
    Audio/test_wave_streaming_buffers.cpp
    Audio/test_voice_budget.cpp
    Audio/test_device_preview_offset.cpp
    Audio/test_audio_cache_audit.cpp
    Audio/Voice/test_von_codec.cpp
    Audio/Voice/test_von_capture.cpp
    Audio/Voice/test_von_openal_speaker.cpp
    Audio/Voice/test_von_net.cpp
    Audio/Voice/test_von_buffer.cpp
    Audio/Voice/test_von_app.cpp

    # Rendering/Draw tests (batch-10)
    Graphics/Rendering/test_draw.cpp
    Graphics/Rendering/test_font_data.cpp
    Graphics/Rendering/test_font_bucket.cpp
    Graphics/Rendering/test_font_mapping.cpp
    Graphics/Rendering/test_font_system.cpp
    Graphics/Rendering/test_font_utf8_drawpath.cpp

    # Debug tests (batch-11)
    Dev/Debug/test_debug_profiling.cpp
    Dev/Debug/test_debug_commands.cpp

    # Ballistics diagnostic: trajectory store, ring eviction, pause gate
    Dev/Diag/test_ballistics_recorder.cpp

    # Streaming observability: counter snapshot + preparer request->residency latency
    Dev/Diag/test_streaming_diag.cpp
    Dev/Diag/test_frame_capture.cpp

    # DIAG-001: diagnostics ported from Malprave (JSON helpers, stuck detector, event writer, diag_step)
    Dev/Diag/test_op_diag.cpp

    # Mission introspection
    Game/Mission/test_mission_info.cpp

    # Core types tests (batch-12)
    Core/Types/test_core_types.cpp

    # Settings tests (per-section user configs)
    UI/Settings/test_aspectRatio.cpp
    UI/Settings/test_presentation.cpp
    UI/Settings/test_presentationRects.cpp
    UI/Settings/test_presentation_single_entry.cpp
    UI/Settings/test_audioConfig.cpp
    UI/Settings/test_controlsConfig.cpp
    UI/Settings/test_displayConfig.cpp
    UI/Settings/test_display_startup_overrides.cpp
    UI/Settings/test_graphicsConfig.cpp
    UI/Settings/test_gameSettingsConfig.cpp
    UI/Settings/test_viewDistance.cpp
    UI/Settings/test_contextControlsConfig.cpp
    UI/Settings/test_gamepadConfig.cpp
    UI/Settings/test_mouseConfig.cpp

    # Platform config tests (batch-13)
    Platform/test_platform_config.cpp

    # Platform game directories tests
    Platform/test_gameDirs.cpp

    # Platform main tests (main)
    Platform/test_main.cpp

    # Input tests (batch-14)
    Input/test_binding_conflict.cpp
    Input/test_combo_fire_regression.cpp
    Input/test_controls_category.cpp
    Input/test_input_codes.cpp
    Input/test_modifier_combos.cpp
    Input/test_input_subsystem.cpp
    Input/test_input_context.cpp
    Input/test_input_profile.cpp
    Input/test_input_integration.cpp
    Input/test_controller_ui_layout.cpp
    Input/test_response_curve.cpp
    Input/test_combo_detector.cpp
    Input/test_viewer_controller.cpp
    Input/test_viewer_controls_apply.cpp
    Input/test_pilot_controller.cpp
    Input/test_gamepad_state.cpp
    Input/test_mouse_state.cpp
    Input/test_keyboard_state.cpp
    Input/test_ui_key_repeat_sdl.cpp
    Graphics/test_wgpu_instance_queue_contract.cpp
    Graphics/test_parked_model_refill.cpp
    Input/test_rumble_manager.cpp

    # IO tests (batch-15)
    World/test_io_types.cpp
    IO/test_mapFile.cpp
    IO/test_paramFileExt.cpp
    IO/test_paramFile_defaults.cpp

    # Scene/Camera tests (batch-17)
    World/Scene/test_camera_enums.cpp

    # Game tests
    Game/test_game_state_ext.cpp
    Game/test_oper_map.cpp
    Game/test_editor.cpp
    Game/test_titeffects_split.cpp
    Game/test_tri_generic_asserts.cpp
    Game/test_tri_value_getters.cpp
    Dev/Debug/test_dev_mode_gates.cpp
    Dev/Debug/test_dev_panel_search.cpp

    # Network tests
    Network/test_network.cpp
    Network/test_player_mute_ignore.cpp
    Network/test_multiplayer_auth.cpp
    Network/test_ip_ban.cpp
    Network/test_ban_commands.cpp
    Network/test_network_server.cpp
    Network/test_mp_join.cpp
    Network/test_master_server.cpp
    Network/test_network_dispatch.cpp
    Network/test_script_codec.cpp
    Network/test_wire_format.cpp
    Network/test_transport_io.cpp
    Network/test_encoded_matrix3.cpp
    Network/test_rot8b.cpp
    Network/test_chat_utf8_wire.cpp
    Network/Legacy/test_network_channel.cpp
    Network/Legacy/test_network_message.cpp
    Network/Legacy/test_network_peer.cpp
    Network/Legacy/test_network_pool.cpp
    Network/Legacy/test_random_james.cpp
    Network/XML/test_xml_attributes.cpp
    Network/XML/test_xml_entities.cpp
    Network/XML/test_xml_errors.cpp
    Network/XML/test_xml_parser.cpp
    Network/XML/test_xml_squad.cpp

    # Rendering tests
    Graphics/Rendering/test_smokes.cpp
    Graphics/Rendering/test_smoke_density_grid.cpp
    Graphics/Rendering/test_pactext.cpp

    # Weapons tests
    World/Entities/Weapons/test_shots.cpp
    World/Entities/Weapons/test_weapon_displayname_lang_switch.cpp

    # Vehicles tests
    World/Entities/Vehicles/test_ship.cpp
    World/Entities/Vehicles/test_tank.cpp
    World/Entities/Vehicles/test_car.cpp
    World/Entities/Vehicles/test_motorcycle.cpp
    World/Entities/Vehicles/Air/test_airplane.cpp
    World/Entities/Vehicles/Air/test_helicopter.cpp

    # Scene tests
    World/Scene/test_object.cpp
    World/Scene/test_render_interp_camera.cpp
    World/Scene/test_scene.cpp
    World/Scene/test_scene_preloader.cpp
    World/Scene/test_surface_draw_order.cpp
    World/Scene/test_alpha_sort_order.cpp

    # Core subsystem tests
    UI/Locale/test_stringtable_system.cpp
    UI/Locale/test_language_registry.cpp
    Core/test_config_system.cpp

    # Detection tests
    World/Detection/test_detector.cpp

    # Infantry tests
    World/Entities/Infantry/test_soldierOld.cpp

    # AI tests
    AI/test_ai.cpp
    AI/test_path_planner.cpp
    AI/test_simple_path_admission.cpp
    AI/test_entity_event_handlers.cpp
    AI/test_vehicle_ai.cpp
    AI/test_entity_displayname_lang_switch.cpp
    AI/test_ai_subgroup.cpp
    AI/test_unit_number_list.cpp
    AI/test_stable_ordering.cpp
    AI/test_ai_group_scan_determinism.cpp

    # UI tests
    UI/Map/test_uiMap.cpp
    UI/Map/test_map_legend_localized.cpp
    UI/Map/test_equipment_localization.cpp
    UI/Controls/test_uiControls.cpp
    UI/Controls/test_progress_bar_widget.cpp
    UI/Controls/test_control_object_rotate_devmode.cpp
    UI/Controls/test_html_table_row.cpp
    UI/test_button_modal_page.cpp
    UI/test_capture_page.cpp
    UI/test_confirm_page.cpp
    UI/test_confirm_revert_page.cpp
    UI/test_display_page.cpp
    UI/test_displayUI.cpp
    UI/test_audio_page.cpp
    UI/test_flat_menu_page.cpp
    UI/test_game_page.cpp
    UI/test_font_renderer.cpp
    UI/test_gamepad_page.cpp
    UI/test_gamepad_tuning_page.cpp
    UI/test_graphics_page.cpp
    UI/test_html_body_text.cpp
    UI/test_html_text_wrap.cpp
    UI/test_kbm_page.cpp
    UI/test_meter_widget.cpp
    UI/Locale/test_mission_language_detector.cpp
    UI/test_mouse_page.cpp
    UI/test_mp_version_message_localization.cpp
    UI/test_main_menu_layout.cpp
    UI/Controls/test_cursor_layout.cpp
    UI/test_notebook_theme.cpp
    UI/test_optionsUI.cpp
    UI/test_local_maps_catalog.cpp
    UI/test_local_vehicles_catalog.cpp
    UI/test_addon_content.cpp
    UI/test_profile_squad_audit.cpp
    UI/test_press_button_page.cpp
    UI/test_press_key_page.cpp
    UI/test_ui_zbias_staircase.cpp
    UI/test_gameModule.cpp
    UI/test_viewer_cursor_overlay.cpp
    UI/test_game_cursor_overlay.cpp
    UI/InGame/test_inGameUI.cpp
    # Vehicles tests
    World/Entities/Vehicles/test_transport.cpp
    # World tests
    World/test_world.cpp
    World/test_simulation_demand.cpp
    World/test_authored_object_ids.cpp
    World/test_simulation_model_bounds.cpp
    World/test_simulation_coverage_index.cpp
    World/test_world_object_placement_frame.cpp
    World/test_world_serialization.cpp
    World/test_segment_cache_lod.cpp
    World/test_effect_shape_guards_audit.cpp
    World/test_sky_load_or_keep_audit.cpp
    World/test_gamestate_vars_roundtrip.cpp

    # Simulation tests (rest batch)
    World/Simulation/test_cloth.cpp
    World/Simulation/test_uniform_wetness.cpp
    Graphics/Rendering/test_muzzle_flash_appearance.cpp
    World/Simulation/test_remote_interp.cpp

    # Tools shared library tests
    Asset/Probes/test_asset_info.cpp
    Asset/Probes/test_asset_preview.cpp
    Asset/Probes/test_shadow_inspect.cpp
    Tools/test_font.cpp
    Tools/test_file_category.cpp
    Tools/test_pbo_inspect_scan.cpp
    Core/test_mod_archive.cpp
    Core/test_mod_collection.cpp
    Core/test_mod_install.cpp
    Core/test_mod_selection.cpp
    Core/test_server_mod_resolve.cpp
    Core/test_pending_connect.cpp
    Core/test_download_progress.cpp
    Core/test_download_worker.cpp
    Core/test_download_dialog_view.cpp
    Asset/Probes/test_wave_to_lip.cpp
)

set(POSEIDON_TEST_HEADERS
    test_fixtures.hpp
    Asset/Formats/BISFramework/test_helpers.hpp
)

set_source_files_properties(World/test_landscape_layout_no_pch.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
)

add_executable(PoseidonTests ${POSEIDON_TEST_SOURCES} ${POSEIDON_TEST_HEADERS})
include("${CMAKE_SOURCE_DIR}/cmake/MeshoptimizerClod.cmake")
target_link_libraries(PoseidonTests PRIVATE OpenPoseidon::MeshoptimizerClod)

# Link against Poseidon (which now also owns the former Base and support payloads) and Catch2
target_link_libraries(PoseidonTests PRIVATE
    PoseidonGL33
    Poseidon
    PoseidonOpenAL
    PoseidonFormats
    OpenAL::OpenAL
    Catch2::Catch2
)

# glslang — used by test_shader_named_outputs.cpp (I-28) to compile every
# shipped GL33 shader and assert the source parses cleanly.
find_package(glslang CONFIG REQUIRED)
target_link_libraries(PoseidonTests PRIVATE
    glslang::glslang
    glslang::glslang-default-resource-limits
)

# zstd — test_mod_archive.cpp generates multi-chunk zstd frames at test time to
# exercise ModArchive::DecompressArchive's streaming loop + truncation guard.
find_package(zstd CONFIG REQUIRED)
target_link_libraries(PoseidonTests PRIVATE zstd::libzstd)
if(WIN32)
    target_link_libraries(PoseidonTests PRIVATE 
        ws2_32      # Windows sockets
        winmm       # Windows multimedia
        version     # Version info
        comctl32    # Common controls
        legacy_stdio_definitions.lib  # Legacy CRT functions (swprintf, etc.)
    )
elseif(APPLE)
    target_link_libraries(PoseidonTests PRIVATE
        PoseidonGL33 Poseidon PoseidonOpenAL
        pthread dl
    )
else()
    target_link_libraries(PoseidonTests PRIVATE
        -Wl,--start-group
        PoseidonGL33 Poseidon PoseidonOpenAL
        -Wl,--end-group
        pthread dl
    )
endif()

# Add link directory for external libs (eaxguid.lib)
# 32-bit only
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_link_directories(PoseidonTests PRIVATE
        ${CMAKE_SOURCE_DIR}/thirdparty/libs
    )
endif()

# Disable SafeSEH for compatibility with legacy external .obj files
if(MSVC OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_link_options(PoseidonTests PRIVATE /SAFESEH:NO)
endif()


# Compile definitions to match Poseidon library
target_compile_definitions(PoseidonTests PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>
    POSEIDON_TEST_BUILD  # Disable strict static initialization checks for Catch2
    GAME_DATA_DIR="${OFPR_GAME_DATA_DIR}"
    TESTS_ROOT_DIR="${CMAKE_SOURCE_DIR}/tests"
)

# Include directories
target_include_directories(PoseidonTests PRIVATE 
    ${CMAKE_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Copy shared test fixtures to output directory
set(FIXTURES_ROOT ${CMAKE_SOURCE_DIR}/tests/fixtures)
add_custom_command(TARGET PoseidonTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/audio       $<TARGET_FILE_DIR:PoseidonTests>/fixtures/audio
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/cfg         $<TARGET_FILE_DIR:PoseidonTests>/fixtures/cfg
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/font        $<TARGET_FILE_DIR:PoseidonTests>/fixtures/font
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/mlod        $<TARGET_FILE_DIR:PoseidonTests>/fixtures/mlod
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/p3d         $<TARGET_FILE_DIR:PoseidonTests>/fixtures/p3d
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/paa         $<TARGET_FILE_DIR:PoseidonTests>/fixtures/texture/paa
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/pac         $<TARGET_FILE_DIR:PoseidonTests>/fixtures/texture/pac
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/jpg         $<TARGET_FILE_DIR:PoseidonTests>/fixtures/jpg
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/pbo         $<TARGET_FILE_DIR:PoseidonTests>/fixtures/pbo
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/rtm         $<TARGET_FILE_DIR:PoseidonTests>/fixtures/rtm
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/wrp         $<TARGET_FILE_DIR:PoseidonTests>/fixtures/wrp
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FIXTURES_ROOT}/xml         $<TARGET_FILE_DIR:PoseidonTests>/fixtures
    COMMENT "Copying test fixtures to output directory"
)

# Copy TcPbo WCX plugin DLL next to test executable
if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(WCX_TEST_NAME "pbo.wcx64")
    else()
        set(WCX_TEST_NAME "pbo.wcx")
    endif()
    add_custom_command(TARGET PoseidonTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:TcPbo>
            $<TARGET_FILE_DIR:PoseidonTests>/${WCX_TEST_NAME}
        COMMENT "Copying TcPbo plugin for tests"
    )
    add_dependencies(PoseidonTests TcPbo)
endif()

# Copy ASan runtime DLL to build directory for sanitizer configurations
if(WIN32)
include(${CMAKE_SOURCE_DIR}/cmake/FindLLVMSanitizer.cmake)

# x86 sanitizer builds
if(VCPKG_TARGET_TRIPLET MATCHES "x86.*-san")
    find_asan_runtime(ASAN_DLL_PATH "x86")
    if(ASAN_DLL_PATH)
        add_custom_command(TARGET PoseidonTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ASAN_DLL_PATH}"
                $<TARGET_FILE_DIR:PoseidonTests>
            COMMENT "Copying ASan runtime DLL (x86): ${ASAN_DLL_PATH}"
        )
    endif()
endif()

# x64 sanitizer builds
if(VCPKG_TARGET_TRIPLET MATCHES "x64.*-san")
    find_asan_runtime(ASAN_DLL_PATH "x64")
    if(ASAN_DLL_PATH)
        add_custom_command(TARGET PoseidonTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ASAN_DLL_PATH}"
                $<TARGET_FILE_DIR:PoseidonTests>
            COMMENT "Copying ASan runtime DLL (x64): ${ASAN_DLL_PATH}"
        )
    endif()
endif()
endif()

include(${CMAKE_SOURCE_DIR}/cmake/CatchWindowsSafe.cmake)
ofpr_catch_discover_tests(PoseidonTests 
    ${OFPR_CATCH_TEST_SPEC_ARGS}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    PROPERTIES
        ENVIRONMENT "UBSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/ubsan-suppressions.txt;LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/lsan-suppressions.txt"
)
