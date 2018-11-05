/** @file
  DXE capsule process.

  Caution: This module requires additional review when modified.
  This module will have external input - capsule image.
  This external input must be validated carefully to avoid security issue like
  buffer overflow, integer overflow.

  ProcessCapsules(), ProcessTheseCapsules() will receive untrusted
  input and do basic validation.

  Copyright (c) 2016 - 2018, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiDxe.h>
#include <Protocol/EsrtManagement.h>
#include <Protocol/FirmwareManagementProgress.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiLib.h>
#include <Library/PcdLib.h>
#include <Library/HobLib.h>
#include <Library/ReportStatusCodeLib.h>
#include <Library/CapsuleLib.h>
#include <Library/DisplayUpdateProgressLib.h>
#include <Library/ResetUtilityLib.h>            // MU_CHANGE - Use the enhanced reset subtype.

#include <IndustryStandard/WindowsUxCapsule.h>

extern EDKII_FIRMWARE_MANAGEMENT_PROGRESS_PROTOCOL  *mFmpProgress;

/**
  Return if this FMP is a system FMP or a device FMP, based upon CapsuleHeader.

  @param[in] CapsuleHeader A pointer to EFI_CAPSULE_HEADER

  @retval TRUE  It is a system FMP.
  @retval FALSE It is a device FMP.
**/
BOOLEAN
IsFmpCapsule (
  IN EFI_CAPSULE_HEADER  *CapsuleHeader
  );

/**
  Validate Fmp capsules layout.

  Caution: This function may receive untrusted input.

  This function assumes the caller validated the capsule by using
  IsValidCapsuleHeader(), so that all fields in EFI_CAPSULE_HEADER are correct.
  The capsule buffer size is CapsuleHeader->CapsuleImageSize.

  This function validates the fields in EFI_FIRMWARE_MANAGEMENT_CAPSULE_HEADER
  and EFI_FIRMWARE_MANAGEMENT_CAPSULE_IMAGE_HEADER.

  This function need support nested FMP capsule.

  @param[in]   CapsuleHeader        Points to a capsule header.
  @param[out]  EmbeddedDriverCount  The EmbeddedDriverCount in the FMP capsule.

  @retval EFI_SUCESS             Input capsule is a correct FMP capsule.
  @retval EFI_INVALID_PARAMETER  Input capsule is not a correct FMP capsule.
**/
EFI_STATUS
ValidateFmpCapsule (
  IN EFI_CAPSULE_HEADER  *CapsuleHeader,
  OUT UINT16             *EmbeddedDriverCount OPTIONAL
  );

/**
  Validate if it is valid capsule header

  This function assumes the caller provided correct CapsuleHeader pointer
  and CapsuleSize.

  This function validates the fields in EFI_CAPSULE_HEADER.

  @param[in]  CapsuleHeader    Points to a capsule header.
  @param[in]  CapsuleSize      Size of the whole capsule image.

**/
BOOLEAN
IsValidCapsuleHeader (
  IN EFI_CAPSULE_HEADER  *CapsuleHeader,
  IN UINT64              CapsuleSize
  );

extern BOOLEAN                   mDxeCapsuleLibEndOfDxe;
BOOLEAN                          mNeedReset = FALSE;

VOID                        **mCapsulePtr;
EFI_STATUS                  *mCapsuleStatusArray;
UINT32                      mCapsuleTotalNumber;

/**
  The firmware implements to process the capsule image.

  Caution: This function may receive untrusted input.

  @param[in]  CapsuleHeader         Points to a capsule header.
  @param[out] ResetRequired         Indicates whether reset is required or not.

  @retval EFI_SUCESS            Process Capsule Image successfully.
  @retval EFI_UNSUPPORTED       Capsule image is not supported by the firmware.
  @retval EFI_VOLUME_CORRUPTED  FV volume in the capsule is corrupted.
  @retval EFI_OUT_OF_RESOURCES  Not enough memory.
**/
EFI_STATUS
EFIAPI
ProcessThisCapsuleImage (
  IN EFI_CAPSULE_HEADER  *CapsuleHeader,
  OUT BOOLEAN            *ResetRequired OPTIONAL
  );

