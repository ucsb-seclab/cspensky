// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2015-2016, Linaro Limited
 * All rights reserved.
 * Copyright (c) 2014, STMicroelectronics International N.V.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
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

#include <assert.h>
#include <bench.h>
#include <compiler.h>
#include <initcall.h>
#include <kernel/linker.h>
#include <kernel/msg_param.h>
#include <kernel/panic.h>
#include <kernel/tee_misc.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <mm/mobj.h>
#include <optee_msg.h>
#include <sm/optee_smc.h>
#include <string.h>
#include <tee/entry_std.h>
#include <tee/tee_cryp_utl.h>
#include <tee/uuid.h>
#include <util.h>
#include <trustio_test.h>
#include <aes.h>
#include <kernel/thread.h>
#include <kernel/tee_time.h>
#include <optee_msg_supplicant.h>

#define SHM_CACHE_ATTRS	\
	(uint32_t)(core_mmu_is_shm_cached() ?  OPTEE_SMC_SHM_CACHED : 0)

/* Sessions opened from normal world */
static struct tee_ta_session_head tee_open_sessions =
TAILQ_HEAD_INITIALIZER(tee_open_sessions);

static struct mobj *shm_mobj;
#ifdef CFG_SECURE_DATA_PATH
static struct mobj **sdp_mem_mobjs;
#endif

long read_from_trustio_pin(void);

static bool param_mem_from_mobj(struct param_mem *mem, struct mobj *mobj,
				const paddr_t pa, const size_t sz)
{
	paddr_t b;

	if (mobj_get_pa(mobj, 0, 0, &b) != TEE_SUCCESS)
		panic("mobj_get_pa failed");

	if (!core_is_buffer_inside(pa, MAX(sz, 1UL), b, mobj->size))
		return false;

	mem->mobj = mobj;
	mem->offs = pa - b;
	mem->size = sz;
	return true;
}

/* fill 'struct param_mem' structure if buffer matches a valid memory object */
static TEE_Result assign_mobj_to_param_mem(const paddr_t pa, const size_t sz,
					   uint32_t attr, uint64_t shm_ref,
					   struct param_mem *mem)
{
	struct mobj __maybe_unused **mobj;

	/* NULL Memory Rerefence? */
	if (!pa && !sz) {
		mem->mobj = NULL;
		mem->offs = 0;
		mem->size = 0;
		return TEE_SUCCESS;
	}

	/* Non-contigous buffer from non sec DDR? */
	if (attr & OPTEE_MSG_ATTR_NONCONTIG) {
		mem->mobj = msg_param_mobj_from_noncontig(pa, sz, shm_ref,
							  false);
		if (!mem->mobj)
			return TEE_ERROR_BAD_PARAMETERS;
		mem->offs = pa & SMALL_PAGE_MASK;
		mem->size = sz;
		return TEE_SUCCESS;
	}

	/* Belongs to nonsecure shared memory? */
	if (param_mem_from_mobj(mem, shm_mobj, pa, sz))
		return TEE_SUCCESS;

#ifdef CFG_SECURE_DATA_PATH
	/* Belongs to SDP memories? */
	for (mobj = sdp_mem_mobjs; *mobj; mobj++)
		if (param_mem_from_mobj(mem, *mobj, pa, sz))
			return TEE_SUCCESS;
#endif

	return TEE_ERROR_BAD_PARAMETERS;
}

static TEE_Result set_rmem_param(const struct optee_msg_param *param,
				 struct param_mem *mem)
{
	mem->mobj = mobj_reg_shm_find_by_cookie(param->u.rmem.shm_ref);
	if (!mem->mobj)
		return TEE_ERROR_BAD_PARAMETERS;
	mem->offs = param->u.rmem.offs;
	mem->size = param->u.rmem.size;

	return TEE_SUCCESS;
}

