# files/ —— 线上实际部署的替换文件

这里的文件是**已经改好、并且正在线上跑**的版本，可以直接覆盖回站点。

目录层级与站点根保持一致：`files/public/...` → `<站点根>/public/...`

## 站点路径映射

| 仓库路径 | 站点路径 |
|---|---|
| `files/public/themes/web/www/static/script/common.js` | `public/themes/web/www/static/script/common.js` |
| `files/public/themes/clientarea/userCenter/static/script/common.js` | `public/themes/clientarea/userCenter/static/script/common.js` |
| `files/public/themes/cart/server/static/script/cart.js` | `public/themes/cart/server/static/script/cart.js` |
| `files/public/admin_path/api-local.js` | `public/<后台目录>/api-local.js` |
| `files/public/admin_path/api/router.php` | `public/<后台目录>/api/router.php` |
| `files/public/admin_path/js/app.0589beb3.js` | `public/<后台目录>/js/app.0589beb3.js` |
| `files/public/admin_path/js/scuiChunks.9e0b4ed4.js` | `public/<后台目录>/js/scuiChunks.9e0b4ed4.js` |
| `files/public/admin_path/js/login.69e3f7f1c.js` | `public/<后台目录>/js/login.69e3f7f1c.js` |
| `files/public/admin_path/js/layout.3d1487a2.js` | `public/<后台目录>/js/layout.3d1487a2.js` |

> **`admin_path` 不是目录名**，是占位符：实际后台目录名以站点 `public/install.lock` 里的
> `Admin path:` 为准（当前为 `mbadmin`，但安装时可以改）。同步时把它换成真实目录名。

## 各文件改了什么

### 授权链（前端）

**`themes/web/www/static/script/common.js`**
- UA 白名单检测函数直接 `return true`
- 去掉动态加载 `search.js` 的入口（`search.js` 里是遮罩 `#bodyky` + 弹窗 `#myModal` + "N 秒后关闭" + 跳转）
- 去掉反调试（`setInterval(()=>{debugger;},50)`）
- 已反混淆成扁平代码

**`themes/clientarea/userCenter/static/script/common.js`**
- 去掉动态加载 `search.js` 的入口
- 去掉反调试

**`themes/cart/server/static/script/cart.js`**
- 反混淆成扁平代码后，整段删除 `_0x47ecaf()`（它加载 `search.js`，导致 `/cart` 跳 `/login`）
- 去掉反调试

### 后台（不再依赖官方账号体系）

后台原本把登录 / 菜单 / 版本 / 公告全部走向官方接口，站点未授权时一律拿不到。
这里改成**本地账号体系**，其余业务接口保持原样。

**`api/router.php`** —— 新增，本地接管 `/v1/*`
- `login` / `verifyLogin` / `verify` / `status` / `myMenus` / `changePassword`
- 账号存 `shd_tpl_config`（`mb_admin_user` / `mb_admin_pass` / `mb_admin_email`），密码 bcrypt
- 会话是 HMAC-SHA256 签名的 cookie
- `register` / `reset` / `forget` / `sms` 一律 403
- 未知接口返回 `200 + []`（返回 `null` 会让前端 `.length` 崩）

**`api-local.js`** —— 新增，浏览器侧适配
- 劫持 XHR/fetch，把 `apiv1.sukeyun.com` 的请求转到本地 `router.php`
- 首次安装态：改标题/按钮、加二次密码框
- 隐藏「忘记密码」「创建新账号」
- 用户下拉增加「重置密码」（ElMessageBox 同款弹窗）
- 右侧登录表单垂直居中

**`js/login.69e3f7f1c.js`**
- 从备份重建，只把 `a-tabs`（密码/扫码切换）换成单个 password-form
- 保留背景图、左侧插画、分隔标题、底部版权

**`js/scuiChunks.9e0b4ed4.js`**
- password-form 模块：删图形验证码、`emailType` 下拉、忘记密码、创建新账号、`type:"number"` 校验
- 登录载荷精简为 `{username,password}`
- 去掉「无任何菜单权限」的弹窗拦截

**`js/app.0589beb3.js`**
- 删除 `/reset_password` 残留路由
- 根路由改为条件重定向：有菜单 → `/dashboard`，无菜单 → `/blank`（新增的隐藏子路由）
- 已登录访问 `/login` 自动跳回
- chunk 映射表随 hash 更新

**`js/layout.3d1487a2.js`**
- `pmenu.meta.title` 空值兜底（修通栏布局报错）
- 删除侧栏「无子集菜单」空状态

## 同步注意事项

1. 覆盖后 `chown -R www:www` 对应用目录
2. `themes` 下的 js 用 `?<时间戳>` 引入，改完刷新即生效；后台 js 在 `index.html` 里带 `?v=N`，**改文件必须同时递增 `?v=`**
3. 后台 `data/runtime/temp/*` 是模板编译缓存，改模板后要清
