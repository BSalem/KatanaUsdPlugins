# Copyright 2019 Foundry
#
# Licensed under the Apache License, Version 2.0 (the "Apache License")
# with the following modification; you may not use this file except in
# compliance with the Apache License and the following modification to it:
# Section 6. Trademarks. is deleted and replaced with:
#
# 6. Trademarks. This License does not grant permission to use the trade
# names, trademarks, service marks, or product names of the Licensor
# and its affiliates, except as required to comply with Section 4(c) of
# the License and to reproduce the content of the NOTICE file.
#
# You may obtain a copy of the Apache License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the Apache License with the above modification is
# distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied. See the Apache License for the specific
# language governing permissions and limitations under the Apache License.

include(UsdUtils)
# Sets up interfaces for required thirdparty libraries, defines functions
# to keep this logic separate
include(SetupInterfaces)


if(USE_KATANA_THIRDPARTY_LIBS)
    set(USE_KATANA_BOOST ON)
    set(USE_KATANA_TBB  ON)
    set(USE_KATANA_PYTHON ON)
    set(USE_KATANA_USD ON)
endif()


# KatanaAPI
find_package(KatanaAPI REQUIRED)

# Python
add_python_interface()
# Boost
add_boost_interface()
# TBB
add_tbb_interface()
# USD Core
add_usd_interface()