static TEE_Result copy_in_params(const struct optee_msg_param *params,
				 uint32_t num_params,
				 struct tee_ta_param *ta_param,
				 uint64_t *saved_attr)
{
	TEE_Result res;
	size_t n;
	uint8_t pt[TEE_NUM_PARAMS];

	if (num_params > TEE_NUM_PARAMS)
		return TEE_ERROR_BAD_PARAMETERS;

	memset(ta_param, 0, sizeof(*ta_param));

	for (n = 0; n < num_params; n++) {
		uint32_t attr;
		saved_attr[n] = params[n].attr;

		if (saved_attr[n] & OPTEE_MSG_ATTR_META)
			return TEE_ERROR_BAD_PARAMETERS;

		attr = saved_attr[n] & OPTEE_MSG_ATTR_TYPE_MASK;
		switch (attr) {
		case OPTEE_MSG_ATTR_TYPE_NONE:
			pt[n] = TEE_PARAM_TYPE_NONE;
			memset(&ta_param->u[n], 0, sizeof(ta_param->u[n]));
			break;
		case OPTEE_MSG_ATTR_TYPE_VALUE_INPUT:
		case OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT:
		case OPTEE_MSG_ATTR_TYPE_VALUE_INOUT:
			pt[n] = TEE_PARAM_TYPE_VALUE_INPUT + attr -
				OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
			ta_param->u[n].val.a = params[n].u.value.a;
			ta_param->u[n].val.b = params[n].u.value.b;
			break;
		case OPTEE_MSG_ATTR_TYPE_TMEM_INPUT:
		case OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT:
		case OPTEE_MSG_ATTR_TYPE_TMEM_INOUT:
			pt[n] = TEE_PARAM_TYPE_MEMREF_INPUT + attr -
				OPTEE_MSG_ATTR_TYPE_TMEM_INPUT;
			res = assign_mobj_to_param_mem(params[n].u.tmem.buf_ptr,
						       params[n].u.tmem.size,
						       saved_attr[n],
						       params[n].u.tmem.shm_ref,
						       &ta_param->u[n].mem);
			if (res != TEE_SUCCESS)
				return res;
			break;
		case OPTEE_MSG_ATTR_TYPE_RMEM_INPUT:
		case OPTEE_MSG_ATTR_TYPE_RMEM_OUTPUT:
		case OPTEE_MSG_ATTR_TYPE_RMEM_INOUT:
			pt[n] = TEE_PARAM_TYPE_MEMREF_INPUT + attr -
				OPTEE_MSG_ATTR_TYPE_RMEM_INPUT;

			res = set_rmem_param(params + n, &ta_param->u[n].mem);
			if (res != TEE_SUCCESS)
				return res;
			break;
		default:
			return TEE_ERROR_BAD_PARAMETERS;
		}
	}

	ta_param->types = TEE_PARAM_TYPES(pt[0], pt[1], pt[2], pt[3]);

	return TEE_SUCCESS;
}

static void cleanup_params(const struct optee_msg_param *params,
			   const uint64_t *saved_attr,
			   uint32_t num_params)
{
	size_t n;

	for (n = 0; n < num_params; n++)
		if (msg_param_attr_is_tmem(saved_attr[n]) &&
		    saved_attr[n] & OPTEE_MSG_ATTR_NONCONTIG)
			mobj_free(mobj_reg_shm_find_by_cookie(
					  params[n].u.tmem.shm_ref));
}

static void copy_out_param(struct tee_ta_param *ta_param, uint32_t num_params,
			   struct optee_msg_param *params, uint64_t *saved_attr)
{
	size_t n;

