import io, os, re

root = r'E:\UnrealProject\AutoChess\Source'
files = []
for base, _, fns in os.walk(root):
    for fn in fns:
        if fn.endswith(('.h', '.cpp')):
            files.append(os.path.join(base, fn))
files.sort()


def real_strip(src):
    """正确的 C++ 词法剥离：处理行注释、块注释、字符串、字符字面量、转义。"""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                i += 1
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            i += 2
            while i + 1 < n and not (src[i] == '*' and src[i + 1] == '/'):
                i += 1
            i += 2
        elif c == '"':
            i += 1
            while i < n and src[i] != '"':
                i += 2 if src[i] == '\\' else 1
            i += 1
        elif c == "'":
            i += 1
            while i < n and src[i] != "'":
                i += 2 if src[i] == '\\' else 1
            i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


bodies = {}
for p in files:
    bodies[p] = real_strip(io.open(p, encoding='utf-8', newline='').read())

print('=' * 72)
print('### 1. BALANCE (proper lexer)')
bad = []
for p in files:
    t = bodies[p]
    for o, c in (('{', '}'), ('(', ')'), ('[', ']')):
        if t.count(o) != t.count(c):
            bad.append((p.replace(root, ''), o, t.count(o), t.count(c)))
print('  unbalanced: %d' % len(bad))
for b in bad:
    print('    %s  %s %d/%d' % b)

print()
print('=' * 72)
print('### 2. INCLUDE RESOLVABILITY (live code only)')
allh = set()
for base, _, fns in os.walk(root):
    for fn in fns:
        if fn.endswith('.h'):
            allh.add(fn)
engine = ('AbilitySystem', 'GameplayEffect', 'GameplayAbility', 'GameplayTasks', 'GameplayTags',
          'Engine/', 'GameFramework/', 'Components/', 'Misc/', 'HAL/', 'Dom/', 'Serialization/',
          'Policies/', 'Templates/', 'UObject/', 'Json', 'Net/', 'Subsystems/', 'Animation/',
          'Niagara', 'Sound/', 'Kismet/', 'Blueprint', 'Curves/', 'ScalableFloat',
          'NativeGameplayTag', 'Modules/', 'Abilities/', 'CoreMinimal.h', 'Math/', 'Containers/',
          'Delegates/', 'Stats/', 'Logging/', 'Internationalization/', 'GenericPlatform/',
          'Widgets/', 'Styling/', 'Framework/', 'GameplayEffectComponents/', 'GameplayCue',
          'AbilitySystemBlueprintLibrary', 'AbilitySystemInterface', 'GameplayPrediction',
          'GameplayTagContainer.h', 'AttributeSet.h', 'ActiveGameplayEffect')
missing = []
for p in files:
    for m in re.finditer(r'#include\s+"([^"]+)"', bodies[p]):
        rel = m.group(1)
        if not rel.endswith('.h'):
            continue
        base = os.path.basename(rel)
        if base in allh or base.endswith('.generated.h'):
            continue
        if any(rel.startswith(k) for k in engine):
            continue
        missing.append((p.replace(root, ''), rel))
print('  unresolvable project includes: %d' % len(missing))
for m in missing:
    print('    %s -> %s' % m)

print()
print('=' * 72)
print('### 3. REFLECTIVE HEADERS: .generated.h LAST')
prob = []
for p in files:
    if not p.endswith('.h'):
        continue
    t = bodies[p]
    if not re.search(r'\b(UCLASS|USTRUCT|UENUM)\s*\(', t):
        continue
    incs = re.findall(r'#include\s+"([^"]+)"', t)
    if not incs:
        continue
    if not incs[-1].endswith('.generated.h'):
        prob.append((p.replace(root, ''), incs[-1]))
print('  problems: %d' % len(prob))
for x in prob:
    print('    %s (last include = %s)' % x)

print()
print('=' * 72)
print('### 4. DELETED SYMBOLS (live refs must be 0)')
dead = ['FEffectSystem', 'FACEffectBlock', 'UBattleEffectLibrary', 'EACActionType', 'EACConditionType',
        'FAbnormalStateContainer', 'FACAbnormalStateDef', 'EACStackPolicy', 'FAbilityExecutor',
        'FACSkillDef', 'FFocusState', 'FActiveChannel', 'FStunEntry', 'EACCastType', 'EACFocusCostMode',
        'EACInterruptCause', 'FTimelineSystem', 'FTimelineEntry', 'FBattleRng', 'EACRngChannel',
        'ComputeStateHash', 'FinalStateHash', 'RngDrawCount', 'UBattlePresentationBridge',
        'ACPresentationBridge', 'EACPresentationCueType', 'FACPresentationCue', 'FACUnitViewModel',
        'FACBattleViewModel', 'FBattleClock', 'FSecondTickDispatcher', 'FixedDt', 'SecondsToTicks',
        'MaxCatchUpSteps', 'bHeadless', 'EACModScope', 'FACDamageModifier', 'UBattleUnit',
        'EffectBlockIds', 'OnHitEffectBlockIds', 'RngSeed']
viol = 0
for s in dead:
    rx = re.compile(r'\b' + re.escape(s) + r'\b')
    hits = [p.replace(root, '') for p in files if rx.search(bodies[p])]
    if hits:
        viol += 1
        print('  VIOLATION %-28s %s' % (s, '; '.join(hits[:3])))
print('  violations: %d / %d checked' % (viol, len(dead)))

print()
print('=' * 72)
print('### 5. KEPT SUBSYSTEMS (must be > 0)')
for s in ['FBattleGrid', 'UACHexGridStatics', 'FActionScheduler', 'FTargetingSystem', 'FMentalSystem',
          'FBattleStatsCollector', 'FBattleLogBuffer', 'FBattleEventBus', 'FCombatResolver', 'FShieldPool',
          'TryActivateAbility', 'UACBattleAttributeSet', 'UACAbilitySetComponent', 'FACBattleTime',
          'AACBattleUnitBase', 'SetHealthFromResolver', 'GrantFocusToUnit']:
    rx = re.compile(r'\b' + re.escape(s) + r'\b')
    n = len([p for p in files if rx.search(bodies[p])])
    print('  %-28s %d' % (s, n))

print()
print('=' * 72)
print('### 6. ENCODING')
bad2 = []
for p in files:
    raw = open(p, 'rb').read()
    if raw[:3] == b'\xef\xbb\xbf':
        bad2.append((p.replace(root, ''), 'BOM'))
    if b'\r\n' in raw:
        bad2.append((p.replace(root, ''), 'CRLF'))
    if '\ufffd' in raw.decode('utf-8', 'replace'):
        bad2.append((p.replace(root, ''), 'U+FFFD'))
print('  problems: %d' % len(bad2))
for x in bad2:
    print('    %s : %s' % x)

print()
print('=' * 72)
print('### 7. TOTALS')
tot = 0
for p in files:
    tot += io.open(p, encoding='utf-8', newline='').read().count('\n')
print('  files: %d   lines: %d' % (len(files), tot))
