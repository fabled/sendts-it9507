#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <spawn.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/signalfd.h>

#define array_size(x) (sizeof(x) / sizeof(x[0]))

static int do_syslog = 0;
static int loglevel = LOG_DEBUG;

static void dlog(int prio, const char *format, ...)
{
	va_list va;

	if (prio > loglevel) return;

	va_start(va, format);
	if (do_syslog)
		vsyslog(prio, format, va);
	else {
		flockfile(stderr);
		vfprintf(stderr, format, va);
		fputc('\n', stderr);
		funlockfile(stderr);
	}
	va_end(va);
}

struct mpegts_buffer {
	size_t len;
	unsigned char data[10000*0xbc];
};

struct mpegts_stream {
	int id;
	int input_fd;
	int num_argv;
	char *argv[256];
	struct timeval timeout;

	int oldlen;
	unsigned char data[0xbc + 1024];
};

static int mpegts_stream_start(struct mpegts_stream *stream)
{
	int r, pipefd[2];
	posix_spawn_file_actions_t fa;

	dlog(LOG_DEBUG, "[%d] launching exec program: %s", stream->id, stream->argv[0]);

	if (pipe2(pipefd, O_CLOEXEC) < 0)
		return 0;

	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
	r = posix_spawnp(NULL, stream->argv[0], &fa, NULL, stream->argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(pipefd[1]);

	if (r != 0) {
		close(pipefd[0]);
		return 0;
	}

	fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
	stream->input_fd = pipefd[0];
	return 1;
}

static int mpegts_stream_stop(struct mpegts_stream *stream)
{
	if (stream->input_fd >= 0) {
		dlog(LOG_DEBUG, "[%d] closing input stream", stream->id);
		close(stream->input_fd);
		stream->input_fd = -1;
	}
	return 1;
}

static int mpegts_stream_process(struct mpegts_stream *stream, int newlen, struct mpegts_buffer *out)
{
	unsigned char *buf = &stream->data[0];
	int i = 0, len = newlen + stream->oldlen;

	while (i + 0xbc <= len) {
		if (buf[i] != 0x47) {
			while (buf[i] != 0x47 && i < len)
				i++;
			continue;
		}
		if (out) {
			if (out->len + 0xbc <= sizeof out->data) {
				memcpy(&out->data[out->len], &buf[i], 0xbc);
				out->len += 0xbc;
			} else {
				dlog(LOG_NOTICE, "[%d] output pipe full", stream->id);
			}
		}
		i += 0xbc;
	}

	stream->oldlen = len - i;
	memmove(&buf[0], &buf[i], stream->oldlen);
	return 0;
}


struct mpegts_mux {
	int active_stream;
	int num_streams;
	struct mpegts_stream streams[16];
};

static void mpegts_mux_loop(struct mpegts_mux *mux)
{
	sigset_t sigchldmask;
	struct mpegts_buffer out;
	struct signalfd_siginfo fdsi;
	struct pollfd fds[16+2];
	struct timeval now;
	int num_fds = -1, recalc_fds = 1;
	int i, j, n, status, timeout;
	ssize_t newlen;
	pid_t pid;

	sigemptyset(&sigchldmask);
	sigaddset(&sigchldmask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &sigchldmask, NULL);
	fds[0] = (struct pollfd){
		.fd = signalfd(-1, &sigchldmask, SFD_NONBLOCK|SFD_CLOEXEC),
		.events = POLLIN,
	};
	fds[1] = (struct pollfd){
		.fd = STDOUT_FILENO,
		//.events = POLLOUT,
	};

	sleep(1);
	out.len = 0;
	fcntl(STDOUT_FILENO, F_SETFL, O_NONBLOCK);

	gettimeofday(&now, NULL);
	while (1) {
		/* calculate first timeout */
		timeout = 2000;
		for (i = 0; i < mux->num_streams; i++) {
			struct mpegts_stream *s = &mux->streams[i];

			if (timercmp(&now, &s->timeout, <)) {
				struct timeval t;
				int to;
				timersub(&mux->streams[i].timeout, &now, &t);
				to = t.tv_sec * 1000 + t.tv_usec / 1000;
				if (to < timeout) timeout = to;
			} else {
				/* currently active fds timed out, start next stream */
				if (i == mux->active_stream &&
				    mux->active_stream+1 < mux->num_streams) {
					mux->active_stream++;
					out.len = 0;
					dlog(LOG_DEBUG, "[%d] timeout, activating %d", s->id,
						mux->active_stream);
				}

				/* respawn processes if needed */
				if (s->input_fd <= 0 && i <= mux->active_stream) {
					dlog(LOG_DEBUG, "[%d] timeout, starting process", s->id);
					s->timeout = now;
					s->timeout.tv_sec += 2;
					mpegts_stream_start(s);
					recalc_fds = 1;
				}
			}
		}

		if (recalc_fds) {
			recalc_fds = 0;
			num_fds = 2;
			for (i = 0; i < mux->num_streams; i++) {
				if (mux->streams[i].input_fd <= 0)
					continue;
				fds[num_fds++] = (struct pollfd) {
					.fd = mux->streams[i].input_fd,
					.events = POLLIN,
				};
			}
		}
		fds[1].events = fds[1].revents = out.len ? POLLOUT : 0;

		n = poll(fds, num_fds, timeout);
		if (n < 0) continue;

		gettimeofday(&now, NULL);
		if (fds[0].revents) {
			while (read(fds[0].fd, &fdsi, sizeof fdsi) > 0)
				;
			while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
				dlog(LOG_DEBUG, "reaped PID %d", pid);
		}
		if (fds[1].revents) {
			newlen = write(STDOUT_FILENO, out.data, out.len);
			if (newlen >= 0) {
				if (newlen != out.len) {
					out.len -= newlen;
					memmove(&out.data[0], &out.data[newlen], out.len);
				} else {
					out.len = 0;
				}
			}
		}
		for (n = 1; n < num_fds; n++) {
			if (!(fds[n].revents & (POLLIN | POLLHUP)))
				continue;

			for (i = 0; i < mux->num_streams; i++) {
				struct mpegts_stream *s = &mux->streams[i];

				if (fds[n].fd != s->input_fd)
					continue;

				newlen = read(s->input_fd, &s->data[s->oldlen], sizeof s->data - s->oldlen);
				if (newlen > 0) {
					/* if priority less than current active, fail
					 * back to this, and stop all lower priority
					 * streams */
					if (i < mux->active_stream) {
						mux->active_stream = i;
						out.len = 0;
						for (j = i+1; j < mux->num_streams; j++) {
							mpegts_stream_stop(&mux->streams[j]);
							recalc_fds = 1;
						}
					}
					mpegts_stream_process(s, newlen, i == mux->active_stream ? &out : 0);
				} else {
					mpegts_stream_stop(s);
					recalc_fds = 1;
				}
				s->timeout = now;
				s->timeout.tv_sec += 2;
				break;
			}
		}
	}
}

int main(int argc, char **argv)
{
	struct mpegts_mux mux;
	int i;

	if (argc <= 1) {
		fprintf(stderr, "usage: %s [cmd1 args..] [-- [cmd2 args..]]...\n", argv[0]);
		return 1;
	}

	memset(&mux, 0, sizeof mux);
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0) {
			mux.num_streams++;
			continue;
		}
		struct mpegts_stream *stream = &mux.streams[mux.num_streams];
		stream->argv[stream->num_argv++] = argv[i];
		stream->argv[stream->num_argv] = 0;
	}
	mux.num_streams++;

	for (i = 0; i < mux.num_streams; i++)
		mux.streams[i].id = i+1;

	mpegts_mux_loop(&mux);
}