	for (n = 0; n < num_params; n++) {
		switch (TEE_PARAM_TYPE_GET(ta_param->types, n)) {
		case TEE_PARAM_TYPE_MEMREF_OUTPUT:
		case TEE_PARAM_TYPE_MEMREF_INOUT:
			switch (saved_attr[n] & OPTEE_MSG_ATTR_TYPE_MASK) {
			case OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT:
			case OPTEE_MSG_ATTR_TYPE_TMEM_INOUT:
				params[n].u.tmem.size = ta_param->u[n].mem.size;
				break;
			case OPTEE_MSG_ATTR_TYPE_RMEM_OUTPUT:
			case OPTEE_MSG_ATTR_TYPE_RMEM_INOUT:
				params[n].u.rmem.size = ta_param->u[n].mem.size;
				break;
			default:
				break;
			}
			break;
		case TEE_PARAM_TYPE_VALUE_OUTPUT:
		case TEE_PARAM_TYPE_VALUE_INOUT:
			params[n].u.value.a = ta_param->u[n].val.a;
			params[n].u.value.b = ta_param->u[n].val.b;
			break;
		default:
			break;
		}
	}
}

/*
 * Extracts mandatory parameter for open session.
 *
 * Returns
 * false : mandatory parameter wasn't found or malformatted
 * true  : paramater found and OK
 */
static TEE_Result get_open_session_meta(size_t num_params,
					struct optee_msg_param *params,
					size_t *num_meta, TEE_UUID *uuid,
					TEE_Identity *clnt_id)
{
	const uint32_t req_attr = OPTEE_MSG_ATTR_META |
				  OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;

	if (num_params < 2)
		return TEE_ERROR_BAD_PARAMETERS;

	if (params[0].attr != req_attr || params[1].attr != req_attr)
		return TEE_ERROR_BAD_PARAMETERS;

	tee_uuid_from_octets(uuid, (void *)&params[0].u.value);
	clnt_id->login = params[1].u.value.c;
	switch (clnt_id->login) {
	case TEE_LOGIN_PUBLIC:
		memset(&clnt_id->uuid, 0, sizeof(clnt_id->uuid));
		break;
	case TEE_LOGIN_USER:
	case TEE_LOGIN_GROUP:
	case TEE_LOGIN_APPLICATION:
	case TEE_LOGIN_APPLICATION_USER:
	case TEE_LOGIN_APPLICATION_GROUP:
		tee_uuid_from_octets(&clnt_id->uuid,
				     (void *)&params[1].u.value);
		break;
	default:
		return TEE_ERROR_BAD_PARAMETERS;
	}

	*num_meta = 2;
	return TEE_SUCCESS;
}

static void entry_open_session(struct thread_smc_args *smc_args,
			       struct optee_msg_arg *arg, uint32_t num_params)
{
	TEE_Result res;
	TEE_ErrorOrigin err_orig = TEE_ORIGIN_TEE;
	struct tee_ta_session *s = NULL;
	TEE_Identity clnt_id;
	TEE_UUID uuid;
	struct tee_ta_param param;
	size_t num_meta;
	uint64_t saved_attr[TEE_NUM_PARAMS];

	res = get_open_session_meta(num_params, arg->params, &num_meta, &uuid,
				    &clnt_id);
	if (res != TEE_SUCCESS)
		goto out;

	res = copy_in_params(arg->params + num_meta, num_params - num_meta,
			     &param, saved_attr);
	if (res != TEE_SUCCESS)
		goto cleanup_params;

	res = tee_ta_open_session(&err_orig, &s, &tee_open_sessions, &uuid,
				  &clnt_id, TEE_TIMEOUT_INFINITE, &param);
	if (res != TEE_SUCCESS)
		s = NULL;
	copy_out_param(&param, num_params - num_meta, arg->params + num_meta,
		       saved_attr);

	/*
	 * The occurrence of open/close session command is usually
	 * un-predictable, using this property to increase randomness
	 * of prng
	 */
	plat_prng_add_jitter_entropy();

cleanup_params:
	cleanup_params(arg->params + num_meta, saved_attr,
		       num_params - num_meta);

out:
	if (s)
		arg->session = (vaddr_t)s;
	else
		arg->session = 0;
	arg->ret = res;
	arg->ret_origin = err_orig;
	smc_args->a0 = OPTEE_SMC_RETURN_OK;
}


