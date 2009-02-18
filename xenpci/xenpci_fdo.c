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

#include "xenpci.h"
#include <stdlib.h>
#include <aux_klib.h>

#define SYSRQ_PATH "control/sysrq"
#define SHUTDOWN_PATH "control/shutdown"
#define BALLOON_PATH "memory/target"

static VOID
XenPci_MapHalThenPatchKernel(PXENPCI_DEVICE_DATA xpdd)
{
  NTSTATUS status;
  PAUX_MODULE_EXTENDED_INFO amei;
  ULONG module_info_buffer_size;
  ULONG i;
   
  FUNCTION_ENTER();

  status = AuxKlibInitialize();
  amei = NULL;
  /* buffer size could change between requesting and allocating - need to loop until we are successful */
  while ((status = AuxKlibQueryModuleInformation(&module_info_buffer_size, sizeof(AUX_MODULE_EXTENDED_INFO), amei)) == STATUS_BUFFER_TOO_SMALL || amei == NULL)
  {
    if (amei != NULL)
      ExFreePoolWithTag(amei, XENPCI_POOL_TAG);
    amei = ExAllocatePoolWithTag(NonPagedPool, module_info_buffer_size, XENPCI_POOL_TAG);
  }
  
  KdPrint((__DRIVER_NAME "     AuxKlibQueryModuleInformation = %d\n", status));
  for (i = 0; i < module_info_buffer_size / sizeof(AUX_MODULE_EXTENDED_INFO); i++)
  {
    if (strcmp((PCHAR)amei[i].FullPathName + amei[i].FileNameOffset, "hal.dll") == 0)
    {
      KdPrint((__DRIVER_NAME "     hal.dll found at %p - %p\n", 
        amei[i].BasicInfo.ImageBase,
        ((PUCHAR)amei[i].BasicInfo.ImageBase) + amei[i].ImageSize));
      XenPci_PatchKernel(xpdd, amei[i].BasicInfo.ImageBase, amei[i].ImageSize);
    }
  }
  ExFreePoolWithTag(amei, XENPCI_POOL_TAG);
  FUNCTION_EXIT();
}

