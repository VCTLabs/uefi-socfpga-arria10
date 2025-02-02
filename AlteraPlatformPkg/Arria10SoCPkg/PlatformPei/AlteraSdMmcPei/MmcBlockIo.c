/** @file

  Portions of the code modified by Altera to support SoC devices are licensed as follows:
  Copyright (c) 2015, Altera Corporation. All rights reserved.

  Redistribution and use in source and binary forms, with or without modification,
  are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or other
  materials provided with the distribution.

  3. Neither the name of the copyright holder nor the names of its contributors may
  be used to endorse or promote products derived from this software without specific
  prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
  OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
  SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
  TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
  DAMAGE.

  The original software modules are licensed as follows:

  Copyright (c) 2011-2014, ARM Limited. All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD 3 Clause LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <Library/BaseMemoryLib.h>
#include <Library/SerialPortPrintLib.h>
#include <Library/TimerLib.h>
#include "Mmc.h"

#if (FixedPcdGet32(PcdDebugMsg_SdMmc) == 0)
//  #define ProgressPrint(FormatString, ...)    /* do nothing */
  #define ProgressPrint                       SerialPortPrint
  #define InfoPrint(FormatString, ...)        /* do nothing */
  #define MmioHexDumpEx(BaseAddr, Data32Size, PrintLineStartAddress)   /* do nothing */
#else
  #define ProgressPrint  SerialPortPrint
  #define InfoPrint      SerialPortPrint
  #define MmioHexDumpEx  SerialPortMmioHexDumpEx
#endif

EFI_STATUS
MmcNotifyState (
  IN PEI_SDMMC_INSTANCE *MmcHostInstance,
  IN MMC_STATE State
  )
{
  MmcHostInstance->State = State;
  return MmcHostInstance->MmcHost->NotifyState (MmcHostInstance->MmcHost, State);
}