static void trustio_handle_challenge_req(struct thread_smc_args __maybe_unused *smc_args, struct optee_msg_arg __maybe_unused *arg, uint32_t __maybe_unused num_params) {
	// get the target value		       
    //long target_val = arg->params[0].u.value.a;
    
    //trust_io_hash = smc_args->a5;
    //uint32_t encVal = target_val;
    TEE_Result res;
    struct optee_msg_param params;
    struct mobj *mobj;
    uint64_t c = 0;
    char *va;
    struct AES_ctx ctx;
    uint32_t tarretval, tarVal = 0x453;
    
    
    uint8_t iv[]  = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
    uint8_t key[] = { 0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c };
    
    
    mobj = thread_rpc_alloc_payload(128, &c);
    //TEE_Time old_time, new_time;
    // Do the RPC.
    va = mobj_get_va(mobj, 0);
    
    //*(uint32_t*)(va + 12) = tarVal;
    memcpy(va + 12, &tarVal, sizeof(tarVal));
    
    //DMSG("TRUSTIO Read Before Encryption: 0x%x, 0x%x, 0x%x, 0x%x\n", (int)(*(va + 12)), (int)(*(va + 13)), (int)(*(va + 14)), (int)(*(va + 15)));
    
    AES_init_ctx_iv(&ctx, key, iv);
    
    AES_CBC_encrypt_buffer(&ctx, (uint8_t *)va, 16);
    
    //DMSG("TRUSTIO Read After Encryption: 0x%x, 0x%x, 0x%x, 0x%x\n", (int)(*(va + 12)), (int)(*(va + 13)), (int)(*(va + 14)), (int)(*(va + 15)));
    
    memset(&params, 0, sizeof(params));
    params.attr = OPTEE_MSG_ATTR_TYPE_VALUE_INOUT;
    
    msg_param_init_memparam(&params, mobj, 0, 128, c, MSG_PARAM_MEM_DIR_INOUT);
    
    res = thread_rpc_cmd(OPTEE_TRUSTIO_NETWORK_CALL, 1, &params);
    if(res != TEE_SUCCESS) {
        DMSG("TRUSTIO Read: RPC FAILED\n");
    }
    
    AES_init_ctx_iv(&ctx, key, iv);
    
    //DMSG("TRUSTIO Read Before Decryption: 0x%x, 0x%x, 0x%x, 0x%x\n", (int)(*(va + 12)), (int)(*(va + 13)), (int)(*(va + 14)), (int)(*(va + 15)));
    AES_CBC_decrypt_buffer(&ctx, (uint8_t *)va, 16);
    
    memcpy(&tarretval, va + 12, sizeof(tarretval));
    
    //DMSG("TRUSTIO Read After Decryption: 0x%x, 0x%x, 0x%x, 0x%x\n", (int)(*(va + 12)), (int)(*(va + 13)), (int)(*(va + 14)), (int)(*(va + 15)));
    
    if(tarretval == tarVal + 1) {
        smc_args->a3 = read_from_trustio_pin();
    } else {
        DMSG("TRUSTIO Read Bammed up, Expected:0x%x, Got:0x%x\n", tarVal + 1, tarretval);
    }
    
    thread_rpc_free_payload(c, mobj);
    
    
    
    
    // params[0].u.value.a = encVal ^ 0xdededede;
    //params[0].attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT;
    //params[0].u.value.a = read_from_trustio_pin();
    //params[0].u.value.a ^= target_hash_val;
    //tee_time_get_sys_time(&old_time);
    //res = thread_rpc_cmd(OPTEE_TRUSTIO_NETWORK_CALL, 2, &params);
    
    
    //tee_time_get_sys_time(&new_time);
    //DMSG("TRUSTIO: SECURE SIDE TIME: seconds=%d, milliseconds=%d\n",new_time.seconds-old_time.seconds, new_time.millis-old_time.millis);
    /*if(res != TEE_SUCCESS) {
        //DMSG("TRUSTIO: RPC FAILED\n");
    } else {
        if(params[0].u.value.b == encVal) {
            //DMSG("TRUSTIO: GOT VALID RESPONSE FROM NETWORK\n");
            // Write to the device.
            write_to_trustio_pin(target_val);
        } else {
            DMSG("TRUSTIO: VALUE ERROR: Expected:0x%x, Got:0x%lx\n", encVal, params.u.value.b);
        }
    }*/
    //smc_args->a5 = target_hash_val;
    
}

