/*
 * vircgroupv2devices.c: methods for cgroups v2 BPF devices
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */
#include <config.h>

#if HAVE_DECL_BPF_CGROUP_DEVICE
# include <fcntl.h>
# include <linux/bpf.h>
# include <sys/stat.h>
# include <sys/syscall.h>
# include <sys/types.h>
#endif /* !HAVE_DECL_BPF_CGROUP_DEVICE */

#include "internal.h"

#define LIBVIRT_VIRCGROUPPRIV_H_ALLOW
#include "vircgrouppriv.h"

#include "viralloc.h"
#include "virbpf.h"
#include "vircgroup.h"
#include "vircgroupv2devices.h"
#include "virfile.h"
#include "virlog.h"

VIR_LOG_INIT("util.cgroup");

#define VIR_FROM_THIS VIR_FROM_CGROUP

#if HAVE_DECL_BPF_CGROUP_DEVICE
bool
virCgroupV2DevicesAvailable(virCgroupPtr group)
{
    VIR_AUTOCLOSE cgroupfd = -1;
    unsigned int progCnt = 0;

    cgroupfd = open(group->unified.mountPoint, O_RDONLY);
    if (cgroupfd < 0) {
        VIR_DEBUG("failed to open cgroup '%s'", group->unified.mountPoint);
        return false;
    }

    if (virBPFQueryProg(cgroupfd, 0, BPF_CGROUP_DEVICE, &progCnt, NULL) < 0) {
        VIR_DEBUG("failed to query cgroup progs");
        return false;
    }

    return true;
}


/* Steps to get assembly version of devices BPF program:
 *
 * Save the following program into bpfprog.c, compile it using clang:
 *
 *     clang -O2 -Wall -target bpf -c bpfprog.c -o bpfprog.o
 *
 * Now you can use llvm-objdump to get the list if instructions:
 *
 *     llvm-objdump -S -no-show-raw-insn bpfprog.o
 *
 * which can be converted into program using VIR_BPF_* macros.
 *
 * ----------------------------------------------------------------------------
 * #include <linux/bpf.h>
 * #include <linux/version.h>
 *
 * #define SEC(NAME) __attribute__((section(NAME), used))
 *
 * struct bpf_map_def {
 *     unsigned int type;
 *     unsigned int key_size;
 *     unsigned int value_size;
 *     unsigned int max_entries;
 *     unsigned int map_flags;
 *     unsigned int inner_map_idx;
 *     unsigned int numa_node;
 * };
 *
 * static void *(*bpf_map_lookup_elem)(void *map, void *key) =
 *     (void *) BPF_FUNC_map_lookup_elem;
 *
 * struct bpf_map_def SEC("maps") devices = {
 *     .type = BPF_MAP_TYPE_HASH,
 *     .key_size = sizeof(__u64),
 *     .value_size = sizeof(__u32),
 *     .max_entries = 65,
 * };
 *
 * SEC("cgroup/dev") int
 * bpf_libvirt_cgroup_device(struct bpf_cgroup_dev_ctx *ctx)
 * {
 *     __u64 key = ((__u64)ctx->major << 32) | ctx->minor;
 *     __u32 *val = 0;
 *
 *     val = bpf_map_lookup_elem(&devices, &key);
 *     if (val && (ctx->access_type & *val) == ctx->access_type)
 *         return 1;
 *
 *     key = ((__u64)ctx->major << 32) | 0xffffffff;
 *     val = bpf_map_lookup_elem(&devices, &key);
 *     if (val && (ctx->access_type & *val) == ctx->access_type)
 *         return 1;
 *
 *     key = 0xffffffff00000000 | ctx->minor;
 *     val = bpf_map_lookup_elem(&devices, &key);
 *     if (val && (ctx->access_type & *val) == ctx->access_type)
 *         return 1;
 *
 *     key = 0xffffffffffffffff;
 *     val = bpf_map_lookup_elem(&devices, &key);
 *     if (val && (ctx->access_type & *val) == ctx->access_type)
 *         return 1;
 *
 *     return 0;
 * }
 *
 * char _license[] SEC("license") = "GPL";
 * __u32 _version SEC("version") = LINUX_VERSION_CODE;
 * ----------------------------------------------------------------------------
 * */
