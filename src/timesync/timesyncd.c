/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Kay Sievers

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/timerfd.h>
#include <sys/timex.h>
#include <sys/socket.h>

#include "missing.h"
#include "util.h"
#include "sparse-endian.h"
#include "log.h"
#include "sd-event.h"
#include "sd-daemon.h"

#define TIME_T_MAX (time_t)((1UL << ((sizeof(time_t) << 3) - 1)) - 1)

#ifndef ADJ_SETOFFSET
#define ADJ_SETOFFSET                   0x0100  /* add 'time' to current time */
#endif

/* expected accuracy of time synchronization; used to adjust the poll interval */
#define NTP_ACCURACY_SEC                0.2

/*
 * "A client MUST NOT under any conditions use a poll interval less
 * than 15 seconds."
 */
#define NTP_POLL_INTERVAL_MIN_SEC       32
#define NTP_POLL_INTERVAL_MAX_SEC       2048

/*
 * Maximum delta in seconds which the system clock is gradually adjusted
 * (slew) to approach the network time. Deltas larger that this are set by
 * letting the system time jump. The kernel's limit for adjtime is 0.5s.
 */
#define NTP_MAX_ADJUST                  0.4

/* NTP protocol, packet header */
#define NTP_LEAP_PLUSSEC                1
#define NTP_LEAP_MINUSSEC               2
#define NTP_LEAP_NOTINSYNC              3
#define NTP_MODE_CLIENT                 3
#define NTP_MODE_SERVER                 4
#define NTP_FIELD_LEAP(f)               (((f) >> 6) & 3)
#define NTP_FIELD_VERSION(f)            (((f) >> 3) & 7)
#define NTP_FIELD_MODE(f)               ((f) & 7)
#define NTP_FIELD(l, v, m)              (((l) << 6) | ((v) << 3) | (m))

/*
 * "NTP timestamps are represented as a 64-bit unsigned fixed-point number,
 * in seconds relative to 0h on 1 January 1900."
 */
#define OFFSET_1900_1970        2208988800UL

struct ntp_ts {
        be32_t sec;
        be32_t frac;
} _packed_;

struct ntp_ts_short {
        be16_t sec;
        be16_t frac;
} _packed_;

struct ntp_msg {
        uint8_t field;
        uint8_t stratum;
        int8_t poll;
        int8_t precision;
        struct ntp_ts_short root_delay;
        struct ntp_ts_short root_dispersion;
        char refid[4];
        struct ntp_ts reference_time;
        struct ntp_ts origin_time;
        struct ntp_ts recv_time;
        struct ntp_ts trans_time;
} _packed_;

typedef struct Manager Manager;
struct Manager {
        sd_event *event;

        /* peer */
        sd_event_source *event_receive;
        char *server;
        struct sockaddr_in server_addr;
        int server_socket;
        uint64_t packet_count;

        /* last sent packet */
        struct timespec trans_time_mon;
        struct timespec trans_time;
        usec_t retry_interval;
        bool pending;

        /* poll timer */
        sd_event_source *event_timer;
        usec_t poll_interval_usec;
        bool poll_resync;

        /* history data */
        struct {
                double offset;
                double delay;
        } samples[8];
        unsigned int samples_idx;
        double samples_jitter;

        /* last change */
        bool jumped;

        /* watch for time changes */
        sd_event_source *event_clock_watch;
        int clock_watch_fd;
};

static void manager_free(Manager *m);
DEFINE_TRIVIAL_CLEANUP_FUNC(Manager*, manager_free);
#define _cleanup_manager_free_ _cleanup_(manager_freep)

static int sntp_arm_timer(Manager *m, usec_t next);
static int sntp_clock_watch_setup(Manager *m);
static void sntp_server_disconnect(Manager *m);

static double ntp_ts_to_d(const struct ntp_ts *ts) {
        return be32toh(ts->sec) + ((double)be32toh(ts->frac) / UINT_MAX);
}

static double tv_to_d(const struct timeval *tv) {
        return tv->tv_sec + (1.0e-6 * tv->tv_usec);
}