static void trustio_handle_gpio_on(struct thread_smc_args __maybe_unused *smc_args,
			       struct optee_msg_arg *arg, uint32_t __maybe_unused num_params) {
	// get the target value		       
    long target_val = arg->params[0].u.value.a;
    //long target_hash_val;
    //uint32_t encVal = target_val;
    struct optee_msg_param params;
    //TEE_Time old_time, new_time;
    TEE_Result res;
    
    struct mobj *mobj;
    uint64_t c = 0;
    char *va;
    struct AES_ctx ctx;
    uint32_t tarretval, tarVal = 0x668;
    
    
    uint8_t iv[]  = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
    uint8_t key[] = { 0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c };
    
    
    mobj = thread_rpc_alloc_payload(128, &c);
    //TEE_Time old_time, new_time;
    // Do the RPC.
    va = mobj_get_va(mobj, 0);
    
    //*(uint32_t*)(va + 12) = tarVal;
    memcpy(va + 12, &tarVal, sizeof(tarVal));
    
    //DMSG("TRUSTIO Read Before Encryption: 0x%x, 0x%x, 0x%x, 0x%x\n", (int)(*(va + 12)), (int)(*(va + 13)), (int)(*(va + 14)), (int)(*(va + 15)));
    
    AES_init_ctx_iv(&ctx, key, iv);
    
    AES_CBC_encrypt_buffer(&ctx, (uint8_t *)va, 16);
    
    //DMSG("TRUSTIO Read After Encryption: 0x%x, 0x%x, 0x%x, 0x%x\n", (int)(*(va + 12)), (int)(*(va + 13)), (int)(*(va + 14)), (int)(*(va + 15)));
    
    memset(&params, 0, sizeof(params));
    params.attr = OPTEE_MSG_ATTR_TYPE_VALUE_INOUT;
    
    msg_param_init_memparam(&params, mobj, 0, 128, c, MSG_PARAM_MEM_DIR_INOUT);
    
    res = thread_rpc_cmd(OPTEE_TRUSTIO_NETWORK_CALL, 1, &params);
    if(res != TEE_SUCCESS) {
        DMSG("TRUSTIO Read: RPC FAILED\n");
    }
    
    AES_init_ctx_iv(&ctx, key, iv);
    
    //DMSG("TRUSTIO Read Before Decryption: 0x%x, 0x%x, 0x%x, 0x%x\n", (int)(*(va + 12)), (int)(*(va + 13)), (int)(*(va + 14)), (int)(*(va + 15)));
    AES_CBC_decrypt_buffer(&ctx, (uint8_t *)va, 16);
    
    memcpy(&tarretval, va + 12, sizeof(tarretval));
    
    if(tarretval == tarVal + 1) {
        write_to_trustio_pin(target_val);
    } else {
        DMSG("TRUSTIO Write Bammed up, Expected:0x%x, Got:0x%x\n", tarVal + 1, tarretval);
    }
    
    thread_rpc_free_payload(c, mobj);
    
    
    
    //params.attr = OPTEE_MSG_ATTR_TYPE_VALUE_INOUT;
    
    //res = thread_rpc_cmd(OPTEE_TRUSTIO_NETWORK_CALL, 1, &params);
    //target_hash_val = params[0].u.value.b;
    //target_hash_val ^= read_from_trustio_pin();
    
    
    //params.u.value.a = encVal ^ 0xdededede;
    //params[0].attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT;
    //params[0].u.value.a = read_from_trustio_pin();
    //params[0].u.value.a ^= target_hash_val;
    //tee_time_get_sys_time(&old_time);
    //res = thread_rpc_cmd(OPTEE_TRUSTIO_NETWORK_CALL, 1, &params);
    
    
    //tee_time_get_sys_time(&new_time);
    //DMSG("TRUSTIO: SECURE SIDE TIME: seconds=%d, milliseconds=%d\n",new_time.seconds-old_time.seconds, new_time.millis-old_time.millis);
    /*if(res != TEE_SUCCESS) {
        DMSG("TRUSTIO: RPC FAILED\n");
    } else {
        if(params.u.value.b == encVal) {
            //DMSG("TRUSTIO: GOT VALID RESPONSE FROM NETWORK\n");
            // Write to the device.
            write_to_trustio_pin(target_val);
        } else {
            DMSG("TRUSTIO: VALUE ERROR: Expected:0x%x, Got:0x%lx\n", encVal, params.u.value.b);
        }
    }*/
    
}
			       

