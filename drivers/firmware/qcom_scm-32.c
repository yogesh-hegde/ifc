/* Copyright (c) 2010,2015, The Linux Foundation. All rights reserved.
>>>>>>> firmware: qcom: scm: Split out 32-bit specific SCM code
 * Copyright (C) 2015 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include <linux/slab.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/qcom_scm.h>
#include <linux/dma-mapping.h>

#include <asm/cacheflush.h>

#include "qcom_scm.h"

#define QCOM_SCM_ENOMEM		-5
#define QCOM_SCM_EOPNOTSUPP	-4
#define QCOM_SCM_EINVAL_ADDR	-3
#define QCOM_SCM_EINVAL_ARG	-2
#define QCOM_SCM_ERROR		-1
#define QCOM_SCM_INTERRUPTED	1

#define QCOM_SCM_FLAG_COLDBOOT_CPU0	0x00
#define QCOM_SCM_FLAG_COLDBOOT_CPU1	0x01
#define QCOM_SCM_FLAG_COLDBOOT_CPU2	0x08
#define QCOM_SCM_FLAG_COLDBOOT_CPU3	0x20

#define QCOM_SCM_FLAG_WARMBOOT_CPU0	0x04
#define QCOM_SCM_FLAG_WARMBOOT_CPU1	0x02
#define QCOM_SCM_FLAG_WARMBOOT_CPU2	0x10
#define QCOM_SCM_FLAG_WARMBOOT_CPU3	0x40

#define IOMMU_SECURE_PTBL_SIZE		3
#define IOMMU_SECURE_PTBL_INIT		4
#define IOMMU_SET_CP_POOL_SIZE		5
#define IOMMU_SECURE_MAP		6
#define IOMMU_SECURE_UNMAP		7
#define IOMMU_SECURE_MAP2		0xb
#define IOMMU_SECURE_MAP2_FLAT		0x12
#define IOMMU_SECURE_UNMAP2		0xc

struct qcom_scm_entry {
	int flag;
	void *entry;
};

static struct qcom_scm_entry qcom_scm_wb[] = {
	{ .flag = QCOM_SCM_FLAG_WARMBOOT_CPU0 },
	{ .flag = QCOM_SCM_FLAG_WARMBOOT_CPU1 },
	{ .flag = QCOM_SCM_FLAG_WARMBOOT_CPU2 },
	{ .flag = QCOM_SCM_FLAG_WARMBOOT_CPU3 },
};

static DEFINE_MUTEX(qcom_scm_lock);

/**
 * struct qcom_scm_command - one SCM command buffer
 * @len: total available memory for command and response
 * @buf_offset: start of command buffer
 * @resp_hdr_offset: start of response buffer
 * @id: command to be executed
 * @buf: buffer returned from qcom_scm_get_command_buffer()
 *
 * An SCM command is laid out in memory as follows:
 *
 *	------------------- <--- struct qcom_scm_command
 *	| command header  |
 *	------------------- <--- qcom_scm_get_command_buffer()
 *	| command buffer  |
 *	------------------- <--- struct qcom_scm_response and
 *	| response header |      qcom_scm_command_to_response()
 *	------------------- <--- qcom_scm_get_response_buffer()
 *	| response buffer |
 *	-------------------
 *
 * There can be arbitrary padding between the headers and buffers so
 * you should always use the appropriate qcom_scm_get_*_buffer() routines
 * to access the buffers in a safe manner.
 */
struct qcom_scm_command {
	__le32 len;
	__le32 buf_offset;
	__le32 resp_hdr_offset;
	__le32 id;
	__le32 buf[0];
};

/**
 * struct qcom_scm_response - one SCM response buffer
 * @len: total available memory for response
 * @buf_offset: start of response data relative to start of qcom_scm_response
 * @is_complete: indicates if the command has finished processing
 */
struct qcom_scm_response {
	__le32 len;
	__le32 buf_offset;
	__le32 is_complete;
};

