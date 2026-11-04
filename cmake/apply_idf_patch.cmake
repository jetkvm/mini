function(apply_idf_patch patch_relative_path)
    set(patch_path "${CMAKE_SOURCE_DIR}/${patch_relative_path}")
    set(idf_path "$ENV{IDF_PATH}")
    if(NOT EXISTS "${patch_path}" OR NOT EXISTS "${idf_path}/components/fatfs")
        message(FATAL_ERROR "IDF patch inputs are missing: ${patch_path}, ${idf_path}")
    endif()
    find_package(Git REQUIRED QUIET)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check --whitespace=nowarn
                --ignore-space-change --ignore-whitespace "${patch_path}"
        WORKING_DIRECTORY "${idf_path}"
        RESULT_VARIABLE forward_result OUTPUT_QUIET ERROR_QUIET)
    if(forward_result EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn
                    --ignore-space-change --ignore-whitespace "${patch_path}"
            WORKING_DIRECTORY "${idf_path}"
            RESULT_VARIABLE apply_result
            OUTPUT_VARIABLE apply_output ERROR_VARIABLE apply_error)
        if(NOT apply_result EQUAL 0)
            message(FATAL_ERROR "Failed to apply IDF patch: ${apply_output}${apply_error}")
        endif()
        message(STATUS "Applied ESP-IDF patch: ${patch_relative_path}")
        return()
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --whitespace=nowarn
                --ignore-space-change --ignore-whitespace "${patch_path}"
        WORKING_DIRECTORY "${idf_path}"
        RESULT_VARIABLE reverse_result OUTPUT_QUIET ERROR_QUIET)
    if(reverse_result EQUAL 0)
        message(STATUS "ESP-IDF patch already applied: ${patch_relative_path}")
        return()
    endif()
    message(FATAL_ERROR
        "${patch_relative_path} matches neither ESP-IDF forward nor reverse; "
        "this project is locked to the reviewed ESP-IDF 6.0.2 FatFs layout")
endfunction()
