// 代码侧内容库：把**全部示例内容**（能力 / GE 清单 / 干员 / 敌人 / 召唤物 / 装备池 / 规则覆盖）
// 定义成 C++ 数据，并注册进 `UBattleDataSubsystem`。
//
// 存在的意义（对应"Content 为空也要能跑起来"这条约束）：
//   战斗内核的内容入口是 `FACBattleDataContext`（定义资产 + 能力清单）。
//   本类提供**代码侧那一路**，让工程在没有 .uasset 时依然是一条完整可跑的链路。
//
// 三处职责分界：
//   - `ACBattleContentDefinitions.cpp` —— 真正的**内容数据**（要改干员/敌人就改那里）；
//   - 本文件 —— 内容数据的**装配与注册**（通用机制）；
//   - `UACOperatorLibrary`（Run 层用）—— 从注册结果里抽出 Run 需要的视图（招募池/强化选项）。
//
// 阶段 3.2a（GAS 重构实施方案 §3.1 表 / §7 阶段 3.2）本文件删掉的：
//   `Blocks` 命名空间（14 个效果块 BlockId 常量）、`BuildEffectBlocks`、
//   `GetEffectLibrary` / `CodeEffectLibrary`（`UBattleEffectLibrary`）、
//   `BuildAbnormalStates` / `OwnedAbnormalStates`（`UAbnormalStateDefinition`）、
//   以及只服务于效果块的一个工厂：`MakeSelfStatAction`。
//   新增：`BuildAbilitySets`（定义 Id → `UACAbilitySet`）。
// **阶段 3.3**：过渡映射表 `BuildEffectBlockGEs` **已删除** —— `Run/*` 与 `FACBattleSetup` 的
//   效果块字段已经是 GE / 能力**类引用**，翻译层不再需要。
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "Templates/SubclassOf.h"
#include "Core/ACDataTypes.h"
#include "Core/ACBattleTypes.h"
// Run/ACRunTypes.h 是必须的：本头文件按值持有 FACRunEquipment / FACRunFragment /
// FACRunTimelineGrant（USTRUCT 成员与函数参数），只靠前向声明无法定义 TArray 元素类型。
#include "Run/ACRunTypes.h"
#include "ACBattleContentLibrary.generated.h"

class UEnemyDefinition;
class UOperatorDefinition;
class UBattleRuleConfig;
class UUnitDefinitionBase;
class USummonDefinition;
class UACAbilitySet;
class UGameplayEffect;
class UGameplayAbility;

/**
 * 一条个性强化候选（阶段 3.3：替代旧的 `FName` 效果块 Id）。
 *
 * 两个数组的分工就是"GAS 把触发与效果拆开"这件事在内容侧的投影：
 *   · `Effects`           —— 选中后**施加**的战斗内 GE（纯属性类强化，例如 C 级"专注置换"）；
 *   · `GrantedAbilities` —— 选中后**授予**的被动能力（带触发条件的强化，例如 S 级"处刑"，
 *     它自己再监听 `Hook.Kill` 并在击杀数达标时施加 `UACGE_Upgrade_OP01_S1`）。
 * 一条候选可以只填其中一个，也可以两个都填。
 */
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACContentUpgradeOption
{
    GENERATED_BODY()

    /** 选中后施加的战斗内常驻 GE。 */
    UPROPERTY(BlueprintReadOnly, Category = "Content")
    TArray<TSubclassOf<UGameplayEffect>> Effects;

    /** 选中后授予的被动能力（"选了才有"的触发式内容必须先授予能力，见 `FACRunOperator`）。 */
    UPROPERTY(BlueprintReadOnly, Category = "Content")
    TArray<TSubclassOf<UGameplayAbility>> GrantedAbilities;
};

/** 一名干员随内容一起注册的附加信息（Run 层的招募/成长需要，战斗内核不需要）。 */
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACContentOperatorEntry
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Content")
    TObjectPtr<UOperatorDefinition> Definition = nullptr;

    /** 招募品级 0..4（D/C/B/A/S）。 */
    UPROPERTY(BlueprintReadOnly, Category = "Content")
    int32 RecruitTier = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Content")
    FGameplayTag ClassTag;

    /**
     * 个性强化候选项：每个等级档位（0=C … 3=S）一项，注册/升级时直接取该档位那一项。
     * （示例内容每档只给一个候选；"三选一 UI"落地时把它扩成"每个档位一组候选"即可，
     *  `ResolveUpgradeChoice` 的按档位取值的形状不用改。）
     */
    UPROPERTY(BlueprintReadOnly, Category = "Content")
    TArray<FACContentUpgradeOption> UpgradeChoices;
};

/** 某一品级的装备池。 */
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACContentEquipmentPool
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Content")
    TArray<FACRunEquipment> Items;
};

/**
 * 某一品阶的锻体碎片池。
 * 必须包成 USTRUCT：UHT 禁止 TMap 的 value 直接是 TArray
 * （"The type 'TArray<...>' can not be used as a value in a TMap"）。
 */
