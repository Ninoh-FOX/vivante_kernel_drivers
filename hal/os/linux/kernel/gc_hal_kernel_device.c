/****************************************************************************
*
*    Copyright (c) 2005 - 2013 by Vivante Corp.  All rights reserved.
*
*    The material in this file is confidential and contains trade secrets
*    of Vivante Corporation. This is proprietary information owned by
*    Vivante Corporation. No part of this work may be disclosed,
*    reproduced, copied, transmitted, or used in any way for any purpose,
*    without the express written permission of Vivante Corporation.
*
*****************************************************************************/


#include "gc_hal_kernel_linux.h"
#include <linux/pagemap.h>
#include <linux/seq_file.h>
#include <linux/mman.h>
#include <linux/slab.h>

#define _GC_OBJ_ZONE    gcvZONE_DEVICE

#define DEBUG_FILE          "galcore_trace"
#define PARENT_FILE         "gpu"


#ifdef FLAREON
    static struct dove_gpio_irq_handler gc500_handle;
#endif

#define gcmIS_CORE_PRESENT(Device, Core) (Device->irqLines[Core] > 0)

/******************************************************************************\
*************************** Memory Allocation Wrappers *************************
\******************************************************************************/
static gceSTATUS
_FreeMemory(
    IN gckGALDEVICE Device,
    IN gctPOINTER Logical,
    IN gctPHYS_ADDR Physical)
{
    gceSTATUS status;

    gcmkHEADER_ARG("Device=0x%x Logical=0x%x Physical=0x%x",
                   Device, Logical, Physical);

    gcmkVERIFY_ARGUMENT(Device != NULL);

#if MRVL_USE_GPU_RESERVE_MEM
    status = gckOS_FreeVidmemFromMemblock(Device->os, Physical);
#else
    status = gckOS_FreeContiguous(
        Device->os, Physical, Logical,
        ((PLINUX_MDL) Physical)->numPages * PAGE_SIZE
        );
#endif

    gcmkFOOTER();
    return status;
}

#if MRVL_USE_GPU_RESERVE_MEM
static gceSTATUS
_AllocateContiguousMemory(
    IN gckGALDEVICE Device,
    IN gckKERNEL Kernel,
    IN gctUINT32 ContiguousBase,
    IN gctSIZE_T ContiguousSize,
    IN gctSIZE_T BankSize
    )
{
    gckGALDEVICE device = (gckGALDEVICE)Device;
    gckKERNEL kernel = (gckKERNEL)Kernel;
    gceSTATUS status;
    gctUINT32 physAddr;

    status = gckOS_AllocateVidmemFromMemblock(device->os,
                                              ContiguousSize,
                                              (gctPOINTER)ContiguousBase,
                                              &device->contiguousPhysical);

    if (gcmIS_SUCCESS(status))
    {
        device->contiguousPhysicalName = gcmPTR_TO_NAME(device->contiguousPhysical);
        physAddr = ((PLINUX_MDL)device->contiguousPhysical)->dmaHandle - device->baseAddress;

        status = gckVIDMEM_Construct(
            device->os,
            physAddr | device->systemMemoryBaseAddress,
            device->contiguousSize,
            64,
            BankSize,
            &device->contiguousVidMem
            );

        if (!gcmIS_SUCCESS(status))
        {
            gcmkVERIFY_OK(_FreeMemory(
                device,
                device->contiguousBase,
                device->contiguousPhysical
                ));

            gcmRELEASE_NAME(device->contiguousPhysicalName);
            device->contiguousBase     = gcvNULL;
            device->contiguousPhysical = gcvNULL;
        }
    }

    return status;
}
#else
static gceSTATUS
_AllocateContiguousMemory(
    IN gckGALDEVICE Device,
    IN gckKERNEL Kernel,
    IN gctUINT32 ContiguousBase,
    IN gctSIZE_T ContiguousSize,
    IN gctSIZE_T BankSize
    )
{
    gckGALDEVICE device = (gckGALDEVICE)Device;
    gckKERNEL kernel = (gckKERNEL)Kernel;
    gceSTATUS status;
    gctUINT32 physAddr;

    if (ContiguousBase == 0)
    {
        while (device->contiguousSize > 0)
        {
            /* Allocate contiguous memory. */
            status = gckOS_AllocateContiguous(device->os,
                    gcvFALSE,
                    &device->contiguousSize,
                    &device->contiguousPhysical,
                    &device->contiguousBase);

            if (gcmIS_SUCCESS(status))
            {
                device->contiguousPhysicalName = gcmPTR_TO_NAME(device->contiguousPhysical);
                physAddr = ((PLINUX_MDL)device->contiguousPhysical)->dmaHandle - device->baseAddress;

                status = gckVIDMEM_Construct(
                        device->os,
                        physAddr | device->systemMemoryBaseAddress,
                        device->contiguousSize,
                        64,
                        BankSize,
                        &device->contiguousVidMem
                        );

                if (gcmIS_SUCCESS(status))
                {
                    break;
                }

                gcmkONERROR(_FreeMemory(
                            device,
                            device->contiguousBase,
                            device->contiguousPhysical
                            ));

                gcmRELEASE_NAME(device->contiguousPhysicalName);
                device->contiguousBase     = gcvNULL;
                device->contiguousPhysical = gcvNULL;
            }

            if (device->contiguousSize <= (4 << 20))
            {
                device->contiguousSize = 0;
            }
            else
            {
                device->contiguousSize -= (4 << 20);
            }
        }
    }
    else
    {
        /* Create the contiguous memory heap. */
        status = gckVIDMEM_Construct(
                device->os,
                ContiguousBase | device->systemMemoryBaseAddress,
                ContiguousSize,
                64, BankSize,
                &device->contiguousVidMem
                );

        if (gcmIS_ERROR(status))
        {
            /* Error, disable contiguous memory pool. */
            device->contiguousVidMem = gcvNULL;
            device->contiguousSize   = 0;
        }
        else
        {
            struct resource* mem_region = request_mem_region(
                    ContiguousBase, ContiguousSize, "galcore managed memory"
                    );

            if (mem_region == gcvNULL)
            {
                gcmkTRACE_ZONE(
                        gcvLEVEL_ERROR, gcvZONE_DRIVER,
                        "%s(%d): Failed to claim %ld bytes @ 0x%08X\n",
                        __FUNCTION__, __LINE__,
                        ContiguousSize, ContiguousBase
                        );

                gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
            }

            device->requestedContiguousBase  = ContiguousBase;
            device->requestedContiguousSize  = ContiguousSize;

#if !gcdDYNAMIC_MAP_RESERVED_MEMORY && gcdENABLE_VG
            if (gcmIS_CORE_PRESENT(device, gcvCORE_VG))
            {
                device->contiguousBase
#if gcdPAGED_MEMORY_CACHEABLE
                    = (gctPOINTER) ioremap_cached(ContiguousBase, ContiguousSize);
#else
                = (gctPOINTER) ioremap_nocache(ContiguousBase, ContiguousSize);
#endif
                if (device->contiguousBase == gcvNULL)
                {
                    device->contiguousVidMem = gcvNULL;
                    device->contiguousSize = 0;

                    gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
                }
            }
#endif

            device->contiguousPhysical = gcvNULL;
            device->contiguousPhysicalName = 0;
            device->contiguousSize     = ContiguousSize;
            device->contiguousMapped   = gcvTRUE;
        }
    }

    return gcvSTATUS_OK;

OnError:
    return status;
}
#endif

/******************************************************************************\
******************************* Interrupt Handler ******************************
\******************************************************************************/
#if gcdMULTI_GPU
static irqreturn_t isrRoutine3D0(int irq, void *ctxt)
{
    gceSTATUS status;
    gckGALDEVICE device;

    device = (gckGALDEVICE) ctxt;

    /* Call kernel interrupt notification. */
    status = gckKERNEL_Notify(device->kernels[gcvCORE_MAJOR],
                              gcvCORE_3D_0_ID,
                              gcvNOTIFY_INTERRUPT,
                              gcvTRUE);

    if (gcmIS_SUCCESS(status))
    {
        /* Wake up the threadRoutine to process events. */
        device->dataReady3D[gcvCORE_3D_0_ID] = gcvTRUE;
        wake_up_interruptible(&device->intrWaitQueue3D[gcvCORE_3D_0_ID]);

        return IRQ_HANDLED;
    }

    return IRQ_NONE;
}

static int threadRoutine3D0(void *ctxt)
{
    gckGALDEVICE device = (gckGALDEVICE) ctxt;

    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_DRIVER,
                   "Starting isr Thread with extension=%p",
                   device);

    for (;;)
    {
        /* Sleep until being awaken by the interrupt handler. */
        wait_event_interruptible(device->intrWaitQueue3D[gcvCORE_3D_0_ID],
                                 device->dataReady3D[gcvCORE_3D_0_ID] == gcvTRUE);
        device->dataReady3D[gcvCORE_3D_0_ID] = gcvFALSE;

        if (device->killThread == gcvTRUE)
        {
            /* The daemon exits. */
            while (!kthread_should_stop())
            {
                gckOS_Delay(device->os, 1);
            }

            return 0;
        }

        gckKERNEL_Notify(device->kernels[gcvCORE_MAJOR],
                         gcvCORE_3D_0_ID,
                         gcvNOTIFY_INTERRUPT,
                         gcvFALSE);
    }
}

