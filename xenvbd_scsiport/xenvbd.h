/*
PV Drivers for Windows Xen HVM Domains
Copyright (C) 2013 James Harper

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#if !defined(_XENVBD_H_)
#define _XENVBD_H_

#include <ntddk.h>
//#include <wdm.h>
#include <initguid.h>
#define NTSTRSAFE_LIB
#include <ntstrsafe.h>

#include <srb.h>
#include <scsi.h>
#include <ntddscsi.h>
#include <ntdddisk.h>
#include <stdlib.h>

#define __DRIVER_NAME "XenVbd"

#include <xen_windows.h>
#include <xen_public.h>
#include <io/protocols.h>
#include <memory.h>
#include <event_channel.h>
#include <hvm/params.h>
#include <hvm/hvm_op.h>
#include <io/ring.h>
#include <io/blkif.h>
#include <io/xenbus.h>

#include "..\xenvbd_common\common.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define BLK_RING_SIZE __RING_SIZE((blkif_sring_t *)0, PAGE_SIZE)

#define SHADOW_ID_ID_MASK   0x03FF /* maximum of 1024 requests - currently use a maximum of 64 though */
#define SHADOW_ID_DUMP_FLAG 0x8000 /* indicates the request was generated by dump mode */

/* if this is ever increased to more than 1 then we need a way of tracking it properly */
#define DUMP_MODE_UNALIGNED_PAGES 1 /* only for unaligned buffer use */

#include "common.h"

struct {
  PXENVBD_DEVICE_DATA xvdd;
  ULONG outstanding;
  PSCSI_REQUEST_BLOCK stop_srb;
  /* this is the size of the buffer to allocate at the end of DeviceExtenstion. It includes an extra PAGE_SIZE-1 bytes to assure that we can always align to PAGE_SIZE */
  #define UNALIGNED_BUFFER_DATA_SIZE ((BLKIF_MAX_SEGMENTS_PER_REQUEST + 1) * PAGE_SIZE - 1)
  #define UNALIGNED_BUFFER_DATA_SIZE_DUMP_MODE ((DUMP_MODE_UNALIGNED_PAGES + 1) * PAGE_SIZE - 1)
  /* this has to be right at the end of DeviceExtension */
  /* can't allocate too much data in dump mode so size DeviceExtensionSize accordingly */
  UCHAR aligned_buffer_data[1];
} typedef XENVBD_SCSIPORT_DATA, *PXENVBD_SCSIPORT_DATA;

#endif
