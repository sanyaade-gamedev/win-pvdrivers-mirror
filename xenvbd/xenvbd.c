/*
PV Drivers for Windows Xen HVM Domains
Copyright (C) 2007 James Harper

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

#define INITGUID
#include "xenvbd.h"
#include <io/blkif.h>
#include <scsi.h>
#include <ntddscsi.h>
#include <ntdddisk.h>
#include <stdlib.h>
#include <xen_public.h>
#include <io/xenbus.h>
#include <io/protocols.h>

#pragma warning(disable: 4127)

#ifdef ALLOC_PRAGMA
DRIVER_INITIALIZE DriverEntry;
#pragma alloc_text (INIT, DriverEntry)
#endif

#if defined(__x86_64__)
  #define LongLongToPtr(x) (PVOID)(x)
#else
  #define LongLongToPtr(x) UlongToPtr(x)
#endif

static BOOLEAN dump_mode = FALSE;

ULONGLONG parse_numeric_string(PCHAR string)
{
  ULONGLONG val = 0;
  while (*string != 0)
  {
    val = val * 10 + (*string - '0');
    string++;
  }
  return val;
}

static blkif_shadow_t *
get_shadow_from_freelist(PXENVBD_DEVICE_DATA xvdd)
{
  if (xvdd->shadow_free == 0)
  {
    KdPrint((__DRIVER_NAME "     No more shadow entries\n"));    
    return NULL;
  }
  xvdd->shadow_free--;
  if (xvdd->shadow_free < xvdd->shadow_min_free)
    xvdd->shadow_min_free = xvdd->shadow_free;
  return &xvdd->shadows[xvdd->shadow_free_list[xvdd->shadow_free]];
}

static VOID
put_shadow_on_freelist(PXENVBD_DEVICE_DATA xvdd, blkif_shadow_t *shadow)
{
  xvdd->shadow_free_list[xvdd->shadow_free] = (USHORT)shadow->req.id;
  shadow->srb = NULL;
  xvdd->shadow_free++;
}

static blkif_response_t *
XenVbd_GetResponse(PXENVBD_DEVICE_DATA xvdd, int i)
{
  blkif_other_response_t *rep;
  if (!xvdd->use_other)
    return RING_GET_RESPONSE(&xvdd->ring, i);
  rep = RING_GET_RESPONSE(&xvdd->other_ring, i);
  xvdd->tmp_rep.id = rep->id;
  xvdd->tmp_rep.operation = rep->operation;
  xvdd->tmp_rep.status = rep->status;
  return &xvdd->tmp_rep;
}

static VOID
XenVbd_PutRequest(PXENVBD_DEVICE_DATA xvdd, blkif_request_t *req)
{
  blkif_other_request_t *other_req;

  if (!xvdd->use_other)
  {
    *RING_GET_REQUEST(&xvdd->ring, xvdd->ring.req_prod_pvt) = *req;
  }
  else
  {  
    other_req = RING_GET_REQUEST(&xvdd->other_ring, xvdd->ring.req_prod_pvt);
    other_req->operation = req->operation;
    other_req->nr_segments = req->nr_segments;
    other_req->handle = req->handle;
    other_req->id = req->id;
    other_req->sector_number = req->sector_number;
    memcpy(other_req->seg, req->seg, sizeof(struct blkif_request_segment) * req->nr_segments);
  }
  xvdd->ring.req_prod_pvt++;
}

static ULONG
XenVbd_InitFromConfig(PXENVBD_DEVICE_DATA xvdd)
{
  ULONG i;
  PUCHAR ptr;
  USHORT type;
  PCHAR setting, value, value2;
  ULONG qemu_protocol_version = 0;

  xvdd->device_type = XENVBD_DEVICETYPE_UNKNOWN;
  xvdd->sring = NULL;
  xvdd->event_channel = 0;

  xvdd->inactive = TRUE;  
  ptr = xvdd->device_base;
  while((type = GET_XEN_INIT_RSP(&ptr, (PVOID)&setting, (PVOID)&value, (PVOID)&value2)) != XEN_INIT_TYPE_END)
  {
    switch(type)
    {
    case XEN_INIT_TYPE_RING: /* frontend ring */
      KdPrint((__DRIVER_NAME "     XEN_INIT_TYPE_RING - %s = %p\n", setting, value));
      if (strcmp(setting, "ring-ref") == 0)
      {
        xvdd->sring = (blkif_sring_t *)value;
        FRONT_RING_INIT(&xvdd->ring, xvdd->sring, PAGE_SIZE);
        /* this bit is for when we have to take over an existing ring on a crash dump */
        xvdd->ring.req_prod_pvt = xvdd->sring->req_prod;
        xvdd->ring.rsp_cons = xvdd->ring.req_prod_pvt;
      }
      break;
    case XEN_INIT_TYPE_EVENT_CHANNEL: /* frontend event channel */
    case XEN_INIT_TYPE_EVENT_CHANNEL_IRQ: /* frontend event channel */
      KdPrint((__DRIVER_NAME "     XEN_INIT_TYPE_EVENT_CHANNEL - %s = %d\n", setting, PtrToUlong(value) & 0x3FFFFFFF));
      if (strcmp(setting, "event-channel") == 0)
      {
        /* cheat here - save the state of the ring in the topmost bits of the event-channel */
        xvdd->event_channel_ptr = (ULONG *)(((PCHAR)ptr) - sizeof(ULONG));
        xvdd->event_channel = PtrToUlong(value) & 0x3FFFFFFF;
        if (PtrToUlong(value) & 0x80000000)
        {
          xvdd->cached_use_other = (BOOLEAN)!!(PtrToUlong(value) & 0x40000000);
          KdPrint((__DRIVER_NAME "     cached_use_other = %d\n", xvdd->cached_use_other));
        }
      }
      break;
    case XEN_INIT_TYPE_READ_STRING_BACK:
    case XEN_INIT_TYPE_READ_STRING_FRONT:
      KdPrint((__DRIVER_NAME "     XEN_INIT_TYPE_READ_STRING - %s = %s\n", setting, value));
      if (strcmp(setting, "sectors") == 0)
        xvdd->total_sectors = parse_numeric_string(value);
      else if (strcmp(setting, "sector-size") == 0)
        xvdd->bytes_per_sector = (ULONG)parse_numeric_string(value);
      else if (strcmp(setting, "device-type") == 0)
      {
        if (strcmp(value, "disk") == 0)
        {
          KdPrint((__DRIVER_NAME "     device-type = Disk\n"));    
          xvdd->device_type = XENVBD_DEVICETYPE_DISK;
        }
        else if (strcmp(value, "cdrom") == 0)
        {
          KdPrint((__DRIVER_NAME "     device-type = CDROM\n"));    
          xvdd->device_type = XENVBD_DEVICETYPE_CDROM;
        }
        else
        {
          KdPrint((__DRIVER_NAME "     device-type = %s (This probably won't work!)\n", value));
          xvdd->device_type = XENVBD_DEVICETYPE_UNKNOWN;
        }
      }
      else if (strcmp(setting, "mode") == 0)
      {
        if (strncmp(value, "r", 1) == 0)
        {
          KdPrint((__DRIVER_NAME "     mode = r\n"));    
          xvdd->device_mode = XENVBD_DEVICEMODE_READ;
        }
        else if (strncmp(value, "w", 1) == 0)
        {
          KdPrint((__DRIVER_NAME "     mode = w\n"));    
          xvdd->device_mode = XENVBD_DEVICEMODE_WRITE;
        }
        else
        {
          KdPrint((__DRIVER_NAME "     mode = unknown\n"));
          xvdd->device_mode = XENVBD_DEVICEMODE_UNKNOWN;
        }
      }
      break;
    case XEN_INIT_TYPE_VECTORS:
      KdPrint((__DRIVER_NAME "     XEN_INIT_TYPE_VECTORS\n"));
      if (((PXENPCI_VECTORS)value)->length != sizeof(XENPCI_VECTORS) ||
        ((PXENPCI_VECTORS)value)->magic != XEN_DATA_MAGIC)
      {
        KdPrint((__DRIVER_NAME "     vectors mismatch (magic = %08x, length = %d)\n",
          ((PXENPCI_VECTORS)value)->magic, ((PXENPCI_VECTORS)value)->length));
        KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ "\n"));
        return SP_RETURN_BAD_CONFIG;
      }
      else
        memcpy(&xvdd->vectors, value, sizeof(XENPCI_VECTORS));
      break;
    case XEN_INIT_TYPE_STATE_PTR:
      KdPrint((__DRIVER_NAME "     XEN_INIT_TYPE_DEVICE_STATE - %p\n", PtrToUlong(value)));
      xvdd->device_state = (PXENPCI_DEVICE_STATE)value;
      break;
    case XEN_INIT_TYPE_ACTIVE:
      xvdd->inactive = FALSE;
      break;
    case XEN_INIT_TYPE_QEMU_PROTOCOL_VERSION:
      qemu_protocol_version = PtrToUlong(value);
      break;
    case XEN_INIT_TYPE_GRANT_ENTRIES:
      xvdd->dump_grant_ref = *(grant_ref_t *)value;
      break;
    default:
      KdPrint((__DRIVER_NAME "     XEN_INIT_TYPE_%d\n", type));
      break;
    }
  }
  if (xvdd->device_type == XENVBD_DEVICETYPE_UNKNOWN
    || xvdd->sring == NULL
    || xvdd->event_channel == 0
    || xvdd->total_sectors == 0
    || xvdd->bytes_per_sector == 0)
  {
    KdPrint((__DRIVER_NAME "     Missing settings\n"));
    KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ "\n"));
    return SP_RETURN_BAD_CONFIG;
  }
  if (!xvdd->inactive && xvdd->device_type == XENVBD_DEVICETYPE_CDROM && qemu_protocol_version > 0)
  {
    xvdd->inactive = TRUE;
  }
  
  if (xvdd->inactive)
    KdPrint((__DRIVER_NAME "     Device is inactive\n"));
  
  if (xvdd->device_type == XENVBD_DEVICETYPE_CDROM)
  {
    /* CD/DVD drives must have bytes_per_sector = 2048. */
    xvdd->bytes_per_sector = 2048;
  }

  /* for some reason total_sectors is measured in 512 byte sectors always, so correct this to be in bytes_per_sectors */
