#!/bin/bash
if [[ -z "${NO_BUILD}" ]]; then
	pushd emulator
	source build.sh
	popd
fi
