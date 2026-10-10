cmake_minimum_required(VERSION 3.10)

option(DTV "Enable video output" OFF)
option(PIRANHA_ENABLED "Enable scripting input" ON)
option(DISCORD_ENABLED "Enable Discord Rich Presence" ON)

if (DTV)
    add_compile_definitions(ATG_ENGINE_SIM_VIDEO_CAPTURE)
endif (DTV)

if (PIRANHA_ENABLED)
    add_compile_definitions(ATG_ENGINE_SIM_PIRANHA_ENABLED)
endif (PIRANHA_ENABLED)

if (DISCORD_ENABLED)
    add_compile_definitions(ATG_ENGINE_SIM_DISCORD_ENABLED)
endif (DISCORD_ENABLED)

# Enable group projects in folders
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "cmake")

project(engine-sim)

set(CMAKE_CXX_STANDARD 17)

# ========================================================
# GTEST

include(FetchContent)
FetchContent_Declare(
    googletest
    URL
    https://github.com/google/googletest/archive/609281088cfefc76f9d0ce82e1ff6c30cc3591e5.zip
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set_property(TARGET gmock PROPERTY FOLDER "gtest")
set_property(TARGET gmock_main PROPERTY FOLDER "gtest")
set_property(TARGET gtest PROPERTY FOLDER "gtest")
set_property(TARGET gtest_main PROPERTY FOLDER "gtest")

# ========================================================

add_library(engine-sim STATIC
    # Source files
    src/audio_buffer.cpp
    src/camshaft.cpp
    src/crankshaft.cpp
    src/combustion_chamber.cpp
    src/connecting_rod.cpp
    src/convolution_filter.cpp
    src/cylinder_bank.cpp
    src/cylinder_head.cpp
    src/delay_filter.cpp
    src/derivative_filter.cpp
    src/direct_throttle_linkage.cpp
    src/dynamometer.cpp
    src/engine.cpp
    src/exhaust_system.cpp
    src/feedback_comb_filter.cpp
    src/filter.cpp
    src/fuel.cpp
    src/function.cpp
    src/gas_system.cpp
    src/gaussian_filter.cpp
    src/governor.cpp
    src/ignition_module.cpp
    src/impulse_response.cpp
    src/intake.cpp
    src/jitter_filter.cpp
    src/leveling_filter.cpp
    src/low_pass_filter.cpp
    src/part.cpp
    src/piston.cpp
    src/piston_engine_simulator.cpp
    src/simulator.cpp
    src/standard_valvetrain.cpp
    src/starter_motor.cpp
    src/synthesizer.cpp
    src/throttle.cpp
    src/transmission.cpp
    src/utilities.cpp
    src/valvetrain.cpp
    src/vehicle.cpp
    src/vehicle_drag_constraint.cpp
    src/vtec_valvetrain.cpp

    # Include files
    include/audio_buffer.h
    include/application_settings.h
    include/camshaft.h
    include/crankshaft.h
    include/combustion_chamber.h
    include/connecting_rod.h
    include/convolution_filter.h
    include/cylinder_bank.h
    include/cylinder_head.h
    include/delay_filter.h
    include/derivative_filter.h
    include/direct_throttle_linkage.h
    include/dynamometer.h
    include/engine.h
    include/exhaust_system.h
    include/feedback_comb_filter.h
    include/filter.h
    include/fuel.h
    include/function.h
    include/gas_system.h
    include/gaussian_filter.h
    include/governor.h
    include/ignition_module.h
    include/impulse_response.h
    include/intake.h
    include/jitter_filter.h
    include/leveling_filter.h
    include/low_pass_filter.h
    include/part.h
    include/piston.h
    include/piston_engine_simulator.h
    include/simulator.h
    include/standard_valvetrain.h
    include/starter_motor.h
    include/synthesizer.h
    include/throttle.h
    include/transmission.h
    include/units.h
    include/utilities.h
    include/valvetrain.h
    include/vehicle.h
    include/vehicle_drag_constraint.h
    include/vtec_valvetrain.h
)

target_link_libraries(engine-sim
    simple-2d-constraint-solver
    csv-io
    delta-basic)

target_include_directories(engine-sim
    PUBLIC dependencies/submodules)

if (PIRANHA_ENABLED)
    add_library(engine-sim-script-interpreter STATIC
        # Source files
        scripting/src/channel_types.cpp
        scripting/src/compiler.cpp
        scripting/src/engine_context.cpp
        scripting/src/language_rules.cpp

        # Include files
        scripting/include/actions.h
        scripting/include/camshaft_node.h
        scripting/include/channel_types.h
        scripting/include/compiler.h
        scripting/include/connecting_rod_node.h
        scripting/include/crankshaft_node.h
        scripting/include/cylinder_bank_node.h
        scripting/include/cylinder_head_node.h
        scripting/include/engine_context.h
        scripting/include/engine_node.h
        scripting/include/engine_sim.h
        scripting/include/exhaust_system_node.h
        scripting/include/intake_node.h
        scripting/include/function_node.h
        scripting/include/ignition_module_node.h
        scripting/include/ignition_wire_node.h
        scripting/include/impulse_response_node.h
        scripting/include/intake_node.h
        scripting/include/language_rules.h
        scripting/include/node.h
        scripting/include/object_reference_node.h
        scripting/include/object_reference_node_output.h
        scripting/include/piranha.h
        scripting/include/piston_node.h
        scripting/include/rod_journal_node.h
        scripting/include/standard_valvetrain_node.h
        scripting/include/transmission_node.h
        scripting/include/valvetrain_node.h
        scripting/include/vtec_valvetrain_node.h
		scripting/include/vehicle_node.h
    )

    target_include_directories(engine-sim-script-interpreter
        PUBLIC dependencies/submodules)

    target_link_libraries(engine-sim-script-interpreter
        csv-io
        piranha)
endif (PIRANHA_ENABLED)

if (DISCORD_ENABLED)
    add_library(discord STATIC
        # Source files
        dependencies/discord/Discord.cpp

        # Include files
        dependencies/discord/Discord.h
        dependencies/discord/discord_register.h
        dependencies/discord/discord_rpc.h
    )
endif (DISCORD_ENABLED)

add_executable(engine-sim-app WIN32
    # Source files
    src/main.cpp
    src/engine_sim_application.cpp
    src/geometry_generator.cpp
    src/simulation_object.cpp
    src/piston_object.cpp
    src/connecting_rod_object.cpp
    src/ui_element.cpp
    src/ui_manager.cpp
    src/cylinder_pressure_gauge.cpp
    src/ui_math.cpp
    src/gauge.cpp
    src/crankshaft_object.cpp
    src/cylinder_bank_object.cpp
    src/cylinder_head_object.cpp
    src/ui_button.cpp
    src/ui_utilities.cpp
    src/combustion_chamber_object.cpp
    src/oscilloscope.cpp
    src/shaders.cpp
    src/engine_view.cpp
    src/right_gauge_cluster.cpp
    src/cylinder_temperature_gauge.cpp
    src/labeled_gauge.cpp
    src/throttle_display.cpp
    src/afr_cluster.cpp
    src/fuel_cluster.cpp
    src/oscilloscope_cluster.cpp
    src/performance_cluster.cpp
    src/firing_order_display.cpp
    src/load_simulation_cluster.cpp
    src/mixer_cluster.cpp
    src/info_cluster.cpp

    # Include files
    include/delta.h
    include/dtv.h
    include/engine_sim_application.h
    include/geometry_generator.h
    include/simulation_object.h
    include/piston_object.h
    include/connecting_rod_object.h
    include/ui_element.h
    include/ui_manager.h
    include/cylinder_pressure_gauge.h
    include/ui_math.h
    include/units.h
    include/crankshaft_object.h
    include/cylinder_bank_object.h
    include/cylinder_head_object.h
    include/ui_button.h
    include/ui_utilities.h
    include/combustion_chamber_object.h
    include/oscilloscope.h
    include/shaders.h
    include/engine_view.h
    include/right_gauge_cluster.h
    include/cylinder_temperature_gauge.h
    include/labeled_gauge.h
    include/throttle_display.h
    include/afr_cluster.h
    include/fuel_cluster.h
    include/oscilloscope_cluster.h
    include/performance_cluster.h
    include/firing_order_display.h
    include/load_simulation_cluster.h
    include/mixer_cluster.h
    include/info_cluster.h
)

target_link_libraries(engine-sim-app
    engine-sim
)

if (DTV)
    target_link_libraries(engine-sim-app
        direct-to-video)
endif (DTV)

if (PIRANHA_ENABLED)
    target_link_libraries(engine-sim-app
        engine-sim-script-interpreter)
endif (PIRANHA_ENABLED)

if (DISCORD_ENABLED)
    add_library(discord-rpc STATIC IMPORTED)
    set_property(TARGET discord-rpc PROPERTY IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/dependencies/discord/lib/discord-rpc.lib)
    target_link_libraries(engine-sim-app
    	discord
	discord-rpc)
endif (DISCORD_ENABLED)

target_include_directories(engine-sim-app
    PUBLIC dependencies/submodules)

# ========================================================
# Submodule patches
#
# Fixes for submodules that can't be committed upstream from here. Each patch
# is applied to the submodule's working tree at configure time; applying is
# idempotent (a patch that is already in place is detected and skipped).
#
#   piranha-boost-filesystem-1.85.patch
#       boost::filesystem::path::is_complete() was removed in Boost 1.85;
#       is_absolute() is the identical replacement.

set(ENGINE_SIM_SUBMODULE_PATCHES
    "dependencies/submodules/piranha|patches/piranha-boost-filesystem-1.85.patch")

find_package(Git QUIET)
foreach (entry IN LISTS ENGINE_SIM_SUBMODULE_PATCHES)
    string(REPLACE "|" ";" entry "${entry}")
    list(GET entry 0 patch_target)
    list(GET entry 1 patch_file)
    set(patch_target "${CMAKE_CURRENT_SOURCE_DIR}/${patch_target}")
    set(patch_file "${CMAKE_CURRENT_SOURCE_DIR}/${patch_file}")

    if (NOT GIT_FOUND)
        message(FATAL_ERROR "git is required to apply ${patch_file}")
    endif ()

    # --ignore-whitespace tolerates CRLF checkouts (core.autocrlf).
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --ignore-whitespace "${patch_file}"
        WORKING_DIRECTORY "${patch_target}"
        RESULT_VARIABLE patch_already_applied
        OUTPUT_QUIET ERROR_QUIET)

    if (NOT patch_already_applied EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${patch_file}"
            WORKING_DIRECTORY "${patch_target}"
            RESULT_VARIABLE patch_result
            ERROR_VARIABLE patch_error)

        if (NOT patch_result EQUAL 0)
            message(FATAL_ERROR "Failed to apply ${patch_file}:\n${patch_error}")
        endif ()

        message(STATUS "Applied ${patch_file}")
    endif ()
