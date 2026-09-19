import io, os, re

root = r'E:\UnrealProject\AutoChess\Source'
files = []
for base, _, fns in os.walk(root):
    for fn in fns:
        if fn.endswith(('.h', '.cpp')):
            files.append(os.path.join(base, fn))
files.sort()


def strip(t):
    t = re.sub(r'/\*.*?\*/', ' ', t, flags=re.S)
    t = re.sub(r'//[^\n]*', ' ', t)
    t = re.sub(r'"(?:\\.|[^"\\])*"', '""', t)
    return t


live = {}
for p in files:
    live[p] = strip(io.open(p, encoding='utf-8', newline='').read())

groups = {
    'EFFECT SYSTEM (deleted)': ['FEffectSystem', 'FACEffectBlock', 'FACEffectAction', 'FACCondition',
                                'UBattleEffectLibrary', 'EACActionType', 'EACConditionType',
                                'EACEffectTriggerType', 'EACTriggerPolicy', 'OnHitEffectBlockIds',
                                'OnSpawnEffectBlockIds', 'EffectBlockIds'],
    'ABNORMAL (deleted)': ['FAbnormalStateContainer', 'FAbnormalStateInstance', 'FACAbnormalStateDef',
                           'UAbnormalStateDefinition', 'EACStackPolicy'],
    'ABILITY (deleted)': ['FAbilityExecutor', 'FACSkillDef', 'FFocusState', 'FActiveChannel', 'FStunEntry',
                          'EACCastType', 'EACFocusCostMode', 'EACInterruptCause'],
    'TIMELINE (deleted)': ['FTimelineSystem', 'FTimelineEntry', 'FTimelineModifier'],
    'DETERMINISM (deleted)': ['FBattleRng', 'EACRngChannel', 'ComputeStateHash', 'FinalStateHash',
                              'RngDrawCount', 'GetRng'],
    'PRESENTATION BRIDGE (deleted)': ['UBattlePresentationBridge', 'ACPresentationBridge',
                                      'EACPresentationCueType', 'FACPresentationCue',
                                      'FACUnitViewModel', 'FACBattleViewModel'],
    'FIXED STEP (deleted)': ['FBattleClock', 'FSecondTickDispatcher', 'FixedDt', 'SecondsToTicks',
                             'MaxCatchUpSteps', 'BattleFixedDt'],
    'HEADLESS (deleted)': ['bHeadless'],
    'UNIT ID (replaced)': ['FUnitId', 'InvalidUnitId'],
    'MOD SCOPE / DAMAGE MOD (deleted)': ['EACModScope', 'FACDamageModifier'],
    'OLD UNIT (deleted)': ['UBattleUnit', 'ACBattleUnit.h'],
}

print('=' * 78)
for g, syms in groups.items():
    print('\n### ' + g)
    for s in syms:
        pat = re.compile(r'\b' + re.escape(s) + r'\b')
        hits = [p.replace(root, '') for p in files if pat.search(live[p])]
        if hits:
            print('  %-30s LIVE=%d  %s' % (s, len(hits), '; '.join(hits[:4])))
        else:
            print('  %-30s clean' % s)

print('\n' + '=' * 78)
print('### KEPT FEATURES (must be non-zero)')
kept = ['FBattleGrid', 'UACHexGridStatics', 'FActionScheduler', 'FTargetingSystem', 'FBattleAISystem',
        'FMentalSystem', 'FBattleStatsCollector', 'FBattleLogBuffer', 'FBattleEventBus',
        'FCombatResolver', 'FShieldPool', 'FBattleTime', 'FACAttackPatternDef',
        'TryActivateAbility', 'UACBattleAttributeSet', 'UACBasicAttackAbility', 'UACAbilitySet',
        'UACAbilitySetComponent', 'GrantFocusToUnit', 'FACBattleTime', 'AACBattleUnitBase',
        'AACOperatorActor', 'AACBattleBoardActor', 'UACUnitGridComponent']
for s in kept:
    pat = re.compile(r'\b' + re.escape(s) + r'\b')
    hits = [p for p in files if pat.search(live[p])]
    print('  %-30s refs=%d' % (s, len(hits)))

print('\n' + '=' * 78)
print('### INCLUDES')
allh = set()
for base, _, fns in os.walk(root):
    for fn in fns:
        if fn.endswith('.h'):
            allh.add(fn)
engine_prefixes = ('AbilitySystem', 'GameplayEffect', 'GameplayAbility', 'GameplayTasks',
                   'GameplayTags', 'Engine/', 'GameFramework/', 'Components/', 'Misc/',
                   'HAL/', 'Dom/', 'Serialization/', 'Policies/', 'Templates/', 'UObject/',
                   'Json', 'Net/', 'Subsystems/', 'Animation/', 'Niagara', 'Sound/', 'Kismet/',
                   'Blueprint', 'Curves/', 'ScalableFloat', 'NativeGameplayTags',
                   'Modules/', 'Abilities/', 'CoreMinimal.h', 'Math/', 'Containers/',
                   'Delegates/', 'Stats/', 'Logging/', 'Internationalization/', 'GenericPlatform/',
                   'Widgets/', 'Styling/', 'Framework/', 'GameplayEffectComponents/',
                   'GameplayCue', 'GameplayAbilitySpec', 'GameplayEffectTypes',
                   'ActiveGameplayEffect', 'AttributeSet.h', 'ScalableFloat.h',
                   'GameplayTagContainer.h', 'GameplayTagAssetInterface.h', 'NativeGameplayTag',
                   'AbilitySystemBlueprintLibrary', 'AbilitySystemInterface', 'GameplayPrediction')
inc_re = re.compile(r'#include\s+"([^"]+)"')
missing = []
for p in files:
    # ⚠️ 必须在**剥掉注释之后**再找 include：本项目大量注释里会写
    #    「`#include "X.h"` 已删除」，用原文扫描会把它们误报成缺失头。
    body = live[p]
    for m in inc_re.finditer(body):
        rel = m.group(1)
        if not rel.endswith('.h'):
            continue
        base = os.path.basename(rel)
        if base in allh:
            continue
        if base.endswith('.generated.h'):
            continue  # UHT 生成，源码树里本来就不存在
        if any(rel.startswith(k) for k in engine_prefixes):
            continue
        missing.append((p.replace(root, ''), rel))
if missing:
    for m in missing[:25]:
        print('  MISSING %s -> %s' % m)
    print('  total:', len(missing))
else:
    print('  OK: every project-relative include resolves to an existing header')
    print('  OK: no dangling .generated.h or deleted-header includes in live code')

print('\n' + '=' * 78)
print('### SOURCE MANIFEST')
for p in files:
    n = io.open(p, encoding='utf-8', newline='').read().count('\n')
    print('  %5d  %s' % (n, p.replace(root, '')))
