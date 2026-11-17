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

---

# 追加：两个重要更正（2026-11-17 07:40）

## 更正 1：opcode 编号我之前搞错了，之前所有 op_array 实验因此失真

PHP 7.4 **真实**编号（`/opt/php74/include/php/Zend/zend_vm_opcodes.h`）：

```
59 = ZEND_INIT_FCALL_BY_NAME
60 = ZEND_DO_FCALL          ← 不是 RETURN！
62 = ZEND_RETURN            ← 真正的 RETURN
63 = ZEND_RECV
64 = ZEND_RECV_INIT         ← dump 里 op[0]=64，与 FINAL_dis 的 "000 RECV_INIT" 完全吻合
68 = ZEND_NEW
```

所以之前 dump 到的 `op[2]=60 / op[5]=60` 是 **DO_FCALL**，不是 RETURN；
我"抄一条现成 RETURN 的 handler"时抄到的是 **DO_FCALL 的 handler**，
再配上 opcode 写成（我误认为的）RETURN —— 造出来的是一条畸形指令。

**这意味着：op_array 改写从来没有在正确参数下被真正验证过。**
之前的"返回 NULL / 乱码"不能作为"不可行"的证据。

## 更正 2：loader 对加密文件完全不走 `zend_compile_file`

dp 实测（probe 比 loader 先加载，故 loader MINIT 时保存的 orig 就是 probe 的 hook）：

```
RINIT-ENTER, zend_compile_file=0x7f3f5880baa0   ← = loader 基址 + 0x10baa0
```

但 `compile_file called` 一条都没有 → **loader 用自己的编译器，不经过任何 PHP 公开入口**。

## 更正 3：loader 是「节头全删」的加壳二进制

```
readelf -SW      → 空（节头被删）
objdump -s -j .rodata → 无输出
readelf -rW      → no relocations（动态重定位 0 条）
readelf -dW      → NEEDED 只有 libc.so.6
readelf -sW      → 无符号表
```

但 `strings` 里有一堆 `zend_*` 名字 → 它是**按名字在内存里自解析**（手动 dlsym 风格）。
→ **没有 GOT 可以劫持**，没法在 PLT 层面插桩。

## 更正 4：h53.asm 不是「53 个 handler 的表」，而是 opcode 特化分发逻辑

```asm
movzbl 0x1c(%rbx),%edx        ; 取一个字节（loader 自己 opcode 流里的 opcode）
lea    0x43f02(%rip),%rax     ; 0x43f30 ← flags 表（uint32 × N）
mov    (%rax,%rdx,4),%eax     ; flags = table[opcode]
test   $0x10000,%eax          ; 位测试 → 选特化 handler
lea    (%rdx,%rdx,4),%edx     ; edx = edx*5（累加偏移）
add    (%rcx,%rsi,4),%edx     ; 再加 operand type 表（0x43ad0）的项
```

这是 **PHP 标准 `ZEND_VM_SET_OPCODE_HANDLER` 的特化选择逻辑** —— 说明 loader 解码后
产出的就是标准 zend_op，**用的就是标准 VM 语义**。

## 因此下一步（对症的做法）

既然 loader 为「被调函数」生成**特化的调用 handler**（把 op_array 指针硬编码进调用点），
那真正要对症的是那个**硬编码的 op_array 指针**，而不是函数表：

1. gdb 在 `templateAuthorization` 执行时抓现场（断点：`ZEND_DO_FCALL` 之后、
   或直接在那条特化 handler 上）
2. 从调用点的 opcode / run_time_cache 里找出 op_array 指针
3. 把那个指针换成我们自己构造的 `function(){ return true; }` 的 op_array
   （`zend_compile_string` 现编一个就能拿到）

比改 opcode 干净：**不动任何指令，只换一个指针**。

另外，**用正确的 62 + 从别的函数借 RETURN handler** 重新跑一遍 op_array 改写
也应该做 —— 之前的失败是在错参数下得到的，不算数。

---

