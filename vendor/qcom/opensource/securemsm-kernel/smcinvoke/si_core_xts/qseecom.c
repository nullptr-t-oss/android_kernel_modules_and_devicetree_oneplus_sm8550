// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fdtable.h>
#include <linux/anon_inodes.h>
#include <linux/delay.h>
#include <linux/kref.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <linux/elf.h>
#include <linux/qtee_shmbridge.h>
#include <linux/firmware/qcom/si_object.h>
#include <linux/firmware/qcom/si_core_xts.h>

#if !IS_ENABLED(CONFIG_QSEECOM)
//#include "linux/qseecom_api.h"
#if IS_ENABLED(CONFIG_QSEECOM_PROXY)
#include <linux/qseecom_kernel.h>
#else
#include "misc/qseecom_kernel.h"
#endif /* CONFIG_QSEECOM_PROXY */
#endif /* CONFIG_QSEECOM */

/* Device for firmware API :0.*/
extern struct device *smci_dev;
const uint32_t CQSEEComCompatAppLoader_UID = 122;

#define MAX_FW_APP_SIZE		256	/* Application name size. */
#define FILE_EXT_SIZE		(4 + 1)	/* Suffix length + NULL terminator */

#if !IS_ENABLED(CONFIG_QSEECOM)

#define SI_CORE_QSEECOMCOMPAT_TRF_TIMEOUT_MS			420000
#define SI_CORE_QSEECOMCOMPAT_TRF_INTERVAL_MS			5
#define Object_ERROR_BUSY					-99

/* SMCI QSEECOMCOMPAT OP IDs */
#define SI_CORE_QSEECOMCOMPAT_OP_SEND_REQUEST			0
#define SI_CORE_QSEECOMCOMPAT_OP_LOAD_FROM_BUFFER		1
#define SI_CORE_QSEECOMCOMPAT_OP_LOOKUP_TA			2
#define SI_CORE_QSEECOMCOMPAT_OP_LOAD_FROM_REGION		0
#define SI_CORE_QSEECOMCOMPAT_OP_UNLOAD				2

struct qseecom_compat_context {
	void *dev; /* in/out */
	unsigned char *sbuf; /* in/out */
	u32 sbuf_len;
	struct qtee_shm shm;
	u8 app_arch;
	struct si_object *app_controller;
	struct si_object_invoke_ctx oic;
};

struct qsee_mo_ctx {
	struct si_object *object;
	void *vaddr;
	size_t size;
	struct sg_table *sgt;
};

char *firmware_request_from_smcinvoke(const char *appname,
	size_t *fw_size, struct qtee_shm *shm);

static int si_object_do_invoke_with_retry(struct si_object_invoke_ctx *oic,
	struct si_object *object, unsigned long op, struct si_arg u[], int *result)
{
	int ret;
	unsigned int attempts = 0;

	unsigned long deadline = jiffies + msecs_to_jiffies(SI_CORE_QSEECOMCOMPAT_TRF_TIMEOUT_MS);

	do {
		ret = si_object_do_invoke(oic, object, op, u, result);
		if (ret)
			return ret;

		if (*result != Object_ERROR_BUSY)
			return 0;

		pr_info("%s: BUSY (%d), retry #%u, op=%lu, sleep %u ms\n",
			__func__, *result, attempts + 1, op, SI_CORE_QSEECOMCOMPAT_TRF_INTERVAL_MS);

		if (time_after_eq(jiffies, deadline))
			break;

		attempts++;
		msleep(SI_CORE_QSEECOMCOMPAT_TRF_INTERVAL_MS);
	} while (time_before(jiffies, deadline));

	pr_err("%s: timeout on BUSY (op=%lu)\n", __func__, op);

	return 0;
}

