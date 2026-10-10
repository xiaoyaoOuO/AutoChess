# 配方手册（recipes）

验证环境：引擎 `5.6.1-44394996+++UE5+Release-5.6`，项目 `E:\UnrealProject\AutoChess\AutoChess.uproject`，
验证日期 **2026-10-10**，验证时编辑器打开的是未保存的模板关卡 `/Temp/Untitled_1`，`/Game` 下 496 个资产。

> ⚠️ **先说清楚代价：验证过程中编辑器被搞崩过一次**（§13d）。
> §1–§12 的代码与输出都是在运行中的 UE 5.6 里真实跑出来的，包括 §9 的 PIE 启停与 §11 的截图落盘。
> §8 的蓝图 / UMG 部分**确认了相关 API 根本不存在**，因此不提供配方，只留反面记录。
> **照抄本文任何写入类代码之前，先读 §13。**

调用形态（下文不再重复）：

```powershell
$py = 'C:\Users\姚宇翔\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
$ue = '<skill-dir>\scripts\ue.py'
& $py $ue run -f 'E:\UnrealProject\AutoChess\Saved\UEAgent\probe.py'   # 首选
& $py $ue run -c "print(1)"
& $py $ue eval "unreal.SystemLibrary.get_engine_version()"
```

每次 `run` 都是一次独立的解释器上下文，**上一段的变量不会留到下一段**，所以每块代码都要自包含。

关于 `run -f` / `-c` 的一个重要实现细节：远程通道**不发送源码文本，发送的是文件路径**，由编辑器自己去读文件。
原因是引擎的 `ExecuteFile` 模式会扫描命令里的字面量 `.py`，把 `.py` 之前的**全部内容当成文件路径**
（`PythonScriptPlugin.cpp` 的 `TryExtractPathnameAndCommand`，只作用于 `ExecuteFile`）。
代码里随便一句 `"# 见 job.py"` 就能让整段脚本变成 `Could not load Python file ...`。
`ue.py` 因此对 `-c` / `--stdin` 的代码先落盘到 `%TEMP%\ue_remote_agent\` 再传路径；`run -f` 直接传你的文件路径。
副作用是好的：**traceback 里是真实文件名和行号**，不再是 `<string>` 第 1 行。

---

## 1. 环境自检

开局先确认引擎版本、项目路径、当前关卡、脏包。脏包必须在动手前后各看一次。

```python
import unreal

ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
world = ues.get_editor_world()

print("engine_version :", unreal.SystemLibrary.get_engine_version())
print("project_dir    :", unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()))
print("content_dir    :", unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_content_dir()))
print("world          :", world.get_path_name())
print("current_level  :", les.get_current_level().get_path_name())
print("is_in_pie      :", les.is_in_play_in_editor())

dirty_maps = unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages()
dirty_content = unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()
print("dirty_maps     :", len(dirty_maps), [p.get_name() for p in dirty_maps])
print("dirty_content  :", len(dirty_content), [p.get_name() for p in dirty_content])
```

> 实测输出
> ```text
> engine_version : 5.6.1-44394996+++UE5+Release-5.6
> project_dir    : E:/UnrealProject/AutoChess/
> content_dir    : E:/UnrealProject/AutoChess/Content/
> world          : /Temp/Untitled_1.Untitled_1
> current_level  : /Temp/Untitled_1.Untitled_1:PersistentLevel
> is_in_pie      : False
> dirty_maps     : 0 []
> dirty_content  : 0 []
> ```

注意点：

- 关卡路径前缀是 `/Temp/` 时说明**当前打开的是从未保存过的临时关卡**（`Untitled_N`）。对这种关卡做任何 spawn / 属性写都会把它标脏，而它无法用 Python 清除脏标记（见 §13）。
- `unreal.Paths.get_project_file_path()` 返回的是 `../../../../../UnrealProject/AutoChess/AutoChess.uproject` 这种相对路径，别拿它当绝对路径用；要绝对路径就用 `convert_relative_path_to_full(unreal.Paths.project_dir())`。

---

## 2. 资产只读：列出、判存在、加载、查类、查依赖

```python
import unreal
eas = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)

# 2a 列出资产（返回的是 "/Game/A/B.B" 这种带点号的完整对象路径）
all_assets = eas.list_assets("/Game", recursive=True, include_folder=False)
print("total:", len(all_assets))
print(all_assets[:3])
print("subfolder:", eas.list_assets("/Game/Blueprints/Weapon", recursive=True, include_folder=False))

# 2b 判断存在（带不带 ".AssetName" 后缀都认）
print(eas.does_asset_exist("/Game/Blueprints/Weapon/WeaponActor"))          # True
print(eas.does_asset_exist("/Game/Blueprints/Weapon/WeaponActor.WeaponActor"))  # True
print(eas.does_asset_exist("/Game/Nope/DoesNotExist"))                      # False
print(eas.do_assets_exist(["/Game/Blueprints/Weapon/WeaponActor"]))         # True（全部存在才 True）

# 2c 查资产类：EditorAssetSubsystem 上没有 get_asset_class，走 AssetData
ad = eas.find_asset_data("/Game/Blueprints/Weapon/WeaponActor")
print(ad.asset_class_path.asset_name)   # Blueprint
print(ad.asset_name, ad.package_name, ad.package_path)
print(unreal.AssetRegistryHelpers.find_asset_native_class(ad))
print("is_redirector:", unreal.AssetRegistryHelpers.is_redirector(ad))

# 2d 加载并读属性（用真实存在的 StaticMesh 举例）
mesh = eas.load_asset("/Game/Assets/Weapons/Enemy_Gruntling_Weapons/Meshes/SM_Gruntling_Torch_Internal")
print(mesh.get_name(), mesh.get_num_lods(), mesh.get_bounds())
print(mesh.get_editor_property("static_materials"))

