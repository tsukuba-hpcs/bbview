BBView: A View-Aware Burst-Buffer Mechanism for MPI-IO
===
 
BBView is a lightweight, semantics-aware burst-buffer extension for Open MPI that accelerates MPI-IO checkpoint performance on HPC systems. By elevating the MPI file view to the fundamental unit of buffering, BBView captures application intent directly—without requiring application changes, kernel patches, or alternative mount points.

## Build

```bash
$ git clone --recursive https://github.com/open-mpi/ompi
$ cd ompi
$ git submodule add https://github.com/tsukuba-hpcs/bbview ompi/mca/io/bbview
$ ./autogen.pl
$ ./configure --with-bbview-tmp-dir=/scr/
# Modify OpenMPI code (See `Patch to OpenMPI`)
$ make -k
$ make # to build bbviewd
$ make install
```

### Patch to OpenMPI

```
--- a/ompi/mca/io/base/io_base_file_select.c
+++ b/ompi/mca/io/base/io_base_file_select.c
@@ -198,7 +198,9 @@ int mca_io_base_file_select(ompi_file_t *file,
     OBJ_RELEASE(selectable);
 
     if (!strcmp (selected.ai_component.v3_0_0.io_version.mca_component_name,
-                 "ompio")) {
+                 "ompio") ||
+        !strcmp (selected.ai_component.v3_0_0.io_version.mca_component_name,
+                 "bbview")) {
         int ret;
```

# Run

1. Launch bbviewd on background. Each compute node has 1 bbviewd process.

```bash
$ ssh $node bbviewd 1 & # number of threads to flush
```

2. Launch MPI app.

```bash
$ mpirun -np 1 --mca io bbview --mca fcoll individual ./a.out
```

3. Terminate bbviewd

```bash
$ ssh $node bbviewd wait
```
