/*
 * Copyright (c) 2013-2015, ARM Limited and Contributors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of ARM nor the names of its contributors may be used
 * to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Top-level SMC handler for ZynqMP power management calls and
 * IPI setup functions for communication with PMU.
 */

#include <errno.h>
#include <gic_common.h>
#include <runtime_svc.h>
#include <string.h>
#include "pm_api_sys.h"
#include "pm_client.h"
#include "pm_ipi.h"
#include "../zynqmp_private.h"

#if ZYNQMP_WARM_RESTART
#include <arch_helpers.h>
#include <platform.h>
#include <spinlock.h>
#include <mmio.h>
#endif

#define PM_GET_CALLBACK_DATA	0xa01
#define PM_SET_SUSPEND_MODE	0xa02

/* 0 - UP, !0 - DOWN */
static int32_t pm_down = !0;
#if ZYNQMP_WARM_RESTART
static spinlock_t inc_lock;
static int active_cores = 0;
#endif
/**
 * pm_context - Structure which contains data for power management
 * @api_version		version of PM API, must match with one on PMU side
 * @payload		payload array used to store received
 *			data from ipi buffer registers
 */
static struct {
	uint32_t api_version;
	uint32_t payload[PAYLOAD_ARG_CNT];
} pm_ctx;

#if ZYNQMP_WARM_RESTART
/**
 * ipi_fiq_handler() - IPI Handler for PM-API callbacks
 * @buf:	Pointer to a structure holding the IPI data
 *
 * Function registered as INTR_TYPE_EL3 interrupt handler
 *
 * PMU sends IPI interrupts for PM-API callbacks.
 * This handler reads data from payload buffers and
 * based on read data decodes type of callback and call proper function.
 *
 * In presence of non-secure software layers (EL1/2) sets the interrupt
 * at registered entrance in GIC and informs that PMU responsed or demands
 * action
 */
static uint32_t ipi_fiq_handler(void)
{
	uint32_t handled = 0;
	uint32_t core_count = 0;
	uint32_t core_status[3];
	uint32_t target_cpu_list = 0;
	int i;

	for(i=0; i< 4; i++)
	{
		pm_get_node_status(NODE_APU_0 + i, core_status);
		if (core_status[0] == 1)
		{
			core_count ++;
			target_cpu_list |= (1 << i);
		}
	}

	spin_lock(&inc_lock);
	active_cores = core_count;
	spin_unlock(&inc_lock);

	INFO("Active Cores: %d\n", active_cores);

	/* trigger SGI to active cores */
	plat_ic_trigger_sgi(ARM_IRQ_SEC_SGI_7, target_cpu_list);

	handled |= IPI_APU_IXR_PMU_1_MASK;

	return handled;
}

static uint64_t __unused __dead2 zynqmp_sgi7_irq(uint32_t id, uint32_t flags,
						 void *handle, void *cookie)
{
	int i;
	/* enter wfi and stay there */
	INFO("Entering wfi\n");

	spin_lock(&inc_lock);
	active_cores--;
	spin_unlock(&inc_lock);

	for (i = 0; i < 4; i++) {
		mmio_write_32(0xF9010000 + 0xF10 + 4 * i, 0xffffffff); // ICENABLE
	}

	dsb();

	if (active_cores == 0) {
		pm_system_shutdown(PMF_SHUTDOWN_TYPE_RESET,
					PMF_SHUTDOWN_SUBTYPE_SUBSYSTEM);
	}

	/* enter wfi and stay there */
	while (1)
		wfi();
}
#endif

/**
 * pm_setup() - PM service setup
 *
 * @return	On success, the initialization function must return 0.
 *		Any other return value will cause the framework to ignore
 *		the service
 *
 * Initialization functions for ZynqMP power management for
 * communicaton with PMU.
 *
 * Called from sip_svc_setup initialization function with the
 * rt_svc_init signature.
 */
