function(apply_managed_component_patch patch_relative_path)
    set(patch_path "${CMAKE_SOURCE_DIR}/${patch_relative_path}")
    if(NOT EXISTS "${patch_path}")
        message(FATAL_ERROR "Managed-component patch is missing: ${patch_path}")
    endif()

    find_package(Git REQUIRED QUIET)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check --whitespace=nowarn
                --ignore-space-change --ignore-whitespace "${patch_path}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE patch_check_result
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(patch_check_result EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn
                    --ignore-space-change --ignore-whitespace "${patch_path}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE patch_apply_result
            OUTPUT_VARIABLE patch_apply_output
            ERROR_VARIABLE patch_apply_error
        )
        if(NOT patch_apply_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to apply ${patch_relative_path}: "
                "${patch_apply_output}${patch_apply_error}")
        endif()
        message(STATUS "Applied managed-component patch: ${patch_relative_path}")
        return()
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --whitespace=nowarn
                --ignore-space-change --ignore-whitespace "${patch_path}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE reverse_check_result
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(reverse_check_result EQUAL 0)
        message(STATUS "Managed-component patch already applied: ${patch_relative_path}")
        return()
    endif()

    message(FATAL_ERROR
        "${patch_relative_path} applies neither forward nor in reverse. "
        "The locked managed component likely changed; refresh the patch explicitly.")
endfunction()