#ifdef __MINGW32__
  /* mingw can't divide, so shift instead (assumes bps is ^2 and at least 512) */
  {
    ULONG num_512_byte_sectors = xvdd->bytes_per_sector / 512;
    ULONG index;

    bit_scan_forward(&index, num_512_byte_sectors);
    xvdd->total_sectors <<= index-1;
  }
#else
  xvdd->total_sectors /= xvdd->bytes_per_sector / 512;
#endif


  xvdd->shadow_free = 0;
  memset(xvdd->shadows, 0, sizeof(blkif_shadow_t) * SHADOW_ENTRIES);
  for (i = 0; i < SHADOW_ENTRIES; i++)
  {
    xvdd->shadows[i].req.id = i;
    put_shadow_on_freelist(xvdd, &xvdd->shadows[i]);
  }
  
  return SP_RETURN_FOUND;
}

static __inline ULONG
decode_cdb_length(PSCSI_REQUEST_BLOCK srb)
{
  switch (srb->Cdb[0])
  {
  case SCSIOP_READ:
  case SCSIOP_WRITE:
    return (srb->Cdb[7] << 8) | srb->Cdb[8];
  case SCSIOP_READ16:
  case SCSIOP_WRITE16:
    return (srb->Cdb[10] << 24) | (srb->Cdb[11] << 16) | (srb->Cdb[12] << 8) | srb->Cdb[13];    
  default:
    return 0;
  }
}

static __inline ULONGLONG
decode_cdb_sector(PSCSI_REQUEST_BLOCK srb)
{
  ULONGLONG sector;
  
  switch (srb->Cdb[0])
  {
  case SCSIOP_READ:
  case SCSIOP_WRITE:
    sector = (srb->Cdb[2] << 24) | (srb->Cdb[3] << 16) | (srb->Cdb[4] << 8) | srb->Cdb[5];
    break;
  case SCSIOP_READ16:
  case SCSIOP_WRITE16:
    sector = ((ULONGLONG)srb->Cdb[2] << 56) | ((ULONGLONG)srb->Cdb[3] << 48)
           | ((ULONGLONG)srb->Cdb[4] << 40) | ((ULONGLONG)srb->Cdb[5] << 32)
           | ((ULONGLONG)srb->Cdb[6] << 24) | ((ULONGLONG)srb->Cdb[7] << 16)
           | ((ULONGLONG)srb->Cdb[8] << 8) | ((ULONGLONG)srb->Cdb[9]);
    //KdPrint((__DRIVER_NAME "     sector_number = %d (high) %d (low)\n", (ULONG)(sector >> 32), (ULONG)sector));
    break;
  default:
    sector = 0;
    break;
  }
  return sector;
}

static __inline BOOLEAN
decode_cdb_is_read(PSCSI_REQUEST_BLOCK srb)
{
  switch (srb->Cdb[0])
  {
  case SCSIOP_READ:
  case SCSIOP_READ16:
    return TRUE;
  case SCSIOP_WRITE:
  case SCSIOP_WRITE16:
    return FALSE;
  default:
    return FALSE;
  }
}

static __forceinline PVOID
get_databuffer_virtual(PXENVBD_DEVICE_DATA xvdd, PSCSI_REQUEST_BLOCK srb)
{
  ULONG data_buffer_length;
  
  if (!dump_mode)
  {
    return LongLongToPtr(ScsiPortGetPhysicalAddress(xvdd, srb, srb->DataBuffer, &data_buffer_length).QuadPart);
  }
  else
  {
    /* in dump mode, we can count on srb->DataBuffer being the virtual address we want */
    return srb->DataBuffer;
  }
}

static VOID
XenVbd_PutSrbOnRing(PXENVBD_DEVICE_DATA xvdd, PSCSI_REQUEST_BLOCK srb)
{
  ULONG block_count;
  blkif_shadow_t *shadow;
  ULONG remaining, offset, length;
  PUCHAR ptr;
  int notify;

  //FUNCTION_ENTER();

  ptr = srb->DataBuffer;

  block_count = decode_cdb_length(srb);;
  block_count *= xvdd->bytes_per_sector / 512;
  remaining = block_count * 512;

  shadow = get_shadow_from_freelist(xvdd);
  ASSERT(shadow);
  shadow->req.sector_number = decode_cdb_sector(srb);
  shadow->req.sector_number *= xvdd->bytes_per_sector / 512;
  shadow->req.handle = 0;
  shadow->req.operation = decode_cdb_is_read(srb)?BLKIF_OP_READ:BLKIF_OP_WRITE;
  shadow->req.nr_segments = 0;
  shadow->srb = srb;

  //KdPrint((__DRIVER_NAME "     sector_number = %d, block_count = %d\n", (ULONG)shadow->req.sector_number, block_count));
  //KdPrint((__DRIVER_NAME "     SrbExtension = %p\n", srb->SrbExtension));
  //KdPrint((__DRIVER_NAME "     DataBuffer   = %p\n", srb->DataBuffer));

  //KdPrint((__DRIVER_NAME "     sector_number = %d\n", (ULONG)shadow->req.sector_number));
  //KdPrint((__DRIVER_NAME "     handle = %d\n", shadow->req.handle));
  //KdPrint((__DRIVER_NAME "     operation = %d\n", shadow->req.operation));
  if (!dump_mode)
  {
    while (remaining > 0)
    {
      PHYSICAL_ADDRESS physical_address;
      physical_address = ScsiPortGetPhysicalAddress(xvdd, srb, ptr, &length);
      offset = physical_address.LowPart & (PAGE_SIZE - 1);
      if (offset & 511)
      {
        KdPrint((__DRIVER_NAME "     DataTransferLength = %d, remaining = %d, block_count = %d, offset = %d, ptr = %p, srb->DataBuffer = %p\n",
          srb->DataTransferLength, remaining, block_count, offset, ptr, srb->DataBuffer));
        ptr = srb->DataBuffer;
        block_count = decode_cdb_length(srb);;
        block_count *= xvdd->bytes_per_sector / 512;
        remaining = block_count * 512;
        while (remaining > 0)
        {
          physical_address = ScsiPortGetPhysicalAddress(xvdd, srb, ptr, &length);
          KdPrint((__DRIVER_NAME "     ptr = %p, physical_address = %08x:%08x, length = %d\n",
            ptr, physical_address.HighPart, physical_address.LowPart, length));
          remaining -= length;
          ptr += length;
          KdPrint((__DRIVER_NAME "     remaining = %d\n", remaining));
        }
      }
      
      ASSERT((offset & 511) == 0);
      ASSERT((length & 511) == 0);
      //length = min(PAGE_SIZE - offset, remaining);
      //KdPrint((__DRIVER_NAME "     length(a) = %d\n", length));
      shadow->req.seg[shadow->req.nr_segments].gref = (grant_ref_t)(physical_address.QuadPart >> PAGE_SHIFT);
      //KdPrint((__DRIVER_NAME "     length(b) = %d\n", length));
      shadow->req.seg[shadow->req.nr_segments].first_sect = (UCHAR)(offset >> 9);
      shadow->req.seg[shadow->req.nr_segments].last_sect = (UCHAR)(((offset + length) >> 9) - 1);
      remaining -= length;
      ptr += length;
      //KdPrint((__DRIVER_NAME "     seg[%d].gref = %d\n", shadow->req.nr_segments, shadow->req.seg[shadow->req.nr_segments].gref));
      //KdPrint((__DRIVER_NAME "     seg[%d].first_sect = %d\n", shadow->req.nr_segments, shadow->req.seg[shadow->req.nr_segments].first_sect));
      //KdPrint((__DRIVER_NAME "     seg[%d].last_sect = %d\n", shadow->req.nr_segments, shadow->req.seg[shadow->req.nr_segments].last_sect));
      shadow->req.nr_segments++;
    }
  }
  else
  {
    PHYSICAL_ADDRESS physical_address;
    //KdPrint((__DRIVER_NAME "     remaining = %d\n", remaining));
    ASSERT(remaining <= PAGE_SIZE); /* one page at a time in dump mode */
    physical_address = ScsiPortGetPhysicalAddress(xvdd, srb, ptr, &length);
    offset = physical_address.LowPart & (PAGE_SIZE - 1);
    ASSERT(offset == 0);
    //ASSERT((offset & 511) == 0);
    shadow->req.seg[shadow->req.nr_segments].gref = 
      (grant_ref_t)xvdd->vectors.GntTbl_GrantAccess(xvdd->vectors.context, 0,
      (ULONG)(physical_address.QuadPart >> PAGE_SHIFT), FALSE, xvdd->dump_grant_ref);
    //KdPrint((__DRIVER_NAME "     gref = %d, dump_grant_ref = %d\n",
    //  shadow->req.seg[shadow->req.nr_segments].gref, xvdd->dump_grant_ref));
    shadow->req.seg[shadow->req.nr_segments].first_sect = (UCHAR)(offset >> 9);
    shadow->req.seg[shadow->req.nr_segments].last_sect = (UCHAR)(((offset + remaining) >> 9) - 1);
    remaining -= length;
    shadow->req.nr_segments++;
  }
  //KdPrint((__DRIVER_NAME "     nr_segments = %d\n", shadow->req.nr_segments));

  XenVbd_PutRequest(xvdd, &shadow->req);

  RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&xvdd->ring, notify);
  if (notify)
  {
    //KdPrint((__DRIVER_NAME "     Notifying\n"));
    xvdd->vectors.EvtChn_Notify(xvdd->vectors.context, xvdd->event_channel);
  }

  if (xvdd->shadow_free)
    ScsiPortNotification(NextLuRequest, xvdd, 0, 0, 0);

  //FUNCTION_EXIT();
}

