# Launcher self-update: versions, release JSON, SHA-256, ZIP safety, staging, jobs and rollback.
set(_crackdown_updater_sources
    ../src/updater/github_release.cpp
    ../src/updater/install_transaction.cpp
    ../src/updater/json.cpp
    ../src/updater/sha256.cpp
    ../src/updater/update_job.cpp
    ../src/updater/update_paths.cpp
    ../src/updater/update_platform.cpp
    ../src/updater/update_zip.cpp
    ../src/updater/version.cpp
    ../src/updater/third_party/miniz/miniz.c
)
set_source_files_properties(../src/updater/third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "-w")
crackdown_add_test(update_tests SOURCES update_tests.cpp ${_crackdown_updater_sources})
if(APPLE)
    crackdown_add_test(macos_process_tests SOURCES macos_process_tests.cpp ${_crackdown_updater_sources})
    add_test(NAME macos_process_arguments COMMAND macos_process_tests alpha "two words" "q\"uote")
endif()
if(WIN32)
    target_link_libraries(update_tests PRIVATE shell32)
endif()

if(WIN32)
    add_executable(trg-updater WIN32
        ../src/updater/helper_main.cpp
        ../src/updater/install_transaction.cpp
        ../src/updater/sha256.cpp
        ../src/updater/update_job.cpp
        ../src/updater/update_paths.cpp
        ../src/updater/update_platform.cpp
    )
    target_include_directories(trg-updater PRIVATE "${CRACKDOWN_SRC_DIR}")
    target_link_libraries(trg-updater PRIVATE shell32)
    target_link_options(trg-updater PRIVATE -static)
    set_property(TARGET trg-updater PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(trg-updater PRIVATE -Wall -Wextra)

    # The test is also the program the helper restarts, which the helper starts without a console.
    crackdown_add_test(update_helper_tests SOURCES update_helper_tests.cpp)
    set_target_properties(update_helper_tests PROPERTIES WIN32_EXECUTABLE ON)
    target_link_options(update_helper_tests PRIVATE "-Wl,/entry:mainCRTStartup")
    target_link_libraries(update_helper_tests PRIVATE shell32)
    target_compile_definitions(update_helper_tests PRIVATE CRACKDOWN_UPDATER_EXE="$<TARGET_FILE:trg-updater>")
    add_dependencies(update_helper_tests trg-updater)
endif()