# 2e 查依赖（正向）与反向依赖
ar = unreal.AssetRegistryHelpers.get_asset_registry()
opts = unreal.AssetRegistryDependencyOptions()
print(ar.get_dependencies("/Game/Blueprints/Weapon/WeaponActor", opts))
print(eas.find_package_referencers_for_asset("/Game/Blueprints/Weapon/WeaponActor"))
```

> 实测输出
> ```text
> total: 496
> ['/Game/Blueprints/AnimNotifies/AN_MaterialEffect.AN_MaterialEffect', ...]
> subfolder: ['/Game/Blueprints/Weapon/BP_Weapon_HellHammer.BP_Weapon_HellHammer', ...]
> True
> True
> False
> True
> Blueprint
> WeaponActor /Game/Blueprints/Weapon/WeaponActor /Game/Blueprints/Weapon
> <Object '/Script/Engine.Blueprint' (0x00007FF404C94C08) Class 'Class'>
> is_redirector: False
> SM_Gruntling_Torch_Internal 1 <Struct 'BoxSphereBounds' ... {origin: {x: 0.539844, ...}, sphere_radius: 42.260448}>
> [{material_interface: "/Script/Engine.Material'/Game/Assets/.../M_Gruntling_weapons.M_Gruntling_weapons'", material_slot_name: "M_Gruntling_weapons", ...}]
> ["/Script/GameplayTags", "/Script/NavigationSystem", "/Script/GameplayAbilities", "/Script/ClothingSystemRuntimeNv", "/Engine/EditorBlueprintResources/StandardMacros", "/Game/Assets/Sounds/UI/A_UI_WaveEnd"]
> ["/Game/Blueprints/AnimNotifies/WeaponAttackNS", "/Game/Blueprints/Weapon/GuardianWeaponActor", "/Game/Blueprints/Weapon/BP_Weapon_HellHammer", "/Game/Blueprints/Weapon/Spider/BP_WeaponSpider", "/Game/Blueprints/Weapon/Goblin/GoblinWeapon_Base"]
> ```

注意点：

- **`eas.list_assets("/Game", recursive=False)` 返回 0**。这个项目 `/Game` 根目录下没有资产，全在子目录里；`recursive=False` 只看一层，所以拿到空列表不代表资产不存在。
- `EditorAssetSubsystem` **没有 `get_asset_class`**（实测 `hasattr(eas, "get_asset_class") == False`）。查类只能走 `find_asset_data(path).asset_class_path.asset_name`。
- `list_assets` 返回的字符串是 `包路径.对象名`（如 `/Game/A/B.B`）。判断存在、加载、查类都吃这个格式；传包路径（`/Game/A/B`）也行。
- `ar.get_dependencies(package_name, options)` 的 `package_name` 是**包路径**（不带 `.AssetName`），返回 `Array[Name]`，且只反映**磁盘上的引用**。
- 输出中 `asset_class_path.asset_name` 是 `Name` 类型，`print` 出来是 `Blueprint` 这样；做字符串比较用 `str(...)`。

---

## 3. 关卡 Actor：列出、按类过滤、计数

```python
import unreal
eacs = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

actors = eacs.get_all_level_actors()
print("total:", len(actors))

# 按类过滤：没有 get_all_level_actors_of_class，用 EditorFilterLibrary
meshes = unreal.EditorFilterLibrary.by_class(actors, unreal.StaticMeshActor)
lights = unreal.EditorFilterLibrary.by_class(actors, unreal.DirectionalLight)
print("StaticMeshActor:", len(meshes), [a.get_actor_label() for a in meshes])
print("DirectionalLight :", len(lights))

# isinstance 同样可行，纯 Python 侧过滤
print("isinstance count :", len([a for a in actors if isinstance(a, unreal.StaticMeshActor)]))

# 类名直方图
from collections import Counter
print(Counter(a.get_class().get_name() for a in actors).most_common(5))

# 常用只读取值
a = meshes[0]
print(a.get_actor_label(), a.get_actor_location(), a.get_actor_rotation(), a.get_actor_scale3d())
print(a.get_actor_transform())
print(a.get_actor_bounds(False))
print(a.get_component_by_class(unreal.StaticMeshComponent))
```

> 实测输出
> ```text
> total: 138
> StaticMeshActor: 1 ['SM_SkySphere']
> DirectionalLight : 1
> isinstance count : 1
> [('LandscapeStreamingProxy', 64), ('WorldPartitionHLOD', 64), ('WorldDataLayers', 1), ('WorldPartitionMiniMap', 1), ('DirectionalLight', 1)]
> SM_SkySphere <Struct 'Vector' ... {x: 0.000000, y: 0.000000, z: 0.000000}> <Struct 'Rotator' ... {pitch: 0.000000, yaw: 0.000000, roll: 0.000000}> <Struct 'Vector' ... {x: 400.000000, y: 400.000000, z: 400.000000}>
> (<Struct 'Vector' ...>, <Struct 'Vector' ... {x: 1638400.000000, ...}>)
> ```

注意点：

- **`EditorActorSubsystem.get_all_level_actors_of_class()` 不存在**（实测 `AttributeError`）。按类过滤用 `unreal.EditorFilterLibrary.by_class(actors, cls)`，签名是 `by_class(target_array, object_class, filter_type=EditorScriptingFilterType.INCLUDE) -> Array[Object]`。
- `EditorFilterLibrary` 还有 `by_actor_label` / `by_actor_tag` / `by_layer` / `by_level_name` / `by_selection`，都是现成的过滤入口。
- 这个关卡是 World Partition 关卡，大量 Actor 属于外部 Actor 包（`__ExternalActors__`），`a.get_outermost()` 不是地图包。

---

## 4. 选择：读当前选中、设置选中

```python
import unreal
eacs = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

# 读
sel = eacs.get_selected_level_actors()
print("selected:", len(sel), [a.get_actor_label() for a in sel])

# 写：整体设置
targets = unreal.EditorFilterLibrary.by_class(eacs.get_all_level_actors(), unreal.DirectionalLight)
eacs.set_selected_level_actors(targets)
print("after set:", [a.get_actor_label() for a in eacs.get_selected_level_actors()])