#if 0
static VOID
XenVbd_Resume(PVOID DeviceExtension)
{
  PXENVBD_DEVICE_DATA xvdd = (PXENVBD_DEVICE_DATA)DeviceExtension;
  ULONG i;

  FUNCTION_ENTER();
  KdPrint((__DRIVER_NAME "     found device in resume state\n"));
  //FRONT_RING_INIT(&xvdd->ring, xvdd->sring, PAGE_SIZE); what was this for???
  // re-submit srb's
  
KdPrint((__DRIVER_NAME "     About to call InitFromConfig\n"));
  XenVbd_InitFromConfig(xvdd);
KdPrint((__DRIVER_NAME "     Back from InitFromConfig\n"));
  
  
  xvdd->device_state->resume_state = RESUME_STATE_RUNNING;

KdPrint((__DRIVER_NAME "     resume_state set to RESUME_STATE_RUNNING\n"));
  
  if (i == 0)
  {
    /* no requests, so we might need to tell scsiport that we can accept a new one if we deferred one earlier */
KdPrint((__DRIVER_NAME "     No shadows - notifying to get things started again\n"));
    ScsiPortNotification(NextLuRequest, DeviceExtension, 0, 0, 0);
  }
  FUNCTION_EXIT();
}
#endif

static ULONG DDKAPI
XenVbd_HwScsiFindAdapter(PVOID DeviceExtension, PVOID HwContext, PVOID BusInformation, PCHAR ArgumentString, PPORT_CONFIGURATION_INFORMATION ConfigInfo, PBOOLEAN Again)
{
//  PACCESS_RANGE AccessRange;
  PXENVBD_DEVICE_DATA xvdd = (PXENVBD_DEVICE_DATA)DeviceExtension;
  ULONG status;
//  PXENPCI_XEN_DEVICE_DATA XenDeviceData;
  PACCESS_RANGE access_range;

  UNREFERENCED_PARAMETER(HwContext);
  UNREFERENCED_PARAMETER(BusInformation);
  UNREFERENCED_PARAMETER(ArgumentString);

  FUNCTION_ENTER(); 
  KdPrint((__DRIVER_NAME "     IRQL = %d\n", KeGetCurrentIrql()));

  *Again = FALSE;

  KdPrint((__DRIVER_NAME "     BusInterruptLevel = %d\n", ConfigInfo->BusInterruptLevel));
  KdPrint((__DRIVER_NAME "     BusInterruptVector = %03x\n", ConfigInfo->BusInterruptVector));

  KdPrint((__DRIVER_NAME "     NumberOfAccessRanges = %d\n", ConfigInfo->NumberOfAccessRanges));    
  if (ConfigInfo->NumberOfAccessRanges != 1 && ConfigInfo->NumberOfAccessRanges != 2)
  {
    return SP_RETURN_BAD_CONFIG;
  }

  access_range = &((*(ConfigInfo->AccessRanges))[0]);
  KdPrint((__DRIVER_NAME "     RangeStart = %08x, RangeLength = %08x\n",
    access_range->RangeStart.LowPart, access_range->RangeLength));
  xvdd->device_base = ScsiPortGetDeviceBase(
    DeviceExtension,
    ConfigInfo->AdapterInterfaceType,
    ConfigInfo->SystemIoBusNumber,
    access_range->RangeStart,
    access_range->RangeLength,
    !access_range->RangeInMemory);
  if (!xvdd->device_base)
  {
    KdPrint((__DRIVER_NAME "     Invalid config\n"));
    FUNCTION_EXIT(); 
    return SP_RETURN_BAD_CONFIG;
  }
  
  status = XenVbd_InitFromConfig(xvdd);
  if (status != SP_RETURN_FOUND)
  {
    FUNCTION_EXIT();
    return status;
  }

  if (dump_mode)
  {
    ConfigInfo->MaximumTransferLength = BLKIF_MAX_SEGMENTS_PER_REQUEST * PAGE_SIZE;
    ConfigInfo->NumberOfPhysicalBreaks = BLKIF_MAX_SEGMENTS_PER_REQUEST - 1;
    ConfigInfo->ScatterGather = TRUE;
  }
  else
  {
    ConfigInfo->MaximumTransferLength = 4096;
    ConfigInfo->NumberOfPhysicalBreaks = 0;
    ConfigInfo->ScatterGather = FALSE;
  }
  ConfigInfo->AlignmentMask = 0;
  ConfigInfo->NumberOfBuses = 1;
  ConfigInfo->InitiatorBusId[0] = 1;
  ConfigInfo->MaximumNumberOfLogicalUnits = 1;
  ConfigInfo->MaximumNumberOfTargets = 2;
  ConfigInfo->BufferAccessScsiPortControlled = TRUE;
  if (ConfigInfo->Dma64BitAddresses == SCSI_DMA64_SYSTEM_SUPPORTED)
  {
    ConfigInfo->Master = TRUE;
    ConfigInfo->Dma64BitAddresses = SCSI_DMA64_MINIPORT_SUPPORTED;
    ConfigInfo->Dma32BitAddresses = FALSE;
    KdPrint((__DRIVER_NAME "     Dma64BitAddresses supported\n"));
  }
  else
  {
    ConfigInfo->Master = TRUE; //FALSE;
    ConfigInfo->Dma32BitAddresses = TRUE;
    KdPrint((__DRIVER_NAME "     Dma64BitAddresses not supported\n"));
  }

  FUNCTION_EXIT();

  return SP_RETURN_FOUND;
}

static BOOLEAN DDKAPI
XenVbd_HwScsiInitialize(PVOID DeviceExtension)
{
  PXENVBD_DEVICE_DATA xvdd = (PXENVBD_DEVICE_DATA)DeviceExtension;
  blkif_request_t *req;
  int i;
  int notify;
  
  KdPrint((__DRIVER_NAME " --> " __FUNCTION__ "\n"));
  KdPrint((__DRIVER_NAME "     IRQL = %d\n", KeGetCurrentIrql()));

  if (!dump_mode)
  {
    req = RING_GET_REQUEST(&xvdd->ring, xvdd->ring.req_prod_pvt);
    req->operation = 0xff;
    req->nr_segments = 0;
    for (i = 0; i < BLKIF_MAX_SEGMENTS_PER_REQUEST; i++)
    {
      req->seg[i].gref = 0; //0xffffffff;
      req->seg[i].first_sect = 0; //0xff;
      req->seg[i].last_sect = 0; //0xff;
    }
    xvdd->ring.req_prod_pvt++;

    req = RING_GET_REQUEST(&xvdd->ring, xvdd->ring.req_prod_pvt);
    req->operation = 0xff;
    req->nr_segments = 0;
    for (i = 0; i < BLKIF_MAX_SEGMENTS_PER_REQUEST; i++)
    {
      req->seg[i].gref = 0; //0xffffffff;
      req->seg[i].first_sect = 0; //0xff;
      req->seg[i].last_sect = 0; //0xff;
    }
    xvdd->ring.req_prod_pvt++;

    RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&xvdd->ring, notify);
    if (notify)
      xvdd->vectors.EvtChn_Notify(xvdd->vectors.context, xvdd->event_channel);
    xvdd->ring_detect_state = 0;
  }
  else
  {
    if (xvdd->cached_use_other)
    {
      xvdd->ring.nr_ents = BLK_OTHER_RING_SIZE;
      xvdd->use_other = TRUE;
    }
    xvdd->ring_detect_state = 2;
  }
  
  KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ "\n"));

  return TRUE;
}

