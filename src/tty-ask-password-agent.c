/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <sys/poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/signalfd.h>

#include "util.h"
#include "conf-parser.h"
#include "utmp-wtmp.h"
#include "socket-util.h"

static enum {
        ACTION_LIST,
        ACTION_QUERY,
        ACTION_WATCH,
        ACTION_WALL
} arg_action = ACTION_QUERY;

static bool arg_plymouth = false;

static int ask_password_plymouth(const char *message, usec_t until, const char *flag_file, char **_passphrase) {
        int fd = -1, notify = -1;
        union sockaddr_union sa;
        char *packet = NULL;
        ssize_t k;
        int r, n;
        struct pollfd pollfd[2];
        char buffer[LINE_MAX];
        size_t p = 0;
        enum {
                POLL_SOCKET,
                POLL_INOTIFY
        };

        if (flag_file) {
                if ((notify = inotify_init1(IN_CLOEXEC|IN_NONBLOCK)) < 0) {
                        r = -errno;
                        goto finish;
                }

                if (inotify_add_watch(notify, flag_file, IN_ATTRIB /* for the link count */) < 0) {
                        r = -errno;
                        goto finish;
                }
        }

        if ((fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0)) < 0) {
                r = -errno;
                goto finish;
        }

        zero(sa);
        sa.sa.sa_family = AF_UNIX;
        strncpy(sa.un.sun_path+1, "/ply-boot-protocol", sizeof(sa.un.sun_path)-1);

        if (connect(fd, &sa.sa, sizeof(sa.un)) < 0) {
                r = -errno;
                goto finish;
        }

        if (asprintf(&packet, "*\002%c%s%n", (int) (strlen(message) + 1), message, &n) < 0) {
                r = -ENOMEM;
                goto finish;
        }

        if ((k = loop_write(fd, packet, n+1, true)) != n+1) {
                r = k < 0 ? (int) k : -EIO;
                goto finish;
        }

        zero(pollfd);
        pollfd[POLL_SOCKET].fd = fd;
        pollfd[POLL_SOCKET].events = POLLIN;
        pollfd[POLL_INOTIFY].fd = notify;
        pollfd[POLL_INOTIFY].events = POLLIN;

        for (;;) {
                int sleep_for = -1, j;

                if (until > 0) {
                        usec_t y;

                        y = now(CLOCK_MONOTONIC);

                        if (y > until) {
                                r = -ETIMEDOUT;
                                goto finish;
                        }

                        sleep_for = (int) ((until - y) / USEC_PER_MSEC);
                }

                if (flag_file)
                        if (access(flag_file, F_OK) < 0) {
                                r = -errno;
                                goto finish;
                        }

                if ((j = poll(pollfd, notify > 0 ? 2 : 1, sleep_for)) < 0) {

                        if (errno == EINTR)
                                continue;

                        r = -errno;
                        goto finish;
                } else if (j == 0) {
                        r = -ETIMEDOUT;
                        goto finish;
                }

                if (notify > 0 && pollfd[POLL_INOTIFY].revents != 0)
                        flush_fd(notify);

                if (pollfd[POLL_SOCKET].revents == 0)
                        continue;

                if ((k = read(fd, buffer + p, sizeof(buffer) - p)) <= 0) {
                        r = k < 0 ? -errno : -EIO;
                        goto finish;
                }

                p += k;

                if (p < 1)
                        continue;

                if (buffer[0] == 5) {
                        /* No password, because UI not shown */
                        r = -ENOENT;
                        goto finish;

                } else if (buffer[0] == 2) {
                        uint32_t size;
                        char *s;

                        /* One answer */
                        if (p < 5)
                                continue;

                        memcpy(&size, buffer+1, sizeof(size));
                        if (size+5 > sizeof(buffer)) {
                                r = -EIO;
                                goto finish;
                        }

                        if (p-5 < size)
                                continue;

                        if (!(s = strndup(buffer + 5, size))) {
                                r = -ENOMEM;
                                goto finish;
                        }

                        *_passphrase = s;
                        break;
                } else {
                        /* Unknown packet */
                        r = -EIO;
                        goto finish;
                }
        }

        r = 0;

finish:
        if (notify >= 0)
                close_nointr_nofail(notify);

        if (fd >= 0)
                close_nointr_nofail(fd);

        free(packet);

        return r;
}