/**
 * alloc_qcom_scm_command() - Allocate an SCM command
 * @cmd_size: size of the command buffer
 * @resp_size: size of the response buffer
 *
 * Allocate an SCM command, including enough room for the command
 * and response headers as well as the command and response buffers.
 *
 * Returns a valid &qcom_scm_command on success or %NULL if the allocation fails.
 */
static struct qcom_scm_command *alloc_qcom_scm_command(size_t cmd_size, size_t resp_size)
{
	struct qcom_scm_command *cmd;
	size_t len = sizeof(*cmd) + sizeof(struct qcom_scm_response) + cmd_size +
		resp_size;
	u32 offset;

	cmd = kzalloc(PAGE_ALIGN(len), GFP_KERNEL);
	if (cmd) {
		cmd->len = cpu_to_le32(len);
		offset = offsetof(struct qcom_scm_command, buf);
		cmd->buf_offset = cpu_to_le32(offset);
		cmd->resp_hdr_offset = cpu_to_le32(offset + cmd_size);
	}
	return cmd;
}

/**
 * free_qcom_scm_command() - Free an SCM command
 * @cmd: command to free
 *
 * Free an SCM command.
 */
static inline void free_qcom_scm_command(struct qcom_scm_command *cmd)
{
	kfree(cmd);
}

/**
 * qcom_scm_command_to_response() - Get a pointer to a qcom_scm_response
 * @cmd: command
 *
 * Returns a pointer to a response for a command.
 */
static inline struct qcom_scm_response *qcom_scm_command_to_response(
		const struct qcom_scm_command *cmd)
{
	return (void *)cmd + le32_to_cpu(cmd->resp_hdr_offset);
}

/**
 * qcom_scm_get_command_buffer() - Get a pointer to a command buffer
 * @cmd: command
 *
 * Returns a pointer to the command buffer of a command.
 */
static inline void *qcom_scm_get_command_buffer(const struct qcom_scm_command *cmd)
{
	return (void *)cmd->buf;
}

/**
 * qcom_scm_get_response_buffer() - Get a pointer to a response buffer
 * @rsp: response
 *
 * Returns a pointer to a response buffer of a response.
 */
static inline void *qcom_scm_get_response_buffer(const struct qcom_scm_response *rsp)
{
	return (void *)rsp + le32_to_cpu(rsp->buf_offset);
}

static u32 smc(u32 cmd_addr)
{
	int context_id;
	register u32 r0 asm("r0") = 1;
	register u32 r1 asm("r1") = (u32)&context_id;
	register u32 r2 asm("r2") = cmd_addr;
	do {
		asm volatile(
			__asmeq("%0", "r0")
			__asmeq("%1", "r0")
			__asmeq("%2", "r1")
			__asmeq("%3", "r2")
#ifdef REQUIRES_SEC
			".arch_extension sec\n"
#endif
			"smc	#0	@ switch to secure world\n"
			: "=r" (r0)
			: "r" (r0), "r" (r1), "r" (r2)
			: "r3");
	} while (r0 == QCOM_SCM_INTERRUPTED);

	return r0;
}

static int __qcom_scm_call(const struct qcom_scm_command *cmd)
{
	int ret;
	u32 cmd_addr = virt_to_phys(cmd);

	/*
	 * Flush the command buffer so that the secure world sees
	 * the correct data.
	 */
	secure_flush_area(cmd, cmd->len);

	ret = smc(cmd_addr);
	if (ret < 0)
		ret = qcom_scm_remap_error(ret);

	return ret;
}

static void qcom_scm_inv_range(unsigned long start, unsigned long end)
{
	u32 cacheline_size, ctr;

	asm volatile("mrc p15, 0, %0, c0, c0, 1" : "=r" (ctr));
	cacheline_size = 4 << ((ctr >> 16) & 0xf);

	start = round_down(start, cacheline_size);
	end = round_up(end, cacheline_size);
	outer_inv_range(start, end);
	while (start < end) {
		asm ("mcr p15, 0, %0, c7, c6, 1" : : "r" (start)
		     : "memory");
		start += cacheline_size;
	}
	dsb();
	isb();
}