#if 0
PMDL
XenPCI_AllocMMIO(WDFDEVICE device, ULONG len)
{
  PMDL mdl = ExAllocatePoolWithTag(NonPagedPool, MmSizeOfMdl(0, len), XENPCI_POOL_TAG);
  PVOID va = MmAllocateMappingAddress(len, XENPCI_POOL_TAG);
  
  for (i = 0; i < ADDRESS_AND_SIZE_TO_SPAN_PAGES(0, len);
  
}
#endif

/*
 * Alloc MMIO from the device's MMIO region. There is no corresponding free() fn
 */
PHYSICAL_ADDRESS
XenPci_AllocMMIO(PXENPCI_DEVICE_DATA xpdd, ULONG len)
{
  PHYSICAL_ADDRESS addr;

  len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  addr = xpdd->platform_mmio_addr;
  addr.QuadPart += xpdd->platform_mmio_alloc;
  xpdd->platform_mmio_alloc += len;

  ASSERT(xpdd->platform_mmio_alloc <= xpdd->platform_mmio_len);

  return addr;
}

extern ULONG tpr_patch_requested;

NTSTATUS
XenPci_EvtDeviceQueryRemove(WDFDEVICE device)
{
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(device);
  NTSTATUS status;
  
  FUNCTION_ENTER();
  if (xpdd->removable)
    status = STATUS_SUCCESS;
  else
    status = STATUS_UNSUCCESSFUL;
  FUNCTION_EXIT();
  return status;
}

static NTSTATUS
XenPci_Init(PXENPCI_DEVICE_DATA xpdd)
{
  struct xen_add_to_physmap xatp;
  int ret;

  FUNCTION_ENTER();

  hvm_get_stubs(xpdd);

  if (!xpdd->shared_info_area)
  {
    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
    /* this should be safe as this part will never be called on resume where IRQL == HIGH_LEVEL */
    xpdd->shared_info_area_unmapped = XenPci_AllocMMIO(xpdd, PAGE_SIZE);
    xpdd->shared_info_area = MmMapIoSpace(xpdd->shared_info_area_unmapped,
      PAGE_SIZE, MmNonCached);
  }
  KdPrint((__DRIVER_NAME "     shared_info_area_unmapped.QuadPart = %lx\n", xpdd->shared_info_area_unmapped.QuadPart));
  xatp.domid = DOMID_SELF;
  xatp.idx = 0;
  xatp.space = XENMAPSPACE_shared_info;
  xatp.gpfn = (xen_pfn_t)(xpdd->shared_info_area_unmapped.QuadPart >> PAGE_SHIFT);
  KdPrint((__DRIVER_NAME "     gpfn = %x\n", xatp.gpfn));
  ret = HYPERVISOR_memory_op(xpdd, XENMEM_add_to_physmap, &xatp);
  KdPrint((__DRIVER_NAME "     hypervisor memory op (XENMAPSPACE_shared_info) ret = %d\n", ret));
  
  FUNCTION_EXIT();

  return STATUS_SUCCESS;
}

static NTSTATUS
XenPci_Resume(PXENPCI_DEVICE_DATA xpdd)
{
  return XenPci_Init(xpdd);
}

static VOID
XenPci_SysrqHandler(char *path, PVOID context)
{
  PXENPCI_DEVICE_DATA xpdd = context;
  char *value;
  char letter;
  char *res;

  UNREFERENCED_PARAMETER(path);

  FUNCTION_ENTER();

  XenBus_Read(xpdd, XBT_NIL, SYSRQ_PATH, &value);

  KdPrint((__DRIVER_NAME "     SysRq Value = %s\n", value));

  if (value != NULL && strlen(value) != 0)
  {
    letter = *value;
    res = XenBus_Write(xpdd, XBT_NIL, SYSRQ_PATH, "");
    if (res)
    {
      KdPrint(("Error writing sysrq path\n"));
      XenPci_FreeMem(res);
      return;
    }
  }
  else
  {
    letter = 0;
  }

  if (value != NULL)
  {
    XenPci_FreeMem(value);
  }

  switch (letter)
  {
  case 0:
    break;
  case 'B': /* cause a bug check */
    KeBugCheckEx(('X' << 16)|('E' << 8)|('N'), 0x00000001, 0x00000000, 0x00000000, 0x00000000);
    break;
  default:
    KdPrint(("     Unhandled sysrq letter %c\n", letter));
    break;
  }

  FUNCTION_EXIT();
}

#if 0
static VOID
XenPci_PrintPendingInterrupts()
{
  PULONG bitmap = (PULONG)0xFFFE0200;
  int i;
  int j;
  ULONG value;
  
  for (i = 0; i < 8; i++)
  {
    value = bitmap[(7 - i) * 4];
    if (value)
    {
      for (j = 0; j < 32; j++)
      {
        if ((value >> j) & 1)
          KdPrint(("     Interrupt pending on pin %d\n", ((7 - i) << 5) | j));
      }
    }
  }
}
#endif

static VOID
XenPci_Suspend0(PVOID context)
{
  PXENPCI_DEVICE_DATA xpdd = context;
  ULONG cancelled;
  
  FUNCTION_ENTER();

  GntTbl_Suspend(xpdd);
  
  cancelled = hvm_shutdown(xpdd, SHUTDOWN_suspend);
  KdPrint((__DRIVER_NAME "     back from suspend, cancelled = %d\n", cancelled));

  if (qemu_filtered_by_qemu)
  {
    XenPci_HideQemuDevices();
    ASSERT(qemu_filtered_by_qemu);
  } 

  XenPci_Resume(xpdd);
  GntTbl_Resume(xpdd);
  EvtChn_Resume(xpdd); /* this enables interrupts again too */

  FUNCTION_EXIT();
}

static VOID
XenPci_SuspendN(PVOID context)
{
  UNREFERENCED_PARAMETER(context);
  
  FUNCTION_ENTER();
  KdPrint((__DRIVER_NAME "     doing nothing on cpu N\n"));
  FUNCTION_EXIT();
}

/* Called at PASSIVE_LEVEL */
static VOID DDKAPI
XenPci_SuspendResume(WDFWORKITEM workitem)
{
  NTSTATUS status;
  //KAFFINITY ActiveProcessorMask = 0; // this is for Vista+
  WDFDEVICE device = WdfWorkItemGetParentObject(workitem);
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(device);
  WDFCHILDLIST child_list = WdfFdoGetDefaultChildList(device);
  WDF_CHILD_LIST_ITERATOR child_iterator;
  WDFDEVICE child_device;

  FUNCTION_ENTER();

  if (xpdd->suspend_state == SUSPEND_STATE_NONE)
  {
    xpdd->suspend_state = SUSPEND_STATE_SCHEDULED;
    KeMemoryBarrier();

    WDF_CHILD_LIST_ITERATOR_INIT(&child_iterator, WdfRetrievePresentChildren);
    WdfChildListBeginIteration(child_list, &child_iterator);
    while ((status = WdfChildListRetrieveNextDevice(child_list, &child_iterator, &child_device, NULL)) == STATUS_SUCCESS)
    {
      KdPrint((__DRIVER_NAME "     Suspending child\n"));
      XenPci_Pdo_Suspend(child_device);
    }
    KdPrint((__DRIVER_NAME "     WdfChildListRetrieveNextDevice = %08x, STATUS_NO_MORE_ENTRIES = %08x\n", status, STATUS_NO_MORE_ENTRIES));
    WdfChildListEndIteration(child_list, &child_iterator);

    XenBus_Suspend(xpdd);
    EvtChn_Suspend(xpdd);
    XenPci_HighSync(XenPci_Suspend0, XenPci_SuspendN, xpdd);

    xpdd->suspend_state = SUSPEND_STATE_RESUMING;
    XenBus_Resume(xpdd);

    WdfChildListBeginIteration(child_list, &child_iterator);
    while ((status = WdfChildListRetrieveNextDevice(child_list, &child_iterator, &child_device, NULL)) == STATUS_SUCCESS)
    {
      KdPrint((__DRIVER_NAME "     Resuming child\n"));
      XenPci_Pdo_Resume(child_device);
    }
    KdPrint((__DRIVER_NAME "     WdfChildListRetrieveNextDevice = %08x, STATUS_NO_MORE_ENTRIES = %08x\n", status, STATUS_NO_MORE_ENTRIES));
    WdfChildListEndIteration(child_list, &child_iterator);

    xpdd->suspend_state = SUSPEND_STATE_NONE;
  }
  FUNCTION_EXIT();
}

static void
XenPci_ShutdownHandler(char *path, PVOID context)
{
  NTSTATUS status;
  WDFDEVICE device = context;
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(device);
  char *res;
  char *value;
  //KIRQL old_irql;
  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_WORKITEM_CONFIG workitem_config;
  WDFWORKITEM workitem;

  UNREFERENCED_PARAMETER(path);

  FUNCTION_ENTER();

  res = XenBus_Read(xpdd, XBT_NIL, SHUTDOWN_PATH, &value);
  if (res)
  {
    KdPrint(("Error reading shutdown path - %s\n", res));
    XenPci_FreeMem(res);
    return;
  }

  KdPrint((__DRIVER_NAME "     Shutdown value = %s\n", value));

  if (strlen(value) && strcmp(value, "suspend") == 0)
  {
    {
      KdPrint((__DRIVER_NAME "     Suspend detected\n"));
      /* we have to queue this as a work item as we stop the xenbus thread, which we are currently running in! */
      WDF_WORKITEM_CONFIG_INIT(&workitem_config, XenPci_SuspendResume);
      WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
      attributes.ParentObject = device;
      status = WdfWorkItemCreate(&workitem_config, &attributes, &workitem);
      // TODO: check status here
      WdfWorkItemEnqueue(workitem);
    }
  }

  XenPci_FreeMem(value);

  FUNCTION_EXIT();
}

static VOID
XenPci_DeviceWatchHandler(char *path, PVOID context)
{
  char **bits;
  int count;
  char *err;
  char *value;
  PXENPCI_DEVICE_DATA xpdd = context;

  FUNCTION_ENTER();

  bits = SplitString(path, '/', 4, &count);
  if (count == 3)
  {
    err = XenBus_Read(xpdd, XBT_NIL, path, &value);
    if (err)
    {
      /* obviously path no longer exists, in which case the removal is being taken care of elsewhere and we shouldn't invalidate now */
      XenPci_FreeMem(err);
    }
    else
    {
      XenPci_FreeMem(value);
      /* we probably have to be a bit smarter here and do nothing if xenpci isn't running yet */
      KdPrint((__DRIVER_NAME "     Invalidating Device Relations\n"));
      //IoInvalidateDeviceRelations(xpdd->common.pdo, BusRelations);
    }
  }
  FreeSplitString(bits, count);

  FUNCTION_EXIT();
}

NTSTATUS
XenPci_EvtDevicePrepareHardware (WDFDEVICE device, WDFCMRESLIST resources_raw, WDFCMRESLIST resources_translated)
{
  NTSTATUS status = STATUS_SUCCESS;
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(device);
  PCM_PARTIAL_RESOURCE_DESCRIPTOR raw_descriptor, translated_descriptor;
  ULONG i;

  FUNCTION_ENTER();
  
  ASSERT(WdfCmResourceListGetCount(resources_raw) == WdfCmResourceListGetCount(resources_translated));
  
  for (i = 0; i < WdfCmResourceListGetCount(resources_raw); i++)
  {
    raw_descriptor = WdfCmResourceListGetDescriptor(resources_raw, i);
    translated_descriptor = WdfCmResourceListGetDescriptor(resources_translated, i);
    switch (raw_descriptor->Type) {
    case CmResourceTypePort:
      KdPrint((__DRIVER_NAME "     IoPort Address(%x) Length: %d\n", translated_descriptor->u.Port.Start.LowPart, translated_descriptor->u.Port.Length));
      xpdd->platform_ioport_addr = translated_descriptor->u.Port.Start.LowPart;
      xpdd->platform_ioport_len = translated_descriptor->u.Port.Length;
      break;
    case CmResourceTypeMemory:
      KdPrint((__DRIVER_NAME "     Memory mapped CSR:(%x:%x) Length:(%d)\n", translated_descriptor->u.Memory.Start.LowPart, translated_descriptor->u.Memory.Start.HighPart, translated_descriptor->u.Memory.Length));
      KdPrint((__DRIVER_NAME "     Memory flags = %04X\n", translated_descriptor->Flags));
#if 0      
      mmio_freelist_free = 0;
      for (j = 0; j < translated_descriptor->u.Memory.Length >> PAGE_SHIFT; j++)
        put_mmio_on_freelist((xpdd->platform_mmio_addr >> PAGE_SHIFT) + j);
#endif
      xpdd->platform_mmio_addr = translated_descriptor->u.Memory.Start;
      xpdd->platform_mmio_len = translated_descriptor->u.Memory.Length;
      xpdd->platform_mmio_flags = translated_descriptor->Flags;
      break;
    case CmResourceTypeInterrupt:
	    xpdd->irq_level = (KIRQL)translated_descriptor->u.Interrupt.Level;
  	  xpdd->irq_vector = translated_descriptor->u.Interrupt.Vector;
	    xpdd->irq_affinity = translated_descriptor->u.Interrupt.Affinity;
      xpdd->irq_mode = (translated_descriptor->Flags & CM_RESOURCE_INTERRUPT_LATCHED)?Latched:LevelSensitive;
      xpdd->irq_number = raw_descriptor->u.Interrupt.Vector;      
      KdPrint((__DRIVER_NAME "     irq_number = %03x\n", raw_descriptor->u.Interrupt.Vector));
      KdPrint((__DRIVER_NAME "     irq_vector = %03x\n", translated_descriptor->u.Interrupt.Vector));
      KdPrint((__DRIVER_NAME "     irq_level = %03x\n", translated_descriptor->u.Interrupt.Level));
      KdPrint((__DRIVER_NAME "     irq_mode = %s\n", (xpdd->irq_mode == Latched)?"Latched":"LevelSensitive"));
      switch(translated_descriptor->ShareDisposition)
      {
      case CmResourceShareDeviceExclusive:
        KdPrint((__DRIVER_NAME "     ShareDisposition = CmResourceShareDeviceExclusive\n"));
        break;
      case CmResourceShareDriverExclusive:
        KdPrint((__DRIVER_NAME "     ShareDisposition = CmResourceShareDriverExclusive\n"));
        break;
      case CmResourceShareShared:
        KdPrint((__DRIVER_NAME "     ShareDisposition = CmResourceShareShared\n"));
        break;
      default:
        KdPrint((__DRIVER_NAME "     ShareDisposition = %d\n", translated_descriptor->ShareDisposition));
        break;
      }
      break;
    case CmResourceTypeDevicePrivate:
      KdPrint((__DRIVER_NAME "     Private Data: 0x%02x 0x%02x 0x%02x\n", translated_descriptor->u.DevicePrivate.Data[0], translated_descriptor->u.DevicePrivate.Data[1], translated_descriptor->u.DevicePrivate.Data[2]));
      break;
    default:
      KdPrint((__DRIVER_NAME "     Unhandled resource type (0x%x)\n", translated_descriptor->Type));
      break;
    }
  }

  FUNCTION_EXIT();
  
  return status;
}

NTSTATUS
XenPci_EvtDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous_state)
{
  NTSTATUS status = STATUS_SUCCESS;
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(device);

  UNREFERENCED_PARAMETER(previous_state);

  FUNCTION_ENTER();
  
  XenPci_Init(xpdd);
  if (tpr_patch_requested && !xpdd->tpr_patched)
  {
    XenPci_MapHalThenPatchKernel(xpdd);
    xpdd->tpr_patched = TRUE;
  }
  GntTbl_Init(xpdd);
  EvtChn_Init(xpdd);

  FUNCTION_EXIT();

  return status;
}

NTSTATUS
XenPci_EvtDeviceD0EntryPostInterruptsEnabled(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous_state)
{
  NTSTATUS status = STATUS_SUCCESS;
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(device);
  PCHAR response;

  UNREFERENCED_PARAMETER(previous_state);

  FUNCTION_ENTER();
  
  XenBus_Init(xpdd);

  response = XenBus_AddWatch(xpdd, XBT_NIL, SYSRQ_PATH, XenPci_SysrqHandler, xpdd);
  
  response = XenBus_AddWatch(xpdd, XBT_NIL, SHUTDOWN_PATH, XenPci_ShutdownHandler, device);

  response = XenBus_AddWatch(xpdd, XBT_NIL, "device", XenPci_DeviceWatchHandler, xpdd);

#if 0
  response = XenBus_AddWatch(xpdd, XBT_NIL, BALLOON_PATH, XenPci_BalloonHandler, Device);
  KdPrint((__DRIVER_NAME "     balloon watch response = '%s'\n", response));
#endif

#if 0
  status = IoSetDeviceInterfaceState(&xpdd->legacy_interface_name, TRUE);
  if (!NT_SUCCESS(status))
  {
    KdPrint((__DRIVER_NAME "     IoSetDeviceInterfaceState (legacy) failed with status 0x%08x\n", status));
  }

  status = IoSetDeviceInterfaceState(&xpdd->interface_name, TRUE);
  if (!NT_SUCCESS(status))
  {
    KdPrint((__DRIVER_NAME "     IoSetDeviceInterfaceState failed with status 0x%08x\n", status));
  }
#endif

  FUNCTION_EXIT();
  
  return status;
}

