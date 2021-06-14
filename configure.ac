AC_PREREQ([2.69])
define(_CLIENT_VERSION_MAJOR, 21)
define(_CLIENT_VERSION_MINOR, 99)
define(_CLIENT_VERSION_BUILD, 0)
define(_CLIENT_VERSION_RC, 0)
define(_CLIENT_VERSION_IS_RELEASE, false)
define(_COPYRIGHT_YEAR, 2021)
define(_COPYRIGHT_HOLDERS,[The %s developers])
define(_COPYRIGHT_HOLDERS_SUBSTITUTION,[[Bitcoin Core]])
AC_INIT([Bitcoin Core],m4_join([.], _CLIENT_VERSION_MAJOR, _CLIENT_VERSION_MINOR, _CLIENT_VERSION_BUILD)m4_if(_CLIENT_VERSION_RC, [0], [], [rc]_CLIENT_VERSION_RC),[https://github.com/bitcoin/bitcoin/issues],[bitcoin],[https://bitcoincore.org/])
AC_CONFIG_SRCDIR([src/validation.cpp])
AC_CONFIG_HEADERS([src/config/bitcoin-config.h])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([build-aux/m4])

m4_ifndef([PKG_PROG_PKG_CONFIG], [AC_MSG_ERROR([PKG_PROG_PKG_CONFIG macro not found. Please install pkg-config and re-run autogen.sh])])
PKG_PROG_PKG_CONFIG
if test "x$PKG_CONFIG" = x; then
  AC_MSG_ERROR([pkg-config not found])
fi

BITCOIN_DAEMON_NAME=bitcoind
BITCOIN_GUI_NAME=bitcoin-qt
BITCOIN_CLI_NAME=bitcoin-cli
BITCOIN_TX_NAME=bitcoin-tx
BITCOIN_UTIL_NAME=bitcoin-util
BITCOIN_WALLET_TOOL_NAME=bitcoin-wallet
dnl Multi Process
BITCOIN_MP_NODE_NAME=bitcoin-node
BITCOIN_MP_GUI_NAME=bitcoin-gui

dnl Unless the user specified ARFLAGS, force it to be cr
AC_ARG_VAR(ARFLAGS, [Flags for the archiver, defaults to <cr> if not set])
if test "x${ARFLAGS+set}" != "xset"; then
  ARFLAGS="cr"
fi

AC_CANONICAL_HOST

AH_TOP([#ifndef BITCOIN_CONFIG_H])
AH_TOP([#define BITCOIN_CONFIG_H])
AH_BOTTOM([#endif //BITCOIN_CONFIG_H])

dnl faketime breaks configure and is only needed for make. Disable it here.
unset FAKETIME

dnl Automake init set-up and checks
AM_INIT_AUTOMAKE([1.13 no-define subdir-objects foreign])

dnl faketime messes with timestamps and causes configure to be re-run.
dnl --disable-maintainer-mode can be used to bypass this.
AM_MAINTAINER_MODE([enable])

# Make the compilation flags quiet unless V=1 is used.
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

dnl Compiler checks (here before libtool).
if test "x${CXXFLAGS+set}" = "xset"; then
  CXXFLAGS_overridden=yes
else
  CXXFLAGS_overridden=no
fi
AC_PROG_CXX

dnl By default, libtool for mingw refuses to link static libs into a dll for
dnl fear of mixing pic/non-pic objects, and import/export complications. Since
dnl we have those under control, re-enable that functionality.
case $host in
  *mingw*)
     lt_cv_deplibs_check_method="pass_all"
  ;;
esac

dnl Require C++17 compiler (no GNU extensions)
AX_CXX_COMPILE_STDCXX([17], [noext], [mandatory])

dnl Check if -latomic is required for <std::atomic>
CHECK_ATOMIC

dnl Unless the user specified OBJCXX, force it to be the same as CXX. This ensures
dnl that we get the same -std flags for both.
m4_ifdef([AC_PROG_OBJCXX],[
if test "x${OBJCXX+set}" = "x"; then
  OBJCXX="${CXX}"
fi
AC_PROG_OBJCXX
])

dnl Since libtool 1.5.2 (released 2004-01-25), on Linux libtool no longer
dnl sets RPATH for any directories in the dynamic linker search path.
dnl See more: https://wiki.debian.org/RpathIssue
LT_PREREQ([1.5.2])
dnl Libtool init checks.
LT_INIT([pic-only])

dnl Check/return PATH for base programs.
AC_PATH_TOOL(AR, ar)
AC_PATH_TOOL(RANLIB, ranlib)
AC_PATH_TOOL(STRIP, strip)

# Save definition of AC_PROG_CC because AM_PROG_CC_C_O in automake<=1.13 will
# redefine AC_PROG_CC to exit with an error, which avoids the user calling it
# accidently and screwing up the effect of AM_PROG_CC_C_O. However, we'll need
# AC_PROG_CC later on in AX_PROG_CC_FOR_BUILD, where its usage is fine, and
# we'll carefully make sure not to call AC_PROG_CC anywhere else.
m4_copy([AC_PROG_CC], [saved_AC_PROG_CC])
AM_PROG_CC_C_O
# Restore AC_PROG_CC
m4_rename_force([saved_AC_PROG_CC], [AC_PROG_CC])

AC_ARG_ENABLE([threadlocal],
  [AS_HELP_STRING([--enable-threadlocal],
  [enable features that depend on the c++ thread_local keyword (currently just thread names in debug logs). (default is to enabled if there is platform support and glibc-back-compat is not enabled)])],
  [use_thread_local=$enableval],
  [use_thread_local=auto])

AC_ARG_ENABLE([asm],
  [AS_HELP_STRING([--disable-asm],
  [disable assembly routines (enabled by default)])],
  [use_asm=$enableval],
  [use_asm=yes])

if test "x$use_asm" = xyes; then
  AC_DEFINE(USE_ASM, 1, [Define this symbol to build in assembly routines])
fi

AC_ARG_WITH([system-univalue],
  [AS_HELP_STRING([--with-system-univalue],
  [Build with system UniValue (default is no)])],
  [system_univalue=$withval],
  [system_univalue=no]
)
AC_ARG_ENABLE([zmq],
  [AS_HELP_STRING([--disable-zmq],
  [disable ZMQ notifications])],
  [use_zmq=$enableval],
  [use_zmq=yes])

AC_ARG_WITH([libmultiprocess],
  [AS_HELP_STRING([--with-libmultiprocess=yes|no|auto],
  [Build with libmultiprocess library. (default: auto, i.e. detect with pkg-config)])],
  [with_libmultiprocess=$withval],
  [with_libmultiprocess=auto])

AC_ARG_WITH([mpgen],
  [AS_HELP_STRING([--with-mpgen=yes|no|auto|PREFIX],
  [Build with libmultiprocess codegen tool. Useful to specify different libmultiprocess host system library and build system codegen tool prefixes when cross-compiling (default is host system libmultiprocess prefix)])],
  [with_mpgen=$withval],
  [with_mpgen=auto])

AC_ARG_ENABLE([multiprocess],
  [AS_HELP_STRING([--enable-multiprocess],
  [build multiprocess bitcoin-node, bitcoin-wallet, and bitcoin-gui executables in addition to monolithic bitcoind and bitcoin-qt executables. Requires libmultiprocess library. Experimental (default is no)])],
  [enable_multiprocess=$enableval],
  [enable_multiprocess=no])

AC_ARG_ENABLE(man,
    [AS_HELP_STRING([--disable-man],
                    [do not install man pages (default is to install)])],,
    enable_man=yes)
AM_CONDITIONAL(ENABLE_MAN, test "$enable_man" != no)

dnl Enable debug
AC_ARG_ENABLE([debug],
    [AS_HELP_STRING([--enable-debug],
                    [use compiler flags and macros suited for debugging (default is no)])],
    [enable_debug=$enableval],
    [enable_debug=no])

dnl Enable different -fsanitize options
AC_ARG_WITH([sanitizers],
    [AS_HELP_STRING([--with-sanitizers],
                    [comma separated list of extra sanitizers to build with (default is none enabled)])],
    [use_sanitizers=$withval])

dnl Enable gprof profiling
AC_ARG_ENABLE([gprof],
    [AS_HELP_STRING([--enable-gprof],
                    [use gprof profiling compiler flags (default is no)])],
    [enable_gprof=$enableval],
    [enable_gprof=no])

dnl Pass compiler & linker flags that make builds deterministic
AC_ARG_ENABLE([determinism],
    [AS_HELP_STRING([--enable-determinism],
                    [Enable compilation flags that make builds deterministic (default is no)])],
    [enable_determinism=$enableval],
    [enable_determinism=no])

dnl Turn warnings into errors
AC_ARG_ENABLE([werror],
    [AS_HELP_STRING([--enable-werror],
                    [Treat certain compiler warnings as errors (default is no)])],
    [enable_werror=$enableval],
    [enable_werror=no])

AC_ARG_ENABLE([external-signer],
    [AS_HELP_STRING([--enable-external-signer],[compile external signer support (default is yes, requires Boost::Process)])],
    [use_external_signer=$enableval],
    [use_external_signer=yes])

AC_LANG_PUSH([C++])

dnl Check for a flag to turn compiler warnings into errors. This is helpful for checks which may
dnl appear to succeed because by default they merely emit warnings when they fail.
dnl
dnl Note that this is not necessarily a check to see if -Werror is supported, but rather to see if
dnl a compile with -Werror can succeed. This is important because the compiler may already be
dnl warning about something unrelated, for example about some path issue. If that is the case,
dnl -Werror cannot be used because all of those warnings would be turned into errors.
AX_CHECK_COMPILE_FLAG([-Werror],[CXXFLAG_WERROR="-Werror"],[CXXFLAG_WERROR=""])

dnl Check for a flag to turn linker warnings into errors. When flags are passed to linkers via the
dnl compiler driver using a -Wl,-foo flag, linker warnings may be swallowed rather than bubbling up.
dnl See note above, the same applies here as well.
dnl
dnl LDFLAG_WERROR Should only be used when testing -Wl,*
case $host in
 *darwin*)
    AX_CHECK_LINK_FLAG([-Wl,-fatal_warnings],[LDFLAG_WERROR="-Wl,-fatal_warnings"],[LDFLAG_WERROR=""])
    ;;
  *)
    AX_CHECK_LINK_FLAG([-Wl,--fatal-warnings],[LDFLAG_WERROR="-Wl,--fatal-warnings"],[LDFLAG_WERROR=""])
    ;;
esac

if test "x$enable_debug" = xyes; then
  dnl Clear default -g -O2 flags
  if test "x$CXXFLAGS_overridden" = xno; then
	CXXFLAGS=""
  fi

  dnl Disable all optimizations
  AX_CHECK_COMPILE_FLAG([-O0], [[DEBUG_CXXFLAGS="$DEBUG_CXXFLAGS -O0"]],,[[$CXXFLAG_WERROR]])

  dnl Prefer -g3, fall back to -g if that is unavailable.
  AX_CHECK_COMPILE_FLAG(
    [-g3],
    [[DEBUG_CXXFLAGS="$DEBUG_CXXFLAGS -g3"]],
    [AX_CHECK_COMPILE_FLAG([-g],[[DEBUG_CXXFLAGS="$DEBUG_CXXFLAGS -g"]],,[[$CXXFLAG_WERROR]])],
    [[$CXXFLAG_WERROR]])

  AX_CHECK_PREPROC_FLAG([-DDEBUG],[[DEBUG_CPPFLAGS="$DEBUG_CPPFLAGS -DDEBUG"]],,[[$CXXFLAG_WERROR]])
  AX_CHECK_PREPROC_FLAG([-DDEBUG_LOCKORDER],[[DEBUG_CPPFLAGS="$DEBUG_CPPFLAGS -DDEBUG_LOCKORDER"]],,[[$CXXFLAG_WERROR]])
  AX_CHECK_PREPROC_FLAG([-DABORT_ON_FAILED_ASSUME],[[DEBUG_CPPFLAGS="$DEBUG_CPPFLAGS -DABORT_ON_FAILED_ASSUME"]],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-ftrapv],[DEBUG_CXXFLAGS="$DEBUG_CXXFLAGS -ftrapv"],,[[$CXXFLAG_WERROR]])
fi

if test x$use_sanitizers != x; then
  dnl First check if the compiler accepts flags. If an incompatible pair like
  dnl -fsanitize=address,thread is used here, this check will fail. This will also
  dnl fail if a bad argument is passed, e.g. -fsanitize=undfeined
  AX_CHECK_COMPILE_FLAG(
    [[-fsanitize=$use_sanitizers]],
    [[SANITIZER_CXXFLAGS=-fsanitize=$use_sanitizers]],
    [AC_MSG_ERROR([compiler did not accept requested flags])])

  dnl Some compilers (e.g. GCC) require additional libraries like libasan,
  dnl libtsan, libubsan, etc. Make sure linking still works with the sanitize
  dnl flag. This is a separate check so we can give a better error message when
  dnl the sanitize flags are supported by the compiler but the actual sanitizer
  dnl libs are missing.
  AX_CHECK_LINK_FLAG(
    [[-fsanitize=$use_sanitizers]],
    [[SANITIZER_LDFLAGS=-fsanitize=$use_sanitizers]],
    [AC_MSG_ERROR([linker did not accept requested flags, you are missing required libraries])],
    [],
    [AC_LANG_PROGRAM([[
    #include <cstdint>
    #include <cstddef>
    extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { return 0; }
    __attribute__((weak)) // allow for libFuzzer linking
    ]],[[]])])
fi

ERROR_CXXFLAGS=
if test "x$enable_werror" = "xyes"; then
  if test "x$CXXFLAG_WERROR" = "x"; then
    AC_MSG_ERROR("enable-werror set but -Werror is not usable")
  fi
  AX_CHECK_COMPILE_FLAG([-Werror=gnu],[ERROR_CXXFLAGS="$ERROR_CXXFLAGS -Werror=gnu"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Werror=vla],[ERROR_CXXFLAGS="$ERROR_CXXFLAGS -Werror=vla"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Werror=shadow-field],[ERROR_CXXFLAGS="$ERROR_CXXFLAGS -Werror=shadow-field"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Werror=switch],[ERROR_CXXFLAGS="$ERROR_CXXFLAGS -Werror=switch"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Werror=thread-safety],[ERROR_CXXFLAGS="$ERROR_CXXFLAGS -Werror=thread-safety"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Werror=range-loop-analysis],[ERROR_CXXFLAGS="$ERROR_CXXFLAGS -Werror=range-loop-analysis"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Werror=unused-variable],[ERROR_CXXFLAGS="$ERROR_CXXFLAGS -Werror=unused-variable"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Werror=date-time],[ERROR_CXXFLAGS="$ERROR_CXXFLAGS -Werror=date-time"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Werror=return-type],[ERROR_CXXFLAGS="$ERROR_CXXFLAGS -Werror=return-type"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Werror=conditional-uninitialized],[ERROR_CXXFLAGS="$ERROR_CXXFLAGS -Werror=conditional-uninitialized"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Werror=sign-compare],[ERROR_CXXFLAGS="$ERROR_CXXFLAGS -Werror=sign-compare"],,[[$CXXFLAG_WERROR]])
  dnl -Wsuggest-override is broken with GCC before 9.2
  dnl https://gcc.gnu.org/bugzilla/show_bug.cgi?id=78010
  AX_CHECK_COMPILE_FLAG([-Werror=suggest-override],[ERROR_CXXFLAGS="$ERROR_CXXFLAGS -Werror=suggest-override"],,[[$CXXFLAG_WERROR]],
                        [AC_LANG_SOURCE([[struct A { virtual void f(); }; struct B : A { void f() final; };]])])
  AX_CHECK_COMPILE_FLAG([-Werror=unreachable-code-loop-increment],[ERROR_CXXFLAGS="$ERROR_CXXFLAGS -Werror=unreachable-code-loop-increment"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Werror=mismatched-tags], [ERROR_CXXFLAGS="$ERROR_CXXFLAGS -Werror=mismatched-tags"], [], [$CXXFLAG_WERROR])

  if test x$suppress_external_warnings != xno ; then
    AX_CHECK_COMPILE_FLAG([-Werror=documentation],[ERROR_CXXFLAGS="$ERROR_CXXFLAGS -Werror=documentation"],,[[$CXXFLAG_WERROR]])
  fi
fi

if test "x$CXXFLAGS_overridden" = "xno"; then
  AX_CHECK_COMPILE_FLAG([-Wall],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wall"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wextra],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wextra"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wgnu],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wgnu"],,[[$CXXFLAG_WERROR]])
  dnl some compilers will ignore -Wformat-security without -Wformat, so just combine the two here.
  AX_CHECK_COMPILE_FLAG([-Wformat -Wformat-security],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wformat -Wformat-security"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wvla],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wvla"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wshadow-field],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wshadow-field"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wswitch],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wswitch"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wthread-safety],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wthread-safety"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wrange-loop-analysis],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wrange-loop-analysis"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wredundant-decls],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wredundant-decls"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wunused-variable],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wunused-variable"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wunused-member-function],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wunused-member-function"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wdate-time],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wdate-time"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wconditional-uninitialized],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wconditional-uninitialized"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wsign-compare],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wsign-compare"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wduplicated-branches],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wduplicated-branches"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wduplicated-cond],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wduplicated-cond"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wlogical-op],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wlogical-op"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Woverloaded-virtual],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Woverloaded-virtual"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wsuggest-override],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wsuggest-override"],,[[$CXXFLAG_WERROR]],
                        [AC_LANG_SOURCE([[struct A { virtual void f(); }; struct B : A { void f() final; };]])])
  AX_CHECK_COMPILE_FLAG([-Wunreachable-code-loop-increment],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wunreachable-code-loop-increment"],,[[$CXXFLAG_WERROR]])

  if test x$suppress_external_warnings != xno ; then
    AX_CHECK_COMPILE_FLAG([-Wdocumentation],[WARN_CXXFLAGS="$WARN_CXXFLAGS -Wdocumentation"],,[[$CXXFLAG_WERROR]])
  fi

  dnl Some compilers (gcc) ignore unknown -Wno-* options, but warn about all
  dnl unknown options if any other warning is produced. Test the -Wfoo case, and
  dnl set the -Wno-foo case if it works.
  AX_CHECK_COMPILE_FLAG([-Wunused-parameter],[NOWARN_CXXFLAGS="$NOWARN_CXXFLAGS -Wno-unused-parameter"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wself-assign],[NOWARN_CXXFLAGS="$NOWARN_CXXFLAGS -Wno-self-assign"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wunused-local-typedef],[NOWARN_CXXFLAGS="$NOWARN_CXXFLAGS -Wno-unused-local-typedef"],,[[$CXXFLAG_WERROR]])
  AX_CHECK_COMPILE_FLAG([-Wimplicit-fallthrough],[NOWARN_CXXFLAGS="$NOWARN_CXXFLAGS -Wno-implicit-fallthrough"],,[[$CXXFLAG_WERROR]])
  if test x$suppress_external_warnings != xyes ; then
    AX_CHECK_COMPILE_FLAG([-Wdeprecated-copy],[NOWARN_CXXFLAGS="$NOWARN_CXXFLAGS -Wno-deprecated-copy"],,[[$CXXFLAG_WERROR]])
  fi
fi

dnl Don't allow extended (non-ASCII) symbols in identifiers. This is easier for code review.
AX_CHECK_COMPILE_FLAG([-fno-extended-identifiers],[[CXXFLAGS="$CXXFLAGS -fno-extended-identifiers"]],,[[$CXXFLAG_WERROR]])

enable_sse42=no
enable_sse41=no
enable_avx2=no
enable_shani=no

if test "x$use_asm" = "xyes"; then

dnl Check for optional instruction set support. Enabling these does _not_ imply that all code will
dnl be compiled with them, rather that specific objects/libs may use them after checking for runtime
dnl compatibility.

dnl x86
AX_CHECK_COMPILE_FLAG([-msse4.2],[[SSE42_CXXFLAGS="-msse4.2"]],,[[$CXXFLAG_WERROR]])
AX_CHECK_COMPILE_FLAG([-msse4.1],[[SSE41_CXXFLAGS="-msse4.1"]],,[[$CXXFLAG_WERROR]])
AX_CHECK_COMPILE_FLAG([-mavx -mavx2],[[AVX2_CXXFLAGS="-mavx -mavx2"]],,[[$CXXFLAG_WERROR]])
AX_CHECK_COMPILE_FLAG([-msse4 -msha],[[SHANI_CXXFLAGS="-msse4 -msha"]],,[[$CXXFLAG_WERROR]])

TEMP_CXXFLAGS="$CXXFLAGS"
CXXFLAGS="$CXXFLAGS $SSE42_CXXFLAGS"
AC_MSG_CHECKING(for SSE4.2 intrinsics)
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
    #include <stdint.h>
    #if defined(_MSC_VER)
    #include <intrin.h>
    #elif defined(__GNUC__) && defined(__SSE4_2__)
    #include <nmmintrin.h>
    #endif
  ]],[[
    uint64_t l = 0;
    l = _mm_crc32_u8(l, 0);
    l = _mm_crc32_u32(l, 0);
    l = _mm_crc32_u64(l, 0);
    return l;
  ]])],
 [ AC_MSG_RESULT(yes); enable_sse42=yes],
 [ AC_MSG_RESULT(no)]
)
CXXFLAGS="$TEMP_CXXFLAGS"

