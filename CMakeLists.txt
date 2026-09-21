cmake_policy(SET CMP0079 NEW)

set(the_description "lowgui")

if(NOT (TARGET ${the_module}))
  ocv_add_module(lowgui opencv_core opencv_imgproc opencv_imgcodecs opencv_v4d opencv_plan)

  file(GLOB lowgui_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
  ocv_glob_module_sources(${lowgui_sources})

  ocv_module_include_directories(
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${OPENCV_EXTRA_MODULES_PATH}/v4d/third/imgui"
    "${OPENCV_EXTRA_MODULES_PATH}/v4d/third/imgui/backends/"
    "${OPENCV_EXTRA_MODULES_PATH}/v4d/third/nanovg/src/"
  )

  ocv_create_module()
  set_target_properties(${the_module} PROPERTIES LINKER_LANGUAGE CXX)

  target_compile_features(${the_module} PRIVATE cxx_std_20)

  if(NOT APPLE)
    ocv_target_link_libraries(${the_module} OpenGL GLU)
  endif()
endif()

if(BUILD_EXAMPLES AND NOT BUILD_opencv_world)
  add_binary_sample(example_lowgui_demo samples/lowgui_demo.cpp)
  ocv_target_link_libraries(example_lowgui_demo ${the_module})
endif()
