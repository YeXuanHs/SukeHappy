PHP_ARG_ENABLE(sukehappy, whether to enable sukehappy support,
[  --enable-sukehappy           Enable sukehappy support])

if test "$PHP_SUKEHAPPY" != "no"; then
  PHP_NEW_EXTENSION(sukehappy, sukehappy.c, $ext_shared)
fi