/**
 * qcom_scm_call() - Send an SCM command
 * @svc_id: service identifier
 * @cmd_id: command identifier
 * @cmd_buf: command buffer
 * @cmd_len: length of the command buffer
 * @resp_buf: response buffer
 * @resp_len: length of the response buffer
 *
 * Sends a command to the SCM and waits for the command to finish processing.
 *
 * A note on cache maintenance:
 * Note that any buffers that are expected to be accessed by the secure world
 * must be flushed before invoking qcom_scm_call and invalidated in the cache
 * immediately after qcom_scm_call returns. Cache maintenance on the command
 * and response buffers is taken care of by qcom_scm_call; however, callers are
 * responsible for any other cached buffers passed over to the secure world.
 */
static int qcom_scm_call(u32 svc_id, u32 cmd_id, const void *cmd_buf,
			size_t cmd_len, void *resp_buf, size_t resp_len)
{
	int ret;
	struct qcom_scm_command *cmd;
	struct qcom_scm_response *rsp;
	unsigned long start, end;

	cmd = alloc_qcom_scm_command(cmd_len, resp_len);
	if (!cmd)
		return -ENOMEM;

	cmd->id = cpu_to_le32((svc_id << 10) | cmd_id);
	if (cmd_buf)
		memcpy(qcom_scm_get_command_buffer(cmd), cmd_buf, cmd_len);

	mutex_lock(&qcom_scm_lock);
	ret = __qcom_scm_call(cmd);
	mutex_unlock(&qcom_scm_lock);
	if (ret)
		goto out;

	rsp = qcom_scm_command_to_response(cmd);
	start = (unsigned long)rsp;

	do {
		qcom_scm_inv_range(start, start + sizeof(*rsp));
	} while (!rsp->is_complete);

	end = (unsigned long)qcom_scm_get_response_buffer(rsp) + resp_len;
	qcom_scm_inv_range(start, end);

	if (resp_buf)
		memcpy(resp_buf, qcom_scm_get_response_buffer(rsp), resp_len);
out:
	free_qcom_scm_command(cmd);
	return ret;
}

#define SCM_CLASS_REGISTER	(0x2 << 8)
#define SCM_MASK_IRQS		BIT(5)
#define SCM_ATOMIC(svc, cmd, n) (((((svc) << 10)|((cmd) & 0x3ff)) << 12) | \
				SCM_CLASS_REGISTER | \
				SCM_MASK_IRQS | \
				(n & 0xf))

/**
 * qcom_scm_call_atomic1() - Send an atomic SCM command with one argument
 * @svc_id: service identifier
 * @cmd_id: command identifier
 * @arg1: first argument
 *
 * This shall only be used with commands that are guaranteed to be
 * uninterruptable, atomic and SMP safe.
 */
static s32 qcom_scm_call_atomic1(u32 svc, u32 cmd, u32 arg1)
{
	int context_id;

	register u32 r0 asm("r0") = SCM_ATOMIC(svc, cmd, 1);
	register u32 r1 asm("r1") = (u32)&context_id;
	register u32 r2 asm("r2") = arg1;

	asm volatile(
			__asmeq("%0", "r0")
			__asmeq("%1", "r0")
			__asmeq("%2", "r1")
			__asmeq("%3", "r2")
#ifdef REQUIRES_SEC
			".arch_extension sec\n"
#endif
			"smc    #0      @ switch to secure world\n"
			: "=r" (r0)
			: "r" (r0), "r" (r1), "r" (r2)
			: "r3");
	return r0;
}