static int si_core_qseecomcompat_lookup_ta(
	struct si_object_invoke_ctx *oic,
	struct si_object *app_loader,
	const void *appName_ptr, size_t appName_len,
	struct si_object **appCompat,
	uint32_t *appArchType_ptr)
{
	int result = 0;
	int ret = 0;
	struct si_arg args[4] = {0};

	args[0].type = SI_AT_IB;
	args[0].b.addr = (void *)appName_ptr;
	args[0].b.size = appName_len * sizeof(uint8_t);
	args[1].type = SI_AT_OB;
	args[1].b.addr = appArchType_ptr;
	args[1].b.size = sizeof(uint32_t);
	args[2].type = SI_AT_OO;
	args[3].type = SI_AT_END;

	ret = si_object_do_invoke_with_retry(oic, app_loader,
				SI_CORE_QSEECOMCOMPAT_OP_LOOKUP_TA,
				args, &result);
	if (ret || result) {
		pr_info("%s result %d (ret = %d): App (%s) doesn't exist.\n", __func__,
				result, ret, (char *)appName_ptr);
		return -EINVAL;
	}

	*appCompat = args[2].o;

	return 0;
}

static int si_core_qseecomcompat_send_request(
	struct si_object_invoke_ctx *oic,
	struct si_object *app_controller,
	const void *req_in_ptr, size_t req_in_len,
	const void *rsp_in_ptr, size_t rsp_in_len,
	void *req_out_ptr, size_t req_out_len, size_t *req_out_lenout,
	void *rsp_out_ptr, size_t rsp_out_len, size_t *rsp_out_lenout,
	const uint32_t *embedded_buf_offsets_ptr, size_t embedded_buf_offsets_len,
	uint32_t is64,
	struct si_object *smo1, struct si_object *smo2,
	struct si_object *smo3, struct si_object *smo4)
{
	int result = 0;
	int ret = 0;
	struct si_arg args[11] = {0};

	args[0].type = SI_AT_IB;
	args[0].b.addr = (void *)req_in_ptr;
	args[0].b.size = req_in_len;
	args[1].type = SI_AT_IB;
	args[1].b.addr = (void *)rsp_in_ptr;
	args[1].b.size = rsp_in_len;
	args[2].type = SI_AT_IB;
	args[2].b.addr = (void *)embedded_buf_offsets_ptr;
	args[2].b.size = embedded_buf_offsets_len * sizeof(uint32_t);
	args[3].type = SI_AT_IB;
	args[3].b.addr = (void *)&is64;
	args[3].b.size = sizeof(uint32_t);
	args[4].type = SI_AT_OB;
	args[4].b.addr = req_out_ptr;
	args[4].b.size = req_out_len;
	args[5].type = SI_AT_OB;
	args[5].b.addr = rsp_out_ptr;
	args[5].b.size = rsp_out_len;
	args[6].type = SI_AT_IO;
	args[6].o = smo1;
	args[7].type = SI_AT_IO;
	args[7].o = smo2;
	args[8].type = SI_AT_IO;
	args[8].o = smo3;
	args[9].type = SI_AT_IO;
	args[9].o = smo4;
	args[10].type = SI_AT_END;

	ret = si_object_do_invoke_with_retry(oic, app_controller,
				SI_CORE_QSEECOMCOMPAT_OP_SEND_REQUEST,
				args, &result);
	if (ret || result) {
		pr_err("%s failed with result %d (ret = %d).\n", __func__,
				result, ret);
		return -EINVAL;
	}

	*req_out_lenout = args[4].b.size;
	*rsp_out_lenout = args[5].b.size;

	return 0;
}

static void qsee_mo_release(void *priv)
{
	struct qsee_mo_ctx *ctx = (struct qsee_mo_ctx *)priv;

	if (ctx && ctx->sgt) {
		__free_page(sg_page(ctx->sgt->sgl));
		sg_free_table(ctx->sgt);
		kfree(ctx->sgt);
		kfree(ctx);
	}
}

static struct si_object *qsee_mo_init(void *ta_file, size_t size)
{
	int ret;
	struct sg_table *sgt;
	struct page *page;
	size_t size_aligned = 0;
	void *buffer;
	struct qsee_mo_ctx *ctx;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return NULL;

	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (ret) {
		pr_err("sg_alloc_table() failed! ret: %d\n", ret);
		goto err_sg_alloc;
	}