/**
  Function indicate the current completion progress of the firmware
  update. Platform may override with own specific progress function.

  @param[in]  Completion  A value between 1 and 100 indicating the current
                          completion progress of the firmware update

  @retval EFI_SUCESS             The capsule update progress was updated.
  @retval EFI_INVALID_PARAMETER  Completion is greater than 100%.
**/
EFI_STATUS
EFIAPI
UpdateImageProgress (
  IN UINTN  Completion
  )
{
  EFI_STATUS                           Status;
  UINTN                                Seconds;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL_UNION  *Color;

  DEBUG((DEBUG_INFO, "Update Progress - %d%%\n", Completion));

  if (Completion > 100) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Use a default timeout of 5 minutes if there is not FMP Progress Protocol.
  //
  Seconds = 5 * 60;
  Color   = NULL;
  if (mFmpProgress != NULL) {
    Seconds = mFmpProgress->WatchdogSeconds;
    Color   = &mFmpProgress->ProgressBarForegroundColor;
  }

  //
  // Cancel the watchdog timer
  //
  gBS->SetWatchdogTimer (0, 0x0000, 0, NULL);

  if (Completion != 100) {
    //
    // Arm the watchdog timer from PCD setting
    //
    if (Seconds != 0) {
      DEBUG ((DEBUG_VERBOSE, "Arm watchdog timer %d seconds\n", Seconds));
      gBS->SetWatchdogTimer (Seconds, 0x0000, 0, NULL);
    }
  }

  Status = DisplayUpdateProgress (Completion, Color);

  return Status;
}

/**
  This function initializes the mCapsulePtr, mCapsuleStatusArray and mCapsuleTotalNumber.
**/
VOID
InitCapsulePtr (
  VOID
  )
{
  EFI_PEI_HOB_POINTERS        HobPointer;
  UINTN                       Index;

  //
  // Find all capsule images from hob
  //
  HobPointer.Raw = GetHobList ();
  while ((HobPointer.Raw = GetNextHob (EFI_HOB_TYPE_UEFI_CAPSULE, HobPointer.Raw)) != NULL) {
    if (!IsValidCapsuleHeader((VOID *)(UINTN)HobPointer.Capsule->BaseAddress, HobPointer.Capsule->Length)) {
      HobPointer.Header->HobType = EFI_HOB_TYPE_UNUSED; // Mark this hob as invalid
    } else {
      mCapsuleTotalNumber++;
    }
    HobPointer.Raw = GET_NEXT_HOB (HobPointer);
  }

  DEBUG ((DEBUG_INFO, "mCapsuleTotalNumber - 0x%x\n", mCapsuleTotalNumber));

  if (mCapsuleTotalNumber == 0) {
    return ;
  }

  //
  // Init temp Capsule Data table.
  //
  mCapsulePtr       = (VOID **) AllocateZeroPool (sizeof (VOID *) * mCapsuleTotalNumber);
  if (mCapsulePtr == NULL) {
    DEBUG ((DEBUG_ERROR, "Allocate mCapsulePtr fail!\n"));
    mCapsuleTotalNumber = 0;
    return ;
  }
  mCapsuleStatusArray = (EFI_STATUS *) AllocateZeroPool (sizeof (EFI_STATUS) * mCapsuleTotalNumber);
  if (mCapsuleStatusArray == NULL) {
    DEBUG ((DEBUG_ERROR, "Allocate mCapsuleStatusArray fail!\n"));
    FreePool (mCapsulePtr);
    mCapsulePtr = NULL;
    mCapsuleTotalNumber = 0;
    return ;
  }
  SetMemN (mCapsuleStatusArray, sizeof (EFI_STATUS) * mCapsuleTotalNumber, EFI_NOT_READY);

  //
  // Find all capsule images from hob
  //
  HobPointer.Raw = GetHobList ();
  Index = 0;
  while ((HobPointer.Raw = GetNextHob (EFI_HOB_TYPE_UEFI_CAPSULE, HobPointer.Raw)) != NULL) {
    mCapsulePtr [Index++] = (VOID *) (UINTN) HobPointer.Capsule->BaseAddress;
    HobPointer.Raw = GET_NEXT_HOB (HobPointer);
  }
}

