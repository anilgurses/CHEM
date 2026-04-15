
include(CheckCXXCompilerFlag)


########################################################################
# Check for SIMD headers
########################################################################

# Check for SSE3 support
check_cxx_compiler_flag("-msse3" HAVE_SSE)
if(HAVE_SSE)
    message(STATUS "SSE is supported")
endif(HAVE_SSE)

# Check for AVX2 support
check_cxx_compiler_flag("-mavx2" HAVE_AVX2)
if(HAVE_AVX2)
    message(STATUS "AVX2 is supported")
    set(HAVE_SSE OFF)
endif(HAVE_AVX2)

# Check for AVX512 support
check_cxx_compiler_flag("-mavx512" HAVE_AVX512)
if(HAVE_AVX512)
    message(STATUS "AVX512 is supported")
    set(HAVE_AVX2 OFF)
endif(HAVE_AVX512)

# Check for AVX512 support
check_cxx_compiler_flag("-mfma" HAVE_FMA)
if(HAVE_FMA)
    message(STATUS "FMA is supported")
    set(HAVE_FMA OFF)
endif(HAVE_FMA)

mark_as_advanced(HAVE_SSE, HAVE_AVX2, HAVE_AVX512, HAVE_FMA)
