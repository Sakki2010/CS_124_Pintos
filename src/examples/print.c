/* print.c

   Simple program to test whether printing works. */
#include <syscall.h>
#include <stdio.h>

int main (void)
{
    printf("test\n");
    exit(2);
}