#if gcdMULTI_GPU > 1
static irqreturn_t isrRoutine3D1(int irq, void *ctxt)
{
    gceSTATUS status;
    gckGALDEVICE device;

    device = (gckGALDEVICE) ctxt;

    /* Call kernel interrupt notification. */
    status = gckKERNEL_Notify(device->kernels[gcvCORE_MAJOR],
                              gcvCORE_3D_1_ID,
                              gcvNOTIFY_INTERRUPT,
                              gcvTRUE);

    if (gcmIS_SUCCESS(status))
    {
        /* Wake up the worker thread to process events. */
        device->dataReady3D[gcvCORE_3D_1_ID] = gcvTRUE;
        wake_up_interruptible(&device->intrWaitQueue3D[gcvCORE_3D_1_ID]);

        return IRQ_HANDLED;
    }

    return IRQ_NONE;
}

static int threadRoutine3D1(void *ctxt)
{
    gckGALDEVICE device = (gckGALDEVICE) ctxt;

    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_DRIVER,
                   "Starting isr Thread with extension=%p",
                   device);

    for (;;)
    {
        /* Sleep until being awaken by the interrupt handler. */
        wait_event_interruptible(device->intrWaitQueue3D[gcvCORE_3D_1_ID],
                                 device->dataReady3D[gcvCORE_3D_1_ID] == gcvTRUE);
        device->dataReady3D[gcvCORE_3D_1_ID] = gcvFALSE;

        if (device->killThread == gcvTRUE)
        {
            /* The daemon exits. */
            while (!kthread_should_stop())
            {
                gckOS_Delay(device->os, 1);
            }

            return 0;
        }

        gckKERNEL_Notify(device->kernels[gcvCORE_MAJOR],
                         gcvCORE_3D_1_ID,
                         gcvNOTIFY_INTERRUPT,
                         gcvFALSE);
    }
}
#endif
#elif gcdMULTI_GPU_AFFINITY
static irqreturn_t isrRoutine3D0(int irq, void *ctxt)
{
    gceSTATUS status;
    gckGALDEVICE device;

    device = (gckGALDEVICE) ctxt;

    /* Call kernel interrupt notification. */
    status = gckKERNEL_Notify(device->kernels[gcvCORE_MAJOR], gcvNOTIFY_INTERRUPT, gcvTRUE);

    if (gcmIS_SUCCESS(status))
    {
        device->dataReadys[gcvCORE_MAJOR] = gcvTRUE;

        up(&device->semas[gcvCORE_MAJOR]);

        return IRQ_HANDLED;
    }

    return IRQ_NONE;
}

static int threadRoutine3D0(void *ctxt)
{
    gckGALDEVICE device = (gckGALDEVICE) ctxt;

    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_DRIVER,
                   "Starting isr Thread with extension=%p",
                   device);

    for (;;)
    {
        static int down;

        down = down_interruptible(&device->semas[gcvCORE_MAJOR]);
        if (down); /*To make gcc 4.6 happye*/
        device->dataReadys[gcvCORE_MAJOR] = gcvFALSE;

        if (device->killThread == gcvTRUE)
        {
            /* The daemon exits. */
            while (!kthread_should_stop())
            {
                gckOS_Delay(device->os, 1);
            }

            return 0;
        }

        gckKERNEL_Notify(device->kernels[gcvCORE_MAJOR],
                         gcvNOTIFY_INTERRUPT,
                         gcvFALSE);
    }
}

static irqreturn_t isrRoutine3D1(int irq, void *ctxt)
{
    gceSTATUS status;
    gckGALDEVICE device;

    device = (gckGALDEVICE) ctxt;

    /* Call kernel interrupt notification. */
    status = gckKERNEL_Notify(device->kernels[gcvCORE_OCL], gcvNOTIFY_INTERRUPT, gcvTRUE);

    if (gcmIS_SUCCESS(status))
    {
        device->dataReadys[gcvCORE_OCL] = gcvTRUE;

        up(&device->semas[gcvCORE_OCL]);

        return IRQ_HANDLED;
    }

    return IRQ_NONE;
}

static int threadRoutine3D1(void *ctxt)
{
    gckGALDEVICE device = (gckGALDEVICE) ctxt;

    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_DRIVER,
                   "Starting isr Thread with extension=%p",
                   device);

    for (;;)
    {
        static int down;

        down = down_interruptible(&device->semas[gcvCORE_OCL]);
        if (down); /*To make gcc 4.6 happye*/
        device->dataReadys[gcvCORE_OCL] = gcvFALSE;

        if (device->killThread == gcvTRUE)
        {
            /* The daemon exits. */
            while (!kthread_should_stop())
            {
                gckOS_Delay(device->os, 1);
            }

            return 0;
        }

        gckKERNEL_Notify(device->kernels[gcvCORE_OCL],
                         gcvNOTIFY_INTERRUPT,
                         gcvFALSE);
    }
}
#else
static irqreturn_t isrRoutine(int irq, void *ctxt)
{
    gceSTATUS status;
    gckGALDEVICE device;

    device = (gckGALDEVICE) ctxt;

    /* Call kernel interrupt notification. */
    status = gckKERNEL_Notify(device->kernels[gcvCORE_MAJOR], gcvNOTIFY_INTERRUPT, gcvTRUE);

    if (gcmIS_SUCCESS(status))
    {
        device->dataReadys[gcvCORE_MAJOR] = gcvTRUE;

        up(&device->semas[gcvCORE_MAJOR]);

        return IRQ_HANDLED;
    }

    return IRQ_NONE;
}

static int threadRoutine(void *ctxt)
{
    gckGALDEVICE device = (gckGALDEVICE) ctxt;

    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_DRIVER,
                   "Starting isr Thread with extension=%p",
                   device);

    for (;;)
    {
        static int down;

        down = down_interruptible(&device->semas[gcvCORE_MAJOR]);
        if (down); /*To make gcc 4.6 happye*/
        device->dataReadys[gcvCORE_MAJOR] = gcvFALSE;

        if (device->killThread == gcvTRUE)
        {
            /* The daemon exits. */
            while (!kthread_should_stop())
            {
                gckOS_Delay(device->os, 1);
            }

            return 0;
        }

        gckKERNEL_Notify(device->kernels[gcvCORE_MAJOR],
                         gcvNOTIFY_INTERRUPT,
                         gcvFALSE);
    }
}
#endif

static irqreturn_t isrRoutine2D(int irq, void *ctxt)
{
    gceSTATUS status;
    gckGALDEVICE device;

    device = (gckGALDEVICE) ctxt;

    /* Call kernel interrupt notification. */
    status = gckKERNEL_Notify(device->kernels[gcvCORE_2D],
#if gcdMULTI_GPU
                              0,
#endif
                              gcvNOTIFY_INTERRUPT,
                              gcvTRUE);
    if (gcmIS_SUCCESS(status))
    {
        device->dataReadys[gcvCORE_2D] = gcvTRUE;

        up(&device->semas[gcvCORE_2D]);

        return IRQ_HANDLED;
    }

    return IRQ_NONE;
}

static int threadRoutine2D(void *ctxt)
{
    gckGALDEVICE device = (gckGALDEVICE) ctxt;

    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_DRIVER,
                   "Starting isr Thread with extension=%p",
                   device);

    for (;;)
    {
        static int down;

        down = down_interruptible(&device->semas[gcvCORE_2D]);
        if (down); /*To make gcc 4.6 happye*/
        device->dataReadys[gcvCORE_2D] = gcvFALSE;

        if (device->killThread == gcvTRUE)
        {
            /* The daemon exits. */
            while (!kthread_should_stop())
            {
                gckOS_Delay(device->os, 1);
            }

            return 0;
        }
        gckKERNEL_Notify(device->kernels[gcvCORE_2D],
#if gcdMULTI_GPU
                         0,
#endif
                         gcvNOTIFY_INTERRUPT,
                         gcvFALSE);
    }
}

static irqreturn_t isrRoutineVG(int irq, void *ctxt)
{
#if gcdENABLE_VG
    gceSTATUS status;
    gckGALDEVICE device;

    device = (gckGALDEVICE) ctxt;

    /* Serve the interrupt. */
    status = gckVGINTERRUPT_Enque(device->kernels[gcvCORE_VG]->vg->interrupt);

    /* Determine the return value. */
    return (status == gcvSTATUS_NOT_OUR_INTERRUPT)
        ? IRQ_RETVAL(0)
        : IRQ_RETVAL(1);
#else
    return IRQ_NONE;
#endif
}

static int threadRoutineVG(void *ctxt)
{
    gckGALDEVICE device = (gckGALDEVICE) ctxt;

    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_DRIVER,
                   "Starting isr Thread with extension=%p",
                   device);

    for (;;)
    {
        static int down;

        down = down_interruptible(&device->semas[gcvCORE_VG]);
        if (down); /*To make gcc 4.6 happye*/
        device->dataReadys[gcvCORE_VG] = gcvFALSE;

        if (device->killThread == gcvTRUE)
        {
            /* The daemon exits. */
            while (!kthread_should_stop())
            {
                gckOS_Delay(device->os, 1);
            }

            return 0;
        }
        gckKERNEL_Notify(device->kernels[gcvCORE_VG],
#if gcdMULTI_GPU
                         0,
#endif
                         gcvNOTIFY_INTERRUPT,
                         gcvFALSE);
    }
}

