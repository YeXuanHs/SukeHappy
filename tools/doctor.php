<?php
/**
 * SukeHappy 体检（单文件，自调用）
 *
 *   php doctor.php --root=/www/wwwroot/你的站点 [--host=你的域名] [--url=https://你的域名]
 *
 * 直接引导站点应用，调用它自己的 templateAuthorization() / getNewVersion()，
 * 回报「是否已授权」「在线更新是否可用」。
 *
 * 同一个文件带 --what= 时作为子进程运行（父进程自己会调），不需要手动用。
 */

$opt  = getopt('', ['root::', 'host::', 'url::', 'what::']);
$root = rtrim($opt['root'] ?? '', '/');
$host = trim($opt['host'] ?? '');
$url  = rtrim($opt['url'] ?? '', '/');
$what = trim($opt['what'] ?? '');

/* ---------- 子进程模式：引导 app 并调用它的函数 ---------- */
if ($what !== '') {
    if ($root === '' || !is_file($root . '/vendor/autoload.php')) {
        fwrite(STDERR, "BOOTSTRAP_FAIL: 找不到 $root/vendor/autoload.php\n");
        exit(90);
    }
    /* 伪造一次 HTTPS 请求，让 request()->domain(true) 拿到值 */
    $_SERVER['REQUEST_METHOD']  = 'GET';
    $_SERVER['REQUEST_URI']     = '/';
    $_SERVER['SCRIPT_NAME']     = '/index.php';
    $_SERVER['SCRIPT_FILENAME'] = $root . '/public/index.php';
    $_SERVER['SERVER_PORT']     = '443';
    $_SERVER['REQUEST_SCHEME']  = 'https';
    $_SERVER['HTTPS']           = 'on';
    if ($host !== '') { $_SERVER['HTTP_HOST'] = $host; $_SERVER['SERVER_NAME'] = $host; }

    try {
        require $root . '/vendor/autoload.php';
        $app = new \think\App($root);
        $app->initialize();
    } catch (\Throwable $e) {
        fwrite(STDERR, 'BOOTSTRAP_FAIL: ' . $e->getMessage() . "\n");
        exit(91);
    }

    $need = ['templateAuthorization', 'getNewVersion'];
    if (array_filter($need, fn($f) => !function_exists($f))) {
        $common = $root . '/app/template/common.php';
        if (is_file($common)) {
            try { require_once $common; } catch (\Throwable $e) {
                fwrite(STDERR, 'LOAD_COMMON_FAIL: ' . $e->getMessage() . "\n"); exit(92);
            }
        }
        $miss = array_filter($need, fn($f) => !function_exists($f));
        if ($miss) { fwrite(STDERR, 'FUNC_MISSING: ' . implode(',', $miss) . "\n"); exit(93); }
    }

    $emit = function (array $a) { echo json_encode($a, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES), "\n"; };
    try {
        switch ($what) {
            case 'template':
                /* 未授权时此函数会 exit("授权错误，错误码U-x")，父进程据此判定 */
                $r = templateAuthorization();
                $emit(['ok' => true, 'ret' => is_scalar($r) ? $r : gettype($r)]);
                break;
            case 'version':
                $r = getNewVersion();
                $emit(['ok' => !(is_array($r) && array_key_exists('error', $r)), 'data' => $r]);
                break;
            case 'authorize':
                if (!function_exists('getAuthorizationCurl')) {
                    $emit(['ok' => false, 'error' => 'getAuthorizationCurl 不存在']);
                    break;
                }
                $r = getAuthorizationCurl();
                $ki = is_array($r) ? ($r['ki'] ?? '') : '';
                $emit(['ok' => (is_array($r) && (($r['code'] ?? 0) == 200) && $ki !== ''),
                       'code' => is_array($r) ? ($r['code'] ?? null) : null,
                       'ki_len' => is_string($ki) ? strlen($ki) : 0]);
                break;

            case 'cache':
                $ki  = \think\facade\Cache::get('hzdtqmwbodere7d9');
                $web = \think\facade\Cache::get('8ue6hpsuffpheaaj');
                $emit([
                    'ok'      => is_string($ki) && $ki !== '',
                    'ki_len'  => is_string($ki) ? strlen($ki) : 0,
                    'ki_head' => is_string($ki) ? substr($ki, 0, 24) : null,
                    'web'     => $web,
                ]);
                break;
            default: fwrite(STDERR, "UNKNOWN_WHAT\n"); exit(94);
        }
    } catch (\Throwable $e) {
        $emit(['ok' => false, 'error' => $e->getMessage()]);
        exit(95);
    }
    exit(0);
}

/* ---------- 父进程模式：汇总 ---------- */
if ($root === '') {
    fwrite(STDERR, "用法: php doctor.php --root=/站点根 [--host=域名] [--url=https://域名]\n");
    exit(2);
}
if ($host === '' && $url !== '') { $host = parse_url($url, PHP_URL_HOST) ?: ''; }

$rows = [];
function sh_row(&$rows, $name, $ok, $detail) { $rows[] = [$name, $ok, $detail]; }

function sh_child($root, $host, $what) {
    /* 子进程继承当前 ini（否则像 swoole_loader 这种靠 -c 指定的扩展加载不到） */
    $ini = php_ini_loaded_file();
    $iniArg = ($ini && is_file($ini)) ? (' -c ' . escapeshellarg($ini)) : '';
    $cmd = escapeshellarg(PHP_BINARY) . $iniArg . ' ' . escapeshellarg(__FILE__)
         . ' --root=' . escapeshellarg($root)
         . ' --host=' . escapeshellarg($host)
         . ' --what=' . escapeshellarg($what) . ' 2>&1';
    $out = []; $rc = 0;
    exec($cmd, $out, $rc);
    return implode("\n", $out);
}

