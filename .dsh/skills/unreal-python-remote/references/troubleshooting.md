# 排障

下文命令都写成 `& $py $ue <子命令>`，先在 shell 里定好这两个变量：

```powershell
$py = 'C:\Users\姚宇翔\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
$ue = '<skill-dir>\scripts\ue.py'
```

按 `& $py $ue doctor` 的**退出码**定位，不要盲试。

---

## 退出码 2：`no Unreal Editor answered on the multicast group ...`

链路是「本机 UDP 多播发现 + TCP 回连」，所以出错只可能在这几处。按顺序查：

**1. 编辑器真的在跑吗？**

```powershell
Get-Process UnrealEditor -ErrorAction SilentlyContinue | Select-Object Id,StartTime
```

**2. 项目的 Python 插件设置。** 远程执行是**项目级**开关，编辑器照常运行、Python 在编辑器内部也能用，但只要这个开关是关的，它就**完全不响应发现请求** —— 这是最难猜的一种失败。

权威入口是编辑器界面：`Project Settings > Plugins > Python > Enable Remote Execution?`。在界面里切换是**实时生效**的（插件会重建 UDP 监听，源码见 `FPythonScriptRemoteExecution::SyncToSettings`），不需要重启。

本项目的声明写在 `Config/DefaultEngine.ini`（已实测确认这是本机生效来源）：

```powershell
Select-String -Path '<项目>\Config\DefaultEngine.ini' -Pattern 'PythonScriptPluginSettings' -Context 0,3
```

需要看到：

```ini
[/Script/PythonScriptPlugin.PythonScriptPluginSettings]
bRemoteExecution=True
```

⚠️ 界面上切换会实时生效，但**手工改 ini 文件不会** —— 那条路径只在插件初始化时读一次，改完要重启编辑器。所以优先用界面改，用 ini 只是用来确认"项目是怎么声明的"。

**3. Python Editor Script Plugin 是否启用。** `Edit > Plugins` 搜 `Python`。在本 skill 之外确认：

```powershell
Select-String -Path '<项目>\AutoChess.uproject' -Pattern 'PythonScriptPlugin'
```

没写进 `.uproject` 也不一定是问题（可能只启用在引擎级），以 `Edit > Plugins` 里的实际状态为准。

**4. 多播地址/端口是否被改过。** 默认值是：

| 项 | 默认 |
|---|---|
| 多播组 | `239.0.0.1` |
| 多播端口 | `6766` |
| 绑定地址 | `127.0.0.1` |

如果项目里覆盖成了别的值，`ue.py` 也要跟上：

```powershell
& $py $ue --multicast 239.0.0.1 --multicast-port 6766 nodes
```

多播 TTL 是 `0`（仅限本机），**跨机器发现不了是设计如此**，不是 bug。

**5. 列表为空但端口在听。** 验证编辑器侧确实占用了多播端口：

```powershell
Get-NetUDPEndpoint | Where-Object LocalPort -eq 6766 | Select-Object LocalAddress,LocalPort,OwningProcess
```

看到一个 `127.0.0.1:6766` 且 `OwningProcess` 就是编辑器 PID，说明编辑器侧没问题，那就是客户端的组/端口对不上（回到第 4 条）。

**6. `could not locate a Unreal Engine install`。** 引擎不在标准位置、或是自定义源码构建时，显式指定：

```powershell
& $py $ue --engine "D:\MyEngine\UE_5.6" nodes
# 或者设环境变量 UE_ENGINE_DIR
```

`ue.py` 的定位顺序是：`--engine` → `UE_ENGINE_DIR` / `UNREAL_ENGINE_DIR` / `UE_ROOT` / `UE5_ENGINE` → `.uproject` 的 `EngineAssociation` 查注册表 → 扫描常见安装目录。`doctor` 会打印最终选中的引擎路径，先看那一行。

> ⚠️ 在没确认引擎前，`doctor` 的退出码也是 2。它没有列出节点时，错误信息会明确写是"找不到引擎"还是"没有编辑器应答"，**读那行字**再决定查哪一支。

