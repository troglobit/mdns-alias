AC_PREREQ(2.61)
AC_INIT([mdns-alias], [1.0], [https://github.com/troglobit/mdns-alias/issues])
AM_INIT_AUTOMAKE(1.11 foreign)
AM_SILENT_RULES(yes)

AC_CONFIG_FILES([Makefile src/Makefile])

AC_PROG_CC
AC_PROG_INSTALL

# Check for pkg-config first, warn if it's not installed
PKG_PROG_PKG_CONFIG
PKG_CHECK_MODULES([avahi], [avahi-client >= 0.8])

AC_OUTPUT

cat <<EOF

------------------ Summary ------------------
 $PACKAGE_NAME version $PACKAGE_VERSION
  Prefix................: $prefix
  C Compiler............: $CC $CFLAGS $CPPFLAGS $LDFLAGS $LIBS
  Avahi client CFLAGS...: $avahi_CFLAGS
  Avahi client LIBS.....: $avahi_LIBS

------------- Compiler version --------------
$($CC --version || true)
---------------------------------------------

Check the above options and compile with:
 ${MAKE-make}

EOF
