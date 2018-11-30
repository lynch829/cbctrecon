set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# Sorry, but apparently ITK cannot figure out how to download test data.
set(BUILD_TESTING ${BUILD_TESTING} CACHE BOOL "" FORCE)

set(ITK_BUILD_DEFAULT_MODULES OFF CACHE BOOL "" FORCE)
set(Module_ITKReview ON CACHE BOOL "" FORCE)
set(ITK_WRAPPING OFF CACHE BOOL "" FORCE)
# set(ITKV4_COMPATIBILITY ON CACHE BOOL "" FORCE)
set(Module_ITKVtkGlue OFF CACHE BOOL "" FORCE)

# DCMTK
set(ITK_USE_SYSTEM_DCMTK ON CACHE BOOL "" FORCE)
set(DCMTK_DIR "${DCMTK_BINARY_DIR}/../" CACHE PATH "" FORCE)

# ZLIB
set(ITK_USE_SYSTEM_ZLIB ON CACHE BOOL "" FORCE)

# RTK and DCMTK
# set(Module_ITKIODCMTK ON CACHE BOOL "" FORCE)
set(Module_RTK ON CACHE BOOL "" FORCE)
set(RTK_BUILD_APPLICATIONS OFF CACHE BOOL "")

# FFTW
set(ITK_USE_FFTWF ${FFTW_FOUND} CACHE BOOL "" FORCE)
set(ITK_USE_FFTWD ${FFTW_FOUND} CACHE BOOL "" FORCE)
set(ITK_USE_SYSTEM_FFTW ${FFTW_FOUND} CACHE BOOL "" FORCE)
set(FFTWD_LIB ${FFTW_LIBRARY_DOUBLE} CACHE FILEPATH "" FORCE)
set(FFTWF_LIB ${FFTW_LIBRARY_FLOAT} CACHE FILEPATH "" FORCE)
set(FFTWD_THREADS_LIB ${FFTW_THREADS_LIBRARY_DOUBLE} CACHE FILEPATH "" FORCE)
set(FFTWF_THREADS_LIB ${FFTW_THREADS_LIBRARY_FLOAT} CACHE FILEPATH "" FORCE)
set(FFTW_INCLUDE_DIR ${FFTW_INCLUDE_DIR} CACHE PATH "" FORCE)
set(FFTW_INCLUDE_PATH ${FFTW_INCLUDE_DIR} CACHE PATH "" FORCE)

# GPU
set(ITK_USE_GPU ON CACHE BOOL "" FORCE)
set(OPENCL_INCLUDE_DIRS ${OpenCL_INCLUDE_DIR} CACHE PATH "" FORCE)
set(OPENCL_LIBRARIES ${OpenCL_LIBRARY} CACHE FILEPATH "" FORCE)

# CUDA
set(CUDA_HAVE_GPU ${USE_CUDA} CACHE BOOL "" FORCE)
set(CUDA_SEPARABLE_COMPILATION OFF CACHE BOOL "" FORCE)

