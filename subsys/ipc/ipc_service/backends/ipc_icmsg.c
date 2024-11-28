/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ipc_icmsg.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/ipc/icmsg.h>

#include <zephyr/ipc/ipc_service_backend.h>

#define DT_DRV_COMPAT	zephyr_ipc_icmsg

static int register_ept(const struct device *instance, void **token,
			const struct ipc_ept_cfg *cfg)
{
	const struct icmsg_config_t *conf = instance->config;
	struct icmsg_data_t *dev_data = instance->data;

	/* Only one endpoint is supported. No need for a token. */
	*token = NULL;

	return icmsg_open(conf, dev_data, &cfg->cb, cfg->priv);
}

static int deregister_ept(const struct device *instance, void *token)
{
	const struct icmsg_config_t *conf = instance->config;
	struct icmsg_data_t *dev_data = instance->data;

	return icmsg_close(conf, dev_data);
}

static int send(const struct device *instance, void *token,
		const void *msg, size_t len)
{
	const struct icmsg_config_t *conf = instance->config;
	struct icmsg_data_t *dev_data = instance->data;

	return icmsg_send(conf, dev_data, msg, len);
}

const static struct ipc_service_backend backend_ops = {
	.register_endpoint = register_ept,
	.deregister_endpoint = deregister_ept,
	.send = send,
};

static int backend_init(const struct device *instance)
{
	return 0;
}

#define UNBOUND_MODE(i) CONCAT(ICMSG_UNBOUND_MODE_, DT_INST_STRING_UPPER_TOKEN(i, unbound))

#define DEFINE_BACKEND_DEVICE(i)						\
	static const struct icmsg_config_t backend_config_##i = {		\
		.mbox_tx = MBOX_DT_SPEC_INST_GET(i, tx),			\
		.mbox_rx = MBOX_DT_SPEC_INST_GET(i, rx),			\
		.unbound_mode = UNBOUND_MODE(i),				\
	};									\
										\
	PBUF_DEFINE(tx_pb_##i,							\
			DT_REG_ADDR(DT_INST_PHANDLE(i, tx_region)),		\
			DT_REG_SIZE(DT_INST_PHANDLE(i, tx_region)),		\
			DT_INST_PROP_OR(i, dcache_alignment, 0),		\
			UNBOUND_MODE(i) != ICMSG_UNBOUND_MODE_DISABLE,		\
			UNBOUND_MODE(i) == ICMSG_UNBOUND_MODE_DETECT);		\
	PBUF_DEFINE(rx_pb_##i,							\
			DT_REG_ADDR(DT_INST_PHANDLE(i, rx_region)),		\
			DT_REG_SIZE(DT_INST_PHANDLE(i, rx_region)),		\
			DT_INST_PROP_OR(i, dcache_alignment, 0),		\
			UNBOUND_MODE(i) != ICMSG_UNBOUND_MODE_DISABLE,		\
			UNBOUND_MODE(i) == ICMSG_UNBOUND_MODE_DETECT);		\
										\
	BUILD_ASSERT(UNBOUND_MODE(i) != ICMSG_UNBOUND_MODE_DISABLE ||		\
		IS_ENABLED(CONFIG_IPC_SERVICE_ICMSG_UNBOUND_DISABLED_ALLOWED),	\
		"Unbound mode \"disabled\" is was forbidden in Kconfig.");	\
										\
	BUILD_ASSERT(UNBOUND_MODE(i) != ICMSG_UNBOUND_MODE_ENABLE ||		\
		IS_ENABLED(CONFIG_IPC_SERVICE_ICMSG_UNBOUND_ENABLED_ALLOWED),	\
		"Unbound mode \"enabled\" is was forbidden in Kconfig.");	\
										\
	BUILD_ASSERT(UNBOUND_MODE(i) != ICMSG_UNBOUND_MODE_DETECT ||		\
		IS_ENABLED(CONFIG_IPC_SERVICE_ICMSG_UNBOUND_DETECT_ALLOWED),	\
		"Unbound mode \"detect\" is was forbidden in Kconfig.");	\
										\
	static struct icmsg_data_t backend_data_##i = {				\
		.tx_pb = &tx_pb_##i,						\
		.rx_pb = &rx_pb_##i,						\
	};									\
										\
	DEVICE_DT_INST_DEFINE(i,						\
			 &backend_init,						\
			 NULL,							\
			 &backend_data_##i,					\
			 &backend_config_##i,					\
			 POST_KERNEL,						\
			 CONFIG_IPC_SERVICE_REG_BACKEND_PRIORITY,		\
			 &backend_ops);

DT_INST_FOREACH_STATUS_OKAY(DEFINE_BACKEND_DEVICE)

/* TODO: REMOVE THIS WORKAROUND!!! */

static int workaround_ppr_reset(void)
{
#define _FIX_RESET_MEM(i) \
	memset(&backend_data_##i, 0, sizeof(backend_data_##i)); \
	backend_data_##i.tx_pb = &tx_pb_##i; \
	backend_data_##i.rx_pb = &rx_pb_##i;
	DT_INST_FOREACH_STATUS_OKAY(_FIX_RESET_MEM);
}

SYS_INIT(workaround_ppr_reset, PRE_KERNEL_1, 0);