# 单个增删 / 清空
eacs.set_actor_selection_state(targets[0], False)
print("after deselect:", len(eacs.get_selected_level_actors()))
eacs.select_nothing()
print("after select_nothing:", len(eacs.get_selected_level_actors()))
```

> 实测输出
> ```text
> selected: 0 []
> after set: ['DirectionalLight']
> after deselect: 0
> after select_nothing: 0
> ```

注意点：

- **`LevelEditorSubsystem.get_selected_actors()` 不存在**（实测 `hasattr == False`）。读选中只能用 `EditorActorSubsystem.get_selected_level_actors()`。
- `EditorActorSubsystem` 还有 `clear_actor_selection_set()` / `invert_selection(world)` / `select_all(world)` / `select_all_children(recurse_children)` / `duplicate_selected_actors(world)` / `delete_selected_actors(world)`。
- 改选中不改资产，但 `set_selected_level_actors` 是编辑器 UI 状态，会影响用户当前看到的画面，收尾时用 `select_nothing()` 还原。

---

## 5. 属性读写（包在 ScopedEditorTransaction 里以便撤销）

```python
import unreal
eacs = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

actor = unreal.EditorFilterLibrary.by_class(
    eacs.get_all_level_actors(), unreal.StaticMeshActor)[0]

# 读：普通 UPROPERTY 走 get_editor_property
print("tags   :", list(actor.get_editor_property("tags")))
print("guid   :", actor.get_editor_property("actor_guid"))
print("repl   :", actor.get_editor_property("replicates"))

# 写：必须包 Transaction，否则用户 Ctrl+Z 撤不掉
with unreal.ScopedEditorTransaction("ue-skill: set tags") as tx:
    actor.set_editor_property("tags", ["ue_skill_demo"])
print("tags after:", list(actor.get_editor_property("tags")))

# 还原
with unreal.ScopedEditorTransaction("ue-skill: restore tags") as tx2:
    actor.set_editor_property("tags", [])

# 子组件属性：Actor 上的 mobility 读不到，必须在组件上读写
smc = actor.get_editor_property("static_mesh_component")
print("mobility:", smc.get_editor_property("mobility"))          # ComponentMobility.STATIC
old = smc.get_editor_property("cast_shadow")
with unreal.ScopedEditorTransaction("ue-skill: set cast_shadow") as tx3:
    smc.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    smc.set_editor_property("cast_shadow", not old)
print("mobility after:", smc.get_editor_property("mobility"))
# ... 收尾时把 cast_shadow / mobility 写回原值
```

> 实测输出
> ```text
> tags   : []
> guid   : <Struct 'Guid' (0x000002B5ED949850) {}> 
> repl   : False
> mobility: <ComponentMobility.STATIC: 0>
> mobility after: <ComponentMobility.MOVABLE: 2>
> ```

注意点：

- **`actor_label` 不是 editor property**。`actor.get_editor_property("actor_label")` 抛
  `Exception: StaticMeshActor: Failed to find property 'actor_label'`。标签要用专用 API：`get_actor_label()` / `set_actor_label("Name")`。
  同理 `actor_hidden` / `is_editor_only` / `mobility` 在 Actor 上都不存在。
- 属性名是**蛇形小写**的 C++ 属性名（`Tags` → `tags`，`Replicates` → `replicates`），且只有 `[Read-Write]` 的才可写。
- `unreal.ScopedEditorTransaction` 支持 `with`，构造参数是显示在 Undo 历史里的事务名。同类里还有 `cancel()`（在 `with` 块内调用可取消整个事务）。
- **开事务会把当前关卡标脏**，即使你随后把值改回去了。脏的是关卡包，不是内容包。
- 改真实关卡 Actor 的属性前，先把原值读出来存着，收尾写回；不确定就只在临时 Actor 上演示。

---

## 6. Actor 的 spawn / destroy（同样用 Transaction）

```python
import unreal
eacs = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

before = len(eacs.get_all_level_actors())
print("actor_count before:", before)
spawned = []
try:
    with unreal.ScopedEditorTransaction("ue-skill: spawn scratch actors") as tx:
        light = eacs.spawn_actor_from_class(
            unreal.PointLight, unreal.Vector(0, 0, 500), unreal.Rotator(0, 0, 0), transient=False)
        mesh = eacs.spawn_actor_from_class(
            unreal.StaticMeshActor, unreal.Vector(200, 0, 500), unreal.Rotator(0, 0, 0), transient=False)
    spawned = [light, mesh]
    print("spawned:", [(a.get_name(), a.get_actor_label()) for a in spawned])
    print("actor_count now:", len(eacs.get_all_level_actors()))
finally:
    with unreal.ScopedEditorTransaction("ue-skill: destroy scratch actors") as tx2:
        ok = eacs.destroy_actors(spawned)
    print("destroy_actors ->", ok)

print("actor_count after:", len(eacs.get_all_level_actors()), "== before:", len(eacs.get_all_level_actors()) == before)
for a in spawned:
    print(a.get_name(), "is_valid:", unreal.SystemLibrary.is_valid(a))
```

> 实测输出
> ```text
> actor_count before: 138
> spawned: [('PointLight_UAID_08BFB8C570E3D60903_1358056999', 'PointLight'), ('StaticMeshActor_0', 'StaticMeshActor')]
> actor_count now: 138
> destroy_actors -> True
> actor_count after: 138 == before: True
> StaticMeshActor_1 is_valid: False
> ```

注意点：

- `spawn_actor_from_class(actor_class, location, rotation=[0,0,0], transient=False) -> Actor`；
  从已有对象复制用 `spawn_actor_from_object(object_to_use, location, rotation, transient=False)`。
- **`transient=True` 生成的 Actor 不出现在 `get_all_level_actors()` 里**（实测 `in` 判定为 `False`，但它的 outer 确实是 `PersistentLevel`）。所以**不能用"数量回到 before"来证明清理干净**——要用 `unreal.SystemLibrary.is_valid(actor)` 逐个确认已失效。
- `destroy_actor(a) -> bool`、`destroy_actors(list) -> bool`。重复销毁返回 `False` 而不抛异常，`destroy_actors([])` 返回 `True`，都安全。
- 这里用 `transient=False` 是为了让 Actor 真正进关卡、数量可见（138→139→138），验证更有说服力；代价是关卡被标脏。**只在自己的临时 Actor 上做，做完立刻销毁。**
- 相关 API：`duplicate_actor` / `duplicate_actors(list, to_world, offset)` / `set_actor_transform(actor, transform)` / `convert_actors(...)`。

---

## 7. DataTable 读取

项目 `/Game` 下**没有任何 DataTable 资产**（实测 496 个资产里 `DataTable` 类型数量为 0；设计表是 `doc\tables\*.csv`，没导进引擎）。
所以本节的 DataTable 是我在 `/Game/__ue_skill_scratch/` 里临时用 `DataTableFactory` 建出来、读完立刻删掉的，
读法对真实的 DataTable 完全一致。先探测再读：

```python
import unreal
eas = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)