/**
  This function returns if all capsule images are processed.

  @retval TRUE   All capsule images are processed.
  @retval FALSE  Not all capsule images are processed.
**/
BOOLEAN
AreAllImagesProcessed (
  VOID
  )
{
  UINTN  Index;

  for (Index = 0; Index < mCapsuleTotalNumber; Index++) {
    if (mCapsuleStatusArray[Index] == EFI_NOT_READY) {
      return FALSE;
    }
  }

  return TRUE;
}

/**
  This function populates capsule in the configuration table.
**/
VOID
PopulateCapsuleInConfigurationTable (
  VOID
  )
{
  VOID                        **CapsulePtrCache;
  EFI_GUID                    *CapsuleGuidCache;
  EFI_CAPSULE_HEADER          *CapsuleHeader;
  EFI_CAPSULE_TABLE           *CapsuleTable;
  UINT32                      CacheIndex;
  UINT32                      CacheNumber;
  UINT32                      CapsuleNumber;
  UINTN                       Index;
  UINTN                       Size;
  EFI_STATUS                  Status;

  if (mCapsuleTotalNumber == 0) {
    return ;
  }

  CapsulePtrCache     = NULL;
  CapsuleGuidCache    = NULL;
  CacheIndex          = 0;
  CacheNumber         = 0;

  CapsulePtrCache  = (VOID **) AllocateZeroPool (sizeof (VOID *) * mCapsuleTotalNumber);
  if (CapsulePtrCache == NULL) {
    DEBUG ((DEBUG_ERROR, "Allocate CapsulePtrCache fail!\n"));
    return ;
  }
  CapsuleGuidCache = (EFI_GUID *) AllocateZeroPool (sizeof (EFI_GUID) * mCapsuleTotalNumber);
  if (CapsuleGuidCache == NULL) {
    DEBUG ((DEBUG_ERROR, "Allocate CapsuleGuidCache fail!\n"));
    FreePool (CapsulePtrCache);
    return ;
  }

  //
  // Capsules who have CAPSULE_FLAGS_POPULATE_SYSTEM_TABLE always are used for operating
  // System to have information persist across a system reset. EFI System Table must
  // point to an array of capsules that contains the same CapsuleGuid value. And agents
  // searching for this type capsule will look in EFI System Table and search for the
  // capsule's Guid and associated pointer to retrieve the data. Two steps below describes
  // how to sorting the capsules by the unique guid and install the array to EFI System Table.
  // Firstly, Loop for all coalesced capsules, record unique CapsuleGuids and cache them in an
  // array for later sorting capsules by CapsuleGuid.
  //
  for (Index = 0; Index < mCapsuleTotalNumber; Index++) {
    CapsuleHeader = (EFI_CAPSULE_HEADER*) mCapsulePtr [Index];
    if ((CapsuleHeader->Flags & CAPSULE_FLAGS_POPULATE_SYSTEM_TABLE) != 0) {
      //
      // For each capsule, we compare it with known CapsuleGuid in the CacheArray.
      // If already has the Guid, skip it. Whereas, record it in the CacheArray as
      // an additional one.
      //
      CacheIndex = 0;
      while (CacheIndex < CacheNumber) {
        if (CompareGuid(&CapsuleGuidCache[CacheIndex],&CapsuleHeader->CapsuleGuid)) {
          break;
        }
        CacheIndex++;
      }
      if (CacheIndex == CacheNumber) {
        CopyMem(&CapsuleGuidCache[CacheNumber++],&CapsuleHeader->CapsuleGuid,sizeof(EFI_GUID));
      }
    }
  }

  //
  // Secondly, for each unique CapsuleGuid in CacheArray, gather all coalesced capsules
  // whose guid is the same as it, and malloc memory for an array which preceding
  // with UINT32. The array fills with entry point of capsules that have the same
  // CapsuleGuid, and UINT32 represents the size of the array of capsules. Then install
  // this array into EFI System Table, so that agents searching for this type capsule
  // will look in EFI System Table and search for the capsule's Guid and associated
  // pointer to retrieve the data.
  //
  for (CacheIndex = 0; CacheIndex < CacheNumber; CacheIndex++) {
    CapsuleNumber = 0;
    for (Index = 0; Index < mCapsuleTotalNumber; Index++) {
      CapsuleHeader = (EFI_CAPSULE_HEADER*) mCapsulePtr [Index];
      if ((CapsuleHeader->Flags & CAPSULE_FLAGS_POPULATE_SYSTEM_TABLE) != 0) {
        if (CompareGuid (&CapsuleGuidCache[CacheIndex], &CapsuleHeader->CapsuleGuid)) {
          //
          // Cache Caspuleheader to the array, this array is uniqued with certain CapsuleGuid.
          //
          CapsulePtrCache[CapsuleNumber++] = (VOID*)CapsuleHeader;
        }
      }
    }
    if (CapsuleNumber != 0) {
      Size = sizeof(EFI_CAPSULE_TABLE) + (CapsuleNumber - 1) * sizeof(VOID*);
      CapsuleTable = AllocateRuntimePool (Size);
      if (CapsuleTable == NULL) {
        DEBUG ((DEBUG_ERROR, "Allocate CapsuleTable (%g) fail!\n", &CapsuleGuidCache[CacheIndex]));
        continue;
      }
      CapsuleTable->CapsuleArrayNumber =  CapsuleNumber;
      CopyMem(&CapsuleTable->CapsulePtr[0], CapsulePtrCache, CapsuleNumber * sizeof(VOID*));
      Status = gBS->InstallConfigurationTable (&CapsuleGuidCache[CacheIndex], (VOID*)CapsuleTable);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "InstallConfigurationTable (%g) fail!\n", &CapsuleGuidCache[CacheIndex]));
      }
    }
  }

  FreePool(CapsuleGuidCache);
  FreePool(CapsulePtrCache);
}