#if MRVL_PROFILE_THREAD
int profile_thread(void *context)
{
    gceSTATUS    status    = gcvSTATUS_OK;
    gckGALDEVICE device    = (gckGALDEVICE) context;
    gckKERNEL    kernel    = device->kernels[gcvCORE_MAJOR];
#if MRVL_PLATFORM_MMP3
    gckKERNEL    kernel2D  = device->kernels[gcvCORE_2D];
    gctINT32     value2D = 0;
#endif
    gctUINT32    delayTime = 300; /* should be the max time of one draw */
    gctINT32     value = 0;

    while (1)
    {
//        delayTime = device->profileStep;
        gcmkONERROR(gckOS_AtomGet(device->os, kernel->atomClients, &value));
#if MRVL_PLATFORM_MMP3
        gcmkONERROR(gckOS_AtomGet(device->os, kernel2D->atomClients, &value2D));
#endif

        /* FIXME: enhanced this condition */
        if((device->os != gcvNULL)
            && (value > 0
#if MRVL_PLATFORM_MMP3
                || value2D > 0
#endif
               )
            && (device->currentPMode != gcvPM_NORMAL)
        )
        {
            /* Try acquiring power semaphore to see if GC is off/suspend/idle */
            status = gckOS_TryAcquireSemaphore(device->os, kernel->command->powerSemaphore);
            if (gcmIS_SUCCESS(status))
            {
                gcmkVERIFY_OK(gckOS_ReleaseSemaphore(device->os, kernel->command->powerSemaphore));
                goto OnSUCCESS;
            }
            else if (status == gcvSTATUS_TIMEOUT && kernel->hardware->chipPowerState != gcvPOWER_OFF)
            {
                /* 3D power semaphore is already acquired, but power
                     is not off, hardware is in suspend/idle status,
                     goto success to power off 3D.
                  */
                goto OnSUCCESS;
            }

            /* otherwise, time out to acquire 3D power semaphore, it shall mean 3D is powered off. */

#if MRVL_PLATFORM_MMP3
            status = gckOS_TryAcquireSemaphore(device->os, kernel2D->command->powerSemaphore);
            if (gcmIS_SUCCESS(status))
            {
                gcmkVERIFY_OK(gckOS_ReleaseSemaphore(device->os, kernel2D->command->powerSemaphore));
                goto OnSUCCESS;
            }
            else if (status == gcvSTATUS_TIMEOUT && kernel2D->hardware->chipPowerState != gcvPOWER_OFF)
            {
                /* 2D power semaphore is already acquired, but power
                     is not off, hardware is in suspend/idle status,
                     goto success to power off 2D.
                  */
                goto OnSUCCESS;
            }
#endif

            /* otherwise, time out to acquire 2D power semaphore, it shall mean 2D is powered off. */
            gcmkONERROR(gckOS_Delay(device->os, 1000));
            goto OnBailOut;

OnSUCCESS:
            /* power off GC when idle */
            if(device->powerOffWhenIdle)
            {
                gckOS_PowerOffWhenIdle(device->os, gcvTRUE);
            }

OnBailOut:
            gcmkONERROR(gckOS_Delay(device->os, delayTime));
        }
        else
        {
            gcmkONERROR(gckOS_Delay(device->os, 10000));
        }
    }

    return 0;

OnError:
    gcmkPRINT("%s(%d): thread has an exception!", __FUNCTION__, __LINE__);
    return status;
}
#endif

/******************************************************************************\
******************************* gckGALDEVICE Code ******************************
\******************************************************************************/

/*******************************************************************************
**
**  gckGALDEVICE_Construct
**
**  Constructor.
**
**  INPUT:
**
**  OUTPUT:
**
**      gckGALDEVICE * Device
**          Pointer to a variable receiving the gckGALDEVICE object pointer on
**          success.
*/
gceSTATUS
gckGALDEVICE_Construct(
#if gcdMULTI_GPU || gcdMULTI_GPU_AFFINITY
    IN gctINT IrqLine3D0,
    IN gctUINT32 RegisterMemBase3D0,
    IN gctSIZE_T RegisterMemSize3D0,
    IN gctINT IrqLine3D1,
    IN gctUINT32 RegisterMemBase3D1,
    IN gctSIZE_T RegisterMemSize3D1,
#else
    IN gctINT IrqLine,
    IN gctUINT32 RegisterMemBase,
    IN gctSIZE_T RegisterMemSize,
#endif
    IN gctINT IrqLine2D,
    IN gctUINT32 RegisterMemBase2D,
    IN gctSIZE_T RegisterMemSize2D,
    IN gctINT IrqLineVG,
    IN gctUINT32 RegisterMemBaseVG,
    IN gctSIZE_T RegisterMemSizeVG,
    IN gctUINT32 ContiguousBase,
    IN gctSIZE_T ContiguousSize,
    IN gctSIZE_T BankSize,
    IN gctINT FastClear,
    IN gctINT Compression,
    IN gctUINT32 PhysBaseAddr,
    IN gctUINT32 PhysSize,
    IN gctINT Signal,
    IN gctUINT LogFileSize,
    IN gctINT PowerManagement,
    IN gctINT GpuProfiler,
    IN gcsDEVICE_CONSTRUCT_ARGS * Args,
    OUT gckGALDEVICE *Device
    )
{
    gctUINT32 internalBaseAddress = 0, internalAlignment = 0;
    gctUINT32 externalBaseAddress = 0, externalAlignment = 0;
    gctUINT32 horizontalTileSize, verticalTileSize;
    struct resource* mem_region;
    gctUINT32 physical;
    gckGALDEVICE device;
    gceSTATUS status;
    gctINT32 i;
#if gcdMULTI_GPU
    gctINT32 j;
#endif
    gceHARDWARE_TYPE type;
    gckDB sharedDB = gcvNULL;
    gckKERNEL kernel = gcvNULL;

#if gcdMULTI_GPU || gcdMULTI_GPU_AFFINITY
    gcmkHEADER_ARG("IrqLine3D0=%d RegisterMemBase3D0=0x%08x RegisterMemSize3D0=%u "
                   "IrqLine2D=%d RegisterMemBase2D=0x%08x RegisterMemSize2D=%u "
                   "IrqLineVG=%d RegisterMemBaseVG=0x%08x RegisterMemSizeVG=%u "
                   "ContiguousBase=0x%08x ContiguousSize=%lu BankSize=%lu "
                   "FastClear=%d Compression=%d PhysBaseAddr=0x%x PhysSize=%d Signal=%d",
                   IrqLine3D0, RegisterMemBase3D0, RegisterMemSize3D0,
                   IrqLine2D, RegisterMemBase2D, RegisterMemSize2D,
                   IrqLineVG, RegisterMemBaseVG, RegisterMemSizeVG,
                   ContiguousBase, ContiguousSize, BankSize, FastClear, Compression,
                   PhysBaseAddr, PhysSize, Signal);
#else
    gcmkHEADER_ARG("IrqLine=%d RegisterMemBase=0x%08x RegisterMemSize=%u "
                   "IrqLine2D=%d RegisterMemBase2D=0x%08x RegisterMemSize2D=%u "
                   "IrqLineVG=%d RegisterMemBaseVG=0x%08x RegisterMemSizeVG=%u "
                   "ContiguousBase=0x%08x ContiguousSize=%lu BankSize=%lu "
                   "FastClear=%d Compression=%d PhysBaseAddr=0x%x PhysSize=%d Signal=%d",
                   IrqLine, RegisterMemBase, RegisterMemSize,
                   IrqLine2D, RegisterMemBase2D, RegisterMemSize2D,
                   IrqLineVG, RegisterMemBaseVG, RegisterMemSizeVG,
                   ContiguousBase, ContiguousSize, BankSize, FastClear, Compression,
                   PhysBaseAddr, PhysSize, Signal);
#endif

    /* Allocate device structure. */
    device = kmalloc(sizeof(struct _gckGALDEVICE), GFP_KERNEL | __GFP_NOWARN);

    if (!device)
    {
        gcmkONERROR(gcvSTATUS_OUT_OF_MEMORY);
    }

    memset(device, 0, sizeof(struct _gckGALDEVICE));

    device->dbgNode = gcvNULL;

    if (gckDEBUGFS_CreateNode(
            device, LogFileSize, PARENT_FILE,DEBUG_FILE, &(device->dbgNode)))
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): Failed to create  the debug file system  %s/%s \n",
            __FUNCTION__, __LINE__,
            PARENT_FILE, DEBUG_FILE
        );
    }
    else if (LogFileSize)
    {
        gckDEBUGFS_SetCurrentNode(device->dbgNode);
    }

#if gcdMULTI_GPU
    if (IrqLine3D0 != -1)
    {
        device->requestedRegisterMemBase3D[gcvCORE_3D_0_ID] = RegisterMemBase3D0;
        device->requestedRegisterMemSize3D[gcvCORE_3D_0_ID] = RegisterMemSize3D0;
    }
#if gcdMULTI_GPU > 1
    if (IrqLine3D1 != -1)
    {
        device->requestedRegisterMemBase3D[gcvCORE_3D_1_ID] = RegisterMemBase3D1;
        device->requestedRegisterMemSize3D[gcvCORE_3D_1_ID] = RegisterMemSize3D1;
    }