TEMP_CXXFLAGS="$CXXFLAGS"
CXXFLAGS="$CXXFLAGS $SSE41_CXXFLAGS"
AC_MSG_CHECKING(for SSE4.1 intrinsics)
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
    #include <stdint.h>
    #include <immintrin.h>
  ]],[[
    __m128i l = _mm_set1_epi32(0);
    return _mm_extract_epi32(l, 3);
  ]])],
 [ AC_MSG_RESULT(yes); enable_sse41=yes; AC_DEFINE(ENABLE_SSE41, 1, [Define this symbol to build code that uses SSE4.1 intrinsics]) ],
 [ AC_MSG_RESULT(no)]
)
CXXFLAGS="$TEMP_CXXFLAGS"

TEMP_CXXFLAGS="$CXXFLAGS"
CXXFLAGS="$CXXFLAGS $AVX2_CXXFLAGS"
AC_MSG_CHECKING(for AVX2 intrinsics)
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
    #include <stdint.h>
    #include <immintrin.h>
  ]],[[
    __m256i l = _mm256_set1_epi32(0);
    return _mm256_extract_epi32(l, 7);
  ]])],
 [ AC_MSG_RESULT(yes); enable_avx2=yes; AC_DEFINE(ENABLE_AVX2, 1, [Define this symbol to build code that uses AVX2 intrinsics]) ],
 [ AC_MSG_RESULT(no)]
)
CXXFLAGS="$TEMP_CXXFLAGS"

