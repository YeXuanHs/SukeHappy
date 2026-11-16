# mdyguard

一个 PHP 扩展，用来在运行时**拦截已知的后门入口**，适用于无法修改源码的场景
（例如源码经 Swoole Compiler 加密分发的第三方模板）。

## 它拦什么

| 层次 | 拦什么 | 默认值 | 命中后行为 |
|---|---|---|---|
| URL | 请求 URI 子串 | `i5nyyoblvmcpuj71`、`o5tlkifzcrrgcvhx` | 404 + 终止 |
| 函数 | 执行到这些函数 | `greatMove`、`e2iz72t17dzwmbnv`、`admin_decrypt` | 404 + 终止 |
| SQL | PDO 语句含毁灭性/外带特征 | `DROP TABLE`、`TRUNCATE`、`ALTER TABLE`、`INTO OUTFILE`、`LOAD_FILE(` 等 | 404 + 终止 |

三层是独立的，任何一层命中都会终止请求。

两者都可通过 php.ini 覆盖，不写死：

```ini
mdyguard.block_uris      = "i5nyyoblvmcpuj71,o5tlkifzcrrgcvhx"
mdyguard.block_functions = "greatMove,e2iz72t17dzwmbnv,admin_decrypt"
mdyguard.block_sql       = 1          ; SQL 兜底层开关，1=开 0=关
mdyguard.logfile         = "/var/log/mdyguard.log"
```

## 安装

从 [Actions](../../actions) 里下载对应 PHP 版本的 `.so`（注意 **NTS / ZTS** 要跟你的 PHP 一致），然后：

```ini
extension=/path/to/mdyguard-7.4-nts-linux-x86_64.so
mdyguard.logfile=/var/log/mdyguard.log
```

重载 php-fpm 后确认：

```bash
php -m | grep mdyguard
```

## 本地编译

```bash
phpize
./configure --with-php-config=$(which php-config)
make
sudo make install
```

## 说明

- 只做**拦截**，不修改任何业务逻辑；命中即 404 + `zend_bailout()`，不输出任何内容。
- 拦的是「函数名」，不是「类方法」，所以同名函数会被一起拦下——本仓库面向的后门函数名足够独特。
- 第 3 层只拦**毁灭性/外带**语句（DROP/TRUNCATE/ALTER/OUTFILE/LOAD_FILE/GRANT），
  **不拦 INSERT/UPDATE/DELETE** —— 否则会把正常业务打挂。所以它挡不住"静默篡改数据"，
  只挡"灭库/写文件/提权"。要防篡改，得靠第 1、2 层在入口就把请求掐死。
- 后台的「卸载模板」本身会 DROP TABLE，开了第 3 层会被拦。真需要卸载时临时
  `mdyguard.block_sql=0` 或在 CLI 里跑（CLI 不设 HTTP 状态，但同样会 bailout）。
- 这是**缓解手段**，不是根治。根治要打官方补丁或替换掉含后门的加密文件。
- 仅限用于你自己拥有/运维的站点。


## 已验证

在 PHP 7.4.33 (NTS) 上实测：

| 用例 | 结果 |
|---|---|
| 加载扩展 | `mdyguard` 出现在 `get_loaded_extensions()` |
| 普通代码 | 正常执行 |
| 调用 `greatMove()` | 无任何输出，请求终止 |
| 调用 `e2iz72t17dzwmbnv()` | 无任何输出，请求终止 |
| 调用 `admin_decrypt()` | 无任何输出，请求终止 |
| 不加载扩展（对照） | 正常执行，说明拦截确实来自本扩展 |
| 自定义 `mdyguard.block_functions=myDanger` | 生效 |
| `PDO::exec("CREATE TABLE ...")` | 通过 |
| `PDO::exec("INSERT/SELECT ...")` | 通过 |
| `PDO::exec("DROP TABLE ...")` | 被拦，无输出 |
| `PDO::exec("... INTO OUTFILE ...")` | 被拦，无输出 |
| `mdyguard.block_sql=0` 时再执行 DROP | 放行（开关有效） |

CI 每次构建都会跑同类冒烟测试（见 `.github/workflows/build.yml`），
拦截失效会导致构建失败。

## License

MIT
