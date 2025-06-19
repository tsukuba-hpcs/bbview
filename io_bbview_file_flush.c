/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2016 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008-2018 University of Houston. All rights reserved.
 * Copyright (c) 2015-2018 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016-2017 IBM Corporation. All rights reserved.
 *  $COPYRIGHT$
 *
 *  Additional copyrights may follow
 *
 *  $HEADER$
 */

#include "ompi_config.h"

#include "ompi/communicator/communicator.h"
#include "ompi/info/info.h"
#include "ompi/file/file.h"
#include "ompi/mca/pml/pml.h"
#include "opal/datatype/opal_convertor.h"
#include "ompi/datatype/ompi_datatype.h"
#include <stdlib.h>
#include <stdio.h>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include "io_bbview.h"

static int set_xattrs_for_bbview(const char *path, mca_common_bbview_data_t *data)
{
    char buf[PATH_MAX];
    pmix_data_buffer_t *proc = NULL;
    ompi_proc_t *local = ompi_proc_local();
    int rc;
    char *proc_buf;
    size_t proc_len;

    if (realpath(data->ompio_fh.f_filename, buf) == NULL) {
        perror("realpath");
        return -1;
    }

    rc = setxattr(path, BBVIEW_ATTR_DEST_PATH, buf, strlen(buf), 0);
    if (rc < 0) {
        perror("setxattr BBVIEW_ATTR_DEST_PATH");
        return -1;
    }

    rc = setxattr(path, BBVIEW_ATTR_DISP, &data->saved_disp, sizeof(data->saved_disp), 0);
    if (rc < 0) {
        perror("setxattr BBVIEW_ATTR_DISP");
        return -1;
    }

    rc = setxattr(path, BBVIEW_ATTR_DATATYPE, data->saved_dt_buf, data->saved_dt_len, 0);
    if (rc < 0) {
        perror("setxattr BBVIEW_ATTR_DATATYPE");
        return -1;
    }

    rc = setxattr(path, BBVIEW_ATTR_ETYPE, data->saved_et_buf, data->saved_et_len, 0);
    if (rc < 0) {
        perror("setxattr BBVIEW_ATTR_ETYPE");
        return -1;
    }

    return 0;
}


int
mca_io_bbview_file_flush(struct ompi_file_t *fp)
{
	mca_common_bbview_data_t *data;
    char local_filename[PATH_MAX];
    int sock;
    struct sockaddr_un addr;
	char buf[PATH_MAX];


	data = (mca_common_bbview_data_t *)fp->f_io_selected_data;

    if (data->state != BBVIEW_STATE_ACTIVE) {
        fprintf(stderr, "File is not in active state, skip flush\n");
        return OMPI_SUCCESS;
    }
    fprintf(stderr, "Flushing file %s for view index %ld\n",
            fp->f_filename, data->view_index);

	snprintf(local_filename, sizeof(local_filename), BBVIEW_TMP_DIR "/%s-%d-%ld",
		fp->f_filename, ompi_comm_rank(fp->f_comm), data->view_index);
	for (size_t i = strlen(BBVIEW_TMP_DIR) + 2; i < strlen(local_filename); i++) {
		if (local_filename[i] == '/')
			local_filename[i] = '-';
	}

    if (set_xattrs_for_bbview(local_filename, data) != 0) {
        fprintf(stderr, "Failed to set xattrs for %s\n", local_filename);
        return OMPI_ERROR;
    }

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return OMPI_ERROR;
    }

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, BBVIEW_SOCK, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_un)) == -1) {
        perror("connect");
        close(sock);
        return OMPI_ERROR;
    }

    if (write(sock, local_filename, strlen(local_filename)) != strlen(local_filename)) {
        perror("write");
        close(sock);
        return OMPI_ERROR;
    }

    close(sock);

    return OMPI_SUCCESS;
}
