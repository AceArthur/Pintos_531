#include <stdio.h>
#include <syscall.h>
#include <string.h>


int
main (int argc, char **argv)
{
  int i; 
//  printf("Hello\n");
//  printf("World\n");
//printf("argc - %x\n",&argc);
//printf("argv = %x\n",&argv);
  for (i = 0; i < argc; i++) 
   printf ("%s ", argv[i]);
  printf ("\n");

  return EXIT_SUCCESS;
}
