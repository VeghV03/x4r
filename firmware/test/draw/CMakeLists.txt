# The firmware's drawing on the Mac: the real GfxRenderer and font code over a panel held in
# memory and an SD card served from Mac folders (stubs/). HostGfx.h is the way in.
set(DRAW_SDK ${REPO_ROOT}/freeink-sdk/libs)

add_library(x4_host_gfx STATIC
  HostGfx.cpp
  HostStorage.cpp
  ${REPO_ROOT}/lib/GfxRenderer/GfxRenderer.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFontManager.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFontRegistry.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
  ${DRAW_SDK}/font/FreeInkFont/src/FontAlloc.c
)
# stubs/ comes first: it stands in for the hardware headers of lib/hal and the SDK.
target_include_directories(x4_host_gfx PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/hal
  ${DRAW_SDK}/font/FreeInkFont/include
  ${DRAW_SDK}/hardware/MemoryManager/include
)
target_link_libraries(x4_host_gfx PUBLIC crosspoint_test_common)

# A test card through that renderer: x4draw --fonts build/fonts --out card.pgm [--gray]
add_executable(x4draw x4draw.cpp)
target_link_libraries(x4draw PRIVATE x4_host_gfx)

# The paper's screens from a JSON view: x4paper --fonts build/fonts --out front.pgm < view.json
add_executable(x4paper x4paper.cpp ${REPO_ROOT}/src/learn/ui/PaperDraw.cpp)
target_link_libraries(x4paper PRIVATE x4_host_gfx)
target_include_directories(x4paper PRIVATE ${REPO_ROOT}/src ${REPO_ROOT}/src/learn
  ${REPO_ROOT}/.pio/libdeps/default/ArduinoJson/src)