/**

  This routine is called to process capsules.

  Caution: This function may receive untrusted input.

  Each individual capsule result is recorded in capsule record variable.

  @param[in]  FirstRound         TRUE:  First round. Need skip the FMP capsules with non zero EmbeddedDriverCount.
                                 FALSE: Process rest FMP capsules.

  @retval EFI_SUCCESS             There is no error when processing capsules.
  @retval EFI_OUT_OF_RESOURCES    No enough resource to process capsules.

**/
EFI_STATUS
ProcessTheseCapsules (
  IN BOOLEAN  FirstRound
  )
{
  EFI_STATUS                  Status;
  EFI_CAPSULE_HEADER          *CapsuleHeader;
  UINT32                      Index;
  ESRT_MANAGEMENT_PROTOCOL    *EsrtManagement;
  UINT16                      EmbeddedDriverCount;
  BOOLEAN                     ResetRequired;

  REPORT_STATUS_CODE(EFI_PROGRESS_CODE, (EFI_SOFTWARE | PcdGet32(PcdStatusCodeSubClassCapsule) | PcdGet32(PcdCapsuleStatusCodeProcessCapsulesBegin)));

  if (FirstRound) {
    InitCapsulePtr ();
  }

  if (mCapsuleTotalNumber == 0) {
    //
    // We didn't find a hob, so had no errors.
    //
    DEBUG ((DEBUG_ERROR, "We can not find capsule data in capsule update boot mode.\n"));
    return EFI_SUCCESS;
  }

  if (AreAllImagesProcessed ()) {
    return EFI_SUCCESS;
  }

  //
  // Check the capsule flags,if contains CAPSULE_FLAGS_POPULATE_SYSTEM_TABLE, install
  // capsuleTable to configure table with EFI_CAPSULE_GUID
  //
  if (FirstRound) {
    PopulateCapsuleInConfigurationTable ();
  }

  REPORT_STATUS_CODE(EFI_PROGRESS_CODE, (EFI_SOFTWARE | PcdGet32(PcdStatusCodeSubClassCapsule) | PcdGet32(PcdCapsuleStatusCodeUpdatingFirmware)));

  //
  // If Windows UX capsule exist, process it first
  //
  for (Index = 0; Index < mCapsuleTotalNumber; Index++) {
    CapsuleHeader = (EFI_CAPSULE_HEADER*) mCapsulePtr [Index];
    if (CompareGuid (&CapsuleHeader->CapsuleGuid, &gWindowsUxCapsuleGuid)) {
      DEBUG ((DEBUG_INFO, "ProcessThisCapsuleImage (Ux) - 0x%x\n", CapsuleHeader));
      DEBUG ((DEBUG_INFO, "Display logo capsule is found.\n"));
      Status = ProcessThisCapsuleImage (CapsuleHeader, NULL);
      mCapsuleStatusArray [Index] = EFI_SUCCESS;
      DEBUG((DEBUG_INFO, "ProcessThisCapsuleImage (Ux) - %r\n", Status));
      break;
    }
  }

  // MS_CHANGE - Printing to the screen is unacceptable in production FW.
  // Even if the graphics capsule didn't exist.
  // DEBUG ((DEBUG_INFO, "Updating the firmware ......\n"));

  //
  // All capsules left are recognized by platform.
  //
  for (Index = 0; Index < mCapsuleTotalNumber; Index++) {
    if (mCapsuleStatusArray [Index] != EFI_NOT_READY) {
      // already processed
      continue;
    }
    CapsuleHeader = (EFI_CAPSULE_HEADER*) mCapsulePtr [Index];
    if (!CompareGuid (&CapsuleHeader->CapsuleGuid, &gWindowsUxCapsuleGuid)) {
      //
      // Call capsule library to process capsule image.
      //
      EmbeddedDriverCount = 0;
      if (IsFmpCapsule(CapsuleHeader)) {
        Status = ValidateFmpCapsule (CapsuleHeader, &EmbeddedDriverCount);
        if (EFI_ERROR(Status)) {
          DEBUG((DEBUG_ERROR, "ValidateFmpCapsule failed. Ignore!\n"));
          mCapsuleStatusArray [Index] = EFI_ABORTED;
          continue;
        }
      } else {
        mCapsuleStatusArray [Index] = EFI_ABORTED;
        continue;
      }

      if ((!FirstRound) || (EmbeddedDriverCount == 0)) {
        DEBUG((DEBUG_INFO, "ProcessThisCapsuleImage - 0x%x\n", CapsuleHeader));
        ResetRequired = FALSE;
        Status = ProcessThisCapsuleImage (CapsuleHeader, &ResetRequired);
        mCapsuleStatusArray [Index] = Status;
        DEBUG((DEBUG_INFO, "ProcessThisCapsuleImage - %r\n", Status));

        if (Status != EFI_NOT_READY) {
          if (EFI_ERROR(Status)) {
            REPORT_STATUS_CODE(EFI_ERROR_CODE, (EFI_SOFTWARE | PcdGet32(PcdStatusCodeSubClassCapsule) | PcdGet32(PcdCapsuleStatusCodeUpdateFirmwareFailed)));
            DEBUG ((DEBUG_ERROR, "Capsule process failed!\n"));
          } else {
            REPORT_STATUS_CODE(EFI_PROGRESS_CODE, (EFI_SOFTWARE | PcdGet32(PcdStatusCodeSubClassCapsule) | PcdGet32(PcdCapsuleStatusCodeUpdateFirmwareSuccess)));
          }

          mNeedReset |= ResetRequired;
          if ((CapsuleHeader->Flags & PcdGet16(PcdSystemRebootAfterCapsuleProcessFlag)) != 0) {
            mNeedReset = TRUE;
          }
        }
      }
    }
  }

  Status = gBS->LocateProtocol(&gEsrtManagementProtocolGuid, NULL, (VOID **)&EsrtManagement);
  //
  // Always sync ESRT Cache from FMP Instance
  //
  if (!EFI_ERROR(Status)) {
    EsrtManagement->SyncEsrtFmp();
  }
  Status = EFI_SUCCESS;

  REPORT_STATUS_CODE(EFI_PROGRESS_CODE, (EFI_SOFTWARE | PcdGet32(PcdStatusCodeSubClassCapsule) | PcdGet32(PcdCapsuleStatusCodeProcessCapsulesEnd)));

  return Status;
}