NTSTATUS
XenPci_EvtDeviceD0ExitPreInterruptsDisabled(WDFDEVICE device, WDF_POWER_DEVICE_STATE target_state)
{
  NTSTATUS status = STATUS_SUCCESS;
  
  UNREFERENCED_PARAMETER(device);
  UNREFERENCED_PARAMETER(target_state);
  
  FUNCTION_ENTER();
  FUNCTION_EXIT();
  
  return status;
}

NTSTATUS
XenPci_EvtDeviceD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target_state)
{
  NTSTATUS status = STATUS_SUCCESS;
  
  UNREFERENCED_PARAMETER(device);
  UNREFERENCED_PARAMETER(target_state);
  
  FUNCTION_ENTER();
  FUNCTION_EXIT();
  
  return status;
}

NTSTATUS
XenPci_EvtDeviceReleaseHardware(WDFDEVICE device, WDFCMRESLIST resources_translated)
{
  NTSTATUS status = STATUS_SUCCESS;
  
  UNREFERENCED_PARAMETER(device);
  UNREFERENCED_PARAMETER(resources_translated);
  
  FUNCTION_ENTER();
  FUNCTION_EXIT();
  
  return status;
}

VOID
XenPci_EvtChildListScanForChildren(WDFCHILDLIST child_list)
{
  NTSTATUS status;
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(WdfChildListGetDevice(child_list));
  char *msg;
  char **devices;
  char **instances;
  int i, j;
  CHAR path[128];
  XENPCI_PDO_IDENTIFICATION_DESCRIPTION child_description;
  
  FUNCTION_ENTER();

  WdfChildListBeginScan(child_list);

  msg = XenBus_List(xpdd, XBT_NIL, "device", &devices);
  if (!msg)
  {
    for (i = 0; devices[i]; i++)
    {
      RtlStringCbPrintfA(path, ARRAY_SIZE(path), "device/%s", devices[i]);
      msg = XenBus_List(xpdd, XBT_NIL, path, &instances);
      if (!msg)
      {
        for (j = 0; instances[j]; j++)
        {
          /* the device comparison is done as a memory compare so zero-ing the structure is important */
          RtlZeroMemory(&child_description, sizeof(child_description));
          WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&child_description.header, sizeof(child_description));
          RtlStringCbPrintfA(path, ARRAY_SIZE(path), "device/%s/%s", devices[i], instances[j]);
          RtlStringCbCopyA(child_description.path, ARRAY_SIZE(child_description.path), path);
          RtlStringCbCopyA(child_description.device, ARRAY_SIZE(child_description.device), devices[i]);
          child_description.index = atoi(instances[j]);
          status = WdfChildListAddOrUpdateChildDescriptionAsPresent(child_list, &child_description.header, NULL);
          if (!NT_SUCCESS(status))
          {
            KdPrint((__DRIVER_NAME "     WdfChildListAddOrUpdateChildDescriptionAsPresent failed with status 0x%08x\n", status));
          }
          XenPci_FreeMem(instances[j]);
        }
        XenPci_FreeMem(instances);
      }
      else
      {
        // wtf do we do here???
        KdPrint((__DRIVER_NAME "     Failed to list %s tree\n", devices[i]));
      }
      XenPci_FreeMem(devices[i]);
    }
    XenPci_FreeMem(devices);
  }
  else
  {
    // wtf do we do here???
    KdPrint((__DRIVER_NAME "     Failed to list device tree\n"));
  }

  WdfChildListEndScan(child_list);
  
  FUNCTION_EXIT();
}

#if 0
#if 0
static VOID
XenBus_BalloonHandler(char *Path, PVOID Data);
#endif

#pragma warning(disable : 4200) // zero-sized array

NTSTATUS
XenPci_Power_Fdo(PDEVICE_OBJECT device_object, PIRP irp)
{
  NTSTATUS status;
  PIO_STACK_LOCATION stack;
  POWER_STATE_TYPE power_type;
  POWER_STATE power_state;
  PXENPCI_DEVICE_DATA xpdd = (PXENPCI_DEVICE_DATA)device_object->DeviceExtension;

  UNREFERENCED_PARAMETER(device_object);
  
  FUNCTION_ENTER();

  stack = IoGetCurrentIrpStackLocation(irp);
  power_type = stack->Parameters.Power.Type;
  power_state = stack->Parameters.Power.State;

  switch (stack->MinorFunction)
  {
  case IRP_MN_POWER_SEQUENCE:
    //irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
    KdPrint((__DRIVER_NAME "     IRP_MN_POWER_SEQUENCE\n"));
    break;
  case IRP_MN_QUERY_POWER:
    KdPrint((__DRIVER_NAME "     IRP_MN_QUERY_POWER\n"));
    break;
  case IRP_MN_SET_POWER:
    KdPrint((__DRIVER_NAME "     IRP_MN_SET_POWER\n"));
    switch (power_type) {
    case DevicePowerState:
      KdPrint((__DRIVER_NAME "     DevicePowerState\n"));
      break;
    case SystemPowerState:
      KdPrint((__DRIVER_NAME "     SystemPowerState\n"));
      break;
    default:
      KdPrint((__DRIVER_NAME "     %d\n", power_type));
    }    
    break;
  case IRP_MN_WAIT_WAKE:
    KdPrint((__DRIVER_NAME "     IRP_MN_WAIT_WAKE\n"));
    break;
  default:
    KdPrint((__DRIVER_NAME "     IRP_MN_%d\n", stack->MinorFunction));
    break;  
  }
  PoStartNextPowerIrp(irp);
  IoSkipCurrentIrpStackLocation(irp);
  status = PoCallDriver(xpdd->common.lower_do, irp);
  
  FUNCTION_EXIT();

  return status;
}

NTSTATUS
XenPci_Dummy_Fdo(PDEVICE_OBJECT device_object, PIRP irp)
{
  NTSTATUS status;
  PIO_STACK_LOCATION stack;
  PXENPCI_DEVICE_DATA xpdd;

  FUNCTION_ENTER();

  xpdd = (PXENPCI_DEVICE_DATA)device_object->DeviceExtension;
  stack = IoGetCurrentIrpStackLocation(irp);
  IoSkipCurrentIrpStackLocation(irp);
  status = IoCallDriver(xpdd->common.lower_do, irp);

  FUNCTION_EXIT();

  return status;
}