static ULONG
XenVbd_FillModePage(PXENVBD_DEVICE_DATA xvdd, PSCSI_REQUEST_BLOCK srb)
{
  PMODE_PARAMETER_HEADER parameter_header;
  PMODE_PARAMETER_BLOCK param_block;
  PMODE_FORMAT_PAGE format_page;
  ULONG offset;
  UCHAR buffer[256];
  BOOLEAN valid_page = FALSE;
  BOOLEAN cdb_llbaa;
  BOOLEAN cdb_dbd;
  UCHAR cdb_page_code;
  USHORT cdb_allocation_length;
  PVOID data_buffer;
  //ULONG data_buffer_length;

  UNREFERENCED_PARAMETER(xvdd);

  //KdPrint((__DRIVER_NAME " --> " __FUNCTION__ "\n"));
  
  //data_buffer = LongLongToPtr(ScsiPortGetPhysicalAddress(xvdd, srb, srb->DataBuffer, &data_buffer_length).QuadPart);
  data_buffer = get_databuffer_virtual(xvdd, srb);
  //cdb = (PCDB)srb->Cdb;
  switch (srb->Cdb[0])
  {
  case SCSIOP_MODE_SENSE:
    cdb_llbaa = FALSE;
    cdb_dbd = (BOOLEAN)!!(srb->Cdb[1] & 8);
    cdb_page_code = srb->Cdb[2] & 0x3f;
    cdb_allocation_length = srb->Cdb[4];
    KdPrint((__DRIVER_NAME "     SCSIOP_MODE_SENSE llbaa = %d, dbd = %d, page_code = %d, allocation_length = %d\n",
      cdb_llbaa, cdb_dbd, cdb_page_code, cdb_allocation_length));
    break;
  case SCSIOP_MODE_SENSE10:
    cdb_llbaa = (BOOLEAN)!!(srb->Cdb[1] & 16);
    cdb_dbd = (BOOLEAN)!!(srb->Cdb[1] & 8);
    cdb_page_code = srb->Cdb[2] & 0x3f;
    cdb_allocation_length = (srb->Cdb[7] << 8) | srb->Cdb[8];
    KdPrint((__DRIVER_NAME "     SCSIOP_MODE_SENSE10 llbaa = %d, dbd = %d, page_code = %d, allocation_length = %d\n",
      cdb_llbaa, cdb_dbd, cdb_page_code, cdb_allocation_length));
    break;
  default:
    KdPrint((__DRIVER_NAME "     SCSIOP_MODE_SENSE_WTF (%02x)\n", (ULONG)srb->Cdb[0]));
    return FALSE;
  }
  offset = 0;
  
  RtlZeroMemory(data_buffer, srb->DataTransferLength);
  RtlZeroMemory(buffer, ARRAY_SIZE(buffer));

  parameter_header = (PMODE_PARAMETER_HEADER)&buffer[offset];
  parameter_header->MediumType = 0;
  parameter_header->DeviceSpecificParameter = 0;
  parameter_header->BlockDescriptorLength = 0;
  offset += sizeof(MODE_PARAMETER_HEADER);
  
  if (xvdd->device_mode == XENVBD_DEVICEMODE_READ)
  {
    KdPrint((__DRIVER_NAME " Mode sense to a read only disk.\n"));
    parameter_header->DeviceSpecificParameter|=MODE_DSP_WRITE_PROTECT; 
  }
  
  if (!cdb_dbd)
  {
    parameter_header->BlockDescriptorLength += sizeof(MODE_PARAMETER_BLOCK);
    param_block = (PMODE_PARAMETER_BLOCK)&buffer[offset];
    if (xvdd->device_type == XENVBD_DEVICETYPE_DISK)
    {
      if (xvdd->total_sectors >> 32) 
      {
        param_block->DensityCode = 0xff;
        param_block->NumberOfBlocks[0] = 0xff;
        param_block->NumberOfBlocks[1] = 0xff;
        param_block->NumberOfBlocks[2] = 0xff;
      }
      else
      {
        param_block->DensityCode = (UCHAR)((xvdd->total_sectors >> 24) & 0xff);
        param_block->NumberOfBlocks[0] = (UCHAR)((xvdd->total_sectors >> 16) & 0xff);
        param_block->NumberOfBlocks[1] = (UCHAR)((xvdd->total_sectors >> 8) & 0xff);
        param_block->NumberOfBlocks[2] = (UCHAR)((xvdd->total_sectors >> 0) & 0xff);
      }
      param_block->BlockLength[0] = (UCHAR)((xvdd->bytes_per_sector >> 16) & 0xff);
      param_block->BlockLength[1] = (UCHAR)((xvdd->bytes_per_sector >> 8) & 0xff);
      param_block->BlockLength[2] = (UCHAR)((xvdd->bytes_per_sector >> 0) & 0xff);
    }
    offset += sizeof(MODE_PARAMETER_BLOCK);
  }
  if (xvdd->device_type == XENVBD_DEVICETYPE_DISK && (cdb_page_code == MODE_PAGE_FORMAT_DEVICE || cdb_page_code == MODE_SENSE_RETURN_ALL))
  {
    valid_page = TRUE;
    format_page = (PMODE_FORMAT_PAGE)&buffer[offset];
    format_page->PageCode = MODE_PAGE_FORMAT_DEVICE;
    format_page->PageLength = sizeof(MODE_FORMAT_PAGE) - FIELD_OFFSET(MODE_FORMAT_PAGE, PageLength);
    /* 256 sectors per track */
    format_page->SectorsPerTrack[0] = 0x01;
    format_page->SectorsPerTrack[1] = 0x00;
    /* xxx bytes per sector */
    format_page->BytesPerPhysicalSector[0] = (UCHAR)(xvdd->bytes_per_sector >> 8);
    format_page->BytesPerPhysicalSector[1] = (UCHAR)(xvdd->bytes_per_sector & 0xff);
    format_page->HardSectorFormating = TRUE;
    format_page->SoftSectorFormating = TRUE;
    offset += sizeof(MODE_FORMAT_PAGE);
  }
  parameter_header->ModeDataLength = (UCHAR)(offset - 1);
  if (!valid_page && cdb_page_code != MODE_SENSE_RETURN_ALL)
  {
    srb->SrbStatus = SRB_STATUS_ERROR;
  }
  else if(offset < srb->DataTransferLength)
    srb->SrbStatus = SRB_STATUS_DATA_OVERRUN;
  else
    srb->SrbStatus = SRB_STATUS_SUCCESS;
  srb->DataTransferLength = min(srb->DataTransferLength, offset);
  srb->ScsiStatus = 0;
  memcpy(data_buffer, buffer, srb->DataTransferLength);
  
  //KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ "\n"));

  return TRUE;
}

static VOID
XenVbd_MakeSense(PXENVBD_DEVICE_DATA xvdd, PSCSI_REQUEST_BLOCK srb, UCHAR sense_key, UCHAR additional_sense_code)
{
  PSENSE_DATA sd = srb->SenseInfoBuffer;
 
  UNREFERENCED_PARAMETER(xvdd);
  
  if (!srb->SenseInfoBuffer)
    return;
  
  sd->ErrorCode = 0x70;
  sd->Valid = 1;
  sd->SenseKey = sense_key;
  sd->AdditionalSenseLength = sizeof(SENSE_DATA) - FIELD_OFFSET(SENSE_DATA, AdditionalSenseLength);
  sd->AdditionalSenseCode = additional_sense_code;
  return;
}

static VOID
XenVbd_MakeAutoSense(PXENVBD_DEVICE_DATA xvdd, PSCSI_REQUEST_BLOCK srb)
{
  if (srb->SrbStatus == SRB_STATUS_SUCCESS || srb->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE)
    return;
  XenVbd_MakeSense(xvdd, srb, xvdd->last_sense_key, xvdd->last_additional_sense_code);
  srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
}