endforeach ()

# ========================================================
# SDL2 (vcpkg compatibility)
#
# delta-studio locates SDL2 through its own legacy Find modules, which only
# search for import libraries named "SDL2"/"SDL2_image". vcpkg's static
# triplets name them differently and export imported targets instead. When
# those targets are available, seed the cache variables the Find modules
# check so that they resolve to the vcpkg targets (which also carry SDL's
# transitive system libraries).

find_package(SDL2 CONFIG QUIET)
find_package(SDL2_image CONFIG QUIET)

if (TARGET SDL2::SDL2-static AND NOT SDL2_LIBRARY)
    set(SDL2_LIBRARY SDL2::SDL2-static CACHE STRING "SDL2 library (vcpkg target)")
elseif (TARGET SDL2::SDL2 AND NOT SDL2_LIBRARY)
    set(SDL2_LIBRARY SDL2::SDL2 CACHE STRING "SDL2 library (vcpkg target)")
endif ()

if (TARGET SDL2_image::SDL2_image-static AND NOT SDL2_IMAGE_LIBRARY)
    set(SDL2_IMAGE_LIBRARY SDL2_image::SDL2_image-static CACHE STRING "SDL2_image library (vcpkg target)")
elseif (TARGET SDL2_image::SDL2_image AND NOT SDL2_IMAGE_LIBRARY)
    set(SDL2_IMAGE_LIBRARY SDL2_image::SDL2_image CACHE STRING "SDL2_image library (vcpkg target)")
