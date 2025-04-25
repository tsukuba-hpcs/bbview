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
#include "io_bbview.h"

static int datatype_duplicate(ompi_datatype_t *oldtype,
			      ompi_datatype_t **newtype);
static int
datatype_duplicate(ompi_datatype_t *oldtype, ompi_datatype_t **newtype)
{
	ompi_datatype_t *type;
	if (ompi_datatype_is_predefined(oldtype)) {
		OBJ_RETAIN(oldtype);
		*newtype = oldtype;
		return OMPI_SUCCESS;
	}

	if (OMPI_SUCCESS != ompi_datatype_duplicate(oldtype, &type)) {
		ompi_datatype_destroy(&type);
		return MPI_ERR_INTERN;
	}

	ompi_datatype_set_args(type, 0, NULL, 0, NULL, 1, &oldtype,
			       MPI_COMBINER_DUP);

	*newtype = type;
	return OMPI_SUCCESS;
}

int
mca_io_bbview_file_set_view(ompi_file_t *fp, OMPI_MPI_OFFSET_TYPE disp,
			    ompi_datatype_t *etype, ompi_datatype_t *filetype,
			    const char *datarep, opal_info_t *info)
{
	int ret = OMPI_SUCCESS;
	mca_common_bbview_data_t *data;
	ompio_file_t *fh;
	char local_filename[PATH_MAX];

	if ((strcmp(datarep, "native") && strcmp(datarep, "NATIVE"))) {
		return MPI_ERR_UNSUPPORTED_DATAREP;
	}

	data = (mca_common_bbview_data_t *)fp->f_io_selected_data;

	/* we need to call the internal file set view twice: once for the
	   individual file pointer, once for the shared file pointer (if it is
	   existent)
	*/
	fh = &data->ompio_fh;

	if (MPI_DISPLACEMENT_CURRENT == disp &&
	    !(fh->f_amode & MPI_MODE_SEQUENTIAL)) {
		// MPI_DISPLACEMENT_CURRENT is only valid if amode is
		// MPI_MODE_SEQUENTIAL
		return MPI_ERR_DISP;
	}

	if (fh->f_atomicity ||
		(fp->f_amode & MPI_MODE_RDONLY) ||
		(fp->f_amode & MPI_MODE_RDWR)) {
		OPAL_THREAD_LOCK(&fp->f_lock);
		ret = mca_common_ompio_set_view(fh, disp, etype, filetype, datarep, info);
		OPAL_THREAD_UNLOCK(&fp->f_lock);
		return ret;
	}

	OPAL_THREAD_LOCK(&fp->f_lock);
	if (data->view_index) {
		/* flush the previous view */
		ret = mca_io_bbview_file_flush(fp);
		if (OMPI_SUCCESS != ret) {
			OPAL_THREAD_UNLOCK(&fp->f_lock);
			return ret;
		}
		data->saved_datarep = NULL;
		data->saved_info = NULL;
	}

	data->view_index++;
	ompi_datatype_type_size(etype, &data->saved_etype_size);
	if (datarep)
		data->saved_datarep = strdup(datarep);
	if (info)
		opal_info_dup(info, &data->saved_info);
	data->saved_etype = etype;
	data->saved_filetype = filetype;
	data->saved_disp = disp;

    int num_ints, num_addrs, num_types, combiner;
retry:
    PMPI_Type_get_envelope(filetype, &num_ints, &num_addrs, &num_types, &combiner);
	int *ints = malloc(sizeof(int) * num_ints);
	MPI_Aint *addrs = malloc(sizeof(MPI_Aint) * num_addrs);
	MPI_Datatype *types = malloc(sizeof(MPI_Datatype) * num_types);
	if (combiner == MPI_COMBINER_VECTOR) {
		PMPI_Type_get_contents(filetype, num_ints, num_addrs, num_types, ints, addrs, types);
		data->saved_count = ints[0];
		data->saved_blklen = ints[1];
		data->saved_stride = ints[2];
		free(ints);
		free(addrs);
		free(types);
	} else if (combiner == MPI_COMBINER_DUP) {
		MPI_Type_get_contents(filetype, num_ints, num_addrs, num_types, ints, addrs, types);
		filetype = types[0];
		free(ints);
		free(addrs);
		free(types);
		goto retry;
	} else {
		fprintf(stderr, "Unsupported filetype combiner: %d\n", combiner);
		OPAL_THREAD_UNLOCK(&fp->f_lock);
		return OMPI_ERR_NOT_SUPPORTED;
	}
	close(fh->fd);
	snprintf(local_filename, sizeof(local_filename), BBVIEW_TMP_DIR "/%s-%d-%ld",
		fp->f_filename, ompi_comm_rank(fp->f_comm), data->view_index);
	for (size_t i = strlen(BBVIEW_TMP_DIR) + 2; i < strlen(local_filename); i++) {
		if (local_filename[i] == '/')
			local_filename[i] = '-';
	}
	int flags = O_RDWR | O_CREAT | O_TRUNC;
	if ((data->saved_blklen * data->saved_etype_size) % 4096 == 0) {
		flags |= O_DIRECT;
	}
	fh->fd = open(local_filename, flags, 0666);
	if (fh->fd < 0) {
		OPAL_THREAD_UNLOCK(&fp->f_lock);
		return OMPI_ERROR;
	}
	
	ret =
		mca_common_ompio_set_view(fh, 0, etype, etype, datarep, info);

	OPAL_THREAD_UNLOCK(&fp->f_lock);
	return ret;
}

int
mca_io_bbview_file_get_view(struct ompi_file_t *fp, OMPI_MPI_OFFSET_TYPE *disp,
			    struct ompi_datatype_t **etype,
			    struct ompi_datatype_t **filetype, char *datarep)
{
	mca_common_bbview_data_t *data;
	ompio_file_t *fh;

	data = (mca_common_bbview_data_t *)fp->f_io_selected_data;
	fh = &data->ompio_fh;

	OPAL_THREAD_LOCK(&fp->f_lock);
	*disp = data->saved_disp;
	datatype_duplicate(data->saved_etype, etype);
	datatype_duplicate(data->saved_filetype, filetype);
	strcpy(datarep, data->saved_datarep);
	OPAL_THREAD_UNLOCK(&fp->f_lock);

	return OMPI_SUCCESS;
}
