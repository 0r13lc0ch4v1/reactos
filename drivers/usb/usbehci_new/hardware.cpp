/*
 * PROJECT:     ReactOS Universal Serial Bus Bulk Enhanced Host Controller Interface
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        drivers/usb/usbehci/hcd_controller.cpp
 * PURPOSE:     USB EHCI device driver.
 * PROGRAMMERS:
 *              Michael Martin (michael.martin@reactos.org)
 *              Johannes Anderwald (johannes.anderwald@reactos.org)
 */

#define INITGUID
#include "usbehci.h"
#include "hardware.h"

BOOLEAN
NTAPI
InterruptServiceRoutine(
    IN PKINTERRUPT  Interrupt,
    IN PVOID  ServiceContext);

class CUSBHardwareDevice : public IUSBHardwareDevice
{
public:
    STDMETHODIMP QueryInterface( REFIID InterfaceId, PVOID* Interface);

    STDMETHODIMP_(ULONG) AddRef()
    {
        InterlockedIncrement(&m_Ref);
        return m_Ref;
    }
    STDMETHODIMP_(ULONG) Release()
    {
        InterlockedDecrement(&m_Ref);

        if (!m_Ref)
        {
            delete this;
            return 0;
        }
        return m_Ref;
    }
    // com
    NTSTATUS Initialize(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT FunctionalDeviceObject, PDEVICE_OBJECT PhysicalDeviceObject, PDEVICE_OBJECT LowerDeviceObject);
    NTSTATUS PnpStart(PCM_RESOURCE_LIST RawResources, PCM_RESOURCE_LIST TranslatedResources);
    NTSTATUS PnpStop(void);
    NTSTATUS HandlePower(PIRP Irp);
    NTSTATUS GetDeviceDetails(PULONG VendorId, PULONG DeviceId, PULONG NumberOfPorts, PULONG Speed);
    NTSTATUS GetDmaMemoryManager(OUT struct IDMAMemoryManager **OutMemoryManager);
    NTSTATUS GetUSBQueue(OUT struct IUSBQueue **OutUsbQueue);
    NTSTATUS StartController();
    NTSTATUS StopController();
    NTSTATUS ResetController();
    NTSTATUS ResetPort(ULONG PortIndex);
    KIRQL AcquireDeviceLock(void);
    VOID ReleaseDeviceLock(KIRQL OldLevel);
    // local
    BOOLEAN InterruptService();

    // friend function
    friend BOOLEAN NTAPI InterruptServiceRoutine(IN PKINTERRUPT  Interrupt, IN PVOID  ServiceContext);

    // constructor / destructor
    CUSBHardwareDevice(IUnknown *OuterUnknown){}
    virtual ~CUSBHardwareDevice(){}

protected:
    LONG m_Ref;
    PDRIVER_OBJECT m_DriverObject;
    PDEVICE_OBJECT m_PhysicalDeviceObject;
    PDEVICE_OBJECT m_FunctionalDeviceObject;
    PDEVICE_OBJECT m_NextDeviceObject;
    KSPIN_LOCK m_Lock;
    PKINTERRUPT m_Interrupt;
    PULONG m_Base;
    PDMA_ADAPTER m_Adapter;
    ULONG m_MapRegisters;
    PQUEUE_HEAD AsyncListQueueHead;
    EHCI_CAPS m_Capabilities;

    VOID SetCommandRegister(PEHCI_USBCMD_CONTENT UsbCmd);
    VOID GetCommandRegister(PEHCI_USBCMD_CONTENT UsbCmd);
    VOID SetStatusRegister(PEHCI_USBSTS_CONTENT UsbSts);
    VOID GetStatusRegister(PEHCI_USBSTS_CONTENT UsbSts);
    //VOID SetPortRegister(PEHCI_USBPORTSC_CONTENT UsbPort);
    //VOID GetPortRegister(PEHCI_USBPORTSC_CONTENT UsbPort);
    ULONG EHCI_READ_REGISTER_ULONG(ULONG Offset);
    ULONG EHCI_READ_REGISTER_USHORT(ULONG Offset);
    ULONG EHCI_READ_REGISTER_UCHAR(ULONG Offset);
    VOID EHCI_WRITE_REGISTER_ULONG(ULONG Offset, ULONG Value);
    VOID EHCI_WRITE_REGISTER_USHORT(ULONG Offset, ULONG Value);
    VOID EHCI_WRITE_REGISTER_UCHAR(ULONG Offset, ULONG Value);
};