function(pxr_library NAME)
    set(options
    )
    set(oneValueArgs
        TYPE
        PRECOMPILED_HEADER_NAME
    )
    set(multiValueArgs
        PUBLIC_CLASSES
        PUBLIC_HEADERS
        PRIVATE_CLASSES
        PRIVATE_HEADERS
        CPPFILES
        LIBRARIES
        INCLUDE_DIRS
        RESOURCE_FILES
        PYTHON_PUBLIC_CLASSES
        PYTHON_PRIVATE_CLASSES
        PYTHON_PUBLIC_HEADERS
        PYTHON_PRIVATE_HEADERS
        PYTHON_CPPFILES
        PYMODULE_CPPFILES
        PYMODULE_FILES
        PYSIDE_UI_FILES
    )
    cmake_parse_arguments(args
        "${options}"
        "${oneValueArgs}"
        "${multiValueArgs}"
        ${ARGN}
    )
    foreach(cls ${args_PUBLIC_CLASSES})
        list(APPEND ${NAME}_CPPFILES ${cls}.cpp)
        list(APPEND args_PUBLIC_HEADERS ${cls}.h)
    endforeach()
    foreach(cls ${args_PRIVATE_CLASSES})
        list(APPEND ${NAME}_CPPFILES ${cls}.cpp)
    endforeach()
    string(TOUPPER ${NAME} uppercaseName)
    if(args_TYPE STREQUAL "STATIC")
        add_library(${NAME} STATIC "${args_CPPFILES};${${NAME}_CPPFILES}")
        set_target_properties(${NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        # no install for static libraries
        _get_install_dir("lib/usd" pluginInstallPrefix)
    elseif (args_TYPE STREQUAL "SHARED")
        add_library(${NAME} SHARED "${args_CPPFILES};${${NAME}_CPPFILES}")
        set_target_properties(${NAME} PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            PUBLIC_HEADER "${args_PUBLIC_HEADERS}"
        )
        if(CMAKE_BUILD_TYPE MATCHES Debug AND WIN32)
            target_compile_options(${NAME} PRIVATE "/bigobj")
        endif()
        list(APPEND ${NAME}_DEFINITIONS ${uppercaseName}_EXPORTS=1)
        _get_install_dir("lib/usd" pluginInstallPrefix)

        if(BUILD_KATANA_INTERNAL_USD_PLUGINS)
            _katana_build_install(${NAME} lib)
            if(args_PUBLIC_HEADERS)
                foreach(file ${args_PUBLIC_HEADERS})
                    file(COPY ${file}
                        DESTINATION
                            ${PXR_INSTALL_SUBDIR}/include/${NAME}
                    )
                endforeach()
            endif()
        else()
            install(TARGETS ${NAME}
                LIBRARY DESTINATION lib
                ARCHIVE DESTINATION lib
                RUNTIME DESTINATION lib
                PUBLIC_HEADER DESTINATION include/${NAME}
            )
        endif()
    elseif (args_TYPE STREQUAL "PLUGIN")
        add_library(${NAME} SHARED "${args_CPPFILES};${${NAME}_CPPFILES}")
        if(BUILD_KATANA_INTERNAL_USD_PLUGINS)
            _katana_build_install(${NAME} plugin/Libs)
        endif()
        set_target_properties(${NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        list(APPEND ${NAME}_DEFINITIONS ${uppercaseName}_EXPORTS=1)
        _get_install_dir("plugin" pluginInstallPrefix)
        install(TARGETS ${NAME} DESTINATION ${pluginInstallPrefix}/Libs)
    else()
        message(FATAL_ERROR "Unsupported library type: " args_TYPE)
    endif()

    if (args_TYPE STREQUAL "SHARED" OR args_TYPE STREQUAL "PLUGIN")
        if (NOT WIN32)
            set_target_properties(${NAME}
                PROPERTIES
                BUILD_WITH_INSTALL_RPATH TRUE
                INSTALL_RPATH "$ORIGIN;$ORIGIN/../../lib"
            )
        endif ()
    endif ()

    set(pluginToLibraryPath "")
    if(BUILD_KATANA_INTERNAL_USD_PLUGINS)
        if(WIN32)
            set(pluginToLibraryPath "Fn${NAME}.dll")
        else()
            set(pluginToLibraryPath "Fn${NAME}.so")
        endif()
    else()
        if(WIN32)
            set(pluginToLibraryPath "${NAME}.dll")
        else()
            set(pluginToLibraryPath "lib${NAME}.so")
        endif()
    endif()

    if(BUILD_KATANA_INTERNAL_USD_PLUGINS)
        set_target_properties(${NAME} PROPERTIES CXX_VISIBILITY_PRESET default)
        set_target_properties(${NAME} PROPERTIES CMAKE_C_VISIBILITY_PRESET default)
        set_target_properties(${NAME} PROPERTIES CMAKE_VISIBILITY_INLINES_HIDDEN FALSE)
        set_target_properties(${NAME} PROPERTIES PREFIX "Fn")
    endif()
    _install_resource_files(
        ${NAME}
        "lib"
        "${pluginToLibraryPath}"
        ${args_RESOURCE_FILES}
    )

    target_include_directories(
        ${NAME}
        PRIVATE
        ${args_INCLUDE_DIRS}
        ${KATANA_USD_PLUGINS_SRC_ROOT}/lib
        ${KATANA_USD_PLUGINS_SRC_ROOT}/plugin
        ${KATANA_API_INCLUDE_DIR}
        ${GEOLIB_SRC_API_INCLUDE_DIR}
        ${KATANA_SRC_API_INCLUDE_DIR}
    )
    target_compile_definitions(
        ${NAME}
        PRIVATE
        -DBOOST_ALL_NO_LIB
        -DFNASSET_STATIC=1
        -DFNATTRIBUTE_STATIC=1
        -DFNATTRIBUTEFUNCTION_STATIC=1
        -DFNCONFIG_STATIC=1
        -DFNDEFAULTATTRIBUTEPRODUCER_STATIC=1
        -DFNGEOLIB_STATIC=1
        -DFNGEOLIBSERVICES_STATIC=1
        -DFNLOGGING_STATIC=1
        -DFNPLATFORM_STATIC=1
        -DFNPLUGINMANAGER_STATIC=1
        -DFNPLUGINSYSTEM_STATIC=1
        -DFNRENDEROUTPUTLOCATION_STATIC=1
        -DFNRENDEROUTPUTUTILS_STATIC=1
        -DFNRENDER_STATIC=1
        -DFNRENDERERINFO_STATIC=1
        -DFNSCENEGRAPHGENERATOR_STATIC=1
        -DFNSCENEGRAPHITERATOR_STATIC=1
        -DPYSTRING_STATIC=1
        -DFNDISPLAYDRIVER_STATIC=1
        -DFNVIEWERMODIFIER_STATIC=1
        -DFNVIEWERAPI_STATIC=1
        ${${NAME}_DEFINITIONS}
    )
    target_compile_options(
        ${NAME}
        PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-Wall -Wno-unused-but-set-variable -Wno-deprecated -Wno-unused-local-typedefs -Werror=narrowing>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /wd4267 /wd4100 /wd4702 /wd4244 /wd4800 /wd4996 /wd4456 /wd4127 /wd4701 /wd4305 /wd4838 /wd4624 /wd4506 /wd4245 /wd4996 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNOGDI /FIiso646.h>
        $<$<CONFIG:Debug>:-DTBB_USE_DEBUG=1> # needed on Windows to avoid errors when Python changes _DEBUG
        $<$<CONFIG:Debug>:-DBOOST_DEBUG_PYTHON> # needed on Windows to help with the autolink library name (even though adding BOOST_ALL_NO_LIB doesn't seem to disable this)
    )
    target_link_libraries(
        ${NAME}
        PRIVATE
        ${args_LIBRARIES}
        $<$<CXX_COMPILER_ID:MSVC>:OPENGL32.lib>
    )

    # make a separate shared library for the Python wrapper
    if(args_PYMODULE_CPPFILES AND ENABLE_KATANAUSD_PYTHON_PLUGINS)
        set(pythonWrapperModuleName "_${NAME}")
        _get_python_module_name(${NAME} pyModuleName)

        add_library(${pythonWrapperModuleName} SHARED ${args_PYMODULE_CPPFILES})

        target_include_directories(
            ${pythonWrapperModuleName}
            PRIVATE
            ${KATANA_USD_PLUGINS_SRC_ROOT}/lib
            ${KATANA_USD_PLUGINS_SRC_ROOT}/plugin
            ${KATANA_API_INCLUDE_DIR}
            ${GEOLIB_SRC_API_INCLUDE_DIR}
            ${KATANA_SRC_API_INCLUDE_DIR}
        )
        target_compile_definitions(
            ${pythonWrapperModuleName}
            PRIVATE
            -DBOOST_ALL_NO_LIB
            MFB_PACKAGE_NAME=${NAME}
            MFB_ALT_PACKAGE_NAME=${NAME}
            MFB_PACKAGE_MODULE="${pyModuleName}"
        )
        if(BUILD_KATANA_INTERNAL_USD_PLUGINS)
            _katana_build_install(
                ${pythonWrapperModuleName}
                lib/python/${pyModuleName}
            )
        endif()
        target_compile_options(
            ${pythonWrapperModuleName}
            PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-Wall -Wno-deprecated -Wno-unused-local-typedefs>
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /wd4244 /wd4305 /wd4100 /wd4459 /wd4996 /DWIN32_LEAN_AND_MEAN /DNOMINMAX>
            $<$<CONFIG:Debug>:-DBOOST_DEBUG_PYTHON> # needed on Windows to help with the autolink library name (even though adding BOOST_ALL_NO_LIB doesn't seem to disable this)
        )
        target_link_libraries(
            ${pythonWrapperModuleName}
            PRIVATE
            ${args_LIBRARIES}
            ${NAME}
        )
        if(PXR_INSTALL_SUBDIR)
            set(pyModuleInstallDir ${PXR_INSTALL_SUBDIR}/lib/python/${pyModuleName})
        else()
            set(pyModuleInstallDir lib/python/${pyModuleName})
        endif()

        install(TARGETS ${pythonWrapperModuleName} DESTINATION "${pyModuleInstallDir}")
        unset(pyModuleInstallDir)

        if(args_PYMODULE_FILES AND ENABLE_KATANAUSD_PYTHON_PLUGINS)
            _install_python(
                ${pythonWrapperModuleName}
                FILES ${args_PYMODULE_FILES}
            )
        endif()

        set_target_properties(${pythonWrapperModuleName} PROPERTIES PREFIX "")
        if(WIN32)
            # Python modules must be suffixed with .pyd on Windows.
            set(PY_MODULE_SUFFIX ".pyd")
            if(CMAKE_BUILD_TYPE MATCHES Debug AND WIN32)
                # Add the `_d` debug suffix to the Python Module
                set(PY_MODULE_SUFFIX _d${PY_MODULE_SUFFIX})
            endif()
            set_target_properties(
                ${pythonWrapperModuleName}
                PROPERTIES
                SUFFIX ${PY_MODULE_SUFFIX}
            )
        endif()
    endif()
endfunction()

macro(pxr_static_library NAME)
    pxr_library(${NAME} TYPE "STATIC" ${ARGN})
endmacro(pxr_static_library)

macro(pxr_shared_library NAME)
    pxr_library(${NAME} TYPE "SHARED" ${ARGN})
endmacro(pxr_shared_library)

macro(pxr_plugin NAME)
    pxr_library(${NAME} TYPE "PLUGIN" ${ARGN})
endmacro(pxr_plugin)

function(pxr_test_scripts)
    MESSAGE(AUTHOR_WARNING "Test scripts not implemented")
endfunction()

function(pxr_install_test_dir)
    MESSAGE(AUTHOR_WARNING "Test install dir not implemented")
endfunction()

function(pxr_register_test TEST_NAME)
    MESSAGE(AUTHOR_WARNING "Test register not implemented")
endfunction()
