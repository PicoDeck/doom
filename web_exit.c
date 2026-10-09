// The browser build's exit(): the Makefile's main.wasm rule compiles every
// file with -Dexit=dg_web_exit, so Doom's exit() calls (I_Quit, I_Error)
// arrive here and go back to picodeck_main, as stubs_newlib.c's _exit() does
// on the device. Emscripten's own exit() would end the whole page.
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

extern jmp_buf g_exit_jmp;

void dg_web_exit(int status) {
    fflush(stdout);
    longjmp(g_exit_jmp, status ? status : -1);
}
