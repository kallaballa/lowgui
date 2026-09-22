cmake_policy(SET CMP0079 NEW)

set(the_description "lowgui")

if(NOT (TARGET ${the_module}))
  ocv_add_module(lowgui opencv_core opencv_imgproc opencv_imgcodecs opencv_v4d opencv_plan)

  file(GLOB lowgui_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
  ocv_glob_module_sources(${lowgui_sources})

  # v4d's public headers transitively include third-party imgui/nanovg headers,
  # so every compilation unit that touches lowgui's internal headers must have
  # these on the include path (module sources and the test binaries).
  # The Plan-V4D *submodule* lives inside this repo at ./Plan-V4D — never point
  # at a sibling ../Plan-V4D, which breaks in a fresh CI checkout.
  set(LOWGUI_GPU_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/Plan-V4D/modules/v4d/third/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/Plan-V4D/modules/v4d/third/imgui/backends/"
    "${CMAKE_CURRENT_SOURCE_DIR}/Plan-V4D/modules/v4d/third/nanovg/src/"
  )
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Plan-V4D/modules/v4d/third/imgui/imgui.h")
    message(FATAL_ERROR "lowgui: the Plan-V4D submodule is not checked out. "
      "Run 'git submodule update --init' before configuring (see build.sh).")
  endif()

  ocv_module_include_directories(
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    ${LOWGUI_GPU_INCLUDE_DIRS}
  )

  ocv_create_module()
  set_target_properties(${the_module} PROPERTIES LINKER_LANGUAGE CXX)

  target_compile_features(${the_module} PRIVATE cxx_std_20)

  if(BUILD_TESTS AND NOT BUILD_opencv_world)
    # An explicit source list both selects the accuracy tests and suppresses
    # ocv_add_accuracy_tests' recursive GLOB (which would otherwise re-pull the
    # test_offscreen sources into the main binary). OPENCV_TEST_<module>_SOURCES
    # is the extension point for that; passing FILES ... would eat the first
    # entry as a source-group label.
    set(OPENCV_TEST_lowgui_SOURCES
      test/test_main.cpp
      test/test_lowgui_api.cpp
      test/test_lowgui_input.cpp
      test/test_window_manager.cpp
    )
    ocv_add_accuracy_tests()
    if(TARGET opencv_test_lowgui)
      target_compile_features(opencv_test_lowgui PRIVATE cxx_std_20)
      ocv_target_include_directories(opencv_test_lowgui PRIVATE
        ${LOWGUI_GPU_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}/src")
    endif()
    add_subdirectory(test_offscreen)
  endif()

  if(NOT APPLE)
    ocv_target_link_libraries(${the_module} OpenGL GLU)
  endif()
endif()

if(BUILD_EXAMPLES AND NOT BUILD_opencv_world)
  add_binary_sample(example_lowgui_demo samples/lowgui_demo.cpp)
  ocv_target_link_libraries(example_lowgui_demo ${the_module})
endif()