/**
  Do reset system.
**/
VOID
DoResetSystem (
  VOID
  )
{
  DEBUG((DEBUG_INFO, "Capsule Request Cold Reboot."));

  REPORT_STATUS_CODE(EFI_PROGRESS_CODE, (EFI_SOFTWARE | PcdGet32(PcdStatusCodeSubClassCapsule) | PcdGet32(PcdCapsuleStatusCodeResettingSystem)));

  // MU_CHANGE - Use the enhanced reset subtype so that this reset can be filtered/handled
  //             in a platform-specific way.
  // gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
  ResetSystemWithSubtype( EfiResetCold, &gCapsuleUpdateCompleteResetGuid );

  CpuDeadLoop();
}

/**

  This routine is called to process capsules.

  Caution: This function may receive untrusted input.

  The capsules reported in EFI_HOB_UEFI_CAPSULE are processed.
  If there is no EFI_HOB_UEFI_CAPSULE, this routine does nothing.

  This routine should be called twice in BDS.
  1) The first call must be before EndOfDxe. The system capsules is processed.
     If device capsule FMP protocols are exposted at this time and device FMP
     capsule has zero EmbeddedDriverCount, the device capsules are processed.
     Each individual capsule result is recorded in capsule record variable.
     System may reset in this function, if reset is required by capsule and
     all capsules are processed.
     If not all capsules are processed, reset will be defered to second call.

  2) The second call must be after EndOfDxe and after ConnectAll, so that all
     device capsule FMP protocols are exposed.
     The system capsules are skipped. If the device capsules are NOT processed
     in first call, they are processed here.
     Each individual capsule result is recorded in capsule record variable.
     System may reset in this function, if reset is required by capsule
     processed in first call and second call.

  @retval EFI_SUCCESS             There is no error when processing capsules.
  @retval EFI_OUT_OF_RESOURCES    No enough resource to process capsules.

**/
EFI_STATUS
EFIAPI
ProcessCapsules (
  VOID
  )
{
  EFI_STATUS                    Status;

  // MS_CHANGE [BEGIN]
  // TEMP CHANGE - BDS is not currently wired to call this twice
  //                and 1) we block embedded drivers and 2) this all falls
  //                within our trust model, so we can execute in a single pass.
  /*
  if (!mDxeCapsuleLibEndOfDxe) {
    Status = ProcessTheseCapsules(TRUE);

    //
    // Reboot System if and only if all capsule processed.
    // If not, defer reset to 2nd process.
    //
    if (mNeedReset && AreAllImagesProcessed()) {
      DoResetSystem();
    }
  } else {
  */
    // Status = ProcessTheseCapsules(FALSE);
    Status = ProcessTheseCapsules(TRUE);
    //
    // Reboot System if required after all capsule processed
    //
    if (mNeedReset) {
      DoResetSystem();
    }
  // }
  // MS_CHANGE [END]
  return Status;
}

