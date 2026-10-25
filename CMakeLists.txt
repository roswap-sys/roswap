# 顶层构建入口。内核模块仍由 Kbuild 编译，这里负责调用它。

cmake_minimum_required(VERSION 3.16)
project(RoSwap LANGUAGES C)

set(OFED_DIR "/usr/src/ofa_kernel/default" CACHE PATH "MLNX_OFED 内核头目录")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  execute_process(
    COMMAND uname -r
    OUTPUT_VARIABLE KERNEL_RELEASE
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()
set(KDIR "/lib/modules/${KERNEL_RELEASE}/build" CACHE PATH "内核构建目录")
set(ROSWAP_BIN ${CMAKE_SOURCE_DIR}/bin)

find_program(GO_EXECUTABLE go REQUIRED)
find_program(GNU_MAKE make REQUIRED)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${ROSWAP_BIN})

enable_testing()
add_test(
  NAME unit
  COMMAND ${GO_EXECUTABLE} test ./...
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_custom_target(roswap-cli ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ROSWAP_BIN}
  COMMAND ${GO_EXECUTABLE} build -o ${ROSWAP_BIN}/roswap ./tools/cmd/roswap
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL
  COMMENT "构建 bin/roswap"
)

add_custom_target(probe
  COMMAND ${ROSWAP_BIN}/roswap probe
          --ofed ${OFED_DIR}
          --kdir ${KDIR}
          --out ${CMAKE_SOURCE_DIR}/kernel/roswap/include/rs_compat.h
  DEPENDS roswap-cli
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL
)

add_custom_target(check
  COMMAND ${GO_EXECUTABLE} test ./...
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_custom_target(kmod
    COMMAND ${CMAKE_COMMAND} -E env OFED_DIR=${OFED_DIR}
            ${GNU_MAKE} -C ${KDIR} M=${CMAKE_SOURCE_DIR}/kernel/roswap
            OFED_DIR=${OFED_DIR} modules
    DEPENDS probe
    USES_TERMINAL
  )
  add_custom_target(agent
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ROSWAP_BIN}
    COMMAND ${GO_EXECUTABLE} build -o ${ROSWAP_BIN}/roswap-agent ./cmd/roswap-agent
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
  )
else()
  foreach(tgt kmod agent)
    add_custom_target(${tgt}
      COMMAND ${CMAKE_COMMAND} -E echo
              "${tgt} 需要在 Linux 目标机上构建（当前 ${CMAKE_SYSTEM_NAME}）"
      COMMAND ${CMAKE_COMMAND} -E false
    )
  endforeach()
endif()
