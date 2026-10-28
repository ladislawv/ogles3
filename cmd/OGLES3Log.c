/*
 * OGLES3Log: write ogles3.library's buffered log to its file now (plan
 * section 10). The file is T:ogles2.log, or the path in ENV:OGLES3/LogFile.
 * Useful while a program that uses the library is still running.
 *   usage: OGLES3Log
 */
#include <stdio.h>
#include <string.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <libraries/ogles3.h>

extern ULONG OGLES3PrivateLogFlushLVO(void); /* libogles3.a: vector -36 */

int main(void)
{
    char path[256];

    /* Any version: this only needs the log vector. */
    OGLES3Base = OpenLibrary((CONST_STRPTR)OGLES3NAME, 1);
    if (!OGLES3Base) {
        printf("OGLES3Log: cannot open %s\n", OGLES3NAME);
        return 10;
    }
    if (!OGLES3PrivateLogFlushLVO()) {
        printf("OGLES3Log: the library could not write its log\n");
        CloseLibrary(OGLES3Base);
        return 10;
    }
    CloseLibrary(OGLES3Base);
    if (GetVar((STRPTR) "OGLES3/LogFile", (STRPTR)path, sizeof(path), GVF_GLOBAL_ONLY) <= 0)
        strcpy(path, "T:ogles2.log");
    printf("OGLES3Log: log appended to %s\n", path);
    return 0;
}
