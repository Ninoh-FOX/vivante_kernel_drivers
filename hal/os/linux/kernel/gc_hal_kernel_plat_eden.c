/*
 * gc_hal_kernel_plat_eden.c
 *
 * Author: Watson Wang <zswang@marvell.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. *
 */

#include "gc_hal_kernel_plat_common.h"

#if MRVL_PLATFORM_TTD2
#define __PLAT_APINAME(apiname)     eden_##apiname

extern void gc3d_pwr(PARAM_TYPE_PWR);
#define GC3D_PWR    gc3d_pwr
extern void gc2d_pwr(PARAM_TYPE_PWR);
#define GC2D_PWR    gc2d_pwr

/**
 * gc3d shader definition
 */
static struct gc_ops gc3dsh_ops = {
    .init       = gpu_lock_init_dft,
    .enableclk  = gpu_clk_enable_dft,
    .disableclk = gpu_clk_disable_dft,
    .setclkrate = gpu_clk_setrate_dft,
    .getclkrate = gpu_clk_getrate_dft,
};

static struct gc_iface gc3dsh_iface = {
    .name               = "gc3dsh",
    .con_id             = "GC3D_CLK2X",
    .ops                = &gc3dsh_ops,
};

/**
 * gc3d definition
 */
static void __PLAT_APINAME(gc3d_pwr_ops)(struct gc_iface *iface, PARAM_TYPE_PWR enabled)
{
    PR_DEBUG("[%6s] %s %d\n", iface->name, __func__, enabled);
#if MRVL_DFC_JUMP_HI_INDIRECT
    {
        unsigned int count = iface->chains_count;

        /* Make it safe to do power operations */
        gpu_clk_setrate(iface, 156000);

        while(count-- != 0)
            gpu_clk_setrate(iface->chains_clk[count], 156000);
    }
#endif
    GC3D_PWR(enabled);
}

static struct gc_ops gc3d_ops = {
    .init       = gpu_lock_init_dft,
    .enableclk  = gpu_clk_enable_dft,
    .disableclk = gpu_clk_disable_dft,
    .setclkrate = gpu_clk_setrate_dft,
    .getclkrate = gpu_clk_getrate_dft,
    .pwrops     = __PLAT_APINAME(gc3d_pwr_ops),
};

static struct gc_iface *gc3d_iface_chains[] = {
    &gc3dsh_iface,
};

static struct gc_iface gc3d_iface = {
    .name               = "gc3d",
    .con_id             = "GC3D_CLK1X",
    .ops                = &gc3d_ops,
    .chains_count       = 1,
    .chains_clk         = gc3d_iface_chains,
};

/**
 * gc2d definition
 */
static void __PLAT_APINAME(gc2d_pwr_ops)(struct gc_iface *iface, PARAM_TYPE_PWR enabled)
{
    PR_DEBUG("[%6s] %s %d\n", iface->name, __func__, enabled);
#if MRVL_DFC_JUMP_HI_INDIRECT
    {
        /* make it safe to do Power operation */
        gpu_clk_setrate(iface, 156000);
    }
#endif
    GC2D_PWR(enabled);
}

static struct gc_ops gc2d_ops = {
    .init       = gpu_lock_init_dft,
    .enableclk  = gpu_clk_enable_dft,
    .disableclk = gpu_clk_disable_dft,
    .setclkrate = gpu_clk_setrate_dft,
    .getclkrate = gpu_clk_getrate_dft,
    .pwrops     = __PLAT_APINAME(gc2d_pwr_ops),
};

static struct gc_iface gc2d_iface = {
    .name               = "gc2d",
    .con_id             = "GC2D_CLK",
    .ops                = &gc2d_ops,
};

struct gc_iface *gc_ifaces[] = {
    [gcvCORE_MAJOR] = &gc3d_iface,
    [gcvCORE_2D]    = &gc2d_iface,
    [gcvCORE_SH]    = &gc3dsh_iface,
    gcvNULL,
};

#endif
