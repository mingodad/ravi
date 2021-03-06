cmake_minimum_required(VERSION 2.8)
project(Ravi)

enable_language(CXX)
enable_language(C)
enable_testing()

# Get access to CMake helpers
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")

# By default LLVM JIT is ON, and GCC JIT is OFF
# Only one can be ON, and at last one of them must be ON
option(LLVM_JIT "Controls whether LLVM JIT compilation will be enabled" OFF)
option(GCC_JIT "Controls whether GCC JIT compilation will be enabled" OFF)

# We cannot link to both LLVM and GCC JIT
if (LLVM_JIT AND GCC_JIT)
    message(FATAL_ERROR
            "Both LLVM_JIT and GCC_JIT cannot be set to ON at the same time")
endif ()

if (LLVM_JIT)
    find_package(LLVM REQUIRED CONFIG)

    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
    message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

    include_directories(${LLVM_INCLUDE_DIRS})
    add_definitions(${LLVM_DEFINITIONS})
    add_definitions(-DUSE_LLVM)
endif ()

if (GCC_JIT)
    find_package(GCCJIT REQUIRED)

    message(STATUS "Found GCCJIT")

    include_directories(${GCCJIT_INCLUDE_DIRS})
    add_definitions(-DUSE_GCCJIT)
endif ()

if (NOT LLVM_JIT AND NOT GCC_JIT)
    message(WARNING "Neither LLVM nor gccjit will be enabled; specify -DLLVM_JIT or -DGCC_JIT to enable")
endif()

if (MSVC)
    set(CMAKE_C_FLAGS_DEBUG "/Od /D_DEBUG /MDd /Zi /RTC1 /EHsc")
    set(CMAKE_C_FLAGS_RELEASE "/DNDEBUG /O2 /MD /EHsc")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "/DNDEBUG /O2 /MD /Zi /EHsc")
    set(CMAKE_C_FLAGS_MINSIZEREL "/DNDEBUG /O2 /MD /EHsc")

    set(CMAKE_CXX_FLAGS "/EHsc")
    set(CMAKE_CXX_FLAGS_DEBUG "/Od /D_DEBUG /MDd /Zi /RTC1 /EHsc")
    set(CMAKE_CXX_FLAGS_RELEASE "/DNDEBUG /O2 /MD /EHsc")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/DNDEBUG /O2 /MD /Zi /EHsc")
    set(CMAKE_CXX_FLAGS_MINSIZEREL "/DNDEBUG /O2 /MD /EHsc")

    SET(CMAKE_EXE_LINKER_FLAGS_DEBUG "/DEBUG /STACK:10000000")
    SET(CMAKE_EXE_LINKER_FLAGS_RELEASE "/RELEASE /INCREMENTAL:NO /STACK:10000000")
    SET(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO "/DEBUG /RELEASE /INCREMENTAL:NO /STACK:10000000")
    SET(CMAKE_EXE_LINKER_FLAGS_MINSIZEREL "/RELEASE /INCREMENTAL:NO /STACK:10000000")

    SET(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
    SET(CMAKE_SHARED_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG}")
    SET(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE}")
    SET(CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO}")
    SET(CMAKE_SHARED_LINKER_FLAGS_MINSIZEREL "${CMAKE_EXE_LINKER_FLAGS_MINSIZEREL}")

    # Disable signed/unsigned int conversion warnings.
    add_definitions("/wd4018")
    # Disable warning about using struct/class for the same symobl.
    add_definitions("/wd4099")
    # Disable performance warning about int-to-bool conversion.
    add_definitions("/wd4800")
    # Disable signed/unsigned int conversion warnings.
    add_definitions("/wd4018")
    # Disable warning about the insecurity of using "std::copy".
    add_definitions("/wd4996")
    add_definitions("/wd4291")
    add_definitions("/wd4624")
endif ()


if (CMAKE_COMPILER_IS_GNUCXX AND NOT APPLE)
    if (NOT WIN32)
        # assume Linux
        set(CXX_OPTIONS -DLUA_USE_LINUX)
    endif ()
    # -fsanitize=bounds -fsanitize=alignment -fsanitize=object-size
    set(SANITIZER_FLAGS "-fsanitize=address")
    set(CMAKE_C_FLAGS_DEBUG "${SANITIZER_FLAGS} -fno-omit-frame-pointer -Werror -std=c99 -O0 -g3 -Wall -Wextra -DLUA_COMPAT_5_2 ${CXX_OPTIONS}")
    set(CMAKE_C_FLAGS_RELEASE "-Werror -std=c99 -O3 -Wall -Wextra -DLUA_COMPAT_5_2 ${CXX_OPTIONS}")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "${SANITIZER_FLAGS} -Werror -std=c99 -O1 -g3 -Wall -Wextra -DLUA_COMPAT_5_2 ${CXX_OPTIONS}")
    set(CMAKE_CXX_FLAGS_RELEASE "-Werror -fno-rtti -O3 -Wall -Wno-sign-compare -std=c++11 -fno-exceptions -DLUA_COMPAT_5_2 ${CXX_OPTIONS}")
    set(CMAKE_CXX_FLAGS_DEBUG "${SANITIZER_FLAGS} -fno-omit-frame-pointer -Werror -fno-rtti -O0 -g3 -Wall -Wno-sign-compare -std=c++11 -fno-exceptions -DLUA_COMPAT_5_2 ${CXX_OPTIONS}")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${SANITIZER_FLAGS} -Werror -fno-rtti -O1 -g3 -Wall -Wno-sign-compare -std=c++11 -fno-exceptions -DLUA_COMPAT_5_2 ${CXX_OPTIONS}")
    # In case we are using gcc 5.1 set ABI version
    # add_definitions("-D_GLIBCXX_USE_CXX11_ABI=0")