#endif
#elif gcdMULTI_GPU_AFFINITY
    if (IrqLine3D0 != -1)
    {
        device->requestedRegisterMemBases[gcvCORE_MAJOR] = RegisterMemBase3D0;
        device->requestedRegisterMemSizes[gcvCORE_MAJOR] = RegisterMemSize3D0;
    }

    if (IrqLine3D1 != -1)
    {
        device->requestedRegisterMemBases[gcvCORE_OCL] = RegisterMemBase3D1;
        device->requestedRegisterMemSizes[gcvCORE_OCL] = RegisterMemSize3D1;
    }
#else
    if (IrqLine != -1)
    {
        device->requestedRegisterMemBases[gcvCORE_MAJOR] = RegisterMemBase;
        device->requestedRegisterMemSizes[gcvCORE_MAJOR] = RegisterMemSize;
    }
#endif

    if (IrqLine2D != -1)
    {
        device->requestedRegisterMemBases[gcvCORE_2D] = RegisterMemBase2D;
        device->requestedRegisterMemSizes[gcvCORE_2D] = RegisterMemSize2D;
    }

    if (IrqLineVG != -1)
    {
        device->requestedRegisterMemBases[gcvCORE_VG] = RegisterMemBaseVG;
        device->requestedRegisterMemSizes[gcvCORE_VG] = RegisterMemSizeVG;
    }

    device->requestedContiguousBase  = 0;
    device->requestedContiguousSize  = 0;

    for (i = 0; i < gcdMAX_GPU_COUNT; i++)
    {
#if gcdMULTI_GPU
        if (i == gcvCORE_MAJOR)
        {
            for (j = 0; j < gcdMULTI_GPU; j++)
            {
                physical = device->requestedRegisterMemBase3D[j];

                /* Set up register memory region. */
                if (physical != 0)
                {
                    mem_region = request_mem_region(physical,
                            device->requestedRegisterMemSize3D[j],
                            "galcore register region");

                    if (mem_region == gcvNULL)
                    {
                        gcmkTRACE_ZONE(
                                gcvLEVEL_ERROR, gcvZONE_DRIVER,
                                "%s(%d): Failed to claim %lu bytes @ 0x%08X\n",
                                __FUNCTION__, __LINE__,
                                physical, device->requestedRegisterMemSize3D[j]
                        );

                        gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
                    }

                    device->registerBase3D[j] = (gctPOINTER) ioremap_nocache(
                            physical, device->requestedRegisterMemSize3D[j]);

                    if (device->registerBase3D[j] == gcvNULL)
                    {
                        gcmkTRACE_ZONE(
                                gcvLEVEL_ERROR, gcvZONE_DRIVER,
                                "%s(%d): Unable to map %ld bytes @ 0x%08X\n",
                                __FUNCTION__, __LINE__,
                                physical, device->requestedRegisterMemSize3D[j]
                        );

                        gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
                    }

                    physical += device->requestedRegisterMemSize3D[j];
                }
                else
                {
                    device->registerBase3D[j] = gcvNULL;
                }
            }
        }
        else
#endif
        {
            physical = device->requestedRegisterMemBases[i];

            /* Set up register memory region. */
            if (physical != 0)
            {
                mem_region = request_mem_region(physical,
                        device->requestedRegisterMemSizes[i],
                        "galcore register region");

                if (mem_region == gcvNULL)
                {
                    gcmkTRACE_ZONE(
                            gcvLEVEL_ERROR, gcvZONE_DRIVER,
                            "%s(%d): Failed to claim %lu bytes @ 0x%08X\n",
                            __FUNCTION__, __LINE__,
                            physical, device->requestedRegisterMemSizes[i]
                    );

                    gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
                }

                device->registerBases[i] = (gctPOINTER) ioremap_nocache(
                        physical, device->requestedRegisterMemSizes[i]);

                if (device->registerBases[i] == gcvNULL)
                {
                    gcmkTRACE_ZONE(
                            gcvLEVEL_ERROR, gcvZONE_DRIVER,
                            "%s(%d): Unable to map %ld bytes @ 0x%08X\n",
                            __FUNCTION__, __LINE__,
                            physical, device->requestedRegisterMemSizes[i]
                    );

                    gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
                }

                physical += device->requestedRegisterMemSizes[i];
            }
        }
    }

    /* Set the base address */
    device->baseAddress = PhysBaseAddr;

    /* Construct the gckOS object. */
    gcmkONERROR(gckOS_Construct(device, &device->os));

#if gcdMULTI_GPU || gcdMULTI_GPU_AFFINITY
    if (IrqLine3D0 != -1)
#else
    if (IrqLine != -1)
#endif
    {
        /* Construct the gckKERNEL object. */
        gcmkONERROR(gckKERNEL_Construct(
            device->os, gcvCORE_MAJOR, device,
            gcvNULL, &device->kernels[gcvCORE_MAJOR]));

        sharedDB = device->kernels[gcvCORE_MAJOR]->db;

        /* Initialize core mapping */
        for (i = 0; i < 8; i++)
        {
            device->coreMapping[i] = gcvCORE_MAJOR;
        }

        /* Setup the ISR manager. */
        gcmkONERROR(gckHARDWARE_SetIsrManager(
            device->kernels[gcvCORE_MAJOR]->hardware,
            (gctISRMANAGERFUNC) gckGALDEVICE_Setup_ISR,
            (gctISRMANAGERFUNC) gckGALDEVICE_Release_ISR,
            device
            ));

        gcmkONERROR(gckHARDWARE_SetFastClear(
            device->kernels[gcvCORE_MAJOR]->hardware, FastClear, Compression
            ));

        gcmkONERROR(gckHARDWARE_SetPowerManagement(
            device->kernels[gcvCORE_MAJOR]->hardware, PowerManagement
            ));

        gcmkONERROR(gckHARDWARE_SetGpuProfiler(
            device->kernels[gcvCORE_MAJOR]->hardware, GpuProfiler
            ));

        gcmkVERIFY_OK(gckKERNEL_SetRecovery(
            device->kernels[gcvCORE_MAJOR], Args->recovery, Args->stuckDump
            ));

#if COMMAND_PROCESSOR_VERSION == 1
        /* Start the command queue. */
        gcmkONERROR(gckCOMMAND_Start(device->kernels[gcvCORE_MAJOR]->command));
#endif
    }
    else
    {
        device->kernels[gcvCORE_MAJOR] = gcvNULL;
    }

#if gcdMULTI_GPU_AFFINITY
    if (IrqLine3D1 != -1)
    {
        /* Construct the gckKERNEL object. */
        gcmkONERROR(gckKERNEL_Construct(
            device->os, gcvCORE_OCL, device,
            gcvNULL, &device->kernels[gcvCORE_OCL]));

        if (sharedDB == gcvNULL) sharedDB = device->kernels[gcvCORE_OCL]->db;

        /* Initialize core mapping */
        if (device->kernels[gcvCORE_MAJOR] == gcvNULL)
        {
            for (i = 0; i < 8; i++)
            {
                device->coreMapping[i] = gcvCORE_OCL;
            }
        }
        else
        {
            device->coreMapping[gcvHARDWARE_OCL] = gcvCORE_OCL;
        }

        /* Setup the ISR manager. */
        gcmkONERROR(gckHARDWARE_SetIsrManager(
            device->kernels[gcvCORE_OCL]->hardware,
            (gctISRMANAGERFUNC) gckGALDEVICE_Setup_ISR,
            (gctISRMANAGERFUNC) gckGALDEVICE_Release_ISR,
            device
            ));

        gcmkONERROR(gckHARDWARE_SetFastClear(
            device->kernels[gcvCORE_OCL]->hardware, FastClear, Compression
            ));

        gcmkONERROR(gckHARDWARE_SetPowerManagement(
            device->kernels[gcvCORE_OCL]->hardware, PowerManagement
            ));

#if COMMAND_PROCESSOR_VERSION == 1
        /* Start the command queue. */
        gcmkONERROR(gckCOMMAND_Start(device->kernels[gcvCORE_OCL]->command));
#endif
    }
    else
    {
        device->kernels[gcvCORE_OCL] = gcvNULL;
    }
