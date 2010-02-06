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

VOID
GntTbl_PutRef(PVOID Context, grant_ref_t ref)
{
  PXENPCI_DEVICE_DATA xpdd = Context;

  stack_push(xpdd->gnttab_ss, (PVOID)ref);
}

grant_ref_t
GntTbl_GetRef(PVOID Context)
{
  PXENPCI_DEVICE_DATA xpdd = Context;
  unsigned int ref;
  PVOID ptr_ref;

  if (!stack_pop(xpdd->gnttab_ss, &ptr_ref))
  {
    KdPrint((__DRIVER_NAME "     No free grant refs\n"));
    return INVALID_GRANT_REF;
  }
  ref = (grant_ref_t)(ULONG_PTR)ptr_ref;

  return ref;
}

int 
GntTbl_Map(PVOID Context, unsigned int start_idx, unsigned int end_idx)
{
  PXENPCI_DEVICE_DATA xpdd = Context;
  struct xen_add_to_physmap xatp;
  unsigned int i = end_idx;

  /* Loop backwards, so that the first hypercall has the largest index,  ensuring that the table will grow only once.  */
  do {
    xatp.domid = DOMID_SELF;
    xatp.idx = i;
    xatp.space = XENMAPSPACE_grant_table;
    xatp.gpfn = (xen_pfn_t)(xpdd->gnttab_table_physical.QuadPart >> PAGE_SHIFT) + i;
    if (HYPERVISOR_memory_op(xpdd, XENMEM_add_to_physmap, &xatp))
    {
      KdPrint((__DRIVER_NAME "     ***ERROR MAPPING FRAME***\n"));
    }
  } while (i-- > start_idx);

  return 0;
}

grant_ref_t
GntTbl_GrantAccess(
  PVOID Context,
  domid_t domid,
  uint32_t frame, // xen api limits pfn to 32bit, so no guests over 8TB
  int readonly,
  grant_ref_t ref)
{
  PXENPCI_DEVICE_DATA xpdd = Context;

  //KdPrint((__DRIVER_NAME " --> GntTbl_GrantAccess\n"));

  //KdPrint((__DRIVER_NAME "     Granting access to frame %08x\n", frame));

  if (ref == INVALID_GRANT_REF)
    ref = GntTbl_GetRef(Context);
  if (ref == INVALID_GRANT_REF)
    return ref;
  
  xpdd->gnttab_table[ref].frame = frame;
  xpdd->gnttab_table[ref].domid = domid;

  if (xpdd->gnttab_table[ref].flags)
    KdPrint((__DRIVER_NAME "     WARNING: Attempting to re-use grant entry that is already in use!\n"));
  ASSERT(!xpdd->gnttab_table[ref].flags);

  KeMemoryBarrier();
  readonly *= GTF_readonly;
  xpdd->gnttab_table[ref].flags = GTF_permit_access | (uint16_t)readonly;

  //KdPrint((__DRIVER_NAME " <-- GntTbl_GrantAccess (ref = %d)\n", ref));

  return ref;
}

BOOLEAN
GntTbl_EndAccess(
  PVOID Context,
  grant_ref_t ref,
  BOOLEAN keepref)
{
  PXENPCI_DEVICE_DATA xpdd = Context;
  unsigned short flags, nflags;

  //KdPrint((__DRIVER_NAME " --> GntTbl_EndAccess\n"));

  ASSERT(ref != INVALID_GRANT_REF);
  
  nflags = xpdd->gnttab_table[ref].flags;
  do {
    if ((flags = nflags) & (GTF_reading|GTF_writing))
    {
      KdPrint((__DRIVER_NAME "     WARNING: g.e. %d still in use!\n", ref));
      return FALSE;
    }
  } while ((nflags = InterlockedCompareExchange16(
    (volatile SHORT *)&xpdd->gnttab_table[ref].flags, 0, flags)) != flags);

  if (!keepref)
    GntTbl_PutRef(Context, ref);
  //KdPrint((__DRIVER_NAME " <-- GntTbl_EndAccess\n"));
  return TRUE;
}

static unsigned int 
GntTbl_QueryMaxFrames(PXENPCI_DEVICE_DATA xpdd)
{
  struct gnttab_query_size query;
  int rc;

  query.dom = DOMID_SELF;

  rc = HYPERVISOR_grant_table_op(xpdd, GNTTABOP_query_size, &query, 1);
  if ((rc < 0) || (query.status != GNTST_okay))
  {
    KdPrint((__DRIVER_NAME "     ***CANNOT QUERY MAX GRANT FRAME***\n"));
    return 4; /* Legacy max supported number of frames */
  }
  return query.max_nr_frames;
}

VOID
GntTbl_Init(PXENPCI_DEVICE_DATA xpdd)
{
  int i;
  int grant_entries;
  
  FUNCTION_ENTER();
  
  xpdd->grant_frames = GntTbl_QueryMaxFrames(xpdd);
  KdPrint((__DRIVER_NAME "     grant_frames = %d\n", xpdd->grant_frames));
  grant_entries = min(NR_GRANT_ENTRIES, (xpdd->grant_frames * PAGE_SIZE / sizeof(grant_entry_t)));
  KdPrint((__DRIVER_NAME "     grant_entries = %d\n", grant_entries));
  
  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
  xpdd->gnttab_table_copy = ExAllocatePoolWithTag(NonPagedPool, xpdd->grant_frames * PAGE_SIZE, XENPCI_POOL_TAG);
  ASSERT(xpdd->gnttab_table_copy); // lazy
  xpdd->gnttab_table_physical = XenPci_AllocMMIO(xpdd, PAGE_SIZE * xpdd->grant_frames);
  xpdd->gnttab_table = MmMapIoSpace(xpdd->gnttab_table_physical, PAGE_SIZE * xpdd->grant_frames, MmNonCached);
  if (!xpdd->gnttab_table)
  {
    KdPrint((__DRIVER_NAME "     Error Mapping Grant Table Shared Memory\n"));
    // this should be a show stopper...
    return;
  }

  stack_new(&xpdd->gnttab_ss, grant_entries);
  
  for (i = NR_RESERVED_ENTRIES; i < grant_entries; i++)
    stack_push(xpdd->gnttab_ss, (PVOID)i);
  
  GntTbl_Map(xpdd, 0, xpdd->grant_frames - 1);

  RtlZeroMemory(xpdd->gnttab_table, PAGE_SIZE * xpdd->grant_frames);
  
  FUNCTION_EXIT();
}

VOID
GntTbl_Suspend(PXENPCI_DEVICE_DATA xpdd)
{
  memcpy(xpdd->gnttab_table_copy, xpdd->gnttab_table, xpdd->grant_frames * PAGE_SIZE);
}

VOID
GntTbl_Resume(PXENPCI_DEVICE_DATA xpdd)
{
  ULONG new_grant_frames;
  ULONG result;
  
  FUNCTION_ENTER();
  
  new_grant_frames = GntTbl_QueryMaxFrames(xpdd);
  KdPrint((__DRIVER_NAME "     new_grant_frames = %d\n", new_grant_frames));
  ASSERT(new_grant_frames >= xpdd->grant_frames); // lazy
  result = GntTbl_Map(xpdd, 0, xpdd->grant_frames - 1);
  KdPrint((__DRIVER_NAME "     GntTbl_Map result = %d\n", result));
  memcpy(xpdd->gnttab_table, xpdd->gnttab_table_copy, xpdd->grant_frames * PAGE_SIZE);
  
  FUNCTION_EXIT();
}
