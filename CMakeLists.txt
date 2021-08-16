project(obs-screenshot-filter-jpeg)
set(PROJECT_FULL_NAME "Screenshot filter")

find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil avformat swscale swresample)
include_directories(${FFMPEG_INCLUDE_DIRS})

set(obs-screenshot-filter-jpeg_SOURCES
      screenshot-filter.c
)

add_library(obs-screenshot-filter-jpeg MODULE
      ${obs-screenshot-filter-jpeg_SOURCES})
if(WIN32)
target_link_libraries(obs-screenshot-filter-jpeg
      libobs
      obs-frontend-api
      wininet.lib
      ${obs-ffmpeg_PLATFORM_DEPS}
      ${FFMPEG_LIBRARIES}
)
else()
target_link_libraries(obs-screenshot-filter-jpeg
      libobs
      ${obs-ffmpeg_PLATFORM_DEPS}
      ${FFMPEG_LIBRARIES}
)
endif()

if(UNIX AND NOT APPLE)
	install_obs_plugin(obs-screenshot-filter-jpeg)
endif()

set_target_properties(obs-screenshot-filter-jpeg PROPERTIES FOLDER "plugins")
