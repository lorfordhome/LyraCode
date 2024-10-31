// Copyright Epic Games, Inc. All Rights Reserved.

#include "LyraCharacterMovementComponent.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LyraCharacterMovementComponent)

UE_DEFINE_GAMEPLAY_TAG(TAG_Gameplay_MovementStopped, "Gameplay.MovementStopped");

namespace LyraCharacter
{
	static float GroundTraceDistance = 100000.0f;
	FAutoConsoleVariableRef CVar_GroundTraceDistance(TEXT("LyraCharacter.GroundTraceDistance"), GroundTraceDistance, TEXT("Distance to trace down when generating ground information."), ECVF_Cheat);
};


ULyraCharacterMovementComponent::ULyraCharacterMovementComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void ULyraCharacterMovementComponent::SimulateMovement(float DeltaTime)
{
	if (bHasReplicatedAcceleration)
	{
		// Preserve our replicated acceleration
		const FVector OriginalAcceleration = Acceleration;
		Super::SimulateMovement(DeltaTime);
		Acceleration = OriginalAcceleration;
	}
	else
	{
		Super::SimulateMovement(DeltaTime);
	}
}

bool ULyraCharacterMovementComponent::CanAttemptJump() const
{
	// Same as UCharacterMovementComponent's implementation but without the crouch check
	return IsJumpAllowed() &&
		(IsMovingOnGround() || IsFalling()); // Falling included for double-jump and non-zero jump hold time, but validated by character.
}

void ULyraCharacterMovementComponent::InitializeComponent()
{
	Super::InitializeComponent();
}

const FLyraCharacterGroundInfo& ULyraCharacterMovementComponent::GetGroundInfo()
{
	if (!CharacterOwner || (GFrameCounter == CachedGroundInfo.LastUpdateFrame))
	{
		return CachedGroundInfo;
	}

	if (MovementMode == MOVE_Walking)
	{
		CachedGroundInfo.GroundHitResult = CurrentFloor.HitResult;
		CachedGroundInfo.GroundDistance = 0.0f;
	}
	else
	{
		const UCapsuleComponent* CapsuleComp = CharacterOwner->GetCapsuleComponent();
		check(CapsuleComp);

		const float CapsuleHalfHeight = CapsuleComp->GetUnscaledCapsuleHalfHeight();
		const ECollisionChannel CollisionChannel = (UpdatedComponent ? UpdatedComponent->GetCollisionObjectType() : ECC_Pawn);
		const FVector TraceStart(GetActorLocation());
		const FVector TraceEnd(TraceStart.X, TraceStart.Y, (TraceStart.Z - LyraCharacter::GroundTraceDistance - CapsuleHalfHeight));

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(LyraCharacterMovementComponent_GetGroundInfo), false, CharacterOwner);
		FCollisionResponseParams ResponseParam;
		InitCollisionParams(QueryParams, ResponseParam);

		FHitResult HitResult;
		GetWorld()->LineTraceSingleByChannel(HitResult, TraceStart, TraceEnd, CollisionChannel, QueryParams, ResponseParam);

		CachedGroundInfo.GroundHitResult = HitResult;
		CachedGroundInfo.GroundDistance = LyraCharacter::GroundTraceDistance;

		if (MovementMode == MOVE_NavWalking)
		{
			CachedGroundInfo.GroundDistance = 0.0f;
		}
		else if (HitResult.bBlockingHit)
		{
			CachedGroundInfo.GroundDistance = FMath::Max((HitResult.Distance - CapsuleHalfHeight), 0.0f);
		}
	}

	CachedGroundInfo.LastUpdateFrame = GFrameCounter;

	return CachedGroundInfo;
}

void ULyraCharacterMovementComponent::SetReplicatedAcceleration(const FVector& InAcceleration)
{
	bHasReplicatedAcceleration = true;
	Acceleration = InAcceleration;
}

