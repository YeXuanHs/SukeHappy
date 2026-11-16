# Swoole Loader op_array 方案：实测结论

> 环境：分析机 57737（`103.236.99.244:57737`），PHP 7.4.33 + `swoole_loader74.so`
> 加密样本：`/root/dec/sw/app/template/common.php`
> 探针：`tools/probe/probe.c`（在 execute_ex / RINIT 里 dump 与改写 op_array）

## 一、loader 的真实形态

`readelf --dyn-syms` 显示 loader 的 **UND 符号数为 0** —— 它把 PHP 的编译器、哈希表、VM
**全部静态链接了一份**，对外只暴露：

```
swoole_loader_compile_file          编译入口
swoole_loader_functions             它自己的函数表
swoole_loader_accel_new_interned_string   自建 interned string 池
swoole_loader_zend_accel_set_auto_globals  替换 executor globals
zend_deserialize_opcode_handler     恢复 op_array 时调用
AES128_ECB_decrypt                  解密
```

**结论：它不是「加密的 PHP 源码」，而是一整套自带编译器 + 执行器 + 哈希表的运行时。**

## 二、op_array 结构（实测 dump）

模板函数**同时被注册进 PHP 的 EG / CG(function_table)**，所以能拿到**标准 zend_op_array**：

```
type            = 2 (ZEND_USER_FUNCTION)
function_name   = templateAuthorization      ← 驼峰原样，函数表键也是驼峰
filename        = (空)
last            = 54      T = 3      last_var = 6      last_literal = 38
opcodes         = 0x7f59552938e8
literals        = 0x7f5955293450

op[0] opcode=64   op[1] opcode=68   op[2] opcode=60   op[3] opcode=234
op[4] opcode=84   op[5] opcode=60   op[6] opcode=27   op[7] opcode=146
op[8] opcode=146  op[9] opcode=27   op[10] opcode=27  op[11] opcode=229

lit[5] type=3 bool=true                              ← 有现成的 IS_TRUE literal
lit[6] str='https://apiv1.sukeyun.com/ky/admin/template?url='
lit[7] str='&v=cx'
lit[9] str='QDKZEEP5AAL'
```

**注意 opcode 编号**：`68 / 146 / 234 / 229` 这些在 PHP 标准 opcode 表里**根本不存在**。
→ **loader 重新编号了 opcode，用的是它自己那张 opcode 表和自己的 handler。**
→ `60` 是 loader 的 RETURN（从紧跟 RECV 的位置反推出来的）。

## 三、试过的做法与实测结果

| # | 做法 | 实测结果 |
|---|---|---|
| 1 | 把函数体第一条指令改成 `RETURN <IS_TRUE literal>` | 函数返回 **NULL** |
| 2 | **不改任何指令**，只把执行位置 `opline` 跳到最后那条 `op1_type==IS_UNUSED` 的 RETURN | 仍返回 **NULL** |
| 3 | 在 `execute_ex` 里遍历 `EG/CG(function_table)` | 整页输出**乱码** |
| 4 | 替换全局 `zend_execute_ex` | 抢走 loader 的执行入口 → **乱码** |
| 5 | 替换全局 `zend_compile_file` | loader 在 MINIT 把它覆盖掉 → hook **不触发** |
| 6 | 在 `zend_hash_str_find_ptr` 下断点 | **0 命中** —— loader 用自己静态链接的哈希表 |
| 7 | 在 RINIT 时查 `CG(function_table)` 找模板函数 | **找不到** —— 函数是 require 时才注册的 |

### 第 2 条是最致命的证据

按 PHP 标准 VM 语义，那条 `op1_type == IS_UNUSED` 的 RETURN 执行时返回 `EX(return_value)`，
正是塞 `true` 的地方。我们**一个字节的指令都没改**，只是换了执行位置，返回值就不对
→ **loader 的 RETURN 处理根本不看 `op1_type`，它有自己的特化 handler。**

## 四、根因

> loader 把模板编译成 op_array 后，**用它自己那张表查找函数、用它自己那张 opcode 表执行**。
> PHP 主程序的 `EG/CG(function_table)` 只是它顺手登记的一份副本，**执行时不看**。
>
> 所以「按 PHP 标准语义去改 op_array」这个前提不成立：
> 改 PHP 表里那份 → loader 不看；改 loader 自己那份 → 才知道它在哪。

## 五、唯一还剩的路（未做）

**Ghidra 定位 loader 内部那张函数表 + 它自己的 opcode 表**，然后在它自己的表里改 op_array。
需要的具体信息：

1. loader 内部「函数注册」和「函数查找」的实现位置（混淆 + 控制流平坦化，Ghidra 静态分析
   出的 `swoole_loader_compile_file` 全是垃圾指令，难度高）
2. 它自己那张表的 key 是怎么编码的（自己的字符串池？）
3. 表里 op_array 的存放形式（是标准 zend_op_array 还是自有结构）
4. 它的 RETURN（编号 60）对应的 handler 语义 —— 要知道它怎么取返回值

**已知的现成材料**：
- Ghidra 分析结果：`/root/sw/ghidra_out.txt`（`swoole_loader_compile_file @ 0x10baa0`）
- 每个加密文件的完整 opcode 反汇编：`/root/sw/out/enc/*.ops`
- 解密后的伪源码：`/root/dec/sw/DEC_family/`
- 本探针（能 dump 到标准 op_array 结构）：`tools/probe/probe.c`

## 六、结论一句话

**op_array 方案在 Swoole Loader 环境下不是「难」，是「入口不对」**：
不是拿不到 op_array（拿得到，结构还是标准的），而是**改错了那份 op_array**——
loader 执行时用的是它自己表里的那一份。

真要做，成本集中在「定位 loader 内部函数表 + 它自己的 opcode 语义」，
属于二进制 patch 级别的工程（不是 PHP 扩展能覆盖的层面）。
