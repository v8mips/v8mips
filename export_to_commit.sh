#!/bin/sh

# We cannot have 2 concurrent versionning system int the v8 commit folder as
# gcl wouldn't know which to use.
# This script takes the local files and copy them to the $V8_COMMIT_PATH.

V8_COMMIT_PATH=../v8_commit
TRACKED_FILES='SConstruct compile_mips-test.sh compile_all.sh'
TRACKED_FILES_SRC='src/SConscript src/*.h src/*.cc src/*.c'
TRACKED_FILES_MIPS='src/mips/*.h src/mips/*.cc src/mips/readme'
TRACKED_FILES_MIPS_CCTEST=' test/cctest/SConscript test/cctest/test-regexp.cc test/cctest/test-assembler-mips.cc test/cctest/test-mips.cc'

echo "Copying files to ${V8_COMMIT_PATH}"
cp ${TRACKED_FILES} ${V8_COMMIT_PATH}
cp ${TRACKED_FILES_SRC} ${V8_COMMIT_PATH}/src
cp ${TRACKED_FILES_MIPS} ${V8_COMMIT_PATH}/src/mips
cp ${TRACKED_FILES_MIPS_CCTEST} ${V8_COMMIT_PATH}/test/cctest


# If we don't want to test in v8 commit repository replace architecture
# independent files with their unmodified version.
if [ $# -eq 0 ]; then
	echo "Replacing architecture indepent files with their default version."
	cp src/flag-definitions.h.orig ${V8_COMMIT_PATH}/src/flag-definitions.h
else
	if [ $1 != "test"  ]; then
		echo "Replacing architecture indepent files with their default version."
		cp src/flag-definitions.h.orig ${V8_COMMIT_PATH}/src/flag-definitions.h
	else
		echo "Warning: architecture independent files still have MIPS options."
	fi
fi
