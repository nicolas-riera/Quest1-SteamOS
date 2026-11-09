# ksystemstats GPU plugin for the Quest 1's kgsl GPU (quest1_gpu.cpp). Its file name gives it the stock GPU
# plugin's id: installed in /usr/local/lib/qt6/plugins/ksystemstats, it replaces the stock one for a ksystemstats
# started with QT_PLUGIN_PATH=/usr/local/lib/qt6/plugins (device/holo-desktop.sh).
cmake_minimum_required(VERSION 3.20)
project(quest1_ksysgpu CXX)
set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(KF6CoreAddons REQUIRED)
find_package(KSysGuard REQUIRED)
add_library(ksystemstats_plugin_gpu MODULE quest1_gpu.cpp)
set_target_properties(ksystemstats_plugin_gpu PROPERTIES PREFIX "")
target_link_libraries(ksystemstats_plugin_gpu Qt6::Core KF6::CoreAddons KSysGuard::SystemStats)
install(TARGETS ksystemstats_plugin_gpu DESTINATION lib/qt6/plugins/ksystemstats)
