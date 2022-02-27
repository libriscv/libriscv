#!/bin/bash
THIS_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

build_libriscv() {
	WORK_DIR=`mktemp -d`
	# check if tmp dir was created
	if [[ ! "$WORK_DIR" || ! -d "$WORK_DIR" ]]; then
	  echo "Could not create temp dir"
	  exit 1
	fi

	pushd $WORK_DIR
	cmake $THIS_DIR $@
	VERBOSE=1 make -j6
	popd

	rm -rf "$WORK_DIR"
}

set -e

# 1. Normal build
build_libriscv
# 2. Debug build
build_libriscv -DRISCV_DEBUG=ON
# 3. Icache build
build_libriscv -DRISCV_ICACHE=ON
# 4. No extensions build
build_libriscv -DRISCV_EXT_A=OFF -DRISCV_EXT_C=OFF -DRISCV_EXT_F=OFF
# 5. A-extension only build
build_libriscv -DRISCV_EXT_A=ON -DRISCV_EXT_C=OFF -DRISCV_EXT_F=OFF
# 6. C-extension only build
build_libriscv -DRISCV_EXT_A=OFF -DRISCV_EXT_C=ON -DRISCV_EXT_F=OFF
# 7. F-extension only build
build_libriscv -DRISCV_EXT_A=OFF -DRISCV_EXT_C=OFF -DRISCV_EXT_F=ON
# 8. Experimental only build
build_libriscv -DRISCV_EXPERIMENTAL=ON -DRISCV_ICACHE=OFF
# 9. Experimental icache build
build_libriscv -DRISCV_EXPERIMENTAL=ON -DRISCV_ICACHE=ON
# 10. Experimental icache debug build
build_libriscv -DRISCV_EXPERIMENTAL=ON -DRISCV_ICACHE=ON -DRISCV_DEBUG=ON
# 11. Multiprocessing disabled build
build_libriscv -DRISCV_MULTIPROCESS=OFF
# 12. Multiprocessing disabled debug build
build_libriscv -DRISCV_MULTIPROCESS=OFF -DRISCV_DEBUG=ON
