---
name: unreal-python-remote
description: Drive a running Unreal Editor from the agent by executing Python inside it over the Python Editor Script Plugin's remote execution protocol. Use for any UE editor work — 查/改资产、Actor、关卡、蓝图、UMG、DataTable、PIE、跑控制台命令、批量编辑器自动化 — when the editor is open. Needs the editor running with the Python plugin enabled; no restart and nothing to install.
---

# 操控运行中的 Unreal 编辑器

编辑器里跑的 Python 拥有和 C++ 编辑器模块同等的 `unreal` API。本 skill 通过引擎自带的远程执行协议把代码送进去执行，把 `print()` / `unreal.log()` 的输出抓回来。

客户端 `<skill-dir>/scripts/ue.py` **直接加载引擎安装目录里的 `remote_execution.py`**，所以协议永远和引擎一致；它只用 **Python 标准库**，不装任何包，不改项目、不改引擎，任何 Python 3.8+ 都能跑。

## 0. 固定这两个变量

后面的命令都用 `& $py $ue <子命令>` 的形式，先把这两个变量定下来（每个 shell 会话说一次）：

```powershell
$py = 'C:\Users\姚宇翔\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
$ue = '<skill-dir>\scripts\ue.py'
```

`$py` 用 harness 自带的 Python；`load_workspace_dependencies` 返回的就是这个路径，换机器时以它为准。也可以用任何其它 Python 3.8+（已实测 msys64 的 Python 同样可用）。

## 1. 先确认链路

每次会话开始，先跑这一条：

```powershell
& $py $ue doctor
```

正常的输出形如：

```text
python   : 3.12.14  (...)
uproject : E:\UnrealProject\AutoChess\AutoChess.uproject
[ue] engine: E:\UnrealEngine\UE_5.6
  [1] project=AutoChess engine=5.6.1-... machine=... root=E:/UnrealEngine/UE_5.6/Engine/ id=02B20EEA
```

看到 `[1] project=...` 就说明链路通了。**没有列出节点就不要继续猜**，直接看 `references/troubleshooting.md`。

## 2. 调用形态

| 目的 | 命令 |
|---|---|
| 列出所有在跑的编辑器实例 | `& $py $ue nodes` |
| **执行一个脚本文件（首选）** | `& $py $ue run -f <路径>` |
| 执行一段代码 | `& $py $ue run -c "<代码>"` |
| 求值单个表达式并打印结果 | `& $py $ue eval "<表达式>"` |
| 从 stdin 读代码 | `& $py $ue run --stdin` |
| 机器可读输出 | 上面任意命令加 `--json` |
| 同时开了多个编辑器时选一个 | `--node <id 前缀 \| 项目名 \| 序号>` |

退出码：**0** 成功；**1** 编辑器里的代码抛了异常（traceback 在 stdout）；**2** 连不上编辑器或找不到引擎；**3** 超时；**4** 命令用错了（例如 `run` 没给代码来源）。

全局开关（`--json`、`--timeout`、`--engine`、`--connect-timeout`）放在子命令**之前**；`--node` 放在 `run` / `eval` **之后**。`--json` 两种位置都接受。

```powershell
& $py $ue --json nodes                      # ✓
& $py $ue --timeout 600 run -f .\big.py     # ✓
& $py $ue run -f .\job.py --node AutoChess  # ✓
```

## 3. 工作流

1. **自检**：`& $py $ue doctor`。
2. **写脚本**：把要执行的 Python 写进 `<workspace>\Saved\UEAgent\<名字>.py`。这个目录已被 `.gitignore` 的 `Saved/*` 覆盖，不会污染仓库。
   - 用 `run -f` 而不是 `run -c`：多行代码不必和 shell 转义搏斗，中文也不会乱码。
   - 脚本里自己 `import unreal`；`print()` 和 `unreal.log()` 的输出都会被抓回来。
   - 脚本要自包含：每次调用都是一次新的解释器上下文，**上一段代码的变量不会留到下一段**。
3. **执行并读输出**：`& $py $ue run -f <路径>`，看真实 stdout，据此改脚本再跑。
4. **复验**：任何"我改了 X"的结论，都用一条只读命令再读一遍 X 并打印。不要靠"没报错"当作成功。
5. **汇报**：改了什么、验证命令与输出、现在有没有脏包（`unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()`）、是否需要用户手动保存。

## 硬性规则

前三条是**能让整个编辑器假死或被直接杀死**的规则 —— 一旦发生，远程通道回不来，只能由用户在编辑器窗口里处理。
这不是危言耸听：本项目已经因为违反第三条崩过一次编辑器（`Blueprint.cpp:786` 断言，编辑器进程直接退出）。

