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
#include <strings.h> 
#include "io_bbview.h"

extern struct ompi_predefined_datatype_t ompi_mpi_byte;

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

static bool enable_directio(ompi_datatype_t *filetype,
	int pagesize, int threshold)
{
	int ni, na, nd, combiner;
	int ret;
	int *i = NULL;
	MPI_Aint *a = NULL;
	ompi_datatype_t **d = NULL;
	ret = PMPI_Type_get_envelope(filetype, &ni, &na, &nd, &combiner);
    if(ret != MPI_SUCCESS)
		return false;
	i = malloc(sizeof(int) * ni);
	a = malloc(sizeof(MPI_Aint) * na);
	d = malloc(sizeof(ompi_datatype_t *) * nd);

	switch (combiner) {
	case MPI_COMBINER_NAMED: {
		int size;
		ret = PMPI_Type_size(filetype, &size);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (size == 0)
			goto disable;
		if (size % pagesize != 0 || size < threshold)
			goto disable;
		goto enable;
	}
	case MPI_COMBINER_DUP: {
		ret = PMPI_Type_get_contents(filetype, ni, na, nd,
					      i, a, d);
		if (ret != MPI_SUCCESS)
			goto disable;
		ret = enable_directio(d[0], pagesize, threshold);
		if (ret)
			goto enable;
		goto disable;
	}
	case MPI_COMBINER_CONTIGUOUS: {
		int count;
		int size;
		ret = PMPI_Type_get_contents(filetype, ni, na, nd,
					      i, a, d);
		if (ret != MPI_SUCCESS)
			goto disable;
		count = i[0];
		if (count == 0)
			goto disable;
		ret = enable_directio(d[0], pagesize, threshold);
		if (ret)
			goto enable;
		ret = PMPI_Type_get_envelope(d[0], &ni, &na, &nd, &combiner);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (combiner != MPI_COMBINER_NAMED)
			goto disable;
		ret = PMPI_Type_size(d[0], &size);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (size == 0)
			goto disable;
		if ((size * count) % pagesize != 0 || (size * count) < threshold)
			goto disable;
		goto enable;
	}
	case MPI_COMBINER_VECTOR:
	case MPI_COMBINER_HVECTOR: {
		int size;
		ret = PMPI_Type_get_contents(filetype, ni, na, nd,
					      i, a, d);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (i[0] == 0)
			goto disable;
		ret = enable_directio(d[0], pagesize, threshold);
		if (ret)
			goto enable;
		ret = PMPI_Type_get_envelope(d[0], &ni, &na, &nd, &combiner);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (combiner != MPI_COMBINER_NAMED)
			goto disable;
		ret = PMPI_Type_size(d[0], &size);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (size == 0)
			goto disable;
		if ((size * i[1]) % pagesize != 0 || (size * i[1]) < threshold)
			goto disable;
		goto enable;
	}
	case MPI_COMBINER_INDEXED:
	case MPI_COMBINER_HINDEXED: {
		int size;
		ret = PMPI_Type_get_contents(filetype, ni, na, nd,
					      i, a, d);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (i[0] == 0)
			goto disable;
		ret = enable_directio(d[0], pagesize, threshold);
		if (ret)
			goto enable;
		ret = PMPI_Type_get_envelope(d[0], &ni, &na, &nd, &combiner);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (combiner != MPI_COMBINER_NAMED)
			goto disable;
		ret = PMPI_Type_size(d[0], &size);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (size == 0)
			goto disable;
		for (int c = 0; c < i[0]; c++) {
			if ((size * i[c+1]) % pagesize != 0 || 
			    (size * i[c+1]) < threshold)
				goto disable;
		}
		goto enable;
	}
	case MPI_COMBINER_INDEXED_BLOCK:
	case MPI_COMBINER_HINDEXED_BLOCK: {
		int size;
		ret = PMPI_Type_get_contents(filetype, ni, na, nd,
					      i, a, d);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (i[0] == 0)
			goto disable;
		ret = enable_directio(d[0], pagesize, threshold);
		if (ret)
			goto enable;
		ret = PMPI_Type_get_envelope(d[0], &ni, &na, &nd, &combiner);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (combiner != MPI_COMBINER_NAMED)
			goto disable;
		ret = PMPI_Type_size(d[0], &size);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (size == 0)
			goto disable;
		if ((size * i[1]) % pagesize != 0 || 
		    (size * i[1]) < threshold)
			goto disable;
		goto enable;
	}
	case MPI_COMBINER_STRUCT: {
		int size;
		ret = PMPI_Type_get_contents(filetype, ni, na, nd,
					      i, a, d);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (i[0] == 0)
			goto disable;
		for (int c = 0; c < i[0]; c++) {
			if (enable_directio(d[c], pagesize, threshold))
				continue;
			ret = PMPI_Type_get_envelope(d[c], &ni, &na, &nd,
						     &combiner);
			if (ret != MPI_SUCCESS)
				goto disable;
			if (combiner != MPI_COMBINER_NAMED)
				goto disable;
			ret = PMPI_Type_size(d[c], &size);
			if (ret != MPI_SUCCESS)
				goto disable;
			if (size == 0)
				goto disable;
			if ((size * i[c+1]) % pagesize != 0 ||
			    (size * i[c+1]) < threshold)
				goto disable;
		}
		goto enable;
	}
	case MPI_COMBINER_SUBARRAY: {
		int size;
		int order;
		ret = PMPI_Type_get_contents(filetype, ni, na, nd,
					      i, a, d);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (i[0] == 0)
			goto disable;
		ret = enable_directio(d[0], pagesize, threshold);
		if (ret)
			goto enable;
		ret = PMPI_Type_get_envelope(d[0], &ni, &na, &nd, &combiner);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (combiner != MPI_COMBINER_NAMED)
			goto disable;
		ret = PMPI_Type_size(d[0], &size);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (size == 0)
			goto disable;
		order = i[3*i[0]+1];
		if (order == MPI_ORDER_C) {
			if ((size * i[2*i[0]]) % pagesize != 0 ||
			    (size * i[2*i[0]]) < threshold)
				goto disable;
		} else if (order == MPI_ORDER_FORTRAN) {
			if ((size * i[i[0]+1]) % pagesize != 0 ||
			    (size * i[i[0]+1]) < threshold)
				goto disable;
		} else {
			goto disable;
		}
		goto enable;
	}
	case MPI_COMBINER_DARRAY: {
		int size;
		int order;
		int ndims;
		int dim;
		int distrib;
		ret = PMPI_Type_get_contents(filetype, ni, na, nd,
					      i, a, d);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (i[0] == 0)
			goto disable;
		ret = enable_directio(d[0], pagesize, threshold);
		if (ret)
			goto enable;
		ret = PMPI_Type_get_envelope(d[0], &ni, &na, &nd, &combiner);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (combiner != MPI_COMBINER_NAMED)
			goto disable;
		ret = PMPI_Type_size(d[0], &size);
		if (ret != MPI_SUCCESS)
			goto disable;
		if (size == 0)
			goto disable;
		order = i[4*i[2]+3];
		ndims = i[2];
		if (order == MPI_ORDER_C)
			dim = ndims - 1;
		else if (order == MPI_ORDER_FORTRAN)
			dim = 0;
		else
			goto disable;
		distrib = i[ndims+3 + dim];
		if (distrib == MPI_DISTRIBUTE_NONE) {
			if ((size * i[3 + dim]) % pagesize != 0 ||
			    (size * i[3 + dim]) < threshold)
				goto disable;
			goto enable;
		} else if ( distrib == MPI_DISTRIBUTE_BLOCK) {
			int darg = i[2*ndims+3 + dim];
			int block_size = (darg == MPI_DISTRIBUTE_DFLT_DARG) ?
				(i[3 + dim] / i[3*ndims+3+dim]) : darg;
			if ((size * block_size) % pagesize != 0 ||
			    (size * block_size) < threshold)
				goto disable;
			goto enable;
		} else {
			goto disable;
		}
	}
	case MPI_COMBINER_F90_REAL:
	case MPI_COMBINER_F90_COMPLEX:
	case MPI_COMBINER_F90_INTEGER:
	case MPI_COMBINER_RESIZED:
	    goto disable;
	}
enable:
	free(i);
	free(a);
	free(d);
	return true;
disable:
	free(i);
	free(a);
	free(d);
	return false;
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

	OPAL_THREAD_LOCK(&fp->f_lock);

	if (disp == 0 && etype == &ompi_mpi_byte.dt && 
	    filetype == &ompi_mpi_byte.dt) {
		if (data->state == BBVIEW_STATE_ACTIVE) {
			ret = mca_io_bbview_file_flush(fp);
			if (OMPI_SUCCESS != ret) {
				OPAL_THREAD_UNLOCK(&fp->f_lock);
				return ret;
			}
			data->saved_datarep = NULL;
			close(fh->fd);
			fh->fd = open(fp->f_filename, O_RDWR);
		}
		if (data->state != BBVIEW_STATE_FALLBACK)
			data->state = BBVIEW_STATE_DEFAULT;
		ret = mca_common_ompio_set_view(fh, disp, etype, filetype, datarep, info);
		OPAL_THREAD_UNLOCK(&fp->f_lock);
		return ret;
	}

	if (data->state == BBVIEW_STATE_FALLBACK) {
		ret = mca_common_ompio_set_view(fh, disp, etype, filetype, datarep, info);
		OPAL_THREAD_UNLOCK(&fp->f_lock);
		return ret;
	} else if (data->state == BBVIEW_STATE_ACTIVE) {
		/* flush the previous view */
		ret = mca_io_bbview_file_flush(fp);
		if (OMPI_SUCCESS != ret) {
			OPAL_THREAD_UNLOCK(&fp->f_lock);
			return ret;
		}
		data->saved_datarep = NULL;
	}

	data->state = BBVIEW_STATE_ACTIVE;
	data->view_index++;
	if (datarep)
		data->saved_datarep = strdup(datarep);
	data->saved_etype = etype;
	data->saved_filetype = filetype;
	data->saved_disp = disp;

	data->saved_dt_len = ompi_datatype_pack_description_length(filetype);
	void *b = NULL;
	ret = ompi_datatype_get_pack_description(filetype, (const void **)&b);
	if (ret != OMPI_SUCCESS) {
		OPAL_THREAD_UNLOCK(&fp->f_lock);
		return ret;
	}
	data->saved_dt_buf = malloc(data->saved_dt_len);
	memcpy(data->saved_dt_buf, b, data->saved_dt_len);

	data->saved_et_len = ompi_datatype_pack_description_length(etype);
	ret = ompi_datatype_get_pack_description(etype, (const void **)&b);
	if (ret != OMPI_SUCCESS) {
		OPAL_THREAD_UNLOCK(&fp->f_lock);
		return ret;
	}
	data->saved_et_buf = malloc(data->saved_et_len);
	memcpy(data->saved_et_buf, b, data->saved_et_len);

	ptrdiff_t extent;
	ret = ompi_datatype_type_extent(filetype, &extent);
	if (ret != MPI_SUCCESS) {
		OPAL_THREAD_UNLOCK(&fp->f_lock);
		return ret;
	}
	close(fh->fd);
	snprintf(local_filename, sizeof(local_filename), BBVIEW_TMP_DIR "/%s-%d-%ld",
		fp->f_filename, ompi_comm_rank(fp->f_comm), data->view_index);
	for (size_t i = strlen(BBVIEW_TMP_DIR) + 2; i < strlen(local_filename); i++) {
		if (local_filename[i] == '/')
			local_filename[i] = '-';
	}
	int flags = O_RDWR | O_CREAT | O_TRUNC;
	if (enable_directio(filetype, 4096, 512*1024))
		flags |= O_DIRECT;
	opal_cstring_t *dio_buf = NULL;
	int dio_set = 0;
	if (info)
		opal_info_get(info, "direct_io", &dio_buf,
                                &dio_set);
	if (dio_set) {
		bool rc;
		ret = opal_cstring_to_bool(dio_buf, &rc);
		if (ret == OPAL_SUCCESS && rc)
			flags |= O_DIRECT;
		else
			flags &= ~O_DIRECT;
	}
	if (flags & O_DIRECT) {
		fprintf(stderr, "Using O_DIRECT for %s\n", local_filename);
	} else {
		fprintf(stderr, "Not using O_DIRECT for %s\n", local_filename);
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