u32 qcom_scm_get_version(void)
{
	int context_id;
	static u32 version = -1;
	register u32 r0 asm("r0");
	register u32 r1 asm("r1");

	if (version != -1)
		return version;

	mutex_lock(&qcom_scm_lock);

	r0 = 0x1 << 8;
	r1 = (u32)&context_id;
	do {
		asm volatile(
			__asmeq("%0", "r0")
			__asmeq("%1", "r1")
			__asmeq("%2", "r0")
			__asmeq("%3", "r1")
#ifdef REQUIRES_SEC
			".arch_extension sec\n"
#endif
			"smc	#0	@ switch to secure world\n"
			: "=r" (r0), "=r" (r1)
			: "r" (r0), "r" (r1)
			: "r2", "r3");
	} while (r0 == QCOM_SCM_INTERRUPTED);

	version = r1;
	mutex_unlock(&qcom_scm_lock);

	return version;
}
EXPORT_SYMBOL(qcom_scm_get_version);

/*
 * Set the cold/warm boot address for one of the CPU cores.
 */
static int qcom_scm_set_boot_addr(u32 addr, int flags)
{
	struct {
		__le32 flags;
		__le32 addr;
	} cmd;

	cmd.addr = cpu_to_le32(addr);
	cmd.flags = cpu_to_le32(flags);
	return qcom_scm_call(QCOM_SCM_SVC_BOOT, QCOM_SCM_BOOT_ADDR,
			&cmd, sizeof(cmd), NULL, 0);
}

/**
 * qcom_scm_set_cold_boot_addr() - Set the cold boot address for cpus
 * @entry: Entry point function for the cpus
 * @cpus: The cpumask of cpus that will use the entry point
 *
 * Set the cold boot address of the cpus. Any cpu outside the supported
 * range would be removed from the cpu present mask.
 */
int __qcom_scm_set_cold_boot_addr(void *entry, const cpumask_t *cpus)
{
	int flags = 0;
	int cpu;
	int scm_cb_flags[] = {
		QCOM_SCM_FLAG_COLDBOOT_CPU0,
		QCOM_SCM_FLAG_COLDBOOT_CPU1,
		QCOM_SCM_FLAG_COLDBOOT_CPU2,
		QCOM_SCM_FLAG_COLDBOOT_CPU3,
	};

	if (!cpus || (cpus && cpumask_empty(cpus)))
		return -EINVAL;

	for_each_cpu(cpu, cpus) {
		if (cpu < ARRAY_SIZE(scm_cb_flags))
			flags |= scm_cb_flags[cpu];
		else
			set_cpu_present(cpu, false);
	}

	return qcom_scm_set_boot_addr(virt_to_phys(entry), flags);
}

/**
 * qcom_scm_set_warm_boot_addr() - Set the warm boot address for cpus
 * @entry: Entry point function for the cpus
 * @cpus: The cpumask of cpus that will use the entry point
 *
 * Set the Linux entry point for the SCM to transfer control to when coming
 * out of a power down. CPU power down may be executed on cpuidle or hotplug.
 */
int __qcom_scm_set_warm_boot_addr(void *entry, const cpumask_t *cpus)
{
	int ret;
	int flags = 0;
	int cpu;

	/*
	 * Reassign only if we are switching from hotplug entry point
	 * to cpuidle entry point or vice versa.
	 */
	for_each_cpu(cpu, cpus) {
		if (entry == qcom_scm_wb[cpu].entry)
			continue;
		flags |= qcom_scm_wb[cpu].flag;
	}

	/* No change in entry function */
	if (!flags)
		return 0;

	ret = qcom_scm_set_boot_addr(virt_to_phys(entry), flags);
	if (!ret) {
		for_each_cpu(cpu, cpus)
			qcom_scm_wb[cpu].entry = entry;
	}

	return ret;
}

/**
 * qcom_scm_cpu_power_down() - Power down the cpu
 * @flags - Flags to flush cache
 *
 * This is an end point to power down cpu. If there was a pending interrupt,
 * the control would return from this function, otherwise, the cpu jumps to the
 * warm boot entry point set for this cpu upon reset.
 */