static void entry_close_session(struct thread_smc_args *smc_args,
			struct optee_msg_arg *arg, uint32_t num_params)
{
	TEE_Result res;
	struct tee_ta_session *s;

	if (num_params) {
		res = TEE_ERROR_BAD_PARAMETERS;
		goto out;
	}

	plat_prng_add_jitter_entropy();

	s = (struct tee_ta_session *)(vaddr_t)arg->session;
	res = tee_ta_close_session(s, &tee_open_sessions, NSAPP_IDENTITY);
out:
	arg->ret = res;
	arg->ret_origin = TEE_ORIGIN_TEE;
	smc_args->a0 = OPTEE_SMC_RETURN_OK;
}

static void entry_invoke_command(struct thread_smc_args *smc_args,
				 struct optee_msg_arg *arg, uint32_t num_params)
{
	TEE_Result res;
	TEE_ErrorOrigin err_orig = TEE_ORIGIN_TEE;
	struct tee_ta_session *s;
	struct tee_ta_param param;
	uint64_t saved_attr[TEE_NUM_PARAMS];

	bm_timestamp();

	res = copy_in_params(arg->params, num_params, &param, saved_attr);
	if (res != TEE_SUCCESS)
		goto out;

	s = tee_ta_get_session(arg->session, true, &tee_open_sessions);
	if (!s) {
		res = TEE_ERROR_BAD_PARAMETERS;
		goto out;
	}

	res = tee_ta_invoke_command(&err_orig, s, NSAPP_IDENTITY,
				    TEE_TIMEOUT_INFINITE, arg->func, &param);

	bm_timestamp();

	tee_ta_put_session(s);

	copy_out_param(&param, num_params, arg->params, saved_attr);

out:
	cleanup_params(arg->params, saved_attr, num_params);

	arg->ret = res;
	arg->ret_origin = err_orig;
	smc_args->a0 = OPTEE_SMC_RETURN_OK;
}

static void entry_cancel(struct thread_smc_args *smc_args,
			struct optee_msg_arg *arg, uint32_t num_params)
{
	TEE_Result res;
	TEE_ErrorOrigin err_orig = TEE_ORIGIN_TEE;
	struct tee_ta_session *s;

	if (num_params) {
		res = TEE_ERROR_BAD_PARAMETERS;
		goto out;
	}

	s = tee_ta_get_session(arg->session, false, &tee_open_sessions);
	if (!s) {
		res = TEE_ERROR_BAD_PARAMETERS;
		goto out;
	}

	res = tee_ta_cancel_command(&err_orig, s, NSAPP_IDENTITY);
	tee_ta_put_session(s);

out:
	arg->ret = res;
	arg->ret_origin = err_orig;
	smc_args->a0 = OPTEE_SMC_RETURN_OK;
}