	page = alloc_pages(GFP_KERNEL, get_order(size));
	if (!page) {
		pr_err("alloc_pages() failed!\n");
		goto err_alloc_pages;
	}

	size_aligned = (PAGE_SIZE << get_order(size));
	sg_set_page(sgt->sgl, page, size_aligned, 0);
	pr_info("Allocated pages of order %d and size 0x%zx\n", get_order(size),
		size_aligned);

	buffer = sg_virt(sgt->sgl);
	memcpy(buffer, ta_file, size);

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		goto err_ctx_alloc;

	ctx->vaddr = buffer;
	ctx->size = size_aligned;
	ctx->sgt = sgt;

	ctx->object = init_si_mem_object_sg(sgt, 0, 0, qsee_mo_release, ctx);
	if (!ctx->object) {
		pr_err("init_si_mem_object_sg() failed.\n");
		goto err_init_mo;
	}

	return ctx->object;

err_init_mo:
	kfree(ctx);
err_ctx_alloc:
	__free_page(page);
err_alloc_pages:
	sg_free_table(sgt);
err_sg_alloc:
	kfree(sgt);

	return NULL;
}

static int si_core_qseecomcompat_load_from_region(
	struct si_object_invoke_ctx *oic,
	struct si_object *app_loader,
	struct si_object *mem_obj,
	const void *filename_ptr, size_t filename_len,
	struct si_object **app_controller)
{
	int result = 0;
	int ret = 0;
	struct si_arg args[4] = {0};

	args[0].type = SI_AT_IB;
	args[0].b.addr = (void *)filename_ptr;
	args[0].b.size = filename_len * sizeof(uint8_t);
	args[1].type = SI_AT_IO;
	args[1].o = mem_obj;
	args[2].type = SI_AT_OO;
	args[3].type = SI_AT_END;

	ret = si_object_do_invoke(oic, app_loader,
				SI_CORE_QSEECOMCOMPAT_OP_LOAD_FROM_REGION,
				args, &result);
	if (ret || result) {
		pr_err("%s failed with result %d (ret = %d).\n", __func__,
				result, ret);
		return -EINVAL;
	}
	*app_controller = args[2].o;

	return 0;
}

static int si_core_qseecomcompat_unload(
	struct si_object_invoke_ctx *oic,
	struct si_object *app_controller)
{
	int result = 0;
	int ret = 0;
	struct si_arg args[1] = {0};

	args[0].type = SI_AT_END;

	ret = si_object_do_invoke_with_retry(oic, app_controller,
				SI_CORE_QSEECOMCOMPAT_OP_UNLOAD,
				args, &result);
	if (ret || result) {
		pr_err("%s failed with result %d (ret = %d).\n", __func__,
				result, ret);
		return -EINVAL;
	}

	return 0;
}

static int load_app(struct qseecom_compat_context *cxt,
	const char *app_name, struct si_object *app_loader)
{
	size_t fw_size = 0;
	u8 *imgbuf_va = NULL;
	int ret = 0;
	struct qtee_shm shm = {0};
	uint32_t arch_type = 0;
	struct si_object *mo = NULL;

	if (strnlen(app_name, MAX_FW_APP_SIZE) == MAX_FW_APP_SIZE) {
		pr_err("The app_name (%s) with length %zu is not valid\n",
			app_name, strnlen(app_name, MAX_FW_APP_SIZE));
		return -EINVAL;
	}

	ret = si_core_qseecomcompat_lookup_ta(&cxt->oic, app_loader,
		app_name, strlen(app_name), &cxt->app_controller, &arch_type);
	if (!ret) {
		pr_info("app %s exists\n", app_name);
		return ret;
	}

	/* Assemble split bins into a contiguous buffer via firmware API */
	imgbuf_va = firmware_request_from_smcinvoke(app_name, &fw_size, &shm);
	if (imgbuf_va == NULL) {
		pr_err("Failed on firmware_request_from_smcinvoke\n");
		return -EINVAL;
	}

	mo = qsee_mo_init(imgbuf_va, fw_size);
	if (!mo) {
		ret = -EINVAL;
		goto out;
	}
	get_si_object(mo);

	ret = si_core_qseecomcompat_load_from_region(&cxt->oic, app_loader,
					mo, app_name, strlen(app_name),
					&cxt->app_controller);
	if (ret) {
		pr_err("loadFromRegion failed for app %s, ret = %d\n",
				app_name, ret);
		goto out_release_mo;
	}

	cxt->app_arch = *(uint8_t *)(imgbuf_va + EI_CLASS);
	pr_info("%s %d, loaded app %s\n", __func__, __LINE__, app_name);

out_release_mo:
	put_si_object(mo);
out:
	qtee_shmbridge_free_shm(&shm);
	return ret;
}