---

## 退出码 3：超时

`[ue] timed out after Ns.` —— **这不等价于失败，也不等价于取消。**

编辑器里的代码还在游戏线程上继续跑。此时：

1. **不要立刻重试。** 重试只会再排一条命令到同一条被堵住的线程后面。
2. 先看编辑器窗口是否响应。如果还活着、只是慢，等它跑完即可。
3. 如果是真的卡死，见下一节。

预防：把长任务拆成多个短调用；需要进度显示时用 `unreal.ScopedSlowTask`；默认 `--timeout` 是 120 秒，可以显式放宽：

```powershell
& $py $ue --timeout 600 run -f .\long_job.py
```

---

## 编辑器假死

**没有远程手段能救回来。** Python 就卡在游戏线程上，任何新命令也进不去那条线程；`ue.py` 只会一直超时。只能由用户在编辑器窗口里处理。

最常见的元凶，写脚本时逐条避开：

| 元凶 | 症状 |
|---|---|
| `input()` / 读 stdin | 永久卡住，无解 |
| `unreal.EditorDialog`、`MessageDialog`、模态 Slate | 弹窗在等点击，阻塞游戏线程 |
| `while True:` 且没有退出条件 | 编辑器 UI 完全无响应 |
| `time.sleep()` 超过一两秒 | 整个编辑器冻结相应时长 |
| 同步加载/编译大量资产 | 长时间无响应，通常会自己恢复 |
| 循环次数极大且每帧都重绘进度 | 编辑器变卡，通常仍能恢复 |

安全的长任务写法：

```python
import unreal

with unreal.ScopedSlowTask(100, "处理资产") as task:
    task.make_dialog(True)          # True = 可取消
    for i in range(100):
        if task.should_cancel():
            break
        task.enter_progress_frame(1)
        # 每次迭代做一小步
```

---

## 编辑器处于 PIE：一切"空"都是假的

**这是最容易误判的一种状态。** 编辑器在播放（PIE / Simulate）时，一批编辑器 API 不报错，只**静默返回空值**。

实测（编辑器正在 PIE 时）：

```text
get_editor_world()      : None
get_current_level()     : None
is_in_play_in_editor()  : True
get_all_level_actors()  : 0
asset registry alive    : 0
```
```text
LogUtils: Error: The Editor is currently in a play mode.
```

`list_assets("/Game")` 返回 `0`、`get_all_level_actors()` 返回 `0` —— 很容易被当成"项目里真的没有"。

**处置**：任何编辑器态脚本第一件事就是过闸门。

```python
import unreal
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if les.is_in_play_in_editor():
    raise RuntimeError("编辑器正在 PIE；先停止播放再跑编辑器态操作")
```

`raise` 会让 `ue.py` 退出码变成 **1**，不会被误判为成功。

`is_in_play_in_editor()` 在 **Play 和 Simulate 两种模式下都为 `True`**，无法用它区分。
另外 PIE 期间 `get_editor_world()` 仍返回**编辑器世界**，不是 PIE 世界。

---

## 编辑器进程没了 / 日志里出现 Assertion

`ue.py` 突然全部超时、`nodes` 再也发现不到节点 —— 先确认进程还在不在：

```powershell
Get-Process UnrealEditor -ErrorAction SilentlyContinue | Select-Object Id,StartTime
```

进程消失了就去看崩溃现场：

```powershell
$d = Get-ChildItem '<项目>\Saved\Crashes' -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Get-Content "$($d.FullName)\AutoChess.log" -Tail 40
Select-String -Path "$($d.FullName)\CrashContext.runtime-xml" -Pattern 'CrashType|ProcessId|SecondsSinceStart'
```

- 断言来自 **`reload_packages`** → 对"只在内存、磁盘无 `.uasset`"的包调用它必崩（`Blueprint.cpp:786`）。**这是我们自己造的，别再犯了。**
- 日志里有 **`this package is now potentially corrupt`** → 引擎在建议重启编辑器。照做，**不要**用更激进的手段去删那个包。

