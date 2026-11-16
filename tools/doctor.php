<?php
/**
 * SukeHappy 体检（单进程，不依赖 exec）
 *
 *   php doctor.php --root=/www/wwwroot/你的站点 [--host=你的域名] [--url=https://你的域名]
 *
 * 引导站点应用后直接调用模板自己的 templateAuthorization() / getNewVersion()。
 * 授权检查放最后执行：该函数未授权时会 exit，用输出缓冲 + shutdown 钩子兜住。
 */

$opt  = getopt('', ['root::', 'host::', 'url::']);
$root = rtrim($opt['root'] ?? '', '/');
$host = trim($opt['host'] ?? '');
$url  = rtrim($opt['url'] ?? '', '/');
if ($host === '' && $url !== '') { $host = parse_url($url, PHP_URL_HOST) ?: ''; }

if ($root === '') {
    fwrite(STDERR, "用法: php doctor.php --root=/站点根 [--host=域名] [--url=https://域名]\n");
    exit(2);
}

$rows = [];
$state = ['tpl_ret' => null, 'tpl_out' => ''];

function sh_row(&$rows, $name, $ok, $detail) { $rows[] = [$name, $ok, $detail]; }

function sh_finish() {
    global $rows, $state;

    while (ob_get_level() > 0) { ob_end_clean(); }

    if ($state['tpl_ret'] === null && $state['tpl_out'] !== '') {
        $msg = trim(preg_replace('/\s+/', ' ', $state['tpl_out']));
        if (strpos($msg, '授权错误') !== false || preg_match('/U-[123]/', $msg)) {
            sh_row($rows, '授权状态', false, '未授权：' . mb_substr($msg, 0, 60));
        } else {
            sh_row($rows, '授权状态', null, '过程终止：' . mb_substr($msg, 0, 60));
        }
    } elseif ($state['tpl_ret'] === null) {
        sh_row($rows, '授权状态', null, '未返回结果（进程被终止，无输出）');
    }

    echo "\n";
    foreach ($rows as $r) {
        $tag = $r[1] === null ? ' ?  ' : ($r[1] ? ' OK ' : 'FAIL');
        printf("  [%s] %-22s %s\n", $tag, $r[0], $r[2]);
    }
    $fails = 0; $unks = 0;
    foreach ($rows as $r) {
        if ($r[1] === false) { $fails++; }
        elseif ($r[1] === null) { $unks++; }
    }
    echo "\n  ", $fails === 0 ? '全部通过' : "有 $fails 项失败",
         $unks ? "，$unks 项待确认" : '', "\n";
}

function sh_http($url, $timeout = 6) {
    $ctx = stream_context_create([
        'http' => ['method' => 'GET', 'timeout' => $timeout, 'ignore_errors' => true,
                   'header' => "User-Agent: SukeHappy-Doctor/1.0\r\n", 'follow_location' => 0],
        'ssl'  => ['verify_peer' => false, 'verify_peer_name' => false],
    ]);
    @file_get_contents($url, false, $ctx);
    if (isset($http_response_header[0]) && preg_match('#\s(\d{3})#', $http_response_header[0], $m)) return (int)$m[1];
    return 0;
}

register_shutdown_function('sh_finish');

echo str_repeat('=', 72), "\n  SukeHappy 体检 · $root", ($host ? " ($host)" : ''), "\n", str_repeat('=', 72), "\n";

/* 1. 扩展 */
$loaded = extension_loaded('sukehappy');
sh_row($rows, '扩展已加载', $loaded, $loaded ? 'sukehappy' : '未加载（php.ini 没配 / ABI 不符 / 未重载 fpm）');
if ($loaded) {
    $cfg = ['block_functions', 'block_uris', 'block_sql'];
    foreach ($cfg as $k) {
        $v = ini_get('sukehappy.' . $k);
        sh_row($rows, "配置 $k", true, ($v === false || $v === '') ? '(空)' : $v);
    }
}

/* 2. 引导应用 */
$_SERVER['REQUEST_METHOD']  = 'GET';
$_SERVER['REQUEST_URI']     = '/';
$_SERVER['SCRIPT_NAME']     = '/index.php';
$_SERVER['SCRIPT_FILENAME'] = $root . '/public/index.php';
$_SERVER['SERVER_PORT']     = '443';
$_SERVER['REQUEST_SCHEME']  = 'https';
$_SERVER['HTTPS']           = 'on';
if ($host !== '') { $_SERVER['HTTP_HOST'] = $host; $_SERVER['SERVER_NAME'] = $host; }

$booted = false;
if (!is_file($root . '/vendor/autoload.php')) {
    sh_row($rows, '引导应用', false, 'BOOTSTRAP_FAIL: 找不到 ' . $root . '/vendor/autoload.php');
} else {
    try {
        /* 按站点真实入口 public/index.php 的方式引导：常量 + vendor/thinkphp/base.php */
        if (!defined('CMF_ROOT')) { define('CMF_ROOT', $root . '/'); }
        if (!defined('APP_PATH')) { define('APP_PATH', CMF_ROOT . 'app/'); }
        if (!defined('CMF_DATA')) { define('CMF_DATA', CMF_ROOT . 'data/'); }
        if (!defined('WEB_ROOT')) { define('WEB_ROOT', $root . '/public/'); }
        if (!defined('UPLOAD_PATH')) { define('UPLOAD_PATH', CMF_ROOT . 'uploads/'); }
        if (!defined('DOWN_PATH')) { define('DOWN_PATH', CMF_ROOT . 'downloads/'); }

        require CMF_ROOT . 'vendor/thinkphp/base.php';
        $app = \think\Container::get('app', [APP_PATH]);
        $app->initialize();
        $booted = true;
        sh_row($rows, '引导应用', true, 'think\App 初始化完成');
    } catch (\Throwable $e) {
        sh_row($rows, '引导应用', false, 'BOOTSTRAP_FAIL: ' . $e->getMessage());
    }
}

