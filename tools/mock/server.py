#!/usr/bin/env python3
"""
sukehappy 本地授权 mock 服务

用途：让模板对 apiv1.sukeyun.com 的请求落到本地，返回「授权通过」的响应。
扩展把 curl 的 URL 改写到这里，curl 正常执行并拿到 200 ——
模板既不会看到 CURL ERROR，也不会走进任何异常路径。

为什么用 Python 而不是 php -S：
  php -S 默认单线程，配合 curl 的 keep-alive 会把唯一的 worker 占住，
  后续请求全部排队直到超时（表现为 FPM worker exit(1) → nginx 502）。
  本服务用 ThreadingHTTPServer，每个连接一个线程，稳。

接口：
  /ky/admin/template      → {"code":200,"msg":"","data":{...}}
  /ky/admin               → {"code":200,"ki":"","msg":"ok"}
  /ky/admin/verification  → {"code":200,"msg":"ok",...}
  /ky/admin/version       → {"code":200,"msg":"ok",...}
  /ky/admin/callback      → {"code":200,"msg":"ok",...}

msg 的取值可从 /root/sukehappy_mock/msg.txt 读（调试用，默认空串）。
"""

import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HOST = '127.0.0.1'
PORT = int(os.environ.get('SUKE_MOCK_PORT', '8099'))
MSG_FILE = '/root/sukehappy_mock/msg.txt'
ACCESS_LOG = '/root/sukehappy_mock/access.log'


def read_msg():
    try:
        with open(MSG_FILE, 'r', encoding='utf-8') as f:
            return f.read().strip('\r\n')
    except OSError:
        return ''


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    server_version = 'sukehappy-mock'

    def log_message(self, fmt, *args):
        pass  # 静默默认日志

    def _send(self, obj):
        body = json.dumps(obj, ensure_ascii=False).encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        # 必须关闭连接：curl 默认 keep-alive，不关会让连接堆积
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        try:
            with open(ACCESS_LOG, 'a', encoding='utf-8') as f:
                f.write('%s %s\n' % (self.log_date_time_string(), self.path))
        except OSError:
            pass

        u = urlparse(self.path)
        path = u.path
        q = parse_qs(u.query)

        if path.startswith('/ky/admin/template'):
            # 结构必须与授权中心真实响应一致（只有 code 与 msg）。
            # 多给 data 会让模板走进不同分支，甚至卡死。
            self._send({'code': 200, 'msg': read_msg()})
        elif path.startswith('/ky/admin/verification'):
            self._send({'code': 200, 'msg': 'ok', 'data': {'query': True}})
        elif path.startswith('/ky/admin/version'):
            self._send({
                'code': 200, 'msg': 'ok', 'status': 200,
                'data': {'version': '1.0.7.6', 'status': 200},
            })
        elif path.startswith('/ky/admin/callback'):
            self._send({'code': 200, 'msg': 'ok', 'data': {'query': True}})
        elif path.rstrip('/') == '/ky/admin':
            # ki 留空：假 ki 会让 v33 中间件拿它去验非法 token，
            # 走进 firebase/php-jwt 异常路径 → zend_mm_heap corrupted（worker SIGSEGV）
            self._send({'code': 200, 'ki': '', 'msg': 'ok'})
        else:
            self._send({'code': 404, 'msg': 'not mocked: ' + path})

    do_POST = do_GET


def main():
    srv = ThreadingHTTPServer((HOST, PORT), Handler)
    srv.daemon_threads = True
    sys.stderr.write('sukehappy mock listening on %s:%d\n' % (HOST, PORT))
    sys.stderr.flush()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