- **绝不阻塞游戏线程。** 编辑器 Python 在游戏线程上执行。`input()`、`unreal.EditorDialog`、任何模态 Slate 窗口、死循环、长 `time.sleep()` 都会卡死编辑器。
- **绝不调用 `unreal.EditorLoadingAndSavingUtils.reload_packages()`。** 对**磁盘上没有 `.uasset` 的纯内存包**调用它会命中 `Assertion failed: nullptr != GeneratedClass`（`Blueprint.cpp:786`）并**崩溃编辑器**。反过来，日志一旦出现 `this package is now potentially corrupt` 或 `Consider restarting the editor`，那是引擎在叫你**停手并报告用户**，不要换更激进的手段去撬。
- **不要为了验证某个 API 能不能用，就在用户的真实项目里创建、改名、删除任何资产。** 只读探测（`hasattr` / `dir()` / `__doc__` / AssetRegistry 元信息）能回答绝大多数问题。确实需要临时资产时，先向用户要一个沙箱路径并拿到明确同意。
- **先过 PIE 闸门。** 编辑器处于 PIE 时，`get_editor_world()` / `get_current_level()` 返回 `None`，`list_assets` / `get_all_level_actors` 返回 `0`，日志报 `The Editor is currently in a play mode.` —— 这是**静默空值**，不检查就会把"拿不到"当成"真的没有"。编辑器态脚本开头先 `if les.is_in_play_in_editor(): raise ...`。
- **CLI 超时 ≠ 取消。** `--timeout` 只是客户端放弃等待，编辑器里那段代码**还在跑**。超时后不要立刻重试，先确认编辑器是否已经卡住。
- **串行执行。** 同一个编辑器一次只跑一条命令，不要并发发起多个 `ue.py` 进程。
- **改动要能撤销。** 修改资产、关卡或 Actor 属性时，用 `unreal.ScopedEditorTransaction` 包住，否则用户按 Ctrl+Z 撤不掉。改完**立刻读回验证**，能还原就还原。
- **不要替用户保存。** 除非任务明确要求落盘，不要调用 `save_dirty_packages` / `save_asset` / `save_loaded_asset`。改完如实告诉用户"已修改、未保存"。
- **注意：开事务会把关卡标脏，而 Python 侧清不掉关卡的脏标记**（`set_dirty_flag` 对 World / Level / Package 一律返回 `False`）。所以纯只读任务也要避开 spawn / 属性写 / 开事务。
- **不要用弃用 API。** UE 5.6 里 `unreal.EditorLevelLibrary` / `unreal.EditorAssetLibrary` 已弃用，调用会打印 DeprecationWarning。改用
  `unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem / EditorActorSubsystem / EditorAssetSubsystem / LevelEditorSubsystem)`。
- **输出要克制。** 不要 `print` 上千个资产。先 `len()` 看规模，再切片打印前 N 个；大输出既慢又难读。
- **API 名字先探测再用。** 不同引擎版本差异很大，用 `hasattr` / `dir()` / `__doc__` 确认，别凭记忆写。已实测**不存在**的有：`unreal.KismetSystemLibrary`、`unreal.LogLevel`、`LevelEditorSubsystem.get_selected_actors`、`EditorActorSubsystem.get_all_level_actors_of_class`、`EditorAssetSubsystem.get_asset_class`、`LevelEditorSubsystem.editor_set_game_paused`。完整清单见 `references/recipes.md` §14。
- **改动类操作尽量限定作用域。** 批量重命名/删除资产前，先把待改清单打印出来让用户确认，再执行。

## 配方与排障

- **`references/recipes.md`** — 各类操作的可复制代码（环境自检、资产只读、Actor、选择、属性读写、Transaction、spawn/destroy、DataTable、蓝图/UMG、PIE 启停、控制台命令、截图、进度条）。§8 明确记录了**蓝图变量表与 UMG widget tree 在 Python 侧读不到**，不要为它们编造写法。
- **`references/troubleshooting.md`** — 连不上编辑器、超时、卡死、PIE 静默空值、编辑器崩溃后的取证、协议端口、多实例、编码问题。

## 维护这个 skill

**遇到本 skill 尚未覆盖的新经验或新事故，就地补进来，不要另建文件。**

- 新的**危险调用**或踩坑 → 加进上面「硬性规则」，并在 `references/recipes.md` 的对应配方里留一段反面记录（连同真实报错）。
- 新的**可行配方** → 加进 `references/recipes.md`，附上真实抓回的 stdout；跑不通的不要写，或明确标注未实测。
- 新的**排查路径**（某个错误码怎么定位）→ 加进 `references/troubleshooting.md`。

判断标准很简单：**写下一条能让下一个 agent 少踩一次坑的信息**，而不是记录这次发生了什么。

## 收尾

任务结束时至少报告一次脏包状态，让用户决定是否保存：

```python
import unreal
dirty_content = unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()
dirty_maps = unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages()
print("脏资产:", len(dirty_content), "脏关卡:", len(dirty_maps))
```