/* 3. 主动刷新授权（getAuthorizationCurl 会写 ki 缓存） */
if ($booted) {
    if (!function_exists('getAuthorizationCurl') && is_file($root . '/app/template/common.php')) {
        try { require_once $root . '/app/template/common.php'; } catch (\Throwable $e) {}
    }
    if (function_exists('getAuthorizationCurl')) {
        try {
            /* getAuthorizationCurl($token)：用请求头里的 token 作为凭据 */
            $token = isset($_SERVER['HTTP_ACCEPT_TOKEN']) ? $_SERVER['HTTP_ACCEPT_TOKEN'] : '';
            $a = getAuthorizationCurl($token);
            $code = is_array($a) ? ($a['code'] ?? null) : null;
            $ki   = is_array($a) ? ($a['ki'] ?? '') : '';
            $ok   = ($code == 200) && is_string($ki) && $ki !== '';
            sh_row($rows, '授权刷新', $ok,
                $ok ? '成功（code=200，ki ' . strlen($ki) . ' 字节）'
                    : '失败（code=' . var_export($code, true) . '）');
        } catch (\Throwable $e) {
            sh_row($rows, '授权刷新', null, '调用失败：' . $e->getMessage());
        }
    } else {
        sh_row($rows, '授权刷新', null, 'getAuthorizationCurl 不存在');
    }
}

/* 4. 授权缓存 */
if ($booted) {
    try {
        $ki  = \think\facade\Cache::get('hzdtqmwbodere7d9');
        $web = \think\facade\Cache::get('8ue6hpsuffpheaaj');
        $hasKi = is_string($ki) && $ki !== '';
        sh_row($rows, '授权缓存 ki', $hasKi,
            $hasKi ? '有（' . strlen($ki) . ' 字节）' : '没有 → 后台可能被判未授权');
        sh_row($rows, '授权标记 web', $web === 'web', $web === 'web' ? '"web" 已写入' : '未写入');
    } catch (\Throwable $e) {
        sh_row($rows, '授权缓存', null, '读取失败：' . $e->getMessage());
    }
}

/* 4. 在线更新（不 exit，可以先跑） */
if ($booted) {
    if (!function_exists('getNewVersion') && is_file($root . '/app/template/common.php')) {
        try { require_once $root . '/app/template/common.php'; } catch (\Throwable $e) {}
    }
    if (function_exists('getNewVersion')) {
        try {
            /* getNewVersion($upid)：签名要一个参数，传站点标识即可 */
            $upid = parse_url('https://' . $host, PHP_URL_HOST) ?: 'doctor';
            $v = getNewVersion($upid);
            $hasErr = is_array($v) && array_key_exists('error', $v);
            if (!$hasErr) {
                $ver = is_array($v) ? ($v['version'] ?? null) : null;
                sh_row($rows, '在线更新', true, '可用' . ($ver ? "，最新版本 $ver" : ''));
            } else {
                sh_row($rows, '在线更新', false, '不可用：' . mb_substr((string)$v['error'], 0, 60));
            }
        } catch (\Throwable $e) {
            sh_row($rows, '在线更新', null, '调用失败：' . $e->getMessage());
        }
    } else {
        sh_row($rows, '在线更新', null, 'getNewVersion 不存在');
    }
}

/* 5. 后门入口（可选） */
if ($url !== '') {
    $paths = ['/i5nyyoblvmcpuj71', '/o5tlkifzcrrgcvhx'];
    foreach ($paths as $p) {
        $c = sh_http($url . $p);
        sh_row($rows, "后门入口 $p", in_array($c, [403, 404, 444], true), $c ? "HTTP $c" : '连不上');
    }
} else {
    sh_row($rows, '后门入口', null, '未给 --url，跳过');
}

/* 6. 授权状态 —— 放最后：未授权时它会 exit */
if ($booted && function_exists('templateAuthorization')) {
    ob_start();
    try {
        /* templateAuthorization($retry = 0) */
        $r = templateAuthorization(0);
        $state['tpl_ret'] = is_scalar($r) ? $r : true;
        $state['tpl_out'] = '';
        while (ob_get_level() > 0) { ob_end_clean(); }
        sh_row($rows, '授权状态', true, '已授权（templateAuthorization 正常返回）');
    } catch (\Throwable $e) {
        $state['tpl_out'] = ob_get_clean();
        $state['tpl_ret'] = null;
        sh_row($rows, '授权状态', false, '异常：' . mb_substr($e->getMessage(), 0, 60));
    }
} elseif ($booted) {
    sh_row($rows, '授权状态', null, 'templateAuthorization 不存在');
}