TEMP_CXXFLAGS="$CXXFLAGS"
CXXFLAGS="$CXXFLAGS $SHANI_CXXFLAGS"
AC_MSG_CHECKING(for SHA-NI intrinsics)
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
    #include <stdint.h>
    #include <immintrin.h>
  ]],[[
    __m128i i = _mm_set1_epi32(0);
    __m128i j = _mm_set1_epi32(1);
    __m128i k = _mm_set1_epi32(2);
    return _mm_extract_epi32(_mm_sha256rnds2_epu32(i, i, k), 0);
  ]])],
 [ AC_MSG_RESULT(yes); enable_shani=yes; AC_DEFINE(ENABLE_SHANI, 1, [Define this symbol to build code that uses SHA-NI intrinsics]) ],
 [ AC_MSG_RESULT(no)]
)
CXXFLAGS="$TEMP_CXXFLAGS"

# ARM
AX_CHECK_COMPILE_FLAG([-march=armv8-a+crc+crypto],[[ARM_CRC_CXXFLAGS="-march=armv8-a+crc+crypto"]],,[[$CXXFLAG_WERROR]])

TEMP_CXXFLAGS="$CXXFLAGS"
CXXFLAGS="$CXXFLAGS $ARM_CRC_CXXFLAGS"
AC_MSG_CHECKING(for ARM CRC32 intrinsics)
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
    #include <arm_acle.h>
    #include <arm_neon.h>
  ]],[[
    __crc32cb(0, 0); __crc32ch(0, 0); __crc32cw(0, 0); __crc32cd(0, 0);
    vmull_p64(0, 0);
  ]])],
 [ AC_MSG_RESULT(yes); enable_arm_crc=yes; ],
 [ AC_MSG_RESULT(no)]
)
CXXFLAGS="$TEMP_CXXFLAGS"

