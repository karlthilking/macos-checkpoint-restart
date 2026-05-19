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

#ifndef POSIX_SPAWN_DISABLE_ASLR
#define POSIX_SPAWN_DISABLE_ASLR 0x0100
#endif

__attribute__((noreturn))
void print(char *ckptfile)
{
        if (execl("./printckpt", "./printckpt", ckptfile, NULL) < 0) {
                fprintf(stderr, "%s: execl: %s\n", 
                        __FILE__, strerror(errno));
        }
        
        exit(EXIT_FAILURE);
}

__attribute__((noreturn))
void checkpoint(char **args)
{
        if (putenv("DYLD_INSERT_LIBRARIES=./libckpt.dylib") < 0) {
                fprintf(stderr, "%s: execl: %s\n",
                        __FILE__, strerror(errno));
                exit(EXIT_FAILURE);
        }
                
        printf("Executing %s (pid=%d)\n", args[0], getpid());
        if (execvp(args[0], args) < 0) {
                fprintf(stderr, "%s: execvp: %s\n",
                        __FILE__, strerror(errno));
        }
        
        exit(EXIT_FAILURE);
}

__attribute__((noreturn)) 
void restart(char *ckptfile)
{
        int                     retval;
        short                   flags;
        pid_t                   pid;
        extern char             **environ;
        posix_spawnattr_t       attr;

        posix_spawnattr_init(&attr);
        char *args[] = {"./restart", ckptfile, NULL};

        /**
         * Disable address space layout randomization so fixed 
         * restart text and data segments are not slid
         */
        flags = POSIX_SPAWN_DISABLE_ASLR | POSIX_SPAWN_SETEXEC;
        
        if (posix_spawnattr_setflags(&attr, flags) < 0)
                err(EXIT_FAILURE, "posix_spawnattr_setflags");
        
        retval = posix_spawn(&pid, "./restart", NULL,
                             &attr, args, environ);
        if (retval < 0) {
                fprintf(stderr, "%s: posix_spawn: %s\n",
                        __FILE__, strerror(errno));
        }

        posix_spawnattr_destroy(&attr);
        exit(EXIT_FAILURE);
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
