#include "Operator_Solivar.h"

#include "Engine/SkeletalMeshSocket.h"

AOperator_Solivar::AOperator_Solivar()
{
	AxeWeaponMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("AxeWeaponMesh"));
	if (SkeletalMeshComponent != nullptr)
	{
		if (const auto RightHandSocket = SkeletalMeshComponent->GetSocketByName("hand_rSocket"))
		{
			AxeWeaponMesh->SetupAttachment(SkeletalMeshComponent, RightHandSocket->GetFName());
		}
	}
}