#endif

    if (IrqLine2D != -1)
    {
        gcmkONERROR(gckKERNEL_Construct(
            device->os, gcvCORE_2D, device,
            sharedDB, &device->kernels[gcvCORE_2D]));

        if (sharedDB == gcvNULL) sharedDB = device->kernels[gcvCORE_2D]->db;

        /* Verify the hardware type */
        gcmkONERROR(gckHARDWARE_GetType(device->kernels[gcvCORE_2D]->hardware, &type));

        if (type != gcvHARDWARE_2D)
        {
            gcmkTRACE_ZONE(
                gcvLEVEL_ERROR, gcvZONE_DRIVER,
                "%s(%d): Unexpected hardware type: %d\n",
                __FUNCTION__, __LINE__,
                type
                );

            gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
        }

        /* Initialize core mapping */
        if (device->kernels[gcvCORE_MAJOR] == gcvNULL
#if gcdMULTI_GPU_AFFINITY
            && device->kernels[gcvCORE_OCL] == gcvNULL
#endif
            )
        {
            for (i = 0; i < 8; i++)
            {
                device->coreMapping[i] = gcvCORE_2D;
            }
        }
        else
        {
            device->coreMapping[gcvHARDWARE_2D] = gcvCORE_2D;
        }

        /* Setup the ISR manager. */
        gcmkONERROR(gckHARDWARE_SetIsrManager(
            device->kernels[gcvCORE_2D]->hardware,
            (gctISRMANAGERFUNC) gckGALDEVICE_Setup_ISR_2D,
            (gctISRMANAGERFUNC) gckGALDEVICE_Release_ISR_2D,
            device
            ));

        gcmkONERROR(gckHARDWARE_SetPowerManagement(
            device->kernels[gcvCORE_2D]->hardware, PowerManagement
            ));

        gcmkVERIFY_OK(gckKERNEL_SetRecovery(
            device->kernels[gcvCORE_2D], Args->recovery, Args->stuckDump
            ));

#if COMMAND_PROCESSOR_VERSION == 1
        /* Start the command queue. */
        gcmkONERROR(gckCOMMAND_Start(device->kernels[gcvCORE_2D]->command));
#endif
    }
    else
    {
        device->kernels[gcvCORE_2D] = gcvNULL;
    }

    if (IrqLineVG != -1)
    {
#if gcdENABLE_VG
        gcmkONERROR(gckKERNEL_Construct(
            device->os, gcvCORE_VG, device,
            sharedDB, &device->kernels[gcvCORE_VG]));
        /* Initialize core mapping */
        if (device->kernels[gcvCORE_MAJOR] == gcvNULL
            && device->kernels[gcvCORE_2D] == gcvNULL
#if gcdMULTI_GPU_AFFINITY
            && device->kernels[gcvCORE_OCL] == gcvNULL
#endif
            )
        {
            for (i = 0; i < 8; i++)
            {
                device->coreMapping[i] = gcvCORE_VG;
            }
        }
        else
        {
            device->coreMapping[gcvHARDWARE_VG] = gcvCORE_VG;
        }


        gcmkONERROR(gckVGHARDWARE_SetPowerManagement(
            device->kernels[gcvCORE_VG]->vg->hardware,
            PowerManagement
            ));
#endif
    }
    else
    {
        device->kernels[gcvCORE_VG] = gcvNULL;
    }

    /* Initialize power related values. */
    device->powerOffWhenIdle        = gcvTRUE;
    device->needPowerOff            = gcvFALSE;

    device->profileStep             = 300;
    device->profileTimeSlice        = 300;
    device->profileTailTimeSlice    = 33;
    device->idleThreshold           = 80;

    /* debug flags */
    device->powerDebug              = gcvFALSE;
    device->pmrtDebug               = gcvFALSE;
    device->profilerDebug           = gcvFALSE;
    device->printPID                = gcvFALSE;

    /* Initialize the ISR. */
#if gcdMULTI_GPU
    device->irqLine3D[gcvCORE_3D_0_ID] = IrqLine3D0;
#if gcdMULTI_GPU > 1
    device->irqLine3D[gcvCORE_3D_1_ID] = IrqLine3D1;
#endif
#elif gcdMULTI_GPU_AFFINITY
    device->irqLines[gcvCORE_MAJOR] = IrqLine3D0;
    device->irqLines[gcvCORE_OCL]   = IrqLine3D1;
#else
    device->irqLines[gcvCORE_MAJOR] = IrqLine;
#endif
    device->irqLines[gcvCORE_2D] = IrqLine2D;
    device->irqLines[gcvCORE_VG] = IrqLineVG;

    /* Initialize the kernel thread semaphores. */
    for (i = 0; i < gcdMAX_GPU_COUNT; i++)
    {
#if gcdMULTI_GPU
        if (i == gcvCORE_MAJOR)
        {
            for (j = 0; j < gcdMULTI_GPU; j++)
            {
                if (device->irqLine3D[j] != -1) init_waitqueue_head(&device->intrWaitQueue3D[j]);
            }
        }
        else
#endif
        {
            if (device->irqLines[i] != -1) sema_init(&device->semas[i], 0);
        }
    }

    device->signal = Signal;

    for (i = 0; i < gcdMAX_GPU_COUNT; i++)
    {
        if (device->kernels[i] != gcvNULL) break;
    }

    if (i == gcdMAX_GPU_COUNT)
    {
        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }

#if gcdENABLE_VG
    if (i == gcvCORE_VG)
    {
        /* Query the ceiling of the system memory. */
        gcmkONERROR(gckVGHARDWARE_QuerySystemMemory(
                device->kernels[i]->vg->hardware,
                &device->systemMemorySize,
                &device->systemMemoryBaseAddress
                ));
            /* query the amount of video memory */
        gcmkONERROR(gckVGHARDWARE_QueryMemory(
            device->kernels[i]->vg->hardware,
            &device->internalSize, &internalBaseAddress, &internalAlignment,
            &device->externalSize, &externalBaseAddress, &externalAlignment,
            &horizontalTileSize, &verticalTileSize
            ));
    }
    else
#endif
    {
        /* Query the ceiling of the system memory. */
        gcmkONERROR(gckHARDWARE_QuerySystemMemory(
                device->kernels[i]->hardware,
                &device->systemMemorySize,
                &device->systemMemoryBaseAddress
                ));

            /* query the amount of video memory */
        gcmkONERROR(gckHARDWARE_QueryMemory(
            device->kernels[i]->hardware,
            &device->internalSize, &internalBaseAddress, &internalAlignment,
            &device->externalSize, &externalBaseAddress, &externalAlignment,
            &horizontalTileSize, &verticalTileSize
            ));
    }


    /* Grab the first availiable kernel */
    for (i = 0; i < gcdMAX_GPU_COUNT; i++)
    {
#if gcdMULTI_GPU
        if (i == gcvCORE_MAJOR)
        {
            for (j = 0; j < gcdMULTI_GPU; j++)
            {
                if (device->irqLine3D[j] != -1)
                {
                    kernel = device->kernels[i];
                    break;
                }
            }
        }
        else
#endif
        {
            if (device->irqLines[i] != -1)
            {
                kernel = device->kernels[i];
                break;
            }
        }
    }

    /* Set up the internal memory region. */
    if (device->internalSize > 0)
    {
        status = gckVIDMEM_Construct(
            device->os,
            internalBaseAddress, device->internalSize, internalAlignment,
            0, &device->internalVidMem
            );

        if (gcmIS_ERROR(status))
        {
            /* Error, disable internal heap. */
            device->internalSize = 0;
        }
        else
        {
            /* Map internal memory. */
            device->internalLogical
                = (gctPOINTER) ioremap_nocache(physical, device->internalSize);

            if (device->internalLogical == gcvNULL)
            {
                gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
            }

            device->internalPhysical = (gctPHYS_ADDR)(gctUINTPTR_T) physical;
            device->internalPhysicalName = gcmPTR_TO_NAME(device->internalPhysical);
            physical += device->internalSize;
        }
    }

    if (device->externalSize > 0)
    {
        /* create the external memory heap */
        status = gckVIDMEM_Construct(
            device->os,
            externalBaseAddress, device->externalSize, externalAlignment,
            0, &device->externalVidMem
            );

        if (gcmIS_ERROR(status))
        {
            /* Error, disable internal heap. */
            device->externalSize = 0;
        }
        else
        {
            /* Map external memory. */
            device->externalLogical
                = (gctPOINTER) ioremap_nocache(physical, device->externalSize);

            if (device->externalLogical == gcvNULL)
            {
                gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
            }

            device->externalPhysical = (gctPHYS_ADDR)(gctUINTPTR_T) physical;
            device->externalPhysicalName = gcmPTR_TO_NAME(device->externalPhysical);
            physical += device->externalSize;
        }
    }

    /* set up the contiguous memory */
    device->contiguousSize = ContiguousSize;
    device->contiguousBase = (gctPOINTER)(gctUINTPTR_T)ContiguousBase;

    if (ContiguousSize > 0)
    {
        _AllocateContiguousMemory(device,
                                  kernel,
                                  ContiguousBase,
                                  ContiguousSize,
                                  BankSize);
    }

    /* Initialize GC memory profile*/
    device->reservedMem                     = device->contiguousSize;
    device->vidMemUsage                     = 0;
#if MRVL_VIDEO_MEMORY_USE_PMEM
    /*device->reservedPmemMem                 = PmemSize;*/
    device->pmemUsage                       = 0;
    gcmkPRINT("[galcore] GC use pmem as video memory is limited to Size = 0x%08x\n", (gctUINT32)device->reservedPmemMem);
#endif
    device->contiguousNonPagedMemUsage      = 0;
    device->contiguousPagedMemUsage         = 0;
    device->virtualPagedMemUsage            = 0;
    device->wastBytes                       = 0;
    device->indexVidMemUsage                = 0;
    device->vertexVidMemUsage               = 0;
    device->textureVidMemUsage              = 0;
    device->renderTargetVidMemUsage         = 0;
    device->depthVidMemUsage                = 0;
    device->bitmapVidMemUsage               = 0;
    device->tileStatusVidMemUsage           = 0;
    device->imageVidMemUsage                = 0;
    device->maskVidMemUsage                 = 0;
    device->scissorVidMemUsage              = 0;
    device->hierarchicalDepthVidMemUsage    = 0;
    device->othersVidMemUsage               = 0;

    /* Return pointer to the device. */
    *Device = device;

    gcmkFOOTER_ARG("*Device=0x%x", * Device);
    return gcvSTATUS_OK;