# 先找出项目里真实存在的 DataTable
tables = [p for p in eas.list_assets("/Game", recursive=True, include_folder=False)
          if str(eas.find_asset_data(p).asset_class_path.asset_name) == "DataTable"]
print("DataTable assets:", tables)

# 读一张表
dt = eas.load_asset(tables[0])
print("row_struct :", unreal.DataTableFunctionLibrary.get_data_table_row_struct(dt))
print("rows       :", [str(n) for n in unreal.DataTableFunctionLibrary.get_data_table_row_names(dt)])
print("rows (alt) :", [str(n) for n in dt.get_row_names()])
print("columns    :", [str(n) for n in unreal.DataTableFunctionLibrary.get_data_table_column_names(dt)])

# 取列值（Python 侧读单元格的唯一现成入口）
for col in unreal.DataTableFunctionLibrary.get_data_table_column_export_names(dt):
    print(col, "->", list(unreal.DataTableFunctionLibrary.get_data_table_column_as_string(dt, str(col))))

# 判行存在
print(unreal.DataTableFunctionLibrary.does_data_table_row_exist(dt, "RowA"))
print(dt.does_row_exist("RowA"))

# 整表导出成字符串（不落盘）
print(unreal.DataTableFunctionLibrary.export_data_table_to_csv_string(dt))
print(unreal.DataTableFunctionLibrary.export_data_table_to_json_string(dt))
```

> 实测输出（表是我临时建的，`SpawnGroupStruct` 是项目里真实的 `UserDefinedStruct`）
> ```text
> row_struct : <Object '/Game/Blueprints/Progression/SpawnGroupStruct.SpawnGroupStruct' Class 'UserDefinedStruct'>
> rows       : ['RowA']
> rows (alt) : ['RowA']
> columns    : ['Enemies_4_CB340442446B7C7A46FCB38A61B2B03C']
> Enemies -> ['']
> True
> True
> '---,Enemies\nRowA,""\n'
> '[\r\n\t{\r\n\t\t"Name": "RowA",\r\n\t\t"Enemies": []\r\n\t}\r\n]'
> ```

写入（仅当确实需要，且只写临时表）用 CSV 字符串：

```python
csv_text = "---,Enemies\nRowA,RowB\n"     # 第一列必须是行名列，表头写 "---"
print(unreal.DataTableFunctionLibrary.fill_data_table_from_csv_string(dt, csv_text))  # True
print([str(n) for n in dt.get_row_names()])   # ['RowA']
```

注意点：

- **CSV 第一列的列头必须是 `---`**，它承载行名。实测用 `"Enemies\nRowA"`（没有 `---`）时 `fill_data_table_from_csv_string` 照样返回 `True`，但表里一行都没进去 —— 返回值不可信，必须回读 `get_row_names()` 验证。
- **列名有两套**：`get_data_table_column_names()` 给的是内部改名后的名字（`Enemies_4_CB340442446B7C7A46FCB38A61B2B03C`），
  `get_data_table_column_export_names()` 给的才是 CSV/JSON 里的友好名（`Enemies`）。**读列值、写 CSV 一律用 export names。**
- UE 5.6 的 Python 里**没有直接取"某行某列的值"的函数**（没有 `get_data_table_row` 之类）。可行路径只有两条：
  `get_data_table_column_as_string(dt, "列名")` 按列整取，或 `export_data_table_to_json_string(dt)` 整表导出后自己解析 JSON。
- `export_data_table_to_csv_file` / `to_json_file` / `fill_from_*_file` 会**写/读磁盘**，需要落盘时才用；纯读取用 `*_string` 版本。
- `unreal.DataTable` 实例上也挂了同名方法（`dt.get_row_names()` / `dt.get_column_export_names()` / `dt.export_to_csv_string()`），与函数库版本等价。

---

## 8. 蓝图 / UMG 资产结构检查

### 8a 蓝图：生成类、父类、图表

```python
import unreal
eas = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
bp_path = "/Game/Blueprints/Weapon/WeaponActor"
bp = eas.load_asset(bp_path)

# generated_class 是方法，不是属性
print("generated:", bp.generated_class())
print("generated name:", bp.generated_class().get_name())
print("lib version   :", unreal.BlueprintEditorLibrary.generated_class(bp))
print("load_blueprint_class:", eas.load_blueprint_class(bp_path))

# 父类 / 类型 / 组件数：从 AssetRegistry tag 拿（Blueprint 对象上没有这些 editor property）
for tag in ("ParentClass", "GeneratedClass", "BlueprintType", "BlueprintComponents",
            "NativeComponents", "IsDataOnly", "NumReplicatedProperties"):
    print("%-24s %s" % (tag, unreal.AssetRegistryHelpers.get_tag_value(eas.find_asset_data(bp_path), tag)))

# 图表
print("event graph:", unreal.BlueprintEditorLibrary.find_event_graph(bp))
print("by name    :", unreal.BlueprintEditorLibrary.find_graph(bp, "EventGraph"))
```

> 实测输出
> ```text
> generated: <Object '/Game/Blueprints/Weapon/WeaponActor.WeaponActor_C' Class 'BlueprintGeneratedClass'>
> generated name: WeaponActor_C
> lib version   : <Object '/Game/Blueprints/Weapon/WeaponActor.WeaponActor_C' Class 'BlueprintGeneratedClass'>
> load_blueprint_class: <Object '/Game/Blueprints/Weapon/WeaponActor.WeaponActor_C' Class 'BlueprintGeneratedClass'>
> ParentClass              /Script/CoreUObject.Class'/Script/Engine.Actor'
> GeneratedClass           /Script/Engine.BlueprintGeneratedClass'/Game/Blueprints/Weapon/WeaponActor.WeaponActor_C'
> BlueprintType            BPTYPE_Normal
> BlueprintComponents      3
> NativeComponents         0
> IsDataOnly               False
> NumReplicatedProperties  0
> event graph: <Object '/Game/Blueprints/Weapon/WeaponActor.WeaponActor:EventGraph' Class 'EdGraph'>
> by name    : <Object '/Game/Blueprints/Weapon/WeaponActor.WeaponActor:EventGraph' Class 'EdGraph'>
> ```

### 8b 蓝图：变量与函数 —— Python 侧读不到

```python
import unreal
eas = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
bp = eas.load_asset("/Game/Blueprints/Weapon/WeaponActor")

