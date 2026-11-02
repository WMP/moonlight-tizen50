/* Minimal native capp probe: prove the TV installs + launchpad starts a
 * hand-assembled dev/unsigned capp .tpk (root sideload). Logs who we run as. */
#include <stdio.h>
#include <unistd.h>
#include <time.h>
int main(int argc, char **argv) {
    FILE *f = fopen("/tmp/ml_probe.log", "a");
    time_t t = time(0);
    char lbl[256] = ""; FILE *s = fopen("/proc/self/attr/current", "r");
    if (s) { if (!fgets(lbl, sizeof lbl, s)) lbl[0]=0; fclose(s); }
    char line[512];
    int n = snprintf(line, sizeof line,
        "ml_probe START %.24s pid=%d uid=%d gid=%d argv0=%s smack=[%s]\n",
        ctime(&t), getpid(), getuid(), getgid(), argv[0], lbl);
    if (f) { fwrite(line, 1, n, f); fclose(f); }
    fwrite(line, 1, n, stdout); fflush(stdout);
    sleep(30);           /* stay alive so launchpad sees a live process */
    return 0;
}