endif ()

if (APPLE OR (LLVM_JIT AND (NOT CMAKE_COMPILER_IS_GNUCXX) AND (NOT MSVC)))
    set(CMAKE_C_FLAGS "-std=c99 -O1 -g3 -Wall -Wextra -DLUA_COMPAT_5_2 -DLUA_USE_MACOSX")
    set(CMAKE_C_FLAGS_DEBUG "-std=c99 -O0 -g3 -Wall -Wextra -DLUA_COMPAT_5_2 -DLUA_USE_MACOSX")
    set(CMAKE_C_FLAGS_RELEASE "-std=c99 -O3 -Wall -Wextra -DLUA_COMPAT_5_2 -DLUA_USE_MACOSX")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "-std=c99 -O1 -g3 -Wall -Wextra -DLUA_COMPAT_5_2 -DLUA_USE_MACOSX")
    set(CMAKE_CXX_FLAGS "-O1 -g3 -Wall -fno-rtti -Wno-sign-compare -std=c++11 -fno-exceptions -DLUA_COMPAT_5_2 -DLUA_USE_MACOSX")
    set(CMAKE_CXX_FLAGS_RELEASE "-fno-rtti -O3 -Wall -Wno-sign-compare -std=c++11 -fno-exceptions -DLUA_COMPAT_5_2 -DLUA_USE_MACOSX")
    set(CMAKE_CXX_FLAGS_DEBUG "-fno-rtti -O0 -g3 -Wall -Wno-sign-compare -std=c++11 -fno-exceptions -DLUA_COMPAT_5_2 -DLUA_USE_MACOSX")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-fno-rtti -O1 -g3 -Wall -Wno-sign-compare -std=c++11 -fno-exceptions -DLUA_COMPAT_5_2 -DLUA_USE_MACOSX")
endif ()

include_directories("${PROJECT_SOURCE_DIR}/include")
add_definitions(-DLUA_COMPAT_MODULE)
if (CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "DEBUG")
    message(STATUS "Enabling Lua extended test harness 'ltests'")
    add_definitions(-DLUA_USER_H="ltests.h")
endif ()

# define LLVM JIT compiler sources
if (LLVM_JIT)
    set(LLVM_JIT_SRCS src/ravi_llvmjit.cpp src/ravi_llvmtypes.cpp
            src/ravi_llvmcodegen.cpp src/ravi_llvmforprep.cpp src/ravi_llvmcomp.cpp
            src/ravi_llvmreturn.cpp src/ravi_llvmload.cpp src/ravi_llvmforloop.cpp
            src/ravi_llvmarith1.cpp src/ravi_llvmcall.cpp src/ravi_llvmtable.cpp
            src/ravi_llvmarith2.cpp src/ravi_llvmtforcall.cpp src/ravi_llvmrest.cpp
            src/ravi_llvmluaapi.cpp)
endif ()
if (GCC_JIT)
    set(GCC_JIT_SRCS src/ravi_gccjit.c src/ravi_gcctypes.c
            src/ravi_gcccodegen.c src/ravi_gccforprep.c src/ravi_gcccomp.c
            src/ravi_gccreturn.c src/ravi_gccload.c src/ravi_gccforloop.c
            src/ravi_gccarith1.c src/ravi_gcccall.c src/ravi_gcctable.c
            src/ravi_gccarith2.c src/ravi_gcctforcall.c src/ravi_gccrest.c)
endif ()
if (NOT LLVM_JIT AND NOT GCC_JIT)
    set(NO_JIT_SRCS src/ravi_nojit.c)
endif()
# define the lua core source files
set(LUA_CORE_SRCS src/lapi.c src/lcode.c src/lctype.c src/ldebug.c src/ldo.c src/ldump.c
        src/lfunc.c src/lgc.c src/llex.c src/lmem.c src/lobject.c src/lopcodes.c
        src/lparser.c src/lstate.c src/lstring.c src/ltable.c src/ltm.c src/lundump.c
        src/lvm.c src/lzio.c src/ravijit.cpp src/ltests.c)
# define the lua lib source files
set(LUA_LIB_SRCS src/lauxlib.c src/lbaselib.c src/lbitlib.c src/lcorolib.c src/ldblib.c src/liolib.c
        src/lmathlib.c src/loslib.c src/ltablib.c src/lstrlib.c src/loadlib.c src/linit.c src/lutf8lib.c)

