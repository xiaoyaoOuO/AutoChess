#pragma once
#include "CoreMinimal.h"
#include "Battle/ACOperatorActor.h"
#include "Operator_Solivar.generated.h"

UCLASS()
class AUTOCHESSBATTLE_API AOperator_Solivar : public AACOperatorActor
{
	GENERATED_BODY()
public:
	AOperator_Solivar();
	
	UPROPERTY(EditDefaultsOnly)
	TObjectPtr<USkeletalMeshComponent> AxeWeaponMesh;
};