fi

CPPFLAGS="$CPPFLAGS -DHAVE_BUILD_INFO -D__STDC_FORMAT_MACROS"

AC_ARG_WITH([utils],
  [AS_HELP_STRING([--with-utils],
  [build bitcoin-cli bitcoin-tx bitcoin-util bitcoin-wallet (default=yes)])],
  [build_bitcoin_utils=$withval],
  [build_bitcoin_utils=yes])

AC_ARG_ENABLE([util-cli],
  [AS_HELP_STRING([--enable-util-cli],
  [build bitcoin-cli])],
  [build_bitcoin_cli=$enableval],
  [build_bitcoin_cli=$build_bitcoin_utils])

AC_ARG_ENABLE([util-tx],
  [AS_HELP_STRING([--enable-util-tx],
  [build bitcoin-tx])],
  [build_bitcoin_tx=$enableval],
  [build_bitcoin_tx=$build_bitcoin_utils])

AC_ARG_ENABLE([util-wallet],
  [AS_HELP_STRING([--enable-util-wallet],
  [build bitcoin-wallet])],
  [build_bitcoin_wallet=$enableval],
  [build_bitcoin_wallet=$build_bitcoin_utils])

AC_ARG_ENABLE([util-util],
  [AS_HELP_STRING([--enable-util-util],
  [build bitcoin-util])],
  [build_bitcoin_util=$enableval],
  [build_bitcoin_util=$build_bitcoin_utils])

AC_ARG_WITH([libs],
  [AS_HELP_STRING([--with-libs],
  [build libraries (default=yes)])],
  [build_bitcoin_libs=$withval],
  [build_bitcoin_libs=yes])

AC_ARG_WITH([daemon],
  [AS_HELP_STRING([--with-daemon],
  [build bitcoind daemon (default=yes)])],
  [build_bitcoind=$withval],
  [build_bitcoind=yes])

case $host in
  *mingw*)
     TARGET_OS=windows
     AC_CHECK_LIB([kernel32], [GetModuleFileNameA],, AC_MSG_ERROR(libkernel32 missing))
     AC_CHECK_LIB([user32],   [main],, AC_MSG_ERROR(libuser32 missing))
     AC_CHECK_LIB([gdi32],    [main],, AC_MSG_ERROR(libgdi32 missing))
     AC_CHECK_LIB([comdlg32], [main],, AC_MSG_ERROR(libcomdlg32 missing))
     AC_CHECK_LIB([winmm],    [main],, AC_MSG_ERROR(libwinmm missing))
     AC_CHECK_LIB([shell32],  [SHGetSpecialFolderPathW],, AC_MSG_ERROR(libshell32 missing))
     AC_CHECK_LIB([comctl32], [main],, AC_MSG_ERROR(libcomctl32 missing))
     AC_CHECK_LIB([ole32],    [CoCreateInstance],, AC_MSG_ERROR(libole32 missing))
     AC_CHECK_LIB([oleaut32], [main],, AC_MSG_ERROR(liboleaut32 missing))
     AC_CHECK_LIB([uuid],     [main],, AC_MSG_ERROR(libuuid missing))
     AC_CHECK_LIB([advapi32], [CryptAcquireContextW],, AC_MSG_ERROR(libadvapi32 missing))
     AC_CHECK_LIB([ws2_32],   [WSAStartup],, AC_MSG_ERROR(libws2_32 missing))
     AC_CHECK_LIB([shlwapi],  [PathRemoveFileSpecW],, AC_MSG_ERROR(libshlwapi missing))
     AC_CHECK_LIB([iphlpapi], [GetAdaptersAddresses],, AC_MSG_ERROR(libiphlpapi missing))

     dnl -static is interpreted by libtool, where it has a different meaning.
     dnl In libtool-speak, it's -all-static.
     AX_CHECK_LINK_FLAG([[-static]],[LIBTOOL_APP_LDFLAGS="$LIBTOOL_APP_LDFLAGS -all-static"])

     AC_PATH_PROG([MAKENSIS], [makensis], none)
     if test x$MAKENSIS = xnone; then
       AC_MSG_WARN("makensis not found. Cannot create installer.")
     fi

     AC_PATH_TOOL(WINDRES, windres, none)
     if test x$WINDRES = xnone; then
       AC_MSG_ERROR("windres not found")
     fi

     CPPFLAGS="$CPPFLAGS -D_MT -DWIN32 -D_WINDOWS -D_WIN32_WINNT=0x0601 -D_WIN32_IE=0x0501 -DWIN32_LEAN_AND_MEAN"

     dnl libtool insists upon adding -nostdlib and a list of objects/libs to link against.
     dnl That breaks our ability to build dll's with static libgcc/libstdc++/libssp. Override
     dnl its command here, with the predeps/postdeps removed, and -static inserted. Postdeps are
     dnl also overridden to prevent their insertion later.
     dnl This should only affect dll's.
     archive_cmds_CXX="\$CC -shared \$libobjs \$deplibs \$compiler_flags -static -o \$output_objdir/\$soname \${wl}--enable-auto-image-base -Xlinker --out-implib -Xlinker \$lib"
     postdeps_CXX=

     dnl We require Windows 7 (NT 6.1) or later
     AX_CHECK_LINK_FLAG([[-Wl,--major-subsystem-version -Wl,6 -Wl,--minor-subsystem-version -Wl,1]],[LDFLAGS="$LDFLAGS -Wl,--major-subsystem-version -Wl,6 -Wl,--minor-subsystem-version -Wl,1"],,[[$LDFLAG_WERROR]])
     ;;
  *darwin*)
     TARGET_OS=darwin
     if  test x$cross_compiling != xyes; then
       AC_PATH_PROG([BREW],brew,)
       if test x$BREW != x; then
         # These Homebrew packages may be keg-only, meaning that they won't be found
         # in expected paths because they may conflict with system files. Ask
         # Homebrew where each one is located, then adjust paths accordingly.
         openssl_prefix=`$BREW --prefix openssl 2>/dev/null`
         valgrind_prefix=`$BREW --prefix valgrind 2>/dev/null`
         if test x$openssl_prefix != x; then
           PKG_CONFIG_PATH="$openssl_prefix/lib/pkgconfig:$PKG_CONFIG_PATH"
           export PKG_CONFIG_PATH
           CRYPTO_CPPFLAGS="-I$openssl_prefix/include"
         fi
         if test x$valgrind_prefix != x; then
           VALGRIND_CPPFLAGS="-I$valgrind_prefix/include"
         fi
       else
         AC_PATH_PROG([PORT],port,)
         # If homebrew isn't installed and macports is, add the macports default paths
         # as a last resort.
         if test x$PORT != x; then
           CPPFLAGS="$CPPFLAGS -isystem /opt/local/include"
           LDFLAGS="$LDFLAGS -L/opt/local/lib"
         fi
       fi
     else
       case $build_os in
         *darwin*)
           BUILD_OS=darwin
           ;;
         *)
           AC_PATH_TOOL([DSYMUTIL], [dsymutil], dsymutil)
           AC_PATH_TOOL([INSTALLNAMETOOL], [install_name_tool], install_name_tool)
           AC_PATH_TOOL([OTOOL], [otool], otool)
           AC_PATH_PROGS([XORRISOFS], [xorrisofs], xorrisofs)
           AC_PATH_PROGS([DMG], [dmg], dmg)
           AC_PATH_PROGS([RSVG_CONVERT], [rsvg-convert rsvg],rsvg-convert)
           AC_PATH_PROGS([IMAGEMAGICK_CONVERT], [convert],convert)
           AC_PATH_PROGS([TIFFCP], [tiffcp],tiffcp)

           dnl libtool will try to strip the static lib, which is a problem for
           dnl cross-builds because strip attempts to call a hard-coded ld,
           dnl which may not exist in the path. Stripping the .a is not
           dnl necessary, so just disable it.
           old_striplib=
           ;;
       esac
     fi

     AX_CHECK_LINK_FLAG([[-Wl,-headerpad_max_install_names]], [LDFLAGS="$LDFLAGS -Wl,-headerpad_max_install_names"],, [[$LDFLAG_WERROR]])
     CPPFLAGS="$CPPFLAGS -DMAC_OSX -DOBJC_OLD_DISPATCH_PROTOTYPES=0"
     OBJCXXFLAGS="$CXXFLAGS"
     ;;
   *android*)
     dnl make sure android stays above linux for hosts like *linux-android*
     TARGET_OS=android
     case $host in
       *x86_64*)
          ANDROID_ARCH=x86_64
          ;;
        *aarch64*)
          ANDROID_ARCH=arm64-v8a
          ;;
        *armv7a*)
          ANDROID_ARCH=armeabi-v7a
          ;;
        *i686*)
          ANDROID_ARCH=i686
          ;;
        *) AC_MSG_ERROR("Could not determine Android arch") ;;
      esac
     ;;
   *linux*)
     TARGET_OS=linux
     ;;
