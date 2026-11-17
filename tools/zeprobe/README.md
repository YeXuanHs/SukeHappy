# zend_extension 探针：PHP 7.3 上怎么才能被接受

## 三道坎（缺一个都加载不了）

| # | 坎 | 现象 | 解法 |
|---|---|---|---|
| 1 | 缺 `extension_version_info` | `xxx.so doesn't appear to be a valid Zend extension` | 导出它：`ZEND_DLEXPORT char *extension_version_info = "1.0";` |
| 2 | 结构字段不对齐 | 同上（PHP 读不到 entry） | `zend_extension` 必须 26 个字段、208 字节，顺序见 `zend_extensions.h` |
| 3 | `api_no_check` 传 NULL | `requires Zend Engine API version -135446428`（垃圾值） | 自己实现回调，永远 `return SUCCESS;` |

## 关键实现

```c
ZEND_DLEXPORT char *extension_version_info = "1.0";   /* 坎1，必须 */

static int my_api_no_check(int api_no) {              /* 坎3，不能传 NULL */
    return SUCCESS;
}

ZEND_DLEXPORT zend_extension zend_extension_entry = {
    "ze_probe", "1.0", "author", "url", "copyright",   /* 5 个字符串 */
    startup, shutdown, activate, deactivate,            /* 4 个 */
    message_handler,                                    /* 1 个 */
    op_array_handler,                                   /* ← 我们要的 */
    statement_handler, fcall_begin, fcall_end,          /* 3 个 */
    op_array_ctor, op_array_dtor,                       /* 2 个 */
    my_api_no_check,                                    /* 坎3 */
    build_id_check, persist_calc, persist,              /* 3 个 */
    NULL, NULL, NULL, NULL,                             /* reserved5-8 */
    0, 0                                                /* handle, resource_number */
};
```

加载后 `api_no_check called: 320180731` → **PHP 认了**。

## 但还没解决的问题（2026-11-17 08:45）

加载成功之后：

```
api_no_check called: 320180731   ← 日志只有这一行
（startup 没被调用、op_array_handler 没被调用、脚本不执行、rc=0 但无输出）
```

即：**扩展被接受但没被激活**。下一步要查 startup 为什么没跑。

**还有更关键的未知**：就算激活了，loader 用的是它自己的编译器（不走 `pass_two`），
`op_array_handler` 未必会被调用 —— 这是这条路最大的不确定性。

## 结论

**zend_extension 在 PHP 7.3 上可以被加载**（三道坎已破），
但**能否拿到 op_array 回调仍未验证**，且 loader 绕过标准编译流程的风险很高。

对比之下，PHP 7.4 的 `execute_ex` 不递归，op_array 方案已实测通过
（`templateAuthorization()` 返回 `true`，连跑 5 次）。

---

# 追加：第四道坎（PHP 7.3 内部 build_id 检查，未解决）

## 现象

破了前三道坎之后，`php -v` 完全无输出、rc=0、脚本不执行。

## gdb 抓到的确切崩点

```
Program received signal SIGSEGV
__strcmp_evex ()
#1  zend_load_extension_handle ()     ← PHP 加载 zend_extension 的内部函数
#2  zend_llist_apply ()
#4  php_module_startup ()

rdi = 0x... → "API320180731,NTS"      ← 这是 PHP 内部的 ZEND_BUILD_ID
rsi = 0x0   → NULL                     ← strcmp 的第二个参数是 NULL
```

**即：PHP 7.3 在 `zend_load_extension_handle()` 里会拿 `ZEND_BUILD_ID`
和结构里某个字段做 `strcmp`，该字段为 NULL 时直接段错误。**

## 试过但没解决的

| 尝试 | 结果 |
|---|---|
| `reserved5` 填 `"API320180731,NTS"` | 仍崩（rsi 还是 NULL） |
| `reserved5~8` 全部填 build_id | 仍崩 |
| 自己实现 `build_id_check` 回调返回 SUCCESS | 仍崩（说明 PHP 走的是另一条路径，不是这个回调） |

## 为什么停在这

**`zend_extensions.h` 里没有这个字段** —— 它是 PHP 7.3 内部实现依赖的隐藏字段，
位置在头文件声明的 26 个字段之外（可能在 `handle` / `resource_number` 之后，
或者 PHP 对结构做了额外解释）。

**没有 PHP 7.3 源码就无法确定这个字段的偏移。** 继续只能靠暴力枚举
（每个可能的偏移都试一遍，每次都要重编译+跑 gdb），代价极高且不确定。

## 对照：PHP 7.4 没有这个问题

PHP 7.4 的 `execute_ex` 内部**不**回调 `zend_execute_ex` 指针
（7.3 会，所以 7.3 上 hook 它必然递归），因此 **PHP 7.4 用普通扩展就能做 op_array
改写**，不需要碰 zend_extension 这一整套。

**已验证**：57737（PHP 7.4）上 `templateAuthorization()` 返回 `true`，连跑 5 次稳定。