OnError:
    /* Roll back. */
    gcmkVERIFY_OK(gckGALDEVICE_Destroy(device));

    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckGALDEVICE_Destroy
**
**  Class destructor.
**
**  INPUT:
**
**      Nothing.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      Nothing.
*/
gceSTATUS
gckGALDEVICE_Destroy(
    gckGALDEVICE Device)
{
    gctINT i;
#if gcdMULTI_GPU
    gctINT j;
#endif
    gceSTATUS status = gcvSTATUS_OK;
    gckKERNEL kernel = gcvNULL;

    gcmkHEADER_ARG("Device=0x%x", Device);

    if (Device != gcvNULL)
    {
        /* Grab the first availiable kernel */
        for (i = 0; i < gcdMAX_GPU_COUNT; i++)
        {
#if gcdMULTI_GPU
            if (i == gcvCORE_MAJOR)
            {
                for (j = 0; j < gcdMULTI_GPU; j++)
                {
                    if (Device->irqLine3D[j] != -1)
                    {
                        kernel = Device->kernels[i];
                        break;
                    }
                }
            }
            else
#endif
            {
                if (Device->irqLines[i] != -1)
                {
                    kernel = Device->kernels[i];
                    break;
                }
            }
        }

        if (Device->internalPhysicalName != 0)
        {
            gcmRELEASE_NAME(Device->internalPhysicalName);
            Device->internalPhysicalName = 0;
        }
        if (Device->externalPhysicalName != 0)
        {
            gcmRELEASE_NAME(Device->externalPhysicalName);
            Device->externalPhysicalName = 0;
        }
        if (Device->contiguousPhysicalName != 0)
        {
            gcmRELEASE_NAME(Device->contiguousPhysicalName);
            Device->contiguousPhysicalName = 0;
        }


        for (i = 0; i < gcdMAX_GPU_COUNT; i++)
        {
            if (Device->kernels[i] != gcvNULL)
            {
                /* Destroy the gckKERNEL object. */
                gcmkVERIFY_OK(gckKERNEL_Destroy(Device->kernels[i]));
                Device->kernels[i] = gcvNULL;
            }
        }

        if (Device->internalLogical != gcvNULL)
        {
            /* Unmap the internal memory. */
            iounmap(Device->internalLogical);
            Device->internalLogical = gcvNULL;
        }

        if (Device->internalVidMem != gcvNULL)
        {
            /* Destroy the internal heap. */
            gcmkVERIFY_OK(gckVIDMEM_Destroy(Device->internalVidMem));
            Device->internalVidMem = gcvNULL;
        }

        if (Device->externalLogical != gcvNULL)
        {
            /* Unmap the external memory. */
            iounmap(Device->externalLogical);
            Device->externalLogical = gcvNULL;
        }

        if (Device->externalVidMem != gcvNULL)
        {
            /* destroy the external heap */
            gcmkVERIFY_OK(gckVIDMEM_Destroy(Device->externalVidMem));
            Device->externalVidMem = gcvNULL;
        }

        if (Device->contiguousBase != gcvNULL)
        {
            if (Device->contiguousMapped)
            {
#if !gcdDYNAMIC_MAP_RESERVED_MEMORY && gcdENABLE_VG
                if (Device->contiguousBase)
                {
                    /* Unmap the contiguous memory. */
                    iounmap(Device->contiguousBase);
                }
#endif
            }
            else
            {
                gcmkONERROR(_FreeMemory(
                    Device,
                    Device->contiguousBase,
                    Device->contiguousPhysical
                    ));
            }

            Device->contiguousBase     = gcvNULL;
            Device->contiguousPhysical = gcvNULL;
        }

        if (Device->requestedContiguousBase != 0)
        {
            release_mem_region(Device->requestedContiguousBase, Device->requestedContiguousSize);
            Device->requestedContiguousBase = 0;
            Device->requestedContiguousSize = 0;
        }

        if (Device->contiguousVidMem != gcvNULL)
        {
            /* Destroy the contiguous heap. */
            gcmkVERIFY_OK(gckVIDMEM_Destroy(Device->contiguousVidMem));
            Device->contiguousVidMem = gcvNULL;
        }

        if(gckDEBUGFS_IsEnabled())
        {
            gckDEBUGFS_FreeNode(Device->dbgNode);
         if(Device->dbgNode != gcvNULL)
             {
               kfree(Device->dbgNode);
               Device->dbgNode = gcvNULL;
             }
        }

        for (i = 0; i < gcdMAX_GPU_COUNT; i++)
        {
#if gcdMULTI_GPU
            if (i == gcvCORE_MAJOR)
            {
                for (j = 0; j < gcdMULTI_GPU; j++)
                {
                    if (Device->registerBase3D[j] != gcvNULL)
                    {
                        /* Unmap register memory. */
                        iounmap(Device->registerBase3D[j]);
                        if (Device->requestedRegisterMemBase3D[j] != 0)
                        {
                            release_mem_region(Device->requestedRegisterMemBase3D[j],
                                    Device->requestedRegisterMemSize3D[j]);
                        }

                        Device->registerBase3D[j] = gcvNULL;
                        Device->requestedRegisterMemBase3D[j] = 0;
                        Device->requestedRegisterMemSize3D[j] = 0;
                    }
                }
            }
            else
#endif
            {
                if (Device->registerBases[i] != gcvNULL)
                {
                    /* Unmap register memory. */
                    iounmap(Device->registerBases[i]);
                    if (Device->requestedRegisterMemBases[i] != 0)
                    {
                        release_mem_region(Device->requestedRegisterMemBases[i],
                                Device->requestedRegisterMemSizes[i]);
                    }

                    Device->registerBases[i] = gcvNULL;
                    Device->requestedRegisterMemBases[i] = 0;
                    Device->requestedRegisterMemSizes[i] = 0;
                }
            }
        }

        /* Destroy the gckOS object. */
        if (Device->os != gcvNULL)
        {
            gcmkVERIFY_OK(gckOS_Destroy(Device->os));
            Device->os = gcvNULL;
        }

        /* Free the device. */
        kfree(Device);
    }

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckGALDEVICE_Setup_ISR
**
**  Start the ISR routine.
**
**  INPUT:
**
**      gckGALDEVICE Device
**          Pointer to an gckGALDEVICE object.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      gcvSTATUS_OK
**          Setup successfully.
**      gcvSTATUS_GENERIC_IO
**          Setup failed.
*/
gceSTATUS
gckGALDEVICE_Setup_ISR(
    IN gckGALDEVICE Device
    )
{
    gceSTATUS status;
    gctINT ret = 0;

    gcmkHEADER_ARG("Device=0x%x", Device);

    gcmkVERIFY_ARGUMENT(Device != NULL);

    if (Device->irqLines[gcvCORE_MAJOR] < 0)
    {
        gcmkONERROR(gcvSTATUS_GENERIC_IO);
    }

    /* Hook up the isr based on the irq line. */
#ifdef FLAREON
    gc500_handle.dev_name  = "galcore interrupt service";
    gc500_handle.dev_id    = Device;
    gc500_handle.handler   = isrRoutine;
    gc500_handle.intr_gen  = GPIO_INTR_LEVEL_TRIGGER;
    gc500_handle.intr_trig = GPIO_TRIG_HIGH_LEVEL;

    ret = dove_gpio_request(
        DOVE_GPIO0_7, &gc500_handle
        );
#else
#if gcdMULTI_GPU
    ret = request_irq(
        Device->irqLine3D[gcvCORE_3D_0_ID], isrRoutine3D0, IRQF_DISABLED,
        "galcore_3d_0", Device
        );

    if (ret != 0)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): Could not register irq line %d (error=%d)\n",
            __FUNCTION__, __LINE__,
            Device->irqLine3D[gcvCORE_3D_0_ID], ret
            );

        gcmkONERROR(gcvSTATUS_GENERIC_IO);
    }

    /* Mark ISR as initialized. */
    Device->isrInitialized3D[gcvCORE_3D_0_ID] = gcvTRUE;

#if gcdMULTI_GPU > 1
    ret = request_irq(
        Device->irqLine3D[gcvCORE_3D_1_ID], isrRoutine3D1, IRQF_DISABLED,
        "galcore_3d_1", Device
        );

    if (ret != 0)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): Could not register irq line %d (error=%d)\n",
            __FUNCTION__, __LINE__,
            Device->irqLine3D[gcvCORE_3D_1_ID], ret
            );

        gcmkONERROR(gcvSTATUS_GENERIC_IO);
    }

    /* Mark ISR as initialized. */
    Device->isrInitialized3D[gcvCORE_3D_1_ID] = gcvTRUE;
