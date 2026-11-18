# SukeHappy · 速科模板综合扩展

在运行时**拦截速科模板里已知的后门入口**，并**解锁授权限制**
适用于无法修改源码的场景（源码经 Swoole Compiler 加密分发）。直装、零配置、不碰任何正常业务功能。

## 它做了什么

| 层次 | 作用 | 默认 | 影响正常功能吗 |
|---|---|---|---|
| URL | 请求 URI 含 `i5nyyoblvmcpuj71` / `o5tlkifzcrrgcvhx` | **开** | 否（这两个路径正常业务零引用） |
| 授权标记 | RINIT 保证 `Cache("8ue6hpsuffpheaaj")` 的缓存文件存在 | **开** | 否 |
| 授权密钥 | RINIT 写入 `Cache("hzdtqmwbodere7d9")`（`/v33` 用它当 JWT 密钥） | **开** | 否 |
| curl 应答 | 请求 `https://apiv1.sukeyun.com/ky/admin*` 时替换返回值 | **开** | 否（只对这一个域名生效） |

三层默认全开，均不碰正常业务。

除了扩展，`files/` 里还带一份**后台前端补丁**：把模板后台的账号体系换成本地库（登录不走厂商接口），
并补上「重置密码」「首装创建管理员」等界面；以及前台三处 JS（授权闸门去掉）。
这些是文件覆盖，不随扩展生效，装法见《安装 · 第二步》。

站点根**自动探测**：RINIT 时从 `SG(request_info).path_translated` 反推
（`/site/public/index.php → /site`，再用 `data/runtime` 是否存在校验），
所以拷到任意路径都能直接用。多站点或有特殊情况时用环境变量强制指定：

```ini
; php.ini
env[SUKE_SITE_ROOT] = /www/wwwroot/你的站点
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
| `/ky/admin` | `{"code":200,"ki":"<固定密钥>","msg":"ok"}` | **`ki` 必须与 `api/jwt.php` 的 `SUKE_JWT_SECRET` 一致**（`/v33` 用它验 JWT） |
| `/ky/admin/verification` | `{"code":200,"msg":"ok"}` | |
| `/ky/admin/version` | `{…,"data":{"version":"1.0.7.6"}}` | |
| `/ky/admin/callback` | `{"code":200,"msg":"ok"}` | |

### 第二层：`/v33` 业务接口（后台页面有没有数据）

后台所有列表、统计都走 `/v33/...`。这一层比授权标记更严，**授权标记正常、页面能开，数据照样可以是空的**。

闸门在 `app/template/vendor/composer/AutoloadTask.php::handle()`，它作为中间件跑在控制器之前：

| 条件 | 结果 |
|---|---|
| 缺 `X-User-Token` 请求头 | `terminate("非法请求，-0")` |
| 有 token，但 `Cache("hzdtqmwbodere7d9")` 为空 | `terminate("请求失败，-2")` |
| 缓存有值 + token 能用它验签 | 放行到控制器 |

放行需要同时满足两件事：

1. **授权密钥缓存存在** —— `Cache("hzdtqmwbodere7d9")` 的值就是 `/ky/admin` 返回的 `ki`，
   模板拿它当 **HS256 的 HMAC 密钥**去验请求里的 JWT。
2. **请求头是能用这个 `ki` 验过的 JWT** —— 前端 axios 拦截器会带两个头：
   `X-Api-Token`（登录 token）、`X-User-Token`（`userInfo.user_token`），**网关解的是后者**。

三个坑（都踩过）：

- **`ki` 不能留空**。空 `ki` → 缓存写不进去 → 任何 token 都是 `-2`。
  早期版本为了躲 php-jwt 异常把 `ki` 留空，代价就是 `/v33` 永远不通。
- **缓存文件名是 `md5` 拆开的**：`cahce/<前两位>/<后30位>.php`。
  `md5("hzdtqmwbodere7d9") = 311ccd705bdb0aa5ecc79f8d96274d43`，
  正确路径是 `cahce/31/1ccd705bdb0aa5ecc79f8d96274d43.php` —— 写成 `31/311ccd…` 会 `exists=0`。
- **数据必须从文件第 32 字节开始**：`<?php\n//` + 12 位过期时间 + `\n exit();?>\n`（`exit` 前有个空格）。
  少一个空格，`substr($content,32)` 就会多带一个换行，密钥变成 `"\nk7Qm…"`，签名一样过不去。