static int __qseecom_start_app(struct qseecom_handle **handle,
	char *app_name, uint32_t size)
{
	int ret = 0;
	struct qseecom_compat_context *cxt = NULL;
	struct si_object *client_env = NULL_SI_OBJECT;
	struct si_object *app_loader = NULL_SI_OBJECT;

	pr_warn("%s, start app %s, size %u\n",
		__func__, app_name, size);
	if (app_name == NULL || handle == NULL) {
		pr_err("app_name is null or invalid handle\n");
		return -EINVAL;
	}
	/* allocate qseecom_compat_context */
	cxt = kzalloc(sizeof(struct qseecom_compat_context), GFP_KERNEL);
	if (!cxt)
		return -ENOMEM;

	/* get client env */
	ret = si_core_get_client_env(&cxt->oic, &client_env);
	if (ret) {
		pr_err("failed to get clientEnv when loading app %s, ret=%d\n",
				app_name, ret);
		goto exit_free_cxt;
	}
	/* get app loader */
	ret = si_core_client_env_open(&cxt->oic, client_env,
			CQSEEComCompatAppLoader_UID, &app_loader);
	if (ret) {
		pr_err("failed to get apploader when loading app %s, ret=%d\n",
				app_name, ret);
		ret = -EINVAL;
		goto exit_release_clientenv;
	}
	/* load app */
	ret = load_app(cxt, app_name, app_loader);
	if (ret) {
		pr_err("failed to load app %s, ret = %d\n", app_name, ret);
		ret = -EINVAL;
		goto exit_release_apploader;
	}

	/* Get the physical address of the req/resp buffer */
	ret = qtee_shmbridge_allocate_shm(size, &cxt->shm);
	if (ret) {
		pr_err("qtee_shmbridge_allocate_shm failed, ret :%d\n", ret);
		ret = -EINVAL;
		goto exit_release_appcontroller;
	}
	cxt->sbuf = cxt->shm.vaddr;
	cxt->sbuf_len = size;
	*handle = (struct qseecom_handle *)cxt;

	/* Release handles after successful load */
	put_si_object(app_loader);
	put_si_object(client_env);

	return ret;

exit_release_appcontroller:
	put_si_object(cxt->app_controller);
exit_release_apploader:
	put_si_object(app_loader);
exit_release_clientenv:
	put_si_object(client_env);
exit_free_cxt:
	kfree(cxt);

	return ret;
}

static int __qseecom_shutdown_app(struct qseecom_handle **handle)
{
	int ret = 0;
	struct qseecom_compat_context *cxt = NULL;
	struct si_object_invoke_ctx shutdown_oic;

	if ((handle == NULL) || (*handle == NULL)) {
		pr_err("Handle is NULL\n");
		return -EINVAL;
	}

	cxt = (struct qseecom_compat_context *)(*handle);

	memset(&shutdown_oic, 0, sizeof(shutdown_oic));

	ret = si_core_qseecomcompat_unload(&shutdown_oic, cxt->app_controller);
	if (ret)
		pr_warn("Unload failed with ret = %d, proceeding with cleanup\n", ret);

	qtee_shmbridge_free_shm(&cxt->shm);
	put_si_object(cxt->app_controller);
	kfree(cxt);
	*handle = NULL;
	return 0;
}

