PHP_ARG_ENABLE(mdyguard, whether to enable mdyguard support,
[  --enable-mdyguard           Enable mdyguard support])

if test "$PHP_MDYGUARD" != "no"; then
  PHP_NEW_EXTENSION(mdyguard, mdyguard.c, $ext_shared)
fi