# 以下全部失败，留作反面记录
for p in ("new_variables", "function_graphs", "ubergraph_pages", "parent_class",
          "blueprint_type", "macro_graphs", "interfaces"):
    try:
        print(p, "->", bp.get_editor_property(p))
    except Exception as e:
        print(p, "FAILED:", e)
```

> 实测输出
> ```text
> new_variables FAILED: Blueprint: Failed to find property 'new_variables' for attribute 'new_variables' on 'Blueprint'
> function_graphs FAILED: Blueprint: Failed to find property 'function_graphs' for attribute 'function_graphs' on 'Blueprint'
> ubergraph_pages FAILED: Blueprint: Failed to find property 'ubergraph_pages' for attribute 'ubergraph_pages' on 'Blueprint'
> parent_class FAILED: Blueprint: Failed to find property 'parent_class' for attribute 'parent_class' on 'Blueprint'
> blueprint_type FAILED: Blueprint: Failed to find property 'blueprint_type' for attribute 'blueprint_type' on 'Blueprint'
> status FAILED: Blueprint: Property 'Status' for attribute 'status' on 'Blueprint' is protected and cannot be read
> macro_graphs FAILED: Blueprint: Failed to find property 'macro_graphs' for attribute 'macro_graphs' on 'Blueprint'
> interfaces FAILED: Blueprint: Failed to find property 'interfaces' for attribute 'interfaces' on 'Blueprint'
> ```

**结论（未验证项，明确记录）：UE 5.6 的 Python API 无法枚举蓝图的变量表和函数列表。**
`unreal.Blueprint` 暴露的非下划线成员只有 `generated_class` 和 `set_blueprint_variable_expose_*` 三个写接口：

```text
dir(bp) 非下划线成员:
['generated_class', 'get_class', 'get_default_object', 'get_editor_property', 'get_fname',
 'get_full_name', 'get_name', 'get_outer', 'get_outermost', 'get_package', 'get_path_name',
 'get_typed_outer', 'get_world', 'is_editor_property_overridden', 'is_package_external',
 'modify', 'rename', 'reset_editor_property', 'set_blueprint_variable_expose_on_spawn',
 'set_blueprint_variable_expose_to_cinematics', 'set_blueprint_variable_instance_editable',
 'set_editor_properties', 'set_editor_property', 'static_class']
```

我试过并失败的替代路径：`bp.generated_class()` 的 CDO 上做 `dir()` 差集（差集为空）、
`unreal.get_default_object(gc)` 后按属性名取（`AttributeError: 'Actor' object has no attribute 'scratch_int'`）。
蓝图变量对 Python 不可见是因为生成类的 Python 类型被映射成了最近的已知父类（这里是 `Actor`），变量没有对应的 Python 描述符。

**能用的替代**：想知道某个图表在不在，用 `find_graph(bp, "名字")`（存在返回 `EdGraph`，不存在返回 `None`）；
想要父类/组件数等信息，读 §8a 的 AssetRegistry tag。要真正枚举变量，只能让用户看蓝图编辑器，或写 C++/编辑器工具蓝图。

### 8c UMG：widget tree 读不到

```python
import unreal
eas = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
wbp = eas.load_asset("/Game/UI/SomeWidget")   # 本项目 /Game 下 WidgetBlueprint 数量为 0
```

> 实测输出（对临时建的 WidgetBlueprint）
> ```text
> widget_tree !! Exception: WidgetBlueprint: Failed to find property 'widget_tree' for attribute 'widget_tree' on 'WidgetBlueprint'
> hasattr(unreal, "WidgetTree")                 -> False
> hasattr(unreal, "WidgetBlueprintEditorLibrary") -> False
> hasattr(unreal, "WidgetBlueprintLibrary")     -> False
> hasattr(unreal, "UMGEditorSubsystem")         -> False
> ```

**结论：UE 5.6 的 Python API 无法遍历 UMG widget tree。** `UWidgetTree` 类根本没有暴露给 Python，
`WidgetBlueprint` 上也没有 `widget_tree` editor property。`unreal.WidgetBlueprint` 暴露的成员与 `Blueprint` 基本相同。
**本节不提供 UMG 结构检查配方**——不要在 skill 里给出无法实现的写法。

顺带记录一个真实坑：`unreal.WidgetBlueprintFactory` **没有 `parent_class` 属性**（实测 `AttributeError`），
`dir()` 里只有 `supported_class` / `context_class` / `create_new` / `edit_after_new` 等 `Factory` 基类字段，
所以无法从 Python 指定创建的 WBP 的父类。

---

## 9. PIE

查询：

```python
import unreal
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
print("in PIE:", les.is_in_play_in_editor())
```

启停必须**拆成两次独立调用**：`request_` 是排到下一帧执行，在同一次脚本里等它生效必然死等（脚本自己占着游戏线程，PIE 根本没机会开始）。

```python
# 第 1 次调用：请求开始
import unreal
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
print("before:", les.is_in_play_in_editor())
les.editor_request_begin_play()
print("requested; in PIE now:", les.is_in_play_in_editor())   # 仍是 False
```

```python
# 隔一次独立调用：确认已经进 PIE
import unreal
print("in PIE:", unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).is_in_play_in_editor())
```

```python
# 停止
import unreal
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
les.editor_request_end_play()
print("requested; in PIE now:", les.is_in_play_in_editor())   # 仍是 True
```

> 实测输出
> ```text
> # editor_request_begin_play 之前
> world before    : /Temp/Untitled_1.Untitled_1
> current_level   : /Temp/Untitled_1.Untitled_1:PersistentLevel
> is_in_play      : False
> viewport keys   : ['FourPanes2x2.Viewport 1.Viewport0', 'FourPanes2x2.Viewport 1.Viewport1', 'FourPanes2x2.Viewport 1.Viewport2', 'FourPanes2x2.Viewport 1.Viewport3']
> active viewport : FourPanes2x2.Viewport 1.Viewport1
> pilot actor     : None
> has editor_set_game_paused   : False
>
> requesting begin play ...
> requested
> is_in_play immediately after request: False (request is deferred to next frame)
>
> # 一次独立的后续调用（约 15 秒后）
> [result] True
>
> # 停止时
> in PIE: True
> end play requested; in PIE now: True
> # 再下一次独立调用
> [result] False
> ```

`LevelEditorSubsystem` 上实测存在的方法（`dir()` 过滤 play/pie/simulate/pilot/pause）：

```text
editor_play_simulate          x.editor_play_simulate() -> None
editor_request_begin_play     x.editor_request_begin_play() -> None
editor_request_end_play       x.editor_request_end_play() -> None
is_in_play_in_editor          x.is_in_play_in_editor() -> bool
pilot_level_actor             x.pilot_level_actor(actor_to_pilot, viewport_config_key="None") -> None
eject_pilot_level_actor       x.eject_pilot_level_actor(viewport_config_key="None") -> None
get_pilot_level_actor         x.get_pilot_level_actor(viewport_config_key="None") -> Actor
get_active_viewport_config_key x.get_active_viewport_config_key() -> Name
get_viewport_config_keys      x.get_viewport_config_keys() -> Array[Name]
editor_set_viewport_realtime  x.editor_set_viewport_realtime(realtime, viewport_config_key="None") -> None
editor_invalidate_viewports   x.editor_invalidate_viewports() -> None
```

注意点：

- **PIE 运行时 `UnrealEditorSubsystem.get_editor_world()` 返回 `None`。** 实测在 PIE 中调用拿到 `NoneType`，
  紧接着 `.get_path_name()` 就 `AttributeError`，整个脚本中断（脚本挂掉，编辑器本身没事）。
  任何"先取世界再动手"的脚本都必须先问 `is_in_play_in_editor()`，并且不要在 PIE 里假定 editor world 可用。
- `editor_request_begin_play()` / `editor_request_end_play()` 都是**排队到下一帧**生效，
  调用后立刻读 `is_in_play_in_editor()` 拿到的还是旧值。要确认状态就隔一次独立调用再读。
- **不要在一次远程脚本里 sleep / 轮询等 PIE 起来**：脚本占着游戏线程，PIE 永远不会开始。
- **`LevelEditorSubsystem.editor_set_game_paused` 不存在**（实测 `hasattr == False`）。暂停游戏要另找路子，别凭记忆写。
- `editor_play_simulate()` 本次只确认了方法存在与签名，**没有实际调用**（未实测其行为差异）。
- `get_viewport_config_keys()` 返回关卡编辑器的视口布局键（如 `FourPanes2x2.Viewport 1.Viewport1`）；
  `pilot_level_actor` / `editor_set_viewport_realtime` 的 `viewport_config_key` 参数就用这些键。实测无 pilot 时 `get_pilot_level_actor()` 返回 `None`。

---

## 10. 控制台命令

真实签名（实测从 `__doc__` 读出，`unreal.LogLevel` 确实不存在，不需要它）：

```text
X.execute_console_command(world_context_object, command, specific_player=None) -> None
```

```python
import unreal
ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
world = ues.get_editor_world()

