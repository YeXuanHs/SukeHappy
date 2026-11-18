<?php
/**
 * 模板后台登录：账号校验 / 首次创建 / 签发 JWT
 *
 * 由 router.php 在命中 login 路径时 require 进来，
 * 直接使用 router 里已建好的 $pdo / $pf / $body / $hasAccount。
 */

/* 首次登录：直接创建管理员（长度下限 6） */
if (!$hasAccount) {
    $user = trim((string) ($body['username'] ?? $body['user'] ?? $body['email'] ?? $body['account'] ?? ''));
    $pass = (string) ($body['password'] ?? $body['pass'] ?? '');

    if ($user === '' || $pass === '') {
        jout(400, null, '请输入账号和密码');
    }
    if (strlen($pass) < 6) {
        jout(400, null, '密码至少 6 位');
    }

    cfg_set($pdo, $pf, 'mb_admin_user', $user, '后台管理员账号');
    cfg_set($pdo, $pf, 'mb_admin_pass', password_hash($pass, PASSWORD_DEFAULT), '后台管理员密码哈希');
    if (strpos($user, '@') !== false) {
        cfg_set($pdo, $pf, 'mb_admin_email', $user, '后台管理员邮箱');
    }

    $token = suke_issue_token($user);
    setcookie('mdy_mb_token', $token, time() + SUKE_JWT_TTL, '/', '', false, true);
    $uinfo = suke_user_info($pdo, $pf, $user, $token);
    suke_note_login($pdo, $pf);
    jout(200, [
        'token'  => $token,
        'expire' => SUKE_JWT_TTL,
        'userInfo' => $uinfo,
    ], '管理员账号已创建');
}

/* 正常登录 */
$user = trim((string) ($body['username'] ?? $body['user'] ?? $body['email'] ?? $body['account'] ?? ''));
$pass = (string) ($body['password'] ?? $body['pass'] ?? '');

if ($user === '' || $pass === '') {
    jout(400, null, '请输入账号和密码');
}
if ($user !== $adminUser) {
    jout(400, null, '账号或密码错误');
}
if (!password_verify($pass, $adminPass)) {
    jout(400, null, '账号或密码错误');
}

$token = suke_issue_token($user);
setcookie('mdy_mb_token', $token, time() + SUKE_JWT_TTL, '/', '', false, true);
$uinfo = suke_user_info($pdo, $pf, $user, $token);
suke_note_login($pdo, $pf);
jout(200, [
    'token'  => $token,
    'expire' => SUKE_JWT_TTL,
    'userInfo' => $uinfo,
], 'success');
