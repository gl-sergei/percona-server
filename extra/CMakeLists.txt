# Copyright (c) 2006, 2019, Oracle and/or its affiliates. All rights reserved.
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is also distributed with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have included with MySQL.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA

INCLUDE_DIRECTORIES(
${CMAKE_SOURCE_DIR}/include 
# Following is for perror, in case NDB is compiled in.
${CMAKE_SOURCE_DIR}/storage/ndb/include
${CMAKE_SOURCE_DIR}/storage/ndb/include/util  
${CMAKE_SOURCE_DIR}/storage/ndb/include/ndbapi
${CMAKE_SOURCE_DIR}/storage/ndb/include/portlib
${CMAKE_SOURCE_DIR}/storage/ndb/include/mgmapi)

# Default install component for the files is Server here
SET(MYSQL_INSTALL_COMPONENT Server)

IF(NOT CMAKE_CROSSCOMPILING)
 ADD_EXECUTABLE(comp_err comp_err.c)
 TARGET_LINK_LIBRARIES(comp_err mysys mysys_ssl)
 SET_TARGET_PROPERTIES(comp_err PROPERTIES LINKER_LANGUAGE CXX)
ENDIF()


ADD_CUSTOM_COMMAND(OUTPUT ${PROJECT_BINARY_DIR}/include/mysqld_error.h 
                   ${PROJECT_BINARY_DIR}/sql/share/english/errmsg.sys
                   COMMAND comp_err
                    --charset=${PROJECT_SOURCE_DIR}/sql/share/charsets 
                    --out-dir=${PROJECT_BINARY_DIR}/sql/share/
                    --header_file=${PROJECT_BINARY_DIR}/include/mysqld_error.h 
                    --name_file=${PROJECT_BINARY_DIR}/include/mysqld_ername.h
                    --state_file=${PROJECT_BINARY_DIR}/include/sql_state.h
                    --in_file=${PROJECT_SOURCE_DIR}/sql/share/errmsg-utf8.txt
                   DEPENDS ${PROJECT_SOURCE_DIR}/sql/share/errmsg-utf8.txt
                   ${CMAKE_CURRENT_SOURCE_DIR}/comp_err.c)

ADD_CUSTOM_TARGET(GenError
                  ALL
                  DEPENDS 
                  ${PROJECT_BINARY_DIR}/include/mysqld_error.h
                  ${PROJECT_BINARY_DIR}/sql/share/english/errmsg.sys 
                  ${PROJECT_SOURCE_DIR}/sql/share/errmsg-utf8.txt)

MYSQL_ADD_EXECUTABLE(my_print_defaults my_print_defaults.c)
TARGET_LINK_LIBRARIES(my_print_defaults mysys mysys_ssl)
SET_TARGET_PROPERTIES(my_print_defaults PROPERTIES LINKER_LANGUAGE CXX)

MYSQL_ADD_EXECUTABLE(perror perror.c)
ADD_DEPENDENCIES(perror GenError)
TARGET_LINK_LIBRARIES(perror mysys mysys_ssl)
SET_TARGET_PROPERTIES(perror PROPERTIES LINKER_LANGUAGE CXX)

MYSQL_ADD_EXECUTABLE(resolveip resolveip.c)
TARGET_LINK_LIBRARIES(resolveip mysys mysys_ssl)
SET_TARGET_PROPERTIES(resolveip PROPERTIES LINKER_LANGUAGE CXX)
IF(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
  INCLUDE(CheckFunctionExists)
  INCLUDE(CheckLibraryExists)
  MY_SEARCH_LIBS(inet_aton  "nsl;socket;resolv" SOLARIS_NSL)
  TARGET_LINK_LIBRARIES(resolveip ${SOLARIS_NSL})
ENDIF()


MYSQL_ADD_EXECUTABLE(replace replace.c)
TARGET_LINK_LIBRARIES(replace mysys)

IF(WITH_INNOBASE_STORAGE_ENGINE)
  # Add path to the InnoDB headers
  INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/storage/innobase/include)
  # We use the InnoDB code directly in case the code changes.
  ADD_DEFINITIONS("-DUNIV_INNOCHECKSUM")
  SET(INNOBASE_SOURCES
      ../storage/innobase/buf/buf0checksum.cc
      ../storage/innobase/ut/ut0crc32.cc
      ../storage/innobase/ut/ut0ut.cc
      ../storage/innobase/page/page0page.cc
      ../storage/innobase/page/page0zip.cc
     )
  MYSQL_ADD_EXECUTABLE(innochecksum innochecksum.cc ${INNOBASE_SOURCES})
  TARGET_LINK_LIBRARIES(innochecksum mysys mysys_ssl)
  ADD_DEPENDENCIES(innochecksum GenError)
ENDIF()

IF(UNIX)
  MYSQL_ADD_EXECUTABLE(resolve_stack_dump resolve_stack_dump.cc)
  TARGET_LINK_LIBRARIES(resolve_stack_dump mysys mysys_ssl)

  MYSQL_ADD_EXECUTABLE(mysql_waitpid mysql_waitpid.c)
  TARGET_LINK_LIBRARIES(mysql_waitpid mysys mysys_ssl)
  SET_TARGET_PROPERTIES(mysql_waitpid PROPERTIES LINKER_LANGUAGE CXX)
ENDIF()

