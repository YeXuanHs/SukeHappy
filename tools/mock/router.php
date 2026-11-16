<?php
/**
 * sukehappy 本地授权 mock 服务
 *
 * 用途：让模板对 apiv1.sukeyun.com 的请求全部落到本地，返回「授权通过」的响应。
 * 扩展把 curl 的 URL 改写到这里，curl 本身正常执行（http_code=200、curl_error 为空），
 * 所以模板不会出现 CURL ERROR，也不会触发 worker 崩溃。
 *
 * 启动：php -S 127.0.0.1:8099 -t /root/sukehappy_mock router.php
 * 端口可用环境变量 SUKE_MOCK_PORT 覆盖（需与扩展里的 sukehappy.mock_port 一致）。
 */

$uri  = $_SERVER['REQUEST_URI'] ?? '/';
$path = parse_url($uri, PHP_URL_PATH) ?: '/';
parse_str((string)parse_url($uri, PHP_URL_QUERY), $params);

header('Content-Type: application/json; charset=utf-8');
header('X-SukeHappy-Mock: 1');

/* 授权校验：code=200 且 msg 为空。
 * 模板把 msg 当「错误信息」看，非空即视为授权失败并 exit「授权错误，错误码U-1」。 */
if (strpos($path, '/ky/admin/template') === 0) {
    echo json_encode([
        'code' => 200,
        'msg'  => '',
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
 * 这里留空 —— 填假 ki 会让中间件拿它去验一个非法 token，
 * 走进 firebase/php-jwt 的异常路径，导致 zend_mm_heap corrupted（worker SIGSEGV）。
 * 留空则中间件直接判「非法请求」，不进 JWT 库，不会崩。
 * 要真正登录可用，等账号本地化后由本地接口用同一个 ki 签发 token。 */
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
            'version'     => '1.0.7.6',
            'status'      => 200,
            'phpversion'  => PHP_VERSION,
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