static void register_shm(struct thread_smc_args *smc_args,
			 struct optee_msg_arg *arg, uint32_t num_params)
{
	if (num_params != 1 ||
	    (arg->params[0].attr !=
	     (OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT | OPTEE_MSG_ATTR_NONCONTIG))) {
		arg->ret = TEE_ERROR_BAD_PARAMETERS;
		return;
	}

	/* We don't need mobj pointer there, we only care if it was created */
	if (!msg_param_mobj_from_noncontig(arg->params[0].u.tmem.buf_ptr,
					   arg->params[0].u.tmem.size,
					   arg->params[0].u.tmem.shm_ref,
					   false))
		arg->ret = TEE_ERROR_BAD_PARAMETERS;
	else
		arg->ret = TEE_SUCCESS;

	smc_args->a0 = OPTEE_SMC_RETURN_OK;
}

static void unregister_shm(struct thread_smc_args *smc_args,
			   struct optee_msg_arg *arg, uint32_t num_params)
{
	if (num_params == 1) {
		struct mobj *mobj;
		uint64_t cookie = arg->params[0].u.rmem.shm_ref;

		mobj = mobj_reg_shm_find_by_cookie(cookie);
		if (mobj) {
			mobj_free(mobj);
			arg->ret = TEE_SUCCESS;
		} else {
			EMSG("Can't find mapping with given cookie");
			arg->ret = TEE_ERROR_BAD_PARAMETERS;
		}
	} else {
		arg->ret = TEE_ERROR_BAD_PARAMETERS;
		arg->ret_origin = TEE_ORIGIN_TEE;
	}

	smc_args->a0 = OPTEE_SMC_RETURN_OK;
}

static struct mobj *map_cmd_buffer(paddr_t parg, uint32_t *num_params)
{
	struct mobj *mobj;
	struct optee_msg_arg *arg;
	size_t args_size;

	assert(!(parg & SMALL_PAGE_MASK));
	/* mobj_mapped_shm_alloc checks if parg resides in nonsec ddr */
	mobj = mobj_mapped_shm_alloc(&parg, 1, 0, 0);
	if (!mobj)
		return NULL;

	arg = mobj_get_va(mobj, 0);
	if (!arg) {
		mobj_free(mobj);
		return NULL;
	}

	*num_params = arg->num_params;
	args_size = OPTEE_MSG_GET_ARG_SIZE(*num_params);
	if (args_size > SMALL_PAGE_SIZE) {
		EMSG("Command buffer spans across page boundary");
		mobj_free(mobj);
		return NULL;
	}

	return mobj;
}

static struct mobj *get_cmd_buffer(paddr_t parg, uint32_t *num_params)
{
	struct optee_msg_arg *arg;
	size_t args_size;

	arg = phys_to_virt(parg, MEM_AREA_NSEC_SHM);
	if (!arg)
		return NULL;

	*num_params = arg->num_params;
	args_size = OPTEE_MSG_GET_ARG_SIZE(*num_params);

	return mobj_shm_alloc(parg, args_size);
}

/*
 * Note: this function is weak just to make it possible to exclude it from
 * the unpaged area.
 */