static BOOLEAN DDKAPI
XenVbd_HwScsiInterrupt(PVOID DeviceExtension)
{
  PXENVBD_DEVICE_DATA xvdd = (PXENVBD_DEVICE_DATA)DeviceExtension;
  PSCSI_REQUEST_BLOCK srb;
  RING_IDX i, rp;
  blkif_response_t *rep;
  int block_count;
  int more_to_do = TRUE;
  blkif_shadow_t *shadow;
  ULONG suspend_resume_state_pdo;

  /* in dump mode I think we get called on a timer, not by an actual IRQ */
  if (!dump_mode && !xvdd->vectors.EvtChn_AckEvent(xvdd->vectors.context, xvdd->event_channel))
    return FALSE; /* interrupt was not for us */
    
  suspend_resume_state_pdo = xvdd->device_state->suspend_resume_state_pdo;
  KeMemoryBarrier();

  if (suspend_resume_state_pdo != xvdd->device_state->suspend_resume_state_fdo)
  {
    FUNCTION_ENTER();
    switch (suspend_resume_state_pdo)
    {
      case SR_STATE_SUSPENDING:
        KdPrint((__DRIVER_NAME "     New pdo state SR_STATE_SUSPENDING\n"));
        break;
      case SR_STATE_RESUMING:
        KdPrint((__DRIVER_NAME "     New pdo state SR_STATE_RESUMING\n"));
        XenVbd_InitFromConfig(xvdd);
        xvdd->device_state->suspend_resume_state_fdo = suspend_resume_state_pdo;
        xvdd->vectors.EvtChn_Notify(xvdd->vectors.context, xvdd->device_state->pdo_event_channel);
        break;
      case SR_STATE_RUNNING:
        KdPrint((__DRIVER_NAME "     New pdo state %d\n", suspend_resume_state_pdo));
        xvdd->device_state->suspend_resume_state_fdo = suspend_resume_state_pdo;
        xvdd->vectors.EvtChn_Notify(xvdd->vectors.context, xvdd->device_state->pdo_event_channel);
        ScsiPortNotification(NextLuRequest, DeviceExtension, 0, 0, 0);
      default:
        KdPrint((__DRIVER_NAME "     New pdo state %d\n", suspend_resume_state_pdo));
        xvdd->device_state->suspend_resume_state_fdo = suspend_resume_state_pdo;
        xvdd->vectors.EvtChn_Notify(xvdd->vectors.context, xvdd->device_state->pdo_event_channel);
        break;
    }
    KeMemoryBarrier();
  }

  if (xvdd->device_state->suspend_resume_state_fdo != SR_STATE_RUNNING)
  {
    return FALSE;
  }

  while (more_to_do)
  {
    rp = xvdd->ring.sring->rsp_prod;
    KeMemoryBarrier();
    for (i = xvdd->ring.rsp_cons; i < rp; i++)
    {
      rep = XenVbd_GetResponse(xvdd, i);
/*
* This code is to automatically detect if the backend is using the same
* bit width or a different bit width to us. Later versions of Xen do this
* via a xenstore value, but not all. That 0x0fffffff (notice
* that the msb is not actually set, so we don't have any problems with
* sign extending) is to signify the last entry on the right, which is
* different under 32 and 64 bits, and that is why we set it up there.

* To do the detection, we put two initial entries on the ring, with an op
* of 0xff (which is invalid). The first entry is mostly okay, but the
* second will be grossly misaligned if the backend bit width is different,
* and we detect this and switch frontend structures.
*/
      switch (xvdd->ring_detect_state)
      {
      case 0:
        KdPrint((__DRIVER_NAME "     ring_detect_state = %d, operation = %x, id = %lx, status = %d\n", xvdd->ring_detect_state, rep->operation, rep->id, rep->status));
        xvdd->ring_detect_state = 1;
        break;
      case 1:
        KdPrint((__DRIVER_NAME "     ring_detect_state = %d, operation = %x, id = %lx, status = %d\n", xvdd->ring_detect_state, rep->operation, rep->id, rep->status));
        *xvdd->event_channel_ptr |= 0x80000000;
        if (rep->operation != 0xff)
        {
          xvdd->ring.nr_ents = BLK_OTHER_RING_SIZE;
          xvdd->use_other = TRUE;
          *xvdd->event_channel_ptr |= 0x40000000;
        }
        xvdd->ring_detect_state = 2;
        ScsiPortNotification(NextRequest, DeviceExtension);
        break;
      case 2:
        shadow = &xvdd->shadows[rep->id];
        srb = shadow->srb;
        ASSERT(srb != NULL);
        block_count = decode_cdb_length(srb);
        block_count *= xvdd->bytes_per_sector / 512;
        if (rep->status == BLKIF_RSP_OKAY)
          srb->SrbStatus = SRB_STATUS_SUCCESS;
        else
        {
          KdPrint((__DRIVER_NAME "     Xen Operation returned error\n"));
          if (decode_cdb_is_read(srb))
            KdPrint((__DRIVER_NAME "     Operation = Read\n"));
          else
            KdPrint((__DRIVER_NAME "     Operation = Write\n"));
          KdPrint((__DRIVER_NAME "     Sector = %08X, Count = %d\n", (ULONG)shadow->req.sector_number, block_count));
          srb->SrbStatus = SRB_STATUS_ERROR;
          srb->ScsiStatus = 0x02;
          xvdd->last_sense_key = SCSI_SENSE_MEDIUM_ERROR;
          xvdd->last_additional_sense_code = SCSI_ADSENSE_NO_SENSE;
          XenVbd_MakeAutoSense(xvdd, srb);
        }
        put_shadow_on_freelist(xvdd, shadow);
        if (dump_mode)
        {
          ASSERT(shadow->req.nr_segments == 1);
          //KdPrint((__DRIVER_NAME "     gref = %d, dump_grant_ref = %d\n",
          //  shadow->req.seg[0].gref, xvdd->dump_grant_ref));
          ASSERT(shadow->req.seg[0].gref == xvdd->dump_grant_ref);
          xvdd->vectors.GntTbl_EndAccess(xvdd->vectors.context,
            shadow->req.seg[0].gref, TRUE);
        }
        ScsiPortNotification(RequestComplete, xvdd, srb);
        if (suspend_resume_state_pdo == SR_STATE_RUNNING)
          ScsiPortNotification(NextLuRequest, DeviceExtension, 0, 0, 0);
        break;
      }
    }

    xvdd->ring.rsp_cons = i;
    if (i != xvdd->ring.req_prod_pvt)
    {
      RING_FINAL_CHECK_FOR_RESPONSES(&xvdd->ring, more_to_do);
    }
    else
    {
      xvdd->ring.sring->rsp_event = i + 1;
      more_to_do = FALSE;
    }
  }

  if (suspend_resume_state_pdo == SR_STATE_SUSPENDING)
  {
    if (xvdd->shadow_free == SHADOW_ENTRIES)
    {
      /* all entries are purged from the list. ready to suspend */
      xvdd->device_state->suspend_resume_state_fdo = suspend_resume_state_pdo;
      KeMemoryBarrier();
      KdPrint((__DRIVER_NAME "     Set fdo state SR_STATE_SUSPENDING\n"));
      KdPrint((__DRIVER_NAME "     Notifying event channel %d\n", xvdd->device_state->pdo_event_channel));
      xvdd->vectors.EvtChn_Notify(xvdd->vectors.context, xvdd->device_state->pdo_event_channel);
    }
    FUNCTION_EXIT();
  }

  return FALSE; /* always fall through to the next ISR... */
}

