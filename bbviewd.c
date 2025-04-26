#define _GNU_SOURCE
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <syslog.h>
#include <liburing.h>

#define MAX_BATCH_SIZE (1 << 20)
#define QUEUE_DEPTH 64


#include "io_bbview.h"

struct job {
	char src[PATH_MAX];
	struct job *next;
};

static struct job *q_head = NULL, *q_tail = NULL;
static pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;
static atomic_int active_workers = 0;
static atomic_bool shutting_down = 0;

static void
enqueue(const char *path)
{
	struct job *j = calloc(1, sizeof(*j));
	if (!j) {
		syslog(LOG_ERR, "calloc: %s", strerror(errno));
		return;
	}
	strncpy(j->src, path, sizeof(j->src) - 1);

	pthread_mutex_lock(&q_mtx);
	if (!q_tail)
		q_head = q_tail = j;
	else {
		q_tail->next = j;
		q_tail = j;
	}
	pthread_cond_signal(&q_cv);
	pthread_mutex_unlock(&q_mtx);
}

static int
dequeue(char *out)
{
	pthread_mutex_lock(&q_mtx);
	while (!q_head && !shutting_down)
		pthread_cond_wait(&q_cv, &q_mtx);

	if (!q_head) {
		pthread_mutex_unlock(&q_mtx);
		return 0; /* shutting down */
	}

	struct job *j = q_head;
	q_head = j->next;
	if (!q_head)
		q_tail = NULL;
	pthread_mutex_unlock(&q_mtx);

	strcpy(out, j->src);
	free(j);
	return 1;
}

static int
execute(int in_fd, int out_fd, size_t etype_size, size_t blocklength,
	size_t stride, size_t disp, size_t count)
{
	struct io_uring ring;
	int rc;

	rc = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
	if (rc < 0) {
		syslog(LOG_ERR, "io_uring_queue_init: %s", strerror(-rc));
		return -1;
	}

	size_t block_size = blocklength * etype_size;
	size_t stride_size = stride * etype_size;

	char *buffer = malloc(MAX_BATCH_SIZE);
	if (!buffer) {
		syslog(LOG_ERR, "malloc: %s", strerror(errno));
		io_uring_queue_exit(&ring);
		return -1;
	}

	size_t batch_start_c = 0;
	size_t batch_blocks = 0;
	size_t batch_bytes = 0;

	for (size_t c = 0; c < count; ) {
		batch_start_c = c;
		batch_blocks = 0;
		batch_bytes = 0;

		while (c < count && batch_bytes + block_size <= MAX_BATCH_SIZE) {
			batch_bytes += block_size;
			batch_blocks++;
			c++;
		}

		size_t read_off = batch_start_c * block_size;
		ssize_t nread = pread(in_fd, buffer, batch_bytes, read_off);
		if (nread != (ssize_t)batch_bytes) {
			syslog(LOG_ERR, "pread: %s", strerror(errno));
			rc = -1;
			goto cleanup;
		}

		for (size_t i = 0; i < batch_blocks; i++) {
			struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
			if (!sqe) {
				io_uring_submit(&ring);
				sqe = io_uring_get_sqe(&ring);
				if (!sqe) {
					syslog(LOG_ERR, "get_sqe failed after submit");
					rc = -1;
					goto cleanup;
				}
			}

			off_t dst_off = disp + (batch_start_c + i) * stride_size;
			char *src = buffer + (i * block_size);
			io_uring_prep_write(sqe, out_fd, src, block_size, dst_off);
		}

		io_uring_submit(&ring);

		for (size_t i = 0; i < batch_blocks; i++) {
			struct io_uring_cqe *cqe;
			rc = io_uring_wait_cqe(&ring, &cqe);
			if (rc < 0) {
				syslog(LOG_ERR, "wait_cqe: %s", strerror(-rc));
				goto cleanup;
			}
			if (cqe->res < 0) {
				syslog(LOG_ERR, "cqe res: %s", strerror(-cqe->res));
				rc = -1;
			}
			io_uring_cqe_seen(&ring, cqe);
		}
	}

	rc = 0;

cleanup:
	free(buffer);
	io_uring_queue_exit(&ring);
	return rc;
}