FRotator ULyraCharacterMovementComponent::GetDeltaRotation(float DeltaTime) const
{
	if (UAbilitySystemComponent* ASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(GetOwner()))
	{
		if (ASC->HasMatchingGameplayTag(TAG_Gameplay_MovementStopped))
		{
			return FRotator(0,0,0);
		}
	}

	return Super::GetDeltaRotation(DeltaTime);
}
float ULyraCharacterMovementComponent::GetMaxSpeed() const
{
	if (UAbilitySystemComponent* ASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(GetOwner()))
	{
		if (ASC->HasMatchingGameplayTag(TAG_Gameplay_MovementStopped))
		{
			return 0;
		}
	}

	return Super::GetMaxSpeed();
}
bool ULyraCharacterMovementComponent:: IsSliding() const
{
	return MovementMode==MOVE_Custom;
}
void ULyraCharacterMovementComponent::PhysCustom(float deltaTime, int32 Iterations)
{
	//observed from CharacterMovementComponent::PhysWalking()
	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}
	
	float remainingTime = deltaTime;
	bJustTeleported = false;

	//while loop observed from PhysWalking
	while ( (remainingTime >= MIN_TICK_TIME) && (Iterations < MaxSimulationIterations) && CharacterOwner && (CharacterOwner->Controller || bRunPhysicsWithNoController  || (CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy)) )
	{
		Iterations++; //this keeps track of how many times in a single frame a movement has been performed
		bJustTeleported = false;
		const float timeTick = GetSimulationTimeStep(remainingTime, Iterations);
		remainingTime -= timeTick;
		
		//save current values
		FVector OldLocation=UpdatedComponent->GetComponentLocation();
		FFindFloorResult OldFloor=CurrentFloor;

		MaintainHorizontalGroundVelocity();//makes sure the velocity is horizontal
		
		//Checks the slope of the current floor and applies gravity accordingly
		FVector FloorSlope = CurrentFloor.HitResult.Normal;
		FloorSlope.Z = 0.f;
		Velocity += FloorSlope * SlideGravityForce * deltaTime;
		
		//Calculate Acceleration
		Acceleration=FVector::ZeroVector; //don't want player to be able to control slide movement mode via WASD
		if( !HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() )//observed from PhysWalking ; but since we're not using root animations, this shouldn't actually affect anything
		{
			CalcVelocity(deltaTime,SlideFriction,false,GetMaxBrakingDeceleration());
		}
		ApplyRootMotionToVelocity(deltaTime);
		
		// Compute move parameters
		const FVector MoveVelocity = Velocity;
		const FVector Delta = timeTick * MoveVelocity;
		const bool bZeroDelta = Delta.IsNearlyZero();
		FStepDownResult StepDownResult;

		if ( bZeroDelta )
		{
			remainingTime = 0.f;
		}
		else
		{
			//execute movement
			MoveAlongFloor(MoveVelocity, timeTick, &StepDownResult);
		}
		//update floor
		if (StepDownResult.bComputedFloor)
		{
			CurrentFloor = StepDownResult.FloorResult;
		}
		else
		{
			FindFloor(UpdatedComponent->GetComponentLocation(), CurrentFloor, bZeroDelta, NULL);
		}
		//check for ledges
		const bool bCheckLedges = CanWalkOffLedges();//this is to make sure the character is actually allowed to walk off ledges - by default they should be
		if ( bCheckLedges && !CurrentFloor.IsWalkableFloor() )
		{
			EndSlide();
			StartFalling(Iterations, remainingTime, timeTick, Delta, OldLocation);
		}

		//check if the current surface is valid
		if (CurrentFloor.IsWalkableFloor())
		{
			//if the character should start falling
			if (ShouldCatchAir(OldFloor, CurrentFloor))
			{
				HandleWalkingOffLedge(OldFloor.HitResult.ImpactNormal, OldFloor.HitResult.Normal, OldLocation, timeTick);
				if (IsMovingOnGround())
				{
					// If still walking, then fall. If not, assume the user set a different mode they want to keep.
					StartFalling(Iterations, remainingTime, timeTick, Delta, OldLocation);
					EndSlide();
				}
				return;
			}
			//keeps character aligned with floor
			AdjustFloorHeight();
			SetBase(CurrentFloor.HitResult.Component.Get(), CurrentFloor.HitResult.BoneName);
		}
		
		// If we didn't move at all this iteration then abort (since future iterations will also be stuck).
		if (UpdatedComponent->GetComponentLocation()==OldLocation)
		{
			remainingTime=0.f;
			break;
		}
		
		// Make velocity reflect actual move
		if(!bJustTeleported&&!HasAnimRootMotion()&&!CurrentRootMotion.HasOverrideVelocity())
		{
			Velocity = (UpdatedComponent->GetComponentLocation() - OldLocation) / timeTick;
			MaintainHorizontalGroundVelocity();
		}
		//if velocity falls below certain threshold, take them out of the slide (but don't end crouching)
		if (!CanStartSlide())
		{
			SetMovementMode(MOVE_Walking);
		}
	}
	
}

void ULyraCharacterMovementComponent::BeginSlide()
{
	bWantsToCrouch=true; //crouching reduces capsule height, which we want to apply when sliding too.
	Velocity+=Velocity.GetSafeNormal()*SlideVelocityBonus;
	SetMovementMode(MOVE_Custom,1);
	FindFloor(UpdatedComponent->GetComponentLocation(),CurrentFloor,true, nullptr);
	
}
void ULyraCharacterMovementComponent::EndSlide()
{
	bWantsToCrouch=false;
	SetMovementMode(MOVE_Walking);
}

void ULyraCharacterMovementComponent::UpdateCharacterStateBeforeMovement(float DeltaSeconds)
{
	if (IsSliding()&&!bWantsToCrouch)//have they pressed the crouch button while sliding?
	{
		EndSlide();
	}
	Super::UpdateCharacterStateBeforeMovement(DeltaSeconds);
}

bool ULyraCharacterMovementComponent::IsMovingOnGround() const //without this override, won't detect sliding as moving on ground, which will mess with crouch detection
{
	return Super::IsMovingOnGround()||IsSliding();
}
bool ULyraCharacterMovementComponent::CanStartSlide() const //check if requirements for sliding are met
{
	return (Velocity.SizeSquared()>pow(SlideMinSpeed,2.f));
}
bool ULyraCharacterMovementComponent::CanCrouchInCurrentState() const
{
	return Super::CanCrouchInCurrentState()&&IsMovingOnGround();
}