# 🎉 最终结论（2026-11-17 08:40）：op_array 方案跑通了

## 实测结果

正式扩展 `sukehappy.so` 加载后：

```
$ php-cgi -c ini.ini t8.php
loaded
result => true            ← templateAuthorization() 返回 true
```

连跑 5 次全部 `result => true`。

## 全部踩过的坑（一共 6 个，缺一个都不行）

### 四个技术坑

| # | 坑 | 症状 |
|---|---|---|
| 1 | 函数名长度硬编码成 20（实际 `templateauthorization` 是 **21**） | `zend_hash_str_find` 按长度匹配 → 永远查不到，误判成"拿不到 op_array" |
| 2 | `60` 当成 ZEND_RETURN（实际 60=DO_FCALL，**62=RETURN**） | 抄错 handler → 畸形指令 → 返回 NULL / 整页乱码 |
| 3 | `op1.constant` 当成 literal 索引 | 它是**相对 opline 的字节偏移**：`(char*)&literals[lit] - (char*)opline`；写成索引 → 读到垃圾 → 返回 NULL |
| 4 | 从别的函数「借」handler | 特化 handler 与 op1_type 绑定；而且**遍历函数表**会打断 loader 解密 |

**坑 4 的正解**：用 **`zend_vm_set_opcode_handler(&op)` 函数**自动算 handler。
注意不是宏 —— `ZEND_VM_SET_OPCODE_HANDLER` 宏在 `zend_vm_execute.h` 里，
那个文件太大不能 include；直接写宏名会变成 undefined symbol。

### 两个模块结构坑

| # | 坑 | 症状 |
|---|---|---|
| 5 | 用 `ZEND_MODULE_GLOBALS` + `PHP_GINIT` + `STANDARD_MODULE_PROPERTIES_EX` | PHP 7.4 上 globals 指针错乱，编译报 `sukehappy_globals is a pointer` |
| 6 | 无条件 hook `zend_execute_internal` | loader 解密时会调用内部函数，被截住 → 解密失败 → `common.php` 报 Parse error |

**正解**：全部用 `static` 变量、`MINIT` 保持为空、`STANDARD_MODULE_HEADER` +
`STANDARD_MODULE_PROPERTIES`、只在需要时 hook `execute_internal`。

### 一个环境坑（坑了我 50 多轮）

**用来测试的 ini 文件根本没创建成功** → php-cgi 用默认配置 → 没加载 loader
→ `common.php` 报 Parse error → 误判成"扩展破坏了 loader 解密"。

**教训**：写测试 ini 后**必须 `cat` 出来确认**，别假设写成功了。

## 最终可用的实现要点

```c
/* 1) 找函数：长度用 strlen */
f = zend_hash_str_find_ptr(EG(function_table), "templateauthorization",
                           strlen("templateauthorization"));   /* 21 */

/* 2) 找 IS_TRUE literal */
for (i = 0; i < oa->last_literal; i++)
    if (Z_TYPE_P(&oa->literals[i]) == IS_TRUE) { lit = i; break; }

/* 3) 改第 0 条指令 */
oa->opcodes[0].opcode        = 62;              /* ZEND_RETURN */
oa->opcodes[0].op1_type      = IS_CONST;
oa->opcodes[0].op2_type      = IS_UNUSED;
oa->opcodes[0].result_type   = IS_UNUSED;
oa->opcodes[0].extended_value = 0;
#if ZEND_USE_ABS_CONST_ADDR
oa->opcodes[0].op1.zv        = &oa->literals[lit];
#else
oa->opcodes[0].op1.constant =
    (uint32_t)((char *)&oa->literals[lit] - (char *)&oa->opcodes[0]);
#endif

/* 4) handler 用函数自动算（不要用宏、不要去借） */
zend_vm_set_opcode_handler(&oa->opcodes[0]);

/* 5) EG 和 CG 两份都改 */
```

**时机**：在 `execute_ex` 里，每次进用户函数都试一次；两个函数都改成功就置标志位。