static int parse_password(const char *filename, char **wall) {
        char *socket_name = NULL, *message = NULL, *packet = NULL;
        uint64_t not_after = 0;
        unsigned pid = 0;
        int socket_fd = -1;

        const ConfigItem items[] = {
                { "Socket",   config_parse_string,   &socket_name, "Ask" },
                { "NotAfter", config_parse_uint64,   &not_after,   "Ask" },
                { "Message",  config_parse_string,   &message,     "Ask" },
                { "PID",      config_parse_unsigned, &pid,         "Ask" },
        };

        FILE *f;
        int r;
        usec_t n;

        assert(filename);

        if (!(f = fopen(filename, "re"))) {

                if (errno == ENOENT)
                        return 0;

                log_error("open(%s): %m", filename);
                return -errno;
        }

        if ((r = config_parse(filename, f, NULL, items, false, NULL)) < 0) {
                log_error("Failed to parse password file %s: %s", filename, strerror(-r));
                goto finish;
        }

        if (!socket_name || not_after <= 0) {
                log_error("Invalid password file %s", filename);
                r = -EBADMSG;
                goto finish;
        }

        n = now(CLOCK_MONOTONIC);
        if (n > not_after) {
                r = 0;
                goto finish;
        }

        if (arg_action == ACTION_LIST)
                printf("'%s' (PID %u)\n", message, pid);
        else if (arg_action == ACTION_WALL) {
                char *_wall;

                if (asprintf(&_wall,
                             "%s%sPassword entry required for \'%s\' (PID %u).\r\n"
                             "Please enter password with the systemd-tty-password-agent tool!",
                             *wall ? *wall : "",
                             *wall ? "\r\n\r\n" : "",
                             message,
                             pid) < 0) {
                        log_error("Out of memory");
                        r = -ENOMEM;
                        goto finish;
                }

                free(*wall);
                *wall = _wall;
        } else {
                union {
                        struct sockaddr sa;
                        struct sockaddr_un un;
                } sa;
                char *password;

                assert(arg_action == ACTION_QUERY ||
                       arg_action == ACTION_WATCH);

                if (access(socket_name, W_OK) < 0) {

                        if (arg_action == ACTION_QUERY)
                                log_info("Not querying '%s' (PID %u), lacking privileges.", message, pid);

                        r = 0;
                        goto finish;
                }

                if (arg_plymouth)
                        r = ask_password_plymouth(message, not_after, filename, &password);
                else
                        r = ask_password_tty(message, not_after, filename, &password);

                if (r < 0) {
                        log_error("Failed to query password: %s", strerror(-r));
                        goto finish;
                }

                asprintf(&packet, "+%s", password);
                free(password);

                if (!packet) {
                        log_error("Out of memory");
                        r = -ENOMEM;
                        goto finish;
                }

                if ((socket_fd = socket(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC, 0)) < 0) {
                        log_error("socket(): %m");
                        r = -errno;
                        goto finish;
                }

                zero(sa);
                sa.un.sun_family = AF_UNIX;
                strncpy(sa.un.sun_path, socket_name, sizeof(sa.un.sun_path));

                if (sendto(socket_fd, packet, strlen(packet), MSG_NOSIGNAL, &sa.sa, offsetof(struct sockaddr_un, sun_path) + strlen(socket_name)) < 0) {
                        log_error("Failed to send: %m");
                        r = -errno;
                        goto finish;
                }
        }

finish:
        fclose(f);

        if (socket_fd >= 0)
                close_nointr_nofail(socket_fd);

        free(packet);
        free(socket_name);
        free(message);

        return r;
}

static int show_passwords(void) {
        DIR *d;
        struct dirent *de;
        int r = 0;

        if (!(d = opendir("/dev/.systemd/ask-password"))) {
                if (errno == ENOENT)
                        return 0;

                log_error("opendir(): %m");
                return -errno;
        }

        while ((de = readdir(d))) {
                char *p;
                int q;
                char *wall;

                if (de->d_type != DT_REG)
                        continue;

                if (ignore_file(de->d_name))
                        continue;

                if (!startswith(de->d_name, "ask."))
                        continue;

                if (!(p = strappend("/dev/.systemd/ask-password/", de->d_name))) {
                        log_error("Out of memory");
                        r = -ENOMEM;
                        goto finish;
                }

                wall = NULL;
                if ((q = parse_password(p, &wall)) < 0)
                        r = q;

                free(p);

                if (wall) {
                        utmp_wall(wall);
                        free(wall);
                }
        }

finish:
        if (d)
                closedir(d);

        return r;
}

