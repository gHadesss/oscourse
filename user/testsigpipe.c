#include <inc/lib.h>

int flag_p;
int flag_c;

void 
handler_for_sigpipe(int signo) {
    cprintf("child received SIGPIPE\n");
    flag_c = 0;
}

void 
handler_for_parent(int signo) {
    cprintf("Parent received signal (%d)\n", signo);
    flag_p = 0;
}

void
umain(int argc, char **argv) {
    char buf[128];
    char path_fifo[] = "my_fifo";
    char test_string[] = "This is a message for testing FIFO file";
    int test_len = strlen(test_string);
    int fifo_read_fd, fifo_write_fd;

    cprintf("mkfifo result = %d\n", mkfifo(path_fifo));

    struct sigaction sa1;
    sa1.sa_handler = &handler_for_parent;
    sa1.sa_flags = 0;
    sa1.sa_mask = 0;
    sigaction(SIGUSR1, &sa1, NULL);

    int child;

    if (!(child = fork())) {
        struct sigaction sa2;
        sa2.sa_handler = &handler_for_sigpipe;
        sa2.sa_flags = 0;
        sa2.sa_mask = 0;
        sigaction(SIGPIPE, &sa2, NULL);

        fifo_write_fd = open(path_fifo, O_WRONLY);
        cprintf("fifo_write_fd = %d\n", fifo_write_fd);

        cprintf("Child writes to channel, result: %ld\n", write(fifo_write_fd, test_string, test_len));

        cprintf("Child is ready for SIGPIPE\n");
        sigqueue(thisenv->env_parent_id, SIGUSR1, (const union sigval)0);

        wait(thisenv->env_parent_id);

        cprintf("Child writes to channel, result: %ld\n",  write(fifo_write_fd, test_string, test_len));

        close(fifo_write_fd);
        cprintf("Child ends\n");
    } else {
        fifo_read_fd = open(path_fifo, O_RDONLY);
        cprintf("fifo_read_fd = %d\n", fifo_read_fd);

        flag_p = 1;

        while (flag_p) {
            cprintf("Parent is waiting\n");
        }

        cprintf("Parent read buf from fifo, result %ld:", read(fifo_read_fd, buf, 8));
        cprintf("   %s\n", buf);

        cprintf("Parent is closing fifo\n");
        close(fifo_read_fd);

        cprintf("Parent ends\n");
    }
}
