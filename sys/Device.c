/*
MIT License

Copyright (c) 2016 Benjamin "Nefarius" H�glinger

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/


#include "driver.h"
#include "device.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, HidGuardianCreateDevice)
#pragma alloc_text (PAGE, HidGuardianEvtDeviceContextCleanup)
#pragma alloc_text (PAGE, EvtFileCleanup)
#endif


NTSTATUS
HidGuardianCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    WDF_OBJECT_ATTRIBUTES           attribs;
    PDEVICE_CONTEXT                 pDeviceCtx;
    WDFDEVICE                       device;
    NTSTATUS                        status;
    WDF_FILEOBJECT_CONFIG           deviceConfig;
    WDFMEMORY                       memory;
    WDFMEMORY                       classNameMemory;
    PCWSTR                          className;
    WDF_PNPPOWER_EVENT_CALLBACKS    pnpPowerCallbacks;


    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry");

    //
    // We are a filter driver
    // 
    WdfFdoInitSetFilter(DeviceInit);

    //
    // Prepare registration of EvtFileCleanup
    // 
    WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
    attribs.SynchronizationScope = WdfSynchronizationScopeNone;
    WDF_FILEOBJECT_CONFIG_INIT(&deviceConfig,
        WDF_NO_EVENT_CALLBACK,
        WDF_NO_EVENT_CALLBACK,
        EvtFileCleanup
    );

    //
    // Register EvtFileCleanup
    // 
    WdfDeviceInitSetFileObjectConfig(
        DeviceInit,
        &deviceConfig,
        &attribs
    );

    //
    // Register Power/PNP callbacks
    // 
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDeviceReleaseHardware = EvtWdfDeviceReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(
        DeviceInit,
        &pnpPowerCallbacks
    );

    //
    // Initialize device context
    // 
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attribs, DEVICE_CONTEXT);

    //
    // We will just register for cleanup notification because we have to
    // delete the control-device when the last instance of the device goes
    // away. If we don't delete, the driver wouldn't get unloaded automatically
    // by the PNP subsystem.
    //
    attribs.EvtCleanupCallback = HidGuardianEvtDeviceContextCleanup;

    status = WdfDeviceCreate(&DeviceInit, &attribs, &device);

    if (NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_VERBOSE,
            TRACE_DEVICE,
            "Current device handle: %p", device);

        //
        // Get a pointer to the device context structure that we just associated
        // with the device object. We define this structure in the device.h
        // header file. DeviceGetContext is an inline function generated by
        // using the WDF_DECLARE_CONTEXT_TYPE_WITH_NAME macro in device.h.
        // This function will do the type checking and return the device context.
        // If you pass a wrong object handle it will return NULL and assert if
        // run under framework verifier mode.
        //
        pDeviceCtx = DeviceGetContext(device);

        //
        // Linked list for sticky PIDs
        // 
        pDeviceCtx->StickyPidList = PID_LIST_CREATE();
        if (pDeviceCtx->StickyPidList == NULL) {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_DEVICE,
                "PID_LIST_CREATE failed");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        //
        // Always allow SYSTEM PID 4
        // 
        PID_LIST_PUSH(&pDeviceCtx->StickyPidList, SYSTEM_PID, TRUE);

        WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
        attribs.ParentObject = device;

        //
        // Query for current device's Hardware ID
        // 
        status = WdfDeviceAllocAndQueryProperty(device,
            DevicePropertyHardwareID,
            NonPagedPool,
            &attribs,
            &memory
        );

        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_DEVICE,
                "WdfDeviceAllocAndQueryProperty failed with status %!STATUS!", status);
            return status;
        }

        //
        // Get Hardware ID string
        // 
        pDeviceCtx->HardwareIDsMemory = memory;
        pDeviceCtx->HardwareIDs = WdfMemoryGetBuffer(memory, &pDeviceCtx->HardwareIDsLength);

        WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
        attribs.ParentObject = device;

        //
        // Query for current device's ClassName
        // 
        status = WdfDeviceAllocAndQueryProperty(device,
            DevicePropertyClassName,
            NonPagedPool,
            &attribs,
            &classNameMemory
        );

        if (NT_SUCCESS(status)) {
            className = WdfMemoryGetBuffer(classNameMemory, NULL);

            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_DEVICE,
                "Current device class: %ls", className);
        }

        //
        // Initialize the I/O Package and any Queues
        //
        status = HidGuardianQueueInitialize(device);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_DEVICE,
                "HidGuardianQueueInitialize failed with status %!STATUS!", status);
            return status;
        }

        //
        // Create PendingAuthQueue I/O Queue
        // 
        status = PendingAuthQueueInitialize(device);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_DEVICE,
                "PendingAuthQueueInitialize failed with %!STATUS!", status);
            return status;
        }

        //
        // Create PendingAuthQueue I/O Queue
        //  
        status = PendingCreateRequestsQueueInitialize(device);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_DEVICE,
                "PendingCreateRequestsQueueInitialize failed with %!STATUS!", status);
            return status;
        }

        //
        // Create CreateRequestsQueue I/O Queue
        // 
        status = CreateRequestsQueueInitialize(device);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_DEVICE,
                "CreateRequestsQueueInitialize failed with %!STATUS!", status);
            return status;
        }

        //
        // Add this device to the FilterDevice collection.
        //
        WdfWaitLockAcquire(FilterDeviceCollectionLock, NULL);
        //
        // WdfCollectionAdd takes a reference on the item object and removes
        // it when you call WdfCollectionRemove.
        //
        status = WdfCollectionAdd(FilterDeviceCollection, device);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_DEVICE,
                "WdfCollectionAdd failed with status %!STATUS!", status);
        }
        WdfWaitLockRelease(FilterDeviceCollectionLock);

        //
        // Create a control device
        //
        status = HidGuardianCreateControlDevice(device);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_DEVICE,
                "HidGuardianCreateControlDevice failed with status %!STATUS!", status);

            return status;
        }

        if (AmIMaster(pDeviceCtx))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_DEVICE,
                "I am the master, skipping further initialization");

            status = STATUS_SUCCESS;
            pDeviceCtx->AllowByDefault = FALSE;

            goto creationDone;
        }

        //
        // Expose FDO interface GUID
        // 
        status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_HIDGUARDIAN, NULL);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_DEVICE,
                "WdfDeviceCreateDeviceInterface failed status %!STATUS!", status);
            return status;
        }

        //
        // Fetch default action to take what to do if service isn't available
        // from registry key.
        // 
        GetDefaultAction(pDeviceCtx);

        //
        // Check if this device should get intercepted
        // 
        status = AmIAffected(pDeviceCtx);

        TraceEvents(TRACE_LEVEL_INFORMATION,
            TRACE_DEVICE,
            "AmIAffected status %!STATUS!", status);
    }

creationDone:

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit");

    return status;
}

//
// Gets called when the device gts removed.
// 
// Happens once in the devices lifetime.
// 
#pragma warning(push)
#pragma warning(disable:28118) // this callback will run at IRQL=PASSIVE_LEVEL
_Use_decl_annotations_
VOID
HidGuardianEvtDeviceContextCleanup(
    WDFOBJECT Device
)
{
    ULONG               count;
    PDEVICE_CONTEXT     pDeviceCtx;

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry (PID: %d)", CURRENT_PROCESS_ID());

    pDeviceCtx = DeviceGetContext(Device);

    PID_LIST_DESTROY(&pDeviceCtx->StickyPidList);

    WdfWaitLockAcquire(FilterDeviceCollectionLock, NULL);

    count = WdfCollectionGetCount(FilterDeviceCollection);

    if (count == 1)
    {
        //
        // We are the last instance. So let us delete the control-device
        // so that driver can unload when the FilterDevice is deleted.
        // We absolutely have to do the deletion of control device with
        // the collection lock acquired because we implicitly use this
        // lock to protect ControlDevice global variable. We need to make
        // sure another thread doesn't attempt to create while we are
        // deleting the device.
        //
        HidGuardianDeleteControlDevice((WDFDEVICE)Device);
    }

    WdfCollectionRemove(FilterDeviceCollection, Device);

    WdfWaitLockRelease(FilterDeviceCollectionLock);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit");
}
#pragma warning(pop) // enable 28118 again

//
// Gets called when a device handle gets closed.
// 
// May happen multiple times in the devices lifetime.
// 
_Use_decl_annotations_
VOID
EvtFileCleanup(
    WDFFILEOBJECT  FileObject
)
{
    WDFDEVICE                   device;
    PDEVICE_CONTEXT             pDeviceCtx;
    PCONTROL_DEVICE_CONTEXT     pControlCtx;
    ULONG                       pid;


    PAGED_CODE();

    device = WdfFileObjectGetDevice(FileObject);
    pDeviceCtx = DeviceGetContext(device);
    pControlCtx = ControlDeviceGetContext(ControlDevice);
    pid = CURRENT_PROCESS_ID();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry (PID: %d)", pid);

    if (pControlCtx->CerberusPid == pid) {
        TraceEvents(TRACE_LEVEL_INFORMATION,
            TRACE_DEVICE,
            "Cerberus has left the realm, performing clean-up");

        WdfIoQueuePurgeSynchronously(pDeviceCtx->PendingCreateRequestsQueue);
        WdfIoQueueStart(pDeviceCtx->PendingCreateRequestsQueue);

        WdfIoQueuePurgeSynchronously(pDeviceCtx->PendingAuthQueue);
        WdfIoQueueStart(pDeviceCtx->PendingAuthQueue);
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit");
}

//
// Gets called when the device gets powered down.
// 
_Use_decl_annotations_
NTSTATUS
EvtWdfDeviceReleaseHardware(
    WDFDEVICE  Device,
    WDFCMRESLIST  ResourcesTranslated
)
{
    PDEVICE_CONTEXT     pDeviceCtx;
    
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry");

    pDeviceCtx = DeviceGetContext(Device);

    pDeviceCtx->IsShuttingDown = TRUE;

    WdfIoQueuePurge(pDeviceCtx->CreateRequestsQueue, NULL, NULL);
    WdfIoQueuePurge(pDeviceCtx->PendingCreateRequestsQueue, NULL, NULL);
    WdfIoQueuePurge(pDeviceCtx->PendingAuthQueue, NULL, NULL);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit");

    return STATUS_SUCCESS;
}