print("in PIE:", les.is_in_play_in_editor())
unreal.SystemLibrary.execute_console_command(world, "stat none")
unreal.SystemLibrary.execute_console_command(world, "log LogTemp ue-skill-console-ok")
print("issued")
```

> 实测输出
> ```text
> 'stat none' OK
> issued 'log LogTemp ue-skill-console-ok' (look for it in output)
> ```

注意点：

- 第一个参数是 **world context object**，不是字符串。编辑态传 `UnrealEditorSubsystem.get_editor_world()`。
- 第三个参数 `specific_player` 只在 PIE/游戏里指定 PlayerController 时有用；编辑态省略即可（不能传位置参数占位，用关键字或干脆不传）。
- **返回值是 `None`，没有回显。** 命令是否生效要去 `Saved/Logs/AutoChess.log` 里看。要拿到可回读的效果，优先用 Python API 而不是控制台命令。
- 命令在**编辑器世界**里执行：`stat` / `log` / `OBJ GC` / `OBJ CLEANUP` 这类编辑器命令能用（实测均正常返回），但游戏玩法的 `cheat`/`slomo` 类命令在非 PIE 下没有意义。
- 千万别发 `quit` / `exit` —— 会直接关掉用户的编辑器。

---

## 11. 截图到磁盘

主路径（实测可用，**编辑态和 PIE 下都能出图**）：

```python
import unreal
unreal.AutomationLibrary.finish_loading_before_screenshot()
task = unreal.AutomationLibrary.take_high_res_screenshot(1280, 720, "ue_skill_shot.png")
print("task:", task)
```

> 实测输出
> ```text
> in PIE: True
> task: <Object '/Engine/Transient.AutomationEditorTask_1' (0x00000281BB2BFA00) Class 'AutomationEditorTask'>
> ```

落盘位置与产物（两次实测：一次在 PIE 中，一次在纯编辑态）：

> 实测输出
> ```text
> E:\UnrealProject\AutoChess\Saved\Screenshots\WindowsEditor\ue_skill_pie.png    1079959   <- PIE 中截的
> E:\UnrealProject\AutoChess\Saved\Screenshots\WindowsEditor\ue_skill_shot.png   1121104   <- 编辑态截的
> ```
> 产物已确认是有效的 1280x720 PNG（OpenWorld 模板：天空 + 云 + 地形 + 网格地面）。

完整签名（实测从 `__doc__` 读出）：

```text
X.take_high_res_screenshot(res_x, res_y, filename, camera=None, mask_enabled=False, capture_hdr=False,
                           comparison_tolerance=ComparisonTolerance.LOW, comparison_notes="",
                           delay=0.000000, force_game_view=True) -> AutomationEditorTask
take_automation_screenshot_of_ui(world_context_object, latent_info, name, options) -> None
compare_image_against_reference(image_file_path, comparison_name="", comparison_tolerance=ComparisonTolerance.LOW,
                                comparison_notes="", world_context_object=None) -> bool