static BOOLEAN DDKAPI
XenVbd_HwScsiStartIo(PVOID DeviceExtension, PSCSI_REQUEST_BLOCK Srb)
{
  PUCHAR data_buffer;
  //ULONG data_buffer_length;
  PCDB cdb;
  PXENVBD_DEVICE_DATA xvdd = DeviceExtension;

  //KdPrint((__DRIVER_NAME " --> HwScsiStartIo PathId = %d, TargetId = %d, Lun = %d\n", Srb->PathId, Srb->TargetId, Srb->Lun));

  if (xvdd->inactive)
  {
    Srb->SrbStatus = SRB_STATUS_NO_DEVICE;
    ScsiPortNotification(RequestComplete, DeviceExtension, Srb);
    ScsiPortNotification(NextRequest, DeviceExtension);
    return TRUE;
  }
  
  // If we haven't enumerated all the devices yet then just defer the request
  if (xvdd->ring_detect_state < 2)
  {
    Srb->SrbStatus = SRB_STATUS_BUSY;
    ScsiPortNotification(RequestComplete, DeviceExtension, Srb);
    KdPrint((__DRIVER_NAME " --- HwScsiStartIo (Still figuring out ring)\n"));
    return TRUE;
  }

  if (xvdd->device_state->suspend_resume_state_pdo != SR_STATE_RUNNING)
  {
    KdPrint((__DRIVER_NAME " --> HwScsiStartIo (Resuming)\n"));
    Srb->SrbStatus = SRB_STATUS_BUSY;
    ScsiPortNotification(RequestComplete, DeviceExtension, Srb);
    KdPrint((__DRIVER_NAME " <-- HwScsiStartIo (Resuming)\n"));
    return TRUE;
  }

  if (Srb->PathId != 0 || Srb->TargetId != 0)
  {
    Srb->SrbStatus = SRB_STATUS_NO_DEVICE;
    ScsiPortNotification(RequestComplete, DeviceExtension, Srb);
    ScsiPortNotification(NextRequest, DeviceExtension);
    KdPrint((__DRIVER_NAME " --- HwScsiStartIo (Out of bounds)\n"));
    return TRUE;
  }

  switch (Srb->Function)
  {
  case SRB_FUNCTION_EXECUTE_SCSI:
    cdb = (PCDB)Srb->Cdb;

    switch(cdb->CDB6GENERIC.OperationCode)
    {
    case SCSIOP_TEST_UNIT_READY:
      if (dump_mode)
        KdPrint((__DRIVER_NAME "     Command = TEST_UNIT_READY\n"));
      Srb->SrbStatus = SRB_STATUS_SUCCESS;
      Srb->ScsiStatus = 0;
      break;
    case SCSIOP_INQUIRY:
      if (dump_mode)
      {
        //PHYSICAL_ADDRESS physical;
        KdPrint((__DRIVER_NAME "     Command = INQUIRY\n"));
        //KdPrint((__DRIVER_NAME "     Srb->Databuffer = %p\n", Srb->DataBuffer));
        //physical = ScsiPortGetPhysicalAddress(xvdd, Srb, Srb->DataBuffer, &data_buffer_length);
        //KdPrint((__DRIVER_NAME "     ScsiPortGetPhysicalAddress = %08x:%08x\n", physical.LowPart, physical.HighPart));
      }
//      KdPrint((__DRIVER_NAME "     (LUN = %d, EVPD = %d, Page Code = %02X)\n", Srb->Cdb[1] >> 5, Srb->Cdb[1] & 1, Srb->Cdb[2]));
//      KdPrint((__DRIVER_NAME "     (Length = %d)\n", Srb->DataTransferLength));
      
      //data_buffer = LongLongToPtr(ScsiPortGetPhysicalAddress(xvdd, Srb, Srb->DataBuffer, &data_buffer_length).QuadPart);
      data_buffer = get_databuffer_virtual(xvdd, Srb);
      RtlZeroMemory(data_buffer, Srb->DataTransferLength);
      Srb->SrbStatus = SRB_STATUS_SUCCESS;
      switch (xvdd->device_type)
      {
      case XENVBD_DEVICETYPE_DISK:
        if ((Srb->Cdb[1] & 1) == 0)
        {
          PINQUIRYDATA id = (PINQUIRYDATA)data_buffer;
          id->DeviceType = DIRECT_ACCESS_DEVICE;
          id->Versions = 3;
          id->ResponseDataFormat = 0;
          id->AdditionalLength = FIELD_OFFSET(INQUIRYDATA, VendorSpecific) - FIELD_OFFSET(INQUIRYDATA, AdditionalLength);
          id->CommandQueue = 1;
          memcpy(id->VendorId, "XEN     ", 8); // vendor id
          memcpy(id->ProductId, "PV DISK         ", 16); // product id
          memcpy(id->ProductRevisionLevel, "0000", 4); // product revision level
        }
        else
        {
          switch (Srb->Cdb[2])
          {
          case 0x00:
            data_buffer[0] = DIRECT_ACCESS_DEVICE;
            data_buffer[1] = 0x00;
            data_buffer[2] = 0x00;
            data_buffer[3] = 2;
            data_buffer[4] = 0x00;
            data_buffer[5] = 0x80;
            break;
          case 0x80:
            data_buffer[0] = DIRECT_ACCESS_DEVICE;
            data_buffer[1] = 0x80;
            data_buffer[2] = 0x00;
            data_buffer[3] = 8;
            memset(&data_buffer[4], ' ', 8);
            break;
          default:
            //KdPrint((__DRIVER_NAME "     Unknown Page %02x requested\n", Srb->Cdb[2]));
            Srb->SrbStatus = SRB_STATUS_ERROR;
            break;
          }
        }
        break;
      case XENVBD_DEVICETYPE_CDROM:
        if ((Srb->Cdb[1] & 1) == 0)
        {
          PINQUIRYDATA id = (PINQUIRYDATA)data_buffer;
          id->DeviceType = READ_ONLY_DIRECT_ACCESS_DEVICE;
          id->RemovableMedia = 1;
          id->Versions = 3;
          id->ResponseDataFormat = 0;
          id->AdditionalLength = FIELD_OFFSET(INQUIRYDATA, VendorSpecific) - FIELD_OFFSET(INQUIRYDATA, AdditionalLength);
          id->CommandQueue = 1;
          memcpy(id->VendorId, "XEN     ", 8); // vendor id
          memcpy(id->ProductId, "PV CDROM        ", 16); // product id
          memcpy(id->ProductRevisionLevel, "0000", 4); // product revision level
        }
        else
        {
          switch (Srb->Cdb[2])
          {
          case 0x00:
            data_buffer[0] = READ_ONLY_DIRECT_ACCESS_DEVICE;
            data_buffer[1] = 0x00;
            data_buffer[2] = 0x00;
            data_buffer[3] = 2;
            data_buffer[4] = 0x00;
            data_buffer[5] = 0x80;
            break;
          case 0x80:
            data_buffer[0] = READ_ONLY_DIRECT_ACCESS_DEVICE;
            data_buffer[1] = 0x80;
            data_buffer[2] = 0x00;
            data_buffer[3] = 8;
            data_buffer[4] = 0x31;
            data_buffer[5] = 0x32;
            data_buffer[6] = 0x33;
            data_buffer[7] = 0x34;
            data_buffer[8] = 0x35;
            data_buffer[9] = 0x36;
            data_buffer[10] = 0x37;
            data_buffer[11] = 0x38;
            break;
          default:
            //KdPrint((__DRIVER_NAME "     Unknown Page %02x requested\n", Srb->Cdb[2]));
            Srb->SrbStatus = SRB_STATUS_ERROR;
            break;
          }
        }
        break;
      default:
        //KdPrint((__DRIVER_NAME "     Unknown DeviceType %02x requested\n", xvdd->device_type));
        Srb->SrbStatus = SRB_STATUS_ERROR;
        break;
      }
      break;
    case SCSIOP_READ_CAPACITY:
      if (dump_mode)
        KdPrint((__DRIVER_NAME "     Command = READ_CAPACITY\n"));
      //KdPrint((__DRIVER_NAME "       LUN = %d, RelAdr = %d\n", Srb->Cdb[1] >> 4, Srb->Cdb[1] & 1));
      //KdPrint((__DRIVER_NAME "       LBA = %02x%02x%02x%02x\n", Srb->Cdb[2], Srb->Cdb[3], Srb->Cdb[4], Srb->Cdb[5]));
      //KdPrint((__DRIVER_NAME "       PMI = %d\n", Srb->Cdb[8] & 1));
      //data_buffer = LongLongToPtr(ScsiPortGetPhysicalAddress(xvdd, Srb, Srb->DataBuffer, &data_buffer_length).QuadPart);
      data_buffer = get_databuffer_virtual(xvdd, Srb);
      RtlZeroMemory(data_buffer, Srb->DataTransferLength);
      if ((xvdd->total_sectors - 1) >> 32)
      {
        data_buffer[0] = 0xff;
        data_buffer[1] = 0xff;
        data_buffer[2] = 0xff;
        data_buffer[3] = 0xff;
      }
      else
      {
        data_buffer[0] = (unsigned char)((xvdd->total_sectors - 1) >> 24) & 0xff;
        data_buffer[1] = (unsigned char)((xvdd->total_sectors - 1) >> 16) & 0xff;
        data_buffer[2] = (unsigned char)((xvdd->total_sectors - 1) >> 8) & 0xff;
        data_buffer[3] = (unsigned char)((xvdd->total_sectors - 1) >> 0) & 0xff;
      }
      data_buffer[4] = (unsigned char)(xvdd->bytes_per_sector >> 24) & 0xff;
      data_buffer[5] = (unsigned char)(xvdd->bytes_per_sector >> 16) & 0xff;
      data_buffer[6] = (unsigned char)(xvdd->bytes_per_sector >> 8) & 0xff;
      data_buffer[7] = (unsigned char)(xvdd->bytes_per_sector >> 0) & 0xff;
      Srb->ScsiStatus = 0;
      Srb->SrbStatus = SRB_STATUS_SUCCESS;
      break;
    case SCSIOP_READ_CAPACITY16:
      if (dump_mode)
        KdPrint((__DRIVER_NAME "     Command = READ_CAPACITY\n"));
      //KdPrint((__DRIVER_NAME "       LUN = %d, RelAdr = %d\n", Srb->Cdb[1] >> 4, Srb->Cdb[1] & 1));
      //KdPrint((__DRIVER_NAME "       LBA = %02x%02x%02x%02x\n", Srb->Cdb[2], Srb->Cdb[3], Srb->Cdb[4], Srb->Cdb[5]));
      //KdPrint((__DRIVER_NAME "       PMI = %d\n", Srb->Cdb[8] & 1));
      //data_buffer = LongLongToPtr(ScsiPortGetPhysicalAddress(xvdd, Srb, Srb->DataBuffer, &data_buffer_length).QuadPart);
      data_buffer = get_databuffer_virtual(xvdd, Srb);
      RtlZeroMemory(data_buffer, Srb->DataTransferLength);
      data_buffer[0] = (unsigned char)((xvdd->total_sectors - 1) >> 56) & 0xff;
      data_buffer[1] = (unsigned char)((xvdd->total_sectors - 1) >> 48) & 0xff;
      data_buffer[2] = (unsigned char)((xvdd->total_sectors - 1) >> 40) & 0xff;
      data_buffer[3] = (unsigned char)((xvdd->total_sectors - 1) >> 32) & 0xff;
      data_buffer[4] = (unsigned char)((xvdd->total_sectors - 1) >> 24) & 0xff;
      data_buffer[5] = (unsigned char)((xvdd->total_sectors - 1) >> 16) & 0xff;
      data_buffer[6] = (unsigned char)((xvdd->total_sectors - 1) >> 8) & 0xff;
      data_buffer[7] = (unsigned char)((xvdd->total_sectors - 1) >> 0) & 0xff;
      data_buffer[8] = (unsigned char)(xvdd->bytes_per_sector >> 24) & 0xff;
      data_buffer[9] = (unsigned char)(xvdd->bytes_per_sector >> 16) & 0xff;
      data_buffer[10] = (unsigned char)(xvdd->bytes_per_sector >> 8) & 0xff;
      data_buffer[11] = (unsigned char)(xvdd->bytes_per_sector >> 0) & 0xff;
      Srb->ScsiStatus = 0;
      Srb->SrbStatus = SRB_STATUS_SUCCESS;
      break;
    case SCSIOP_MODE_SENSE:
    case SCSIOP_MODE_SENSE10:
      if (dump_mode)
        KdPrint((__DRIVER_NAME "     Command = MODE_SENSE (DBD = %d, PC = %d, Page Code = %02x)\n", Srb->Cdb[1] & 0x08, Srb->Cdb[2] & 0xC0, Srb->Cdb[2] & 0x3F));
      XenVbd_FillModePage(xvdd, Srb);
      break;
    case SCSIOP_READ:
    case SCSIOP_READ16:
    case SCSIOP_WRITE:
    case SCSIOP_WRITE16:
      //if (dump_mode)
      //  KdPrint((__DRIVER_NAME "     Command = READ/WRITE\n"));
      XenVbd_PutSrbOnRing(xvdd, Srb);
      break;
    case SCSIOP_VERIFY:
      // Should we do more here?
      if (dump_mode)
        KdPrint((__DRIVER_NAME "     Command = VERIFY\n"));
      Srb->SrbStatus = SRB_STATUS_SUCCESS;
      break;
    case SCSIOP_REPORT_LUNS:
      if (dump_mode)
        KdPrint((__DRIVER_NAME "     Command = REPORT_LUNS\n"));
      Srb->SrbStatus = SRB_STATUS_SUCCESS;;
      break;
    case SCSIOP_REQUEST_SENSE:
      if (dump_mode)
        KdPrint((__DRIVER_NAME "     Command = REQUEST_SENSE\n"));
      XenVbd_MakeSense(xvdd, Srb, xvdd->last_sense_key, xvdd->last_additional_sense_code);
      Srb->SrbStatus = SRB_STATUS_SUCCESS;
      break;      
    case SCSIOP_READ_TOC:
      if (dump_mode)
        KdPrint((__DRIVER_NAME "     Command = READ_TOC\n"));
      //data_buffer = LongLongToPtr(ScsiPortGetPhysicalAddress(xvdd, Srb, Srb->DataBuffer, &data_buffer_length).QuadPart);
      data_buffer = get_databuffer_virtual(xvdd, Srb);
//      DataBuffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, HighPagePriority);
/*
#define READ_TOC_FORMAT_TOC         0x00
#define READ_TOC_FORMAT_SESSION     0x01
#define READ_TOC_FORMAT_FULL_TOC    0x02
#define READ_TOC_FORMAT_PMA         0x03
#define READ_TOC_FORMAT_ATIP        0x04
*/
//      KdPrint((__DRIVER_NAME "     Msf = %d\n", cdb->READ_TOC.Msf));
//      KdPrint((__DRIVER_NAME "     LogicalUnitNumber = %d\n", cdb->READ_TOC.LogicalUnitNumber));
//      KdPrint((__DRIVER_NAME "     Format2 = %d\n", cdb->READ_TOC.Format2));
//      KdPrint((__DRIVER_NAME "     StartingTrack = %d\n", cdb->READ_TOC.StartingTrack));
//      KdPrint((__DRIVER_NAME "     AllocationLength = %d\n", (cdb->READ_TOC.AllocationLength[0] << 8) | cdb->READ_TOC.AllocationLength[1]));
//      KdPrint((__DRIVER_NAME "     Control = %d\n", cdb->READ_TOC.Control));
//      KdPrint((__DRIVER_NAME "     Format = %d\n", cdb->READ_TOC.Format));
      switch (cdb->READ_TOC.Format2)
      {
      case READ_TOC_FORMAT_TOC:
        data_buffer[0] = 0; // length MSB
        data_buffer[1] = 10; // length LSB
        data_buffer[2] = 1; // First Track
        data_buffer[3] = 1; // Last Track
        data_buffer[4] = 0; // Reserved
        data_buffer[5] = 0x14; // current position data + uninterrupted data
        data_buffer[6] = 1; // last complete track
        data_buffer[7] = 0; // reserved
        data_buffer[8] = 0; // MSB Block
        data_buffer[9] = 0;
        data_buffer[10] = 0;
        data_buffer[11] = 0; // LSB Block
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;
      case READ_TOC_FORMAT_SESSION:
      case READ_TOC_FORMAT_FULL_TOC:
      case READ_TOC_FORMAT_PMA:
      case READ_TOC_FORMAT_ATIP:
        Srb->SrbStatus = SRB_STATUS_ERROR;
        break;
      }
      break;
    case SCSIOP_START_STOP_UNIT:
      if (dump_mode)
        KdPrint((__DRIVER_NAME "     Command = SCSIOP_START_STOP_UNIT\n"));
      Srb->SrbStatus = SRB_STATUS_SUCCESS;
      break;
    case SCSIOP_RESERVE_UNIT:
      if (dump_mode)
        KdPrint((__DRIVER_NAME "     Command = SCSIOP_RESERVE_UNIT\n"));
      Srb->SrbStatus = SRB_STATUS_SUCCESS;
      break;
    case SCSIOP_RELEASE_UNIT:
      if (dump_mode)
        KdPrint((__DRIVER_NAME "     Command = SCSIOP_RELEASE_UNIT\n"));
      Srb->SrbStatus = SRB_STATUS_SUCCESS;
      break;
    default:
      KdPrint((__DRIVER_NAME "     Unhandled EXECUTE_SCSI Command = %02X\n", Srb->Cdb[0]));
      Srb->SrbStatus = SRB_STATUS_ERROR;
      break;
    }
    if (Srb->SrbStatus == SRB_STATUS_ERROR)
    {
      KdPrint((__DRIVER_NAME "     EXECUTE_SCSI Command = %02X returned error %02x\n", Srb->Cdb[0], xvdd->last_sense_key));
      if (xvdd->last_sense_key == SCSI_SENSE_NO_SENSE)
      {
        xvdd->last_sense_key = SCSI_SENSE_ILLEGAL_REQUEST;
        xvdd->last_additional_sense_code = SCSI_ADSENSE_INVALID_CDB;
      }
      Srb->ScsiStatus = 0x02;
      XenVbd_MakeAutoSense(xvdd, Srb);
      ScsiPortNotification(RequestComplete, DeviceExtension, Srb);
      if (xvdd->device_state->suspend_resume_state_pdo == SR_STATE_RUNNING)
        ScsiPortNotification(NextLuRequest, DeviceExtension, 0, 0, 0);
    }
    else if (Srb->SrbStatus != SRB_STATUS_PENDING)
    {
      xvdd->last_sense_key = SCSI_SENSE_NO_SENSE;
      xvdd->last_additional_sense_code = SCSI_ADSENSE_NO_SENSE;
      ScsiPortNotification(RequestComplete, DeviceExtension, Srb);
      if (xvdd->device_state->suspend_resume_state_pdo == SR_STATE_RUNNING)
        ScsiPortNotification(NextLuRequest, DeviceExtension, 0, 0, 0);
    }
    break;
  case SRB_FUNCTION_IO_CONTROL:
    //KdPrint((__DRIVER_NAME "     SRB_FUNCTION_IO_CONTROL\n"));
    Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
    ScsiPortNotification(RequestComplete, DeviceExtension, Srb);
    if (xvdd->device_state->suspend_resume_state_pdo == SR_STATE_RUNNING)
      ScsiPortNotification(NextLuRequest, DeviceExtension, 0, 0, 0);
    break;
  case SRB_FUNCTION_FLUSH:
    KdPrint((__DRIVER_NAME "     SRB_FUNCTION_FLUSH\n"));
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    ScsiPortNotification(RequestComplete, DeviceExtension, Srb);
    if (xvdd->device_state->suspend_resume_state_pdo == SR_STATE_RUNNING)
      ScsiPortNotification(NextLuRequest, DeviceExtension, 0, 0, 0);
    break;
  case SRB_FUNCTION_SHUTDOWN:
    KdPrint((__DRIVER_NAME "     SRB_FUNCTION_SHUTDOWN %p\n", Srb));
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    ScsiPortNotification(RequestComplete, DeviceExtension, Srb);
    if (xvdd->device_state->suspend_resume_state_pdo == SR_STATE_RUNNING)
      ScsiPortNotification(NextLuRequest, DeviceExtension, 0, 0, 0);
    break;
  default:
    KdPrint((__DRIVER_NAME "     Unhandled Srb->Function = %08X\n", Srb->Function));
    Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
    ScsiPortNotification(RequestComplete, DeviceExtension, Srb);
    if (xvdd->device_state->suspend_resume_state_pdo == SR_STATE_RUNNING)
      ScsiPortNotification(NextLuRequest, DeviceExtension, 0, 0, 0);
    break;
  }

  //FUNCTION_EXIT();
  return TRUE;
}

