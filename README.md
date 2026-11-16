# SukeHappy · 速科模板综合扩展

在运行时**拦截速科模板里已知的后门入口**，适用于无法修改源码的场景
（源码经 Swoole Compiler 加密分发）。直装、零配置、不碰任何正常业务功能。

## 它拦什么

| 层次 | 拦什么 | 默认 | 影响正常功能吗 |
|---|---|---|---|
| URL | 请求 URI 含 `i5nyyoblvmcpuj71` / `o5tlkifzcrrgcvhx` | **开** | 否（这两个路径正常业务零引用） |
| 函数 | 执行到 `greatMove` / `e2iz72t17dzwmbnv` / `admin_decrypt` | **开** | 否（前两个是后门路由专用；`admin_decrypt` 全包只有后门调用） |
| SQL | PDO 语句含 `DROP TABLE` / `TRUNCATE` / `ALTER TABLE` / `INTO OUTFILE` / `LOAD_FILE(` 等 | **关** | **会**（后台「卸载模板」「导入主题」要执行 DDL），需要时再开 |

**默认只有前两层生效，对正常功能零影响。** 第 3 层是可选加固，默认关闭。

三项都可通过 php.ini 覆盖，不写死：

```ini
sukehappy.block_uris      = "i5nyyoblvmcpuj71,o5tlkifzcrrgcvhx"
sukehappy.block_functions = "greatMove,e2iz72t17dzwmbnv,admin_decrypt"
sukehappy.block_sql       = 0          ; SQL 兜底层，默认关；开=1 会拦 DDL，影响卸载/导入
sukehappy.logfile         = "/var/log/sukehappy.log"
```

## 安装

从 [Releases](../../releases) 里下载对应 PHP 版本的 `.so`（现提供 **7.1 / 7.2 / 7.3 / 7.4**，NTS），然后：

```ini
extension=/path/to/SukeHappy-7.4-nts-linux-x86_64.so
sukehappy.logfile=/var/log/sukehappy.log
```

重载 php-fpm 后确认：

```bash
php -m | grep SukeHappy
```

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
| 授权缓存 | 读 `Cache::get('hzdtqmwbodere7d9')`、`Cache::get('8ue6hpsuffpheaaj')`，即 app 自己认的授权状态 |
| 在线更新 | 调 `getNewVersion()`，返回版本信息=可用，返回 `error`=不可用 |
| 后门入口 | 给了 `--url` 才查，HTTP 403/404/444 视为已封 |

输出示例：

```
  [ OK ] 扩展已加载        sukehappy
  [ OK ] 授权状态           已授权（templateAuthorization 正常返回）
  [ OK ] 授权缓存 ki        有（348 字节，5f2a9c...）
  [ OK ] 授权标记 web       "web" 已写入
  [ OK ] 在线更新           可用，最新版本 1.0.7.8
  [ OK ] 后门入口 /i5nyyoblvmcpuj71   HTTP 404
```

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

- 只做**拦截**，不修改任何业务逻辑；命中即 404 + `zend_bailout()`，不输出任何内容。
- 拦的是「函数名」，不是「类方法」，所以同名函数会被一起拦下——本仓库面向的后门函数名足够独特。
- **第 3 层默认关闭**，因为后台的「卸载模板」「导入主题」本身要执行 DDL，开了会误伤。
  需要更强加固时再显式 `sukehappy.block_sql=1`，用完记得关回去。
- 第 3 层即使开了也只拦**毁灭性/外带**语句，**不拦 INSERT/UPDATE/DELETE**（会打挂业务），
  所以它挡不住"静默篡改数据"，只挡"灭库/写文件/提权"。
- 这是**缓解手段**，不是根治。根治要打官方补丁或替换掉含后门的加密文件。
- 仅限用于你自己拥有/运维的站点。


## License

MIT