get_default_screenshot_options_for_gameplay(tolerance=ComparisonTolerance.LOW, delay=0.2) -> AutomationScreenshotOptions
get_default_screenshot_options_for_rendering(tolerance=ComparisonTolerance.LOW, delay=0.2) -> AutomationScreenshotOptions
finish_loading_before_screenshot() -> None
```

也可以走控制台（见 §10）：

```python
import unreal
ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
world = ues.get_editor_world()
unreal.SystemLibrary.execute_console_command(world, "HighResShot 1920x1080")
print("issued HighResShot")
```

注意点：

- 输出目录固定是 **`<项目>/Saved/Screenshots/WindowsEditor/<filename>`**（`WindowsEditor` 这一层由平台与模式决定）。
  `filename` 只给文件名，别给绝对路径。
- 返回的是 `unreal.AutomationEditorTask`，**异步**：函数返回 ≠ 文件已落盘。实测调用后要等若干秒文件才出现，
  紧接着 `Get-ChildItem` 会看不到。不要在同一帧里断言文件已存在。
- **编辑态（非 PIE）同样能出图**，不必先进 PIE；实测 `force_game_view=True` 在编辑态下也没有失败。
  （先前"编辑态没有 game viewport 所以截不了"的猜测被实测否定。）
- 截图是真实写盘操作，属于"写入类操作"。只做验证时，收尾记得把 `Saved/Screenshots/` 下的产物删掉。
- 不要走 `take_automation_screenshot_of_ui`：它需要 `latent_info`（`unreal.LatentActionInfo`），
  而 latent action 依赖 latent action manager 逐帧推进，远程执行的普通脚本上下文里没有宿主来推进它，会等不到完成。
- 走控制台那条路时注意：**PIE 中 `get_editor_world()` 返回 `None`**（见 §9），要在 PIE 里发命令得先拿到别的 world context。

---

## 12. 长任务进度条 ScopedSlowTask

`unreal.ScopedSlowTask` 支持 `with`（实测 `__enter__` / `__exit__` 均存在）：

```text
enter_progress_frame   enter_progress_frame(self, work: float = 1.0, desc: Union[Text, str] = "") -> None
make_dialog            make_dialog(self, can_cancel: bool = False, allow_in_pie: bool = False) -> None
make_dialog_delayed    make_dialog_delayed(self, delay: float, can_cancel: bool = False, allow_in_pie: bool = False) -> None
should_cancel          should_cancel() -> bool
```

```python
import unreal
eas = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
assets = eas.list_assets("/Game", recursive=True, include_folder=False)

with unreal.ScopedSlowTask(len(assets), "Scanning /Game ...") as slow_task:
    slow_task.make_dialog(True)          # True = 允许用户取消
    for path in assets:
        if slow_task.should_cancel():
            break
        slow_task.enter_progress_frame(1, path)
        _ = eas.find_asset_data(path)
