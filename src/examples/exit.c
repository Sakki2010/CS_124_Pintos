/* exit.c

   Simple program to test whether running a system call works.
 	
   Just invokes a system call that shuts down the OS. */

#include <syscall.h>

int main (void)
{
    exit (3);
    /* not reached */
}
