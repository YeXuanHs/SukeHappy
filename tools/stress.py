import sys
sys.path.insert(0, '.')
import paramiko

SRC = 'C:/Users/Administrator/Desktop/智简魔方/_private/menc/_pkg/SukeHappy/sukehappy.c'

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect('103.236.99.244', port=50659, username='root', password='9ePIPU0PBxQVDc',
          timeout=30, banner_timeout=60, auth_timeout=60)

sftp = c.open_sftp()
sftp.put(SRC, '/tmp/sk73/sukehappy.c')

cmd = r'''
cd /tmp/sk73 && touch sukehappy.c && make >/tmp/mk.log 2>&1; echo "make=$?"; grep -iE " error:|warning:" /tmp/mk.log|head -4
if ! grep -qiE " error:|warning:" /tmp/mk.log; then
  cp -f modules/sukehappy.so /www/server/php/73/lib/php/extensions/no-debug-non-zts-20180731/sukehappy.so
  sed -i 's/^;extension=sukehappy.so/extension=sukehappy.so/' /www/server/php/73/etc/php.ini
  sed -i 's/^sukehappy.mock_auth=0/sukehappy.mock_auth=1/' /www/server/php/73/etc/php.ini
  for p in $(ps -eo pid,args | grep "[p]hp-fpm" | grep -v "74/etc" | awk "{print \$1}"); do kill -9 $p 2>/dev/null; done
  sleep 2; rm -f /tmp/php-cgi-73.sock
  /etc/init.d/php-fpm-73 start >/dev/null 2>&1; sleep 4
  rm -rf /www/wwwroot/103.217.193.191_9179/data/runtime/cahce/* 2>/dev/null
  OK=0; BAD=0
  for i in $(seq 1 15); do
    CODE=$(curl -s -m 25 -o /tmp/pb.html -w "%{http_code}" "http://127.0.0.1:9179/news")
    if [ "$CODE" = "200" ] && ! grep -q "快云科技模板提示" /tmp/pb.html; then OK=$((OK+1)); else BAD=$((BAD+1)); echo "    第$i次: $CODE"; fi
  done
  echo "  OK=$OK BAD=$BAD"
fi
'''
_, o, e = c.exec_command(cmd, timeout=270)
print(o.read().decode('utf-8', 'replace')[:1700])
c.close()