static int __qseecom_send_command(struct qseecom_handle *handle,
	void *send_buf, uint32_t sbuf_len, void *resp_buf, uint32_t rbuf_len)
{
	struct qseecom_compat_context *cxt =
		(struct qseecom_compat_context *)handle;
	size_t out_len = 0;

	pr_debug("%s, sbuf_len %u, rbuf_len %u\n",
		__func__, sbuf_len, rbuf_len);

	if (!cxt || !send_buf || !resp_buf || !sbuf_len || !rbuf_len) {
		pr_err("One of params is invalid. %s, handle %p, send_buf %p, resp_buf %p, sbuf_len %u, rbuf_len %u\n",
				__func__, cxt, send_buf, resp_buf, sbuf_len, rbuf_len);
		return -EINVAL;
	}

	return si_core_qseecomcompat_send_request(&cxt->oic, cxt->app_controller,
					send_buf, sbuf_len,
					resp_buf, rbuf_len,
					send_buf, sbuf_len, &out_len,
					resp_buf, rbuf_len, &out_len,
					NULL, 0,  /* embedded offset array */
					(cxt->app_arch == ELFCLASS64),
					NULL_SI_OBJECT, NULL_SI_OBJECT,
					NULL_SI_OBJECT, NULL_SI_OBJECT);
}

static int __qseecom_process_listener_from_smcinvoke(uint32_t *result,
		u64 *response_type, unsigned int *data)
{
	/* this API is not supported by compat */
	pr_debug("%s is not supported by compat\n", __func__);
	return -EOPNOTSUPP;
}

#if IS_ENABLED(CONFIG_QSEECOM_PROXY)
static const struct qseecom_drv_ops qseecom_driver_ops = {
	.qseecom_send_command = __qseecom_send_command,
	.qseecom_start_app = __qseecom_start_app,
	.qseecom_shutdown_app = __qseecom_shutdown_app,
	.qseecom_process_listener_from_smcinvoke = __qseecom_process_listener_from_smcinvoke,
};

int get_qseecom_kernel_fun_ops(void)
{
	return provide_qseecom_kernel_fun_ops(&qseecom_driver_ops);
}
#else /* CONFIG_QSEECOM_PROXY */

int qseecom_start_app(struct qseecom_handle **handle, char *app_name, uint32_t size)
{
	return __qseecom_start_app(handle, app_name, size);
}
EXPORT_SYMBOL_GPL(qseecom_start_app);

int qseecom_shutdown_app(struct qseecom_handle **handle)
{
	return __qseecom_shutdown_app(handle);
}
EXPORT_SYMBOL_GPL(qseecom_shutdown_app);

int qseecom_send_command(struct qseecom_handle *handle, void *send_buf,
	uint32_t sbuf_len, void *resp_buf, uint32_t rbuf_len)
{
	return __qseecom_send_command(handle, send_buf, sbuf_len, resp_buf, rbuf_len);
}
EXPORT_SYMBOL_GPL(qseecom_send_command);

int qseecom_process_listener_from_smcinvoke(uint32_t *result,
		u64 *response_type, unsigned int *data)
{
	return __qseecom_process_listener_from_smcinvoke(uint32_t *result,
		u64 *response_type, unsigned int *data);
}
EXPORT_SYMBOL_GPL(qseecom_process_listener_from_smcinvoke);

#endif /* CONFIG_QSEECOM_PROXY */
#endif /* CONFIG_QSEECOM */

