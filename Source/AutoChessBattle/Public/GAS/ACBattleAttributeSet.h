// 阶段 1 新增（GAS 重构实施方案 §4.3 / §7 阶段 1），阶段 2 起成为**属性唯一权威**：
// 属性集。
//
// 本类是 `M07 属性与修饰器` 的目标承载形式。
//   - 阶段 1：只做镜像，权威读仍是 `FBattleStatSheet`；
//   - **阶段 2（§7 阶段 2）**：读权威**反转**到本类 —— `AACBattleUnitBase::GetStat` /
//     `GetMaxHP` / `GetCurrentHP` / `GetHealthRatio` 全部读本类的 CurrentValue，
//     暴击率 / 暴击倍率 / 攻速 Hz 三个派生量（`FBattleStatPipeline` 的静态工具）也改读本类；
//     护盾总量以本类 `Shield` 为准，当前生命以本类 `Health` 为准。
//     自研管线（`FBattleStatPipeline::Recompute` / 修饰器数组）已整体删除，修饰器交给
//     GAS 的 `FGameplayModifierInfo` + 聚合器（C8：**不**为属性修饰写 MMC）。
//   - 尚未迁移（阶段 3）：本类**一个 GE 都还没被应用过** —— ASC 不 Tick、不授予能力、不施 GE。
//     因此当前 Base 与 Current 恒等，等阶段 3/4 的 GE 开始施加修饰器，聚合器会自动让 Current
//     变成"Base + 修饰器"的结果，而读路径（`GetStat`）一行都不用改。
//
// 19 个基础属性与 `EACStat`（Core/ACBattleTypes.h，定长 19 项）**一一对应**：
//   MaxHP→MaxHealth  ATK→Attack  TECH→Technique  DEF→Defense  RES→Resistance
//   CritValue→CritValue  CritDamageBonus→CritDamageBonus  Lifesteal→Lifesteal  ASPD→AttackSpeed
//   DEFPen→DefensePen  RESPen→ResistancePen  HpRegen→HealthRegen
//   FocusMax→FocusMax  FocusInit→FocusInit  FocusRegen→FocusRegen  FocusPerAttack→FocusPerAttack
//   MentalMax→MentalMax  Range→Range
//
// ⚠️ "19 项"的口径对账（阶段 1 执行时逐项核对结论，**实施者注意**）：
//   `EACStat` 的 19 项 = **18 个真实属性 + 末尾的哨兵 `Count`**（`Core/ACBattleTypes.h:107-128`，
//   `ACStatCount = static_cast<int32>(EACStat::Count)` = 19 就是数组长度）。
//   因此 `UACBattleAttributeSet` 的基础属性是 **18 个**，不是 19 个 —— 给哨兵 `Count` 建属性没有意义
//   （它只是数组长度，`GetAttributeForStat(EACStat::Count)` 返回无效属性）。
//   对照 §4.3 的表（该表列了 18 个）：表里的 **`CritDamageBonus` 那一项是唯一被漏写的**，
//   本类已按 `EACStat` 的顺序把它补在第 7 位（`CritValue` 之后）。除此之外与表逐字一致。
//
// 对应关系**不给编译器推断**：`GetAttributeForStat` 是显式 `switch`（密钥 `EACStat`、值 `FGameplayAttribute`），
// 这样将来 `EACStat` 加减项时编译器会直接报"未处理的枚举值"，而不是静默错位。
//
// 另加 4 个资源属性（不在 `EACStat` 里，故不参与 `FACStatBlock` 的镜像）：
//   `Health`（当前生命）、`Shield`（护盾）、`Focus`（当前专注）、`Mental`（当前精神）。
//   ⚠️ `Shield` 是**普通属性**：GAS 5.6 全库无内置护盾（实施方案附录 C 已核实），
//      吸收逻辑仍在 `FCombatResolver` 里；阶段 2 起它是"总护盾量"的权威，
//      实例数组（`FShieldPool::Instances`，负责 FIFO 与持续护盾）每次都把合计同步进来。
//   ⚠️ `Mental` 的**权威值在 `FMentalSystem`**（`FMentalState`），本属性集里的 `Mental` 只是镜像，
//      阶段 1/2 都不要反过来把它当权威读；阶段 3 之后再裁决是否反转。
#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "Core/ACBattleTypes.h"
#include "ACBattleAttributeSet.generated.h"

