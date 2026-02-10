set(ADAPTIVE_SOURCES adaptive_runtime.cpp)

# Define supported architectures for adaptive runtime
# Start with most common architecture, can be expanded later
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(ADAPTIVE_SUPPORTED_ARCH x86_64)
else()
  set(ADAPTIVE_SUPPORTED_ARCH i386)
endif()

# Use minimal flags for C++ compilation (adaptive_runtime.cpp needs C++ stdlib)
set(ADAPTIVE_CFLAGS)
append_list_if(COMPILER_RT_HAS_FPIC -fPIC ADAPTIVE_CFLAGS)
append_list_if(COMPILER_RT_HAS_FNO_RTTI -fno-rtti ADAPTIVE_CFLAGS)
append_list_if(COMPILER_RT_HAS_FNO_EXCEPTIONS -fno-exceptions ADAPTIVE_CFLAGS)

set(ADAPTIVE_LINK_LIBS)
append_list_if(COMPILER_RT_HAS_LIBPTHREAD pthread ADAPTIVE_LINK_LIBS)

# Build static library
add_compiler_rt_runtime(clang_rt.adaptive
  STATIC
  ARCHS ${ADAPTIVE_SUPPORTED_ARCH}
  SOURCES ${ADAPTIVE_SOURCES}
  CFLAGS ${ADAPTIVE_CFLAGS}
  LINK_LIBS ${ADAPTIVE_LINK_LIBS}
  PARENT_TARGET adaptive)

# Build shared library
add_compiler_rt_runtime(clang_rt.adaptive
  SHARED
  ARCHS ${ADAPTIVE_SUPPORTED_ARCH}
  SOURCES ${ADAPTIVE_SOURCES}
  CFLAGS ${ADAPTIVE_CFLAGS}
  LINK_LIBS ${ADAPTIVE_LINK_LIBS}
  PARENT_TARGET adaptive)