static NTSTATUS
XenPci_Pnp_IoCompletion(PDEVICE_OBJECT device_object, PIRP irp, PVOID context)
{
  PKEVENT event = (PKEVENT)context;

  UNREFERENCED_PARAMETER(device_object);

  FUNCTION_ENTER();

  if (irp->PendingReturned)
  {
    KeSetEvent(event, IO_NO_INCREMENT, FALSE);
  }

  FUNCTION_EXIT();

  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
XenPci_QueueWorkItem(PDEVICE_OBJECT device_object, PIO_WORKITEM_ROUTINE routine, PVOID context)
{
  PIO_WORKITEM work_item;
  NTSTATUS status = STATUS_SUCCESS;

	work_item = IoAllocateWorkItem(device_object);
	IoQueueWorkItem(work_item, routine, DelayedWorkQueue, context);
	
  return status;
}

static NTSTATUS
XenPci_SendAndWaitForIrp(PDEVICE_OBJECT device_object, PIRP irp)
{
  NTSTATUS status;
  PXENPCI_DEVICE_DATA xpdd = (PXENPCI_DEVICE_DATA)device_object->DeviceExtension;
  KEVENT event;

  UNREFERENCED_PARAMETER(device_object);

  FUNCTION_ENTER();

  KeInitializeEvent(&event, NotificationEvent, FALSE);

  IoCopyCurrentIrpStackLocationToNext(irp);
  IoSetCompletionRoutine(irp, XenPci_Pnp_IoCompletion, &event, TRUE, TRUE, TRUE);

  status = IoCallDriver(xpdd->common.lower_do, irp);

  if (status == STATUS_PENDING)
  {
    KdPrint((__DRIVER_NAME "     waiting ...\n"));
    KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    KdPrint((__DRIVER_NAME "     ... done\n"));
    status = irp->IoStatus.Status;
  }

  FUNCTION_EXIT();

  return status;
}

static NTSTATUS
XenPci_ProcessShutdownIrp(PXENPCI_DEVICE_DATA xpdd)
{
  PIO_STACK_LOCATION stack;
  NTSTATUS status;
  PIRP irp;
  KIRQL old_irql;
  ULONG length;

  FUNCTION_ENTER();

  KeAcquireSpinLock(&xpdd->shutdown_ring_lock, &old_irql);
  if (xpdd->shutdown_irp)
  {
    irp = xpdd->shutdown_irp;
    stack = IoGetCurrentIrpStackLocation(irp);
    KdPrint((__DRIVER_NAME "     stack = %p\n", stack));
    KdPrint((__DRIVER_NAME "     length = %d, buffer = %p\n", stack->Parameters.Read.Length, irp->AssociatedIrp.SystemBuffer));
    length = min(xpdd->shutdown_prod - xpdd->shutdown_cons, stack->Parameters.Read.Length);
    KdPrint((__DRIVER_NAME "     length = %d\n", length));
    if (length > 0)
    {
      memcpy(irp->AssociatedIrp.SystemBuffer, &xpdd->shutdown_ring[xpdd->shutdown_cons & (SHUTDOWN_RING_SIZE - 1)], length);
      xpdd->shutdown_cons += length;
      if (xpdd->shutdown_cons > SHUTDOWN_RING_SIZE)
      {
        xpdd->shutdown_cons -= SHUTDOWN_RING_SIZE;
        xpdd->shutdown_prod -= SHUTDOWN_RING_SIZE;
        xpdd->shutdown_start -= SHUTDOWN_RING_SIZE;
      }
      xpdd->shutdown_irp = NULL;
      KeReleaseSpinLock(&xpdd->shutdown_ring_lock, old_irql);
      status = STATUS_SUCCESS;    
      irp->IoStatus.Status = status;
      irp->IoStatus.Information = length;
      IoSetCancelRoutine(irp, NULL);
      IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
    else
    {
      KeReleaseSpinLock(&xpdd->shutdown_ring_lock, old_irql);
      KdPrint((__DRIVER_NAME "     nothing to read... pending\n"));
      IoMarkIrpPending(irp);
      status = STATUS_PENDING;
    }
  }
  else
  {
    KdPrint((__DRIVER_NAME "     no pending irp\n"));
    KeReleaseSpinLock(&xpdd->shutdown_ring_lock, old_irql);
    status = STATUS_SUCCESS;
  }  

  FUNCTION_EXIT();

  return status;
}

static VOID
XenBus_ShutdownIoCancel(PDEVICE_OBJECT device_object, PIRP irp)
{
  PXENPCI_DEVICE_DATA xpdd;
  KIRQL old_irql;

  FUNCTION_ENTER();

  xpdd = (PXENPCI_DEVICE_DATA)device_object->DeviceExtension;
  IoReleaseCancelSpinLock(irp->CancelIrql);
  KeAcquireSpinLock(&xpdd->shutdown_ring_lock, &old_irql);
  if (irp == xpdd->shutdown_irp)
  {
    KdPrint((__DRIVER_NAME "     Not the current irp???\n"));
    xpdd->shutdown_irp = NULL;
  }
  irp->IoStatus.Status = STATUS_CANCELLED;
  irp->IoStatus.Information = 0;
  KeReleaseSpinLock(&xpdd->shutdown_ring_lock, old_irql);
  IoCompleteRequest(irp, IO_NO_INCREMENT);

  FUNCTION_EXIT();
}

struct {
  volatile ULONG do_spin;
  volatile ULONG abort_spin;
  volatile LONG nr_spinning;
  KDPC dpcs[MAX_VIRT_CPUS];
  KEVENT spin_event;
  KEVENT resume_event;
} typedef SUSPEND_INFO, *PSUSPEND_INFO;

/* runs at PASSIVE_LEVEL */
static DDKAPI VOID
XenPci_CompleteResume(PDEVICE_OBJECT device_object, PVOID context)
{
  PSUSPEND_INFO suspend_info = context;
  PXENPCI_DEVICE_DATA xpdd;
  PXEN_CHILD child;

  UNREFERENCED_PARAMETER(context);
  FUNCTION_ENTER();

  xpdd = (PXENPCI_DEVICE_DATA)device_object->DeviceExtension;

  while (suspend_info->nr_spinning)
  {
    KdPrint((__DRIVER_NAME "     %d processors are still spinning\n", suspend_info->nr_spinning));
    KeWaitForSingleObject(&suspend_info->spin_event, Executive, KernelMode, FALSE, NULL);
  }
  KdPrint((__DRIVER_NAME "     all other processors have stopped spinning\n"));

  XenBus_Resume(xpdd);

  for (child = (PXEN_CHILD)xpdd->child_list.Flink; child != (PXEN_CHILD)&xpdd->child_list; child = (PXEN_CHILD)child->entry.Flink)
  {
    XenPci_Pdo_Resume(child->context->common.pdo);
  }

  xpdd->suspend_state = SUSPEND_STATE_NONE;

  FUNCTION_EXIT();
}

/* Called at DISPATCH_LEVEL */
static DDKAPI VOID
XenPci_Suspend(
  PRKDPC Dpc,
  PVOID Context,
  PVOID SystemArgument1,
  PVOID SystemArgument2)
{
  PXENPCI_DEVICE_DATA xpdd = Context;
  PSUSPEND_INFO suspend_info = SystemArgument1;
  ULONG ActiveProcessorCount;
  KIRQL old_irql;
  int cancelled = 0;
  PXEN_CHILD child;
  
  //PUCHAR gnttbl_backup[PAGE_SIZE * NR_GRANT_FRAMES];

  UNREFERENCED_PARAMETER(Dpc);
  UNREFERENCED_PARAMETER(SystemArgument2);

  FUNCTION_ENTER();
  FUNCTION_MSG("(CPU = %d)\n", KeGetCurrentProcessorNumber());

  if (KeGetCurrentProcessorNumber() != 0)
  {
    KdPrint((__DRIVER_NAME "     CPU %d spinning...\n", KeGetCurrentProcessorNumber()));
    KeRaiseIrql(HIGH_LEVEL, &old_irql);
    InterlockedIncrement(&suspend_info->nr_spinning);
    while(suspend_info->do_spin && !suspend_info->abort_spin)
    {
      KeStallExecutionProcessor(1);
      KeMemoryBarrier();
    }
    if (suspend_info->abort_spin)
    {
      KdPrint((__DRIVER_NAME "     CPU %d spin aborted\n", KeGetCurrentProcessorNumber()));
KeBugCheckEx(('X' << 16)|('E' << 8)|('N'), 0x00000003, 0x00000001, 0x00000000, 0x00000000);
      return;
    }
    KeLowerIrql(old_irql);
    InterlockedDecrement(&suspend_info->nr_spinning);
    KeSetEvent(&suspend_info->spin_event, IO_NO_INCREMENT, FALSE);
    FUNCTION_EXIT();
    return;
  }
  ActiveProcessorCount = (ULONG)KeNumberProcessors;

  KeRaiseIrql(HIGH_LEVEL, &old_irql);
  xpdd->suspend_state = SUSPEND_STATE_HIGH_IRQL;
  while (suspend_info->nr_spinning < (LONG)ActiveProcessorCount - 1 && !suspend_info->abort_spin)
  {
    KeStallExecutionProcessor(1);
    //HYPERVISOR_yield(xpdd);
    KeMemoryBarrier();
  }
  if (suspend_info->abort_spin)
  {
    KdPrint((__DRIVER_NAME "     CPU %d spin aborted\n", KeGetCurrentProcessorNumber()));
KeBugCheckEx(('X' << 16)|('E' << 8)|('N'), 0x00000003, 0x00000003, 0x00000000, 0x00000000);
    return;
  }
  cancelled = hvm_shutdown(Context, SHUTDOWN_suspend);
  KdPrint((__DRIVER_NAME "     back from suspend, cancelled = %d\n", cancelled));
  
  XenPci_Init(xpdd);
  
  GntTbl_InitMap(Context);

  /* this enables interrupts again too */  
  EvtChn_Init(xpdd);

  for (child = (PXEN_CHILD)xpdd->child_list.Flink; child != (PXEN_CHILD)&xpdd->child_list; child = (PXEN_CHILD)child->entry.Flink)
  {
    child->context->device_state.resume_state = RESUME_STATE_BACKEND_RESUME;
  }
  KeLowerIrql(old_irql);
  xpdd->suspend_state = SUSPEND_STATE_RESUMING;
  suspend_info->do_spin = FALSE;
  KeMemoryBarrier();  
  KeSetEvent(&suspend_info->resume_event, IO_NO_INCREMENT, FALSE);
  FUNCTION_EXIT();
}

/* Called at PASSIVE_LEVEL */
static VOID DDKAPI
XenPci_BeginSuspend(PDEVICE_OBJECT device_object, PVOID context)
{
  //KAFFINITY ActiveProcessorMask = 0; // this is for Vista+
  PXENPCI_DEVICE_DATA xpdd = device_object->DeviceExtension;
  ULONG ActiveProcessorCount;
  ULONG i;
  PSUSPEND_INFO suspend_info;
  //PKDPC Dpc;
  KIRQL OldIrql;
  PXEN_CHILD child;

  UNREFERENCED_PARAMETER(context);
  FUNCTION_ENTER();

  if (xpdd->suspend_state == SUSPEND_STATE_NONE)
  {
    xpdd->suspend_state = SUSPEND_STATE_SCHEDULED;
    KeMemoryBarrier();

    suspend_info = ExAllocatePoolWithTag(NonPagedPool, sizeof(SUSPEND_INFO), XENPCI_POOL_TAG);
    RtlZeroMemory(suspend_info, sizeof(SUSPEND_INFO));
    KeInitializeEvent(&suspend_info->spin_event, SynchronizationEvent, FALSE);
    KeInitializeEvent(&suspend_info->resume_event, SynchronizationEvent, FALSE);
    
    for (child = (PXEN_CHILD)xpdd->child_list.Flink; child != (PXEN_CHILD)&xpdd->child_list; child = (PXEN_CHILD)child->entry.Flink)
    {
      XenPci_Pdo_Suspend(child->context->common.pdo);
    }

    XenBus_Suspend(xpdd);

    EvtChn_Shutdown(xpdd);

    //ActiveProcessorCount = KeQueryActiveProcessorCount(&ActiveProcessorMask); // this is for Vista+
    ActiveProcessorCount = (ULONG)KeNumberProcessors;
    /* Go to HIGH_LEVEL to prevent any races with Dpc's on the current processor */
    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    suspend_info->do_spin = TRUE;
    for (i = 0; i < ActiveProcessorCount; i++)
    {
      //Dpc = ExAllocatePoolWithTag(NonPagedPool, sizeof(KDPC), XENPCI_POOL_TAG);
      KeInitializeDpc(&suspend_info->dpcs[i], XenPci_Suspend, xpdd);
      KeSetTargetProcessorDpc(&suspend_info->dpcs[i], (CCHAR)i);
      KeSetImportanceDpc(&suspend_info->dpcs[i], HighImportance);
      KdPrint((__DRIVER_NAME "     queuing Dpc for CPU %d\n", i));
      KeInsertQueueDpc(&suspend_info->dpcs[i], suspend_info, NULL);
    }
    KdPrint((__DRIVER_NAME "     All Dpc's queued\n"));
    KeMemoryBarrier();
    KeLowerIrql(OldIrql);
    KdPrint((__DRIVER_NAME "     Waiting for resume_event\n"));
    KeWaitForSingleObject(&suspend_info->resume_event, Executive, KernelMode, FALSE, NULL);
    KdPrint((__DRIVER_NAME "     Got resume_event\n"));
    //xpdd->log_interrupts = TRUE;
    XenPci_CompleteResume(device_object, suspend_info);
  }
  FUNCTION_EXIT();
}

static void
XenPci_ShutdownHandler(char *path, PVOID context)
{
  PXENPCI_DEVICE_DATA xpdd = (PXENPCI_DEVICE_DATA)context;
  char *res;
  char *value;
  KIRQL old_irql;
  PIO_WORKITEM work_item;

  UNREFERENCED_PARAMETER(path);

  FUNCTION_ENTER();

  res = XenBus_Read(xpdd, XBT_NIL, SHUTDOWN_PATH, &value);
  if (res)
  {
    KdPrint(("Error reading shutdown path - %s\n", res));
    XenPci_FreeMem(res);
    return;
  }

  KdPrint((__DRIVER_NAME "     Shutdown value = %s\n", value));

  if (strlen(value) != 0)
  {
    if (strcmp(value, "suspend") == 0)
    {
      KdPrint((__DRIVER_NAME "     Suspend detected\n"));
      /* we have to queue this as a work item as we stop the xenbus thread, which we are currently running in! */
    	work_item = IoAllocateWorkItem(xpdd->common.fdo);
      IoQueueWorkItem(work_item, XenPci_BeginSuspend, DelayedWorkQueue, NULL);
      //XenPci_BeginSuspend(xpdd);
    }
    else
    {
      KeAcquireSpinLock(&xpdd->shutdown_ring_lock, &old_irql);
      if (xpdd->shutdown_start >= xpdd->shutdown_cons)
        xpdd->shutdown_prod = xpdd->shutdown_start;
      else
        xpdd->shutdown_start = xpdd->shutdown_prod;
      memcpy(&xpdd->shutdown_ring[xpdd->shutdown_prod], value, strlen(value));
      xpdd->shutdown_prod += (ULONG)strlen(value);
      xpdd->shutdown_ring[xpdd->shutdown_prod++] = '\r';
      xpdd->shutdown_ring[xpdd->shutdown_prod++] = '\n';
      KeReleaseSpinLock(&xpdd->shutdown_ring_lock, old_irql);
      XenPci_ProcessShutdownIrp(xpdd);
    }
  }

  //XenPci_FreeMem(value);

  FUNCTION_EXIT();
}

static VOID 
XenPci_DumpPdoConfigs(PXENPCI_DEVICE_DATA xpdd)
{
  PXEN_CHILD child;

  for (child = (PXEN_CHILD)xpdd->child_list.Flink; child != (PXEN_CHILD)&xpdd->child_list; child = (PXEN_CHILD)child->entry.Flink)
  {
    XenPci_DumpPdoConfig(child->context->common.pdo);
  }  
}

static VOID
XenPci_SysrqHandler(char *path, PVOID context)
{
  PXENPCI_DEVICE_DATA xpdd = context;
  char *value;
  char letter;
  char *res;

  UNREFERENCED_PARAMETER(path);

  FUNCTION_ENTER();

  XenBus_Read(xpdd, XBT_NIL, SYSRQ_PATH, &value);

  KdPrint((__DRIVER_NAME "     SysRq Value = %s\n", value));

  if (value != NULL && strlen(value) != 0)
  {
    letter = *value;
    res = XenBus_Write(xpdd, XBT_NIL, SYSRQ_PATH, "");
    if (res)
    {
      KdPrint(("Error writing sysrq path\n"));
      XenPci_FreeMem(res);
      return;
    }
  }
  else
  {
    letter = 0;
  }

  if (value != NULL)
  {
    XenPci_FreeMem(value);
  }

  switch (letter)
  {
  case 0:
    break;
  case 'B': /* cause a bug check */
    KeBugCheckEx(('X' << 16)|('E' << 8)|('N'), 0x00000001, 0x00000000, 0x00000000, 0x00000000);
    break;
  case 'X': /* stop delivering events */
  	xpdd->interrupts_masked = TRUE;
    break;    
  case 'C':
    /* show some debugging info */
  	XenPci_DumpPdoConfigs(xpdd);
    break;
  default:
    KdPrint(("     Unhandled sysrq letter %c\n", letter));
    break;
  }

  FUNCTION_EXIT();
}

static DDKAPI VOID
XenPci_Pnp_StartDeviceCallback(PDEVICE_OBJECT device_object, PVOID context)
{
  NTSTATUS status = STATUS_SUCCESS;
  PXENPCI_DEVICE_DATA xpdd = device_object->DeviceExtension;
  PIRP irp = context;
  char *response;

  KdPrint((__DRIVER_NAME " --> " __FUNCTION__ "\n"));
  
  XenPci_Init(xpdd);

  GntTbl_Init(xpdd);

  EvtChn_Init(xpdd);
  EvtChn_ConnectInterrupt(xpdd);
  XenBus_Init(xpdd);

  response = XenBus_AddWatch(xpdd, XBT_NIL, SYSRQ_PATH, XenPci_SysrqHandler, xpdd);
  KdPrint((__DRIVER_NAME "     sysrqwatch response = '%s'\n", response));
  
#if 0
  response = XenBus_AddWatch(xpdd, XBT_NIL, SHUTDOWN_PATH, XenPci_ShutdownHandler, xpdd);
  KdPrint((__DRIVER_NAME "     shutdown watch response = '%s'\n", response));
#endif

  response = XenBus_AddWatch(xpdd, XBT_NIL, "device", XenPci_DeviceWatchHandler, xpdd);
  KdPrint((__DRIVER_NAME "     device watch response = '%s'\n", response));

#if 0
  response = XenBus_AddWatch(xpdd, XBT_NIL, BALLOON_PATH, XenPci_BalloonHandler, Device);
  KdPrint((__DRIVER_NAME "     balloon watch response = '%s'\n", response));
#endif

  status = IoSetDeviceInterfaceState(&xpdd->legacy_interface_name, TRUE);
  if (!NT_SUCCESS(status))
  {
    KdPrint((__DRIVER_NAME "     IoSetDeviceInterfaceState (legacy) failed with status 0x%08x\n", status));
  }

  status = IoSetDeviceInterfaceState(&xpdd->interface_name, TRUE);
  if (!NT_SUCCESS(status))
  {
    KdPrint((__DRIVER_NAME "     IoSetDeviceInterfaceState failed with status 0x%08x\n", status));
  }

  irp->IoStatus.Status = status;
  
  IoCompleteRequest(irp, IO_NO_INCREMENT);

  KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ "\n"));
}