崩溃后必查两件事：

```powershell
cd <项目>; git status --short      # Content/ 下有没有意外变更
Test-Path '<项目>\Content\<临时目录>'   # 创建过的资产是否落盘
```

**从没保存过的资产，进程一退出就没了** —— 这通常意味着"看似闯了祸，其实磁盘零损失"，但仍要如实报告。

另外提醒用户：未保存的关卡可能有自动保存档，在 `<项目>\Saved\Autosaves\` 与 `PackageRestoreData.json`。
**是否恢复完全由用户决定，不要替用户操作崩溃恢复流程。**

---

## 输出与编码

- `ue.py` 已把自身 stdout/stderr 强制为 UTF-8，中文不会因为控制台代码页报 `UnicodeEncodeError`。
- `run --stdin` 会自己嗅探 BOM / UTF-16 / UTF-8，不依赖控制台代码页。但**上游 shell 可能先把文件读坏了**：Windows PowerShell 5.1 的 `Get-Content` 默认按本地代码页（中文机器上是 GBK）读文件，UTF-8 源码会被它读成乱码，再传进来就救不回来了。

  ```powershell
  Get-Content .\job.py | & $ue run --stdin                 # ✗ 中文会变乱码
  Get-Content .\job.py -Encoding utf8 | & $ue run --stdin  # ✓
  & $ue run -f .\job.py                                    # ✓ 最省事，首选
  ```

  **结论：多行脚本一律用 `run -f`。** `--stdin` 只适合纯 ASCII 的临时片段。
- 编辑器里的 `print()` 和 `unreal.log()` / `unreal.log_warning()` / `unreal.log_error()` 都会被 `ExecuteFile` 模式抓回来，**并且都带 `info` 类型、没有级别前缀** —— 已实测：`log_error` 抓回来也是普通文本，不会被标成 `[error]`。
- 因此**脚本要自己让失败可辨识**：关键结论自己加前缀（例如 `print("[CHECK] 脏包 =", n)`），需要硬失败就直接 `raise`，那才会让 `ue.py` 退出码变成 1。
- 编辑器侧的 Python 建议脚本一律以 UTF-8 保存（`run -f` 按 `utf-8-sig` 读取，带 BOM 也没问题）。中文文件名也已实测可用。
- **不要 print 上千行。** 先 `len()` 再切片。大输出既慢又超出可读范围。

---

## 多个编辑器实例

`nodes` 会把所有在跑的实例列出来。同时开了多个项目时必须显式指定，否则 `ue.py` 会拒绝随机挑一个（这是有意的，避免改错项目）：

```powershell
& $py $ue nodes
& $py $ue --json run -f .\job.py --node AutoChess
& $py $ue --json run -f .\job.py --node 1
```

`--node` 接受：完整 node id、id 前缀、项目名/机器名子串、或 1 起的序号。

---

## `unreal` 模块导入失败

`unreal` 只存在于编辑器进程内。**客户端脚本永远不要 `import unreal`** —— `ue.py` 自己只用标准库。所有 `import unreal` 都必须写在送给编辑器的代码里。

---

## 端口

客户端默认每次挑一个空闲的本地 TCP 端口等编辑器回连（引擎默认的 `6776` 常常被上一次没退干净的进程占住）。所以：

- 不需要手动放行 `6776`；
- 也**不要**为了"省事"把 `--command-port` 固定成某个值，除非你确认它没被占用。

---

## 超时/失败后如何确认编辑器真实状态

如果 `ue.py` 报错但你怀疑命令其实执行了（比如"改了但没保存"），用一条只读命令复核，而不是重跑改动命令：

```powershell
& $py $ue run -c "import unreal; print('editor alive:', unreal.SystemLibrary.get_engine_version())"
```

只要这条能通，编辑器就是活的，可以继续；不通就说明编辑器侧确实卡住或已退出。
