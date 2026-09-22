#-------------------------------------------------------------------
# This file is part of the CMake build system for OGRE-Next
#     (Object-oriented Graphics Rendering Engine)
# For the latest info, see http://www.ogre3d.org/
#
# The contents of this file are placed in the public domain. Feel
# free to make use of it in any way you like.
#-------------------------------------------------------------------

# Configure Vulkan RenderSystem build

file(GLOB HEADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/include/Vao/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/include/SPIRV-Reflect/*.h")
file(GLOB SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/Vao/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/SPIRV-Reflect/*.c")

# Each enabled backend APPENDS: more than one can be compiled in (the Interface
# config option picks at runtime), and a plain file( GLOB ... ) into the same
# variable would silently drop every earlier backend's sources.
if( OGRE_VULKAN_WINDOW_ANDROID )
	file( GLOB TMP_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/Windowing/Android/*.h" )
	file( GLOB TMP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/Windowing/Android/*.cpp" )
	list( APPEND PLATFORM_SPECIFIC_HEADER ${TMP_HEADER} )
	list( APPEND PLATFORM_SPECIFIC_SRC ${TMP_SRC} )
endif()
if( OGRE_VULKAN_WINDOW_XCB )
	file( GLOB TMP_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/Windowing/X11/*.h" )
	file( GLOB TMP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/Windowing/X11/*.cpp" )
	list( APPEND PLATFORM_SPECIFIC_HEADER ${TMP_HEADER} )
	list( APPEND PLATFORM_SPECIFIC_SRC ${TMP_SRC} )
endif()
if( OGRE_VULKAN_WINDOW_WIN32 )
	file( GLOB TMP_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/Windowing/win32/*.h" )
	file( GLOB TMP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/Windowing/win32/*.cpp" )
	list( APPEND PLATFORM_SPECIFIC_HEADER ${TMP_HEADER} )
	list( APPEND PLATFORM_SPECIFIC_SRC ${TMP_SRC} )
endif()
if( OGRE_VULKAN_WINDOW_METAL )
	file( GLOB TMP_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/Windowing/OSX/*.h" )
	file( GLOB TMP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/Windowing/OSX/*.mm" )
	list( APPEND PLATFORM_SPECIFIC_HEADER ${TMP_HEADER} )
	list( APPEND PLATFORM_SPECIFIC_SRC ${TMP_SRC} )
	set( METAL_SPECIFIC_SRC ${TMP_SRC} )
endif()
unset( TMP_HEADER )
unset( TMP_SRC )

set( HEADER_FILES ${HEADER_FILES} ${PLATFORM_SPECIFIC_HEADER} )
set( SOURCE_FILES ${SOURCE_FILES} ${PLATFORM_SPECIFIC_SRC} )

include_directories(
  BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/include
)

include_directories( ${Vulkan_INCLUDE_DIRS} )
if( OGRE_VULKAN_USE_SWAPPY )
	include_directories( ${AndroidSwappy_INCLUDE_DIRS} )
endif()

ogre_add_library(RenderSystem_Vulkan ${OGRE_LIB_TYPE} ${HEADER_FILES} ${SOURCE_FILES})
target_link_libraries(RenderSystem_Vulkan ${OGRE_NEXT}Main ${Vulkan_LIBRARIES})

# --- Jahshaka local patch -------------------------------------------------
# Debian/Ubuntu's libshaderc_combined.a is NOT actually combined: it *references*
# glslang (nm shows 'U glslang::InitializeProcess') instead of bundling it, unlike
# Google's upstream build. Without these the plugin links "successfully" (shared
# libs tolerate undefined symbols) and then dies at runtime with
#   symbol lookup error: ... undefined symbol: _ZN7glslang17InitializeProcessEv
# Debian also unbundles SPIRV-Tools from shaderc, so those are needed as well
# (spvtools::Optimizer). Order matters: all of these must come AFTER shaderc_combined.
if( UNIX AND NOT APPLE AND NOT ANDROID )
	find_library( JAHSHAKA_GLSLANG_LIB       glslang )
	find_library( JAHSHAKA_GLSLANG_SPIRV_LIB SPIRV )
	find_library( JAHSHAKA_GLSLANG_MI_LIB    MachineIndependent )
	find_library( JAHSHAKA_GLSLANG_GCG_LIB   GenericCodeGen )
	find_library( JAHSHAKA_GLSLANG_OSD_LIB   OSDependent )
	find_library( JAHSHAKA_SPIRV_TOOLS_OPT   SPIRV-Tools-opt )
	find_library( JAHSHAKA_SPIRV_TOOLS       SPIRV-Tools )
	if( JAHSHAKA_GLSLANG_LIB )
		target_link_libraries( RenderSystem_Vulkan
			${JAHSHAKA_GLSLANG_SPIRV_LIB} ${JAHSHAKA_GLSLANG_LIB}
			${JAHSHAKA_GLSLANG_MI_LIB} ${JAHSHAKA_GLSLANG_GCG_LIB}
			${JAHSHAKA_GLSLANG_OSD_LIB}
			${JAHSHAKA_SPIRV_TOOLS_OPT} ${JAHSHAKA_SPIRV_TOOLS} )
		message( STATUS "Jahshaka patch: linking system glslang into RenderSystem_Vulkan" )
	endif()
endif()
# --- end Jahshaka local patch ---------------------------------------------

if( OGRE_VULKAN_WINDOW_XCB )
	target_link_libraries(RenderSystem_Vulkan xcb-randr X11-xcb)
endif()

if( OGRE_VULKAN_WINDOW_METAL )
	# VK_EXT_metal_surface (VkMetalSurfaceCreateInfoEXT, vkCreateMetalSurfaceEXT)
	# only exists behind this define; the surface is created from a CAMetalLayer,
	# which OgreVulkanMetalWindow.mm hosts in an NSView. Vulkan itself is MoltenVK.
	target_compile_definitions(RenderSystem_Vulkan PRIVATE VK_USE_PLATFORM_METAL_EXT)
	# ARC on the ObjC++ sources only: the rest of the RenderSystem is plain C++.
	set_source_files_properties(${METAL_SPECIFIC_SRC} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
	target_link_libraries(RenderSystem_Vulkan "-framework QuartzCore" "-framework AppKit")
endif()

if( OGRE_VULKAN_USE_SWAPPY )
	target_link_libraries(RenderSystem_Vulkan ${AndroidSwappy_LIBRARY})
endif()

# --- Jahshaka local patch 0027: GPU timestamp sampling ----------------------
# OFF BY DEFAULT, and that is lock 1 of the two the owner asked for (D3,
# 2026-09-12): without this option the three GPU-profiling hooks compile to
# upstream's empty stubs byte for byte, the render system carries no extra
# members and no query pool type exists anywhere in the binary. A production or
# packaging build simply does not pass it. `build-ogre.sh` passes it for DEV
# builds (i.e. unless JAH_PRODUCTION=1). Lock 2 is at runtime: even with this
# on, no pool is created until a capture starts.
option(JAH_GPU_TIMESTAMPS "Jahshaka: fill Vulkan's GPU sample-profiling hooks with timestamp queries (dev builds only)" OFF)
if( JAH_GPU_TIMESTAMPS )
	target_compile_definitions(RenderSystem_Vulkan PRIVATE JAH_GPU_TIMESTAMPS)
endif()
# --- end Jahshaka local patch ----------------------------------------------

if (OGRE_CONFIG_THREADS)
  target_link_libraries(RenderSystem_Vulkan ${OGRE_THREAD_LIBRARIES})
endif ()

ogre_config_framework(RenderSystem_Vulkan)

ogre_config_plugin(RenderSystem_Vulkan)
install(FILES ${HEADER_FILES} DESTINATION include/${OGRE_NEXT_PREFIX}/RenderSystems/Vulkan)
install(FILES ${PLATFORM_HEADERS} DESTINATION include/${OGRE_NEXT_PREFIX}/RenderSystems/Vulkan/${PLATFORM_HEADER_INSTALL})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include DESTINATION include/${OGRE_NEXT_PREFIX}/RenderSystems/Vulkan)