esac

if test x$use_extended_functional_tests != xno; then
  AC_SUBST(EXTENDED_FUNCTIONAL_TESTS, --extended)
fi

if test x$use_lcov = xyes; then
  if test x$LCOV = x; then
    AC_MSG_ERROR("lcov testing requested but lcov not found")
  fi
  if test x$PYTHON = x; then
    AC_MSG_ERROR("lcov testing requested but python not found")
  fi
  if test x$GENHTML = x; then
    AC_MSG_ERROR("lcov testing requested but genhtml not found")
  fi

saved_CFLAGS="$CFLAGS"
CFLAGS="-Wconditional-uninitialized $CFLAGS"
AC_MSG_CHECKING([if ${CC} supports -Wconditional-uninitialized])
AC_COMPILE_IFELSE([AC_LANG_SOURCE([[char foo;]])],
    [ AC_MSG_RESULT([yes]) ],
    [ AC_MSG_RESULT([no])
      CFLAGS="$saved_CFLAGS"
    ])

saved_CFLAGS="$CFLAGS"
CFLAGS="-fvisibility=hidden $CFLAGS"
AC_MSG_CHECKING([if ${CC} supports -fvisibility=hidden])
AC_COMPILE_IFELSE([AC_LANG_SOURCE([[char foo;]])],
    [ AC_MSG_RESULT([yes]) ],
    [ AC_MSG_RESULT([no])
      CFLAGS="$saved_CFLAGS"
    ])

###
### Define config arguments
###

AC_ARG_ENABLE(benchmark,
    AS_HELP_STRING([--enable-benchmark],[compile benchmark [default=yes]]),
    [use_benchmark=$enableval],
    [use_benchmark=yes])

if test x$use_lcov_branch != xno; then
  AC_SUBST(LCOV_OPTS, "$LCOV_OPTS --rc lcov_branch_coverage=1")
fi

dnl Check for __int128
AC_CHECK_TYPES([__int128])

dnl Check for endianness
AC_C_BIGENDIAN

dnl Check for pthread compile/link requirements
AX_PTHREAD

dnl The following macro will add the necessary defines to bitcoin-config.h, but
dnl they also need to be passed down to any subprojects. Pull the results out of
dnl the cache and add them to CPPFLAGS.
AC_SYS_LARGEFILE
dnl detect POSIX or GNU variant of strerror_r
AC_FUNC_STRERROR_R

if test x$ac_cv_sys_file_offset_bits != x &&
   test x$ac_cv_sys_file_offset_bits != xno &&
   test x$ac_cv_sys_file_offset_bits != xunknown; then
  CPPFLAGS="$CPPFLAGS -D_FILE_OFFSET_BITS=$ac_cv_sys_file_offset_bits"
fi

if test x$ac_cv_sys_large_files != x &&
   test x$ac_cv_sys_large_files != xno &&
   test x$ac_cv_sys_large_files != xunknown; then
  CPPFLAGS="$CPPFLAGS -D_LARGE_FILES=$ac_cv_sys_large_files"
fi

AC_ARG_ENABLE(openssl_tests,
    AS_HELP_STRING([--enable-openssl-tests],[enable OpenSSL tests [default=auto]]),
    [enable_openssl_tests=$enableval],
    [enable_openssl_tests=auto])

AC_ARG_ENABLE(experimental,
    AS_HELP_STRING([--enable-experimental],[allow experimental configure options [default=no]]),
    [use_experimental=$enableval],
    [use_experimental=no])

AC_ARG_ENABLE(exhaustive_tests,
    AS_HELP_STRING([--enable-exhaustive-tests],[compile exhaustive tests [default=yes]]),
    [use_exhaustive_tests=$enableval],
    [use_exhaustive_tests=yes])

AC_ARG_ENABLE(ecmult_static_precomputation,
    AS_HELP_STRING([--enable-ecmult-static-precomputation],[enable precomputed ecmult table for signing [default=auto]]),
    [use_ecmult_static_precomputation=$enableval],
    [use_ecmult_static_precomputation=auto])

AC_ARG_ENABLE(module_ecdh,
    AS_HELP_STRING([--enable-module-ecdh],[enable ECDH shared secret computation]),
    [enable_module_ecdh=$enableval],
    [enable_module_ecdh=no])

AC_ARG_ENABLE(module_recovery,
    AS_HELP_STRING([--enable-module-recovery],[enable ECDSA pubkey recovery module [default=no]]),
    [enable_module_recovery=$enableval],
    [enable_module_recovery=no])

AC_ARG_ENABLE(module_extrakeys,
    AS_HELP_STRING([--enable-module-extrakeys],[enable extrakeys module (experimental)]),
    [enable_module_extrakeys=$enableval],
    [enable_module_extrakeys=no])

AC_ARG_ENABLE(module_schnorrsig,
    AS_HELP_STRING([--enable-module-schnorrsig],[enable schnorrsig module (experimental)]),
    [enable_module_schnorrsig=$enableval],
    [enable_module_schnorrsig=no])

AC_ARG_ENABLE(external_default_callbacks,
    AS_HELP_STRING([--enable-external-default-callbacks],[enable external default callback functions [default=no]]),
    [use_external_default_callbacks=$enableval],
    [use_external_default_callbacks=no])

# Test-only override of the (autodetected by the C code) "widemul" setting.
# Legal values are int64 (for [u]int64_t), int128 (for [unsigned] __int128), and auto (the default).
AC_ARG_WITH([test-override-wide-multiply], [] ,[set_widemul=$withval], [set_widemul=auto])

AC_ARG_WITH([asm], [AS_HELP_STRING([--with-asm=x86_64|arm|no|auto],
[assembly optimizations to use (experimental: arm) [default=auto]])],[req_asm=$withval], [req_asm=auto])

AC_ARG_WITH([ecmult-window], [AS_HELP_STRING([--with-ecmult-window=SIZE|auto],
[window size for ecmult precomputation for verification, specified as integer in range [2..24].]
[Larger values result in possibly better performance at the cost of an exponentially larger precomputed table.]
[The table will store 2^(SIZE-1) * 64 bytes of data but can be larger in memory due to platform-specific padding and alignment.]
["auto" is a reasonable setting for desktop machines (currently 15). [default=auto]]
)],
[req_ecmult_window=$withval], [req_ecmult_window=auto])

AC_ARG_WITH([ecmult-gen-precision], [AS_HELP_STRING([--with-ecmult-gen-precision=2|4|8|auto],
[Precision bits to tune the precomputed table size for signing.]
[The size of the table is 32kB for 2 bits, 64kB for 4 bits, 512kB for 8 bits of precision.]
[A larger table size usually results in possible faster signing.]
["auto" is a reasonable setting for desktop machines (currently 4). [default=auto]]
)],
[req_ecmult_gen_precision=$withval], [req_ecmult_gen_precision=auto])

AC_ARG_WITH([valgrind], [AS_HELP_STRING([--with-valgrind=yes|no|auto],
[Build with extra checks for running inside Valgrind [default=auto]]
)],
[req_valgrind=$withval], [req_valgrind=auto])

