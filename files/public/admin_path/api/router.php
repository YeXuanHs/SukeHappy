<?php
/**
 * 模板后台（mbadmin）本地账号 API
 *
 * 原后台把「登录 / 注册 / 找回密码 / 会话校验」全打到远端
 * https://apiv1.sukeyun.com/v1/*，本文件把它改成读本地库 shd_tpl_config。
 *
 * 前端通过 api-local.js 劫持 XHR，把 apiv1.sukeyun.com 的请求转到
 * /mbadmin/api/router.php?__p=<原路径+query>，这里按路径分支处理。
 *
 * 账号存储（shd_tpl_config，key-value 表）：
 *   label=mb_admin_user   value=账号
 *   label=mb_admin_pass   value=password_hash()
 *   label=mb_admin_email  value=邮箱（可选）
 */

header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');

$ROOT = dirname(__DIR__, 3);                 // public/mbadmin/api -> 站点根
$cfg  = require $ROOT . '/app/config/database.php';
$pf   = $cfg['prefix'] ?? 'shd_';
$authcode = $cfg['authcode'] ?? 'mdy';

$orig = $_GET['__p'] ?? '';
$path = parse_url($orig, PHP_URL_PATH) ?: '';
$method = $_SERVER['REQUEST_METHOD'];

/* ---------- 日志（调试，最多保留 200 行） ---------- */
$logFile = '/tmp/mb_local_api.log';
@file_put_contents($logFile, date('m-d H:i:s') . " $method $orig\n", FILE_APPEND | LOCK_EX);
if (@filesize($logFile) > 32768) { @file_put_contents($logFile, ''); }

function jout($code, $data = null, $message = '') {
    echo json_encode(['code' => $code, 'data' => $data, 'message' => $message], JSON_UNESCAPED_UNICODE);
    exit;
}

/* ---------- 数据库 ---------- */
try {
    $pdo = new PDO(
        "mysql:host={$cfg['hostname']};port={$cfg['hostport']};dbname={$cfg['database']};charset=utf8mb4",
        $cfg['username'], $cfg['password'],
        [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION, PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC]
    );
} catch (Throwable $e) {
    jout(500, null, 'database unavailable');
}

function cfg_get(PDO $pdo, $pf, $label) {
    $st = $pdo->prepare("SELECT value FROM {$pf}tpl_config WHERE label = ? LIMIT 1");
    $st->execute([$label]);
    $r = $st->fetch();
    return $r ? $r['value'] : null;
}

function cfg_set(PDO $pdo, $pf, $label, $value, $desc = '', $type = 'admin') {
    $st = $pdo->prepare("SELECT label FROM {$pf}tpl_config WHERE label = ? LIMIT 1");
    $st->execute([$label]);
    if ($st->fetch()) {
        $up = $pdo->prepare("UPDATE {$pf}tpl_config SET value = ? WHERE label = ?");
        $up->execute([$value, $label]);
    } else {
        $in = $pdo->prepare("INSERT INTO {$pf}tpl_config (label, value, hidden, description, type) VALUES (?,?,0,?,?)");
        $in->execute([$label, $value, $desc, $type]);
    }
}

/* ---------- 会话（HMAC 签名 cookie，不依赖 PHP session） ---------- */
function make_token($user, $authcode) {
    $exp = time() + 86400 * 7;
    $payload = $user . '|' . $exp;
    $sig = hash_hmac('sha256', $payload, $authcode);
    return base64_encode($payload . '|' . $sig);
}

function check_token($authcode) {
    $raw = $_COOKIE['mdy_mb_token'] ?? '';
    if (!$raw) return null;
    $dec = base64_decode($raw, true);
    if (!$dec) return null;
    $parts = explode('|', $dec);
    if (count($parts) !== 3) return null;
    list($user, $exp, $sig) = $parts;
    if ((int)$exp < time()) return null;
    if (!hash_equals(hash_hmac('sha256', $user . '|' . $exp, $authcode), $sig)) return null;
    return $user;
}

/* ---------- 账号状态 ---------- */
$adminUser = cfg_get($pdo, $pf, 'mb_admin_user');
$adminPass = cfg_get($pdo, $pf, 'mb_admin_pass');
$hasAccount = ($adminUser !== null && $adminUser !== '' && $adminPass !== null && $adminPass !== '');

/* ---------- 读取 JSON body ---------- */
$body = [];
$rawInput = file_get_contents('php://input');
if ($rawInput) {
    $tmp = json_decode($rawInput, true);
    if (is_array($tmp)) $body = $tmp;
}
if (!$body) $body = $_POST;

/* ---------- 路由 ---------- */
$lower = strtolower($path);

