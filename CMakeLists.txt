cmake_minimum_required( VERSION 3.5 )
project( xrootd-multiuser-posc CXX )

set( CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake )

find_package( Xrootd REQUIRED )
find_package( Cap REQUIRED )

if(NOT XROOTD_PLUGIN_VERSION)
  find_program(XROOTD_CONFIG_EXECUTABLE xrootd-config)
  execute_process(COMMAND ${XROOTD_CONFIG_EXECUTABLE} --plugin-version
                  OUTPUT_VARIABLE XROOTD_PLUGIN_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(XROOTD_PLUGIN_VERSION ${XROOTD_PLUGIN_VERSION} CACHE INTERNAL "")
endif()

set( CMAKE_CXX_STANDARD 11 )
if( CMAKE_COMPILER_IS_GNUCXX )
  set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Werror" )
endif()
set( CMAKE_SHARED_LINKER_FLAGS "-Wl,--no-undefined" )

include_directories( ${XROOTD_INCLUDES} )

add_library( XrdMultiuserPosc SHARED src/XrdMultiuserPosc.cc )
target_link_libraries( XrdMultiuserPosc ${CAP_LIB} ${XROOTD_UTILS_LIB} ${XROOTD_SERVER_LIB} )
set_target_properties( XrdMultiuserPosc PROPERTIES OUTPUT_NAME "XrdMultiuserPosc-${XROOTD_PLUGIN_VERSION}" SUFFIX ".so"
                       LINK_FLAGS "-Wl,--version-script=${CMAKE_SOURCE_DIR}/configs/export-lib-symbols" )

set( LIB_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/lib64" CACHE PATH "Install path for libraries" )
install( TARGETS XrdMultiuserPosc LIBRARY DESTINATION ${LIB_INSTALL_DIR} )