static NTSTATUS
XenPci_Pnp_StartDevice(PDEVICE_OBJECT device_object, PIRP irp)
{
  NTSTATUS status;
  PXENPCI_DEVICE_DATA xpdd = (PXENPCI_DEVICE_DATA)device_object->DeviceExtension;
  PIO_STACK_LOCATION stack;
  PCM_PARTIAL_RESOURCE_LIST res_list;
  PCM_PARTIAL_RESOURCE_DESCRIPTOR res_descriptor;
  ULONG i;

  UNREFERENCED_PARAMETER(device_object);

  FUNCTION_ENTER();

  stack = IoGetCurrentIrpStackLocation(irp);

  IoMarkIrpPending(irp);

  status = XenPci_SendAndWaitForIrp(device_object, irp);

  res_list = &stack->Parameters.StartDevice.AllocatedResources->List[0].PartialResourceList;
  
  for (i = 0; i < res_list->Count; i++)
  {
    res_descriptor = &res_list->PartialDescriptors[i];
    switch (res_descriptor->Type)
    {
    case CmResourceTypeInterrupt:
      KdPrint((__DRIVER_NAME "     irq_number = %03x\n", res_descriptor->u.Interrupt.Vector));
      xpdd->irq_number = res_descriptor->u.Interrupt.Vector;
      //memcpy(&InterruptRaw, res_descriptor, sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);
      break;
    }
  }

  res_list = &stack->Parameters.StartDevice.AllocatedResourcesTranslated->List[0].PartialResourceList;
  
  for (i = 0; i < res_list->Count; i++)
  {
    res_descriptor = &res_list->PartialDescriptors[i];
    switch (res_descriptor->Type) {
    case CmResourceTypePort:
      KdPrint((__DRIVER_NAME "     IoPort Address(%x) Length: %d\n", res_descriptor->u.Port.Start.LowPart, res_descriptor->u.Port.Length));
      xpdd->platform_ioport_addr = res_descriptor->u.Port.Start.LowPart;
      xpdd->platform_ioport_len = res_descriptor->u.Port.Length;
      break;
    case CmResourceTypeMemory:
      KdPrint((__DRIVER_NAME "     Memory mapped CSR:(%x:%x) Length:(%d)\n", res_descriptor->u.Memory.Start.LowPart, res_descriptor->u.Memory.Start.HighPart, res_descriptor->u.Memory.Length));
      KdPrint((__DRIVER_NAME "     Memory flags = %04X\n", res_descriptor->Flags));
      xpdd->platform_mmio_addr = res_descriptor->u.Memory.Start;
      xpdd->platform_mmio_len = res_descriptor->u.Memory.Length;
      xpdd->platform_mmio_alloc = 0;
      xpdd->platform_mmio_flags = res_descriptor->Flags;
      break;
    case CmResourceTypeInterrupt:
      KdPrint((__DRIVER_NAME "     irq_vector = %03x\n", res_descriptor->u.Interrupt.Vector));
      KdPrint((__DRIVER_NAME "     irq_level = %03x\n", res_descriptor->u.Interrupt.Level));
	    xpdd->irq_level = (KIRQL)res_descriptor->u.Interrupt.Level;
  	  xpdd->irq_vector = res_descriptor->u.Interrupt.Vector;
	    xpdd->irq_affinity = res_descriptor->u.Interrupt.Affinity;
      
      xpdd->irq_mode = (res_descriptor->Flags & CM_RESOURCE_INTERRUPT_LATCHED)?Latched:LevelSensitive;
      KdPrint((__DRIVER_NAME "     irq_mode = %s\n", (xpdd->irq_mode == Latched)?"Latched":"LevelSensitive"));
      switch(res_descriptor->ShareDisposition)
      {
      case CmResourceShareDeviceExclusive:
        KdPrint((__DRIVER_NAME "     ShareDisposition = CmResourceShareDeviceExclusive\n"));
        break;
      case CmResourceShareDriverExclusive:
        KdPrint((__DRIVER_NAME "     ShareDisposition = CmResourceShareDriverExclusive\n"));
        break;
      case CmResourceShareShared:
        KdPrint((__DRIVER_NAME "     ShareDisposition = CmResourceShareShared\n"));
        break;
      default:
        KdPrint((__DRIVER_NAME "     ShareDisposition = %d\n", res_descriptor->ShareDisposition));
        break;
      }
      break;
    case CmResourceTypeDevicePrivate:
      KdPrint((__DRIVER_NAME "     Private Data: 0x%02x 0x%02x 0x%02x\n", res_descriptor->u.DevicePrivate.Data[0], res_descriptor->u.DevicePrivate.Data[1], res_descriptor->u.DevicePrivate.Data[2]));
      break;
    default:
      KdPrint((__DRIVER_NAME "     Unhandled resource type (0x%x)\n", res_descriptor->Type));
      break;
    }
  }

  XenPci_QueueWorkItem(device_object, XenPci_Pnp_StartDeviceCallback, irp);

  FUNCTION_EXIT();
  
  return STATUS_PENDING;
}

