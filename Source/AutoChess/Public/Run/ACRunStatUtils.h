// Run 层 ↔ 战斗内核的**属性口径桥梁**。
//
// 为什么要单独一个文件：这是整条链路上最容易出错的地方。
//   - 战斗内核只认 `FACStatBlock`（按 EACStat 索引的定长数组），它不知道等级/锻体/装备/复活惩罚；
//   - Run 层持有的是"成长来源"，不是最终属性；
//   - 因此必须在**启动战斗之前**把成长折算成一份基础属性快照，战斗期间不再变。
// 这就是"事实源唯一"的落地方式：Run 层是成长的事实源，战斗世界是这一场的属性事实源。
//
// 约定：
//   1. 读取顺序 = 等级基础值 → 永久固定加成（Add）→ 永久百分比（MulPct）；
//   2. `MulPct` 是 **乘算**（Value = 1 表示 +100%），不是"加 1 个百分点"；
//   3. 空指针安全：Run 层必须能在"没有任何定义资产"的情况下跑通（Content 为空时也是）。
#pragma once

#include "CoreMinimal.h"
#include "Core/ACBattleTypes.h"

struct FACRunStatModifier;
class UUnitDefinitionBase;
class UOperatorDefinition;

class AUTOCHESS_API FACRunStatUtils
{
public:
    /** 结果数组长度固定为 ACStatCount。 */
    static void MakeZeroBlock(FACStatBlock& OutBlock);

    /**
     * 合成参战基础属性。
     * @param Definition   干员/敌人定义资产；可为 nullptr（此时只用 Fallback）
     * @param Level        0=D … 4=S（同时是 StatsByLevel 的下标）
     * @param StatModifiers 跨战斗保留的永久加成（锻体 / 复活惩罚 / 装备属性）
     * @param Fallback     无定义资产时的兜底属性（可全 0，战斗内核会自行处理 0 值）
     * @param OutBlock     输出（长度 = ACStatCount）
     */
    static void ComposeBaseStats(const UUnitDefinitionBase* Definition, int32 Level,
                                 const TArray<FACRunStatModifier>& StatModifiers,
                                 const FACStatBlock& Fallback, FACStatBlock& OutBlock);

    /**
     * 复活的永久惩罚：对"同一名干员已有的永久修饰"整体乘 (1 - PenaltyPercent/100)。
     * 直接改在修饰器上而不是改等级数据，是为了让惩罚可累计、可回溯（多次复活叠乘）。
     */
    static void ApplyResurrectPenalty(TArray<FACRunStatModifier>& InOutModifiers, float PenaltyPercent);

    /** 干员等级对应的装备槽上限（D=1 … S=4）。Definition 为空时按 D 级处理。 */
    static int32 GetEquipSlotMax(const UOperatorDefinition* Definition, int32 Level);

    /** 等级 → 个性强化数值档位（0=C, 1=B, 2=A, 3=S），与《系统结构说明》§5.11 一致。 */
    static int32 GetTierIndexForLevel(int32 Level);
};
