# zend_extension 在 PHP 7.3 上：全部打通，但钩子拿不到模板函数

## 一、怎么让 PHP 7.3 接受一个 zend_extension（完整解法）

这五个坑缺一个都加载不了。**前三个是我靠实测撞出来的，后两个是查
PHP 7.3 源码（`Zend/zend_extensions.c`）确认的。**

| # | 坑 | 报错 | 解法 |
|---|---|---|---|
| 1 | 缺 `extension_version_info` 符号 | `xxx.so doesn't appear to be a valid Zend extension` | 必须导出（见坑3，它是结构体） |
| 2 | `zend_extension` 字段不对齐 | 同上 | 26 字段 / **208 字节**，严格按 `zend_extensions.h` |
| 3 | **`extension_version_info` 写成 `char*`** | `requires Zend Engine API version -135446428`（垃圾值） | **它是结构体**：`{ int zend_extension_api_no; char *build_id; }` |
| 4 | `api_no_check` 传 NULL | 同上 | 自己实现回调 `return SUCCESS` |
| 5 | `resource_number` 传 0 | 崩溃/静默退出 | **必须是 `-1`**（官方宏 `STANDARD_ZEND_EXTENSION_PROPERTIES` 里就是 -1） |

### 源码依据（PHP 7.3.33 `Zend/zend_extensions.c`）

```c
int zend_load_extension_handle(DL_HANDLE handle, const char *path)
{
	zend_extension *new_extension;
	zend_extension_version_info *extension_version_info;      /* ← 结构体指针！ */

	extension_version_info = DL_FETCH_SYMBOL(handle, "extension_version_info");
	new_extension = DL_FETCH_SYMBOL(handle, "zend_extension_entry");
	if (!extension_version_info || !new_extension) {
		fprintf(stderr, "%s doesn't appear to be a valid Zend extension\n", path);
		...
	}

	if (extension_version_info->zend_extension_api_no != ZEND_EXTENSION_API_NO
	    && (!new_extension->api_no_check || new_extension->api_no_check(...) != SUCCESS)) {
		...
	} else if (strcmp(ZEND_EXTENSION_BUILD_ID, extension_version_info->build_id)   /* ← 崩在这 */
	           && (!new_extension->build_id_check || ...)) {
		...
	} else if (zend_get_extension(new_extension->name)) { ... }

	return zend_register_extension(new_extension, handle);
}
```

对应 `zend_extensions.h`：

```c
typedef struct _zend_extension_version_info {
	int zend_extension_api_no;
	char *build_id;
} zend_extension_version_info;

#define ZEND_EXTENSION_BUILD_ID "API" ZEND_TOSTR(ZEND_EXTENSION_API_NO) ZEND_BUILD_TS ...
```

### 正确写法

```c
ZEND_DLEXPORT zend_extension_version_info extension_version_info = {
    ZEND_EXTENSION_API_NO,
    ZEND_EXTENSION_BUILD_ID
};

ZEND_DLEXPORT zend_extension zend_extension_entry = {
    "name", "1.0", "author", "-", "-",      /* 5 个字符串 */
    startup, shutdown, activate, deactivate, /* 4 个 */
    message_handler,
    op_array_handler,
    statement_handler, fcall_begin_handler, fcall_end_handler,
    op_array_ctor, op_array_dtor,
    my_api_no_check,        /* 不能 NULL */
    my_build_id_check,      /* 不能 NULL */
    NULL, NULL,             /* persist_calc, persist */
    NULL, NULL, NULL, NULL, /* reserved5-8 */
    0,                      /* handle */
    -1                      /* resource_number ← -1，不是 0 */
};
```

**验证通过**：`php -v` 正常、`STARTUP CALLED`、脚本正常执行。

## 二、但钩子全都拿不到模板函数（实测数据）

完整栈（ioncube + swoole_loader + 探针）跑 `index.php`：

| 钩子 | 触发次数 | 能看到 templateauthorization 吗 |
|---|---|---|
| `op_array_handler` | **1644** | ❌ 只有框架函数（getRootPath/autoload/findFile...） |
| `statement_handler` | **0** | ❌ |
| `fcall_begin_handler` | **0** | ❌ |

**原因**：
- `op_array_handler` 在 `pass_two` 里调用 → **loader 用自己的编译器，模板文件不走 pass_two**
- `statement_handler` / `fcall_begin_handler` 由 VM 在执行时检查全局 `zend_execute_ex` 路径触发 → **loader 不走它，所以是 0**

## 三、PHP 7.3 三条路全堵死（都有实测证据）

| 方案 | 结果 | 证据 |
|---|---|---|
| 普通扩展 hook `zend_execute_ex` | ❌ 无限递归 | gdb 栈里 `execute_ex` 叠 5 层；7.3 的 `execute_ex` 内部回调指针 |
| `zend_extension` 的 `op_array_handler` | ❌ 不含模板函数 | 1644 次触发，0 次命中 |
| `zend_extension` 的 `statement` / `fcall` handler | ❌ 0 次触发 | loader 不走 execute_ex 路径 |

## 四、结论

**PHP 7.3 + Swoole Loader 下，没有任何 PHP 层面的钩子能触达模板函数的 op_array。**
（不是找不到钩子，是所有钩子都实测过了，这是封闭运行时的必然结果。）

**PHP 7.4 不同**：`execute_ex` 内部**不**回调指针，普通扩展 hook 就能用，
op_array 改写实测通过（`templateAuthorization()` 返回 `true`，连跑 5 次）。
