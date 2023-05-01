/* Copyright © 2022-2023 Jakub Wilk <jwilk@jwilk.net>
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <termios.h>
#include <unistd.h>

#include <linux/major.h>
#include <linux/tiocl.h>
#include <linux/vt.h>

#define PROGRAM_NAME "vcsnoop"

static void show_usage(FILE *fp)
{
    fprintf(fp, "Usage: %s /dev/ttyN\n", PROGRAM_NAME);
    if (fp != stdout)
        return;
    fprintf(fp,
        "\n"
        "Options:\n"
        "  -h, --help  show this help message and exit\n"
    );
}

static void xerror(const char *context)
{
    int orig_errno = errno;
    fprintf(stderr, "%s: ", PROGRAM_NAME);
    errno = orig_errno;
    perror(context);
    exit(EXIT_FAILURE);
}

static void chvt(int fd, unsigned int n)
{
    int rc;
    rc = ioctl(fd, VT_ACTIVATE, (unsigned long) n);
    if (rc < 0)
        xerror("VT_ACTIVATE");
    rc = ioctl(fd, VT_WAITACTIVE, (unsigned long) n);
    if (rc < 0)
        xerror("VT_WAITACTIVE");
}

static int tty_fd = -1;
static struct termios orig_tio;

static void restore_tty()
{
    if (tty_fd < 0)
        return;
    int rc = tcsetattr(tty_fd, TCSAFLUSH, &orig_tio);
    if (rc < 0)
        xerror("tcsetattr()");
    tty_fd = -1;
}

static void init_tty(int fd)
{
    struct termios tio;
    int rc = tcgetattr(fd, &tio);
    if (rc < 0)
        xerror("tcgetattr()");
    orig_tio = tio;
    tio.c_lflag &= ~ECHO;
    rc = tcsetattr(fd, TCSAFLUSH, &tio);
    if (rc < 0)
        xerror("tcsetattr()");
    tty_fd = fd;
    atexit(restore_tty);
}

static void* rw_thread(void *arg)
{
    int fd = (intptr_t) arg;
    bool empty = true;
    char buf[PIPE_BUF];
    int write_error = 0;
    while (1) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int rc = poll(&pfd, 1, 1000);
        if (rc < 0)
            xerror("poll()");
        if (rc == 0) {
            if (empty) {
                errno = ETIME;
                xerror("poll()");
            }
            break;
        }
        ssize_t n = read(fd, buf, sizeof buf);
        if (n == 0)
            break;
        if (n < 0)
            xerror("read()");
        empty = false;
        if (!write_error) {
            ssize_t m = write(STDOUT_FILENO, buf, n);
            if (m < 0)
                write_error = errno;
            else if (m != n)
                write_error = EIO;
        }
    }
    if (write_error == EPIPE)
        kill(0, SIGPIPE);
    else if (write_error) {
        errno = write_error;
        xerror("write()");
    }
    return NULL;
}

static void snoop(unsigned int n)
{
    int fd = open("/dev/tty", O_RDWR);
    if (fd < 0)
        xerror("/dev/tty");
    struct vt_stat vts;
    int rc = ioctl(fd, VT_GETSTATE, &vts);
    if (rc < 0)
        xerror("VT_GETSTATE");
    unsigned short orig_vt = vts.v_active;
    chvt(fd, n);
    struct {
        char padding;
        char subcode;
        struct tiocl_selection sel;
    } sel_op;
    sel_op.subcode = TIOCL_SETSEL;
    sel_op.sel.xs = 1;
    sel_op.sel.xe = SHRT_MAX;
    sel_op.sel.ys = 1;
    sel_op.sel.ye = SHRT_MAX;
    sel_op.sel.sel_mode = TIOCL_SELLINE;
    rc = ioctl(0, TIOCLINUX, &sel_op.subcode);
    if (rc < 0)
        xerror("TIOCL_SETSEL");
    chvt(fd, orig_vt);
    sigset_t sig_mask;
    sigfillset(&sig_mask);
    sigaddset(&sig_mask, SIGPIPE);
    sigprocmask(SIG_BLOCK, &sig_mask, NULL);
    pthread_t pt;
    rc = pthread_create(&pt, NULL, rw_thread, (void*) (intptr_t) fd);
    if (rc < 0)
        xerror("pthread_create()");
    init_tty(fd); /* turn off ECHO */
    sel_op.subcode = TIOCL_PASTESEL;
    rc = ioctl(0, TIOCLINUX, &sel_op.subcode);
    if (rc < 0)
        xerror("TIOCL_PASTESEL");
    rc = pthread_join(pt, NULL);
    if (rc < 0)
        xerror("pthread_join()");
    restore_tty();
    sigprocmask(SIG_UNBLOCK, &sig_mask, NULL);
}

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "h-:")) != -1)
        switch (opt) {
        case 'h':
            show_usage(stdout);
            exit(EXIT_SUCCESS);
        case '-':
            if (strcmp(optarg, "help") == 0) {
                show_usage(stdout);
                exit(EXIT_SUCCESS);
            }
            /* fall through */
        default:
            show_usage(stderr);
            exit(EXIT_FAILURE);
        }
    argc -= optind;
    argv += optind;
    if (argc != 1) {
        show_usage(stderr);
        exit(EXIT_FAILURE);
    }
    struct stat sb;
    const char *path = argv[0];
    int rc = stat(path, &sb);
    if (rc < 0)
        xerror(path);
    if ((sb.st_mode & S_IFMT) != S_IFCHR)
        goto baddev;
    if (major(sb.st_rdev) != TTY_MAJOR)
        goto baddev;
    unsigned int sb_minor = minor(sb.st_rdev);
    if ((sb_minor < MIN_NR_CONSOLES) || (sb_minor > MAX_NR_CONSOLES))
        goto baddev;
    snoop(sb_minor);
    return 0;
baddev:
    errno = ENOTTY;
    xerror(path);
}

/* vim:set ts=4 sts=4 sw=4 et:*/