static int watch_passwords(void) {
        enum {
                FD_INOTIFY,
                FD_SIGNAL,
                _FD_MAX
        };

        int notify = -1, signal_fd = -1;
        struct pollfd pollfd[_FD_MAX];
        sigset_t mask;
        int r;

        mkdir_p("/dev/.systemd/ask-password", 0755);

        if ((notify = inotify_init1(IN_CLOEXEC)) < 0) {
                r = -errno;
                goto finish;
        }

        if (inotify_add_watch(notify, "/dev/.systemd/ask-password", IN_CLOSE_WRITE|IN_MOVED_TO) < 0) {
                r = -errno;
                goto finish;
        }

        assert_se(sigemptyset(&mask) == 0);
        sigset_add_many(&mask, SIGINT, SIGTERM, -1);
        assert_se(sigprocmask(SIG_SETMASK, &mask, NULL) == 0);

        if ((signal_fd = signalfd(-1, &mask, SFD_NONBLOCK|SFD_CLOEXEC)) < 0) {
                log_error("signalfd(): %m");
                r = -errno;
                goto finish;
        }

        zero(pollfd);
        pollfd[FD_INOTIFY].fd = notify;
        pollfd[FD_INOTIFY].events = POLLIN;
        pollfd[FD_SIGNAL].fd = signal_fd;
        pollfd[FD_SIGNAL].events = POLLIN;

        for (;;) {
                if ((r = show_passwords()) < 0)
                        break;

                if (poll(pollfd, _FD_MAX, -1) < 0) {

                        if (errno == EINTR)
                                continue;

                        r = -errno;
                        goto finish;
                }

                if (pollfd[FD_INOTIFY].revents != 0)
                        flush_fd(notify);

                if (pollfd[FD_SIGNAL].revents != 0)
                        break;
        }

        r = 0;

finish:
        if (notify >= 0)
                close_nointr_nofail(notify);

        if (signal_fd >= 0)
                close_nointr_nofail(signal_fd);

        return r;
}

static int help(void) {

        printf("%s [OPTIONS...]\n\n"
               "Process system password requests.\n\n"
               "  -h --help     Show this help\n"
               "     --list     Show pending password requests\n"
               "     --query    Process pending password requests\n"
               "     --watch    Continously process password requests\n"
               "     --wall     Continously forward password requests to wall\n"
               "     --plymouth Ask question with Plymouth instead of on TTY\n",
               program_invocation_short_name);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_LIST = 0x100,
                ARG_QUERY,
                ARG_WATCH,
                ARG_WALL,
                ARG_PLYMOUTH
        };

        static const struct option options[] = {
                { "help",     no_argument, NULL, 'h'          },
                { "list",     no_argument, NULL, ARG_LIST     },
                { "query",    no_argument, NULL, ARG_QUERY    },
                { "watch",    no_argument, NULL, ARG_WATCH    },
                { "wall",     no_argument, NULL, ARG_WALL     },
                { "plymouth", no_argument, NULL, ARG_PLYMOUTH },
                { NULL,    0,           NULL, 0               }
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0) {

                switch (c) {

                case 'h':
                        help();
                        return 0;

                case ARG_LIST:
                        arg_action = ACTION_LIST;
                        break;

                case ARG_QUERY:
                        arg_action = ACTION_QUERY;
                        break;

                case ARG_WATCH:
                        arg_action = ACTION_WATCH;
                        break;

                case ARG_WALL:
                        arg_action = ACTION_WALL;
                        break;

                case ARG_PLYMOUTH:
                        arg_plymouth = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        log_error("Unknown option code %c", c);
                        return -EINVAL;
                }
        }

        if (optind != argc) {
                help();
                return -EINVAL;
        }

        return 1;
}

int main(int argc, char *argv[]) {
        int r;

        log_parse_environment();
        log_open();

        if ((r = parse_argv(argc, argv)) <= 0)
                goto finish;

        if (arg_action == ACTION_WATCH ||
            arg_action == ACTION_WALL)
                r = watch_passwords();
        else
                r = show_passwords();

finish:
        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}