static double ts_to_d(const struct timespec *ts) {
        return ts->tv_sec + (1.0e-9 * ts->tv_nsec);
}

static void d_to_tv(double d, struct timeval *tv) {
        tv->tv_sec = (long)d;
        tv->tv_usec = (d - tv->tv_sec) * 1000 * 1000;

        /* the kernel expects -0.3s as {-1, 7000.000} */
        if (tv->tv_usec < 0) {
                tv->tv_sec  -= 1;
                tv->tv_usec += 1000 * 1000;
        }
}

static double square(double d) {
        return d * d;
}

static int sntp_send_request(Manager *m) {
        struct ntp_msg ntpmsg = {};
        struct sockaddr_in addr = {};
        ssize_t len;
        int r;

        /*
         * "The client initializes the NTP message header, sends the request
         * to the server, and strips the time of day from the Transmit
         * Timestamp field of the reply.  For this purpose, all the NTP
         * header fields are set to 0, except the Mode, VN, and optional
         * Transmit Timestamp fields."
         */
        ntpmsg.field = NTP_FIELD(0, 4, NTP_MODE_CLIENT);

        /*
         * Set transmit timestamp, remember it; the server will send that back
         * as the origin timestamp and we have an indication that this is the
         * matching answer to our request.
         *
         * The actual value does not matter, We do not care about the correct
         * NTP UINT_MAX fraction; we just pass the plain nanosecond value.
         */
        clock_gettime(CLOCK_MONOTONIC, &m->trans_time_mon);
        clock_gettime(CLOCK_REALTIME, &m->trans_time);
        ntpmsg.trans_time.sec = htobe32(m->trans_time.tv_sec + OFFSET_1900_1970);
        ntpmsg.trans_time.frac = htobe32(m->trans_time.tv_nsec);

        addr.sin_family = AF_INET;
        addr.sin_port = htobe16(123);
        addr.sin_addr.s_addr = inet_addr(m->server);
        len = sendto(m->server_socket, &ntpmsg, sizeof(ntpmsg), MSG_DONTWAIT, &addr, sizeof(addr));
        if (len == sizeof(ntpmsg)) {
                m->pending = true;
                log_debug("Sent NTP request to: %s", m->server);
        } else
                log_debug("Sending NTP request to %s failed: %m", m->server);

        /* re-arm timer with incresing timeout, in case the packets never arrive back */
        if (m->retry_interval > 0) {
                if (m->retry_interval < NTP_POLL_INTERVAL_MAX_SEC * USEC_PER_SEC)
                        m->retry_interval *= 2;
        } else
                m->retry_interval = NTP_POLL_INTERVAL_MIN_SEC * USEC_PER_SEC;
        r = sntp_arm_timer(m, m->retry_interval);
        if (r < 0)
                return r;

        return 0;
}

static int sntp_timer(sd_event_source *source, usec_t usec, void *userdata) {
        Manager *m = userdata;

        assert(m);

        sntp_send_request(m);
        return 0;
}

static int sntp_arm_timer(Manager *m, usec_t next) {
        int r;

        assert(m);
        assert(m->event_receive);

        if (next == 0) {
                m->event_timer = sd_event_source_unref(m->event_timer);
                return 0;
        }

        if (m->event_timer) {
                r = sd_event_source_set_time(m->event_timer, now(CLOCK_MONOTONIC) + next);
                if (r < 0)
                        return r;

                return sd_event_source_set_enabled(m->event_timer, SD_EVENT_ONESHOT);
        }

        r = sd_event_add_time(
                        m->event,
                        &m->event_timer,
                        CLOCK_MONOTONIC,
                        now(CLOCK_MONOTONIC) + next, 0,
                        sntp_timer, m);
        if (r < 0)
                return r;

        return 0;
}

static int sntp_clock_watch(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
        Manager *m = userdata;

        assert(m);
        assert(m->event_receive);

        /* rearm timer */
        sntp_clock_watch_setup(m);

        /* skip our own jumps */
        if (m->jumped) {
                m->jumped = false;
                return 0;
        }

        /* resync */
        log_info("System time changed. Resyncing.");
        m->poll_resync = true;
        sntp_send_request(m);

        return 0;
}