static NTSTATUS
XenPci_Pnp_StopDevice(PDEVICE_OBJECT device_object, PIRP irp, PVOID context)
{
  NTSTATUS status = STATUS_SUCCESS;

  UNREFERENCED_PARAMETER(device_object);
  UNREFERENCED_PARAMETER(context);

  FUNCTION_ENTER();

  irp->IoStatus.Status = status;
  IoCompleteRequest(irp, IO_NO_INCREMENT);

  FUNCTION_EXIT();

  return irp->IoStatus.Status;
}

static NTSTATUS
XenPci_Pnp_QueryStopRemoveDevice(PDEVICE_OBJECT device_object, PIRP irp)
{
  NTSTATUS status;
  PXENPCI_DEVICE_DATA xpdd = (PXENPCI_DEVICE_DATA)device_object->DeviceExtension;

  UNREFERENCED_PARAMETER(device_object);

  FUNCTION_ENTER();

  if (xpdd->common.device_usage_paging
    || xpdd->common.device_usage_dump
    || xpdd->common.device_usage_hibernation)
  {
    /* We are in the paging or hibernation path - can't remove */
    status = irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
  }
  else
  {
    IoSkipCurrentIrpStackLocation(irp);
    status = IoCallDriver(xpdd->common.lower_do, irp);
  }
  
  FUNCTION_EXIT();

  return status;
}

static NTSTATUS
XenPci_Pnp_RemoveDevice(PDEVICE_OBJECT device_object, PIRP irp)
{
  NTSTATUS status;
  PXENPCI_DEVICE_DATA xpdd = (PXENPCI_DEVICE_DATA)device_object->DeviceExtension;

  UNREFERENCED_PARAMETER(device_object);

  FUNCTION_ENTER();

  irp->IoStatus.Status = STATUS_SUCCESS;
  IoSkipCurrentIrpStackLocation(irp);
  status = IoCallDriver(xpdd->common.lower_do, irp);
  IoDetachDevice(xpdd->common.lower_do);

  FUNCTION_EXIT();

  return status;
}

static DDKAPI VOID
XenPci_Pnp_QueryBusRelationsCallback(PDEVICE_OBJECT device_object, PVOID context)
{
  NTSTATUS status = STATUS_SUCCESS;
  PXENPCI_DEVICE_DATA xpdd = (PXENPCI_DEVICE_DATA)device_object->DeviceExtension;
  PXENPCI_PDO_DEVICE_DATA xppdd;
  PIRP irp = context;
  int device_count = 0;
  PDEVICE_RELATIONS dev_relations;
  PXEN_CHILD child, old_child;
  //char *response;
  char *msg;
  char **devices;
  char **instances;
  int i, j;
  CHAR path[128];
  PDEVICE_OBJECT pdo;
  PDEVICE_RELATIONS oldRelations;
  int prevcount, length;

  FUNCTION_ENTER();

  msg = XenBus_List(xpdd, XBT_NIL, "device", &devices);
  if (!msg)
  {
    for (child = (PXEN_CHILD)xpdd->child_list.Flink; child != (PXEN_CHILD)&xpdd->child_list; child = (PXEN_CHILD)child->entry.Flink)
    {
      if (child->state == CHILD_STATE_DELETED)
      {
        KdPrint((__DRIVER_NAME "     Found deleted child - this shouldn't happen\n" ));
      }
      child->state = CHILD_STATE_DELETED;
    }

    for (i = 0; devices[i]; i++)
    {
      RtlStringCbPrintfA(path, ARRAY_SIZE(path), "device/%s", devices[i]);
      msg = XenBus_List(xpdd, XBT_NIL, path, &instances);
      if (!msg)
      {
        for (j = 0; instances[j]; j++)
        {
          RtlStringCbPrintfA(path, ARRAY_SIZE(path), "device/%s/%s", devices[i], instances[j]);

          for (child = (PXEN_CHILD)xpdd->child_list.Flink; child != (PXEN_CHILD)&xpdd->child_list; child = (PXEN_CHILD)child->entry.Flink)
          {
            if (strcmp(child->context->path, path) == 0)
            {
              KdPrint((__DRIVER_NAME "     Existing device %s\n", path));
              ASSERT(child->state == CHILD_STATE_DELETED);
              child->state = CHILD_STATE_ADDED;
              device_count++;
              break;
            }
          }
        
          if (child == (PXEN_CHILD)&xpdd->child_list)
          {
            KdPrint((__DRIVER_NAME "     New device %s\n", path));
            child = ExAllocatePoolWithTag(NonPagedPool, sizeof(XEN_CHILD), XENPCI_POOL_TAG);
            child->state = CHILD_STATE_ADDED;
            status = IoCreateDevice(
              xpdd->common.fdo->DriverObject,
              sizeof(XENPCI_PDO_DEVICE_DATA),
              NULL,
              FILE_DEVICE_UNKNOWN,
              FILE_AUTOGENERATED_DEVICE_NAME | FILE_DEVICE_SECURE_OPEN,
              FALSE,
              &pdo);
            if (!NT_SUCCESS(status))
            {
              KdPrint((__DRIVER_NAME "     IoCreateDevice status = %08X\n", status));
            }
            RtlZeroMemory(pdo->DeviceExtension, sizeof(XENPCI_PDO_DEVICE_DATA));
            child->context = xppdd = pdo->DeviceExtension;
            xppdd->common.fdo = NULL;
            xppdd->common.pdo = pdo;
            ObReferenceObject(pdo);
            xppdd->common.lower_do = NULL;
            INIT_PNP_STATE(&xppdd->common);
            xppdd->common.device_usage_paging = 0;
            xppdd->common.device_usage_dump = 0;
            xppdd->common.device_usage_hibernation = 0;
            xppdd->bus_fdo = xpdd->common.fdo;
            xppdd->bus_pdo = xpdd->common.pdo;
            RtlStringCbCopyA(xppdd->path, ARRAY_SIZE(xppdd->path), path);
            RtlStringCbCopyA(xppdd->device, ARRAY_SIZE(xppdd->device), devices[i]);
            xppdd->index = atoi(instances[j]);
            KeInitializeEvent(&xppdd->backend_state_event, SynchronizationEvent, FALSE);
            xppdd->backend_state = XenbusStateUnknown;
            xppdd->backend_path[0] = '\0';
            InsertTailList(&xpdd->child_list, (PLIST_ENTRY)child);
            device_count++;
          }
          XenPci_FreeMem(instances[j]);
        }
        XenPci_FreeMem(instances);
      }
      XenPci_FreeMem(devices[i]);
    }
    XenPci_FreeMem(devices);

    //
    // Keep track of old relations structure
    //
    oldRelations = (PDEVICE_RELATIONS) irp->IoStatus.Information;
    if (oldRelations)
    {
      prevcount = oldRelations->Count;
    }
    else
    {
      prevcount = 0;
    }

    //
    // Need to allocate a new relations structure and add our
    // PDOs to it.
    //

    length = sizeof(DEVICE_RELATIONS) + ((device_count + prevcount) * sizeof (PDEVICE_OBJECT)) -1;

    dev_relations = (PDEVICE_RELATIONS) ExAllocatePoolWithTag (PagedPool, length, XENPCI_POOL_TAG);
    if (!dev_relations)
    {
      KdPrint((__DRIVER_NAME "**** Failed to allocate a new buffer for query device relations\n"));
      //
      // Fail the IRP
      //
      irp->IoStatus.Status = status = STATUS_INSUFFICIENT_RESOURCES;
      IoCompleteRequest (irp, IO_NO_INCREMENT);
      return;
    }

    //
    // Copy in the device objects so far
    //
    if (prevcount)
    {
      RtlCopyMemory (dev_relations->Objects, oldRelations->Objects, prevcount * sizeof (PDEVICE_OBJECT));
    }

    for (child = (PXEN_CHILD)xpdd->child_list.Flink, device_count = 0; child != (PXEN_CHILD)&xpdd->child_list; child = (PXEN_CHILD)child->entry.Flink)
    {
      if (child->state == CHILD_STATE_ADDED)
      {
        ObReferenceObject(child->context->common.pdo);
        dev_relations->Objects[prevcount++] = child->context->common.pdo;
      }
    }

    dev_relations->Count = prevcount + device_count;

    child = (PXEN_CHILD)xpdd->child_list.Flink;
    while (child != (PXEN_CHILD)&xpdd->child_list)
    {
      if (child->state == CHILD_STATE_DELETED)
      {
        KdPrint((__DRIVER_NAME "     Removing deleted child from device list\n"));
        old_child = child;
        child = (PXEN_CHILD)child->entry.Flink;
        RemoveEntryList((PLIST_ENTRY)old_child);
        xppdd = old_child->context;
        xppdd->reported_missing = TRUE;
        ObDereferenceObject(xppdd->common.pdo);
        ExFreePoolWithTag(old_child, XENPCI_POOL_TAG);
      }
      else
      {
        child = (PXEN_CHILD)child->entry.Flink;
      }
    }
    
    status = STATUS_SUCCESS;
  }
  else
  {
    //
    // Fail the IRP
    //
    irp->IoStatus.Status = status = STATUS_INSUFFICIENT_RESOURCES;
    IoCompleteRequest (irp, IO_NO_INCREMENT);
    return;
  }

  irp->IoStatus.Status = status;
  //
  // Replace the relations structure in the IRP with the new
  // one.
  //
  if (oldRelations)
  {
      ExFreePool (oldRelations);
  }
  irp->IoStatus.Information = (ULONG_PTR)dev_relations;

  IoCompleteRequest (irp, IO_NO_INCREMENT);

  FUNCTION_EXIT();
}