#endif
#elif gcdMULTI_GPU_AFFINITY
    ret = request_irq(
        Device->irqLines[gcvCORE_MAJOR], isrRoutine3D0, IRQF_DISABLED,
        "galcore_3d_0", Device
        );

    if (ret != 0)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): Could not register irq line %d (error=%d)\n",
            __FUNCTION__, __LINE__,
            Device->irqLines[gcvCORE_MAJOR], ret
            );

        gcmkONERROR(gcvSTATUS_GENERIC_IO);
    }

    /* Mark ISR as initialized. */
    Device->isrInitializeds[gcvCORE_MAJOR] = gcvTRUE;

    ret = request_irq(
        Device->irqLines[gcvCORE_OCL], isrRoutine3D1, IRQF_DISABLED,
        "galcore_3d_1", Device
        );

    if (ret != 0)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): Could not register irq line %d (error=%d)\n",
            __FUNCTION__, __LINE__,
            Device->irqLines[gcvCORE_OCL], ret
            );

        gcmkONERROR(gcvSTATUS_GENERIC_IO);
    }

    /* Mark ISR as initialized. */
    Device->isrInitializeds[gcvCORE_OCL] = gcvTRUE;
#else
    ret = request_irq(
        Device->irqLines[gcvCORE_MAJOR], isrRoutine, IRQF_DISABLED,
        "galcore interrupt service", Device
        );

    if (ret != 0)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): Could not register irq line %d (error=%d)\n",
            __FUNCTION__, __LINE__,
            Device->irqLines[gcvCORE_MAJOR], ret
            );

        gcmkONERROR(gcvSTATUS_GENERIC_IO);
    }

    /* Mark ISR as initialized. */
    Device->isrInitializeds[gcvCORE_MAJOR] = gcvTRUE;
#endif
#endif

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}

gceSTATUS
gckGALDEVICE_Setup_ISR_2D(
    IN gckGALDEVICE Device
    )
{
    gceSTATUS status;
    gctINT ret;

    gcmkHEADER_ARG("Device=0x%x", Device);

    gcmkVERIFY_ARGUMENT(Device != NULL);

    if (Device->irqLines[gcvCORE_2D] < 0)
    {
        gcmkONERROR(gcvSTATUS_GENERIC_IO);
    }

    /* Hook up the isr based on the irq line. */
#ifdef FLAREON
    gc500_handle.dev_name  = "galcore interrupt service";
    gc500_handle.dev_id    = Device;
    gc500_handle.handler   = isrRoutine2D;
    gc500_handle.intr_gen  = GPIO_INTR_LEVEL_TRIGGER;
    gc500_handle.intr_trig = GPIO_TRIG_HIGH_LEVEL;

    ret = dove_gpio_request(
        DOVE_GPIO0_7, &gc500_handle
        );
#else
    ret = request_irq(
        Device->irqLines[gcvCORE_2D], isrRoutine2D, IRQF_DISABLED,
        "galcore interrupt service for 2D", Device
        );
#endif

    if (ret != 0)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): Could not register irq line %d (error=%d)\n",
            __FUNCTION__, __LINE__,
            Device->irqLines[gcvCORE_2D], ret
            );

        gcmkONERROR(gcvSTATUS_GENERIC_IO);
    }

    /* Mark ISR as initialized. */
    Device->isrInitializeds[gcvCORE_2D] = gcvTRUE;

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}

gceSTATUS
gckGALDEVICE_Setup_ISR_VG(
    IN gckGALDEVICE Device
    )
{
    gceSTATUS status;
    gctINT ret;

    gcmkHEADER_ARG("Device=0x%x", Device);

    gcmkVERIFY_ARGUMENT(Device != NULL);

    if (Device->irqLines[gcvCORE_VG] < 0)
    {
        gcmkONERROR(gcvSTATUS_GENERIC_IO);
    }

    /* Hook up the isr based on the irq line. */
#ifdef FLAREON
    gc500_handle.dev_name  = "galcore interrupt service";
    gc500_handle.dev_id    = Device;
    gc500_handle.handler   = isrRoutineVG;
    gc500_handle.intr_gen  = GPIO_INTR_LEVEL_TRIGGER;
    gc500_handle.intr_trig = GPIO_TRIG_HIGH_LEVEL;

    ret = dove_gpio_request(
        DOVE_GPIO0_7, &gc500_handle
        );
#else
    ret = request_irq(
        Device->irqLines[gcvCORE_VG], isrRoutineVG, IRQF_DISABLED,
        "galcore interrupt service for 2D", Device
        );
#endif

    if (ret != 0)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): Could not register irq line %d (error=%d)\n",
            __FUNCTION__, __LINE__,
            Device->irqLines[gcvCORE_VG], ret
            );

        gcmkONERROR(gcvSTATUS_GENERIC_IO);
    }

    /* Mark ISR as initialized. */
    Device->isrInitializeds[gcvCORE_VG] = gcvTRUE;

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckGALDEVICE_Release_ISR
**
**  Release the irq line.
**
**  INPUT:
**
**      gckGALDEVICE Device
**          Pointer to an gckGALDEVICE object.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      Nothing.
*/
gceSTATUS
gckGALDEVICE_Release_ISR(
    IN gckGALDEVICE Device
    )
{
    gcmkHEADER_ARG("Device=0x%x", Device);

    gcmkVERIFY_ARGUMENT(Device != NULL);

#if gcdMULTI_GPU
    /* release the irq */
    if (Device->isrInitialized3D[gcvCORE_3D_0_ID])
    {
        free_irq(Device->irqLine3D[gcvCORE_3D_0_ID], Device);
        Device->isrInitialized3D[gcvCORE_3D_0_ID] = gcvFALSE;
    }
#if gcdMULTI_GPU > 1
    /* release the irq */
    if (Device->isrInitialized3D[gcvCORE_3D_1_ID])
    {
        free_irq(Device->irqLine3D[gcvCORE_3D_1_ID], Device);
        Device->isrInitialized3D[gcvCORE_3D_1_ID] = gcvFALSE;
    }
#endif
#else
    /* release the irq */
    if (Device->isrInitializeds[gcvCORE_MAJOR])
    {
#ifdef FLAREON
        dove_gpio_free(DOVE_GPIO0_7, "galcore interrupt service");
#else
        free_irq(Device->irqLines[gcvCORE_MAJOR], Device);
#endif
        Device->isrInitializeds[gcvCORE_MAJOR] = gcvFALSE;
    }
#endif

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;
}

gceSTATUS
gckGALDEVICE_Release_ISR_2D(
    IN gckGALDEVICE Device
    )
{
    gcmkHEADER_ARG("Device=0x%x", Device);

    gcmkVERIFY_ARGUMENT(Device != NULL);

    /* release the irq */
    if (Device->isrInitializeds[gcvCORE_2D])
    {
#ifdef FLAREON
        dove_gpio_free(DOVE_GPIO0_7, "galcore interrupt service");
#else
        free_irq(Device->irqLines[gcvCORE_2D], Device);
#endif

        Device->isrInitializeds[gcvCORE_2D] = gcvFALSE;
    }

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;
}

