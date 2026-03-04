AC_PREREQ([2.69])
AC_INIT([sparsemap], [1.0.0], [greg@burd.me])
AC_CONFIG_SRCDIR([src/sparsemap.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([1.15 foreign -Wall -Werror subdir-objects])
AM_SILENT_RULES([yes])

# Checks for programs
AC_PROG_CC
AC_PROG_CC_C99
AM_PROG_AR
LT_INIT

# Compiler flags
AX_CHECK_COMPILE_FLAG([-std=gnu11], [CFLAGS="$CFLAGS -std=gnu11"], [
  AC_MSG_ERROR([compiler does not support -std=gnu11])
])

# Enable all warnings
AX_CHECK_COMPILE_FLAG([-Wall], [CFLAGS="$CFLAGS -Wall"])
AX_CHECK_COMPILE_FLAG([-Wextra], [CFLAGS="$CFLAGS -Wextra"])

# Checks for libraries
AC_CHECK_LIB([pthread], [pthread_create])

# Checks for header files
AC_CHECK_HEADERS([stdint.h stdlib.h string.h sys/time.h unistd.h pthread.h])

# Checks for typedefs, structures, and compiler characteristics
AC_CHECK_HEADER_STDBOOL
AC_TYPE_SIZE_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_UINT8_T

# Checks for library functions
AC_FUNC_MALLOC
AC_CHECK_FUNCS([clock_gettime gettimeofday])

# Check for __builtin_popcount
AC_MSG_CHECKING([for __builtin_popcount])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([],
  [[return __builtin_popcount(0);]])],
  [AC_MSG_RESULT([yes])
   AC_DEFINE([HAVE_BUILTIN_POPCOUNT], [1],
     [Define to 1 if you have __builtin_popcount])],
  [AC_MSG_RESULT([no])])

# Check for __builtin_popcountll
AC_MSG_CHECKING([for __builtin_popcountll])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([],
  [[return __builtin_popcountll(0);]])],
  [AC_MSG_RESULT([yes])
   AC_DEFINE([HAVE_BUILTIN_POPCOUNTLL], [1],
     [Define to 1 if you have __builtin_popcountll])],
  [AC_MSG_RESULT([no])])

# Optional features
AC_ARG_ENABLE([debug],
  [AS_HELP_STRING([--enable-debug], [enable debug mode])],
  [enable_debug=$enableval],
  [enable_debug=no])

AS_IF([test "x$enable_debug" = "xyes"], [
  AC_DEFINE([DEBUG], [1], [Define to 1 if debug mode is enabled])
  CFLAGS="$CFLAGS -g -O0"
], [
  CFLAGS="$CFLAGS -O3"
])

AC_ARG_ENABLE([diagnostic],
  [AS_HELP_STRING([--enable-diagnostic], [enable diagnostic output])],
  [enable_diagnostic=$enableval],
  [enable_diagnostic=no])

AS_IF([test "x$enable_diagnostic" = "xyes"], [
  AC_DEFINE([SPARSEMAP_DIAGNOSTIC], [1], [Define to 1 if diagnostic output is enabled])
])

# Enable testing support
AC_ARG_ENABLE([testing],
  [AS_HELP_STRING([--enable-testing], [enable testing support])],
  [enable_testing=$enableval],
  [enable_testing=yes])

AS_IF([test "x$enable_testing" = "xyes"], [
  AC_DEFINE([SPARSEMAP_TESTING], [1], [Define to 1 if testing support is enabled])
])

AM_CONDITIONAL([ENABLE_TESTING], [test "x$enable_testing" = "xyes"])

AC_CONFIG_FILES([
  Makefile
  src/Makefile
  tests/Makefile
  examples/Makefile
  sparsemap.pc
])

AC_OUTPUT

AC_MSG_RESULT([
  $PACKAGE_NAME $PACKAGE_VERSION

  prefix:      $prefix
  compiler:    $CC
  cflags:      $CFLAGS
  ldflags:     $LDFLAGS

  debug:       $enable_debug
  diagnostic:  $enable_diagnostic
  testing:     $enable_testing
])