file(GLOB RAVI_HEADERS "${PROJECT_SOURCE_DIR}/include/*.h")
set(LUA_HEADERS include/lua.h include/luaconf.h include/lualib.h include/lauxlib.h)

if (MSVC OR APPLE)
    source_group("Ravi Headers" FILES ${RAVI_HEADERS})
    source_group("Ravi Source Files" FILES ${LUA_CORE_SRCS} ${LUA_LIB_SRCS}
            ${LLVM_JIT_SRCS} ${GCC_JIT_SRCS})
    if (APPLE)
        set(EXTRA_LIBRARIES m readline)
    endif ()
elseif (NOT WIN32)
    # On Linux we need to link libdl to get access to
    # functions like dlopen()
    # ubsan can be added to get -fsanitize=undefined
    set(EXTRA_LIBRARIES m dl readline)
endif ()

# Enable minimal required LLVM components so that the
# the size of the resulting binary is manageable
if (LLVM_JIT)
    if (${LLVM_PACKAGE_VERSION} VERSION_LESS "3.8")
        set(LLVM_EXTRA_LIBS ipa)
    endif()
    message(STATUS "SYSTEM_PROCESSOR ${CMAKE_SYSTEM_PROCESSOR}")
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
	set(LLVM_LIBS_PROCESSOR
		X86CodeGen
		X86AsmParser
		X86Disassembler
		X86AsmPrinter
		X86Desc
		X86Info
		X86Utils
	    )
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
	set(LLVM_LIBS_PROCESSOR
		ARMCodeGen
		ARMAsmParser
		ARMDisassembler
		ARMAsmPrinter
		ARMDesc
		ARMInfo
	    )
    endif()
    llvm_map_components_to_libnames(LLVM_LIBS
            Analysis
            Core
            CodeGen
            AsmParser
            AsmPrinter
            BitReader
            ExecutionEngine
            InstCombine
            ${LLVM_EXTRA_LIBS}
            ipo
            MC
            MCJIT
            MCParser
            Object
            RuntimeDyld
            ScalarOpts
            Support
            Target
            TransformUtils
	    ${LLVM_LIBS_PROCESSOR}
            )
    message(STATUS "LLVM_LIBS ${LLVM_LIBS}")
endif ()

#Main library
add_library(libravi SHARED 
        ${RAVI_HEADERS}
        ${LUA_LIB_SRCS} 
        ${LUA_CORE_SRCS}
        ${LLVM_JIT_SRCS}
        ${GCC_JIT_SRCS}
        ${NO_JIT_SRCS})
if (WIN32)
    # enable DLL export
    set_target_properties(libravi PROPERTIES DEFINE_SYMBOL "LUA_BUILD_AS_DLL")
else()
    set_target_properties(libravi PROPERTIES PREFIX "")
endif ()
target_link_libraries(libravi ${EXTRA_LIBRARIES} ${LLVM_LIBS} ${GCCJIT_LIBRARIES})

#Static library for the debugger 
if (NOT LLVM_JIT)
  add_library(libravistatic 
        ${RAVI_HEADERS}
        ${LUA_LIB_SRCS} 
        ${LUA_CORE_SRCS}
        ${LLVM_JIT_SRCS}
        ${GCC_JIT_SRCS}
        ${NO_JIT_SRCS})
  if (WIN32)
    # enable DLL export
    set_target_properties(libravistatic PROPERTIES DEFINE_SYMBOL "LUA_BUILD_AS_DLL")
  else()
    set_target_properties(libravistatic PROPERTIES PREFIX "")
  endif ()
  target_link_libraries(libravistatic ${EXTRA_LIBRARIES} ${LLVM_LIBS} ${GCCJIT_LIBRARIES})

  add_executable(testravidebug vscode-debugger/src/testravidebug.c vscode-debugger/src/json.c vscode-debugger/src/protocol.c)
  target_link_libraries(testravidebug libravistatic)

  add_test(TestRaviDebug testravidebug)

  # Ravi VSCode Debug adapter
  add_executable(ravidebug vscode-debugger/src/ravidebug.c vscode-debugger/src/json.c vscode-debugger/src/protocol.c)
  target_link_libraries(ravidebug libravistatic)
endif()

add_executable(ravi src/lua.c)
target_link_libraries(ravi libravi)

#VM test
add_executable(test_vm tests/test_vm.c)
target_link_libraries(test_vm libravi)

if (LLVM_JIT)
    #LLVM playground
    add_executable(test_llvm tests/test_llvm.cpp)
    target_link_libraries(test_llvm libravi)
endif ()

add_executable(test_misc tests/test_misc.c)

if (LLVM_JIT)
    add_test(TestLLVM test_llvm)
endif ()
add_test(TestVM test_vm)
add_test(TestMisc test_misc)

install(FILES ${LUA_HEADERS}
        DESTINATION include/ravi)
install(TARGETS libravi ravi
        RUNTIME DESTINATION bin
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib)
