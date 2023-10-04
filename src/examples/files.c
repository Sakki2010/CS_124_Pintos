/* files.c

   Simple program to test each file syscall in order, using
   hard-coded values, so as to not need command-line argparsing. */

#include <syscall.h>
#include <stdio.h>

int main (void)
{
    printf("create: %d\n", create("new-file", 10));
    int fd = open("new-file");
    printf("open: %d\n", fd);
    printf("filesize: %d\n", filesize(fd));
    printf("tell: %d\n", tell(fd));
    char *test = "test\n";
    printf("write: %d\n", write(fd, test, 5));
    seek(fd, 0);
    char buf[5];
    printf("read: %d\n", read(fd, buf, 5));
    printf("buf: %s\n", buf);

    printf("create: %d\n", create("rm-file", 0));
    int fd2 = open("rm-file");
    printf("open: %d\n", fd2);
    printf("remove: %d\n", remove("rm-file"));
    close(fd2);
    return 0;
}
