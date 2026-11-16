PHP_ARG_ENABLE(probe, whether to enable probe support,
[  --enable-probe            Enable probe support])

if test "$PHP_PROBE" != "no"; then
  PHP_NEW_EXTENSION(probe, probe.c, $ext_shared)
fi
