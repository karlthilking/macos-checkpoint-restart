/* ckpt.c */
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <spawn.h>
#include <dlfcn.h>
#include <assert.h>
#include <mach-o/dyld.h>
#include <sys/param.h>
#include "types.h"

#ifndef POSIX_SPAWN_DISABLE_ASLR
# define POSIX_SPAWN_DISABLE_ASLR 0x0100
#endif

void getpath(const char *file, char *out)
{
        char    buf[PATH_MAX], path[PATH_MAX];
        u32     bufsize = PATH_MAX;
        
        if (_NSGetExecutablePath(buf, &bufsize) < 0)
                err(EXIT_FAILURE, "_NSGetExecutablePath");
        else if (realpath(buf, path) == NULL)
                err(EXIT_FAILURE, "realpath(%s, ...)", buf);

        strncpy(out, path, strlen(path) + 1);
        strncpy(strstr(strstr(out, "macos"), "ckpt"),
                file, strlen(file) + 1);
}

void print(char *ckptfile)
{
        char *argv[3], printckpt_path[PATH_MAX];

        getpath("printckpt", printckpt_path);
        
        argv[0] = printckpt_path;
        argv[1] = ckptfile;
        argv[2] = NULL;

        if (execvp(argv[0], argv) < 0)
                err(EXIT_FAILURE, "execvp(%s, ...)", argv[0]);
}

void checkpoint(char **argv)
{
        char libckpt_path[PATH_MAX];

        getpath("libckpt.dylib", libckpt_path);
        printf("Executing %s (pid=%d)\n", argv[0], getpid());

        if (setenv("DYLD_INSERT_LIBRARIES", libckpt_path, 1) < 0)
                err(EXIT_FAILURE, "setenv");
        else if (execvp(argv[0], argv) < 0)
                err(EXIT_FAILURE, "execvp");
}

void restart(char *ckptfile)
{
        int                     retval;
        short                   flags;
        pid_t                   pid;
        extern char             **environ;
        posix_spawnattr_t       attr;
        char                    restart_path[PATH_MAX], *argv[3];

        getpath("restart", restart_path);
        posix_spawnattr_init(&attr);

        argv[0] = restart_path;
        argv[1] = ckptfile;
        argv[2] = NULL;

        /**
         * Disable address space layout randomization so fixed 
         * restart text and data segments are not slid
         */
        flags = POSIX_SPAWN_DISABLE_ASLR | POSIX_SPAWN_SETEXEC;
        if (posix_spawnattr_setflags(&attr, flags) < 0)
                err(EXIT_FAILURE, "posix_spawnattr_setflags");
        
        retval = posix_spawn(&pid, restart_path, NULL,
                             &attr, argv, environ);
        if (retval < 0) {
                posix_spawnattr_destroy(&attr);
                err(EXIT_FAILURE, "posix_spawn");
        }
}

__attribute__((noreturn))
void usage()
{
        fprintf(stderr,
"OVERVIEW: MacOS Checkpoint-Restart\n\n"
"USAGE: ./ckpt [options] file...\n\n"
"OPTIONS:\n"
"  -p <file>            Print checkpoint file contents\n"
"  -c <binary> <args>   Execute binary injected with libckpt.dylib\n"
"  -r <file>            Restart from saved checkpoint file\n\n");

        exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
        if (argc < 3)
                usage();
        
        switch (getopt(argc, argv, "c:r:p:")) {
        case 'c':
                checkpoint(&argv[optind - 1]);
        case 'r':
                restart(optarg);
        case 'p':
                print(optarg);
        case '?':
        default:
                usage();
        }

        return 0;
}