// MS_CHANGE [BEGIN] - This implementation is BootServices/Runtime specific.
//                     Move most business logic into specific Dxe vs RuntimeDxe libs.
/**
  Function indicate the current completion progress of the firmware
  update. Platform may override with own specific progress function.

  @param[in]  Completion    A value between 1 and 100 indicating the current completion progress of the firmware update

  @retval EFI_SUCESS    Input capsule is a correct FMP capsule.
**/
EFI_STATUS
EFIAPI
Update_Image_Progress (
  IN UINTN  Completion
  )
{
  EFI_STATUS                            Status;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL_UNION   Color;

  Color.Raw = (UINT32)((Completion >> 8) & 0xFFFFFF);
  Completion &= 0xFF;  //only the lower 8bits matter for percentage.

  DEBUG((DEBUG_INFO, "Update Progress - %d%%\n", Completion));

  if (Completion > 100) {
    return EFI_INVALID_PARAMETER;
  }

  //PET WATCHDOG
  //cancel watchdog
  gBS->SetWatchdogTimer(0x0000, 0x0000, 0x0000, NULL);
  if (Completion != 100)
  {
    //start watchdog
    gBS->SetWatchdogTimer((UINTN)PcdGet8(PcdCapsuleUpdateWatchdogTimeInSeconds), 0x0000, 0x00, NULL);
  }

  Status = DisplayUpdateProgress(Completion, &Color);

  return Status;
}
// MS_CHANGE [END]