登录侧配套改动在 `files/public/admin_path/api/`：

| 文件 | 作用 |
|---|---|
| `jwt.php` | HS256 签名/验签，`SUKE_JWT_SECRET` 必须等于 `.so` 里的 `SUKE_KI` |
| `login.php` | 账号校验 + 首装建管理员 + 签发 JWT；`userInfo.user_token` 也返回 JWT |
| `router.php` | 只做分发，`login` 分支 `require` 上面的 `login.php` |

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

覆盖后的后台目录会多出/更新这些文件：

| 文件 | 作用 |
|---|---|
| `index.html` | 引入 `css/sky.css`、去掉旧脚本引用、脚本版本号 |
| `css/sky.css` | **新增**：登录页样式修正 + 重置密码弹窗样式 |
| `api/router.php` | **新增**：本地账号接口（会话校验 / 改密），`login` 分支转给 `login.php` |
| `api/login.php` | **新增**：登录 + 首装建管理员 + 签发 HS256 JWT |
| `api/jwt.php` | **新增**：JWT 签名/验签，密钥与 `.so` 的 `ki` 一致 |
| `api/menu.json` | **新增**：后台菜单树（68 个页面，前端启动时读取，不写 localStorage） |
| `js/app.0589beb3.js` | `ADMIN_URL` 指向本地接口；chunk 映射；菜单改为读 `api/menu.json` |
| `js/layout.ke.js` | 下拉菜单「重置密码」+ 弹窗实现（Chunk `7225`） |
| `js/scuiChunks.k2.js` | 登录页精简 + 首装态；菜单不落 localStorage |
| `js/login.69e3f7f1c.js` | 去掉「邮箱登录 / 验证码登录」两个 tab |

> 后台里那三个按钮（机器人 / 公告 / 版本发布）连同它们的抽屉已经从 `layout.ke.js` 的 render 里删掉了，
> 对应方法和 data 也一并清理，不是隐藏。

几个坑：

- **文件名里的哈希不要改**（`layout.ke.js`、`scuiChunks.k2.js`）。文件名由 `app.js` 里的映射表指定，哈希同时承担缓存失效；只替换内容不改名，老用户 12 小时内看不到更新。
- 覆盖完让浏览器强刷一次（Ctrl+F5）。
- 后台目录名取错会连到别的库，务必先 `cat public/install.lock` 确认。

## 精简菜单 / 删除多余页面

菜单是 `api/menu.json`，路由由它动态注册 —— **从里面删掉一项，页面和路由就都没了**。
当前默认菜单已经去掉：用户空间、客服、反馈、盗版举报（一级 14 项、叶子 61 个）。

对应页面的 chunk 文件还能用脚本清掉（不做也不影响，只是占空间）：

```bash
./tools/remove-pages.sh /www/wwwroot/你的站点/public/<后台目录>/js /root/bak-chunks
```

删掉的 10 个 chunk：

```
userCenter-user-space
Chat-Chatset / Chat-DataAnalysis / Chat-Message
Chat-sava-DataAnalysis / Chat-sava-Message
feedback-allfeedback / feedback-allfeedback-fullpage / feedback-submitfeedback
authorization-piracyReport
```

头像下拉也同步精简过：**「帐号信息」已移除**（它跳的 `/usercenter` 在精简后的菜单里没有路由），
现在只剩「更新菜单 / 清除缓存 / 重置密码 / 退出登录」。

要恢复个人中心的话：把 `userCenter/index` 加回 `api/menu.json`，并把 `userCenter-user-space` 这个 chunk 从备份拿回来
（`userCenter.js` 里 `space` 是懒加载，缺了不影响主页面，只是那个 tab 打不开）。

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
| 授权刷新 | 调 `getAuthorizationCurl()`，看能否正常返回（`ki` 为空串，这是有意的） |
| 授权缓存 | 读 `Cache::get('hzdtqmwbodere7d9')`（JWT 密钥）、`Cache::get('8ue6hpsuffpheaaj')`（授权标记） |
| 在线更新 | 调 `getNewVersion()`，返回版本信息=可用，返回 `error`=不可用 |
| 后门入口 | 给了 `--url` 才查，HTTP 403/404/444 视为已封 |

