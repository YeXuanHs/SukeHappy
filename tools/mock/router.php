<?php
/**
 * sukehappy 本地授权 mock 服务
 *
 * 用途：让模板对 apiv1.sukeyun.com 的请求全部落到本地，返回「授权通过」的响应。
 * 扩展把 curl 的 URL 改写到本服务，curl 正常执行并拿到 200 —— 所以模板
 * 既不会看到 CURL ERROR，也不会走进任何异常路径。
 *
 * 重要：本服务必须用**不加载 sukehappy 扩展**的 php.ini 启动，
 * 否则扩展会把本服务自己的 curl 请求也改写过来，造成自我递归挂死。
 *
 * msg 的取值从 msg.txt 读（用于调试不同语义）。
 */

$uri  = $_SERVER['REQUEST_URI'] ?? '/';
$path = parse_url($uri, PHP_URL_PATH) ?: '/';
parse_str((string)parse_url($uri, PHP_URL_QUERY), $params);

$msg = '';
$mf = '/root/sukehappy_mock/msg.txt';
if (is_readable($mf)) {
    $msg = rtrim(file_get_contents($mf), "\r\n");
}

header('Content-Type: application/json; charset=utf-8');
header('X-SukeHappy-Mock: 1');

/* 授权校验：code=200；msg 的语义由 msg.txt 决定 */
if (strpos($path, '/ky/admin/template') === 0) {
    echo json_encode([
        'code' => 200,
        'msg'  => $msg,
        'data' => [
            'query'      => true,
            'domain'     => $params['url'] ?? '',
            'version'    => '1.0.7.6',
            'status'     => 200,
            'activation' => false,
        ],
    ], JSON_UNESCAPED_UNICODE);
    exit;
}

/* 取 ki：ki 是 v33 验 JWT 的密钥。
 * 填假 ki 会让中间件拿它去验非法 token，走进 firebase/php-jwt 异常路径
 * 导致 zend_mm_heap corrupted（worker SIGSEGV）；留空则中间件直接判
 * 「非法请求」，不进 JWT 库，不会崩。 */
if ($path === '/ky/admin' || $path === '/ky/admin/') {
    echo json_encode([
        'code' => 200,
        'ki'   => '',
        'msg'  => 'ok',
    ], JSON_UNESCAPED_UNICODE);
    exit;
}

/* 安全码校验：这个接口要求 msg == "ok" */
if (strpos($path, '/ky/admin/verification') === 0) {
    echo json_encode([
        'code' => 200,
        'msg'  => 'ok',
        'data' => ['query' => true],
    ], JSON_UNESCAPED_UNICODE);
    exit;
}

/* 在线更新检查 */
if (strpos($path, '/ky/admin/version') === 0) {
    echo json_encode([
        'code'   => 200,
        'msg'    => 'ok',
        'status' => 200,
        'data'   => [
            'version'    => '1.0.7.6',
            'status'     => 200,
            'phpversion' => PHP_VERSION,
        ],
    ], JSON_UNESCAPED_UNICODE);
    exit;
}

/* 更新完成回调 */
if (strpos($path, '/ky/admin/callback') === 0) {
    echo json_encode([
        'code' => 200,
        'msg'  => 'ok',
        'data' => ['query' => true],
    ], JSON_UNESCAPED_UNICODE);
    exit;
}

echo json_encode(['code' => 404, 'msg' => 'not mocked: ' . $path], JSON_UNESCAPED_UNICODE);
