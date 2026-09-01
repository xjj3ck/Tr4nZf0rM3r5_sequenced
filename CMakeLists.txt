cmake_minimum_required(VERSION 3.22)
project(tr4nzf0rm3r5_sequenced C)

# Defaults match the author's machine; override on yours:
#   cmake -S . -B build -DOBS_SRC=<obs-studio checkout> -DOBS_LIB=<obs.lib>
set(OBS_SRC "C:/obs-studio" CACHE PATH "Path to an obs-studio source checkout")
set(OBS_LIB "C:/obs-studio/build/libobs/RelWithDebInfo/obs.lib"
    CACHE FILEPATH "Path to obs.lib (Windows only)")

add_library(tr4nzf0rm3r5_sequenced MODULE tr4nzf0rm3r5_sequenced.c)
target_include_directories(tr4nzf0rm3r5_sequenced PRIVATE
	"${OBS_SRC}/libobs"
	"${CMAKE_CURRENT_SOURCE_DIR}/stub")

if(WIN32)
	target_link_libraries(tr4nzf0rm3r5_sequenced PRIVATE "${OBS_LIB}")
else()
	target_compile_options(tr4nzf0rm3r5_sequenced PRIVATE -fPIC)
	# on Linux, libobs symbols resolve when OBS loads the module
endif()

set_target_properties(tr4nzf0rm3r5_sequenced PROPERTIES PREFIX "")