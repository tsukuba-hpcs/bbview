# -*- shell-script -*-
#
# Copyright (c) 2016      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_ompi_io_bbview_CONFIG([action-if-can-compile],
#                          [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_ompi_io_bbview_CONFIG],[
    AC_CONFIG_FILES([ompi/mca/io/bbview/Makefile])

    AS_IF([test "$enable_io_bbview" != "no"],
          [$1],
          [$2])

    AC_ARG_WITH([bbview-tmp-dir],
    [AS_HELP_STRING([--with-bbview-tmp-dir=DIR], [Specify BBVIEW temporary directory])],
    [BBVIEW_TMP_DIR="$withval"],
    [BBVIEW_TMP_DIR="/tmp"])

    AC_DEFINE_UNQUOTED([BBVIEW_TMP_DIR], ["$BBVIEW_TMP_DIR"], [Temporary directory for BBVIEW])
    AC_SUBST([BBVIEW_TMP_DIR])

])dnl
