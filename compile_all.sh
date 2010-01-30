#!/bin/bash

echo "########## Building and running tests for MIPS on x86..."

if [ $# -ne 0 ]; then
	if [ $1 == debug ]; then
		# This compile with debug mode.
		echo "########## Building custom mips tests"
		./compile_mips-test.sh
		if [ $? != 0 ]; then
			echo "Error building v8."
			exit
		fi
	fi
fi

echo "########## Building v8"
scons simulator=mips regexp=interpreted -j3
if [ $? != 0 ]; then
	echo "Error building v8."
	exit
fi

echo "########## Building shell"
scons simulator=mips regexp=interpreted sample=shell -j3
if [ $? != 0 ]; then
	echo "Error building shell."
	exit
fi

echo "########## Building d8"
scons simulator=mips regexp=interpreted d8 -j3
if [ $? != 0 ]; then
	echo "Error building d8."
	exit
fi

echo "########## Building cctests"
scons simulator=mips regexp=interpreted cctests -j3
if [ $? != 0 ]; then
	echo "Error building cctests."
	exit
fi

echo "########## Starting test-assembler-mips"
tools/test.py --simulator=mips -S regexp=interpreted cctest/test-assembler-mips
if [ $? != 0 ]; then
	echo "Error running test-assembler-mips."
	exit
fi

echo "########## Everything was built and tested successfully"
