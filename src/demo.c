/* =============================================================================
 * src/demo.c  –  Phase 4 Demo Program
 *
 * Simulates a process with a known burst time of N seconds.
 * Prints one output line per second for N seconds so the scheduler
 * can visually demonstrate preemption and resumption via SIGSTOP/SIGCONT.
 *
 * Usage: ./demo N
 *   N  – number of seconds (iterations) to run
 * ============================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    /* Validate argument */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <N>\n", argv[0]);
        return 1;
    }

    int n = atoi(argv[1]);
    if (n <= 0) {
        fprintf(stderr, "Error: N must be a positive integer.\n");
        return 1;
    }

    /*
     * Loop N times, printing progress each iteration.
     * fflush(stdout) after every line so output reaches the pipe
     * immediately rather than being held in the stdio buffer —
     * critical for the scheduler to forward output in real time.
     */
    for (int i = 0; i < n; i++) {
        printf("Demo %d/%d\n", i, n);
        fflush(stdout);
        sleep(1);
    }

    return 0;
}