print("scanned:", len(assets))
```

> 实测输出
> ```text
> scanned: 496
> ```

注意点：

- **`unreal.ProgressBar` 不是进度条工具类，它是 UMG 的 `UProgressBar` 控件**（`percent` / `set_percent()` / `bar_fill_type`…）。
  想要"慢任务进度对话框"只能用 `unreal.ScopedSlowTask`，别去 `unreal.ProgressBar` 上找。
- `enter_progress_frame(work, desc)` 的 `work` 是**本次推进的工作量**，所有 frame 的 work 之和应等于构造时给的总量；只传 `desc` 不推进会看起来卡住。
- `make_dialog(can_cancel=True)` 会弹出模态进度窗口。远程执行默认是 **unattended**（`ue.py` 不传 `--attended` 时模态对话框被抑制），
  所以远程跑的时候它通常不显示；只有在用户手动执行、或你确实需要 UI 时才用。**不要**用 `make_dialog` 去等用户点按钮——那会阻塞游戏线程。
- 循环体里不要放 `time.sleep()`。远程执行的编辑器代码跑在游戏线程上，睡久了整个编辑器假死，而且远程通道回不来。
- 输出要克制：`len()` 先看规模，再切片打印前 N 个。几百行 `print` 又慢又难读。

---

## 13. 写入类操作的清理（重要，含两个会让编辑器崩/卡死的坑）

> 🔴 **本节不是叫你照着做，而是记录踩过的雷。**
> §13a 里那些"在 `/Game/__ue_skill_scratch/` 建临时资产再删掉"的操作，**最终把用户的编辑器搞崩了**
> （详见 §13d）。
> **不要为了验证某个 API 能不能用，就在用户的真实项目里创建、改名、删除任何资产。**
> 需要临时资产时，先向用户要一个明确的沙箱路径并得到同意；没有同意就只用只读配方。

### 13a 脏资产删不掉

在用脚本创建了资产、并改过它之后，**包是脏的，`delete_asset` / `delete_loaded_asset` / `unload_packages` 都会失败**：

> 实测输出（`unload_packages` 的报错文本）
> ```text
> (False, Text("以下资产已经被修改并且无法卸载：
>     /Game/__ue_skill_scratch/BP_Scratch
> 保存这些资产即可将它们卸载。"))
> ```

先用 `set_dirty_flag` 把包清干净，再删：

```python
import unreal
eas = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
path = "/Game/__ue_skill_scratch/DT_Scratch"
asset = eas.load_asset(path)
eas.set_dirty_flag(asset, False)     # 清包脏标记，返回 True 表示成功
print(eas.delete_asset(path))        # 现在才删得掉
eas.delete_directory("/Game/__ue_skill_scratch")
```

> 实测输出（DataTable 与 WidgetBlueprint 的清理）
> ```text
> exists /Game/__ue_skill_scratch/DT_Scratch      True
> delete_asset -> True
> exists /Game/__ue_skill_scratch/WBP_Scratch     True
> delete_asset -> True
> delete_directory -> True
> directory exists: False
> remaining: []
> ```

### 13b `set_dirty_flag` 对 World / Level / Package 无效

`set_dirty_flag` 只对**内容资产**有效。对世界、关卡、包对象实测一律返回 `False`：

> 实测输出
> ```text
> on world : False -> ['/Temp/Untitled_1']
> on level : False -> ['/Temp/Untitled_1']
> on package: False -> ['/Temp/Untitled_1']
> on loaded bp, set True : True      <- 内容资产可以
> ```

所以**关卡被标脏后，Python 侧没有官方办法清除它**。`TRANSACTION UNDO`、`OBJ GC`、`OBJ CLEANUP` 都不会清掉这个标记。
结论：**只读任务绝不要碰会标脏关卡的操作（spawn / 属性写 / 开事务）**，除非任务本来就要改关卡；
收尾时如实告诉用户"关卡已修改、未保存，请自行决定保存或撤销"。

### 13c 被原生引用钉住的临时蓝图删不掉

在临时 Blueprint 上调过 `BlueprintEditorLibrary.add_function_graph` / `compile_blueprint` 之后，
该资产会被一个 **C++ 原生引用**钉死，穷尽 `delete_asset` / `delete_loaded_asset(s)` / `unload_packages` /
`set_dirty_flag` / `gc.collect()` / `SystemLibrary.collect_garbage()` / `OBJ GC` / 改名 都无法删除。
引擎日志给出的引用者只有一个：

> 实测输出（`Saved/Logs/AutoChess.log`）
> ```text
> External referencers of Blueprint /Game/__ue_skill_scratch/BP_Scratch.BP_Scratch:
>    GCObjectReferencer /Engine/Transient.GCObjectReferencer_0 (root) (1)
>       0) [[native reference]]
> LogObjectTools: Warning: ForceDeleteObject failed to delete BP_Scratch, this package is now potentially corrupt
> ```

（顺带：这里弹出的模态框在 unattended 模式下被自动关掉了 —— `Message dialog closed, result: Ok, title: 消息`，
所以**远程执行不会因为这类确认框卡死**，但也因此 `delete_asset` 只会静默返回 `False`。）

**规避**：验证"创建蓝图"这条链路时，只用 `create_blueprint_asset_with_parent` + `add_member_variable`（这套可以正常删除），
**不要**在一次性蓝图上调 `add_function_graph`；或者干脆不要创建蓝图，改成只读检查已有蓝图。

### 13d `reload_packages` 会把编辑器打崩（致命）

**绝对不要**对"只存在于内存、磁盘上没有 `.uasset`"的包调用
`unreal.EditorLoadingAndSavingUtils.reload_packages()`。引擎会卸载该包、试图从磁盘重建，
而磁盘上没有文件，重建出的 Blueprint 没有 GeneratedClass，直接断言崩溃。

> 实测输出（`Saved/Logs/AutoChess.log`，编辑器进程随即退出）
> ```text
> LogUObjectGlobals: Reloading 1 Package(s):
> 	Asset Name: /Game/__ue_skill_scratch/BP_ScratchRenamed
> LogOutputDevice: Warning: 
> Script Stack (1 frames) :
> /Script/UnrealEd.EditorLoadingAndSavingUtils.ReloadPackages
> LogWindows: Error: appError called: Assertion failed: nullptr != GeneratedClass
>     [File:D:\build\++UE5\Sync\Engine\Source\Runtime\Engine\Private\Blueprint.cpp] [Line: 786]
> LogWindows: Error: === Critical error: ===
> LogExit: Executing StaticShutdownAfterError
> ```

`unreal.ReloadPackagesInteractionMode` 的取值只有 `ASSUME_NEGATIVE` / `ASSUME_POSITIVE` / `INTERACTIVE`，
换哪个都一样，问题出在"无磁盘文件"这件事本身。同理别对临时包用 `unload_packages` 去"清理"，用 §13a 的正路。

---

## 14. 跨条目备忘（写脚本前先扫一眼）

| 事实 | 说明 |
|---|---|
| `unreal.KismetSystemLibrary` | **不存在** |
| `unreal.LogLevel` | **不存在** |
| `unreal.cast` | 模块级没有 `cast`；类型转换用 `unreal.Package.cast(obj)` 这种类方法形式 |
| `LevelEditorSubsystem.get_selected_actors` | **不存在**，用 `EditorActorSubsystem.get_selected_level_actors()` |
| `LevelEditorSubsystem.editor_set_game_paused` | **不存在** |
| `EditorActorSubsystem.get_all_level_actors_of_class` | **不存在**，用 `EditorFilterLibrary.by_class(actors, cls)` |
| `EditorAssetSubsystem.get_asset_class` | **不存在**，用 `find_asset_data(p).asset_class_path.asset_name` |
| `AssetEditorSubsystem.get_all_edited_assets` | **不存在**；只有 `close_all_editors_for_asset` / `open_editor_for_assets` |
| `unreal.ProgressBar` | 是 UMG 的 `UProgressBar` 控件，不是进度条工具类 |
| `Blueprint.generated_class` | 是**方法**，要写 `bp.generated_class()` |
| `Actor.actor_label` | 不是 editor property；用 `get_actor_label()` / `set_actor_label()` |
| `unreal.EditorLevelLibrary` / `EditorAssetLibrary` | 5.6 已弃用，调用会打 DeprecationWarning；一律用子系统 |
| `ue.py` 通道偶发断开 | 实测遇到 `[WinError 10054] 远程主机强迫关闭了一个现有的连接` / `the editor dropped the command channel`，重试一次即恢复；先确认编辑器进程还活着再重试 |
| 代码里的字面量 `.py` | 会触发引擎的文件路径误判。`ue.py` 已改为传路径规避；**不要绕过 `ue.py` 自己拼协议发源码** |
| 编辑器处于 PIE 时 | `get_editor_world()` / `get_current_level()` 返回 `None`，`list_assets` / `get_all_level_actors` 返回 `0` 并在日志报 `The Editor is currently in a play mode.` —— 是**静默空值**，不是真的没有。动手前先 `is_in_play_in_editor()` |
| 模态框 | 远程执行默认 unattended，模态对话框被自动关掉，不会卡死编辑器；但也因此相关 API 会静默失败（如 `delete_asset` 返回 `False`） |
| 编辑器日志 | 排查远程脚本问题时，`<项目>/Saved/Logs/AutoChess.log` 比 stdout 有用得多——资产删除失败的真实原因、崩溃断言都在这里 |