static BOOLEAN DDKAPI
XenVbd_HwScsiResetBus(PVOID DeviceExtension, ULONG PathId)
{
  PXENVBD_DEVICE_DATA xvdd = DeviceExtension;

  UNREFERENCED_PARAMETER(DeviceExtension);
  UNREFERENCED_PARAMETER(PathId);

  KdPrint((__DRIVER_NAME " --> HwScsiResetBus\n"));

  KdPrint((__DRIVER_NAME "     IRQL = %d\n", KeGetCurrentIrql()));
  if (xvdd->ring_detect_state == 2 && xvdd->device_state->suspend_resume_state_pdo == SR_STATE_RUNNING)
  {
    ScsiPortNotification(NextRequest, DeviceExtension);
  }

  KdPrint((__DRIVER_NAME " <-- HwScsiResetBus\n"));


  return TRUE;
}

static BOOLEAN DDKAPI
XenVbd_HwScsiAdapterState(PVOID DeviceExtension, PVOID Context, BOOLEAN SaveState)
{
  UNREFERENCED_PARAMETER(DeviceExtension);
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(SaveState);

  KdPrint((__DRIVER_NAME " --> " __FUNCTION__ "\n"));
  KdPrint((__DRIVER_NAME "     IRQL = %d\n", KeGetCurrentIrql()));

  KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ "\n"));

  return TRUE;
}

