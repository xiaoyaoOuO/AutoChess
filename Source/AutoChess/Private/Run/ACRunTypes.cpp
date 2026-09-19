#include "Run/ACRunTypes.h"

namespace FACRunLevelName
{
    FText ToDisplayText(int32 Level)
    {
        switch (FMath::Clamp(Level, 0, 4))
        {
        case 0:  return FText::FromString(TEXT("D"));
        case 1:  return FText::FromString(TEXT("C"));
        case 2:  return FText::FromString(TEXT("B"));
        case 3:  return FText::FromString(TEXT("A"));
        default: return FText::FromString(TEXT("S"));
        }
    }
}