USTRUCT(BlueprintType)
struct AUTOCHESS_API FACContentFragmentPool
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Content")
    TArray<FACRunFragment> Items;
};


UCLASS()
class AUTOCHESS_API UACBattleContentLibrary : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;

    /** 幂等：可在任何需要内容就绪的地方调用（构造期/开局期/调试命令）。 */
    void EnsureInitialized();

    bool IsInitialized() const { return bBuilt; }

    const TArray<FACContentOperatorEntry>& GetOperators() const { return Operators; }
    const FACContentOperatorEntry* FindOperator(FName OperatorId) const;
    const UEnemyDefinition* FindEnemy(FName EnemyDefinitionId) const;

    /**
     * 该干员在指定等级档位下应注册的个性强化候选（示例实现：取该档位那一项）。
     * 返回空 `FACContentUpgradeOption`（两个数组都空）= 该档位没有强化。
     */
    FACContentUpgradeOption ResolveUpgradeChoice(FName OperatorId, int32 TierIndex) const;

    /**
     * 取某个单位定义的能力 / 常驻 GE 清单（阶段 3.2a）。
     * 返回 nullptr = 该定义没有配清单（正常情况，例如蕾拉没有常驻 GE）。
     */
    UACAbilitySet* FindAbilitySet(FName DefinitionId) const;

    /** 代码侧规则覆盖：默认 nullptr，走引擎缺省规则（30s 时限 / A9–A20 的默认值）。 */
    UBattleRuleConfig* GetRuleConfig() const { return CodeRuleConfig; }

    /** 装备池（按品级 0=白 1=蓝 2=金 3=红 索引）；集市刷新从这里抽。 */
    void GetEquipmentByTier(int32 Tier, TArray<FACRunEquipment>& OutEquipment) const;

    /** 锻体碎片池（按品阶 0=银 1=金 2=红 索引）；返回 false 表示该品阶没有内容。 */
    bool GetFragmentsByTier(int32 Tier, TArray<FACRunFragment>& OutFragments) const;

private:
    /** 组装并注册全部内容。 */
    void BuildAndRegister();

    UPROPERTY() TObjectPtr<UBattleRuleConfig> CodeRuleConfig = nullptr;
    UPROPERTY() TArray<FACContentOperatorEntry> Operators;
    UPROPERTY() TMap<FName, TObjectPtr<UEnemyDefinition>> Enemies;

    /** 装备池：Tier(0..3) → 该品级的装备列表。 */
    UPROPERTY() TMap<int32, FACContentEquipmentPool> EquipmentPools;

    /** 锻体碎片池：Tier(0..2) → 该品阶的碎片列表（value 必须是 USTRUCT，见 FACContentFragmentPool）。 */
    UPROPERTY() TMap<int32, FACContentFragmentPool> FragmentPools;


    /** 代码侧创建的全部定义对象（GC 根）。 */
    UPROPERTY() TArray<TObjectPtr<UUnitDefinitionBase>> OwnedDefinitions;
    /** 敌人定义单独存一份强类型数组：BuildEnemies 需要 TArray<TObjectPtr<UEnemyDefinition>>。 */
    UPROPERTY() TArray<TObjectPtr<UEnemyDefinition>> OwnedEnemies;
    UPROPERTY() TArray<TObjectPtr<USummonDefinition>> OwnedSummons;

    /**
     * 定义 Id → 能力 / 常驻 GE 清单（阶段 3.2a 新增，替代旧的异常状态与效果块两处登记）。
     * 与 `OwnedDefinitions` 同理由本类强引用：清单是 `NewObject(GetTransientPackage())` 造的，
     * 没有磁盘资产替它保命。
     */
    UPROPERTY() TMap<FName, TObjectPtr<UACAbilitySet>> AbilitySets;

    bool bBuilt = false;
};

/**
 * 内容数据定义入口（实现在 `Private/Content/ACBattleContentDefinitions.cpp`）。
 *
 * 新增一名干员 / 一种敌人的唯一改法：在 `ACBattleContentDefinitions.cpp` 里加一个函数、
 * 在此声明并在 `UACBattleContentLibrary::BuildAndRegister` 中登记。
 * 能力与 GE 全部是**类引用**（阶段 3.1 的原生内容），不再需要 `Blocks` 常量表 ——
 * 拼错 BlockId 这类配置事故因此从"字符串比对失败"变成"编译期找不到类"。
 */
namespace ACBattleContent
{
    /** 干员定义 + Run 侧信息。 */
    AUTOCHESS_API void BuildOperators(TArray<FACContentOperatorEntry>& OutOperators);

    /** 敌人定义（键 = DefinitionId）。 */
    AUTOCHESS_API void BuildEnemies(TMap<FName, TObjectPtr<UEnemyDefinition>>& OutEnemies,
                                    TArray<TObjectPtr<UEnemyDefinition>>& OutOwned);