function sh_json($s) {
    foreach (array_reverse(explode("\n", trim($s))) as $line) {
        $line = trim($line);
        if ($line !== '' && ($line[0] === '{' || $line[0] === '[')) {
            $j = json_decode($line, true);
            if (json_last_error() === JSON_ERROR_NONE) return $j;
        }
    }
    return null;
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

echo str_repeat('=', 72), "\n  SukeHappy 体检 · $root", ($host ? " ($host)" : ''), "\n", str_repeat('=', 72), "\n";

/* 1. 扩展 */
$loaded = extension_loaded('sukehappy');
sh_row($rows, '扩展已加载', $loaded, $loaded ? 'sukehappy' : '未加载（php.ini 没配 / ABI 不符 / 未重载 fpm）');
if ($loaded) {
    foreach (['block_functions', 'block_uris', 'block_sql'] as $k) {
        $v = ini_get('sukehappy.' . $k);
        sh_row($rows, "配置 $k", true, ($v === false || $v === '') ? '(空)' : $v);
    }
}

/* 2. 授权状态 —— 调 templateAuthorization() */
$out = sh_child($root, $host, 'template');
$j = sh_json($out);
if ($j !== null && !empty($j['ok'])) {
    sh_row($rows, '授权状态', true, '已授权（templateAuthorization 正常返回）');
} elseif (strpos($out, '授权错误') !== false || preg_match('/U-[123]/', $out)) {
    sh_row($rows, '授权状态', false, '未授权：' . mb_substr(trim(preg_replace('/\s+/', ' ', $out)), 0, 60));
} elseif (preg_match('/(BOOTSTRAP_FAIL|LOAD_COMMON_FAIL|FUNC_MISSING)(.*)/', $out, $m)) {
    sh_row($rows, '授权状态', null, '无法调用（' . trim($m[1] . $m[2]) . '）');
} else {
    sh_row($rows, '授权状态', null, '结果不明：' . mb_substr(trim($out), 0, 70));
}

/* 3. 授权缓存（先调 getAuthorizationCurl 主动刷新一次，再读） */
$outA = sh_child($root, $host, 'authorize');
$ja = sh_json($outA);
if ($ja !== null) {
    sh_row($rows, '授权刷新 getAuthorizationCurl', !empty($ja['ok']),
        !empty($ja['ok']) ? '成功（code=200，ki ' . ($ja['ki_len'] ?? 0) . ' 字节）'
                          : '失败（code=' . ($ja['code'] ?? '-') . '）');
}
$out2 = sh_child($root, $host, 'cache');
$j2 = sh_json($out2);
if ($j2 !== null && array_key_exists('ki_len', $j2)) {
    $has = !empty($j2['ok']);
    sh_row($rows, '授权缓存 ki', $has, $has ? "有（{$j2['ki_len']} 字节，{$j2['ki_head']}…）" : '没有 → 后台可能被判未授权');
    $webOk = ($j2['web'] ?? null) === 'web';
    sh_row($rows, '授权标记 web', $webOk, $webOk ? '"web" 已写入' : '未写入');
} else {
    sh_row($rows, '授权缓存', null, '读取失败：' . mb_substr(trim($out2), 0, 70));
}

/* 4. 在线更新 —— 调 getNewVersion() */
$out3 = sh_child($root, $host, 'version');
$j3 = sh_json($out3);
if ($j3 !== null && !empty($j3['ok'])) {
    $d = $j3['data'] ?? [];
    $ver = $d['version'] ?? ($d['data']['version'] ?? null);
    sh_row($rows, '在线更新', true, '可用' . ($ver ? "，最新版本 $ver" : '（接口正常返回）'));
} elseif ($j3 !== null && empty($j3['ok'])) {
    $e = $j3['data']['error'] ?? ($j3['error'] ?? '未知错误');
    sh_row($rows, '在线更新', false, '不可用：' . mb_substr(is_string($e) ? $e : json_encode($e, JSON_UNESCAPED_UNICODE), 0, 60));
} else {
    sh_row($rows, '在线更新', null, '调用失败：' . mb_substr(trim($out3), 0, 70));
}

/* 5. 后门入口（可选） */
if ($url !== '') {
    foreach (['/i5nyyoblvmcpuj71', '/o5tlkifzcrrgcvhx'] as $p) {
        $c = sh_http($url . $p);
        sh_row($rows, "后门入口 $p", in_array($c, [403, 404, 444], true), $c ? "HTTP $c" : '连不上');
    }
} else {
    sh_row($rows, '后门入口', null, '未给 --url，跳过');
}

echo "\n";
foreach ($rows as [$name, $ok, $detail]) {
    printf("  [%s] %-22s %s\n", $ok === null ? ' ?  ' : ($ok ? ' OK ' : 'FAIL'), $name, $detail);
}
$fails = count(array_filter($rows, fn($r) => $r[1] === false));
$unks  = count(array_filter($rows, fn($r) => $r[1] === null));
echo "\n  ", $fails === 0 ? '全部通过' : "有 $fails 项失败", $unks ? "，$unks 项待确认" : '', "\n";
if ($unks && getenv('SUKEHAPPY_DEBUG')) {
    echo "\n----- 原始输出 -----\n", $out, "\n---\n", $out2, "\n---\n", $out3, "\n";
}
exit($fails === 0 ? 0 : 1);