###
### Handle config options (except for modules)
###

if test x"$req_valgrind" = x"no"; then
  enable_valgrind=no
else
  SECP_VALGRIND_CHECK
  if test x"$has_valgrind" != x"yes"; then
    if test x"$req_valgrind" = x"yes"; then
      AC_MSG_ERROR([Valgrind support explicitly requested but valgrind/memcheck.h header not available])
    fi
    enable_valgrind=no
  else
    enable_valgrind=yes
  fi

  CPPFLAGS="$CPPFLAGS -DPROVIDE_FUZZ_MAIN_FUNCTION"
fi

if test x$enable_wallet != xno; then
    dnl Check for libdb_cxx only if wallet enabled
    if test "x$use_bdb" != "xno"; then
      BITCOIN_FIND_BDB48
      if test x$suppress_external_warnings != xno ; then
        BDB_CPPFLAGS=SUPPRESS_WARNINGS($BDB_CPPFLAGS)
      fi
    fi

    dnl Check for sqlite3
    if test "x$use_sqlite" != "xno"; then
      PKG_CHECK_MODULES([SQLITE], [sqlite3 >= 3.7.17], [have_sqlite=yes], [have_sqlite=no])
    fi
    AC_MSG_CHECKING([whether to build wallet with support for sqlite])
    if test "x$use_sqlite" = "xno"; then
      use_sqlite=no
    elif test "x$have_sqlite" = "xno"; then
      if test "x$use_sqlite" = "xyes"; then
        AC_MSG_ERROR([sqlite support requested but cannot be built. Use --without-sqlite])
      fi
      use_sqlite=no
    else
      if test x$use_sqlite != xno; then
        AC_DEFINE([USE_SQLITE],[1],[Define if sqlite support should be compiled in])
        use_sqlite=yes
      fi
    fi
    AC_MSG_RESULT([$use_sqlite])

    dnl Disable wallet if both --without-bdb and --without-sqlite
    if test "x$use_bdb$use_sqlite" = "xnono"; then
        if test "x$enable_wallet" = "xyes"; then
            AC_MSG_ERROR([wallet functionality requested but no BDB or SQLite support available.])
        fi
        enable_wallet=no
    fi
fi

if test x$use_ebpf != xno; then
  AC_MSG_CHECKING([whether eBPF tracepoints are supported])
  AC_COMPILE_IFELSE([
    AC_LANG_PROGRAM(
      [#include <sys/sdt.h>],
      [DTRACE_PROBE("context", "event");]
    )],
    [AC_MSG_RESULT(yes); have_sdt=yes; AC_DEFINE([ENABLE_TRACING], [1], [Define to 1 to enable eBPF user static defined tracepoints])],
    [AC_MSG_RESULT(no); have_sdt=no;]
  )
fi

dnl Check for libminiupnpc (optional)
if test x$use_upnp != xno; then
  AC_CHECK_HEADERS(
    [miniupnpc/miniupnpc.h miniupnpc/upnpcommands.h miniupnpc/upnperrors.h],
    [AC_CHECK_LIB([miniupnpc], [upnpDiscover], [MINIUPNPC_LIBS=-lminiupnpc], [have_miniupnpc=no])],
    [have_miniupnpc=no]
  )
dnl The minimum supported miniUPnPc API version is set to 10. This keeps compatibility
dnl with Ubuntu 16.04 LTS and Debian 8 libminiupnpc-dev packages.
if test x$have_miniupnpc != xno; then
  AC_MSG_CHECKING([whether miniUPnPc API version is supported])
  AC_PREPROC_IFELSE([AC_LANG_PROGRAM([[
      @%:@include <miniupnpc/miniupnpc.h>
    ]], [[
      #if MINIUPNPC_API_VERSION >= 10
      // Everything is okay
      #else
      #  error miniUPnPc API version is too old
      #endif
    ]])],[
      AC_MSG_RESULT(yes)
    ],[
    AC_MSG_RESULT(no)
    AC_MSG_WARN([miniUPnPc API version < 10 is unsupported, disabling UPnP support.])
    have_miniupnpc=no
  ])
fi
fi

dnl Check for libnatpmp (optional).
if test "x$use_natpmp" != xno; then
  AC_CHECK_HEADERS([natpmp.h],
                   [AC_CHECK_LIB([natpmp], [initnatpmp], [NATPMP_LIBS=-lnatpmp], [have_natpmp=no])],
                   [have_natpmp=no])
fi

if test x"$enable_coverage" = x"yes"; then
    AC_DEFINE(COVERAGE, 1, [Define this symbol to compile out all VERIFY code])
    CFLAGS="-O0 --coverage $CFLAGS"
    LDFLAGS="--coverage $LDFLAGS"
else
    CFLAGS="-O2 $CFLAGS"
fi

if test x"$req_asm" = x"auto"; then
  SECP_64BIT_ASM_CHECK
  if test x"$has_64bit_asm" = x"yes"; then
    set_asm=x86_64
  fi

  if test x$use_boost = xyes; then

    AX_BOOST_UNIT_TEST_FRAMEWORK

    dnl Determine if -DBOOST_TEST_DYN_LINK is needed
    AC_MSG_CHECKING([for dynamic linked boost test])
    TEMP_LIBS="$LIBS"
    LIBS="$LIBS $BOOST_LDFLAGS $BOOST_UNIT_TEST_FRAMEWORK_LIB"
    TEMP_CPPFLAGS="$CPPFLAGS"
    CPPFLAGS="$CPPFLAGS $BOOST_CPPFLAGS"
    AC_LINK_IFELSE([AC_LANG_SOURCE([
         #define BOOST_TEST_DYN_LINK
         #define BOOST_TEST_MAIN
          #include <boost/test/unit_test.hpp>

         ])],
      [AC_MSG_RESULT(yes)]
      [TESTDEFS="$TESTDEFS -DBOOST_TEST_DYN_LINK"],
      [AC_MSG_RESULT(no)])
    LIBS="$TEMP_LIBS"
    CPPFLAGS="$TEMP_CPPFLAGS"

  fi
fi

# Select assembly optimization
use_external_asm=no

need_bundled_univalue=yes
if test x$build_bitcoin_wallet$build_bitcoin_cli$build_bitcoin_tx$build_bitcoin_util$build_bitcoind$bitcoin_enable_qt$use_tests$use_bench = xnononononononono; then
  need_bundled_univalue=no
else
  if test x$system_univalue != xno; then
    PKG_CHECK_MODULES([UNIVALUE], [libunivalue >= 1.0.4], [found_univalue=yes], [found_univalue=no])
    if test x$found_univalue = xyes; then
      system_univalue=yes
      need_bundled_univalue=no
    elif test x$system_univalue = xyes; then
      AC_MSG_ERROR([univalue not found])
    else
      system_univalue=no
    fi
  fi

if test x"$use_external_asm" = x"yes"; then
  AC_DEFINE(USE_EXTERNAL_ASM, 1, [Define this symbol if an external (non-inline) assembly implementation is used])
fi


# Select wide multiplication implementation
case $set_widemul in
int128)
  AC_DEFINE(USE_FORCE_WIDEMUL_INT128, 1, [Define this symbol to force the use of the (unsigned) __int128 based wide multiplication implementation])
  ;;
int64)
  AC_DEFINE(USE_FORCE_WIDEMUL_INT64, 1, [Define this symbol to force the use of the (u)int64_t based wide multiplication implementation])
  ;;
auto)
  ;;
*)
  AC_MSG_ERROR([invalid wide multiplication implementation])
  ;;
esac

# Set ecmult window size
if test x"$req_ecmult_window" = x"auto"; then
  set_ecmult_window=15
else
  build_multiprocess=no
fi

AM_CONDITIONAL([BUILD_MULTIPROCESS],[test "x$build_multiprocess" = xyes])
AM_CONDITIONAL([BUILD_BITCOIN_NODE], [test "x$build_multiprocess" = xyes])
AM_CONDITIONAL([BUILD_BITCOIN_GUI], [test "x$build_multiprocess" = xyes])

dnl codegen tools check

