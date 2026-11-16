# Swoole Loader 内部结构（57737 实测）

分析对象：`/opt/php74/lib/php/extensions/no-debug-non-zts-20190902/swoole_loader74.so`
（165,316 字节）

## 一、内存布局（运行时 /proc/pid/maps）

```
7f20af200000-7f20af20a000 r-xp   40960 字节    只有 40KB 可执行代码
7f20af2ac000-7f20af4ab000 ---p  2093056 字节   2MB，PROT_NONE，未提交物理页
7f20af4ab000-7f20af4ae000 r--p   12288
7f20af4ae000-7f20af4af000 rw-p    4096
```

**那个 2MB 的 `---p` 段是 loader 的解压/执行区**：
- `h53.asm` 里的 `off=0x57430` 比整个文件（0x285C4）还大，但
  `段基址 + 0x57430` 正好落在这个段内 → **h53.asm 就是从这个段里 dump 的**
- 平时它是 `PROT_NONE` 且**没有物理页**：`mprotect(PROT_READ)` 成功（返回 0），
  但随后读 `/proc/pid/mem` 报 **EIO** → 说明这段从未被写入
- → **loader 是在执行时临时解压进去、用完再 mprotect 回去**（甚至释放）

## 二、53 个 opcode handler（h53.asm）

`/root/sw/out/h53.asm` 是按 opcode 编号分组的 **53 个处理函数**反汇编，
每个带在 loader 里的偏移：

```
opcode   3 off=0x57430    opcode  84 off=0x58ee0    opcode 196 off=0x66530
opcode   4 off=0x58440    opcode  85 off=0x4c170    opcode 202 off=0x2b050
opcode   5 off=0x57b10    opcode  91 off=0x49cf0    opcode 207 off=0x5a790
opcode   9 off=0x5bf90    opcode  97 off=0x477f0    opcode 215 off=0x6ef50
opcode  12 off=0x63c40    opcode 100 off=0x45710    opcode 216 off=0x4af80
opcode  14 off=0x5fc50    opcode 107 off=0x3f900    opcode 218 off=0x517c0
opcode  18 off=0x70a00    opcode 108 off=0x3b940    opcode 229 off=0x486f0
opcode  19 off=0x1f9d0    opcode 122 off=0x36c20    opcode 233 off=0x42bd0
opcode  24 off=0x4f440    opcode 123 off=0x5ec80    opcode 234 off=0x520d0
opcode  29 off=0x32210    opcode 138 off=0x3e4c0    opcode 240 off=0x70c40
opcode  30 off=0x63770    opcode 142 off=0x61790
opcode  42 off=0x707c0    opcode 143 off=0x41d90
opcode  46 off=0x25140    opcode 144 off=0x71510
opcode  47 off=0x58830    opcode 148 off=0x6f3d0
opcode  53 off=0x70580    opcode 149 off=0x15930
opcode  80 off=0x44fa0    opcode 154 off=0x43be0
opcode  81 off=0x48db0    opcode 163 off=0x31930
opcode 167 off=0x48300    opcode 170 off=0x6f610
opcode 175 off=0x4fd90    opcode 177 off=0x5f2d0
opcode 180 off=0x5b400    opcode 183 off=0x54ed0
opcode 184 off=0x44d60    opcode 190 off=0x525a0
```

**编号就是标准 PHP 7.4 的**（84=FETCH_DIM_W、234、229 都与 dump 到的 op_array 对得上）。

**注意 `60`（ZEND_DO_FCALL）和 `64`（ZEND_RECV_INIT）不在这个表里**：
- 64 是纯参数接收，loader 单独处理
- **60（函数调用）是独立特化逻辑** → **op_array 指针就在那里**，这是下一个目标

## 三、handler 的公共前缀

每个 handler 开头都是：

```asm
push %r13 / push %r12 / push %rbp
mov  %rdi,%rbp        ; rdi = 执行上下文
mov  (%rdi),%rbx      ; rbx = *rdi
mov  0x18(%rbx),%eax  ; 取字段
and  $0x3fffffff,%eax
cmp  $0x3fffffff,%eax
je   ...
```

`0x18(%rbx)` 的 flags 位决定选哪个特化分支（`test $0x10000 / $0x20000 / ...`），
这是 PHP 标准 `ZEND_VM_SET_OPCODE_HANDLER` 的特化选择逻辑。

