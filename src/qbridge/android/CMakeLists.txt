cmake_minimum_required(VERSION 3.22)
project(qbridge C CXX)
set(CMAKE_CXX_STANDARD 17)

set(XR_AAR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr_aar)
add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES
    IMPORTED_LOCATION ${XR_AAR}/jni/${ANDROID_ABI}/libopenxr_loader.so)

add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(qbridge SHARED main.cpp)
target_include_directories(qbridge PRIVATE
    ${XR_AAR}/prefab/modules/headers/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../common)
# Keep ANativeActivity_onCreate exported from the static glue.
set_target_properties(qbridge PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate")
target_link_libraries(qbridge native_app_glue openxr_loader android log EGL GLESv3 nativewindow)
