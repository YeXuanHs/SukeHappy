#!/usr/bin/env python3
"""
sukehappy 本地授权 mock 服务（HTTPS 版）

工作方式：
  /etc/hosts 里把 apiv1.sukeyun.com 指向 127.0.0.1，
  本服务在 443 端口用自签证书提供授权响应。
  模板的 Curlrequest 设了 CURLOPT_SSL_VERIFYPEER=false / VERIFYHOST=false，
  所以自签证书可以直接用。

扩展这边不做任何 URL 改写（改参数槽里的字符串要动引用计数，
一旦算错就是 use-after-free → worker SIGSEGV）。
扩展只负责在 curl_exec 之后把返回值换成「授权通过」的响应。

接口（结构必须与授权中心真实响应一致）：
  /ky/admin/template      → {"code":200,"msg":""}
  /ky/admin               → {"code":200,"ki":"","msg":"ok"}
  /ky/admin/verification  → {"code":200,"msg":"ok"}
  /ky/admin/version       → {"code":200,"msg":"ok","status":200,"data":{...}}
  /ky/admin/callback      → {"code":200,"msg":"ok"}
"""

import json
import os
import ssl
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

HOST = '127.0.0.1'
PORT = int(os.environ.get('SUKE_MOCK_PORT', '443'))
CERT = os.environ.get('SUKE_MOCK_CERT', '/root/sukehappy_mock/apiv1.crt')
KEY = os.environ.get('SUKE_MOCK_KEY', '/root/sukehappy_mock/apiv1.key')
ACCESS_LOG = '/root/sukehappy_mock/access.log'


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    server_version = 'sukehappy-mock'

    def log_message(self, fmt, *args):
        pass

    def _send(self, obj):
        body = json.dumps(obj, ensure_ascii=False).encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        try:
            with open(ACCESS_LOG, 'a', encoding='utf-8') as f:
                f.write('%s %s\n' % (self.log_date_time_string(), self.path))
        except OSError:
            pass

        path = urlparse(self.path).path
        if path.startswith('/ky/admin/template'):
            self._send({'code': 200, 'msg': ''})
        elif path.startswith('/ky/admin/verification'):
            self._send({'code': 200, 'msg': 'ok'})
        elif path.startswith('/ky/admin/version'):
            self._send({'code': 200, 'msg': 'ok', 'status': 200,
                        'data': {'version': '1.0.7.6', 'status': 200}})
        elif path.startswith('/ky/admin/callback'):
            self._send({'code': 200, 'msg': 'ok'})
        elif path.rstrip('/') == '/ky/admin':
            self._send({'code': 200, 'ki': '', 'msg': 'ok'})
        else:
            self._send({'code': 404, 'msg': 'not mocked: ' + path})

    do_POST = do_GET


def main():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=CERT, keyfile=KEY)
    srv = ThreadingHTTPServer((HOST, PORT), Handler)
    srv.daemon_threads = True
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    sys.stderr.write('sukehappy mock (https) listening on %s:%d\n' % (HOST, PORT))
    sys.stderr.flush()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