endif ()

add_subdirectory(dependencies)

# GTEST

enable_testing()

add_executable(engine-sim-test
    # Source files
    test/gas_system_tests.cpp
    test/function_test.cpp
    test/synthesizer_tests.cpp
)

target_link_libraries(engine-sim-test
    gtest_main
    engine-sim
)

include(GoogleTest)
gtest_discover_tests(engine-sim-test)

# ========================================================
# GAME AUDIO EXPORT
#
# engine-sim-export renders seamless RPM loops, transients and sweeps for use
# in games (see docs/game-audio-export.md). Requires the script interpreter.

option(GAME_AUDIO_EXPORT "Build the game audio exporter (engine-sim-export)" ON)

if (GAME_AUDIO_EXPORT AND NOT PIRANHA_ENABLED)
    message(FATAL_ERROR "GAME_AUDIO_EXPORT requires PIRANHA_ENABLED")
endif ()

if (GAME_AUDIO_EXPORT)
    find_package(Ogg CONFIG REQUIRED)
    find_package(Vorbis CONFIG REQUIRED)
    find_package(mp3lame CONFIG REQUIRED)

    add_library(engine-sim-game-audio STATIC
        # Source files
        src/game_audio/audio_encoders.cpp
        src/game_audio/engine_session.cpp
        src/game_audio/exporter.cpp
        src/game_audio/json_writer.cpp
        src/game_audio/loop_builder.cpp
        src/game_audio/loudness.cpp
        src/game_audio/wav_file.cpp

        # Include files
        include/game_audio/audio_encoders.h
        include/game_audio/engine_session.h
        include/game_audio/exporter.h
        include/game_audio/json_writer.h
        include/game_audio/loop_builder.h
        include/game_audio/loudness.h
        include/game_audio/wav_file.h
    )

    target_link_libraries(engine-sim-game-audio
        PUBLIC
            engine-sim
            engine-sim-script-interpreter
        PRIVATE
            Vorbis::vorbisenc
            Vorbis::vorbis
            Ogg::ogg
            mp3lame::mp3lame)

    target_include_directories(engine-sim-game-audio
        PUBLIC dependencies/submodules)

    add_executable(engine-sim-export
        src/game_audio/export_main.cpp)

    target_link_libraries(engine-sim-export
        engine-sim-game-audio)

    add_executable(engine-sim-game-audio-test
        test/game_audio_tests.cpp)

    target_link_libraries(engine-sim-game-audio-test
        gtest_main
        engine-sim-game-audio
        Vorbis::vorbisfile)

    # The integration tests load the bundled engine scripts.
    target_compile_definitions(engine-sim-game-audio-test
        PRIVATE ENGINE_SIM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

    gtest_discover_tests(engine-sim-game-audio-test)
endif (GAME_AUDIO_EXPORT)
