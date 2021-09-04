#include <stdio.h>

void flockfile(FILE *filehandle) {
	(void)filehandle;
}
int ftrylockfile(FILE *filehandle) {
	(void)filehandle;
	return 0;
}
void funlockfile(FILE *filehandle) {
	(void)filehandle;
}

void crash() {
	void (*nowhere) () = (void(*)()) 0x0;
	nowhere();
}