if test x$build_multiprocess != xno; then
  if test "x$with_mpgen" = xyes || test "x$with_mpgen" = xauto; then
    MPGEN_PREFIX="$libmultiprocess_prefix"
  elif test "x$with_mpgen" != xno; then
    MPGEN_PREFIX="$with_mpgen";
  fi
  AC_SUBST(MPGEN_PREFIX)
fi

# Set ecmult gen precision
if test x"$req_ecmult_gen_precision" = x"auto"; then
  set_ecmult_gen_precision=4
else
  set_ecmult_gen_precision=$req_ecmult_gen_precision
fi
AC_MSG_RESULT($build_bitcoin_libs)

AC_LANG_POP

if test "x$use_ccache" != "xno"; then
  AC_MSG_CHECKING(if ccache should be used)
  if test x$CCACHE = x; then
    if test "x$use_ccache" = "xyes"; then
      AC_MSG_ERROR([ccache not found.]);
    else
      use_ccache=no
    fi
  else
    use_ccache=yes
    CC="$ac_cv_path_CCACHE $CC"
    CXX="$ac_cv_path_CCACHE $CXX"
  fi
  AC_MSG_RESULT($use_ccache)
  if test "x$use_ccache" = "xyes"; then
    AX_CHECK_COMPILE_FLAG([-fdebug-prefix-map=A=B],[DEBUG_CXXFLAGS="$DEBUG_CXXFLAGS -fdebug-prefix-map=\$(abs_srcdir)=."],,[[$CXXFLAG_WERROR]])
    AX_CHECK_PREPROC_FLAG([-fmacro-prefix-map=A=B],[DEBUG_CPPFLAGS="$DEBUG_CPPFLAGS -fmacro-prefix-map=\$(abs_srcdir)=."],,[[$CXXFLAG_WERROR]])
  fi
fi

if test x"$enable_valgrind" = x"yes"; then
  SECP_INCLUDES="$SECP_INCLUDES $VALGRIND_CPPFLAGS"
fi

# Handle static precomputation (after everything which modifies CFLAGS and friends)
if test x"$use_ecmult_static_precomputation" != x"no"; then
  if test x"$cross_compiling" = x"no"; then
    set_precomp=yes
    if test x"${CC_FOR_BUILD+x}${CFLAGS_FOR_BUILD+x}${CPPFLAGS_FOR_BUILD+x}${LDFLAGS_FOR_BUILD+x}" != x; then
      AC_MSG_WARN([CC_FOR_BUILD, CFLAGS_FOR_BUILD, CPPFLAGS_FOR_BUILD, and/or LDFLAGS_FOR_BUILD is set but ignored because we are not cross-compiling.])
    fi
    # If we're not cross-compiling, simply use the same compiler for building the static precompation code.
    CC_FOR_BUILD="$CC"
    CFLAGS_FOR_BUILD="$CFLAGS"
    CPPFLAGS_FOR_BUILD="$CPPFLAGS"
    LDFLAGS_FOR_BUILD="$LDFLAGS"
  else
    AX_PROG_CC_FOR_BUILD

    # Temporarily switch to an environment for the native compiler
    save_cross_compiling=$cross_compiling
    cross_compiling=no
    SAVE_CC="$CC"
    CC="$CC_FOR_BUILD"
    SAVE_CFLAGS="$CFLAGS"
    CFLAGS="$CFLAGS_FOR_BUILD"
    SAVE_CPPFLAGS="$CPPFLAGS"
    CPPFLAGS="$CPPFLAGS_FOR_BUILD"
    SAVE_LDFLAGS="$LDFLAGS"
    LDFLAGS="$LDFLAGS_FOR_BUILD"

    warn_CFLAGS_FOR_BUILD="-Wall -Wextra -Wno-unused-function"
    saved_CFLAGS="$CFLAGS"
    CFLAGS="$warn_CFLAGS_FOR_BUILD $CFLAGS"
    AC_MSG_CHECKING([if native ${CC_FOR_BUILD} supports ${warn_CFLAGS_FOR_BUILD}])
    AC_COMPILE_IFELSE([AC_LANG_SOURCE([[char foo;]])],
        [ AC_MSG_RESULT([yes]) ],
        [ AC_MSG_RESULT([no])
          CFLAGS="$saved_CFLAGS"
        ])

    AC_MSG_CHECKING([for working native compiler: ${CC_FOR_BUILD}])
    AC_RUN_IFELSE(
      [AC_LANG_PROGRAM([], [])],
      [working_native_cc=yes],
      [working_native_cc=no],[:])

    CFLAGS_FOR_BUILD="$CFLAGS"

    # Restore the environment
    cross_compiling=$save_cross_compiling
    CC="$SAVE_CC"
    CFLAGS="$SAVE_CFLAGS"
    CPPFLAGS="$SAVE_CPPFLAGS"
    LDFLAGS="$SAVE_LDFLAGS"

    if test x"$working_native_cc" = x"no"; then
      AC_MSG_RESULT([no])
      set_precomp=no
      m4_define([please_set_for_build], [Please set CC_FOR_BUILD, CFLAGS_FOR_BUILD, CPPFLAGS_FOR_BUILD, and/or LDFLAGS_FOR_BUILD.])
      if test x"$use_ecmult_static_precomputation" = x"yes";  then
        AC_MSG_ERROR([native compiler ${CC_FOR_BUILD} does not produce working binaries. please_set_for_build])
      else
        AC_MSG_WARN([Disabling statically generated ecmult table because the native compiler ${CC_FOR_BUILD} does not produce working binaries. please_set_for_build])
      fi
    else
      AC_MSG_RESULT([yes])
      set_precomp=yes
    fi
  fi

  AC_SUBST(CC_FOR_BUILD)
  AC_SUBST(CFLAGS_FOR_BUILD)
  AC_SUBST(CPPFLAGS_FOR_BUILD)
  AC_SUBST(LDFLAGS_FOR_BUILD)
else
  set_precomp=no
fi

else
  AC_MSG_RESULT(no)
fi

###
### Handle module options
###

if test x"$enable_module_ecdh" = x"yes"; then
  AC_DEFINE(ENABLE_MODULE_ECDH, 1, [Define this symbol to enable the ECDH module])
fi

dnl Enable NAT-PMP support.
AC_MSG_CHECKING([whether to build with support for NAT-PMP])
if test "x$have_natpmp" = xno; then
  if test "x$use_natpmp" = xyes; then
     AC_MSG_ERROR([NAT-PMP requested but cannot be built. Use --without-natpmp])
  fi
  AC_MSG_RESULT([no])
  use_natpmp=no
else
  if test "x$use_natpmp" != xno; then
    AC_MSG_RESULT([yes])
    AC_MSG_CHECKING([whether to build with NAT-PMP enabled by default])
    use_natpmp=yes
    natpmp_setting=0
    if test "x$use_natpmp_default" != xno; then
      use_natpmp_default=yes
      natpmp_setting=1
    fi
    AC_MSG_RESULT($use_natpmp_default)
    AC_DEFINE_UNQUOTED([USE_NATPMP], [$natpmp_setting], [NAT-PMP support not compiled if undefined, otherwise value (0 or 1) determines default state])
    if test x$TARGET_OS = xwindows; then
      NATPMP_CPPFLAGS="-DSTATICLIB -DNATPMP_STATICLIB"
    fi
  else
    AC_MSG_RESULT([no])
  fi
fi

dnl these are only used when qt is enabled
BUILD_TEST_QT=""
if test x$bitcoin_enable_qt != xno; then
  dnl enable dbus support
  AC_MSG_CHECKING([whether to build GUI with support for D-Bus])
  if test x$bitcoin_enable_qt_dbus != xno; then
    AC_DEFINE([USE_DBUS],[1],[Define if dbus support should be compiled in])
  fi
  AC_MSG_RESULT($bitcoin_enable_qt_dbus)

  dnl enable qr support
  AC_MSG_CHECKING([whether to build GUI with support for QR codes])
  if test x$have_qrencode = xno; then
    if test x$use_qr = xyes; then
      AC_MSG_ERROR([QR support requested but cannot be built. Use --without-qrencode])
    fi
    use_qr=no
  else
    if test x$use_qr != xno; then
      AC_DEFINE([USE_QRCODE],[1],[Define if QR support should be compiled in])
      use_qr=yes
    fi
  fi
  AC_MSG_RESULT([$use_qr])

  if test x$XGETTEXT = x; then
    AC_MSG_WARN("xgettext is required to update qt translations")
  fi

  AC_MSG_CHECKING([whether to build test_bitcoin-qt])
  if test x$use_gui_tests$bitcoin_enable_qt_test = xyesyes; then
    AC_MSG_RESULT([yes])
    BUILD_TEST_QT="yes"
  else
    AC_MSG_RESULT([no])
  fi
