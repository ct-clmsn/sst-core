# SST-CORE src/sst/core/profile CMake
#
# Copyright 2009-2022 NTESS. Under the terms
# of Contract DE-NA0003525 with NTESS, the U.S.
# Government retains certain rights in this software.
#
# Copyright (c) 2009-2022, NTESS
# All rights reserved.
#
# This file is part of the SST software package. For license
# information, see the LICENSE file in the top level directory of the
# distribution.
#

set(SSTProfileHeaders
	profiletool.h
	eventHandlerProfileTool.h
	clockHandlerProfileTool.h
	syncProfileTool.h)

add_library(
  profile OBJECT 
  eventHandlerProfileTool.cc
  clockHandlerProfileTool.cc
  syncProfileTool.cc
  profiletool.cc)


if(SST_ENABLE_HPX)
  if(HPX_FOUND)
    target_compile_definitions(profile PUBLIC SST_BUILDING_CORE=1 SST_ENABLE_HPX=1)
    hpx_setup_target(profile)
  else()
     message(FATAL_ERROR "SST_ENABLE_HPX configured but HPX not found!")
  endif()
else()
  target_compile_definitions(profile PUBLIC SST_BUILDING_CORE=1)
endif()

# NEED To double check this

target_include_directories(profile PRIVATE ${SST_TOP_SRC_DIR}/src)
target_link_libraries(profile PRIVATE sst-config-headers)
install(TARGETS profile DESTINATION lib/sstcore)
set_target_properties(profile PROPERTIES NO_SONAME 1)

if(APPLE)
  target_link_options(profile PRIVATE
                      "LINKER:-undefined,dynamic_lookup")
endif()

install(FILES ${SSTProfileHeaders} DESTINATION "include/sst/core/profile")

# EOF