    /** 召唤物定义（键 = DefinitionId）。 */
    AUTOCHESS_API void BuildSummons(TMap<FName, TObjectPtr<USummonDefinition>>& OutSummons,
                                    TArray<TObjectPtr<USummonDefinition>>& OutOwned);

    /**
     * 单位定义 Id → 能力 / 常驻 GE 清单（阶段 3.2a 新增，替代旧的 `BuildEffectBlocks`
     * 与 `BuildAbnormalStates` 两条内容通道）。
     *
     * 数值来源与旧内容逐项一致（C6/D10）：能力类与 GE 类里的数值都写在
     * `GAS/Abilities/*` 与 `GAS/Effects/*` 的构造函数里，且每个类都标注了它抄自
     * `BuildEffectBlocks` 的哪一段。
     */
    AUTOCHESS_API void BuildAbilitySets(TMap<FName, TObjectPtr<UACAbilitySet>>& OutAbilitySets,
                                        TArray<TObjectPtr<UACAbilitySet>>& OutOwned);

    // 阶段 3.3 已删除：`void BuildEffectBlockGEs(TMap<FName, TSubclassOf<UGameplayEffect>>& OutMap)`。
    // 它是 3.2a 的**过渡映射表**（`Run` 层的 `FName` 效果块 Id → GE 类），
    // 服务于"契约字段还是 `FName`"的那段时间。字段改成类引用后翻译层整体退场 ——
    // 现在内容侧直接给出 `UACGE_*` / `UACPassive_*` 的类引用，拼错内容会在编译期暴露。

    /** 装备池（键 = 品级 0=白 1=蓝 2=金 3=红）。 */
    AUTOCHESS_API void BuildEquipmentPools(TMap<int32, FACContentEquipmentPool>& OutPools);

    /** 锻体碎片池（键 = 品阶 0=银 1=金 2=红）。 */
    AUTOCHESS_API void BuildFragmentPools(TMap<int32, FACContentFragmentPool>& OutPools);

    /** 单值档位（C=B=A=S=Value）。 */
    AUTOCHESS_API FACTieredValue Tier(float Value);

    /** 造一个固定值属性修饰（锻体/装备属性用）。 */
    AUTOCHESS_API FACRunStatModifier MakeStatModifier(EACStat Stat, float Value, FName SourceId = NAME_None);

    /** 造一件装备（阶段 3.3：`Effects` / `GrantedAbilities` / `TimelineGrants` 都收类引用）。 */
    AUTOCHESS_API FACRunEquipment MakeEquipment(FName EquipmentId, const FString& DisplayName, int32 Tier,
                                                const TArray<FACRunStatModifier>& StatModifiers,
                                                const TArray<TSubclassOf<UGameplayEffect>>& Effects = TArray<TSubclassOf<UGameplayEffect>>(),
                                                const TArray<TSubclassOf<UGameplayAbility>>& GrantedAbilities = TArray<TSubclassOf<UGameplayAbility>>(),
                                                const TArray<FACRunTimelineGrant>& TimelineGrants = TArray<FACRunTimelineGrant>());

    /** 造一个锻体碎片。 */
    AUTOCHESS_API FACRunFragment MakeFragment(FName FragmentId, const FString& DisplayName, int32 Tier,
                                              const TArray<FACRunStatModifier>& StatModifiers);

    // ⚠️ **阶段 3.2a 删掉的一处声明**（写在这里让"它去哪了"可查）：
    //   `FACEffectAction MakeSelfStatAction(EACStat, float, /*scope*/, float)`
    //   它造的是 `FACEffectAction`，而那个类型随效果块系统一起删除（§3.1 表），
    //   因此声明本身已经无法表达（既不能定义、也不能被调用 —— 调用点会直接编译失败）。
    //   全仓没有任何调用点（唯一一处是 `BuildEquipmentPools` 的注释）。
    //   替代：Run 层的属性修饰走 `FACRunStatModifier`，战斗内的属性修饰走
    //   `UACGE_StatModifier` 的子类（`GAS/Effects/ACGE_ContentEffects.h`）。

    // ---- 造数据的小工具（避免每处都写 14 行 Set 调用）----

    /** 按 EACStat 顺序填充属性块：顺序 = MaxHP, ATK, TECH, DEF, RES, CritValue, ASPD, Range, FocusMax, FocusInit, FocusRegen, FocusPerAttack。 */
    AUTOCHESS_API FACStatBlock MakeStatBlock(float MaxHP, float ATK, float TECH, float DEF, float RES,
                                             float CritValue, float ASPD, float Range,
                                             float FocusMax, float FocusInit, float FocusRegen, float FocusPerAttack);

    /** 按 S→A→B→C 顺序给出四档数值（与策划表"高等级在前"的书写习惯一致）。 */
    AUTOCHESS_API FACTieredValue Tiers(float S, float A, float B, float C);
}