gceSTATUS
gckGALDEVICE_Release_ISR_VG(
    IN gckGALDEVICE Device
    )
{
    gcmkHEADER_ARG("Device=0x%x", Device);

    gcmkVERIFY_ARGUMENT(Device != NULL);

    /* release the irq */
    if (Device->isrInitializeds[gcvCORE_VG])
    {
#ifdef FLAREON
        dove_gpio_free(DOVE_GPIO0_7, "galcore interrupt service");
#else
        free_irq(Device->irqLines[gcvCORE_VG], Device);
#endif

        Device->isrInitializeds[gcvCORE_VG] = gcvFALSE;
    }

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gckGALDEVICE_Start_Threads
**
**  Start the daemon threads.
**
**  INPUT:
**
**      gckGALDEVICE Device
**          Pointer to an gckGALDEVICE object.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      gcvSTATUS_OK
**          Start successfully.
**      gcvSTATUS_GENERIC_IO
**          Start failed.
*/
gceSTATUS
gckGALDEVICE_Start_Threads(
    IN gckGALDEVICE Device
    )
{
    gceSTATUS status;
    struct task_struct * task;

    gcmkHEADER_ARG("Device=0x%x", Device);

    gcmkVERIFY_ARGUMENT(Device != NULL);

#if gcdMULTI_GPU
    if (Device->kernels[gcvCORE_MAJOR] != gcvNULL)
    {
        /* Start the kernel thread. */
        task = kthread_run(threadRoutine3D0, Device, "galcore_3d_0");

        if (IS_ERR(task))
        {
            gcmkTRACE_ZONE(
                gcvLEVEL_ERROR, gcvZONE_DRIVER,
                "%s(%d): Could not start the kernel thread.\n",
                __FUNCTION__, __LINE__
                );

            gcmkONERROR(gcvSTATUS_GENERIC_IO);
        }

        Device->threadCtxt3D[gcvCORE_3D_0_ID]          = task;
        Device->threadInitialized3D[gcvCORE_3D_0_ID]   = gcvTRUE;

#if gcdMULTI_GPU > 1
        /* Start the kernel thread. */
        task = kthread_run(threadRoutine3D1, Device, "galcore_3d_1");

        if (IS_ERR(task))
        {
            gcmkTRACE_ZONE(
                gcvLEVEL_ERROR, gcvZONE_DRIVER,
                "%s(%d): Could not start the kernel thread.\n",
                __FUNCTION__, __LINE__
                );

            gcmkONERROR(gcvSTATUS_GENERIC_IO);
        }

        Device->threadCtxt3D[gcvCORE_3D_1_ID]          = task;
        Device->threadInitialized3D[gcvCORE_3D_1_ID]   = gcvTRUE;
#endif
    }
#elif gcdMULTI_GPU_AFFINITY
    if (Device->kernels[gcvCORE_MAJOR] != gcvNULL)
    {
        /* Start the kernel thread. */
        task = kthread_run(threadRoutine3D0, Device, "galcore_3d_0");

        if (IS_ERR(task))
        {
            gcmkTRACE_ZONE(
                gcvLEVEL_ERROR, gcvZONE_DRIVER,
                "%s(%d): Could not start the kernel thread.\n",
                __FUNCTION__, __LINE__
                );

            gcmkONERROR(gcvSTATUS_GENERIC_IO);
        }

        Device->threadCtxts[gcvCORE_MAJOR]          = task;
        Device->threadInitializeds[gcvCORE_MAJOR]   = gcvTRUE;
    }

    if (Device->kernels[gcvCORE_OCL] != gcvNULL)
    {
        /* Start the kernel thread. */
        task = kthread_run(threadRoutine3D1, Device, "galcore_3d_1");

        if (IS_ERR(task))
        {
            gcmkTRACE_ZONE(
                gcvLEVEL_ERROR, gcvZONE_DRIVER,
                "%s(%d): Could not start the kernel thread.\n",
                __FUNCTION__, __LINE__
                );

            gcmkONERROR(gcvSTATUS_GENERIC_IO);
        }

        Device->threadCtxts[gcvCORE_OCL]          = task;
        Device->threadInitializeds[gcvCORE_OCL]   = gcvTRUE;
    }
#else
    if (Device->kernels[gcvCORE_MAJOR] != gcvNULL)
    {
        /* Start the kernel thread. */
        task = kthread_run(threadRoutine, Device, "galcore daemon thread");

        if (IS_ERR(task))
        {
            gcmkTRACE_ZONE(
                gcvLEVEL_ERROR, gcvZONE_DRIVER,
                "%s(%d): Could not start the kernel thread.\n",
                __FUNCTION__, __LINE__
                );

            gcmkONERROR(gcvSTATUS_GENERIC_IO);
        }

        Device->threadCtxts[gcvCORE_MAJOR]          = task;
        Device->threadInitializeds[gcvCORE_MAJOR]   = gcvTRUE;
    }
#endif

    if (Device->kernels[gcvCORE_2D] != gcvNULL)
    {
        /* Start the kernel thread. */
        task = kthread_run(threadRoutine2D, Device, "galcore daemon thread for 2D");

        if (IS_ERR(task))
        {
            gcmkTRACE_ZONE(
                gcvLEVEL_ERROR, gcvZONE_DRIVER,
                "%s(%d): Could not start the kernel thread.\n",
                __FUNCTION__, __LINE__
                );

            gcmkONERROR(gcvSTATUS_GENERIC_IO);
        }

        Device->threadCtxts[gcvCORE_2D]         = task;
        Device->threadInitializeds[gcvCORE_2D]  = gcvTRUE;
    }
    else
    {
        Device->threadInitializeds[gcvCORE_2D]  = gcvFALSE;
    }

    if (Device->kernels[gcvCORE_VG] != gcvNULL)
    {
        /* Start the kernel thread. */
        task = kthread_run(threadRoutineVG, Device, "galcore daemon thread for VG");

        if (IS_ERR(task))
        {
            gcmkTRACE_ZONE(
                gcvLEVEL_ERROR, gcvZONE_DRIVER,
                "%s(%d): Could not start the kernel thread.\n",
                __FUNCTION__, __LINE__
                );

            gcmkONERROR(gcvSTATUS_GENERIC_IO);
        }

        Device->threadCtxts[gcvCORE_VG]         = task;
        Device->threadInitializeds[gcvCORE_VG]  = gcvTRUE;
    }
    else
    {
        Device->threadInitializeds[gcvCORE_VG]  = gcvFALSE;
    }

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckGALDEVICE_Stop_Threads
**
**  Stop the gal device, including the following actions: stop the daemon
**  thread, release the irq.
**
**  INPUT:
**
**      gckGALDEVICE Device
**          Pointer to an gckGALDEVICE object.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      Nothing.
*/
gceSTATUS
gckGALDEVICE_Stop_Threads(
    gckGALDEVICE Device
    )
{
    gctINT i;
#if gcdMULTI_GPU
    gctINT j;
#endif

    gcmkHEADER_ARG("Device=0x%x", Device);

    gcmkVERIFY_ARGUMENT(Device != NULL);

    for (i = 0; i < gcdMAX_GPU_COUNT; i++)
    {
#if gcdMULTI_GPU
        if (i == gcvCORE_MAJOR)
        {
            for (j = 0; j < gcdMULTI_GPU; j++)
            {
                /* Stop the kernel threads. */
                if (Device->threadInitialized3D[j])
                {
                    Device->killThread = gcvTRUE;
                    Device->dataReady3D[j] = gcvTRUE;
                    wake_up_interruptible(&Device->intrWaitQueue3D[j]);

                    kthread_stop(Device->threadCtxt3D[j]);
                    Device->threadCtxt3D[j]        = gcvNULL;
                    Device->threadInitialized3D[j] = gcvFALSE;
                }
            }
        }
        else
#endif
        {
            /* Stop the kernel threads. */
            if (Device->threadInitializeds[i])
            {
                Device->killThread = gcvTRUE;
                up(&Device->semas[i]);

                kthread_stop(Device->threadCtxts[i]);
                Device->threadCtxts[i]        = gcvNULL;
                Device->threadInitializeds[i] = gcvFALSE;
            }
        }
    }

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gckGALDEVICE_Start
**
**  Start the gal device, including the following actions: setup the isr routine
**  and start the daemoni thread.
**
**  INPUT:
**
**      gckGALDEVICE Device
**          Pointer to an gckGALDEVICE object.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      gcvSTATUS_OK
**          Start successfully.
*/
gceSTATUS
gckGALDEVICE_Start(
    IN gckGALDEVICE Device
    )
{
    gceSTATUS status;

    gcmkHEADER_ARG("Device=0x%x", Device);

    /* Start the kernel thread. */
    gcmkONERROR(gckGALDEVICE_Start_Threads(Device));

    if (Device->kernels[gcvCORE_MAJOR] != gcvNULL)
    {
        /* Setup the ISR routine. */
        gcmkONERROR(gckGALDEVICE_Setup_ISR(Device));

        /* Switch to SUSPEND power state. */
        gcmkONERROR(gckHARDWARE_SetPowerManagementState(
            Device->kernels[gcvCORE_MAJOR]->hardware, gcvPOWER_OFF_BROADCAST
            ));
    }

    if (Device->kernels[gcvCORE_2D] != gcvNULL)
    {
        /* Setup the ISR routine. */
        gcmkONERROR(gckGALDEVICE_Setup_ISR_2D(Device));

        /* Switch to SUSPEND power state. */
        gcmkONERROR(gckHARDWARE_SetPowerManagementState(
            Device->kernels[gcvCORE_2D]->hardware, gcvPOWER_OFF_BROADCAST
            ));
    }

    if (Device->kernels[gcvCORE_VG] != gcvNULL)
    {
        /* Setup the ISR routine. */
        gcmkONERROR(gckGALDEVICE_Setup_ISR_VG(Device));
    }

#if MRVL_PROFILE_THREAD
    Device->profilethread = kthread_run(profile_thread, Device, "galcore profile thread");
#endif

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckGALDEVICE_Stop
**
**  Stop the gal device, including the following actions: stop the daemon
**  thread, release the irq.
**
**  INPUT:
**
**      gckGALDEVICE Device
**          Pointer to an gckGALDEVICE object.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      Nothing.
*/
gceSTATUS
gckGALDEVICE_Stop(
    gckGALDEVICE Device
    )
{
    gceSTATUS status;

    gcmkHEADER_ARG("Device=0x%x", Device);

    gcmkVERIFY_ARGUMENT(Device != NULL);

#if MRVL_PROFILE_THREAD
    kthread_stop(Device->profilethread);
#endif

    if (Device->kernels[gcvCORE_MAJOR] != gcvNULL)
    {
        /* Switch to OFF power state. */
        gcmkONERROR(gckHARDWARE_SetPowerManagementState(
            Device->kernels[gcvCORE_MAJOR]->hardware, gcvPOWER_OFF
            ));

        /* Remove the ISR routine. */
        gcmkONERROR(gckGALDEVICE_Release_ISR(Device));
    }

    if (Device->kernels[gcvCORE_2D] != gcvNULL)
    {
        /* Setup the ISR routine. */
        gcmkONERROR(gckGALDEVICE_Release_ISR_2D(Device));

        /* Switch to OFF power state. */
        gcmkONERROR(gckHARDWARE_SetPowerManagementState(
            Device->kernels[gcvCORE_2D]->hardware, gcvPOWER_OFF
            ));
    }

    if (Device->kernels[gcvCORE_VG] != gcvNULL)
    {
        /* Setup the ISR routine. */
        gcmkONERROR(gckGALDEVICE_Release_ISR_VG(Device));

#if gcdENABLE_VG
        /* Switch to OFF power state. */
        gcmkONERROR(gckVGHARDWARE_SetPowerManagementState(
            Device->kernels[gcvCORE_VG]->vg->hardware, gcvPOWER_OFF
            ));
#endif
    }

    /* Stop the kernel thread. */
    gcmkONERROR(gckGALDEVICE_Stop_Threads(Device));

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}