输出示例：

```
  [ OK ] 扩展已加载        sukehappy
  [ OK ] 授权状态           已授权（templateAuthorization 正常返回）
  [ OK ] 授权缓存 ki        有（32 字节，/v33 用它验 JWT）
  [ OK ] 授权标记 web       "web" 已写入
  [ -- ] 在线更新           不可用（未接官方授权）
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

- 干两件事：**解掉模板授权** + **封掉后门 URI**。
- 授权靠运行时完成：写授权标记文件、替换 curl 应答、伪装 `curl_error` / `curl_getinfo`。
- 后门按 URI 匹配，命中即 `404` + `zend_bailout()`，不输出任何内容。
- 应答替换只针对 `apiv1.sukeyun.com` 的 `/ky/admin*`，其他域名原样透传。
- 不改任何加密文件，也不改业务逻辑。
- 仅限用于你自己拥有/运维的站点。


## 更新日志

### 未发布

- 首页欢迎卡补齐三项空缺：`上次登陆 / 登录IP / 登录地点`
  - 三个字段的含义（按模板前端的顺序）：
    `上次登陆` = **上次登录的 IP**、`登录IP` = **本次登录的 IP**、`登录地点` = 本次 IP 的归属地
  - 数据源是魔方自己的登录日志表 **`<prefix>admin_log`**（`admin_username / logintime / ipaddress / port / sessionid`），
    取该账号**最近一条**记录的 `ipaddress` 作为「上次登陆」；每次登录再插入一条，所以后台的登录日志也是真的
  - 归属地复用模板自带 `ip2region`（`app/template/vendor/zoujingli/ip2region`），
    与「监控 → 地区」同源，格式一致：`中国福建省莆田市[电信]`
- 头像改为首字母头像（内联 SVG data URI，蓝底白字），不再用 `/static/images/avatar.jpg`（那是魔方 logo）
- 菜单再精简：移除 用户空间 / 客服 / 反馈 / 盗版举报；头像下拉去掉「帐号信息」
- 仪表盘去掉「版本信息」「版本发布日志」两张卡，栅格调成 2×2 + 整行
- 修「产品内容」(`/product/type/content`) 一打开就卡死：接口 `content` 返回字符串 `"[]"`，
  弹窗 `setData()` 直接 `this.form = e.content`，`form` 变成字符串，渲染时往字符上写属性 →
  严格模式每次渲染抛 TypeError → Vue 反复重渲染把页面卡死。
  现在在 `scuiChunks` 里统一 `JSON.parse` 成数组（非数组退化为 `[]`）
- 修「退出登录」点了没反应：原实现只 `router.replace(/login)` 没清 token，被路由守卫弹回
- 「安全码验证」保留，文案改为「密码」（HTML 编辑 / 全局文件两处）

### 1.2.0

- **修通 `/v33`**：`.so` 现在会写入授权密钥缓存 `cahce/31/1ccd705bdb0aa5ecc79f8d96274d43.php`，
  后台列表/统计不再是 `请求失败，-2`
- `/ky/admin` 的 `ki` 从空串改为固定密钥，与 `api/jwt.php` 对齐，登录签发 HS256 JWT
- 登录返回的 `userInfo.user_token` 也改为 JWT（网关验的是 `X-User-Token` 头）
- **站点根自动探测**，不再写死 `/www/wwwroot/…`，拷到任意路径可用
- 后台菜单改为 `api/menu.json` + 每次打开拉取，不写 `localStorage`
- `phpinfo()` 输出扩展版本号

### 1.1.0

- 后台账号体系本地化（登录 / 改密 / 首装建管理员）
- 前台三处 JS 授权闸门下线

## License

MIT

仪表盘上的「版本信息」「版本发布日志」两张卡也已移除（`home-widgets` chunk，hash 换成 `6c1a2f9e`），
顺手把 `登录/注册/访问/IP/在线人数` 的栅格从「半行 + 空位」调成「2×2 + 整行」，
否则删卡片后会留一块空白。

**注意**：`userCenter.*.js`