void __qcom_scm_cpu_power_down(u32 flags)
{
	qcom_scm_call_atomic1(QCOM_SCM_SVC_BOOT, QCOM_SCM_CMD_TERMINATE_PC,
			flags & QCOM_SCM_FLUSH_FLAG_MASK);
}

int __qcom_scm_is_call_available(u32 svc_id, u32 cmd_id)
{
	int ret;
	__le32 svc_cmd = cpu_to_le32((svc_id << 10) | cmd_id);
	__le32 ret_val = 0;

	ret = qcom_scm_call(QCOM_SCM_SVC_INFO, QCOM_IS_CALL_AVAIL_CMD, &svc_cmd,
			sizeof(svc_cmd), &ret_val, sizeof(ret_val));
	if (ret)
		return ret;

	return le32_to_cpu(ret_val);
}

int __qcom_scm_hdcp_req(struct qcom_scm_hdcp_req *req, u32 req_cnt, u32 *resp)
{
	if (req_cnt > QCOM_SCM_HDCP_MAX_REQ_CNT)
		return -ERANGE;

	return qcom_scm_call(QCOM_SCM_SVC_HDCP, QCOM_SCM_CMD_HDCP,
		req, req_cnt * sizeof(*req), resp, sizeof(*resp));
}

int __qcom_scm_pas_mss_reset(bool reset)
{
	__le32 val = cpu_to_le32(reset);
	__le32 resp;

	return qcom_scm_call(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_MSS_RESET,
				&val, sizeof(val),
			    	&resp, sizeof(resp));
}

bool __qcom_scm_pas_supported(u32 peripheral)
{
	u32 ret_val;
	int ret;

	ret = qcom_scm_call(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_IS_SUPPORTED_CMD,
			    &peripheral, sizeof(peripheral),
			    &ret_val, sizeof(ret_val));

	return ret ? false : !!ret_val;
}

int __qcom_scm_pas_init_image(u32 peripheral, dma_addr_t metadata_phys)
{
	__le32 scm_ret;
	int ret;
	struct {
		__le32 proc;
		__le32 image_addr;
	} request;

	request.proc = cpu_to_le32(peripheral);
	request.image_addr = cpu_to_le32(metadata_phys);

	ret = qcom_scm_call(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_INIT_IMAGE_CMD,
			    &request, sizeof(request),
			    &scm_ret, sizeof(scm_ret));

	return ret ? : le32_to_cpu(scm_ret);
}

int __qcom_scm_pas_mem_setup(u32 peripheral, phys_addr_t addr, phys_addr_t size)
{
	u32 scm_ret;
	int ret;
	struct pas_init_image_req {
		u32 proc;
		u32 addr;
		u32 len;
	} request;

	request.proc = peripheral;
	request.addr = addr;
	request.len = size;

	ret = qcom_scm_call(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_MEM_SETUP_CMD,
			    &request, sizeof(request),
			    &scm_ret, sizeof(scm_ret));

	return ret ? : scm_ret;
}

int __qcom_scm_pas_auth_and_reset(u32 peripheral)
{
	u32 scm_ret;
	int ret;

	ret = qcom_scm_call(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_AUTH_AND_RESET_CMD,
			    &peripheral, sizeof(peripheral),
			    &scm_ret, sizeof(scm_ret));

	return ret ? : scm_ret;
}

int __qcom_scm_pas_shutdown(u32 peripheral)
{
	u32 scm_ret;
	int ret;

	ret = qcom_scm_call(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_SHUTDOWN_CMD,
			    &peripheral, sizeof(peripheral),
			    &scm_ret, sizeof(scm_ret));

	return ret ? : scm_ret;
}


