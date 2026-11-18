<?php
/**
 * 客户端 IP / IP 归属地
 *
 * 归属地复用模板自带的 ip2region（app/template/vendor/zoujingli/ip2region），
 * 与后台「监控 → 地区」用的是同一份数据，所以显示格式保持一致：
 *   中国|0|福建省|莆田市|电信  →  中国福建省莆田市[电信]
 */

function suke_client_ip()
{
    $keys = ['HTTP_X_FORWARDED_FOR', 'HTTP_X_REAL_IP', 'HTTP_CF_CONNECTING_IP', 'REMOTE_ADDR'];
    foreach ($keys as $k) {
        $v = isset($_SERVER[$k]) ? $_SERVER[$k] : '';
        if ($v === '') continue;
        $ip = trim(explode(',', $v)[0]);
        if (filter_var($ip, FILTER_VALIDATE_IP)) return $ip;
    }
    return '';
}

function suke_ip_region($ip)
{
    if (!$ip) return '';

    static $searcher = null;

    if ($searcher === null) {
        $file = dirname(__DIR__, 3) . '/app/template/vendor/zoujingli/ip2region/Ip2Region.php';
        if (!is_file($file)) {
            $searcher = false;
            return '';
        }
        try {
            require_once $file;
            $searcher = new Ip2Region();
        } catch (Throwable $e) {
            $searcher = false;
            return '';
        }
    }
    if (!$searcher) return '';

    try {
        $r = $searcher->memorySearch($ip);
    } catch (Throwable $e) {
        return '';
    }

    $region = is_array($r) && isset($r['region']) ? $r['region'] : '';
    if ($region === '') return '';

    $p = explode('|', $region);
    $country = isset($p[0]) ? $p[0] : '';
    $prov    = isset($p[2]) ? $p[2] : '';
    $city    = isset($p[3]) ? $p[3] : '';
    $isp     = isset($p[4]) ? $p[4] : '';

    $out = '';
    foreach ([$country, $prov, $city] as $seg) {
        if ($seg !== '' && $seg !== '0') $out .= $seg;
    }
    if ($isp !== '' && $isp !== '0' && $isp !== $city) {
        $out .= '[' . $isp . ']';
    }

    return $out;
}

/**
 * 首字母头像（内联 SVG，data URI）
 * 不依赖任何图片文件，换站点/换主题都不会 404
 */
function suke_letter_avatar($name, $size = 80)
{
    $ch = strtoupper(mb_substr((string) $name, 0, 1, 'UTF-8'));
    if ($ch === '') $ch = 'A';

    $svg = '<svg xmlns="http://www.w3.org/2000/svg" width="' . $size . '" height="' . $size . '" viewBox="0 0 ' . $size . ' ' . $size . '">'
         . '<rect width="' . $size . '" height="' . $size . '" rx="' . ($size / 2) . '" fill="#0052d9"/>'
         . '<text x="50%" y="50%" dy="0.35em" text-anchor="middle" fill="#fff" '
         . 'font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif" font-size="' . ($size * 0.44) . '" '
         . 'font-weight="600">' . htmlspecialchars($ch, ENT_QUOTES, 'UTF-8') . '</text></svg>';

    return 'data:image/svg+xml;base64,' . base64_encode($svg);
}