static NTSTATUS
XenPci_Pnp_QueryBusRelations(PDEVICE_OBJECT device_object, PIRP irp)
{
  NTSTATUS status;

  UNREFERENCED_PARAMETER(device_object);

  FUNCTION_ENTER();

  IoMarkIrpPending(irp);

  status = XenPci_SendAndWaitForIrp(device_object, irp);

  XenPci_QueueWorkItem(device_object, XenPci_Pnp_QueryBusRelationsCallback, irp);

  FUNCTION_EXIT();

  return STATUS_PENDING;
}

static DDKAPI VOID
XenPci_Pnp_FilterResourceRequirementsCallback(PDEVICE_OBJECT device_object, PVOID context)
{
  NTSTATUS status = STATUS_SUCCESS;
  //PXENPCI_DEVICE_DATA xpdd = (PXENPCI_DEVICE_DATA)device_object->DeviceExtension;
  PIRP irp = context;

  UNREFERENCED_PARAMETER(device_object);

  FUNCTION_ENTER();
  irp->IoStatus.Status = status;
  IoCompleteRequest (irp, IO_NO_INCREMENT);
  
  FUNCTION_EXIT();
}

static NTSTATUS
XenPci_Pnp_FilterResourceRequirements(PDEVICE_OBJECT device_object, PIRP irp)
{
  NTSTATUS status;

  UNREFERENCED_PARAMETER(device_object);

  FUNCTION_ENTER();

  IoMarkIrpPending(irp);

  status = XenPci_SendAndWaitForIrp(device_object, irp);

  XenPci_QueueWorkItem(device_object, XenPci_Pnp_FilterResourceRequirementsCallback, irp);

  FUNCTION_EXIT();

  return STATUS_PENDING;
}

static NTSTATUS
XenPci_Pnp_DeviceUsageNotification(PDEVICE_OBJECT device_object, PIRP irp, PVOID context)
{
  NTSTATUS status;
  PXENPCI_DEVICE_DATA xpdd;
  PIO_STACK_LOCATION stack;
  
  UNREFERENCED_PARAMETER(context);

  FUNCTION_ENTER();

  xpdd = (PXENPCI_DEVICE_DATA)device_object->DeviceExtension;
  stack = IoGetCurrentIrpStackLocation(irp);
  status = irp->IoStatus.Status;

  /* fail if we are in a stop or remove pending state */  
  if (!NT_SUCCESS(irp->IoStatus.Status))
  {
    switch (stack->Parameters.UsageNotification.Type)
    {
    case DeviceUsageTypePaging:
      if (stack->Parameters.UsageNotification.InPath)
        xpdd->common.device_usage_paging--;
      else
        xpdd->common.device_usage_paging++;      
      break;
    case DeviceUsageTypeDumpFile:
      if (stack->Parameters.UsageNotification.InPath)
        xpdd->common.device_usage_dump--;
      else
        xpdd->common.device_usage_dump++;      
      break;
    case DeviceUsageTypeHibernation:
      if (stack->Parameters.UsageNotification.InPath)
        xpdd->common.device_usage_hibernation--;
      else
        xpdd->common.device_usage_hibernation++;      
      break;
    default:
      KdPrint((__DRIVER_NAME " Unknown usage type %x\n",
        stack->Parameters.UsageNotification.Type));
      break;
    }
    if (xpdd->common.device_usage_paging
      || xpdd->common.device_usage_dump
      || xpdd->common.device_usage_hibernation)
    {
      xpdd->common.fdo->Flags &= ~DO_POWER_PAGABLE;
    }
    IoInvalidateDeviceState(xpdd->common.pdo);
  }
  IoCompleteRequest(irp, IO_NO_INCREMENT);

  FUNCTION_EXIT();
  
  return status;
}


NTSTATUS
XenPci_Pnp_Fdo(PDEVICE_OBJECT device_object, PIRP irp)
{
  NTSTATUS status;
  PIO_STACK_LOCATION stack;
  PXENPCI_DEVICE_DATA xpdd = device_object->DeviceExtension;;

  KdPrint((__DRIVER_NAME " --> " __FUNCTION__ "\n"));

  stack = IoGetCurrentIrpStackLocation(irp);

  switch (stack->MinorFunction)
  {
  case IRP_MN_START_DEVICE:
    KdPrint((__DRIVER_NAME "     IRP_MN_START_DEVICE\n"));
    return XenPci_Pnp_StartDevice(device_object, irp);

  case IRP_MN_QUERY_STOP_DEVICE:
    KdPrint((__DRIVER_NAME "     IRP_MN_QUERY_STOP_DEVICE\n"));
    status = XenPci_Pnp_QueryStopRemoveDevice(device_object, irp);
    if (NT_SUCCESS(status))
      SET_PNP_STATE(&xpdd->common, RemovePending);
    return status;

  case IRP_MN_STOP_DEVICE:
    KdPrint((__DRIVER_NAME "     IRP_MN_STOP_DEVICE\n"));
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, XenPci_Pnp_StopDevice, NULL, TRUE, TRUE, TRUE);
    break;

  case IRP_MN_CANCEL_STOP_DEVICE:
    KdPrint((__DRIVER_NAME "     IRP_MN_CANCEL_STOP_DEVICE\n"));
    IoSkipCurrentIrpStackLocation(irp);
    REVERT_PNP_STATE(&xpdd->common);
    irp->IoStatus.Status = STATUS_SUCCESS;
    break;

  case IRP_MN_QUERY_REMOVE_DEVICE:
    KdPrint((__DRIVER_NAME "     IRP_MN_QUERY_REMOVE_DEVICE\n"));
    status = XenPci_Pnp_QueryStopRemoveDevice(device_object, irp);
    if (NT_SUCCESS(status))
      SET_PNP_STATE(&xpdd->common, RemovePending);
    return status;

  case IRP_MN_REMOVE_DEVICE:
    KdPrint((__DRIVER_NAME "     IRP_MN_REMOVE_DEVICE\n"));
    return XenPci_Pnp_RemoveDevice(device_object, irp);
    break;

  case IRP_MN_CANCEL_REMOVE_DEVICE:
    KdPrint((__DRIVER_NAME "     IRP_MN_CANCEL_REMOVE_DEVICE\n"));
    IoSkipCurrentIrpStackLocation(irp);
    REVERT_PNP_STATE(&xpdd->common);
    irp->IoStatus.Status = STATUS_SUCCESS;
    break;

  case IRP_MN_SURPRISE_REMOVAL:
    KdPrint((__DRIVER_NAME "     IRP_MN_SURPRISE_REMOVAL\n"));
    IoSkipCurrentIrpStackLocation(irp);
    irp->IoStatus.Status = STATUS_SUCCESS;
    break;

  case IRP_MN_DEVICE_USAGE_NOTIFICATION:
    KdPrint((__DRIVER_NAME "     IRP_MN_DEVICE_USAGE_NOTIFICATION\n"));
    switch (stack->Parameters.UsageNotification.Type)
    {
    case DeviceUsageTypePaging:
      KdPrint((__DRIVER_NAME "     type = DeviceUsageTypePaging = %d\n", stack->Parameters.UsageNotification.InPath));
      if (stack->Parameters.UsageNotification.InPath)
        xpdd->common.device_usage_paging++;
      else
        xpdd->common.device_usage_paging--;      
      irp->IoStatus.Status = STATUS_SUCCESS;
      break;
    case DeviceUsageTypeDumpFile:
      KdPrint((__DRIVER_NAME "     type = DeviceUsageTypeDumpFile = %d\n", stack->Parameters.UsageNotification.InPath));
      if (stack->Parameters.UsageNotification.InPath)
        xpdd->common.device_usage_dump++;
      else
        xpdd->common.device_usage_dump--;      
      irp->IoStatus.Status = STATUS_SUCCESS;
      break;
    case DeviceUsageTypeHibernation:
      KdPrint((__DRIVER_NAME "     type = DeviceUsageTypeHibernation = %d\n", stack->Parameters.UsageNotification.InPath));
      if (stack->Parameters.UsageNotification.InPath)
        xpdd->common.device_usage_hibernation++;
      else
        xpdd->common.device_usage_hibernation--;      
      irp->IoStatus.Status = STATUS_SUCCESS;
      break;
    default:
      KdPrint((__DRIVER_NAME "     type = unsupported (%d)\n", stack->Parameters.UsageNotification.Type));
      irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
      IoCompleteRequest(irp, IO_NO_INCREMENT);
      return STATUS_NOT_SUPPORTED;
    }
    if (!xpdd->common.device_usage_paging
      && !xpdd->common.device_usage_dump
      && !xpdd->common.device_usage_hibernation)
    {
      xpdd->common.fdo->Flags |= DO_POWER_PAGABLE;
    }
    IoInvalidateDeviceState(xpdd->common.pdo);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, XenPci_Pnp_DeviceUsageNotification, NULL, TRUE, TRUE, TRUE);
    break;

  case IRP_MN_QUERY_DEVICE_RELATIONS:
    KdPrint((__DRIVER_NAME "     IRP_MN_QUERY_DEVICE_RELATIONS\n"));
    switch (stack->Parameters.QueryDeviceRelations.Type)
    {
    case BusRelations:
      KdPrint((__DRIVER_NAME "     BusRelations\n"));
      return XenPci_Pnp_QueryBusRelations(device_object, irp);
      break;  
    default:
      IoSkipCurrentIrpStackLocation(irp);
      break;  
    }
    break;

  case IRP_MN_FILTER_RESOURCE_REQUIREMENTS:
    KdPrint((__DRIVER_NAME "     IRP_MN_FILTER_RESOURCE_REQUIREMENTS\n"));
    return XenPci_Pnp_FilterResourceRequirements(device_object, irp);

  case IRP_MN_QUERY_PNP_DEVICE_STATE:
    KdPrint((__DRIVER_NAME "     IRP_MN_QUERY_PNP_DEVICE_STATE\n"));
    irp->IoStatus.Status = STATUS_SUCCESS;
    if (xpdd->common.device_usage_paging
      || xpdd->common.device_usage_dump
      || xpdd->common.device_usage_hibernation)
    {
      irp->IoStatus.Information |= PNP_DEVICE_NOT_DISABLEABLE;
    }
    IoSkipCurrentIrpStackLocation(irp);
    break;
    
  default:
    KdPrint((__DRIVER_NAME "     Unhandled Minor = %d\n", stack->MinorFunction));
    IoSkipCurrentIrpStackLocation(irp);
    break;
  }

  status = IoCallDriver(xpdd->common.lower_do, irp);

  KdPrint((__DRIVER_NAME " <-- " __FUNCTION__"\n"));

  return status;
}

