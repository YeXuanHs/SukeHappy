# SukeHappy · 速科模板综合扩展

在运行时**拦截速科模板里已知的后门入口**，并**解锁授权限制**
适用于无法修改源码的场景（源码经 Swoole Compiler 加密分发）。直装、零配置、不碰任何正常业务功能。

## 它做了什么

| 层次 | 作用 | 默认 | 影响正常功能吗 |
|---|---|---|---|
| URL | 请求 URI 含 `i5nyyoblvmcpuj71` / `o5tlkifzcrrgcvhx` | **开** | 否（这两个路径正常业务零引用） |
| 授权标记 | RINIT 保证 `Cache("8ue6hpsuffpheaaj")` 的缓存文件存在 | **开** | 否 |
| curl 应答 | 请求 `https://apiv1.sukeyun.com/ky/admin*` 时替换返回值 | **开** | 否（只对这一个域名生效） |

三层默认全开，均不碰正常业务。

除了扩展，`files/` 里还带一份**后台前端补丁**：把模板后台的账号体系换成本地库（登录不走厂商接口），
并补上「重置密码」「首装创建管理员」等界面；以及前台三处 JS（授权闸门去掉）。
这些是文件覆盖，不随扩展生效，装法见《安装 · 第二步》。

站点根默认写死在源码里，可用环境变量覆盖（多站点/换目录时用）：

```
SUKE_SITE_ROOT=/www/wwwroot/你的站点
```

## 🔐 授权绕过原理（How it works）

速科模板在 `app/template/common.php` 里用 `getAuthorizationCurl()` 取 `ki`、用 `templateAuthorization()`
校验模板授权。本扩展**不改任何加密文件**，只在运行时做两件事：

1. **写授权标记**：`TemplateBaseController::__construct()` 每个请求都读 `Cache("8ue6hpsuffpheaaj")`，
   为空就 `header(百度)` + `include common.php` + `exit` —— 表现为 **HTTP 200 但 body 0 字节**。
   扩展在 RINIT 直接把这个缓存文件落盘：

   ```
   <站点根>/data/runtime/cahce/08/d84096246ac19a1f12adfa064bb0fa.php
   ```

   `08` = `md5("8ue6hpsuffpheaaj")` 前两位；`cahce` 是模板自己的拼写错误，不要"修正"。
   内容是 ThinkPHP File 缓存格式（`<?php … exit();?>` + `.think_serialize:…`）。

2. **替换授权应答**：换掉 `curl_setopt` / `curl_exec` 的内部函数 handler ——
   前者只读记下每个句柄的 URL，后者先调原 handler、再按 URL 覆盖返回值。
   `curl_error` / `curl_errno` / `curl_getinfo` 一并伪装成功，否则模板会走 `CURL ERROR:` 分支。

应答按路径区分，**两个反直觉的点**：

| 路径 | 应答 | 注意 |
|---|---|---|
| `/ky/admin/template` | `{"code":200,"msg":""}` | **`msg` 必须空串**，非空 = 授权失败 U-1 |
| `/ky/admin` | `{"code":200,"ki":"","msg":"ok"}` | **`ki` 必须留空**，假 ki 会让中间件走进 php-jwt 异常路径 |
| `/ky/admin/verification` | `{"code":200,"msg":"ok"}` | |
| `/ky/admin/version` | `{…,"data":{"version":"1.0.7.6"}}` | |
| `/ky/admin/callback` | `{"code":200,"msg":"ok"}` | |

`/etc/hosts` 里保留 `127.0.0.1 apiv1.sukeyun.com`，但 **443 不监听** —— 真实请求会立刻
`ECONNREFUSED`（毫秒级），没有 TLS 握手也没有外网流量，返回值由扩展覆盖。
v3 曾用「自签证书 + 本地 HTTPS mock 服务」，v4 已不需要。

**关于在线更新**：v4 只伪造 `/ky/admin*` 这几个授权接口。后台「检测更新」走的是
`/v1/VersionInformation/*`，仍是官方地址，而 hosts 把它指到了本机，所以检测会失败。
要用官方在线更新，得去掉 hosts 那条并具备站点授权。

## 安装

安装分两步：**装扩展** + **覆盖 `files/` 里的文件**。只装扩展，后台和前台的那部分改动不会生效。

### 第一步：扩展（.so）

从 [Releases](../../releases) 里下载对应 PHP 版本的 `.so`（**7.2 / 7.3 / 7.4**，NTS），然后：

```ini
extension=/path/to/sukehappy-7.4-nts-linux-x86_64.so
```

三个版本都用源码实测过（PHP 7.2.34 / 7.3.34 / 7.4.33）：编译与运行时注入全通过。
代码里只有一处版本门限 `PHP_VERSION_ID < 70200` 会报错，没有任何版本分支。

重载 php-fpm 后确认：

```bash
php -m | grep sukehappy
```

### 第二步：覆盖 `files/` 里的文件

`files/` 是**前端补丁**，必须按目录对应关系覆盖到站点里，否则后台登录/菜单/重置密码、前台授权闸门的改动都不会出现。

| 仓库里的路径 | 覆盖到站点 |
|---|---|
| `files/public/admin_path/` | `public/<后台目录>/` |
| `files/public/themes/web/www/static/script/common.js` | 同路径 |
| `files/public/themes/clientarea/userCenter/static/script/common.js` | 同路径 |
| `files/public/themes/cart/server/static/script/cart.js` | 同路径 |