/* wake up when the system time changes underneath us */
static int sntp_clock_watch_setup(Manager *m) {
        struct itimerspec its = { .it_value.tv_sec = TIME_T_MAX };
        _cleanup_close_ int fd = -1;
        sd_event *e;
        sd_event_source *source;
        int r;

        assert(m);
        assert(m->event_receive);

        fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK|TFD_CLOEXEC);
        if (fd < 0) {
                log_error("Failed to create timerfd: %m");
                return -errno;
        }

        if (timerfd_settime(fd, TFD_TIMER_ABSTIME|TFD_TIMER_CANCEL_ON_SET, &its, NULL) < 0) {
                log_error("Failed to set up timerfd: %m");
                return -errno;
        }

        e = sd_event_source_get_event(m->event_receive);
        r = sd_event_add_io(e, &source, fd, EPOLLIN, sntp_clock_watch, m);
        if (r < 0) {
                log_error("Failed to create clock watch event source: %s", strerror(-r));
                return r;
        }

        sd_event_source_unref(m->event_clock_watch);
        m->event_clock_watch = source;

        if (m->clock_watch_fd >= 0)
                close(m->clock_watch_fd);
        m->clock_watch_fd = fd;
        fd = -1;

        return 0;
}

static int sntp_adjust_clock(Manager *m, double offset, int leap_sec) {
        struct timex tmx = {};
        int r;

        /*
         * For small deltas, tell the kernel to gradually adjust the system
         * clock to the NTP time, larger deltas are just directly set.
         *
         * Clear STA_UNSYNC, it will enable the kernel's 11-minute mode, which
         * syncs the system time periodically to the hardware clock.
         */
        if (fabs(offset) < NTP_MAX_ADJUST) {
                tmx.modes = ADJ_STATUS | ADJ_OFFSET | ADJ_TIMECONST | ADJ_MAXERROR | ADJ_ESTERROR;
                tmx.status = STA_PLL;
                tmx.offset = offset * USEC_PER_SEC;
                tmx.constant = log2i(m->poll_interval_usec / USEC_PER_SEC) - 6;
                tmx.maxerror = 0;
                tmx.esterror = 0;
                log_debug("  adjust (slew): %+f sec\n", (double)tmx.offset / USEC_PER_SEC);
        } else {
                tmx.modes = ADJ_SETOFFSET;
                d_to_tv(offset, &tmx.time);

                m->jumped = true;
                log_debug("  adjust (jump): %+f sec\n", tv_to_d(&tmx.time));
        }

        switch (leap_sec) {
        case 1:
                tmx.status |= STA_INS;
                break;
        case -1:
                tmx.status |= STA_DEL;
                break;
        }

        //r = clock_adjtime(CLOCK_REALTIME, &tmx);
        r = adjtimex(&tmx);
        if (r < 0)
                return r;

        log_debug("  status       : %04i %s\n"
                  "  time now     : %li.%06li\n"
                  "  constant     : %li\n"
                  "  offset       : %+f sec\n"
                  "  freq offset  : %+li (%+.3f ppm)\n",
                  tmx.status, tmx.status & STA_UNSYNC ? "" : "sync",
                  tmx.time.tv_sec, tmx.time.tv_usec,
                  tmx.constant,
                  (double)tmx.offset / USEC_PER_SEC,
                  tmx.freq, (double)tmx.freq / 65536);

        return 0;
}