NTSTATUS
XenPci_Irp_Create_Fdo(PDEVICE_OBJECT device_object, PIRP irp)
{
  PXENPCI_DEVICE_DATA xpdd;
  NTSTATUS status;
  PIO_STACK_LOCATION stack;
  PFILE_OBJECT file;

  FUNCTION_ENTER();
  
  stack = IoGetCurrentIrpStackLocation(irp);
  file = stack->FileObject;
  
  KdPrint((__DRIVER_NAME "     filename = %wZ\n", &file->FileName));
  if (wcscmp(L"\\xenbus", file->FileName.Buffer) == 0)
  {
    status = XenPci_Irp_Create_XenBus(device_object, irp);
  }
  else
  {
    // legacy interface
    xpdd = (PXENPCI_DEVICE_DATA)device_object->DeviceExtension;
    file->FsContext = ExAllocatePoolWithTag(NonPagedPool, sizeof(ULONG), XENPCI_POOL_TAG);
    *(PULONG)file->FsContext = DEVICE_INTERFACE_TYPE_LEGACY;
    status = STATUS_SUCCESS;    
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
  }
  KdPrint((__DRIVER_NAME "     context = %p\n", file->FsContext));
  KdPrint((__DRIVER_NAME "     type = %d\n", *(PULONG)file->FsContext));
  FUNCTION_EXIT();

  return status;
}

NTSTATUS
XenPci_Irp_Close_Fdo(PDEVICE_OBJECT device_object, PIRP irp)
{
  PXENPCI_DEVICE_DATA xpdd;
  NTSTATUS status;
  PIO_STACK_LOCATION stack;
  PFILE_OBJECT file;

  FUNCTION_ENTER();
  
  stack = IoGetCurrentIrpStackLocation(irp);
  file = stack->FileObject;

  if (*(PULONG)file->FsContext == DEVICE_INTERFACE_TYPE_XENBUS)
  {
    status = XenPci_Irp_Close_XenBus(device_object, irp);
  }
  else
  {
    // wait until pending irp's 
    xpdd = (PXENPCI_DEVICE_DATA)device_object->DeviceExtension;
    status = STATUS_SUCCESS;    
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
  }
  
  FUNCTION_EXIT();

  return status;
}

NTSTATUS
XenPci_Irp_Read_Fdo(PDEVICE_OBJECT device_object, PIRP irp)
{
  PXENPCI_DEVICE_DATA xpdd;
  NTSTATUS status;
  PIO_STACK_LOCATION stack;
  KIRQL old_irql;
  PFILE_OBJECT file;

  FUNCTION_ENTER();
  
  stack = IoGetCurrentIrpStackLocation(irp);
  file = stack->FileObject;

  if (*(PULONG)file->FsContext == DEVICE_INTERFACE_TYPE_XENBUS)
  {
    status = XenPci_Irp_Read_XenBus(device_object, irp);
  }
  else
  {
    xpdd = (PXENPCI_DEVICE_DATA)device_object->DeviceExtension;
    stack = IoGetCurrentIrpStackLocation(irp);
    if (stack->Parameters.Read.Length == 0)
    {
      irp->IoStatus.Information = 0;
      status = STATUS_SUCCESS;    
      irp->IoStatus.Status = status;
      IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
    else 
    {
      KdPrint((__DRIVER_NAME "     stack = %p\n", stack));
      KdPrint((__DRIVER_NAME "     length = %d, buffer = %p\n", stack->Parameters.Read.Length, irp->AssociatedIrp.SystemBuffer));
      
      KeAcquireSpinLock(&xpdd->shutdown_ring_lock, &old_irql);
      xpdd->shutdown_irp = irp;
      IoSetCancelRoutine(irp, XenBus_ShutdownIoCancel);
      KeReleaseSpinLock(&xpdd->shutdown_ring_lock, old_irql);
      status = XenPci_ProcessShutdownIrp(xpdd);
    }
  }
  FUNCTION_EXIT();

  return status;
}

NTSTATUS
XenPci_Irp_Write_Fdo(PDEVICE_OBJECT device_object, PIRP irp)
{
  PXENPCI_DEVICE_DATA xpdd;
  NTSTATUS status;
  PIO_STACK_LOCATION stack;
  PFILE_OBJECT file;

  FUNCTION_ENTER();
  
  stack = IoGetCurrentIrpStackLocation(irp);
  file = stack->FileObject;
  
  KdPrint((__DRIVER_NAME "     context = %p\n", file->FsContext));
  KdPrint((__DRIVER_NAME "     type = %d\n", *(PULONG)file->FsContext));

  xpdd = (PXENPCI_DEVICE_DATA)device_object->DeviceExtension;
  stack = IoGetCurrentIrpStackLocation(irp);

  if (*(PULONG)file->FsContext == DEVICE_INTERFACE_TYPE_XENBUS)
  {
    status = XenPci_Irp_Write_XenBus(device_object, irp);
  }
  else
  {
    status = STATUS_UNSUCCESSFUL;
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
  }    

  FUNCTION_EXIT();

  return status;
}

NTSTATUS
XenPci_Irp_Cleanup_Fdo(PDEVICE_OBJECT device_object, PIRP irp)
{
  NTSTATUS status;
  PIO_STACK_LOCATION stack;
  PFILE_OBJECT file;

  UNREFERENCED_PARAMETER(device_object);

  FUNCTION_ENTER();
  
  stack = IoGetCurrentIrpStackLocation(irp);
  file = stack->FileObject;
  
  if (*(PULONG)file->FsContext == DEVICE_INTERFACE_TYPE_XENBUS)
  {
    status = XenPci_Irp_Cleanup_XenBus(device_object, irp);
  }
  else
  {
    status = STATUS_SUCCESS;
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
  }
  FUNCTION_EXIT();

  return status;
}

DDKAPI NTSTATUS
XenPci_SystemControl_Fdo(PDEVICE_OBJECT device_object, PIRP irp)
{
  NTSTATUS status;
  PIO_STACK_LOCATION stack;
  PXENPCI_COMMON common = device_object->DeviceExtension;
  
  FUNCTION_ENTER();

  UNREFERENCED_PARAMETER(device_object);

  stack = IoGetCurrentIrpStackLocation(irp);
  DbgPrint(__DRIVER_NAME "     Minor = %d\n", stack->MinorFunction);
  IoSkipCurrentIrpStackLocation(irp);
  status = IoCallDriver(common->lower_do, irp);

  FUNCTION_EXIT();
  
  return status;
}

#endif

#if 0
static VOID
XenPci_BalloonHandler(char *Path, PVOID Data)
{
  WDFDEVICE Device = Data;
  char *value;
  xenbus_transaction_t xbt;
  int retry;

  UNREFERENCED_PARAMETER(Path);

  KdPrint((__DRIVER_NAME " --> XenBus_BalloonHandler\n"));

  XenBus_StartTransaction(Device, &xbt);

  XenBus_Read(Device, XBT_NIL, BALLOON_PATH, &value);

  KdPrint((__DRIVER_NAME "     Balloon Value = %s\n", value));

  // use the memory_op(unsigned int op, void *arg) hypercall to adjust this
  // use XENMEM_increase_reservation and XENMEM_decrease_reservation

  XenBus_EndTransaction(Device, xbt, 0, &retry);

  XenPci_FreeMem(value);

  KdPrint((__DRIVER_NAME " <-- XenBus_BalloonHandler\n"));
}
#endif