static SCSI_ADAPTER_CONTROL_STATUS DDKAPI
XenVbd_HwScsiAdapterControl(PVOID DeviceExtension, SCSI_ADAPTER_CONTROL_TYPE ControlType, PVOID Parameters)
{
  SCSI_ADAPTER_CONTROL_STATUS Status = ScsiAdapterControlSuccess;
  PSCSI_SUPPORTED_CONTROL_TYPE_LIST SupportedControlTypeList;
  //KIRQL OldIrql;

  UNREFERENCED_PARAMETER(DeviceExtension);

  FUNCTION_ENTER();
  KdPrint((__DRIVER_NAME "     IRQL = %d\n", KeGetCurrentIrql()));

  switch (ControlType)
  {
  case ScsiQuerySupportedControlTypes:
    SupportedControlTypeList = (PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Parameters;
    KdPrint((__DRIVER_NAME "     ScsiQuerySupportedControlTypes (Max = %d)\n", SupportedControlTypeList->MaxControlType));
    SupportedControlTypeList->SupportedTypeList[ScsiQuerySupportedControlTypes] = TRUE;
    SupportedControlTypeList->SupportedTypeList[ScsiStopAdapter] = TRUE;
    SupportedControlTypeList->SupportedTypeList[ScsiRestartAdapter] = TRUE;
    break;
  case ScsiStopAdapter:
    KdPrint((__DRIVER_NAME "     ScsiStopAdapter\n"));
    /* I don't think we actually have to do anything here... xenpci cleans up all the xenbus stuff for us */
    break;
  case ScsiRestartAdapter:
    KdPrint((__DRIVER_NAME "     ScsiRestartAdapter\n"));
    break;
  case ScsiSetBootConfig:
    KdPrint((__DRIVER_NAME "     ScsiSetBootConfig\n"));
    break;
  case ScsiSetRunningConfig:
    KdPrint((__DRIVER_NAME "     ScsiSetRunningConfig\n"));
    break;
  default:
    KdPrint((__DRIVER_NAME "     UNKNOWN\n"));
    break;
  }

  FUNCTION_EXIT();
  
  return Status;
}

PVOID init_driver_extension;

static BOOLEAN
XenVbd_DmaNeedVirtualAddress(PIRP irp)
{
  PIO_STACK_LOCATION stack;

  //FUNCTION_ENTER();
  
  stack = IoGetCurrentIrpStackLocation(irp);
  if (stack->MajorFunction != IRP_MJ_SCSI)
  {
    KdPrint((__DRIVER_NAME "     Not IRP_MJ_SCSI\n"));
    //FUNCTION_EXIT();
    return FALSE; /* this actually shouldn't happen */
  }
  
  switch (stack->Parameters.Scsi.Srb->Cdb[0])
  {
  case SCSIOP_READ:
  case SCSIOP_READ16:
  case SCSIOP_WRITE:
  case SCSIOP_WRITE16:
    //KdPrint((__DRIVER_NAME "     read/write operation\n"));
    //FUNCTION_EXIT();
    return FALSE;
  default:
    //KdPrint((__DRIVER_NAME "     not a read/write operation\n"));
    //FUNCTION_EXIT();
    return TRUE;
  }
}

static ULONG
XenVbd_DmaGetAlignment(PIRP irp)
{
  UNREFERENCED_PARAMETER(irp);
  
  //FUNCTION_ENTER();
  //FUNCTION_EXIT();
  return 512;
}

dma_driver_extension_t *dma_driver_extension;

NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
  ULONG status;
  HW_INITIALIZATION_DATA HwInitializationData;
  PVOID driver_extension;
  PUCHAR ptr;

  FUNCTION_ENTER();
  KdPrint((__DRIVER_NAME "     IRQL = %d\n", KeGetCurrentIrql()));
  KdPrint((__DRIVER_NAME "     DriverObject = %p\n", DriverObject));

  /* RegistryPath == NULL when we are invoked as a crash dump driver */
  if (!RegistryPath)
  {
    dump_mode = TRUE;
  }
  
  if (!dump_mode)
  {
    IoAllocateDriverObjectExtension(DriverObject, UlongToPtr(XEN_INIT_DRIVER_EXTENSION_MAGIC), PAGE_SIZE, &driver_extension);
    ptr = driver_extension;
    ADD_XEN_INIT_REQ(&ptr, XEN_INIT_TYPE_RUN, NULL, NULL, NULL);
    ADD_XEN_INIT_REQ(&ptr, XEN_INIT_TYPE_RING, "ring-ref", NULL, NULL);
    ADD_XEN_INIT_REQ(&ptr, XEN_INIT_TYPE_EVENT_CHANNEL_IRQ, "event-channel", NULL, NULL);
    ADD_XEN_INIT_REQ(&ptr, XEN_INIT_TYPE_READ_STRING_FRONT, "device-type", NULL, NULL);
    ADD_XEN_INIT_REQ(&ptr, XEN_INIT_TYPE_READ_STRING_BACK, "mode", NULL, NULL);
    ADD_XEN_INIT_REQ(&ptr, XEN_INIT_TYPE_READ_STRING_BACK, "sectors", NULL, NULL);
    ADD_XEN_INIT_REQ(&ptr, XEN_INIT_TYPE_READ_STRING_BACK, "sector-size", NULL, NULL);
    ADD_XEN_INIT_REQ(&ptr, XEN_INIT_TYPE_GRANT_ENTRIES, NULL, ULongToPtr(1), NULL); /* 1 ref for use in crash dump */
    ADD_XEN_INIT_REQ(&ptr, XEN_INIT_TYPE_END, NULL, NULL, NULL);

    IoAllocateDriverObjectExtension(DriverObject, UlongToPtr(XEN_DMA_DRIVER_EXTENSION_MAGIC), sizeof(dma_driver_extension), &dma_driver_extension);  
    dma_driver_extension->need_virtual_address = XenVbd_DmaNeedVirtualAddress;
    dma_driver_extension->get_alignment = XenVbd_DmaGetAlignment;
  }

  RtlZeroMemory(&HwInitializationData, sizeof(HW_INITIALIZATION_DATA));

  HwInitializationData.HwInitializationDataSize = sizeof(HW_INITIALIZATION_DATA);
  HwInitializationData.AdapterInterfaceType = PNPBus;
  HwInitializationData.DeviceExtensionSize = sizeof(XENVBD_DEVICE_DATA);
  HwInitializationData.SpecificLuExtensionSize = 0;
  HwInitializationData.SrbExtensionSize = 0;
  HwInitializationData.NumberOfAccessRanges = 1;
#if 0
  HwInitializationData.MapBuffers = TRUE;
  HwInitializationData.NeedPhysicalAddresses = FALSE;
#else
  HwInitializationData.MapBuffers = FALSE;
  HwInitializationData.NeedPhysicalAddresses = TRUE;
#endif
  HwInitializationData.TaggedQueuing = FALSE;
  HwInitializationData.AutoRequestSense = TRUE;
  HwInitializationData.MultipleRequestPerLu = TRUE;
  HwInitializationData.ReceiveEvent = FALSE;
  HwInitializationData.VendorIdLength = 0;
  HwInitializationData.VendorId = NULL;
  HwInitializationData.DeviceIdLength = 0;
  HwInitializationData.DeviceId = NULL;

  HwInitializationData.HwInitialize = XenVbd_HwScsiInitialize;
  HwInitializationData.HwStartIo = XenVbd_HwScsiStartIo;
  HwInitializationData.HwInterrupt = XenVbd_HwScsiInterrupt;
  HwInitializationData.HwFindAdapter = XenVbd_HwScsiFindAdapter;
  HwInitializationData.HwResetBus = XenVbd_HwScsiResetBus;
  HwInitializationData.HwDmaStarted = NULL;
  HwInitializationData.HwAdapterState = XenVbd_HwScsiAdapterState;
  HwInitializationData.HwAdapterControl = XenVbd_HwScsiAdapterControl;

#if 0
  if (dump_mode)
    KdBreakPoint();
#endif

  status = ScsiPortInitialize(DriverObject, RegistryPath, &HwInitializationData, NULL);
  
  if(!NT_SUCCESS(status))
  {
    KdPrint((__DRIVER_NAME " ScsiPortInitialize failed with status 0x%08x\n", status));
  }

  FUNCTION_EXIT();

  return status;
}

