/*
PV Drivers for Windows Xen HVM Domains

Copyright (c) 2014, James Harper
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of James Harper nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL JAMES HARPER BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#if !defined(_XENVBD_H_)
#define _XENVBD_H_

#define __DRIVER_NAME "XenVbdFilter"

#include <ntddk.h>
#include <wdf.h>
#if (NTDDI_VERSION < NTDDI_WINXP) /* srb.h causes warnings under 2K for some reason */
#pragma warning(disable:4201) /* nameless struct/union */
#pragma warning(disable:4214) /* bit field types other than int */
#endif
#include <srb.h>
#include <ntstrsafe.h>
#include "xen_windows.h"
#include <xen_public.h>
#include <io/protocols.h>
#include <memory.h>
#include <event_channel.h>
#include <hvm/params.h>
#include <hvm/hvm_op.h>
#include <io/ring.h>
#include <io/blkif.h>
#include <io/xenbus.h>

#pragma warning(disable: 4127)

#if defined(__x86_64__)
  #define ABI_PROTOCOL "x86_64-abi"
#else
  #define ABI_PROTOCOL "x86_32-abi"
#endif

#include "../xenvbd_common/common.h"

#include "../xenvbd_scsiport/common.h"

typedef struct {
  WDFDEVICE wdf_device;
  WDFIOTARGET wdf_target;
  WDFDPC dpc;
  WDFQUEUE io_queue;
  BOOLEAN hibernate_flag;
  /* event state 0 = no event outstanding, 1 = event outstanding, 2 = need event */
  LONG event_state;
  
  XENVBD_DEVICE_DATA xvdd;
} XENVBD_FILTER_DATA, *PXENVBD_FILTER_DATA;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XENVBD_FILTER_DATA, GetXvfd)

#endif
