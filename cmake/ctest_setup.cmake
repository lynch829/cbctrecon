include(CTest)

##### EXTERNAL DATA #####
get_filename_component(_CBCTRECONExternalData_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
include(ExternalData)

if(NOT ExternalData_OBJECT_STORES)
  # Use ExternalData_OBJECT_STORES from environment as default.
  set(ExternalData_OBJECT_STORES_DEFAULT "")
  if(DEFINED "ENV{ExternalData_OBJECT_STORES}")
    file(TO_CMAKE_PATH "$ENV{ExternalData_OBJECT_STORES}" ExternalData_OBJECT_STORES_DEFAULT)
  endif()
endif()

set(ExternalData_OBJECT_STORES "${ExternalData_OBJECT_STORES_DEFAULT}" CACHE STRING
  "Semicolon-separated list of local directories holding data objects in the layout %(algo)/%(hash).")
mark_as_advanced(ExternalData_OBJECT_STORES)
if(NOT ExternalData_OBJECT_STORES)
  set(ExternalData_OBJECT_STORES "${CMAKE_BINARY_DIR}/ExternalData/Objects")
  file(MAKE_DIRECTORY "${ExternalData_OBJECT_STORES}")
endif()
list(APPEND ExternalData_OBJECT_STORES
  # Local data store populated by the ITK pre-commit hook
  "${CMAKE_SOURCE_DIR}/.ExternalData"
  )

set(ExternalData_BINARY_ROOT ${CMAKE_BINARY_DIR}/ExternalData)

set(ExternalData_URL_ALGO_MD5_lower md5)
set(ExternalData_URL_TEMPLATES
  # Data published on Girder
  "https://data.kitware.com/api/v1/file/hashsum/%(algo)/%(hash)/download"
  )

set(ExternalData_LINK_CONTENT MD5)
set(ExternalData_NO_SYMLINKS 1)
##### EXTERNAL DATA END #####


function(add_cbctrecon_test)
  cmake_parse_arguments(
    ARGS
    ""
    "TARGET"
    "SRC_FILES;DATA_ARGS"
    ${ARGN}
    )

  add_executable(${ARGS_TARGET} ${ARGS_SRC_FILES})
  target_link_libraries(${ARGS_TARGET}
    PRIVATE CbctReconLib
    )

  target_include_directories(${ARGS_TARGET}
    PUBLIC ${CBCTRECON_INCLUDE_DIRS}
    PUBLIC ${CMAKE_CURRENT_BINARY_DIR}
    )

  find_program(MEMORYCHECK_COMMAND valgrind)

  if(MEMORYCHECK_COMMAND)
    message(STATUS "Running CTest ${ARGS_TARGET} with valgrind")
    set(VG_COMM ${MEMORYCHECK_COMMAND} "--leak-check=full") 
  endif()
  
  ExternalData_Add_test(CbctData
    NAME ${ARGS_TARGET}
    COMMAND ${VG_COMM} $<TARGET_FILE:${ARGS_TARGET}> ${ARGS_DATA_ARGS}
	WORKING_DIRECTORY bin # Requires install to be build
    )

endfunction()

enable_testing()