static bool sntp_sample_spike_detection(Manager *m, double offset, double delay) {
        unsigned int i, idx_cur, idx_new, idx_min;
        double jitter;
        double j;

        m->packet_count++;

        /* ignore initial sample */
        if (m->packet_count == 1)
                return false;

        /* store the current data in our samples array */
        idx_cur = m->samples_idx;
        idx_new = (idx_cur + 1) % ELEMENTSOF(m->samples);
        m->samples_idx = idx_new;
        m->samples[idx_new].offset = offset;
        m->samples[idx_new].delay = delay;

        /* calculate new jitter value from the RMS differences relative to the lowest delay sample */
        jitter = m->samples_jitter;
        for (idx_min = idx_cur, i = 0; i < ELEMENTSOF(m->samples); i++)
                if (m->samples[i].delay > 0 && m->samples[i].delay < m->samples[idx_min].delay)
                        idx_min = i;

        j = 0;
        for (i = 0; i < ELEMENTSOF(m->samples); i++)
                j += square(m->samples[i].offset - m->samples[idx_min].offset);
        m->samples_jitter = sqrt(j / (ELEMENTSOF(m->samples) - 1));

        /* ignore samples when resyncing */
        if (m->poll_resync)
                return false;

        /* always accept offset if we are farther off than the round-trip delay */
        if (fabs(offset) > delay)
                return false;

        /* we need a few samples before looking at them */
        if (m->packet_count < 4)
                return false;

        /* do not accept anything worse than the maximum possible error of the best sample */
        if (fabs(offset) > m->samples[idx_min].delay)
                return true;

        /* compare the difference between the current offset to the previous offset and jitter */
        return fabs(offset - m->samples[idx_cur].offset) > 3 * jitter;
}

static void sntp_adjust_poll(Manager *m, double offset, bool spike) {
        if (m->poll_resync) {
                m->poll_interval_usec = NTP_POLL_INTERVAL_MIN_SEC * USEC_PER_SEC;
                m->poll_resync = false;
                return;
        }

        /* set to minimal poll interval */
        if (!spike && fabs(offset) > NTP_ACCURACY_SEC) {
                m->poll_interval_usec = NTP_POLL_INTERVAL_MIN_SEC * USEC_PER_SEC;
                return;
        }

        /* increase polling interval */
        if (fabs(offset) < NTP_ACCURACY_SEC * 0.25) {
                if (m->poll_interval_usec < NTP_POLL_INTERVAL_MAX_SEC * USEC_PER_SEC)
                        m->poll_interval_usec *= 2;
                return;
        }

        /* decrease polling interval */
        if (spike || fabs(offset) > NTP_ACCURACY_SEC * 0.75) {
                if (m->poll_interval_usec > NTP_POLL_INTERVAL_MIN_SEC * USEC_PER_SEC)
                        m->poll_interval_usec /= 2;
                return;
        }
}