int __qcom_scm_pil_init_image_cmd(u32 proc, u64 image_addr)
{
	int ret;
	u32 scm_ret = 0;
	struct {
		u32 proc;
		u32 image_addr;
	} req;

	req.proc = proc;
	req.image_addr = image_addr;

	ret = qcom_scm_call(SCM_SVC_PIL, PAS_INIT_IMAGE_CMD, &req,
			    sizeof(req), &scm_ret, sizeof(scm_ret));
	if (ret)
		return ret;

	return scm_ret;
}

int __qcom_scm_pil_mem_setup_cmd(u32 proc, u64 start_addr, u32 len)
{
	u32 scm_ret = 0;
	int ret;
	struct {
		u32 proc;
		u32 start_addr;
		u32 len;
	} req;

	req.proc = proc;
	req.start_addr = start_addr;
	req.len = len;

	ret = qcom_scm_call(SCM_SVC_PIL, PAS_MEM_SETUP_CMD, &req,
			    sizeof(req), &scm_ret, sizeof(scm_ret));
	if (ret)
		return ret;

	return scm_ret;
}

int __qcom_scm_pil_auth_and_reset_cmd(u32 proc)
{
	u32 scm_ret = 0;
	int ret;
	u32 req;

	req = proc;

	ret = qcom_scm_call(SCM_SVC_PIL, PAS_AUTH_AND_RESET_CMD, &req,
			    sizeof(req), &scm_ret, sizeof(scm_ret));
	if (ret)
		return ret;

	return scm_ret;
}

int __qcom_scm_pil_shutdown_cmd(u32 proc)
{
	u32 scm_ret = 0;
	int ret;
	u32 req;

	req = proc;

	ret = qcom_scm_call(SCM_SVC_PIL, PAS_SHUTDOWN_CMD, &req,
			    sizeof(req), &scm_ret, sizeof(scm_ret));
	if (ret)
		return ret;

	return scm_ret;
}

#define SCM_SVC_UTIL			0x3
#define SCM_SVC_MP			0xc
#define IOMMU_DUMP_SMMU_FAULT_REGS	0x0c

int __qcom_scm_iommu_dump_fault_regs(u32 id, u32 context, u64 addr, u32 len)
{
	struct {
		u32 id;
		u32 cb_num;
		u32 buff;
		u32 len;
	} req;
	int resp = 0;

	return qcom_scm_call(SCM_SVC_UTIL, IOMMU_DUMP_SMMU_FAULT_REGS,
		       &req, sizeof(req), &resp, 1);
}

int __qcom_scm_iommu_set_cp_pool_size(u32 size, u32 spare)
{
	struct {
		u32 size;
		u32 spare;
	} req;
	int retval;

	req.size = size;
	req.spare = spare;

	return qcom_scm_call(SCM_SVC_MP, IOMMU_SET_CP_POOL_SIZE,
			     &req, sizeof(req), &retval, sizeof(retval));
}

int __qcom_scm_iommu_secure_ptbl_size(u32 spare, int psize[2])
{
	struct {
		u32 spare;
	} req;

	req.spare = spare;

	return qcom_scm_call(SCM_SVC_MP, IOMMU_SECURE_PTBL_SIZE, &req,
			     sizeof(req), psize, sizeof(psize));
}

int __qcom_scm_iommu_secure_ptbl_init(u64 addr, u32 size, u32 spare)
{
	struct {
		u32 addr;
		u32 size;
		u32 spare;
	} req = {0};
	int ret, ptbl_ret = 0;

	req.addr = addr;
	req.size = size;
	req.spare = spare;

	ret = qcom_scm_call(SCM_SVC_MP, IOMMU_SECURE_PTBL_INIT, &req,
			    sizeof(req), &ptbl_ret, sizeof(ptbl_ret));

	if (ret)
		return ret;

	if (ptbl_ret)
		return ptbl_ret;

	return 0;
}

