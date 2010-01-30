#!/bin/sh

INTERFACE=test-interface-mips.cc
TESTCORE=test-mips.cc
TESTCORE_O=$PWD/test-mips.o
PATH_TO_TEST=$PWD/src/mips
ARGS="-Wall -Werror -W -Wno-unused-parameter -Isrc -Wnon-virtual-dtor -pedantic -g -O0 -ansi -m32 -fno-rtti -fno-exceptions -fvisibility=hidden"
DEFINES="-DV8_TARGET_ARCH_MIPS -DENABLE_DISASSEMBLER -DDEBUG -DENABLE_DEBUGGER_SUPPORT -DV8_ENABLE_CHECKS -DENABLE_LOGGING_AND_PROFILING"
V8ARGS="-Iinclude libv8_g.a -lpthread"


echo "building v8 lib in debug mode"
scons simulator=mips mode=debug regexp=interpreted -j3
echo "g++ -c -o ${TESTCORE_O} ${PATH_TO_TEST}/${TESTCORE} ${ARGS} ${DEFINES}"
g++ -c -o ${TESTCORE_O} ${PATH_TO_TEST}/${TESTCORE} ${ARGS} ${DEFINES}
echo "g++  -o mips-test-interface ${PATH_TO_TEST}/${INTERFACE} ${TESTCORE_O} ${V8ARGS} ${ARGS}"
g++ ${PATH_TO_TEST}/${INTERFACE} -o mips-test-interface ${TESTCORE_O} ${V8ARGS} ${ARGS}