/**
 * 属性访问器宏。
 *
 * 为什么在项目里定义而不是直接包含引擎的：UE 5.6 的 `AttributeSet.h` 只提供了
 * `ATTRIBUTE_ACCESSORS_BASIC`（AttributeSet.h:467），而 `ATTRIBUTE_ACCESSORS` 在引擎里
 * **只是注释里的示例**（AttributeSet.h:421 的示例段），工程侧按惯例自行定义一份。
 * 本宏展开出的四个函数逐一对应 `ATTRIBUTE_ACCESSORS_BASIC`（AttributeSet.h:467-471）：
 *   `static FGameplayAttribute GetXxxAttribute()` / `GetXxx()` / `SetXxx(float)` / `InitXxx(float)`。
 * `SetXxx` 走 ASC 的 `SetNumericAttributeBase`，**在 ASC 尚未 InitAbilityActorInfo 时会 ensure 失败**，
 * 因此初始化必须用 `InitXxx`（或走 `InitializeFromStatBlock`），不要用 `SetXxx`。
 */
#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
    GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
    GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
    GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
    GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

/**
 * 战斗单位属性集。
 *
 * 生命周期：作为 `AACBattleUnitBase` 的默认子对象创建（构造函数里 `CreateDefaultSubobject`），
 * 由 ASC 在 `InitAbilityActorInfo` 时自动注册（`GetSet<UACBattleAttributeSet>()` 可查到）。
 */
UCLASS()
class AUTOCHESSBATTLE_API UACBattleAttributeSet : public UAttributeSet
{
    GENERATED_BODY()

public:
    UACBattleAttributeSet();

