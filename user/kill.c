#include <inc/lib.h>
#include <inc/string.h>

void
usage(void) {
    printf("usage: kill signum pid [sigarg]\n");
    exit();
}

void
umain(int argc, char **argv) {
    if (argc < 3 || argc > 4)
        usage();

    long signum = -1;
    long pid = -1;
    signum = strtol(argv[1], NULL, 10);
    pid = strtol(argv[2], NULL, 0);
    long sigarg = 0;
    
    if (argc == 4) {
        sigarg = strtol(argv[3], NULL, 10);
    }

    if (pid < 0 || signum <= 0) {
        usage();
    }

    union sigval sv;
    sv.sival_int = sigarg;
    int err = sigqueue(pid, signum, sv);
    
    if (err) {
        printf("kill: failed to send signal: %d\n", err);
    }

    sys_yield();
}