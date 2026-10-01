/*
 * Copyright (c) 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *    Keith Packard <keithp@keithp.com>
 *    Xiang Haihao <haihao.xiang@intel.com>
 *    Zhao Yakui <yakui.zhao@intel.com>
 *
 */

/* shaders_gen8.h - the Gen8 pixel shader that samples a B8G8R8A8 surface and
 * writes the render target: exa_wm_src_affine + exa_wm_src_sample_argb +
 * exa_wm_write, the assembled binaries from intel-vaapi-driver's
 * src/shaders/render (the "PS_SUBPIC" kernel of gen8_render.c). */
#pragma once

static const u32 qrt_ps_kernel_gen8[][4] = {
   { 0x0060005a, 0x28403ae8, 0x3a000140, 0x008d0040 },
   { 0x0060005a, 0x28603ae8, 0x3a000140, 0x008d0080 },
   { 0x0060005a, 0x28803ae8, 0x3a000150, 0x008d0040 },
   { 0x0060005a, 0x28a03ae8, 0x3a000150, 0x008d0080 },
   { 0x00000001, 0x2008060c, 0x00000000, 0x00000000 },
   { 0x00600001, 0x2820020c, 0x008d0000, 0x00000000 },
   { 0x02800031, 0x21c00a48, 0x0e000820, 0x0a8c0001 },
   { 0x00600041, 0x22803aec, 0x3a8d0280, 0x000000c0 },
   { 0x00600041, 0x22a03aec, 0x3a8d02a0, 0x000000c0 },
   { 0x00600001, 0x2e00020c, 0x008d0000, 0x00000000 },
   { 0x00600001, 0x2e20020c, 0x008d0020, 0x00000000 },
   { 0x00600001, 0x2e403aec, 0x008d01c0, 0x00000000 },
   { 0x00600001, 0x2e603aec, 0x008d01e0, 0x00000000 },
   { 0x00600001, 0x2e803aec, 0x008d0200, 0x00000000 },
   { 0x00600001, 0x2ea03aec, 0x008d0220, 0x00000000 },
   { 0x00600001, 0x2ec03aec, 0x008d0240, 0x00000000 },
   { 0x00600001, 0x2ee03aec, 0x008d0260, 0x00000000 },
   { 0x00600001, 0x2f003aec, 0x008d0280, 0x00000000 },
   { 0x00600001, 0x2f203aec, 0x008d02a0, 0x00000000 },
   { 0x05800031, 0x20000a40, 0x0e000e00, 0x940b1000 },
   { 0x0000007e, 0x00000000, 0x00000000, 0x00000000 },
   { 0x0000007e, 0x00000000, 0x00000000, 0x00000000 },
   { 0x0000007e, 0x00000000, 0x00000000, 0x00000000 },
   { 0x0000007e, 0x00000000, 0x00000000, 0x00000000 },
   { 0x0000007e, 0x00000000, 0x00000000, 0x00000000 },
   { 0x0000007e, 0x00000000, 0x00000000, 0x00000000 },
   { 0x0000007e, 0x00000000, 0x00000000, 0x00000000 },
   { 0x0000007e, 0x00000000, 0x00000000, 0x00000000 },
};
