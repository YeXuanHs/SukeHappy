<?php
/**
 * JWT 工具（HS256）
 *
 * 密钥必须和 sukehappy.so 里 /ky/admin 返回的 ki 一致
 * —— /v33 用 ki 当 HS256 密钥验签，两边不一致请求会被拒（请求失败，-2）
 */

define('SUKE_JWT_SECRET', 'k7Qm2Xp9Za4Lr8Tn6Vc3Wd1Yb5Hs0Jg_');
define('SUKE_JWT_TTL', 604800);   // 7 天

function suke_b64u($raw)
{
    return rtrim(strtr(base64_encode($raw), '+/', '-_'), '=');
}

function suke_b64u_dec($s)
{
    return base64_decode(strtr($s, '-_', '+/'));
}

function suke_jwt_sign(array $claims)
{
    $claims['iat'] = isset($claims['iat']) ? $claims['iat'] : time();
    $claims['exp'] = isset($claims['exp']) ? $claims['exp'] : time() + SUKE_JWT_TTL;

    $h = suke_b64u(json_encode(['typ' => 'JWT', 'alg' => 'HS256']));
    $p = suke_b64u(json_encode($claims, JSON_UNESCAPED_UNICODE));
    $s = suke_b64u(hash_hmac('sha256', $h . '.' . $p, SUKE_JWT_SECRET, true));

    return $h . '.' . $p . '.' . $s;
}

function suke_jwt_verify($jwt)
{
    $t = explode('.', (string) $jwt);
    if (count($t) !== 3) return null;

    $sig = suke_b64u(hash_hmac('sha256', $t[0] . '.' . $t[1], SUKE_JWT_SECRET, true));
    if (!hash_equals($sig, $t[2])) return null;

    $claims = json_decode(suke_b64u_dec($t[1]), true);
    if (!is_array($claims)) return null;
    if (isset($claims['exp']) && (int) $claims['exp'] < time()) return null;

    return $claims;
}

function suke_issue_token($user)
{
    return suke_jwt_sign([
        'uid'      => 1,
        'user_id'  => 1,
        'id'       => 1,
        'sub'      => $user,
        'username' => $user,
        'nickname' => $user,
        'role'     => 'admin',
    ]);
}