int __qcom_scm_iommu_secure_map(u64 list, u32 list_size, u32 size,
				u32 id, u32 ctx_id, u64 va, u32 info_size,
				u32 flags)
{
	struct {
		struct {
			unsigned int list;
			unsigned int list_size;
			unsigned int size;
		} plist;
		struct {
			unsigned int id;
			unsigned int ctx_id;
			unsigned int va;
			unsigned int size;
		} info;
		unsigned int flags;
	} req;
	u32 resp;
	int ret;

	req.plist.list = list;
	req.plist.list_size = list_size;
	req.plist.size = size;
	req.info.id = id;
	req.info.ctx_id = ctx_id;
	req.info.va = va;
	req.info.size = info_size;
	req.flags = flags;

	ret = qcom_scm_call(SCM_SVC_MP, IOMMU_SECURE_MAP2, &req, sizeof(req),
			    &resp, sizeof(resp));

	if (ret || resp)
		return -EINVAL;

	return 0;
}

int __qcom_scm_iommu_secure_unmap(u32 id, u32 ctx_id, u64 va,
				  u32 size, u32 flags)
{
	struct {
		struct {
			unsigned int id;
			unsigned int ctx_id;
			unsigned int va;
			unsigned int size;
		} info;
		unsigned int flags;
	} req;
	int ret, scm_ret;

	req.info.id = id;
	req.info.ctx_id = ctx_id;
	req.info.va = va;
	req.info.size = size;
	req.flags = flags;

	return qcom_scm_call(SCM_SVC_MP, IOMMU_SECURE_UNMAP2, &req,
			     sizeof(req), &scm_ret, sizeof(scm_ret));
}

int __qcom_scm_get_feat_version(u32 feat)
{
	int ret;

	if (__qcom_scm_is_call_available(SCM_SVC_INFO, GET_FEAT_VERSION_CMD)) {
		u32 version;

		if (!qcom_scm_call(SCM_SVC_INFO, GET_FEAT_VERSION_CMD, &feat,
				   sizeof(feat), &version, sizeof(version)))
			return version;
	}

	return 0;
}

#define RESTORE_SEC_CFG		2
int __qcom_scm_restore_sec_cfg(u32 device_id, u32 spare)
{
	struct {
		u32 device_id;
		u32 spare;
	} req;
	int ret, scm_ret = 0;

	req.device_id = device_id;
	req.spare = spare;

	ret = qcom_scm_call(SCM_SVC_MP, RESTORE_SEC_CFG, &req, sizeof(req),
			    scm_ret, sizeof(scm_ret));
	if (ret || scm_ret)
		return ret ? ret : -EINVAL;

	return 0;
}

#define TZBSP_VIDEO_SET_STATE	0xa
int __qcom_scm_set_video_state(u32 state, u32 spare)
{
	struct {
		u32 state;
		u32 spare;
	} req;
	int scm_ret = 0;
	int ret;

	req.state = state;
	req.spare = spare;

	ret = qcom_scm_call(SCM_SVC_BOOT, TZBSP_VIDEO_SET_STATE, &req,
			    sizeof(req), &scm_ret, sizeof(scm_ret));
	if (ret || scm_ret)
			return ret ? ret : -EINVAL;

	return 0;
}

#define TZBSP_MEM_PROTECT_VIDEO_VAR	0x8

int __qcom_scm_mem_protect_video_var(u32 start, u32 size, u32 nonpixel_start,
				     u32 nonpixel_size)
{
	struct {
		u32 cp_start;
		u32 cp_size;
		u32 cp_nonpixel_start;
		u32 cp_nonpixel_size;
	} req;
	int ret, scm_ret;

	req.cp_start = start;
	req.cp_size = size;
	req.cp_nonpixel_start = nonpixel_start;
	req.cp_nonpixel_size = nonpixel_size;

	ret = qcom_scm_call(SCM_SVC_MP, TZBSP_MEM_PROTECT_VIDEO_VAR, &req,
			    sizeof(req), &scm_ret, sizeof(scm_ret));

	if (ret || scm_ret)
			return ret ? ret : -EINVAL;

	return 0;
}

int __qcom_scm_init(void)
{
	return 0;
}