EFI_STATUS
EFIAPI
MmcGetCardStatus (
  IN PEI_SDMMC_INSTANCE    *MmcHostInstance
  )
{
  EFI_STATUS              Status;
  UINT32                  Response[4];
  UINTN                   CmdArg;
  EFI_MMC_HOST_PROTOCOL   *MmcHost;

  Status = EFI_SUCCESS;
  MmcHost = MmcHostInstance->MmcHost;
  CmdArg = 0;

  if (MmcHost == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (MmcHostInstance->State != MmcHwInitializationState) {
    //Get the Status of the card.
    CmdArg = MmcHostInstance->CardInfo.RCA << 16;
    Status = MmcHost->SendCommand (MmcHost, MMC_CMD13, CmdArg);
    if (EFI_ERROR (Status)) {
      //DEBUG ((EFI_D_ERROR, "MmcGetCardStatus(MMC_CMD13): Error and Status = %r\n", Status));
      return Status;
    }

    //Read Response
    MmcHost->ReceiveResponse (MmcHost, MMC_RESPONSE_TYPE_R1, Response);
    PrintResponseR1 (Response[0]);
  }

  return Status;
}

EFI_STATUS
EFIAPI
MmcReset (
  IN EFI_BLOCK_IO_PROTOCOL    *This,
  IN BOOLEAN                  ExtendedVerification
  )
{
  PEI_SDMMC_INSTANCE      *MmcHostInstance;

  MmcHostInstance = PEI_SDMMC_INSTANCE_FROM_BLOCK_IO_THIS (This);

  if (MmcHostInstance->MmcHost == NULL) {
    // Nothing to do
    return EFI_SUCCESS;
  }

  // If a card is not present then clear all media settings
  if (!MmcHostInstance->MmcHost->IsCardPresent (MmcHostInstance->MmcHost)) {
    MmcHostInstance->BlockIo.Media->MediaPresent = FALSE;
    MmcHostInstance->BlockIo.Media->LastBlock    = 0;
    MmcHostInstance->BlockIo.Media->BlockSize    = 512;  // Should be zero but there is a bug in DiskIo
    MmcHostInstance->BlockIo.Media->ReadOnly     = FALSE;

    // Indicate that the driver requires initialization
    MmcHostInstance->State = MmcHwInitializationState;

    return EFI_SUCCESS;
  }

  // Implement me. Either send a CMD0 (could not work for some MMC host) or just turn off/turn
  //      on power and restart Identification mode
  return EFI_SUCCESS;
}

EFI_STATUS
MmcDetectCard (
  EFI_MMC_HOST_PROTOCOL     *MmcHost
  )
{
  if (!MmcHost->IsCardPresent (MmcHost)) {
    return EFI_NO_MEDIA;
  } else {
    return EFI_SUCCESS;
  }
}

EFI_STATUS
MmcStopTransmission (
  EFI_MMC_HOST_PROTOCOL     *MmcHost
  )
{
  EFI_STATUS              Status;
  UINT32                  Response[4];
  // Command 12 - Stop transmission (ends read or write)
  // Normally only needed for streaming transfers or after error.
  Status = MmcHost->SendCommand (MmcHost, MMC_CMD12, 0);
  if (!EFI_ERROR (Status)) {
    MmcHost->ReceiveResponse (MmcHost, MMC_RESPONSE_TYPE_R1b, Response);
  }
  return Status;
}


EFI_STATUS
MmcIoBlocks (
  IN EFI_BLOCK_IO_PROTOCOL    *This,
  IN UINTN                    Transfer,
  IN UINT32                   MediaId,
  IN EFI_LBA                  Lba,
  IN UINTN                    BufferSize,
  OUT VOID                    *Buffer
  )
{
  UINT32                  Response[4];
  EFI_STATUS              Status;
  UINTN                   CmdArg;
  INTN                    Timeout;
  UINTN                   Cmd;
  PEI_SDMMC_INSTANCE      *MmcHostInstance;
  EFI_MMC_HOST_PROTOCOL   *MmcHost;
  UINTN                   BytesRemainingToBeTransfered;
  UINTN                   BlockCount;

  BlockCount = 1;
  MmcHostInstance = PEI_SDMMC_INSTANCE_FROM_BLOCK_IO_THIS (This);
  ASSERT (MmcHostInstance != NULL);
  MmcHost = MmcHostInstance->MmcHost;
  ASSERT (MmcHost);

  if (This->Media->MediaId != MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((MmcHost == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  // Check if a Card is Present
  if (!MmcHostInstance->BlockIo.Media->MediaPresent) {
    return EFI_NO_MEDIA;
  }

  // All blocks must be within the device
  if ((Lba + (BufferSize / This->Media->BlockSize)) > (This->Media->LastBlock + 1)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Transfer == MMC_IOBLOCKS_WRITE) && (This->Media->ReadOnly == TRUE)) {
    return EFI_WRITE_PROTECTED;
  }

  // Reading 0 Byte is valid
  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }

  // The buffer size must be an exact multiple of the block size
  if ((BufferSize % This->Media->BlockSize) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  // Check the alignment
  if ((This->Media->IoAlign > 2) && (((UINTN)Buffer & (This->Media->IoAlign - 1)) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Transfer == MMC_IOBLOCKS_READ) {
    // Do 4 KB multi block read ?
    if (BufferSize >= 4096)
    {
      BlockCount = 4096 / This->Media->BlockSize;
    }
  }

  BytesRemainingToBeTransfered = BufferSize;
  while (BytesRemainingToBeTransfered > 0) {

    // Check if the Card is in Ready status
    CmdArg = MmcHostInstance->CardInfo.RCA << 16;
    Response[0] = 0;
    Timeout = 20;
    while(   (!(Response[0] & MMC_R0_READY_FOR_DATA))
          && (MMC_R0_CURRENTSTATE (Response) != MMC_R0_STATE_TRAN)
          && Timeout--) {
      Status = MmcHost->SendCommand (MmcHost, MMC_CMD13, CmdArg);
      if (!EFI_ERROR (Status)) {
        MmcHost->ReceiveResponse (MmcHost, MMC_RESPONSE_TYPE_R1, Response);
      }
    }

    if (0 == Timeout) {
      //DEBUG ((EFI_D_ERROR, "The Card is busy\n"));
      return EFI_NOT_READY;
    }

    //Set command argument based on the card access mode (Byte mode or Block mode)
    if (MmcHostInstance->CardInfo.OCRData.AccessMode & BIT1) {
      CmdArg = Lba;
    } else {
      CmdArg = Lba * This->Media->BlockSize;
    }

    if (Transfer == MMC_IOBLOCKS_READ) {
      if (BlockCount == 1) {
        // Read a single block
        Cmd = MMC_CMD17;
      } else {
        // Read multi block
        Cmd = MMC_CMD18;
      }
    } else {
      if (BlockCount == 1) {
        // Write a single block
        Cmd = MMC_CMD24;
      } else {
        // Multi block write unimplemented
        // Should not come here if nobody modify this function
        ASSERT (0);
      }
    }
    Status = MmcHost->SendCommand (MmcHost, Cmd, CmdArg);
    if (EFI_ERROR (Status)) {
      //DEBUG ((EFI_D_ERROR, "MmcIoBlocks(MMC_CMD%d): Error %r\n", Cmd, Status));
      return Status;
    }

    if (Transfer == MMC_IOBLOCKS_READ) {
      // Read block(s) of Data
      Status = MmcHost->ReadBlockData (MmcHost, Lba, BlockCount * This->Media->BlockSize, Buffer);
      if (EFI_ERROR (Status)) {
        //DEBUG ((EFI_D_BLKIO, "MmcIoBlocks(): Error Read Block Data and Status = %r\n", Status));
        MmcStopTransmission (MmcHost);
        return Status;
      }
      Status = MmcNotifyState (MmcHostInstance, MmcProgrammingState);
      if (EFI_ERROR (Status)) {
        //DEBUG ((EFI_D_ERROR, "MmcIoBlocks() : Error MmcProgrammingState\n"));
        return Status;
      }
    } else {
      // Write block(s) of Data
      Status = MmcHost->WriteBlockData (MmcHost, Lba, BlockCount * This->Media->BlockSize, Buffer);
      if (EFI_ERROR (Status)) {
        //DEBUG ((EFI_D_BLKIO, "MmcIoBlocks(): Error Write Block Data and Status = %r\n", Status));
        MmcStopTransmission (MmcHost);
        return Status;
      }
    }

    // Command 13 - Read status and wait for programming to complete (return to tran)
    #define MMCI0_TIMEOUT  10000
    Timeout = MMCI0_TIMEOUT;
    CmdArg = MmcHostInstance->CardInfo.RCA << 16;
    Response[0] = 0;
    while(   (!(Response[0] & MMC_R0_READY_FOR_DATA))
          && (MMC_R0_CURRENTSTATE (Response) != MMC_R0_STATE_TRAN)
          && Timeout--) {
      Status = MmcHost->SendCommand (MmcHost, MMC_CMD13, CmdArg);
      if (!EFI_ERROR (Status)) {
        MmcHost->ReceiveResponse (MmcHost, MMC_RESPONSE_TYPE_R1, Response);
        if ((Response[0] & MMC_R0_READY_FOR_DATA)) {
          break;  // Prevents delay once finished
        }
      }
      NanoSecondDelay (100);
    }

    Status = MmcNotifyState (MmcHostInstance, MmcTransferState);
    if (EFI_ERROR (Status)) {
      //DEBUG ((EFI_D_ERROR, "MmcIoBlocks() : Error MmcTransferState\n"));
      return Status;
    }

    BytesRemainingToBeTransfered -= BlockCount * This->Media->BlockSize;
    Lba += BlockCount;
    Buffer = (UINT8 *)Buffer + (BlockCount * This->Media->BlockSize);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MmcReadBlocks (
  IN EFI_BLOCK_IO_PROTOCOL    *This,
  IN UINT32                   MediaId,
  IN EFI_LBA                  Lba,
  IN UINTN                    BufferSize,
  OUT VOID                    *Buffer
  )
{
  return MmcIoBlocks (This, MMC_IOBLOCKS_READ, MediaId, Lba, BufferSize, Buffer);
}

EFI_STATUS
EFIAPI
MmcWriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL    *This,
  IN UINT32                   MediaId,
  IN EFI_LBA                  Lba,
  IN UINTN                    BufferSize,
  IN VOID                     *Buffer
  )
{
  return MmcIoBlocks (This, MMC_IOBLOCKS_WRITE, MediaId, Lba, BufferSize, Buffer);
}

EFI_STATUS
EFIAPI
MmcFlushBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