/* 1) 状态查询：给前端判断显示「创建管理员」还是「登录」 */
if (strpos($lower, 'status') !== false) {
    jout(200, ['hasAccount' => $hasAccount]);
}

/* 2) 会话校验（页面加载时调用） */
if (strpos($lower, 'verify') !== false) {
    $u = check_token($authcode);
    if ($u === null) {
        jout(400, null, '未登录');
    }
    jout(200, [
        'token' => $u,
        'expire' => 518400,
        'userInfo' => [
            'user_token' => $u,
            'username' => $u,
            'email' => cfg_get($pdo, $pf, 'mb_admin_email') ?: ($u . '@local'),
            'nickname' => $u,
        ],
    ], 'success');
}

/* 2.5) 登录后修改密码 */
if (strpos(strtolower($path), 'changepassword') !== false) {
    $me = check_token($authcode);
    if ($me === null) {
        jout(401, null, '请先登录');
    }
    if (!$hasAccount) {
        jout(400, null, '尚未创建管理员账号');
    }
    $cur = (string)($body['current'] ?? $body['old_password'] ?? '');
    $new = (string)($body['password'] ?? $body['new_password'] ?? '');
    if ($cur === '' || $new === '') {
        jout(400, null, '请输入当前密码和新密码');
    }
    if (strlen($new) < 6) {
        jout(400, null, '新密码至少 6 位');
    }
    if ($new === $cur) {
        jout(400, null, '新密码不能与当前密码相同');
    }
    if (!password_verify($cur, $adminPass)) {
        jout(400, null, '当前密码不正确');
    }
    cfg_set($pdo, $pf, 'mb_admin_pass', password_hash($new, PASSWORD_DEFAULT), '后台管理员密码哈希');
    jout(200, null, '密码已修改');
}

/* 3) 注册 / 找回密码：一律关闭 */
if (strpos($lower, 'register') !== false || strpos($lower, 'reset') !== false
    || strpos($lower, 'forget') !== false || strpos($lower, 'forgot') !== false
    || strpos($lower, 'sms') !== false || strpos($lower, 'code') === 0) {
    jout(403, null, '该功能已关闭');
}

/* 3.5) 菜单：远端站点未授权取不到，本地返回空菜单但放行（只渲染骨架） */
if (strpos($lower, 'mymenus') !== false) {
    jout(200, ['menu' => [], 'permissions' => ['*/*/*']], 'success');
}

/* 4) 登录 / 首次创建 */
if (strpos($lower, 'login') !== false) {
    $user = trim((string)($body['username'] ?? $body['user'] ?? $body['email'] ?? $body['account'] ?? ''));
    $pass = (string)($body['password'] ?? $body['pass'] ?? '');

    if ($user === '' || $pass === '') {
        jout(400, null, '请输入账号和密码');
    }

    if (!$hasAccount) {
        /* 首次：直接创建管理员（长度下限 6） */
        if (strlen($pass) < 6) {
            jout(400, null, '密码至少 6 位');
        }
        cfg_set($pdo, $pf, 'mb_admin_user', $user, '后台管理员账号');
        cfg_set($pdo, $pf, 'mb_admin_pass', password_hash($pass, PASSWORD_DEFAULT), '后台管理员密码哈希');
        if (strpos($user, '@') !== false) {
            cfg_set($pdo, $pf, 'mb_admin_email', $user, '后台管理员邮箱');
        }
        setcookie('mdy_mb_token', make_token($user, $authcode), time() + 86400 * 7, '/', '', false, true);
        jout(200, [
            'token' => $user, 'expire' => 518400,
            'userInfo' => ['user_token' => $user, 'username' => $user, 'email' => $user, 'nickname' => $user],
        ], '管理员账号已创建');
    }

    /* 正常登录 */
    if ($user !== $adminUser) {
        jout(400, null, '账号或密码错误');
    }
    if (!password_verify($pass, $adminPass)) {
        jout(400, null, '账号或密码错误');
    }
    setcookie('mdy_mb_token', make_token($user, $authcode), time() + 86400 * 7, '/', '', false, true);
    jout(200, [
        'token' => $user, 'expire' => 518400,
        'userInfo' => ['user_token' => $user, 'username' => $user,
                       'email' => cfg_get($pdo, $pf, 'mb_admin_email') ?: $user, 'nickname' => $user],
    ], 'success');
}

/* 5) 登出 */
if (strpos($lower, 'logout') !== false) {
    setcookie('mdy_mb_token', '', time() - 3600, '/');
    jout(200, null, '已退出');
}

/* 6) 其他：远端接口已不可用，返回空数据
 *    注意不能返回 data:null —— 前端不少地方直接对 t.data 取 .length / .map，
 *    会抛 TypeError 把整个布局渲染打断。返回空数组最安全。 */
jout(200, [], '');
