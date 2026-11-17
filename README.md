# SukeHappy · 速科模板授权本地化扩展

针对**源码被 Swoole Compiler 加密**、无法直接改代码的速科（zjmf）模板，
在 PHP 扩展层把授权校验的链路改写掉，同时封掉模板内置的后门入口。

> 版本 **4.0.0** 起只支持 PHP **7.4+**（源码里 `PHP_VERSION_ID < 70400` 直接编译报错）。
> v3 源码留在 `archive/`，可以参考但不要用于生产。

## 它做什么

| # | 作用 | 实现层 | 影响正常业务吗 |
|---|---|---|---|
| 1 | 写模板授权标记 | RINIT 落地缓存文件 | 否 |
| 2 | 拦截后门 URI | RINIT 直接 404 | 否（这两个路径正常业务零引用） |
| 3 | 伪造授权中心应答 | hook `curl_*` 内部函数 | 否（只对 `apiv1.sukeyun.com` 生效） |

**v4 不再需要本地 HTTP mock 服务**（v3 靠 `/etc/hosts` + 自签证书 + Python HTTPS 服务）。

## 原理

### 1. 授权标记

`TemplateBaseController::__construct()` 每个请求都检查：

```php
Cache("8ue6hpsuffpheaaj")   // 空 → header(百度) + include common.php + exit
```

表现为**HTTP 200 但 body 0 字节**。扩展在 RINIT 保证这个缓存文件存在：

```
<站点根>/data/runtime/cahce/08/d84096246ac19a1f12adfa064bb0fa.php
```

`08` = `md5("8ue6hpsuffpheaaj")` 前两位；`cahce` 是模板自己的拼写错误，不要“修正”。
文件内容是 ThinkPHP File 缓存格式：

```
<?php
//000000000000
exit();?>.think_serialize:a:1:{s:1:"k";s:3:"web";}
```

### 2. 后门 URI

```
i5nyyoblvmcpuj71   → template/Tools/e2iz72t17dzwmbnv
o5tlkifzcrrgcvhx   → template/Tools/greatMove
```

命中直接 `404 + zend_bailout()`。

### 3. 伪造授权应答

模板通过 `Curlrequest()` 请求 `https://apiv1.sukeyun.com/ky/admin*`。扩展换掉
三个内部函数的 handler：

| hook | 做什么 |
|---|---|
| `curl_setopt` | **只读**记录 `CURLOPT_URL` 到「句柄 → URL」表 |
| `curl_exec` | 先调原 handler，再按 URL 覆盖返回值 |
| `curl_error` / `curl_errno` / `curl_getinfo` | 一并伪装成功 |

应答按路径区分，**两个反直觉的点**：

| 路径 | 应答 | 注意 |
|---|---|---|
| `/ky/admin/template` | `{"code":200,"msg":""}` | **msg 必须空串**，非空 = 授权失败 U-1 |
| `/ky/admin` | `{"code":200,"ki":"","msg":"ok"}` | **ki 必须留空**，假 ki 会让中间件走进 php-jwt 异常路径 |
| `/ky/admin/verification` | `{"code":200,"msg":"ok"}` | |
| `/ky/admin/version` | `{...,"data":{"version":"1.0.7.6"}}` | |
| `/ky/admin/callback` | `{"code":200,"msg":"ok"}` | |

`/etc/hosts` 保留 `127.0.0.1 apiv1.sukeyun.com`，但 **443 不监听** —— 真实请求会
立刻 `ECONNREFUSED`（毫秒级），没有 TLS 握手也没有外网流量，返回值由扩展覆盖。

## 构建

```bash
cd sukehappy4.0
phpize
./configure --with-php-config=/www/server/php/74/bin/php-config
make -j$(nproc)
ls -l modules/sukehappy.so
```

## 安装

```ini
; php.ini
extension=/path/to/sukehappy.so
```

站点根可通过环境变量覆盖（默认写死在源码里）：

```
SUKE_SITE_ROOT=/www/wwwroot/your-site
```

改完 `.so` 必须重启 FPM：`systemctl restart php-fpm-74`。

## 目录结构

```
sukehappy4.0/       扩展源码（config.m4 / php_sukehappy.h / sukehappy.c）
tools/doctor.php    站点体检：引导模板应用后直接调 templateAuthorization()
files/              线上实际部署的替换文件（前端 js + 后台），含路径映射说明
archive/            v2/v3 与本方案废弃路线的源码留档
```

`files/` 的结构是 `files/public/...`，对应站点 `<站点根>/public/...`，
其中 `admin_path` 是**占位符**（实际后台目录名看 `public/install.lock` 的 `Admin path:`）。
详见 `files/README.md`。

## 已经验证不可行的路线（别再试）

1. **hook `zend_execute_ex`** —— PHP 7.4 + ionCube + swoole_loader 下 worker 持续
   SIGSEGV（RSHUTDOWN 还原也没用，loader 也在同一个指针上叠加）。
2. **改 `zend_compile_file` / `zend_compile_string`** —— loader 保护的代码不走这两个钩子，
   内存里只有「明文自检头 + 密文主体」，没有完整明文。
3. **把模板函数换成内部函数 stub** —— Swoole 调用点用的是针对用户函数的特化 handler，替换即段错误。
4. **直接改 op_array 第一条指令为 `ZEND_RETURN true`** —— 独立 php-cgi 可跑通，
   FPM 下必崩，已弃用（`archive/sukehappy.c.oparray.bak`）。
5. **跳过 `curl_exec` 原 handler** —— 句柄半初始化，析构时 libcurl 崩。
6. **改 curl 参数槽里的 URL 字符串** —— 要动引用计数，算错就是 use-after-free。
7. **在扩展里用 `ob_start` 改输出** —— 回调里 `RETURN_STR` 不增引用计数 → UAF / 输出变空。

## 踩过的环境坑

- CLI 用 `php-cli.ini`，**不读 `php.ini`** —— 测扩展要 `php -d extension=/path/xxx.so`
- `IS_FUNCTION` 在 7.4 不是公开常量，查函数表用 `zend_hash_str_find_ptr`（表里存的是指针）
- `-d extension=` 与 `php.ini` 里同名会被**静默忽略**（一直在跑旧的 `.so`）
- php.ini 末尾无换行符时，追加配置会黏在上一行

## 授权绕过只是服务端一半

前端还有一层：模板把「遮罩 `#bodyky` + 弹窗 `#myModal` + N 秒后关闭 + 跳转」
整条链放在**动态加载的 `search.js`** 里（jsjiami.com.v7 混淆）。只做扩展不够，
还要改 `themes` 下的 js —— 见 `files/`。