//=================================================================================================
// COM
//
NTSTATUS
STDMETHODCALLTYPE
CUSBHardwareDevice::QueryInterface(
    IN  REFIID refiid,
    OUT PVOID* Output)
{
    if (IsEqualGUIDAligned(refiid, IID_IUnknown))
    {
        *Output = PVOID(PUNKNOWN(this));
        PUNKNOWN(*Output)->AddRef();
        return STATUS_SUCCESS;
    }

    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
CUSBHardwareDevice::Initialize(
    PDRIVER_OBJECT DriverObject,
    PDEVICE_OBJECT FunctionalDeviceObject,
    PDEVICE_OBJECT PhysicalDeviceObject,
    PDEVICE_OBJECT LowerDeviceObject)
{

    DPRINT1("CUSBHardwareDevice::Initialize\n");

    //
    // store device objects
    // 
    m_DriverObject = DriverObject;
    m_FunctionalDeviceObject = FunctionalDeviceObject;
    m_PhysicalDeviceObject = PhysicalDeviceObject;
    m_NextDeviceObject = LowerDeviceObject;

    //
    // initialize device lock
    //
    KeInitializeSpinLock(&m_Lock);

    return STATUS_SUCCESS;
}

VOID
CUSBHardwareDevice::SetCommandRegister(PEHCI_USBCMD_CONTENT UsbCmd)
{
    PULONG Register;
    Register = (PULONG)UsbCmd;
    WRITE_REGISTER_ULONG((PULONG)((ULONG)m_Base + EHCI_USBCMD), *Register);
}

VOID
CUSBHardwareDevice::GetCommandRegister(PEHCI_USBCMD_CONTENT UsbCmd)
{
    PULONG Register;
    Register = (PULONG)UsbCmd;
    *Register = READ_REGISTER_ULONG((PULONG)((ULONG)m_Base + EHCI_USBCMD));
}


VOID
CUSBHardwareDevice::SetStatusRegister(PEHCI_USBSTS_CONTENT UsbSts)
{
    PULONG Register;
    Register = (PULONG)UsbSts;
    WRITE_REGISTER_ULONG((PULONG)((ULONG)m_Base + EHCI_USBSTS), *Register);
}

VOID
CUSBHardwareDevice::GetStatusRegister(PEHCI_USBSTS_CONTENT UsbSts)
{
    PULONG CmdRegister;
    CmdRegister = (PULONG)UsbSts;
    *CmdRegister = READ_REGISTER_ULONG((PULONG)((ULONG)m_Base + EHCI_USBSTS));
}

ULONG
CUSBHardwareDevice::EHCI_READ_REGISTER_ULONG(ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)((ULONG)m_Base + Offset));
}

ULONG
CUSBHardwareDevice::EHCI_READ_REGISTER_USHORT(ULONG Offset)
{
    return READ_REGISTER_USHORT((PUSHORT)((ULONG)m_Base + Offset));
}

ULONG
CUSBHardwareDevice::EHCI_READ_REGISTER_UCHAR(ULONG Offset)
{
    return READ_REGISTER_UCHAR((PUCHAR)((ULONG)m_Base + Offset));
}

VOID
CUSBHardwareDevice::EHCI_WRITE_REGISTER_ULONG(ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)((ULONG)m_Base + Offset), Value);
}

VOID
CUSBHardwareDevice::EHCI_WRITE_REGISTER_USHORT(ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_USHORT((PUSHORT)((ULONG)m_Base + Offset), Value);
}

VOID
CUSBHardwareDevice::EHCI_WRITE_REGISTER_UCHAR(ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_UCHAR((PUCHAR)((ULONG)m_Base + Offset), Value);
}

