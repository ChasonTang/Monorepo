/* odin/main.c — entire file */
#include "cli.h"

#include <stdio.h>

int main(int argc, char **argv) {
  return odin_cli_main(argc, argv, stdout, stderr);
}
