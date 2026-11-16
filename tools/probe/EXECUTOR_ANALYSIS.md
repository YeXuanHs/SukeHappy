# Swoole Loader 执行器分析（phase 2，57737 实测）

## 一、执行器在哪

loader 文件只有 151KB 可执行代码（`LOAD off=0x0 filesz=0x25011 R+X`），
真正的执行器**不在文件里**——它运行时被解压到一块**匿名可执行 mmap**：

```
7f35b360a000-7f35b36ac000  r-xp  anon  663,552 字节（86.1% 非零）
```

定位方法：
```bash
# 1. 找匿名可执行段
awk '$2 ~ /x/ && $6=="" {print $1}' /proc/<pid>/maps
# 2. 用 python 读 /proc/<pid>/mem 整段 dump（见 tools/dumps.py 思路）
# 3. 段内搜索 41 55 41 54 55 48 89 fd（handler 公共前缀）
#    → 351 个 handler 起点
```

**`h53.asm` 的 off 最大 0x71510 < 段大小 0xa2000** → 它就是这个执行器的采样。

## 二、op_array / zend_op 结构偏移（PHP 7.4.33 实测）

```
zend_op_array:  last=0x3C  last_literal=0xA0  opcodes=0x40  literals=0xA8  function_name=0x08
zend_op:        handler=0x00  op1=0x08  opcode=0x1C  op1_type=0x1D
zend_function:  type=0x00  function_name=0x08  op_array=0x00（摊平）
```
（用 `offsetof()` 编小程序打印得到，见 `.tmp/offsets.c`）

## 三、opcode 分发器 @ 段内 0xb3c0（标准 zend_vm_set_opcode_handler）

```asm
b3c0: movzbl 0x1c(%rdi),%edx      ; edx = op->opcode
b3c4: lea    0x91360(%rip),%rax   ; 【表1】opcode flags（196 项 × 4 字节）
b3cb: mov    (%rax,%rdx,4),%edx   ; edx = flags[opcode]
b3ce: xor    %eax,%eax
b3d0: test   $0x10000,%edx        ; op1 参与？
b3d8:   movzbl 0x1d(%rdi),%ecx    ;   op->op1_type
b3dc:   lea    0x90f00(%rip),%rax ;   【表2】type 权重
b3e3:   mov    (%rax,%rcx,4),%eax ;   eax = t2[op1_type]
b3e6: test   $0x20000,%edx        ; op2 参与？
b3ee:   movzbl 0x1e(%rdi),%esi
b3f9:   lea    (%rax,%rax,4),%eax ;   eax *= 5
b3fc:   add    (%rcx,%rsi,4),%eax ;   += t2[op2_type]
b3ff: test   $0xfffc0000,%edx     ; 有特化位？
b407:   test $0x80000  → jne 0xb460
b40f:   test $0x100000 → jne 0xb498
b41b:   test $0x40000  → jne 0xb480
b423:   test $0x1000000→ jne 0xb4b8
b42f:   test $0x200000 → result_type 特化（×3，比较 0x2b/0x2c）
b450: movzwl %dx,%edx             ; 取 flags 低 16 位（索引基）
b453: add    %edx,%eax            ; 累加
b457: mov    %rax,(%rdi)          ; *** op->handler = rax ***
b45a: jmp    0xa60
```

**结论：执行时看 `op->handler`，不看 `opcode`。**
→ 这就是「改 opcode 无效」的真正原因：改了 opcode，handler 还是旧的。

## 四、两张表（段内相对偏移）

| 表 | 偏移 | 项宽 | 内容 | 长度 |
|---|---|---|---|---|
| 表1 | `0x91360` | 4 字节 | `低16位=handler索引基` / `高16位=特化位` | **196 项**（=PHP 7.4 opcode 总数） |
| 表2 | `0x90f00` | 4 字节 | operand type 权重 | ≥12 项 |

**实测值**：
```
flags[0]  = 0x00000000
flags[1]  = 0x00030001
flags[60] = 0x0008053e    ← ZEND_DO_FCALL：索引基 0x053e，特化位 0x80000
flags[62] = 0x00010541    ← ZEND_RETURN ：索引基 0x0541，特化位 0x0001
flags[64] = 0x00000547    ← ZEND_RECV_INIT
flags[84] = 0x00030614    ← ZEND_FETCH_DIM_W
t2[0..11] = 3,0,1,3,2,3,3,3,4,0,0,0
```

**opcode 60 的 handler 索引**（按 b460 分支）：`0x053e + (byte@0x1f ? 1 : 0)`

## 五、卡在哪：handler 表的位置没找到

`op->handler` 最终需要是**绝对地址**，但上面算出来的只是索引（几百）。
那个「索引 → 地址」的表：

| 搜索假设 | 结果 |
|---|---|
| 段内 8 字节 == handler 起点（段内偏移） | **0 处** |
| 段内 8 字节 == 段基址+handler 起点（绝对地址） | **0 处** |
| 段内 4 字节相对表（连续 ≥40 项） | **0 处** |
| loader 的 2MB `---p` 预留段 | 纯预留，从未写入（114 次 mprotect 均不涉及它） |

→ **表在 loader 的 r--p（12KB）/ rw（4KB）段，或堆上动态分配。**

## 六、下一步（若继续）

1. dump loader 的 `r--p 12288` + `rw-p 4096` 两段，按上表方法再搜一遍
2. 或在 `0xb457`（`mov %rax,(%rdi)`）下断点：
   断住时 `$rax` 就是**最终 handler 绝对地址** → 减去段基址 → 得到相对偏移
   → 再看 `op->handler` 指向的 handler，顺着追到 DO_FCALL 的实际处理
3. 拿到 DO_FALL handler 后，patch 方案：
   - 改 `op->handler` 赋值那一步（让它指向我们的函数），或
   - 直接 patch DO_FCALL handler 的入口 5 字节为 `jmp 我们的函数`

## 七、产物

```
_dump/loader_executor_663k.bin   663KB   解压后的执行器
_dump/loader_executor.asm        8.8MB   它的完整反汇编（171,939 行）
_dump/loader_file_text_151k.bin  151KB   loader 文件的可执行段
tools/probe/probe.c                      op_array 结构探针
```

## 八、为什么这次先停

不是因为做不到，是**投入产出已经翻转**：
- 还差「handler 表定位」一关，之后还要解决**生产环境如何注入 patch**
- 而功能目标（让模板认为授权通过）**现在这个方案已经达到了**
- 继续的收益只是「少依赖一层网络」，风险却是动生产环境的 PHP 扩展
