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

#include "ompi_config.h"

#include "ompi/communicator/communicator.h"
#include "ompi/info/info.h"
#include "ompi/file/file.h"
#include "ompi/mca/pml/pml.h"
#include "opal/datatype/opal_convertor.h"
#include "ompi/datatype/ompi_datatype.h"
#include "ompi/instance/instance.h"
#include "ompi/include/mpi.h"
#include "opal/mca/base/mca_base_framework.h"
#include "ompi/mca/io/base/base.h"
#include "ompi/mca/fs/base/base.h"
#include "ompi/mca/fcoll/base/base.h"
#include "ompi/mca/fbtl/base/base.h"
#include "ompi/mca/sharedfp/base/base.h"


#define MAX_BATCH_SIZE (1 << 20)
#define QUEUE_DEPTH 64

#include "io_bbview.h"

struct job
{
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

#define TARGET_BUFFER_SIZE (4 * 1024 * 1024)

static int
execute(char *src, char *dst)
{
	int src_fd;
	char *et_buf;
	char *dt_buf;
	OMPI_MPI_OFFSET_TYPE disp;
	int xl;
	int ret;
	ompi_file_t *fh;
	ompi_datatype_t *etype = NULL;
	ompi_datatype_t *dtype = NULL;
	size_t etype_size;
	void *buf = NULL;
	size_t buf_size;
	pmix_data_buffer_t *proc = NULL;
	char proc_buf[PATH_MAX];
	size_t proc_len;
	ompi_proc_t **p;

	syslog(LOG_INFO, "bbviewd: begin %s -> %s", src, dst);

	src_fd = open(src, O_RDONLY);
	if (src_fd < 0) {
		syslog(LOG_ERR, "open: %s", strerror(errno));
		return -1;
	}

	et_buf = malloc(PATH_MAX);
	if ((xl = fgetxattr(src_fd, BBVIEW_ATTR_ETYPE, et_buf, PATH_MAX)) < 0) {
		syslog(LOG_ERR, "missing xattr %s on %s: %s", BBVIEW_ATTR_ETYPE, src, strerror(errno));
		goto err_close;
	}
	dt_buf = malloc(PATH_MAX);
	if ((xl = fgetxattr(src_fd, BBVIEW_ATTR_DATATYPE, dt_buf, PATH_MAX)) < 0) {
		syslog(LOG_ERR, "missing xattr %s on %s: %s", BBVIEW_ATTR_DATATYPE, src, strerror(errno));
		goto err_close;
	}
	if ((xl = fgetxattr(src_fd, BBVIEW_ATTR_DISP, &disp, sizeof(disp))) < 0) {
		syslog(LOG_ERR, "missing xattr %s on %s: %s", BBVIEW_ATTR_DISP, src, strerror(errno));
		goto err_close;
	}
	if ((xl = fgetxattr(src_fd, BBVIEW_ATTR_PROC, proc_buf, sizeof(proc_buf))) < 0) {
		syslog(LOG_ERR, "missing xattr %s on %s: %s", BBVIEW_ATTR_PROC, src, strerror(errno));
		goto err_close;
	}
	PMIX_DATA_BUFFER_CREATE(proc);
	PMIX_DATA_BUFFER_LOAD(proc, proc_buf, xl);
	if (!proc) {
		syslog(LOG_ERR, "PMIX_DATA_BUFFER_LOAD failed");
		goto err_close;
	}
	ret = ompi_proc_unpack(proc, 1, &p, NULL, NULL);
	if (ret != OMPI_SUCCESS) {
		syslog(LOG_ERR, "ompi_proc_unpack failed");
		goto err_proc;
	}
	p[0]->super.proc_flags = OPAL_PROC_NON_LOCAL;

	etype = ompi_datatype_create_from_packed_description((void **)&et_buf, p[0]);
	if (etype == NULL) {
		syslog(LOG_ERR, "ompi_datatype_create_from_packed_description(etype) failed");
		goto err_proc;
	}
	dtype = ompi_datatype_create_from_packed_description((void **)&dt_buf, p[0]);
	if (dtype == NULL) {
		syslog(LOG_ERR, "ompi_datatype_create_from_packed_description(dtype) failed");
		goto err_proc;
	}

	ret = ompi_datatype_type_size(etype, &etype_size);
	if (ret != OMPI_SUCCESS) {
		syslog(LOG_ERR, "ompi_datatype_type_size failed");
		goto err_proc;
	}

	size_t etypes_per_buffer = (TARGET_BUFFER_SIZE + etype_size - 1) / etype_size;
	buf_size = etypes_per_buffer * etype_size;

	buf = malloc(buf_size);
	if (!buf) {
		syslog(LOG_ERR, "malloc failed");
		goto err_proc;
	}

	ret = ompi_file_open((struct ompi_communicator_t *)&ompi_mpi_comm_self, dst, OMPIO_MODE_WRONLY, (struct opal_info_t *)&ompi_mpi_info_null, &fh);
	if (ret != OMPI_SUCCESS) {
		syslog(LOG_ERR, "ompi_file_open failed");
		goto err_free;
	}

	ret = mca_io_ompio_file_set_view(fh, disp, etype, dtype, "native", (struct opal_info_t *)&ompi_mpi_info_null);
	if (ret != OMPI_SUCCESS) {
		syslog(LOG_ERR, "ompi_file_set_view failed");
		goto err_fclose;
	}

	ssize_t rsize;
	while ((rsize = read(src_fd, buf, buf_size)) > 0) {
		if (rsize % etype_size != 0) {
			syslog(LOG_WARNING, "partial etype read: rsize=%ld etype_size=%ld", (long)rsize, (long)etype_size);
			rsize -= (rsize % etype_size);
		}

		if (rsize == 0)
			continue;

		int count = rsize / etype_size;

		ret = mca_io_ompio_file_write(fh, buf, count, etype, MPI_STATUS_IGNORE);
		if (ret != OMPI_SUCCESS) {
			syslog(LOG_ERR, "ompi_file_write failed");
			goto err_fclose;
		}
	}

	if (rsize < 0) {
		syslog(LOG_ERR, "read error: %s", strerror(errno));
		goto err_fclose;
	}

	ompi_file_close(&fh);
	free(buf);
	close(src_fd);
	syslog(LOG_INFO, "bbviewd: end %s -> %s", src, dst);
	return 0;

err_fclose:
	ompi_file_close(&fh);
err_free:
	free(buf);
err_proc:
	PMIX_DATA_BUFFER_RELEASE(proc);
err_close:
	close(src_fd);
	return -1;
}

struct worker_arg
{
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
		execute(path, dst);
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

int main(int argc, char **argv)
{
	int provided;
	int ret;
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

	MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
	if (OMPI_SUCCESS != (ret = mca_base_framework_open(&ompi_fs_base_framework, 0))) {
		syslog(LOG_ERR, "Failed to open fs components");
		exit(EXIT_FAILURE);
	}
	if (OMPI_SUCCESS != (ret = mca_base_framework_open(&ompi_fcoll_base_framework, 0))) {
		syslog(LOG_ERR, "Failed to open fcoll components");
		exit(EXIT_FAILURE);
	}
	if (OMPI_SUCCESS != (ret = mca_base_framework_open(&ompi_fbtl_base_framework, 0))) {
		syslog(LOG_ERR, "Failed to open fbtl components");
		exit(EXIT_FAILURE);
	}
	if (OMPI_SUCCESS != (ret = mca_base_framework_open(&ompi_sharedfp_base_framework, 0))) {
		syslog(LOG_ERR, "Failed to open sharedfp components");
		exit(EXIT_FAILURE);
	}
	if (OMPI_SUCCESS != (ret = mca_base_framework_open(&ompi_io_base_framework, 0))) {
		syslog(LOG_ERR, "Failed to open io components");
		exit(EXIT_FAILURE);
	}


	openlog("bbviewd", LOG_PID | LOG_CONS | LOG_NDELAY | LOG_PERROR, LOG_DAEMON);

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
	MPI_Finalize();
	return 0;
}
