PHP_ARG_ENABLE(zeprobe, whether to enable zeprobe,
[  --enable-zeprobe           Enable zeprobe])
if test "$PHP_ZEPROBE" != "no"; then
  PHP_NEW_EXTENSION(zeprobe, zeprobe.c, $ext_shared)
fi
