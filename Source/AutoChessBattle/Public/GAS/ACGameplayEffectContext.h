// 阶段 1 新增（GAS 重构实施方案 §3.3 / §5.1 / §7 阶段 1）：自定义 EffectContext。
//
// 为什么需要它：`FGameplayEffectContext` 是 GAS 官方的"每次效果应用的瞬时上下文"扩展点
// （GameplayEffectTypes.h:239-245 的注释就是这么说的：transient information about an execution）。
// 伤害结算需要的口径 —— 伤害类型 / 伤害来源 / 来源效果块 / 源与目标单位句柄 ——
// 都属于"这次应用才有"的瞬时数据，塞进 `UGameplayEffect` 资产是错的（资产是共享的），
// 塞进 `FGameplayEffectSpec::SetByCaller` 又只能传 float（类型与可读性都丢）。
//
// §5.1 裁决：**战斗内瞬时结构用整型 `FUnitId`**（热路径零解引用开销，同帧内单位不会被销毁）。
// 本 context 正是战斗内瞬时数据，因此 `SourceUnitId` / `TargetUnitId` 用 `FUnitId`（int32），
// 不用 `TWeakObjectPtr<AACBattleUnitBase>`。
#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectTypes.h"
#include "Core/ACBattleTypes.h"
#include "ACGameplayEffectContext.generated.h"

/**
 * 携带战斗口径的 EffectContext。
 */
USTRUCT()
struct AUTOCHESSBATTLE_API FACGameplayEffectContext : public FGameplayEffectContext
{
    GENERATED_BODY()

    /** 伤害类型（物理 / 技术 / 真实 / 精神）。 */
    UPROPERTY()
    EACDamageType DamageType = EACDamageType::Physical;

    /** 伤害来源（普攻 / 技能 / 持续伤害 / 反伤 / 环境 / 处决）。 */
    UPROPERTY()
    EACDamageReason DamageReason = EACDamageReason::Skill;

    /** 来源效果块 Id（阶段 2 起用于日志与"按来源移除"；GE 资产化之后改用 `SourceObject`）。 */
    UPROPERTY()
    FName SourceEffectBlockId;

    /** 伤害来源单位句柄（`InvalidUnitId` = 无来源，例如环境伤害）。 */
    FUnitId SourceUnitId = InvalidUnitId;

    /** 承伤单位句柄（`InvalidUnitId` = 未指定）。 */
    FUnitId TargetUnitId = InvalidUnitId;

    // ---- 基类扩展点 ----

    /**
     * 返回派生结构：GAS 靠它决定按哪个结构做序列化 / 复制。
     * 不重写会让派生字段在 `FGameplayEffectContextHandle` 序列化时被当成基类丢掉。
     */
    virtual UScriptStruct* GetScriptStruct() const override
    {
        return FACGameplayEffectContext::StaticStruct();
    }

    /** 深拷贝：`FGameplayEffectContextHandle` 在复制 spec 时会调它（基类实现见 GameplayEffectTypes.h:394-404）。 */
    virtual FGameplayEffectContext* Duplicate() const override
    {
        FACGameplayEffectContext* const NewContext = new FACGameplayEffectContext();
        *NewContext = *this;
        if (GetHitResult() != nullptr)
        {
            // 基类的 HitResult 是 `TSharedPtr<FHitResult>`：浅拷贝会让两个 context 共享同一个命中结果，
            // 之后任何一处改它都会互相污染。基类自己也是这么处理的（GameplayEffectTypes.h:398-402）。
            NewContext->AddHitResult(*GetHitResult(), true);
        }
        return NewContext;
    }

    /**
     * 网络序列化。
     *
     * ⚠️ 单机项目：本函数**当前不会被调用**（没有任何复制通道），写它是为了满足
     * `TStructOpsTypeTraits::WithNetSerializer` 的接口约定 —— 声明了 trait 就必须有实现，
     * 否则链接期缺符号。实现是"先调基类（它自带 7 位标志的完整结构）+ 再写本类的定长字段"，
     * 顺序与基类 `GameplayEffectContext::NetSerialize`（GameplayEffectTypes.cpp:237-312）的模式一致，
     * 将来真做联机时直接可用，不需要回头补。
     */
    virtual bool NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess) override;
};

template<>
struct AUTOCHESSBATTLE_API TStructOpsTypeTraits<FACGameplayEffectContext> : public TStructOpsTypeTraitsBase2<FACGameplayEffectContext>
{
    enum
    {
        WithNetSerializer = true,
        // 与基类保持一致（GameplayEffectTypes.h:475-483）：基类带 TSharedPtr 成员，必须显式拷贝。
        WithCopy = true
    };
};

/** 从 effect handle 里取出本项目的伤害上下文；类型不匹配（走的是基类 context）时返回 nullptr。 */
namespace ACGameplayEffectContext
{
    FORCEINLINE FACGameplayEffectContext* FromHandle(const FGameplayEffectContextHandle& Handle)
    {
        const FGameplayEffectContext*  BaseContext = Handle.Get();
        if (BaseContext == nullptr || BaseContext->GetScriptStruct() != FACGameplayEffectContext::StaticStruct())
        {
            return nullptr;
        }
        return const_cast<FACGameplayEffectContext*>(static_cast<const FACGameplayEffectContext*>(BaseContext));
    }

    FORCEINLINE const FACGameplayEffectContext* FromHandleConst(const FGameplayEffectContextHandle& Handle)
    {
        const FGameplayEffectContext* const BaseContext = Handle.Get();
        if (BaseContext == nullptr || BaseContext->GetScriptStruct() != FACGameplayEffectContext::StaticStruct())
        {
            return nullptr;
        }
        return static_cast<const FACGameplayEffectContext*>(BaseContext);
    }
}
