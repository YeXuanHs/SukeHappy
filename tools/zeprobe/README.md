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
