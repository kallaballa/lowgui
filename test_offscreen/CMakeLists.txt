set(offscreen_srcs test_offscreen.cpp main.cpp)
ocv_add_executable(opencv_test_lowgui_offscreen ${offscreen_srcs})
# Same dependency expansion as ocv_add_accuracy_tests so all transitive
# module include dirs are available (ts/core/imgproc/highgui/...).
set(offscreen_deps
    opencv_ts ${the_module} opencv_imgcodecs opencv_videoio
    ${OPENCV_MODULE_${the_module}_DEPS} ${OPENCV_MODULE_opencv_ts_DEPS})
ocv_target_include_modules(opencv_test_lowgui_offscreen ${offscreen_deps})
# v4d.hpp pulls in NanoVG/imgui headers transitively (same dirs the module and
# the main accuracy target use).
ocv_target_include_directories(opencv_test_lowgui_offscreen PRIVATE ${LOWGUI_GPU_INCLUDE_DIRS})
ocv_target_link_libraries(opencv_test_lowgui_offscreen PRIVATE
    ${offscreen_deps} ${OPENCV_LINKER_LIBS})
set_target_properties(opencv_test_lowgui_offscreen PROPERTIES
    DEBUG_POSTFIX "${OPENCV_DEBUG_POSTFIX}"
    RUNTIME_OUTPUT_DIRECTORY "${EXECUTABLE_OUTPUT_PATH}"
)
ocv_target_include_directories(opencv_test_lowgui_offscreen PRIVATE ${LOWGUI_GPU_INCLUDE_DIRS})
ocv_append_target_property(opencv_test_lowgui_offscreen COMPILE_DEFINITIONS "__OPENCV_TESTS=1")