static int
virCgroupV2DevicesLoadProg(int mapfd)
{
    struct bpf_insn prog[] = {
        /*  0:  r6 = r1 */
        VIR_BPF_MOV64_REG(BPF_REG_6, BPF_REG_1),
        /*  1:  r1 = *(u32 *)(r6 + 8) */
        VIR_BPF_LDX_MEM(BPF_W, BPF_REG_1, BPF_REG_6, 8),
        /*  2:  r2 = *(u32 *)(r6 + 4) */
        VIR_BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_6, 4),
        /*  3:  r2 <<= 32 */
        VIR_BPF_ALU64_IMM(BPF_LSH, BPF_REG_2, 32),
        /*  4:  r2 |= r1 */
        VIR_BPF_ALU64_REG(BPF_OR, BPF_REG_2, BPF_REG_1),
        /*  5:  *(u64 *)(r10 - 8) = r2 */
        VIR_BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_2, -8),
        /*  6:  r2 = r10 */
        VIR_BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
        /*  7:  r2 += -8 */
        VIR_BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -8),
        /*  8:  r1 = 0 ll */
        VIR_BPF_LD_MAP_FD(BPF_REG_1, mapfd),
        /* 10:  call 1 */
        VIR_BPF_CALL_INSN(BPF_FUNC_map_lookup_elem),
        /* 11:  r1 = r0 */
        VIR_BPF_MOV64_REG(BPF_REG_1, BPF_REG_0),
        /* 12:  if r1 == 0 goto +5 <LBB0_2> */
        VIR_BPF_JMP_IMM(BPF_JEQ, BPF_REG_1, 0, 5),
        /* 13:  r0 = 1 */
        VIR_BPF_MOV64_IMM(BPF_REG_0, 1),
        /* 14:  r2 = *(u32 *)(r6 + 0) */
        VIR_BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_6, 0),
        /* 15:  r1 = *(u32 *)(r1 + 0) */
        VIR_BPF_LDX_MEM(BPF_W, BPF_REG_1, BPF_REG_1, 0),
        /* 16:  r1 &= r2 */
        VIR_BPF_ALU64_REG(BPF_AND, BPF_REG_1, BPF_REG_2),
        /* 17:  if r1 == r2 goto +50 <LBB0_9> */
        VIR_BPF_JMP_REG(BPF_JEQ, BPF_REG_1, BPF_REG_2, 50),
        /* LBB0_2: */
        /* 18:  r1 = *(u32 *)(r6 + 4) */
        VIR_BPF_LDX_MEM(BPF_W, BPF_REG_1, BPF_REG_6, 4),
        /* 19:  r1 <<= 32 */
        VIR_BPF_ALU64_IMM(BPF_LSH, BPF_REG_1, 32),
        /* 20:  r2 = 4294967295 ll */
        VIR_BPF_LD_IMM64(BPF_REG_2, 0xffffffff),
        /* 22:  r1 |= r2 */
        VIR_BPF_ALU64_REG(BPF_OR, BPF_REG_1, BPF_REG_2),
        /* 23:  *(u64 *)(r10 - 8) = r1 */
        VIR_BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_1, -8),
        /* 24:  r2 = r10 */
        VIR_BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
        /* 25:  r2 += -8 */
        VIR_BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -8),
        /* 26:  r1 = 0 ll */
        VIR_BPF_LD_MAP_FD(BPF_REG_1, mapfd),
        /* 28:  call 1 */
        VIR_BPF_CALL_INSN(BPF_FUNC_map_lookup_elem),
        /* 29:  r1 = r0 */
        VIR_BPF_MOV64_REG(BPF_REG_1, BPF_REG_0),
        /* 30:  if r1 == 0 goto +5 <LBB0_4> */
        VIR_BPF_JMP_IMM(BPF_JEQ, BPF_REG_1, 0, 5),
        /* 31:  r0 = 1 */
        VIR_BPF_MOV64_IMM(BPF_REG_0, 1),
        /* 32:  r2 = *(u32 *)(r6 + 0) */
        VIR_BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_6, 0),
        /* 33:  r1 = *(u32 *)(r1 + 0) */
        VIR_BPF_LDX_MEM(BPF_W, BPF_REG_1, BPF_REG_1, 0),
        /* 34:  r1 &= r2 */
        VIR_BPF_ALU64_REG(BPF_AND, BPF_REG_1, BPF_REG_2),
        /* 35:  if r1 == r2 goto +32 <LBB0_9> */
        VIR_BPF_JMP_REG(BPF_JEQ, BPF_REG_1, BPF_REG_2, 32),
        /* LBB0_4: */
        /* 36:  r1 = *(u32 *)(r6 + 8) */
        VIR_BPF_LDX_MEM(BPF_W, BPF_REG_1, BPF_REG_6, 8),
        /* 37:  r2 = -4294967296 ll */
        VIR_BPF_LD_IMM64(BPF_REG_2, 0xffffffff00000000),
        /* 39:  r1 |= r2 */
        VIR_BPF_ALU64_REG(BPF_OR, BPF_REG_1, BPF_REG_2),
        /* 40:  *(u64 *)(r10 - 8) = r1 */
        VIR_BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_1, -8),
        /* 41:  r2 = r10 */
        VIR_BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
        /* 42:  r2 += -8 */
        VIR_BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -8),
        /* 43:  r1 = 0 ll */
        VIR_BPF_LD_MAP_FD(BPF_REG_1, mapfd),
        /* 45:  call 1 */
        VIR_BPF_CALL_INSN(BPF_FUNC_map_lookup_elem),
        /* 46:  r1 = r0 */
        VIR_BPF_MOV64_REG(BPF_REG_1, BPF_REG_0),
        /* 47:  if r1 == 0 goto +5 <LBB0_6> */
        VIR_BPF_JMP_IMM(BPF_JEQ, BPF_REG_1, 0, 5),
        /* 48:  r0 = 1 */
        VIR_BPF_MOV64_IMM(BPF_REG_0, 1),
        /* 49:  r2 = *(u32 *)(r6 + 0) */
        VIR_BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_6, 0),
        /* 50:  r1 = *(u32 *)(r1 + 0) */
        VIR_BPF_LDX_MEM(BPF_W, BPF_REG_1, BPF_REG_1, 0),
        /* 51:  r1 &= r2 */
        VIR_BPF_ALU64_REG(BPF_AND, BPF_REG_1, BPF_REG_2),
        /* 52:  if r1 == r2 goto +15 <LBB0_9> */
        VIR_BPF_JMP_REG(BPF_JEQ, BPF_REG_1, BPF_REG_2, 15),
        /* LBB0_6: */
        /* 53:  r1 = -1 */
        VIR_BPF_MOV64_IMM(BPF_REG_1, -1),
        /* 54:  *(u64 *)(r10 - 8) = r1 */
        VIR_BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_1, -8),
        /* 55:  r2 = r10 */
        VIR_BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
        /* 56:  r2 += -8 */
        VIR_BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -8),
        /* 57:  r1 = 0 ll */
        VIR_BPF_LD_MAP_FD(BPF_REG_1, mapfd),
        /* 59:  call 1 */
        VIR_BPF_CALL_INSN(BPF_FUNC_map_lookup_elem),
        /* 60:  r1 = r0 */
        VIR_BPF_MOV64_REG(BPF_REG_1, BPF_REG_0),
        /* 61:  if r1 == 0 goto +5 <LBB0_8> */
        VIR_BPF_JMP_IMM(BPF_JEQ, BPF_REG_1, 0, 5),
        /* 62:  r0 = 1 */
        VIR_BPF_MOV64_IMM(BPF_REG_0, 1),
        /* 63:  r2 = *(u32 *)(r6 + 0) */
        VIR_BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_6, 0),
        /* 64:  r1 = *(u32 *)(r1 + 0) */
        VIR_BPF_LDX_MEM(BPF_W, BPF_REG_1, BPF_REG_1, 0),
        /* 65:  r1 &= r2 */
        VIR_BPF_ALU64_REG(BPF_AND, BPF_REG_1, BPF_REG_2),
        /* 66:  if r1 == r2 goto +1 <LBB0_9> */
        VIR_BPF_JMP_REG(BPF_JEQ, BPF_REG_1, BPF_REG_2, 1),
        /* LBB0_8: */
        /* 67:  r0 = 0 */
        VIR_BPF_MOV64_IMM(BPF_REG_0, 0),
        /* LBB0_9: */
        /* 68:  exit */
        VIR_BPF_EXIT_INSN(),
    };

    return virBPFLoadProg(prog, BPF_PROG_TYPE_CGROUP_DEVICE, G_N_ELEMENTS(prog));
}