NTSTATUS
CUSBHardwareDevice::PnpStart(
    PCM_RESOURCE_LIST RawResources,
    PCM_RESOURCE_LIST TranslatedResources)
{
    ULONG Index, Count;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR ResourceDescriptor;
    DEVICE_DESCRIPTION DeviceDescription;
    PVOID ResourceBase;
    NTSTATUS Status;

    DPRINT1("CUSBHardwareDevice::PnpStart\n");
    for(Index = 0; Index < TranslatedResources->List[0].PartialResourceList.Count; Index++)
    {
        //
        // get resource descriptor
        //
        ResourceDescriptor = &TranslatedResources->List[0].PartialResourceList.PartialDescriptors[Index];

        switch(ResourceDescriptor->Type)
        {
            case CmResourceTypeInterrupt:
            {
                Status = IoConnectInterrupt(&m_Interrupt,
                                            InterruptServiceRoutine,
                                            (PVOID)this,
                                            NULL,
                                            ResourceDescriptor->u.Interrupt.Vector,
                                            (KIRQL)ResourceDescriptor->u.Interrupt.Level,
                                            (KIRQL)ResourceDescriptor->u.Interrupt.Level,
                                            (KINTERRUPT_MODE)(ResourceDescriptor->Flags & CM_RESOURCE_INTERRUPT_LATCHED),
                                            (ResourceDescriptor->ShareDisposition != CmResourceShareDeviceExclusive),
                                            ResourceDescriptor->u.Interrupt.Affinity,
                                            FALSE);

                if (!NT_SUCCESS(Status))
                {
                    //
                    // failed to register interrupt
                    //
                    DPRINT1("IoConnect Interrupt failed with %x\n", Status);
                    return Status;
                }
                break;
            }
            case CmResourceTypeMemory:
            {
                //
                // get resource base
                //
                ResourceBase = MmMapIoSpace(ResourceDescriptor->u.Memory.Start, ResourceDescriptor->u.Memory.Length, MmNonCached);
                if (!ResourceBase)
                {
                    //
                    // failed to map registers
                    //
                    DPRINT1("MmMapIoSpace failed\n");
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                //
                // Get controllers capabilities 
                //
                m_Capabilities.Length = READ_REGISTER_UCHAR((PUCHAR)ResourceBase);
                m_Capabilities.HCIVersion = READ_REGISTER_USHORT((PUSHORT)((ULONG)ResourceBase + 2));
                m_Capabilities.HCSParamsLong = READ_REGISTER_ULONG((PULONG)((ULONG)ResourceBase + 4));
                m_Capabilities.HCCParamsLong = READ_REGISTER_ULONG((PULONG)((ULONG)ResourceBase + 8));

                DPRINT1("Controller has %d Ports\n", m_Capabilities.HCSParams.PortCount);
                if (m_Capabilities.HCSParams.PortRouteRules)
                {
                    for (Count = 0; Count < m_Capabilities.HCSParams.PortCount; Count++)
                    {
                        m_Capabilities.PortRoute[Count] = READ_REGISTER_UCHAR((PUCHAR)(ULONG)ResourceBase + 12 + Count);
                    }
                }

                //
                // Set m_Base to the address of Operational Register Space
                //
                m_Base = (PULONG)((ULONG)ResourceBase + m_Capabilities.Length);
                break;
            }
        }
    }


    //
    // zero device description
    //
    RtlZeroMemory(&DeviceDescription, sizeof(DEVICE_DESCRIPTION));

    //
    // initialize device description
    //
    DeviceDescription.Version = DEVICE_DESCRIPTION_VERSION;
    DeviceDescription.Master = TRUE;
    DeviceDescription.ScatterGather = TRUE;
    DeviceDescription.Dma32BitAddresses = TRUE;
    DeviceDescription.DmaWidth = Width32Bits;
    DeviceDescription.InterfaceType = PCIBus;
    DeviceDescription.MaximumLength = MAXULONG;

    //
    // get dma adapter
    //
    m_Adapter = IoGetDmaAdapter(m_PhysicalDeviceObject, &DeviceDescription, &m_MapRegisters);
    if (!m_Adapter)
    {
        //
        // failed to get dma adapter
        //
        DPRINT1("Failed to acquire dma adapter\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // FIXME: Create a QueueHead that will always be the address of the AsyncList
    //
    AsyncListQueueHead = NULL;

    //
    // Start the controller
    //
    DPRINT1("Starting Controller\n");
    return StartController();
}

NTSTATUS
CUSBHardwareDevice::PnpStop(void)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
CUSBHardwareDevice::HandlePower(
    PIRP Irp)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
CUSBHardwareDevice::GetDeviceDetails(
    OUT OPTIONAL PULONG VendorId,
    OUT OPTIONAL PULONG DeviceId,
    OUT OPTIONAL PULONG NumberOfPorts,
    OUT OPTIONAL PULONG Speed)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}


NTSTATUS
CUSBHardwareDevice::GetDmaMemoryManager(
    OUT struct IDMAMemoryManager **OutMemoryManager)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}


NTSTATUS
CUSBHardwareDevice::GetUSBQueue(
    OUT struct IUSBQueue **OutUsbQueue)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
CUSBHardwareDevice::StartController(void)
{
    EHCI_USBCMD_CONTENT UsbCmd;
    EHCI_USBSTS_CONTENT UsbSts;
    LONG FailSafe;

    //
    // Stop the controller if its running
    //
    GetStatusRegister(&UsbSts);
    if (UsbSts.HCHalted)
        StopController();

    //
    // Reset the device. Bit is set to 0 on completion.
    //
    SetCommandRegister(&UsbCmd);
    UsbCmd.HCReset = TRUE;
    SetCommandRegister(&UsbCmd);

    //
    // Check that the controller reset
    //
    for (FailSafe = 100; FailSafe > 1; FailSafe--)
    {
        KeStallExecutionProcessor(10);
        GetCommandRegister(&UsbCmd);
        if (!UsbCmd.HCReset)
        {
            break;
        }
    }

    //
    // If the controller did not reset then fail
    //
    if (UsbCmd.HCReset)
    {
        DPRINT1("EHCI ERROR: Controller failed to reset. Hardware problem!\n");
        return STATUS_UNSUCCESSFUL;
    }

    //
    // Disable Interrupts and clear status
    //
    EHCI_WRITE_REGISTER_ULONG(EHCI_USBINTR, 0);
    EHCI_WRITE_REGISTER_ULONG(EHCI_USBSTS, 0x0000001f);

    //
    // FIXME: Assign the AsyncList Register
    //
    
    //
    // Set Schedules to Enable and Interrupt Threshold to 1ms.
    //
    GetCommandRegister(&UsbCmd);
    UsbCmd.PeriodicEnable = FALSE;
    UsbCmd.AsyncEnable = FALSE;  //FIXME: Need USB Memory Manager

    UsbCmd.IntThreshold = 1;
    // FIXME: Set framlistsize when periodic is implemented.
    SetCommandRegister(&UsbCmd);
    
    //
    // Enable Interrupts and start execution
    //
    EHCI_WRITE_REGISTER_ULONG(EHCI_USBINTR, EHCI_USBINTR_INTE | EHCI_USBINTR_ERR | EHCI_USBINTR_ASYNC | EHCI_USBINTR_HSERR
        /*| EHCI_USBINTR_FLROVR*/  | EHCI_USBINTR_PC);

    UsbCmd.Run = TRUE;
    SetCommandRegister(&UsbCmd);
    
    //
    // Wait for execution to start
    //
    for (FailSafe = 100; FailSafe > 1; FailSafe--)
    {
        KeStallExecutionProcessor(10);
        GetStatusRegister(&UsbSts);
        
        if (!UsbSts.HCHalted)
        {
            break;
        }
    }

    if (!UsbSts.HCHalted)
    {
        DPRINT1("Could not start execution on the controller\n");
        return STATUS_UNSUCCESSFUL;
    }

    //
    // Set port routing to EHCI controller
    //
    EHCI_WRITE_REGISTER_ULONG(EHCI_CONFIGFLAG, 1);
    DPRINT1("EHCI Started!\n");
    return STATUS_SUCCESS;
}

NTSTATUS
CUSBHardwareDevice::StopController(void)
{
    EHCI_USBCMD_CONTENT UsbCmd;
    EHCI_USBSTS_CONTENT UsbSts;
    LONG FailSafe;

    //
    // Disable Interrupts and stop execution
    EHCI_WRITE_REGISTER_ULONG (EHCI_USBINTR, 0);
    
    GetCommandRegister(&UsbCmd);
    UsbCmd.Run = FALSE;
    SetCommandRegister(&UsbCmd);

    for (FailSafe = 100; FailSafe > 1; FailSafe--)
    {
        KeStallExecutionProcessor(10);
        GetStatusRegister(&UsbSts);
        if (UsbSts.HCHalted)
        {
            break;
        }
    }

    if (!UsbSts.HCHalted)
    {
        DPRINT1("EHCI ERROR: Controller is not responding to Stop request!\n");
        return STATUS_UNSUCCESSFUL;
    }
    
    return STATUS_SUCCESS;
}

NTSTATUS
CUSBHardwareDevice::ResetController(void)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
CUSBHardwareDevice::ResetPort(
    IN ULONG PortIndex)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

KIRQL
CUSBHardwareDevice::AcquireDeviceLock(void)
{
    KIRQL OldLevel;

    //
    // acquire lock
    //
    KeAcquireSpinLock(&m_Lock, &OldLevel);

    //
    // return old irql
    //
    return OldLevel;
}


VOID
CUSBHardwareDevice::ReleaseDeviceLock(
    KIRQL OldLevel)
{
    KeReleaseSpinLock(&m_Lock, OldLevel);
}

BOOLEAN
NTAPI
InterruptServiceRoutine(
    IN PKINTERRUPT  Interrupt,
    IN PVOID  ServiceContext)
{
    UNIMPLEMENTED
    return FALSE;
}

NTSTATUS
CreateUSBHardware(
    PUSBHARDWAREDEVICE *OutHardware)
{
    PUSBHARDWAREDEVICE This;

    This = new(NonPagedPool, TAG_USBEHCI) CUSBHardwareDevice(0);

    if (!This)
        return STATUS_INSUFFICIENT_RESOURCES;

    This->AddRef();

    // return result
    *OutHardware = (PUSBHARDWAREDEVICE)This;

    return STATUS_SUCCESS;
}