void __weak tee_entry_std(struct thread_smc_args *smc_args)
{
	paddr_t parg;
	struct optee_msg_arg *arg = NULL;	/* fix gcc warning */
	uint32_t num_params = 0;		/* fix gcc warning */
	struct mobj *mobj;

	if (smc_args->a0 != OPTEE_SMC_CALL_WITH_ARG) {
		EMSG("Unknown SMC 0x%" PRIx64, (uint64_t)smc_args->a0);
		DMSG("Expected 0x%x\n", OPTEE_SMC_CALL_WITH_ARG);
		smc_args->a0 = OPTEE_SMC_RETURN_EBADCMD;
		return;
	}
	parg = (uint64_t)smc_args->a1 << 32 | smc_args->a2;

	/* Check if this region is in static shared space */
	if (core_pbuf_is(CORE_MEM_NSEC_SHM, parg,
			  sizeof(struct optee_msg_arg))) {
		mobj = get_cmd_buffer(parg, &num_params);
	} else {
		if (parg & SMALL_PAGE_MASK) {
			smc_args->a0 = OPTEE_SMC_RETURN_EBADADDR;
			return;
		}
		mobj = map_cmd_buffer(parg, &num_params);
	}

	if (!mobj || !ALIGNMENT_IS_OK(parg, struct optee_msg_arg)) {
		EMSG("Bad arg address 0x%" PRIxPA, parg);
		smc_args->a0 = OPTEE_SMC_RETURN_EBADADDR;
		mobj_free(mobj);
		return;
	}

	arg = mobj_get_va(mobj, 0);
	assert(arg && mobj_is_nonsec(mobj));

	/* Enable foreign interrupts for STD calls */
	thread_set_foreign_intr(true);
	switch (arg->cmd) {
	case OPTEE_MSG_CMD_OPEN_SESSION:
		entry_open_session(smc_args, arg, num_params);
		break;
	case OPTEE_MSG_CMD_CLOSE_SESSION:
		entry_close_session(smc_args, arg, num_params);
		break;
	case OPTEE_MSG_CMD_INVOKE_COMMAND:
		entry_invoke_command(smc_args, arg, num_params);
		break;
	case OPTEE_MSG_CMD_CANCEL:
		entry_cancel(smc_args, arg, num_params);
		break;
	case OPTEE_MSG_CMD_REGISTER_SHM:
		register_shm(smc_args, arg, num_params);
		break;
	case OPTEE_MSG_CMD_UNREGISTER_SHM:
		unregister_shm(smc_args, arg, num_params);
		break;
    case OPTEE_MSG_CMD_TRUSTIO_GPIO:
        //TODO: handle trust IO GPIO
        // INIT trust.io
        init_trustio_gpio();
        // perform the trustio stuff
        trustio_handle_gpio_on(smc_args, arg, num_params);
        break;
    case OPTEE_MSG_CMD_TRUSTIO_GPIO_READ:
        init_trustio_gpio();
        // perform the trustio stuff
        trustio_handle_challenge_req(smc_args, arg, num_params);
        break;
	default:
		EMSG("Unknown cmd 0x%x\n", arg->cmd);
		smc_args->a0 = OPTEE_SMC_RETURN_EBADCMD;
	}
	mobj_free(mobj);
}

static TEE_Result default_mobj_init(void)
{
	shm_mobj = mobj_phys_alloc(default_nsec_shm_paddr,
				   default_nsec_shm_size, SHM_CACHE_ATTRS,
				   CORE_MEM_NSEC_SHM);
	if (!shm_mobj)
		panic("Failed to register shared memory");

	mobj_sec_ddr = mobj_phys_alloc(tee_mm_sec_ddr.lo,
				       tee_mm_sec_ddr.hi - tee_mm_sec_ddr.lo,
				       SHM_CACHE_ATTRS, CORE_MEM_TA_RAM);
	if (!mobj_sec_ddr)
		panic("Failed to register secure ta ram");

	mobj_tee_ram = mobj_phys_alloc(CFG_TEE_RAM_START,
				       VCORE_UNPG_RW_PA + VCORE_UNPG_RW_SZ -
						CFG_TEE_RAM_START,
				       TEE_MATTR_CACHE_CACHED,
				       CORE_MEM_TEE_RAM);
	if (!mobj_tee_ram)
		panic("Failed to register tee ram");

#ifdef CFG_SECURE_DATA_PATH
	sdp_mem_mobjs = core_sdp_mem_create_mobjs();
	if (!sdp_mem_mobjs)
		panic("Failed to register SDP memory");
#endif

	return TEE_SUCCESS;
}

driver_init_late(default_mobj_init);