static int sntp_receive_response(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
        Manager *m = userdata;
        unsigned char buf[sizeof(struct ntp_msg)];
        struct iovec iov = {
                .iov_base = buf,
                .iov_len = sizeof(buf),
        };
        union {
                struct cmsghdr cmsghdr;
                uint8_t buf[CMSG_SPACE(sizeof(struct timeval))];
        } control;
        struct sockaddr_in server_addr;
        struct msghdr msghdr = {
                .msg_iov = &iov,
                .msg_iovlen = 1,
                .msg_control = &control,
                .msg_controllen = sizeof(control),
                .msg_name = &server_addr,
                .msg_namelen = sizeof(server_addr),
        };
        struct cmsghdr *cmsg;
        struct timespec now_ts;
        struct timeval *recv_time;
        ssize_t len;
        struct ntp_msg *ntpmsg;
        double origin, receive, trans, dest;
        double delay, offset;
        bool spike;
        int leap_sec;
        int r;

        if (revents & (EPOLLHUP|EPOLLERR)) {
                log_debug("Server connection returned error. Closing.");
                sntp_server_disconnect(m);
                return -ENOTCONN;
        }

        len = recvmsg(fd, &msghdr, MSG_DONTWAIT);
        if (len < 0) {
                log_debug("Error receiving message. Disconnecting.");
                return -EINVAL;
        }

        if (iov.iov_len < sizeof(struct ntp_msg)) {
                log_debug("Invalid response from server. Disconnecting.");
                return -EINVAL;
        }

        if (m->server_addr.sin_addr.s_addr != server_addr.sin_addr.s_addr) {
                log_debug("Response from unknown server. Disconnecting.");
                return -EINVAL;
        }

        recv_time = NULL;
        for (cmsg = CMSG_FIRSTHDR(&msghdr); cmsg; cmsg = CMSG_NXTHDR(&msghdr, cmsg)) {
                if (cmsg->cmsg_level != SOL_SOCKET)
                        continue;

                switch (cmsg->cmsg_type) {
                case SCM_TIMESTAMP:
                        recv_time = (struct timeval *) CMSG_DATA(cmsg);
                        break;
                }
        }
        if (!recv_time) {
                log_debug("Invalid packet timestamp. Disconnecting.");
                return -EINVAL;
        }

        ntpmsg = iov.iov_base;
        if (!m->pending) {
                log_debug("Unexpected reply. Ignoring.");
                return 0;
        }

        /* check our "time cookie" (we just stored nanoseconds in the fraction field) */
        if (be32toh(ntpmsg->origin_time.sec) != m->trans_time.tv_sec + OFFSET_1900_1970 ||
            be32toh(ntpmsg->origin_time.frac) != m->trans_time.tv_nsec) {
                log_debug("Invalid reply; not our transmit time. Ignoring.");
                return 0;
        }

        if (NTP_FIELD_LEAP(ntpmsg->field) == NTP_LEAP_NOTINSYNC) {
                log_debug("Server is not synchronized. Disconnecting.");
                return -EINVAL;
        }

        if (NTP_FIELD_VERSION(ntpmsg->field) != 4) {
                log_debug("Response NTPv%d. Disconnecting.", NTP_FIELD_VERSION(ntpmsg->field));
                return -EINVAL;
        }

        if (NTP_FIELD_MODE(ntpmsg->field) != NTP_MODE_SERVER) {
                log_debug("Unsupported mode %d. Disconnecting.", NTP_FIELD_MODE(ntpmsg->field));
                return -EINVAL;
        }

        /* valid packet */
        m->pending = false;
        m->retry_interval = 0;

        /* announce leap seconds */
        if (NTP_FIELD_LEAP(ntpmsg->field) & NTP_LEAP_PLUSSEC)
                leap_sec = 1;
        else if (NTP_FIELD_LEAP(ntpmsg->field) & NTP_LEAP_MINUSSEC)
                leap_sec = -1;
        else
                leap_sec = 0;

        /*
         * "Timestamp Name          ID   When Generated
         *  ------------------------------------------------------------
         *  Originate Timestamp     T1   time request sent by client
         *  Receive Timestamp       T2   time request received by server
         *  Transmit Timestamp      T3   time reply sent by server
         *  Destination Timestamp   T4   time reply received by client
         *
         *  The round-trip delay, d, and system clock offset, t, are defined as:
         *  d = (T4 - T1) - (T3 - T2)     t = ((T2 - T1) + (T3 - T4)) / 2"
         */
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        origin = tv_to_d(recv_time) - (ts_to_d(&now_ts) - ts_to_d(&m->trans_time_mon)) + OFFSET_1900_1970;
        receive = ntp_ts_to_d(&ntpmsg->recv_time);
        trans = ntp_ts_to_d(&ntpmsg->trans_time);
        dest = tv_to_d(recv_time) + OFFSET_1900_1970;

        offset = ((receive - origin) + (trans - dest)) / 2;
        delay = (dest - origin) - (trans - receive);

        spike = sntp_sample_spike_detection(m, offset, delay);

        sntp_adjust_poll(m, offset, spike);

        log_debug("NTP response:\n"
                  "  leap         : %u\n"
                  "  version      : %u\n"
                  "  mode         : %u\n"
                  "  stratum      : %u\n"
                  "  precision    : %f sec (%d)\n"
                  "  reference    : %.4s\n"
                  "  origin       : %f\n"
                  "  receive      : %f\n"
                  "  transmit     : %f\n"
                  "  dest         : %f\n"
                  "  offset       : %+f sec\n"
                  "  delay        : %+f sec\n"
                  "  packet count : %"PRIu64"\n"
                  "  jitter       : %f%s\n"
                  "  poll interval: %llu\n",
                  NTP_FIELD_LEAP(ntpmsg->field),
                  NTP_FIELD_VERSION(ntpmsg->field),
                  NTP_FIELD_MODE(ntpmsg->field),
                  ntpmsg->stratum,
                  exp2(ntpmsg->precision), ntpmsg->precision,
                  ntpmsg->stratum == 1 ? ntpmsg->refid : "n/a",
                  origin - OFFSET_1900_1970,
                  receive - OFFSET_1900_1970,
                  trans - OFFSET_1900_1970,
                  dest - OFFSET_1900_1970,
                  offset, delay,
                  m->packet_count,
                  m->samples_jitter, spike ? " spike" : "",
                  m->poll_interval_usec / USEC_PER_SEC);

        log_info("%4llu %+10f %10f %10f%s",
                 m->poll_interval_usec / USEC_PER_SEC, offset, delay, m->samples_jitter, spike ? " spike" : "");

        if (!spike) {
                r = sntp_adjust_clock(m, offset, leap_sec);
                if (r < 0)
                        log_error("Failed to call clock_adjtime(): %m");
        }

        r = sntp_arm_timer(m, m->poll_interval_usec);
        if (r < 0)
                return r;

        return 0;
}