struct worker_arg {
	int id;
};

static void *
worker_fn(void *arg)
{
	struct worker_arg *w = arg;

	char path[PATH_MAX];
	atomic_fetch_add(&active_workers, 1);

	while (dequeue(path)) {
		/* obtain destination path from xattr */
		char dst[PATH_MAX];
		ssize_t xl;
		char buf[PATH_MAX];
		size_t etype_size = 0, blocklength = 0, stride = 0, disp = 0,
		       count = 0;
		int in_fd, out_fd;
		int rc;
		struct stat st;

		if ((xl = getxattr(path, BBVIEW_ATTR_DEST_PATH, dst,
				   sizeof(dst) - 1)) < 0) {
			syslog(LOG_ERR,
			       "[worker %d] missing xattr %s on %s: %s\n",
			       w->id, BBVIEW_ATTR_DEST_PATH, path, strerror(errno));
			continue;
		}
		dst[xl] = '\0';
		if (stat(dst, &st) < 0) {
			syslog(LOG_ERR, "[worker %d] stat %s: %s\n", w->id, dst,
			       strerror(errno));
			continue;
		}

		in_fd = open(path, O_RDONLY, 0);
		out_fd = open(dst, O_WRONLY, 0);
		if (in_fd < 0 || out_fd < 0) {
			syslog(LOG_ERR, "open: %s", strerror(errno));
			goto cont;
		}

		if ((xl = fgetxattr(in_fd, BBVIEW_ATTR_ETYPE_SIZE, buf,
				    sizeof(buf) - 1)) < 0) {
			syslog(LOG_ERR,
			       "[worker %d] missing xattr %s on %s: %s\n",
			       w->id, BBVIEW_ATTR_ETYPE_SIZE, path, strerror(errno));
			goto cont;
		}
		buf[xl] = '\0';
		sscanf(buf, "%zu", &etype_size);
		if (etype_size == 0) {
			syslog(LOG_ERR,
			       "[worker %d] invalid etype size %s on %s\n",
			       w->id, buf, path);
			goto cont;
		}
		if ((xl = fgetxattr(in_fd, BBVIEW_ATTR_BLOCK_LENGTH, buf,
				    sizeof(buf) - 1)) < 0) {
			syslog(LOG_ERR,
			       "[worker %d] missing xattr %s on %s: %s\n",
			       w->id, BBVIEW_ATTR_BLOCK_LENGTH, path, strerror(errno));
			goto cont;
		}
		buf[xl] = '\0';
		sscanf(buf, "%zu", &blocklength);
		if (blocklength == 0) {
			syslog(LOG_ERR,
			       "[worker %d] invalid block length %s on %s\n",
			       w->id, buf, path);
			goto cont;
		}
		if ((xl = fgetxattr(in_fd, BBVIEW_ATTR_STRIDE, buf, sizeof(buf) - 1)) <
		    0) {
			syslog(LOG_ERR,
			       "[worker %d] missing xattr %s on %s: %s\n",
			       w->id, BBVIEW_ATTR_STRIDE, path, strerror(errno));
			goto cont;
		}
		buf[xl] = '\0';
		sscanf(buf, "%zu", &stride);
		if (stride == 0) {
			syslog(LOG_ERR, "[worker %d] invalid stride %s on %s\n",
			       w->id, buf, path);
			goto cont;
		}
		if ((xl = fgetxattr(in_fd, BBVIEW_ATTR_DISP, buf, sizeof(buf) - 1)) <
		    0) {
			syslog(LOG_ERR,
			       "[worker %d] missing xattr %s on %s: %s\n",
			       w->id, BBVIEW_ATTR_DISP, path, strerror(errno));
			goto cont;
		}
		buf[xl] = '\0';
		sscanf(buf, "%zu", &disp);
		if ((xl = fgetxattr(in_fd, BBVIEW_ATTR_COUNT, buf, sizeof(buf) - 1)) <
		    0) {
			syslog(LOG_ERR,
			       "[worker %d] missing xattr %s on %s: %s\n",
			       w->id, BBVIEW_ATTR_COUNT, path, strerror(errno));
			goto cont;
		}
		buf[xl] = '\0';
		sscanf(buf, "%zu", &count);
		if (count == 0) {
			syslog(LOG_ERR, "[worker %d] invalid count %s on %s\n",
			       w->id, buf, path);
			goto cont;
		}
		rc = execute(in_fd, out_fd, etype_size, blocklength, stride,
			     disp, count);

		if (!rc) {
			syslog(LOG_INFO,
			       "[worker %d] %s -> %s (%lu bytes) OK\n", w->id,
			       path, dst, count * blocklength * etype_size);
		}
	cont:
		if (in_fd >= 0)
			close(in_fd);
		if (out_fd >= 0)
			close(out_fd);
		continue;
	}

	atomic_fetch_sub(&active_workers, 1);
	return NULL;
}