## 四、当前卡点

要 dump 那 2MB 解压段，必须**在 loader 正在执行的那一刻**抓，因为：
- 平时 PROT_NONE 且无物理页
- 用完立刻 mprotect 回去

**可行的抓法**（下一步）：
1. 用 h53.asm 里某个 handler 的偏移（如 `0x57430`），
   在运行时 `break *(基址+0x57430)` —— 但该地址平时不在映射内，断不上
2. 改成 **watchpoint**：`watch *(char*)0x<段基址>` 会在**第一次解压写入**时命中 ✓
3. 命中后立刻 dump 整段 → 拿到解压后的全部代码
4. 再从里面找 `cmp $0x3c`（比较 opcode==60）→ 那就是 DO_FCALL 处理 →
   定位"取被调函数 op_array"的指令 → **把指针换成我们现编的 `return true` op_array**

## 五、另一条更省事的思路

既然 60 不在 53 个 handler 里，**先确认 loader 是否真的不用标准 DO_FCALL**：
在 gdb 里对 `0x1c(%rbx)==60` 的条件下断（需要在 handler 公共入口设断点 + 条件判断）。
如果能命中，就能顺着往下看到 op_array 指针。

---

# 重要更正（2026-11-17 07:50）：h53.asm 的归属搞错了

## 证据

**1. 字节特征反查，0 命中**

`h53.asm` 里 opcode 3 的首条指令机器码 `41 55 41 54 55 48 89 fd`
（push r13 / push r12 / push rbp / mov rdi,rbp）在 loader 文件里**一次都搜不到**。

**2. ELF 头自己声明没有节头表**

```
e_shoff   = 0x0
e_shnum   = 0
```

**3. 程序头只有两个 LOAD 段**

```
LOAD off=0x0      vaddr=0x0      filesz=0x25011  memsz=0x25011  flags=5 (R+X)
LOAD off=0x25948  vaddr=0x2ab948 filesz=0x2c58   memsz=0xb098   flags=6 (R+W)
```

整个可执行范围就是 **0x0 ~ 0x25011（151KB）**。
而 `h53.asm` 里的 `off` 最大到 **0x71510（464KB）** —— **超出 loader 的任何偏移**。

**4. 那 2MB 的 `---p` 段是纯预留，从未写入**

`catch syscall mprotect` 抓到 114 次调用，地址全在栈区（`0x7ffff...`），
**没有一次涉及 loader 那个 2MB 段**；直接读它还报 EIO（无物理页）。

## 结论

**`h53.asm` 不是 loader 的代码，是 PHP 主程序（php-cgi）自己的 VM 代码**
（`ZEND_VM_SET_OPCODE_HANDLER` 的 opcode 特化分发）。

那"53 个 handler"是 **PHP 自己的**，`60`/`64` 不在表里也正常
（DO_FCALL 是 `ZEND_VM_HOT_HANDLER` 单独实现的，RECV_INIT 是纯参数接收）。

## 由此反推出的关键事实

**loader 用的就是 PHP 标准 VM，执行的就是标准 op_array。**
（与 dump 到的 `op[0]=64(ZEND_RECV_INIT)` 完全吻合。）

所以「改 op_array」在原理上是成立的，之前的失败**全部源于我把
`60(DO_FCALL)` 当成了 `62(RETURN)`**，抄错了 handler。

## 但入口问题依然无解

| 钩子 | 结果 |
|---|---|
| `zend_compile_file` | 加密文件**不走**（probe 在 loader 之前加载，loader 存的 orig 就是 probe 的 hook，仍 0 次调用） |
| `zend_execute_ex` | **不走**（loader 先/probe 后、probe 先/loader 后，两种加载顺序都测过，0 次命中） |
| `zend_hash_str_find_ptr` | **不走**（断点 0 命中） |
| 遍历 `EG/CG(function_table)` | 能找到函数（dump 成功），但改它无效——loader 执行时不看这张表 |

**loader 内嵌了一份静态链接的 PHP VM**（所以它用标准语义，但不经过主程序的任何全局函数指针）。
要改到它的 op_array，只能 patch 它内嵌的那份 VM —— 那是完整逆向工程量。
