# jitvm

用户态 x86/x64 指令翻译器。它把已有机器码译成可在宿主上执行的桩代码，沿客人自己的控制流执行，并在每条指令和每次控制流转移时提供回调。

A user-mode x86/x64 instruction translator. It turns existing machine code into stubs the host can execute, follows the guest's own control flow, and offers callbacks on each instruction and each control-flow transfer.

[中文](#中文) · [English](#english)

推到 GitHub 后，仓库 About 可选用下面任意一句：

- 中文：用户态 x86/x64 指令翻译器，把已有机器码译成宿主桩代码执行，并在指令与跳转边界提供回调。
- English: User-mode x86/x64 translator that runs existing machine code as host stubs, with callbacks on each instruction and each branch.

---

## 中文

### 它做什么

jitvm 在用户态把客人指令翻译成宿主桩代码，再按客人控制流跑完。客人的通用寄存器就是宿主寄存器。`RSI` / `ESI` 被翻译器借作指令游标，客人自己的值保存在线程槽里。

适合把一段已有机器码放进翻译器里执行，并在每条指令或每次跳转时插入回调。当前实现覆盖宿主为 x86 或 x64 的情况。客人代码的字节保持原样，记录、停下和改道都发生在回调里。

### 可以做什么

#### 1. 快速取得执行记录

`on_inst` 在每条指令执行前拿到客人状态，`on_flow` 在跳转、调用和返回之后拿到新的指令指针。把这些地址记下来就是一份执行记录。`jitvm_last_flow_ip` 是转移指令自己的地址，`jitvm_last_flow_type` 区分跳转、条件跳转、调用和返回。

```c
static void on_inst(jitvm_guest_state* state) {
	log_rip((uintptr_t)state->regs.x86->rip);
}

static void on_flow(jitvm_guest_state* state) {
	log_flow(jitvm_last_flow_ip(),
		(uintptr_t)state->regs.x86->rip,
		jitvm_last_flow_type());
}
```

`jitvm_init` 之后默认是 `JITVM_MONITOR_SKIP_FILTER`，每条指令和每次转移都会进回调。只要某一段的记录时，用第 4 节的地址过滤把范围收窄。

#### 2. 无痕中断调试

断点放在回调里，不必往客人代码写入 `int3`。`on_inst` 看到指令指针落在断点上，可以检查或修改 `state->regs.x86`。回调返回后，翻译器执行这条指令，下一条再进回调，单步因此可以留在翻译器里。`jitvm_set_exit_flag` 则从当前客人指令起交回原生执行，客人的通用寄存器按进入翻译器时的约定还原。

```c
static void on_inst(jitvm_guest_state* state) {
	if ((uintptr_t)state->regs.x86->rip == breakpoint) {
		inspect(state->regs.x86);
		jitvm_set_exit_flag();
	}
}
```

客人代码里已经存在的 `int3`、`int`、`int1`、`into` 会进 `on_exit`，后一条指令留在翻译器外面。回调可以先把 `rip` 挪过这条中断，退出后从新地址原生继续。

#### 3. 无痕改执行流

在指令执行前改 `state->regs.x86->rip`，当前这条客人指令就不会执行，翻译器从新地址继续取指令。原来的字节保持不动，后面的代码仍由翻译器接着跑。同一份映像里的通用寄存器也会写回，改参数寄存器后，下一条指令按新值执行。

```c
static void on_inst(jitvm_guest_state* state) {
	if ((uintptr_t)state->regs.x86->rip == call_site)
		state->regs.x86->rip = (intptr_t)replacement;
}
```

#### 4. 监控一段代码

`jitvm_set_inst_monitor` 和 `jitvm_set_flow_monitor` 分开控制指令回调和转移回调：

| 值 | 行为 |
| --- | --- |
| `JITVM_MONITOR_SKIP_FILTER` | 不看过滤，每次都回调。这是 `jitvm_init` 之后的默认值 |
| `JITVM_MONITOR_ALL` | 先问当前地址过滤。命中走回调，未命中走过滤里登记的另一条路径 |
| `JITVM_MONITOR_OFF` | 关闭回调，只翻译执行 |

地址过滤用 `jitvm_x86_range_filter_create`。第三个参数为 0 时，指令指针落在 `[start, end)` 里算命中；为 1 时，区间以外算命中。三条现成的去向：

| 函数 | 去向 |
| --- | --- |
| `jitvm_default_filter` | 进入回调 |
| `jitvm_nofilter` | 跳过回调，继续翻译执行 |
| `jitvm_nofilter_exit` | 退出翻译器，从当前客人指令原生继续 |

只监控某一段，范围外仍然翻译执行：

```c
jitvm_x86_range_filter* filt = jitvm_x86_range_filter_create(start, end, 0);
jitvm_x86_range_filter_set_targets(filt, jitvm_default_filter(), jitvm_nofilter());
jitvm_set_filter(jitvm_x86_range_filter_proc(filt));
jitvm_set_inst_monitor(JITVM_MONITOR_ALL);
jitvm_set_flow_monitor(JITVM_MONITOR_ALL);
```

离开这段就交回原生代码时，把未命中目标换成 `jitvm_nofilter_exit()`。

### 目录

| 路径 | 内容 |
| --- | --- |
| `include/jitvm/` | 公开接口：`jitvm.h`、`jitvm_x86.h` |
| `src/arch/x86/` | 解码、翻译、分派和地址过滤 |
| `src/arch/x86/runtime/` | 翻译失败时链接下一条指令的运行时桩 |
| `src/platform/` | 可执行内存、线程槽、锁 |
| `examples/` | 可编译示例：初始化后跳进翻译器执行一个函数 |
| `tests/` | 与原生执行对比的回归测试 |
| `build/` | CMake 生成目录，不需要提交 |

Zydis 在配置时通过 CMake FetchContent 拉取，版本固定为 4.1.1；Zycore 固定为 1.5.2。

### 环境

- CMake 3.16 或更新
- C++17 编译器
- 能访问 GitHub，配置阶段要克隆 Zydis 和 Zycore
- Windows：Visual Studio 自带的 MSVC，以及 MASM（`ml64` / `ml`）
- Linux / macOS：GCC 或 Clang，以及 GNU 汇编器

只支持 x86 和 x64 宿主。在其它架构上库可以编出来，但 `jitvm_init` 会返回不支持。

### 编译

在仓库根目录生成工程并编译 Debug：

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

Release 把上面的 `Debug` 换成 `Release`。已经有 `build` 目录时，改完代码只需要第二条。

主要产物：

| 产物 | Debug 路径 |
| --- | --- |
| 静态库 | `build\Debug\jitvm.lib` |
| 回归测试 | `build\tests\Debug\jitvm_test.exe` |
| 示例程序 | `build\Debug\jitvm_guest_demo.exe` |

用 Ninja 等单配置生成器时，可执行文件直接在 `build` 和 `build/tests` 下，没有 `Debug` 这一层。

### 运行

回归测试比较原生执行和模拟执行的寄存器、标志和内存：

```powershell
.\build\tests\Debug\jitvm_test.exe
```

也可以用 CTest：

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

示例程序会初始化翻译器，跳进去执行 `guest_box`。这个函数直接调用 `MessageBoxW`，用来看翻译器能跟着系统库走多远：

```powershell
.\build\Debug\jitvm_guest_demo.exe
```

标准错误会打印每次进入新模块时的步数。调用返回后，标准输出打印 `rax` 和经过的模块。这次调用会弹出对话框，点确定后 `rax` 为 1（`IDOK`）。访问违规或非法指令会在退出前打印最后一条客人指令、字节和寄存器。

指令回调会改掉易失的 XMM 和 x87 状态。分派桩在调用回调前后用 `xsave` / `xrstor` 把它们保存下来，所以回调里的普通 C 代码不会把客人的向量寄存器写坏。第一次翻译一条指令时的链接入口同样保存完整的扩展状态，避免链接过程里的普通 SSE 指令清掉 YMM 的高 128 位。

### 示例在做什么

`guest_box` 只有一次 `MessageBoxW` 调用。翻译器会跟着这个调用进入系统库。`guest_work` 仍留在同一个文件里，是一段关闭优化的整数运算，当前入口不跑它。

`main` 的顺序是：

1. `jitvm_init` 按当前机器建立翻译器，并登记指令和跳转回调。
2. `jitvm_set_guest_ip` 把客人指令指针指到 `guest_box`。
3. 单独准备 8MB 客人栈，并把线程环境块里的栈范围放宽。指令回调使用的是另一块栈，范围仍按原线程栈时，栈检查会在第一条客人指令之前访问违规。
4. `demo_run_vm` 换到客人栈，压上返回地址，再跳进 `jitvm_init` 返回的入口。
5. 函数执行 `ret` 时，回调发现指令指针回到了返回点，置退出标志，控制权交回原生代码。

入口汇编在 `examples/enter_x64.asm`（32 位是 `enter_x86.asm`）。

### 模拟范围

翻译按指令形态处理：同一类编码共享一条翻译路径，助记符本身不各写一份。

- 带位移或 RIP 相对寻址的内存操作数会在运行时重算地址。
- 普通立即数在执行时从客人指令读出，并按操作数宽度做符号扩展。
- 只能是立即数的控制字节（移位计数、`enter`、部分 SSE/AVX 立即数）写进分派表，地址仍然重定位。
- `call`、`jmp`、`jcc`、`loop`、`ret` 单独处理，目标由专用路径计算。
- 段前缀会保留，包括 `FS` / `GS`，以及以 `RSP` / `RBP` 为基址时的 `SS`。间接 `call` / `jmp` 先把有效地址算进寄存器再取出目标；地址已经是线性地址之后，只保留 `FS` / `GS`。

回归测试用原生结果对照这些路径，包括内存立即数、一元内存运算、`RSI` 作基址、RIP 相对寻址、`ret imm16`、远调用的栈平衡，以及编译器生成的客人函数。

下面这些走专门路径，或者不适合在测试里真正执行：

- `syscall` / `sysenter` 会落到宿主指令上。翻译器会先恢复客人 `RSI`，并在 `syscall` 返回后把 `RCX` 设成客人的下一条地址。
- `int3` 一类中断走退出回调，随后的指令留在翻译器外面。
- 远 `call` / `jmp` / `ret` 按平坦模型处理：压入 `CS` 和返回地址，返回时都弹出。在开启控制流保护的 Windows 上，直接执行真正的远转移可能访问违规。
- 这是用户态翻译器，页表、中断门和系统调用约定由宿主自己完成。

### 接口入口

公开头文件是 `include/jitvm/jitvm.h`。一个线程开始模拟前调用 `jitvm_init`，它返回进入模拟的函数指针。进入之前用 `jitvm_set_guest_ip` 指定第一条客人指令。`on_inst` 在每条指令执行前调用，`on_flow` 在跳转、调用和返回后调用。回调里可以用 `jitvm_set_exit_flag` 让当前指令边界回到原生代码。

```c
jitvm_config cfg = {0};
cfg.machine = jitvm_host_machine();
cfg.on_inst = on_inst;
cfg.on_flow = on_flow;
cfg.on_exit = on_exit;

jitvm_enter_fn enter = jitvm_init(&cfg);
jitvm_set_guest_ip((uintptr_t)guest_fn);
/* 换到客人栈，压上返回地址，再调用 enter。完整步骤见 examples/guest_demo.cpp。 */
```

x86/x64 寄存器布局、指令长度和地址范围过滤在 `include/jitvm/jitvm_x86.h`。

### 许可证

本项目以 [MIT License](https://opensource.org/license/mit) 授权，全文见仓库根目录的 `LICENSE`。版权所有者是 lifeengines。

配置阶段拉取的第三方组件与 MIT 兼容，许可声明保留在各自仓库里：

| 组件 | 版本 | 许可证 |
| --- | --- | --- |
| [Zydis](https://github.com/zyantific/zydis) | 4.1.1 | MIT |
| [Zycore](https://github.com/zyantific/zycore-c) | 1.5.2 | MIT |

---

## English

### What it does

jitvm translates guest instructions into host stubs and runs them along the guest control flow. Guest general-purpose registers are the host registers. The translator borrows `RSI` / `ESI` as the instruction cursor and keeps the guest's own value in a per-thread slot.

Use it to execute a stretch of existing machine code and to insert callbacks on every instruction or every branch. The current implementation supports x86 and x64 hosts. Guest bytes stay as they are. Tracing, stopping, and redirecting happen in the callbacks.

### What it can do

#### 1. Capture an execution trace

`on_inst` receives the guest state before each instruction. `on_flow` receives the new instruction pointer after a jump, call, or return. Recording those addresses is the trace. `jitvm_last_flow_ip` is the transfer instruction itself. `jitvm_last_flow_type` distinguishes jumps, conditional jumps, calls, and returns.

```c
static void on_inst(jitvm_guest_state* state) {
	log_rip((uintptr_t)state->regs.x86->rip);
}

static void on_flow(jitvm_guest_state* state) {
	log_flow(jitvm_last_flow_ip(),
		(uintptr_t)state->regs.x86->rip,
		jitvm_last_flow_type());
}
```

After `jitvm_init` the default is `JITVM_MONITOR_SKIP_FILTER`, so every instruction and every transfer enters the callback. To trace only one span, narrow it with the address filter in section 4.

#### 2. Breakpoints that do not patch the guest

The breakpoint lives in the callback. There is no need to write `int3` into the guest. When `on_inst` sees the instruction pointer on a breakpoint, it can read or modify `state->regs.x86`. Returning from the callback lets the translator execute that instruction and enter again on the next one, so single-step stays inside the translator. `jitvm_set_exit_flag` hands control back to native execution at the current guest instruction, and restores the guest general-purpose registers the way they were on entry.

```c
static void on_inst(jitvm_guest_state* state) {
	if ((uintptr_t)state->regs.x86->rip == breakpoint) {
		inspect(state->regs.x86);
		jitvm_set_exit_flag();
	}
}
```

An `int3`, `int`, `int1`, or `into` that is already in the guest goes to `on_exit`. The following instruction stays outside the translator. The callback can move `rip` past the interrupt, and native execution continues at the new address.

#### 3. Redirect control flow without a byte patch

Change `state->regs.x86->rip` before the instruction runs. That guest instruction is skipped, and the translator fetches from the new address. The original bytes stay in place, and later code keeps running inside the translator. General-purpose registers in the same image are written back, so the next instruction sees updated argument registers.

```c
static void on_inst(jitvm_guest_state* state) {
	if ((uintptr_t)state->regs.x86->rip == call_site)
		state->regs.x86->rip = (intptr_t)replacement;
}
```

#### 4. Watch a range of code

`jitvm_set_inst_monitor` and `jitvm_set_flow_monitor` choose when the instruction callback and the transfer callback run:

| Value | Behavior |
| --- | --- |
| `JITVM_MONITOR_SKIP_FILTER` | Ignore the filter and call back every time. This is the default after `jitvm_init` |
| `JITVM_MONITOR_ALL` | Ask the current address filter. A hit takes the callback. A miss takes the other path registered in the filter |
| `JITVM_MONITOR_OFF` | Turn callbacks off and only translate |

Build the filter with `jitvm_x86_range_filter_create`. A third argument of 0 matches an instruction pointer inside `[start, end)`. A value of 1 matches addresses outside that range. Three ready-made destinations:

| Function | Destination |
| --- | --- |
| `jitvm_default_filter` | Enter the callback |
| `jitvm_nofilter` | Skip the callback and keep translating |
| `jitvm_nofilter_exit` | Leave the translator and continue natively at the current guest instruction |

Watch one span, and keep translating outside it:

```c
jitvm_x86_range_filter* filt = jitvm_x86_range_filter_create(start, end, 0);
jitvm_x86_range_filter_set_targets(filt, jitvm_default_filter(), jitvm_nofilter());
jitvm_set_filter(jitvm_x86_range_filter_proc(filt));
jitvm_set_inst_monitor(JITVM_MONITOR_ALL);
jitvm_set_flow_monitor(JITVM_MONITOR_ALL);
```

To return to native code when execution leaves the span, use `jitvm_nofilter_exit()` as the miss target.

### Layout

| Path | Contents |
| --- | --- |
| `include/jitvm/` | Public headers: `jitvm.h`, `jitvm_x86.h` |
| `src/arch/x86/` | Decode, translate, dispatch, and address filtering |
| `src/arch/x86/runtime/` | Runtime stubs that link the next instruction when translation fails |
| `src/platform/` | Executable memory, per-thread slots, locks |
| `examples/` | Buildable sample: initialize, then enter the translator on one function |
| `tests/` | Regression tests that compare against native execution |
| `build/` | CMake output; do not commit |

Zydis is fetched at configure time with CMake FetchContent, pinned at 4.1.1. Zycore is pinned at 1.5.2.

### Requirements

- CMake 3.16 or newer
- A C++17 compiler
- Network access to GitHub during configuration, to clone Zydis and Zycore
- Windows: MSVC from Visual Studio, plus MASM (`ml64` / `ml`)
- Linux / macOS: GCC or Clang, plus the GNU assembler

Only x86 and x64 hosts are supported. The library still builds on other architectures, and `jitvm_init` reports that the machine is unsupported.

### Build

From the repository root, generate the project and build Debug:

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

For Release, replace `Debug` with `Release`. After `build` exists, the second command is enough.

Main outputs:

| Artifact | Debug path |
| --- | --- |
| Static library | `build\Debug\jitvm.lib` |
| Regression test | `build\tests\Debug\jitvm_test.exe` |
| Sample | `build\Debug\jitvm_guest_demo.exe` |

With a single-config generator such as Ninja, the executables land directly in `build` and `build/tests`, with no `Debug` directory.

### Run

The regression test compares registers, flags, and memory between native execution and translated execution:

```powershell
.\build\tests\Debug\jitvm_test.exe
```

CTest works as well:

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

The sample initializes the translator and enters `guest_box`. That function calls `MessageBoxW` directly, which shows how far the translator follows a system library:

```powershell
.\build\Debug\jitvm_guest_demo.exe
```

Standard error prints the step count each time execution enters a new module. After the call returns, standard output prints `rax` and the modules that were visited. The call opens a dialog; choosing OK leaves `rax` as 1 (`IDOK`). An access violation or illegal instruction prints the last guest instruction, its bytes, and the registers before exit.

Instruction callbacks clobber volatile XMM and x87 state. The dispatch stub saves and restores that state with `xsave` / `xrstor` around the callback, so ordinary C code in a callback leaves the guest vector registers intact. The link entry used the first time an instruction is translated also saves the full extended state, so ordinary SSE instructions in the linker do not clear the high 128 bits of YMM.

### What the sample does

`guest_box` is a single `MessageBoxW` call. The translator follows that call into the system libraries. `guest_work` remains in the same file: integer code compiled with optimizations off. The current entry point does not run it.

`main` proceeds as follows:

1. `jitvm_init` builds the translator for the current machine and registers the instruction and branch callbacks.
2. `jitvm_set_guest_ip` points the guest instruction pointer at `guest_box`.
3. The sample allocates an 8MB guest stack and widens the stack range recorded in the thread environment block. Callbacks run on a different stack. If the recorded range still describes the original thread stack, the stack check raises an access violation before the first guest instruction.
4. `demo_run_vm` switches to the guest stack, pushes a return address, and jumps to the entry returned by `jitvm_init`.
5. When the function executes `ret`, the callback sees the instruction pointer back at the return point, sets the exit flag, and returns control to native code.

The entry assembly is `examples/enter_x64.asm` (`enter_x86.asm` for 32-bit).

### Translation coverage

Translation is organized by instruction shape. One encoding family shares one path.

- Memory operands with a displacement, including RIP-relative addressing, have their address recomputed at run time.
- Ordinary immediates are read from the guest instruction at execution time and sign-extended to the operand width.
- Control bytes that can only be immediates (shift counts, `enter`, some SSE/AVX immediates) are stored in the dispatch table. Addresses are still relocated.
- `call`, `jmp`, `jcc`, `loop`, and `ret` have their own paths. Targets are computed there.
- Segment prefixes are kept, including `FS` / `GS` and `SS` when the base is `RSP` / `RBP`. An indirect `call` / `jmp` computes the effective address into a register, then loads the target. Once that address is linear, only `FS` / `GS` remain.

Regression tests check these paths against native results: memory immediates, unary memory operations, `RSI` as a base, RIP-relative addressing, `ret imm16`, stack balance of a far call, and compiler-generated guest functions.

These cases use a dedicated path, or are a poor fit for actually executing inside the tests:

- `syscall` / `sysenter` fall through to the host instruction. The translator restores the guest `RSI` first, and after `syscall` returns it sets `RCX` to the guest's next address.
- Interrupts such as `int3` go to the exit callback. Later instructions stay outside the translator.
- Far `call` / `jmp` / `ret` use a flat model: push `CS` and the return address, and pop both on return. On Windows with Control Flow Guard enabled, executing a real far transfer can raise an access violation.
- This is a user-mode translator. The host owns page tables, interrupt gates, and the system-call convention.

### API

The public header is `include/jitvm/jitvm.h`. Call `jitvm_init` before a thread starts translating. It returns the function pointer used to enter. Set the first guest instruction with `jitvm_set_guest_ip` before entry. `on_inst` runs before each instruction. `on_flow` runs after jumps, calls, and returns. From a callback, `jitvm_set_exit_flag` returns to native code at the current instruction boundary.

```c
jitvm_config cfg = {0};
cfg.machine = jitvm_host_machine();
cfg.on_inst = on_inst;
cfg.on_flow = on_flow;
cfg.on_exit = on_exit;

jitvm_enter_fn enter = jitvm_init(&cfg);
jitvm_set_guest_ip((uintptr_t)guest_fn);
/* Switch to a guest stack, push a return address, then call enter.
   The full sequence is in examples/guest_demo.cpp. */
```

The x86/x64 register image, instruction length, and address-range filter are in `include/jitvm/jitvm_x86.h`.

### License

This project is released under the [MIT License](https://opensource.org/license/mit). The full text is in `LICENSE` at the repository root. The copyright holder is lifeengines.

Third-party components fetched at configure time are MIT-compatible. Their license notices stay with those projects:

| Component | Version | License |
| --- | --- | --- |
| [Zydis](https://github.com/zyantific/zydis) | 4.1.1 | MIT |
| [Zycore](https://github.com/zyantific/zycore-c) | 1.5.2 | MIT |