static int sntp_server_connect(Manager *m, const char *server) {
        _cleanup_free_ char *s = NULL;

        assert(m);
        assert(server);
        assert(m->server_socket >= 0);

        s = strdup(server);
        if (!s)
                return -ENOMEM;

        free(m->server);
        m->server = s;
        s = NULL;

        zero(m->server_addr);
        m->server_addr.sin_family = AF_INET;
        m->server_addr.sin_addr.s_addr = inet_addr(server);

        m->poll_interval_usec = NTP_POLL_INTERVAL_MIN_SEC * USEC_PER_SEC;

        return sntp_send_request(m);
}

static void sntp_server_disconnect(Manager *m) {
        if (!m->server)
                return;

        m->event_timer = sd_event_source_unref(m->event_timer);

        m->event_clock_watch = sd_event_source_unref(m->event_clock_watch);
        if (m->clock_watch_fd > 0)
                close(m->clock_watch_fd);
        m->clock_watch_fd = -1;

        m->event_receive = sd_event_source_unref(m->event_receive);
        if (m->server_socket > 0)
                close(m->server_socket);
        m->server_socket = -1;

        zero(m->server_addr);
        free(m->server);
        m->server = NULL;
}

static int sntp_listen_setup(Manager *m) {
        _cleanup_close_ int fd = -1;
        struct sockaddr_in addr;
        const int on = 1;
        const int tos = IPTOS_LOWDELAY;
        int r;

        fd = socket(PF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
                return -errno;

        zero(addr);
        addr.sin_family = AF_INET;
        r = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (r < 0)
                return -errno;

        r = setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &on, sizeof(on));
        if (r < 0)
                return -errno;

        r = setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
        if (r < 0)
                return -errno;

        r = sd_event_add_io(m->event, &m->event_receive, fd, EPOLLIN, sntp_receive_response, m);
        if (r < 0)
                return r;

        m->server_socket = fd;
        fd = -1;

        return 0;
}

static int manager_new(Manager **ret) {
        _cleanup_manager_free_ Manager *m = NULL;
        int r;

        m = new0(Manager, 1);
        if (!m)
                return -ENOMEM;

        r = sd_event_default(&m->event);
        if (r < 0)
                return r;

        r = sntp_listen_setup(m);
        if (r < 0)
                return r;

        r = sntp_clock_watch_setup(m);
        if (r < 0)
                return r;

        *ret = m;
        m = NULL;

        return 0;
}

static void manager_free(Manager *m) {

        if (!m)
                return;

        sd_event_unref(m->event);
        free(m);
}

int main(int argc, char *argv[]) {
        _cleanup_manager_free_ Manager *m = NULL;
        const char *server;
        int r;

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        r = manager_new(&m);
        if (r < 0)
                goto out;

        //server = "216.239.32.15";       /* time1.google.com */
        //server = "192.53.103.108";      /* ntp1.ptb.de */
        server = "27.54.95.11";         /* au.pool.ntp.org */

        sd_notifyf(false,
                  "READY=1\n"
                  "STATUS=Connecting to %s", server);

        r = sntp_server_connect(m, server);
        if (r < 0)
                goto out;

        r = sd_event_loop(m->event);
        if (r < 0)
                goto out;

out:
        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