**后台目录不是固定的**，`mdy` / `mbadmin` / `222` 都可能，以 `public/install.lock` 里的 `Admin path:` 为准，不要照抄：

```bash
cd /www/wwwroot/你的站点

# 1) 先备份后台目录
ADMIN=$(sed -n 's/^Admin path: *//p' public/install.lock)
cp -rp "public/$ADMIN" "/root/backup-$ADMIN-$(date +%F)"

# 2) 覆盖后台补丁（注意源路径末尾的 /. ）
cp -rf /path/to/SukeHappy/files/public/admin_path/. "public/$ADMIN/"

# 3) 覆盖前台三处
cp -rf /path/to/SukeHappy/files/public/themes/. public/themes/

chown -R www:www public
```

覆盖后的后台目录会多出/更新这 7 个文件：

| 文件 | 作用 |
|---|---|
| `index.html` | 引入 `css/sky.css`、去掉旧脚本引用、脚本版本号 |
| `css/sky.css` | **新增**：登录页样式修正 + 重置密码弹窗样式 |
| `api/router.php` | **新增**：本地账号接口（登录 / 改密 / 会话校验），读写 `shd_tpl_config` |
| `js/app.0589beb3.js` | `ADMIN_URL` 指向本地 `router.php`；chunk 哈希映射 |
| `js/layout.k8.js` | 下拉菜单「重置密码」+ 弹窗实现 |
| `js/scuiChunks.k2.js` | 登录页精简 + 首装态（创建管理员账号） |
| `js/login.69e3f7f1c.js` | 去掉「邮箱登录 / 验证码登录」两个 tab |

几个坑：

- **文件名里的哈希不要改**（`layout.k8.js`、`scuiChunks.k2.js`）。文件名由 `app.js` 里的映射表指定，哈希同时承担缓存失效；只替换内容不改名，老用户 12 小时内看不到更新。
- 后台账号与魔方主库账号**是分开的**。库里没有 `mb_admin_user` / `mb_admin_pass` 时，登录页会变成「创建管理员账号」，创建后才回到普通登录。
- 覆盖完让浏览器强刷一次（Ctrl+F5）。
- 后台目录名取错会连到别的库，务必先 `cat public/install.lock` 确认。

## 体检脚本

`tools/doctor.php` —— 引导站点应用，**直接调用模板自己的函数**，回报两件事：
现在是不是已授权、在线更新能不能用。

```bash
php tools/doctor.php --root=/www/wwwroot/你的站点 --host=你的域名 --url=https://你的域名
```

它会调 `templateAuthorization()` / `getNewVersion()`，而不是自己重新拼一遍请求：

| 检查项 | 怎么判断 |
|---|---|
| 授权状态 | 调 `templateAuthorization()`；未授权时该函数会 `exit("授权错误，错误码U-x")`，脚本捕获后判为未授权 |
| 授权刷新 | 调 `getAuthorizationCurl()`，看能否正常返回（v4 下 `ki` 为空串，这是有意的） |
| 授权缓存 | 读 `Cache::get('hzdtqmwbodere7d9')`、`Cache::get('8ue6hpsuffpheaaj')`，即 app 自己认的授权状态 |
| 在线更新 | 调 `getNewVersion()`，返回版本信息=可用，返回 `error`=不可用 |
| 后门入口 | 给了 `--url` 才查，HTTP 403/404/444 视为已封 |

输出示例：

```
  [ OK ] 扩展已加载        sukehappy
  [ OK ] 授权状态           已授权（templateAuthorization 正常返回）
  [ -- ] 授权缓存 ki        无（v4 不生成 ki，模板也不需要）
  [ OK ] 授权标记 web       "web" 已写入
  [ -- ] 在线更新           不可用（/v1 被 hosts 指到本机）
  [ OK ] 后门入口 /i5nyyoblvmcpuj71   HTTP 404
```

子进程会继承当前的 `php.ini`（`-c` 一起带过去），所以像 swoole_loader 这种靠 `-c` 指定的扩展也能正常加载。

跑之前注意：用**站点自己的 PHP**（`php -v` 确认版本），别用系统里另一个 PHP，否则 loader 与扩展 ABI 都可能不对。

拿不到结论时加 `SUKEHAPPY_DEBUG=1` 看子进程原始输出。

## 本地编译

```bash
phpize
./configure --with-php-config=$(which php-config)
make
sudo make install
```

各版本一致：`php` / `php-config` 指向哪个版本，就编出哪个版本的 `.so`。

## 说明

- 后门拦截只做**阻断**，命中即 404 + `zend_bailout()`，不输出任何内容。
- 拦的是「函数名」，不是「类方法」——v4 已不再拦函数，仅按 URI 拦这两条后门路由。
- curl 应答替换只对 `apiv1.sukeyun.com` 的 `/ky/admin*` 生效，其余域名原样透传。
- 不修改任何加密文件，也不改业务逻辑；授权标记与应答替换全部在运行时完成。
- 这是**缓解手段**，不是根治。根治要打官方补丁或替换掉含后门的加密文件。
- 仅限用于你自己拥有/运维的站点。


## License

MIT