static int
create_socket(void)
{
	unlink(BBVIEW_SOCK);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		syslog(LOG_ERR, "socket: %s", strerror(errno));
		return -1;
	}
	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, BBVIEW_SOCK, sizeof(addr.sun_path) - 1);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		syslog(LOG_ERR, "bind: %s", strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 16) < 0) {
		syslog(LOG_ERR, "listen: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static void
listener_loop(int sock_fd)
{
	int term_client_fd = -1;

	for (;;) {
		int cfd = accept(sock_fd, NULL, NULL);
		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			syslog(LOG_ERR, "accept: %s", strerror(errno));
			break;
		}

		char buf[PATH_MAX + 16] = {0};
		ssize_t n = read(cfd, buf, sizeof(buf) - 1);
		if (n <= 0) {
			close(cfd);
			continue;
		}
		buf[n] = '\0';

		if (!strncmp(buf, "terminate", 9)) {
			syslog(LOG_INFO, "Received terminate command");
			shutting_down = 1;
			pthread_cond_broadcast(&q_cv);

			term_client_fd = cfd;
			break;
		} else {
			char *nl = strchr(buf, '\n');
			if (nl)
				*nl = '\0';
			enqueue(buf);
			close(cfd);
		}
	}

	if (term_client_fd >= 0) {
		while (atomic_load(&active_workers) > 0) {
			usleep(1000);
		}

		write(term_client_fd, "done\n", 5);
		close(term_client_fd);
	}

	close(sock_fd);
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <num_threads> | wait\n", argv[0]);
		return EXIT_FAILURE;
	}

	if (strcmp(argv[1], "wait") == 0) {
		int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (sockfd < 0) {
			perror("socket");
			exit(EXIT_FAILURE);
		}

		struct sockaddr_un addr = {0};
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, BBVIEW_SOCK, sizeof(addr.sun_path) - 1);

		if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			perror("connect");
			exit(EXIT_FAILURE);
		}

		write(sockfd, "terminate\n", 10);

		char buf[128] = {0};
		ssize_t n = read(sockfd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = '\0';
			printf("%s", buf);
		}

		close(sockfd);
		return 0;
	}

	if (daemon(0, 0) < 0) {
		perror("daemon");
		exit(EXIT_FAILURE);
	}

	openlog("bbviewd", LOG_PID | LOG_CONS, LOG_DAEMON);

	int nthreads = atoi(argv[1]);
	if (nthreads <= 0)
		nthreads = 1;

	int sock_fd = create_socket();
	if (sock_fd < 0)
		return EXIT_FAILURE;

	/* ignore SIGPIPE so writes to dead clients don’t kill us */
	signal(SIGPIPE, SIG_IGN);

	pthread_t *tids = calloc(nthreads, sizeof(*tids));
	struct worker_arg *args = calloc(nthreads, sizeof(*args));
	for (int i = 0; i < nthreads; ++i) {
		args[i].id = i;
		if (pthread_create(&tids[i], NULL, worker_fn, &args[i]) != 0) {
			syslog(LOG_ERR, "pthread_create: %s", strerror(errno));
			return EXIT_FAILURE;
		}
	}

	listener_loop(sock_fd);

	/* wait for workers to drain and exit */
	for (int i = 0; i < nthreads; ++i)
		pthread_join(tids[i], NULL);

	unlink(BBVIEW_SOCK);
	free(tids);
	free(args);
	return 0;
}