int pm_setup(void)
{
	int status;

	if (!zynqmp_is_pmu_up())
		return -ENODEV;

#if !ZYNQMP_WARM_RESTART
	status = pm_ipi_init();
#else
	status = pm_ipi_init(ipi_fiq_handler);
	if (status)
		goto err;

	/* register IRQ handler for SGI7 */
	status = request_intr_type_el3(ARM_IRQ_SEC_SGI_7, zynqmp_sgi7_irq);
	if (status)
		WARN("BL31: registering SGI7 interrupt failed\n");

err:
#endif

	if (status == 0)
		INFO("BL31: PM Service Init Complete: API v%d.%d\n",
		     PM_VERSION_MAJOR, PM_VERSION_MINOR);
	else
		INFO("BL31: PM Service Init Failed, Error Code %d!\n", status);

	pm_down = status;

	return status;
}

/**
 * pm_smc_handler() - SMC handler for PM-API calls coming from EL1/EL2.
 * @smc_fid - Function Identifier
 * @x1 - x4 - Arguments
 * @cookie  - Unused
 * @handler - Pointer to caller's context structure
 *
 * @return  - Unused
 *
 * Determines that smc_fid is valid and supported PM SMC Function ID from the
 * list of pm_api_ids, otherwise completes the request with
 * the unknown SMC Function ID
 *
 * The SMC calls for PM service are forwarded from SIP Service SMC handler
 * function with rt_svc_handle signature
 */