char *firmware_request_from_smcinvoke(const char *appname, size_t *fw_size, struct qtee_shm *shm)
{
	int rc = 0;
	const struct firmware *fw_entry = NULL, *fw_entry00 = NULL, *fw_entrylast = NULL;
	char fw_name[MAX_FW_APP_SIZE + FILE_EXT_SIZE] = "\0";
	int num_images = 0, phi = 0;
	unsigned char app_arch = 0;
	u8 *img_data_ptr = NULL;
	size_t bufferOffset = 0, phdr_table_offset = 0;
	size_t *offset = NULL;
	Elf32_Phdr phdr32;
	Elf64_Phdr phdr64;
	struct elf32_hdr *ehdr = NULL;
	struct elf64_hdr *ehdr64 = NULL;


	/* load b00*/
	snprintf(fw_name, sizeof(fw_name), "%s.b00", appname);
	rc = firmware_request_nowarn(&fw_entry00, fw_name, smci_dev);
	if (rc) {
		pr_err("Load %s failed, ret:%d\n", fw_name, rc);
		return NULL;
	}

	app_arch = *(unsigned char *)(fw_entry00->data + EI_CLASS);

	/*Get the offsets for split images header*/
	if (app_arch == ELFCLASS32) {

		ehdr = (struct elf32_hdr *)fw_entry00->data;
		num_images = ehdr->e_phnum;
		offset = kcalloc(num_images, sizeof(size_t), GFP_KERNEL);
		if (offset == NULL)
			goto release_fw_entry00;
		phdr_table_offset = (size_t) ehdr->e_phoff;
		for (phi = 1; phi < num_images; ++phi) {
			bufferOffset = phdr_table_offset + phi * sizeof(Elf32_Phdr);
			phdr32 = *(Elf32_Phdr *)(fw_entry00->data + bufferOffset);
			offset[phi] = (size_t)phdr32.p_offset;
		}

	} else if (app_arch == ELFCLASS64) {

		ehdr64 = (struct elf64_hdr *)fw_entry00->data;
		num_images = ehdr64->e_phnum;
		offset = kcalloc(num_images, sizeof(size_t), GFP_KERNEL);
		if (offset == NULL)
			goto release_fw_entry00;
		phdr_table_offset = (size_t) ehdr64->e_phoff;
		for (phi = 1; phi < num_images; ++phi) {
			bufferOffset = phdr_table_offset + phi * sizeof(Elf64_Phdr);
			phdr64 = *(Elf64_Phdr *)(fw_entry00->data + bufferOffset);
			offset[phi] = (size_t)phdr64.p_offset;
		}

	} else {

		pr_err("QSEE %s app, arch %u is not supported\n", appname, app_arch);
		goto release_fw_entry00;
	}

	/*Find the size of last split bin image*/
	snprintf(fw_name, ARRAY_SIZE(fw_name), "%s.b%02d", appname, num_images-1);
	rc = firmware_request_nowarn(&fw_entrylast, fw_name, smci_dev);
	if (rc) {
		pr_err("Failed to locate blob %s\n", fw_name);
		goto release_fw_entry00;
	}

	/*Total size of image will be the offset of last image + the size of last split image*/
	*fw_size = fw_entrylast->size + offset[num_images-1];

	/*Allocate memory for the buffer that will hold the split image*/
	rc = qtee_shmbridge_allocate_shm((*fw_size), shm);
	if (rc) {
		pr_err("smbridge alloc failed for size: %zu\n", *fw_size);
		goto release_fw_entrylast;
	}
	img_data_ptr = shm->vaddr;
	/*
	 * Copy contents of split bins to the buffer
	 */
	memcpy(img_data_ptr, fw_entry00->data, fw_entry00->size);
	for (phi = 1; phi < num_images-1; phi++) {
		snprintf(fw_name, ARRAY_SIZE(fw_name), "%s.b%02d", appname, phi);
		rc = firmware_request_nowarn(&fw_entry, fw_name, smci_dev);
		if (rc) {
			pr_err("Failed to locate blob %s\n", fw_name);
			qtee_shmbridge_free_shm(shm);
			img_data_ptr = NULL;
			goto release_fw_entrylast;
		}
		memcpy(img_data_ptr + offset[phi], fw_entry->data, fw_entry->size);
		release_firmware(fw_entry);
		fw_entry = NULL;
	}
	memcpy(img_data_ptr + offset[phi], fw_entrylast->data, fw_entrylast->size);

release_fw_entrylast:
	release_firmware(fw_entrylast);
release_fw_entry00:
	release_firmware(fw_entry00);
	kfree(offset);
	return img_data_ptr;
}
EXPORT_SYMBOL_GPL(firmware_request_from_smcinvoke);