fi

AM_CONDITIONAL([ENABLE_ZMQ], [test "x$use_zmq" = "xyes"])

AC_MSG_CHECKING([whether to build test_bitcoin])
if test x$use_tests = xyes; then
  if test "x$enable_fuzz" = "xyes"; then
    AC_MSG_RESULT([no, because fuzzing is enabled])
  else
    AC_MSG_RESULT([yes])
  fi
  BUILD_TEST="yes"
else
  AC_MSG_RESULT([no])
  BUILD_TEST=""
fi

if test x"$use_external_default_callbacks" = x"yes"; then
  AC_DEFINE(USE_EXTERNAL_DEFAULT_CALLBACKS, 1, [Define this symbol if an external implementation of the default callbacks is used])
fi

###
### Check for --enable-experimental if necessary
###

if test x"$enable_experimental" = x"yes"; then
  AC_MSG_NOTICE([******])
  AC_MSG_NOTICE([WARNING: experimental build])
  AC_MSG_NOTICE([Experimental features do not have stable APIs or properties, and may not be safe for production use.])
  AC_MSG_NOTICE([Building extrakeys module: $enable_module_extrakeys])
  AC_MSG_NOTICE([Building schnorrsig module: $enable_module_schnorrsig])
  AC_MSG_NOTICE([******])
else
  if test x"$enable_module_extrakeys" = x"yes"; then
    AC_MSG_ERROR([extrakeys module is experimental. Use --enable-experimental to allow.])
  fi
  if test x"$enable_module_schnorrsig" = x"yes"; then
    AC_MSG_ERROR([schnorrsig module is experimental. Use --enable-experimental to allow.])
  fi
  if test x"$set_asm" = x"arm"; then
    AC_MSG_ERROR([ARM assembly optimization is experimental. Use --enable-experimental to allow.])
  fi
fi

###
### Generate output
###

AC_CONFIG_HEADERS([src/libsecp256k1-config.h])
AC_CONFIG_FILES([Makefile libsecp256k1.pc])
AC_SUBST(SECP_INCLUDES)
AC_SUBST(SECP_LIBS)
AC_SUBST(SECP_TEST_LIBS)
AC_SUBST(SECP_TEST_INCLUDES)
AM_CONDITIONAL([ENABLE_COVERAGE], [test x"$enable_coverage" = x"yes"])
AM_CONDITIONAL([USE_TESTS], [test x"$use_tests" != x"no"])
AM_CONDITIONAL([USE_EXHAUSTIVE_TESTS], [test x"$use_exhaustive_tests" != x"no"])
AM_CONDITIONAL([USE_BENCHMARK], [test x"$use_benchmark" = x"yes"])
AM_CONDITIONAL([USE_ECMULT_STATIC_PRECOMPUTATION], [test x"$set_precomp" = x"yes"])
AM_CONDITIONAL([ENABLE_MODULE_ECDH], [test x"$enable_module_ecdh" = x"yes"])
AM_CONDITIONAL([ENABLE_MODULE_RECOVERY], [test x"$enable_module_recovery" = x"yes"])
AM_CONDITIONAL([ENABLE_MODULE_EXTRAKEYS], [test x"$enable_module_extrakeys" = x"yes"])
AM_CONDITIONAL([ENABLE_MODULE_SCHNORRSIG], [test x"$enable_module_schnorrsig" = x"yes"])
AM_CONDITIONAL([USE_EXTERNAL_ASM], [test x"$use_external_asm" = x"yes"])
AM_CONDITIONAL([USE_ASM_ARM], [test x"$set_asm" = x"arm"])

# Make sure nothing new is exported so that we don't break the cache.
PKGCONFIG_PATH_TEMP="$PKG_CONFIG_PATH"
unset PKG_CONFIG_PATH
PKG_CONFIG_PATH="$PKGCONFIG_PATH_TEMP"

PKGCONFIG_LIBDIR_TEMP="$PKG_CONFIG_LIBDIR"
unset PKG_CONFIG_LIBDIR
PKG_CONFIG_LIBDIR="$PKGCONFIG_LIBDIR_TEMP"

if test x$need_bundled_univalue = xyes; then
  AC_CONFIG_SUBDIRS([src/univalue])
fi

ac_configure_args="${ac_configure_args} --disable-shared --with-pic --enable-benchmark=no --enable-module-recovery --enable-module-schnorrsig --enable-experimental"
AC_CONFIG_SUBDIRS([src/secp256k1])

AC_OUTPUT

dnl Replace the BUILDDIR path with the correct Windows path if compiling on Native Windows
case ${OS} in
   *Windows*)
     sed  's/BUILDDIR="\/\([[a-z]]\)/BUILDDIR="\1:/'  test/config.ini > test/config-2.ini
     mv test/config-2.ini test/config.ini
   ;;
esac

echo
echo "Build Options:"
echo "  with ecmult precomp     = $set_precomp"
echo "  with external callbacks = $use_external_default_callbacks"
echo "  with benchmarks         = $use_benchmark"
echo "  with tests              = $use_tests"
echo "  with openssl tests      = $enable_openssl_tests"
echo "  with coverage           = $enable_coverage"
echo "  module ecdh             = $enable_module_ecdh"
echo "  module recovery         = $enable_module_recovery"
echo "  module extrakeys        = $enable_module_extrakeys"
echo "  module schnorrsig       = $enable_module_schnorrsig"
echo
echo "  asm                     = $set_asm"
echo "  ecmult window size      = $set_ecmult_window"
echo "  ecmult gen prec. bits   = $set_ecmult_gen_precision"
# Hide test-only options unless they're used.
if test x"$set_widemul" != xauto; then
echo "  wide multiplication     = $set_widemul"
fi
echo "  with zmq        = $use_zmq"
if test x$enable_fuzz == xno; then
    echo "  with test       = $use_tests"
else
    echo "  with test       = not building test_bitcoin because fuzzing is enabled"
    echo "    with fuzz     = $enable_fuzz"
fi
echo "  with bench      = $use_bench"
echo "  with upnp       = $use_upnp"
echo "  with natpmp     = $use_natpmp"
echo "  use asm         = $use_asm"
echo "  ebpf tracing    = $have_sdt"
echo "  sanitizers      = $use_sanitizers"
echo "  debug enabled   = $enable_debug"
echo "  gprof enabled   = $enable_gprof"
echo "  werror          = $enable_werror"
echo
echo "  target os       = $TARGET_OS"
echo "  build os        = $build_os"
echo
echo "  CC              = $CC"
echo "  CFLAGS          = $PTHREAD_CFLAGS $CFLAGS"
echo "  CPPFLAGS        = $DEBUG_CPPFLAGS $HARDENED_CPPFLAGS $CPPFLAGS"
echo "  CXX             = $CXX"
echo "  CXXFLAGS        = $DEBUG_CXXFLAGS $HARDENED_CXXFLAGS $WARN_CXXFLAGS $NOWARN_CXXFLAGS $ERROR_CXXFLAGS $GPROF_CXXFLAGS $CXXFLAGS"
echo "  LDFLAGS         = $PTHREAD_LIBS $HARDENED_LDFLAGS $GPROF_LDFLAGS $LDFLAGS"
echo "  ARFLAGS         = $ARFLAGS"
echo
if test x"$set_precomp" = x"yes"; then
echo "  CC_FOR_BUILD            = $CC_FOR_BUILD"
echo "  CFLAGS_FOR_BUILD        = $CFLAGS_FOR_BUILD"
echo "  CPPFLAGS_FOR_BUILD      = $CPPFLAGS_FOR_BUILD"
echo "  LDFLAGS_FOR_BUILD       = $LDFLAGS_FOR_BUILD"
fi