    // ---------------------------------------------------------------------
    // 复制样板（框架约定）
    // ---------------------------------------------------------------------
    /**
     * 属性复制声明。
     *
     * ⚠️ 本项目是单机、不做客户端预测，但仍然照写，原因有两条（都是硬的，不是风格偏好）：
     *   ① `DOREPLIFETIME_*` 系列宏内部走 `GetReplicatedProperty()`，它在非 Shipping/Test 构建里
     *      会对没有 `CPF_Net` 标志的属性 `UE_LOG(Fatal)`（Net/UnrealNetwork.h:221-231）。
     *      因此"写了 `DOREPLIFETIME` 就必须给属性加 `Replicated` 关键字"，反之亦然 —— 二者要么都有、要么都没有。
     *   ② 属性集是 GAS 的复制单元（`UAttributeSet::IsSupportedForNetworking()`），
     *      引擎自带的 `UAbilitySystemTestAttributeSet` 同样是"属性带 `Replicated`"的形态
     *      （AbilitySystemTestAttributeSet.h:24-73）。留在这里，将来若做联机不用回头补。
     *
     * 依据：引擎 `GameplayPrediction.h:134-139` 给出的 `UMyHealthSet::GetLifetimeReplicatedProps` 标准写法。
     * 注：`UAttributeSet` 自身**不**声明本函数（AttributeSet.h 全文无 `GetLifetimeReplicatedProps`），
     * 所以这里是新增重写，`Super::` 调用链落在 `UObject` 上。
     */
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    // ---------------------------------------------------------------------
    // 19 个基础属性（对应 EACStat，逐项见文件头映射表）
    // ---------------------------------------------------------------------
    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData MaxHealth;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, MaxHealth);

    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData Attack;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, Attack);

    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData Technique;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, Technique);

    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData Defense;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, Defense);

    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData Resistance;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, Resistance);

    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData CritValue;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, CritValue);

    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData CritDamageBonus;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, CritDamageBonus);

    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData Lifesteal;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, Lifesteal);

    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData AttackSpeed;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, AttackSpeed);

    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData DefensePen;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, DefensePen);

    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData ResistancePen;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, ResistancePen);

    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData HealthRegen;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, HealthRegen);

    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData FocusMax;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, FocusMax);

    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData FocusInit;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, FocusInit);

    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData FocusRegen;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, FocusRegen);

    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData FocusPerAttack;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, FocusPerAttack);

    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData MentalMax;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, MentalMax);

    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute")
    FGameplayAttributeData Range;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, Range);

    // ---------------------------------------------------------------------
    // 资源属性（不在 EACStat 里，不参与 FACStatBlock 镜像）
    // ---------------------------------------------------------------------
    /**
     * 当前生命。**只有 `FCombatResolver` 能改它**（§4.6 新契约）。
     * 实际写入口是 `AACBattleUnitBase::SetHealthFromResolver`（受控入口，名字里带 FromResolver），
     * 它直写本属性的 Base/Current 并按 `[0, MaxHealth]` 夹取。
     */
    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute|Resource")
    FGameplayAttributeData Health;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, Health);

    /**
     * 护盾。GAS 5.6 **没有内置护盾**（实施方案附录 C），吸收逻辑仍写在 `FCombatResolver` 里。
     *
     * 阶段 2 起本属性是**总护盾量的权威**：`FShieldPool` 仍保留实例数组（FIFO 顺序 + 每层各自的
     * `ExpireTime`，属性集单值表达不了），但每次改动实例都会把合计同步进本属性
     *（`FShieldPool::SyncTotalToAttributeSet`），`GetTotal()` 读的也是本属性。
     */
    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute|Resource")
    FGameplayAttributeData Shield;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, Shield);

    /** 当前专注（上限走 `FocusMax`）。 */
    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute|Resource")
    FGameplayAttributeData Focus;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, Focus);

    /**
     * 当前精神。⚠️ **权威值在 `FMentalSystem`**（`FMentalState`），本属性只是镜像；
     * 阶段 1/2 一律不要把它当权威读，反向写入同样由 `FMentalSystem` 决定。
     */
    UPROPERTY(BlueprintReadWrite, Replicated, Category = "Battle|Attribute|Resource")
    FGameplayAttributeData Mental;
    ATTRIBUTE_ACCESSORS(UACBattleAttributeSet, Mental);

    // ---------------------------------------------------------------------
    // 初始化 / 查表
    // ---------------------------------------------------------------------
    /**
     * 把 `FACStatBlock` 的 19 项按 `EACStat` 逐项写入对应属性的 BaseValue（同时把 CurrentValue 对齐）。
     *
     * 口径与 `FBattleStatPipeline::SetBaseFromBlock`（Private/ACBattleStats.cpp:8-18）**一致**：
     * 缺项补 0、越界取 0 —— 两处是**互为独立的实现**（一个走显式属性映射、一个走数组复制），
     * 因此"属性集的值 == `FBattleStatSheet::Base`"随时可以互相校验。
     * 阶段 1 的验收就是这条等式；**阶段 2 起它不只是校验**：属性集是读权威，`GetStat` 读的就是这里写下的值。
     *
     * 实现走 `FGameplayAttributeData::SetBaseValue/SetCurrentValue`（AttributeSet.h:41/47）。
     * **不走** `UAbilitySystemComponent::SetNumericAttributeBase`：那条路要求 ASC 已经
     * `InitAbilityActorInfo` 完毕（本函数在 `BeginPlay` 之前、单位数据填完之后调用，见 ACBattleUnitBase.cpp），
     * 走 ASC 会让属性初始化依赖 ActorInfo 的就绪顺序。资源属性（`Health` / `Shield` / `Focus` / `Mental`）
     * 不属于 `FACStatBlock`，由调用方随后单独初始化。
     */
    void InitializeFromStatBlock(const FACStatBlock& Block);

    /**
     * 初始化一个资源属性（`Health` / `Shield` / `Focus` / `Mental`）。
     *
     * 为什么资源属性不进 `InitializeFromStatBlock`：它们**不在** `EACStat` / `FACStatBlock` 里
     * （`EACStat` 的注释就写了"CurrentHP / 护盾 / 专注 / 精神为资源，不在此表"），
     * 硬塞进去就得给它们编造一个 `EACStat` 索引，那正是"用下标猜映射"。
     * 参数直接收 `FGameplayAttribute`（例如 `GetHealthAttribute()`），
     * 调用点写的是属性名，读起来与赋值语句一样直白。
     *
     * 与 `InitializeFromStatBlock` 同一写法：直接写 `FGameplayAttributeData`，
     * **不走** ASC（初始化发生在 `InitAbilityActorInfo` 之前，走 ASC 会依赖 ActorInfo 就绪顺序）。
     *
     * 实现是对下面静态 `SetAttributeDataValue` 的转发，口径只有一份。
     * 项目内当前有三处写入点，**都直接用静态版**（`AACBattleUnitBase` 初始化、
     * `FCombatResolver` 扣血、`FShieldPool` 护盾总量同步），因此本成员函数目前**没有调用方** ——
     * 保留它是为了给"在别的 `UObject` 语境里拿到属性集实例、想按句柄写一个资源属性"留一个
     * 语义明确的公开入口（比每次写全限定静态调用更短），不是遗留死代码。
     */
    void InitializeResourceAttribute(const FGameplayAttribute& Attribute, float Value);

    /**
     * `EACStat` → `FGameplayAttribute` 的静态查表（显式 switch，不用数组下标猜映射）。
     *
     * 阶段 2 起它是**读路径的核心**：`AACBattleUnitBase::GetStat` 与
     * `FBattleStatPipeline::GetCritChance/GetCritMultiplier/GetAttackSpeedHz` 都经它取句柄。
     * 传入 `EACStat::Count`（或任何越界值）返回无效 `FGameplayAttribute`（`IsValid() == false`）。
     */
    static FGameplayAttribute GetAttributeForStat(EACStat Stat);

    /**
     * 按属性句柄写入 BaseValue + CurrentValue（两个值同时写，保持一致）。
     *
     * 为什么需要它（而不是让调用方自己 `GetSet<>` 再改）：属性的初值写入发生在
     * `InitAbilityActorInfo` **之前**（单位是延迟构造的：先填数据、后 `FinishSpawning` → BeginPlay），
     * 而 `UAbilitySystemComponent::SetNumericAttributeBase` 那条路要求 ActorInfo 已就绪
     * （`ATTRIBUTE_ACCESSORS` 生成的 `SetXxx` 内部就是它，且带 `ensure`）。
     * 因此初始化一律走这里；**运行期**改属性才走 ASC / GE。
     * 阶段 2 起它还被两个内核热路径复用（`SetHealthFromResolver` 的扣血、`FShieldPool` 的护盾总量），
     * 理由相同：那两处都不该为了写一个数去碰聚合器，而夹取口径由写入方按本类 `PreAttributeChange`
     * 的分支逐字复刻（见那些函数的注释）。
     *
     * `Attribute.IsValid() == false` 时静默返回：调用方唯一会传无效值的场景是
     * `GetAttributeForStat(EACStat::Count)`，那是哨兵不是异常。
     */
    static void SetAttributeDataValue(UAttributeSet* Set, const FGameplayAttribute& Attribute, float Value);

    // ---------------------------------------------------------------------
    // 属性变化钩子（阶段 2：Clamp 覆盖 ASC 路径 + GE 管线守卫）
    // ---------------------------------------------------------------------
    /**
     * 属性写入前的 Clamp：`Health` ≤ `MaxHealth`、`Focus` ≤ `FocusMax`、其余非资源属性 ≥ 0。
     *
     * ⚠️ **阶段 2 起这条 Clamp 只覆盖"经 ASC 写入"这一条路径**（GE Modifier 求值 /
     * `SetNumericAttributeBase`）。而本阶段的全部属性写入都是**直写 `FGameplayAttributeData`**
     *（延迟构造期的初始化、`FCombatResolver` 的扣血、护盾总量同步），引擎不会回调任何钩子，
     * 因此那些写入口径必须**自己**做同样的夹取：
     *   - 当前生命 → `AACBattleUnitBase::SetHealthFromResolver`（口径与本函数 `Health` 分支逐字一致）；
     *   - 护盾总量 → `FShieldPool::SyncTotalToAttributeSet`（`Max(0, ...)`，与本函数 `Shield` 分支一致）。
     * 这不是重复劳动：两条路径迟早都会有人走（阶段 3/4 的 GE 走 ASC，内核热路径走直写），
     * 两边口径一致才不会出现"同一个值经不同路径写进去得到不同结果"。
     */
    virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;

    /**
     * **Base 值**夹取：GE 的属性修饰走的是这条路（`ApplyModToAttribute` → `SetAttributeBaseValue`），
     * 而 `PreAttributeChange` **不会被它调用**。因此这里必须独立夹取一次，否则：
     *   ① "清空全部专注"（幅度 `-999`）会把 BaseValue 写成 `-999`，之后每次回专注都在还债；
     *   ② `CanApplyAttributeModifiers` 的付得起判定读的是 BaseValue，`100 + (-999) < 0`
     *      会让这条 Cost **恒被判付不起，技能一次都放不出来**（`GameplayEffect.cpp:5201`）。
     *
     * 注意本函数是 `const`：只能读其它属性的 BaseValue（上限按 Base 判定，见实现注释）。
     */
    virtual void PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const override;

    /**
     * GE 执行后的钩子：**管线守卫**（§4.6 第 3 条 / §8.2 第 3 项不变量）。
     *
     * 契约：任何 `Health` 变化都必须经过 `UACDamageExecution` → `FCombatResolver`。
     *
     * 为什么这里用 `UE_LOG(Warning)` 而**不是** `ensure`（阶段 2 的实现裁决，与 §4.6 原文的差异）：
     *   ① `FCombatResolver` 改 `Health` 的方式是**直写 `FGameplayAttributeData`**
     *      （`AACBattleUnitBase::SetHealthFromResolver` 内的
     *      `UACBattleAttributeSet::SetAttributeDataValue`），**不是** GE Modifier。
     *      本函数只在"GE 修改属性"时被引擎调用，因此**正常伤害根本不会走到这里** ——
     *      在这里放 `ensure` 不会误报正常伤害，但也**抓不到**任何真实伤害路径；
     *   ② 本阶段（以及阶段 3 之前）属性集上**一个 GE 都没有**，所以这里当前恒不触发；
     *   ③ 阶段 3/4 起内容会开始用 GE，其中会有**合法的、非伤害的** `Health` 修改
     *      （治疗 GE、吸血 GE、处决 GE、状态 GE 的周期结算…… 这些都要走 `FCombatResolver`，
     *      但验证它们是否真的绕过管线需要"来源"信息，而本函数拿不到 `SourceEffectBlockId` 之外的判据）。
     *      在这种"误报风险未知"的阶段用 `ensure`，等于给未来埋一个会打断整场战斗的断言。
     * 结论：**用 Warning 记录，并写明唯一允许的合法路径**。
     * 阶段 3 引入 GE 之后再按实际调用点复核一次：届时若确认"所有合法 `Health` GE 修改都必须带
     * 特定标签"，可以升级为 `ensure`（那条判据现在还不存在，不能凭空写）。
     */
    virtual void PostGameplayEffectExecute(const struct FGameplayEffectModCallbackData& Data) override;
};
