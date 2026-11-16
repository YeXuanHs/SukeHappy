import sys
sys.path.insert(0, '.')
import paramiko

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect('103.236.99.244', port=50659, username='root', password='9ePIPU0PBxQVDc',
          timeout=30, banner_timeout=60, auth_timeout=60)

cmd = r'''
INI=/www/server/php/73/etc/php.ini
D=/www/server/php/73/lib/php/extensions/no-debug-non-zts-20180731
R=/www/wwwroot/103.217.193.191_9179

run20() {
  for p in $(ps -eo pid,args | grep "[p]hp-fpm" | grep -v "74/etc" | awk "{print \$1}"); do kill -9 $p 2>/dev/null; done
  sleep 2; rm -f /tmp/php-cgi-73.sock
  /etc/init.d/php-fpm-73 start >/dev/null 2>&1; sleep 4
  rm -rf $R/data/runtime/cahce/* 2>/dev/null
  OK=0; BAD=0
  for i in $(seq 1 20); do
    CODE=$(curl -s -m 25 -o /tmp/pz.html -w "%{http_code}" "http://127.0.0.1:9179/news")
    [ "$CODE" = "200" ] && OK=$((OK+1)) || { BAD=$((BAD+1)); echo "    第$i次: $CODE"; }
  done
  echo "  => OK=$OK BAD=$BAD"
}

echo "===== B1: 扩展加载, mock_auth=0 (不替换 curl handler) ====="
sed -i 's/^;extension=sukehappy.so/extension=sukehappy.so/' $INI
sed -i 's/^sukehappy.mock_auth=1/sukehappy.mock_auth=0/' $INI
run20

echo "===== B2: 扩展加载, mock_auth=1 (替换 handler) ====="
sed -i 's/^sukehappy.mock_auth=0/sukehappy.mock_auth=1/' $INI
run20
'''
_, o, e = c.exec_command(cmd, timeout=270)
print(o.read().decode('utf-8', 'replace')[:1700])
c.close()