uint64_t pm_smc_handler(uint32_t smc_fid, uint64_t x1, uint64_t x2, uint64_t x3,
			uint64_t x4, void *cookie, void *handle, uint64_t flags)
{
	enum pm_ret_status ret;

	uint32_t pm_arg[4];

	/* Handle case where PM wasn't initialized properly */
	if (pm_down)
		SMC_RET1(handle, SMC_UNK);

	pm_arg[0] = (uint32_t)x1;
	pm_arg[1] = (uint32_t)(x1 >> 32);
	pm_arg[2] = (uint32_t)x2;
	pm_arg[3] = (uint32_t)(x2 >> 32);

	switch (smc_fid & FUNCID_NUM_MASK) {
	/* PM API Functions */
	case PM_SELF_SUSPEND:
		ret = pm_self_suspend(pm_arg[0], pm_arg[1], pm_arg[2],
				      pm_arg[3]);
		SMC_RET1(handle, (uint64_t)ret);

	case PM_REQ_SUSPEND:
		ret = pm_req_suspend(pm_arg[0], pm_arg[1], pm_arg[2],
				     pm_arg[3]);
		SMC_RET1(handle, (uint64_t)ret);

	case PM_REQ_WAKEUP:
	{
		/* Use address flag is encoded in the 1st bit of the low-word */
		unsigned int set_addr = pm_arg[1] & 0x1;
		uint64_t address = (uint64_t)pm_arg[2] << 32;

		address |= pm_arg[1] & (~0x1);
		ret = pm_req_wakeup(pm_arg[0], set_addr, address,
				    pm_arg[3]);
		SMC_RET1(handle, (uint64_t)ret);
	}

	case PM_FORCE_POWERDOWN:
		ret = pm_force_powerdown(pm_arg[0], pm_arg[1]);
		SMC_RET1(handle, (uint64_t)ret);

	case PM_ABORT_SUSPEND:
		ret = pm_abort_suspend(pm_arg[0]);
		SMC_RET1(handle, (uint64_t)ret);

	case PM_SET_WAKEUP_SOURCE:
		ret = pm_set_wakeup_source(pm_arg[0], pm_arg[1], pm_arg[2]);
		SMC_RET1(handle, (uint64_t)ret);

	case PM_SYSTEM_SHUTDOWN:
		ret = pm_system_shutdown(pm_arg[0], pm_arg[1]);
		SMC_RET1(handle, (uint64_t)ret);

	case PM_REQ_NODE:
		ret = pm_req_node(pm_arg[0], pm_arg[1], pm_arg[2], pm_arg[3]);
		SMC_RET1(handle, (uint64_t)ret);

	case PM_RELEASE_NODE:
		ret = pm_release_node(pm_arg[0]);
		SMC_RET1(handle, (uint64_t)ret);

	case PM_SET_REQUIREMENT:
		ret = pm_set_requirement(pm_arg[0], pm_arg[1], pm_arg[2],
					 pm_arg[3]);
		SMC_RET1(handle, (uint64_t)ret);

	case PM_SET_MAX_LATENCY:
		ret = pm_set_max_latency(pm_arg[0], pm_arg[1]);
		SMC_RET1(handle, (uint64_t)ret);

	case PM_GET_API_VERSION:
		/* Check is PM API version already verified */
		if (pm_ctx.api_version == PM_VERSION) {
			SMC_RET1(handle, (uint64_t)PM_RET_SUCCESS |
				 ((uint64_t)PM_VERSION << 32));
		}

		ret = pm_get_api_version(&pm_ctx.api_version);
		/*
		 * Enable IPI IRQ
		 * assume the rich OS is OK to handle callback IRQs now.
		 * Even if we were wrong, it would not enable the IRQ in
		 * the GIC.
		 */
		pm_ipi_irq_enable();
		SMC_RET1(handle, (uint64_t)ret |
			 ((uint64_t)pm_ctx.api_version << 32));

	case PM_SET_CONFIGURATION:
		ret = pm_set_configuration(pm_arg[0]);
		SMC_RET1(handle, (uint64_t)ret);

	case PM_INIT_FINALIZE:
		ret = pm_init_finalize();
		SMC_RET1(handle, (uint64_t)ret);

	case PM_GET_NODE_STATUS:
	{
		uint32_t buff[3];

		ret = pm_get_node_status(pm_arg[0], buff);
		SMC_RET2(handle, (uint64_t)ret | ((uint64_t)buff[0] << 32),
			(uint64_t)buff[1] | ((uint64_t)buff[2] << 32));
	}

	case PM_GET_OP_CHARACTERISTIC:
	{
		uint32_t result;

		ret = pm_get_op_characteristic(pm_arg[0], pm_arg[1], &result);
		SMC_RET1(handle, (uint64_t)ret | ((uint64_t)result << 32));
	}

	case PM_REGISTER_NOTIFIER:
		ret = pm_register_notifier(pm_arg[0], pm_arg[1], pm_arg[2],
					   pm_arg[3]);
		SMC_RET1(handle, (uint64_t)ret);

	case PM_RESET_ASSERT:
		ret = pm_reset_assert(pm_arg[0], pm_arg[1]);
		SMC_RET1(handle, (uint64_t)ret);

	case PM_RESET_GET_STATUS:
	{
		uint32_t reset_status;

		ret = pm_reset_get_status(pm_arg[0], &reset_status);
		SMC_RET1(handle, (uint64_t)ret |
			 ((uint64_t)reset_status << 32));
	}

	/* PM memory access functions */
	case PM_MMIO_WRITE:
		ret = pm_mmio_write(pm_arg[0], pm_arg[1], pm_arg[2]);
		SMC_RET1(handle, (uint64_t)ret);

	case PM_MMIO_READ:
	{
		uint32_t value;

		ret = pm_mmio_read(pm_arg[0], &value);
		SMC_RET1(handle, (uint64_t)ret | ((uint64_t)value) << 32);
	}

	case PM_FPGA_LOAD:
		ret = pm_fpga_load(pm_arg[0], pm_arg[1], pm_arg[2], pm_arg[3]);
		SMC_RET1(handle, (uint64_t)ret);

	case PM_FPGA_GET_STATUS:
	{
		uint32_t value;

		ret = pm_fpga_get_status(&value);
		SMC_RET1(handle, (uint64_t)ret | ((uint64_t)value) << 32);
	}

	case PM_GET_CHIPID:
	{
		uint32_t result[2];

		ret = pm_get_chipid(result);
		SMC_RET2(handle, (uint64_t)ret | ((uint64_t)result[0] << 32),
			 result[1]);
	}

	case PM_SECURE_RSA_AES:
		ret = pm_secure_rsaaes(pm_arg[0], pm_arg[1], pm_arg[2],
				       pm_arg[3]);
		SMC_RET1(handle, (uint64_t)ret);

	case PM_GET_CALLBACK_DATA:
	{
		uint32_t result[4];

		pm_get_callbackdata(result, sizeof(result));
		SMC_RET2(handle,
			 (uint64_t)result[0] | ((uint64_t)result[1] << 32),
			 (uint64_t)result[2] | ((uint64_t)result[3] << 32));
	}

	case PM_SET_SUSPEND_MODE:
		ret = pm_set_suspend_mode(pm_arg[0]);
		SMC_RET1(handle, (uint64_t)ret);

	default:
		WARN("Unimplemented PM Service Call: 0x%x\n", smc_fid);
		SMC_RET1(handle, SMC_UNK);
	}
}