int
virCgroupV2DevicesAttachProg(virCgroupPtr group,
                             int mapfd,
                             size_t max)
{
    int ret = -1;
    VIR_AUTOCLOSE progfd = -1;
    VIR_AUTOCLOSE cgroupfd = -1;
    g_autofree char *path = NULL;

    if (virCgroupPathOfController(group, VIR_CGROUP_CONTROLLER_DEVICES,
                                  NULL, &path) < 0) {
        goto cleanup;
    }

    progfd = virCgroupV2DevicesLoadProg(mapfd);
    if (progfd < 0) {
        virReportSystemError(errno, "%s", _("failed to load cgroup BPF prog"));
        goto cleanup;
    }

    cgroupfd = open(path, O_RDONLY);
    if (cgroupfd < 0) {
        virReportSystemError(errno, _("unable to open '%s'"), path);
        goto cleanup;
    }

    if (virBPFAttachProg(progfd, cgroupfd, BPF_CGROUP_DEVICE) < 0) {
        virReportSystemError(errno, "%s", _("failed to attach cgroup BPF prog"));
        goto cleanup;
    }

    if (group->unified.devices.progfd > 0) {
        VIR_DEBUG("Closing existing program that was replaced by new one.");
        VIR_FORCE_CLOSE(group->unified.devices.progfd);
    }

    group->unified.devices.progfd = progfd;
    group->unified.devices.mapfd = mapfd;
    group->unified.devices.max = max;
    progfd = -1;
    mapfd = -1;

    ret = 0;
 cleanup:
    VIR_FORCE_CLOSE(mapfd);
    return ret;
}
#else /* !HAVE_DECL_BPF_CGROUP_DEVICE */
bool
virCgroupV2DevicesAvailable(virCgroupPtr group G_GNUC_UNUSED)
{
    return false;
}


int
virCgroupV2DevicesAttachProg(virCgroupPtr group G_GNUC_UNUSED,
                             int mapfd G_GNUC_UNUSED,
                             size_t max G_GNUC_UNUSED)
{
    virReportSystemError(ENOSYS, "%s",
                         _("cgroups v2 BPF devices not supported "
                           "with this kernel"));
    return -1;
}
#endif /* !HAVE_DECL_BPF_CGROUP_DEVICE */
