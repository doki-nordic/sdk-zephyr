
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/sys/bitarray.h>
#include <zephyr/ipc/icmsg.h>
#include <zephyr/ipc/ipc_service_backend.h>

#define DT_DRV_COMPAT	zephyr_ipc_icmsg_with_buf

#define ID_INVALID 0x7F
#define ID_FREE_BLOCKS 0xFF
#define ID_REGISTER_EP_FLAG 0x80

#define CONFIG_IPC_ICMSG_WITH_BUF_ENDPOINT_COUNT 4 // TODO: move to kconfig

/*
 * Shared memory layout:
 *     | block 0 | block 1 | ... | block N-1 | ICMSG queue |
 * 
 * ICMSG queue contains enough space to hold at least the same number of
 * messages as number of blocks.
*/

#define BLOCK_ALIGNMENT sizeof(uintptr_t)
#define BYTES_PER_ICMSG_MESSAGE 4
#define ICMSG_BUFFER_OVERHEAD 32 // TODO: check it
// TODO: bugfix: queue should have space for both send messages and receive deallocation requests
#define GET_BLOCK_SIZE(total_size, blocks) ROUND_DOWN(			\
	((total_size) - ICMSG_BUFFER_OVERHEAD -				\
	BYTES_PER_ICMSG_MESSAGE * (blocks)) / (blocks), BLOCK_ALIGNMENT)

#define GET_ICMSG_OFFSET(total_size, blocks) \
	(GET_BLOCK_SIZE(total_size, blocks) * (blocks))

#define GET_ICMSG_SIZE(total_size, blocks) \
	((total_size) - GET_ICMSG_OFFSET(total_size, blocks))

struct block_start {
	uintptr_t size;
	uint8_t data[];
};

struct icmsg_with_buf_config_t {
	struct icmsg_config_t icmsg_config;
	uint8_t *tx_blocks_ptr;
	uint8_t *rx_blocks_ptr;
	uint16_t tx_block_count;
	uint16_t rx_block_count;
	size_t tx_block_size;
	size_t rx_block_size;
	sys_bitarray_t *tx_bitmap;
};

struct endpoint_data {
	const struct ipc_ept_cfg *cfg;
	const char *name; // may be set before cfg came
	uint8_t local_id;
	uint8_t remote_id; // TODO: is it needed?
	bool bounded;
};

struct backend_data_t { // TODO: remove _t endings
	const struct icmsg_with_buf_config_t* conf;
	struct icmsg_data_t icmsg_data;
	struct k_mutex epts_mutex;
	struct k_work ep_bound_work;
	struct endpoint_data ep[CONFIG_IPC_ICMSG_WITH_BUF_ENDPOINT_COUNT];
	uint8_t remote_to_local_id[CONFIG_IPC_ICMSG_WITH_BUF_ENDPOINT_COUNT];
	uint8_t ep_count;
	bool icmsg_bounded;
};

static int alloc_tx_buffer(struct backend_data_t *dev_data, size_t size, uint8_t **buffer)
{
	const struct icmsg_with_buf_config_t *conf = dev_data->conf;
	size_t total_size = size + BLOCK_ALIGNMENT; // TODO: offset of block->data
	size_t num_blocks = (total_size + conf->tx_block_size - 1) / conf->tx_block_size;
	size_t offset;
	bool sem_taken = false;
	struct block_start *block;
	int r;

	if (num_blocks > conf->tx_block_count) {
		return -ENOMEM;
	}

	do {
		r = sys_bitarray_alloc(conf->tx_bitmap, num_blocks, &offset);
		if (r == -ENOSPC) {
			sem_taken = true;
			// TODO: sem_take
			continue;
		} else {
			// TODO: assert r == 0
		}
	} while (r != 0);

	if (sem_taken) {
		// TODO: sem_give();
	}

	block = (struct block_start *)((uint8_t *)conf->tx_blocks_ptr + conf->tx_block_size * offset);

	block->size = size;
	if (buffer != NULL) {
		*buffer = block->data;
	}

	return offset;
}


static void bound(void *priv)
{
	const struct device *instance = priv;
	struct backend_data_t *dev_data = instance->data;
}

static void received(const void *data, size_t len, void *priv)
{
	const struct device *instance = priv;
	struct backend_data_t *dev_data = instance->data;
}

static const struct ipc_service_cb cb = {
	.bound = bound,
	.received = received,
	.error = NULL, // TODO: why NULL?
};

static void schedule_ep_bound_process(struct backend_data_t *dev_data) {
	k_work_submit(&dev_data->ep_bound_work);
}

static void ep_bound_process(struct k_work *item)
{
	uint8_t message[2];
	struct backend_data_t *dev_data = CONTAINER_OF(item, struct backend_data_t, ep_bound_work);
	const struct icmsg_with_buf_config_t *conf = dev_data->conf;
	struct endpoint_data *ep = NULL;
	int i;
	int r;
	size_t name_len;
	uint8_t *buffer;

	/* Skip processing if ICMSG was not bounded yet. */
	if (!dev_data->icmsg_bounded) {
		return;
	}

	/* Find first unprocessed endpoint that was already registered locally. */
	for (i = 0; i < dev_data->ep_count; i++) {
		if (!dev_data->ep[i].bounded && dev_data->ep[i].cfg != NULL) {
			ep = &dev_data->ep[i];
			break;
		}
	}

	/* End processing if not found. */
	if (ep == NULL) {
		return;
	}

	ep->bounded = true;

	/* Register EP on remote */
	name_len = strlen(ep->name) + 1;
	r = alloc_tx_buffer(dev_data, name_len, &buffer);
	if (r >= 0) {
		memcpy(buffer, ep->name, name_len);
		message[0] = ep->local_id | ID_REGISTER_EP_FLAG;
		message[1] = r;
		r = icmsg_send(&conf->icmsg_config, &dev_data->icmsg_data, message, sizeof(message));
		/* ICMSG queue should have enough space for all blocks. */
		// TODO: assert(r == 0, "")
	} else {
		/* EP name cannot be bigger then entire allocable space. */
		// TODO: assert(true)
	}

	/* After the registration was send, EP is ready to send more data */
	if (ep->cfg->cb.bound != NULL) {
		ep->cfg->cb.bound(ep->cfg->priv);
	}

	/* Do next processing later */
	schedule_ep_bound_process(dev_data);
}

static int backend_init(const struct device *instance)
{
	const struct icmsg_with_buf_config_t *conf = instance->config;
	struct backend_data_t *dev_data = instance->data;

	dev_data->conf = conf;
	k_mutex_init(&dev_data->epts_mutex);
	k_work_init(&dev_data->ep_bound_work, ep_bound_process);
	return 0;
}

static int open(const struct device *instance)
{
	const struct icmsg_with_buf_config_t *conf = instance->config;
	struct backend_data_t *dev_data = instance->data;

	return icmsg_open(&conf->icmsg_config, &dev_data->icmsg_data, &cb, (void *)instance);
}

static struct endpoint_data *find_ep_by_name(struct backend_data_t *dev_data, const char *name)
{
	int i;

	for (i = 0; i < dev_data->ep_count; i++) {
		if (strcmp(dev_data->ep[i].name, name) == 0) {
			return &dev_data->ep[i];
		}
	}
	return NULL;
}

static int add_endpoint(struct backend_data_t *dev_data, const char *name, uint8_t remote_id, const struct ipc_ept_cfg *cfg)
{
	int r = 0;

	if (dev_data->ep_count < CONFIG_IPC_ICMSG_WITH_BUF_ENDPOINT_COUNT) {
		dev_data->ep[dev_data->ep_count].name = name;
		dev_data->ep[dev_data->ep_count].local_id = dev_data->ep_count;
		dev_data->ep[dev_data->ep_count].remote_id = remote_id;
		dev_data->ep[dev_data->ep_count].cfg = cfg;
		dev_data->ep_count++;
	} else {
		r = -ENOMEM;
	}

	return r;
}

static int register_ept(const struct device *instance, void **token,
			const struct ipc_ept_cfg *cfg)
{
	struct backend_data_t *dev_data = instance->data;
	struct endpoint_data *ep;
	const char *temp_name;
	int r = 0;

	k_mutex_lock(&dev_data->epts_mutex, K_FOREVER);

	ep = find_ep_by_name(dev_data, cfg->name); // TODO: consistent name: ep or ept?

	if (ep == NULL) {
		r = add_endpoint(dev_data, cfg->name, ID_INVALID, cfg);
		k_mutex_unlock(&dev_data->epts_mutex);
		if (r != 0) {
			return r;
		}
	} else {
		if (ep->cfg != NULL) {
			k_mutex_unlock(&dev_data->epts_mutex);
			return -EALREADY;
		}
		temp_name = ep->name;
		ep->name = cfg->name;
		ep->cfg = cfg;
		k_mutex_unlock(&dev_data->epts_mutex);
		// TODO: free_rx_buffer(temp_name);
	}
	schedule_ep_bound_process(dev_data);
	return r;
}


const static struct ipc_service_backend backend_ops = {
	.open_instance = open,
//	.register_endpoint = register_ept,
//	.send = send,
//	.get_tx_buffer = get_tx_buffer,
//	.drop_tx_buffer = drop_tx_buffer,
//	.send_nocopy = send_nocopy,
//	.hold_rx_buffer = hold_rx_buffer,
//	.release_rx_buffer = release_rx_buffer,
//	.open_instance = NULL,
	.register_endpoint = register_ept,
	.send = NULL,
	.get_tx_buffer = NULL,
	.drop_tx_buffer = NULL,
	.send_nocopy = NULL,
	.hold_rx_buffer = NULL,
	.release_rx_buffer = NULL,
};


#define BACKEND_CONFIG_POPULATE(i)						\
	{									\
		.icmsg_config = {				\
			.tx_shm_size = GET_ICMSG_SIZE(DT_REG_SIZE(DT_INST_PHANDLE(i, tx_region)), DT_INST_PROP(i, tx_blocks)),	\
			.tx_shm_addr = DT_REG_ADDR(DT_INST_PHANDLE(i, tx_region)) + GET_ICMSG_OFFSET(DT_REG_SIZE(DT_INST_PHANDLE(i, tx_region)), DT_INST_PROP(i, tx_blocks)),	\
			.rx_shm_size = GET_ICMSG_SIZE(DT_REG_SIZE(DT_INST_PHANDLE(i, rx_region)), DT_INST_PROP(i, rx_blocks)),	\
			.rx_shm_addr = DT_REG_ADDR(DT_INST_PHANDLE(i, rx_region)) + GET_ICMSG_OFFSET(DT_REG_SIZE(DT_INST_PHANDLE(i, rx_region)), DT_INST_PROP(i, rx_blocks)),	\
			.mbox_tx = MBOX_DT_CHANNEL_GET(DT_DRV_INST(i), tx),		\
			.mbox_rx = MBOX_DT_CHANNEL_GET(DT_DRV_INST(i), rx),		\
		},															\
		.tx_blocks_ptr = (uint8_t *)DT_REG_ADDR(DT_INST_PHANDLE(i, tx_region)),	\
		.rx_blocks_ptr = (uint8_t *)DT_REG_ADDR(DT_INST_PHANDLE(i, rx_region)),	\
		.tx_block_count = DT_INST_PROP(i, tx_blocks),				\
		.rx_block_count = DT_INST_PROP(i, rx_blocks),				\
		.tx_block_size = GET_BLOCK_SIZE(DT_REG_SIZE(DT_INST_PHANDLE(i, tx_region)), DT_INST_PROP(i, tx_blocks)),	\
		.rx_block_size = GET_BLOCK_SIZE(DT_REG_SIZE(DT_INST_PHANDLE(i, rx_region)), DT_INST_PROP(i, tx_blocks)),	\
		.tx_bitmap = &backend_bitmap_##i,	\
	}

#define DEFINE_BACKEND_DEVICE(i)						\
	SYS_BITARRAY_DEFINE_STATIC(backend_bitmap_##i, DT_INST_PROP(i, tx_blocks)); \
	static const struct icmsg_with_buf_config_t backend_config_##i =			\
		BACKEND_CONFIG_POPULATE(i);					\
	BUILD_ASSERT(GET_BLOCK_SIZE(DT_REG_SIZE(DT_INST_PHANDLE(i, tx_region)), DT_INST_PROP(i, tx_blocks)) > BLOCK_ALIGNMENT, "TX region is too small for provided number of blocks"); \
	BUILD_ASSERT(GET_BLOCK_SIZE(DT_REG_SIZE(DT_INST_PHANDLE(i, rx_region)), DT_INST_PROP(i, rx_blocks)) > BLOCK_ALIGNMENT, "RX region is too small for provided number of blocks"); \
	static struct backend_data_t backend_data_##i;				\
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

#if defined(CONFIG_IPC_SERVICE_BACKEND_ICMSG_ME_SHMEM_RESET) // TODO: implement
#define BACKEND_CONFIG_DEFINE(i) BACKEND_CONFIG_POPULATE(i),
static int shared_memory_prepare(void)
{
	const struct icmsg_config_t *backend_config;
	const struct icmsg_config_t backend_configs[] = {
		DT_INST_FOREACH_STATUS_OKAY(BACKEND_CONFIG_DEFINE)
	};

	for (backend_config = backend_configs;
	     backend_config < backend_configs + ARRAY_SIZE(backend_configs);
	     backend_config++) {
		icmsg_clear_tx_memory(backend_config);
		icmsg_clear_rx_memory(backend_config);
	}

	return 0;
}

SYS_INIT(shared_memory_prepare, PRE_KERNEL_1, 1);
#endif /* CONFIG_IPC_SERVICE_BACKEND_ICMSG_ME_SHMEM_RESET */

#ifdef DRAFT_PSEUDO_CODE

void ep_bound_process() {
    // called from work queue
    if (data->bound_by_icmsg) {
        for (each_ep) {
            if (!ep->processed && ep->cfg != NULL) {
                ep->processed = true;
                buf = alloc_buffer();
                buf = ep->name;
                icmsg_send([ep_id | 128, buf_index]);
                call_bound_callback();
                send_work__ep_bound_process();
                break;
            }
        }
    }
}


void register_ep(data) {
    if (!endpoint_found) {
        add_new_ep(endpoint);
    } else {
        drop_tx_buffer(ep->name);
    }
    ep->cfg = cfg;
    send_work__ep_bound_process();
}

void on_icmsg_bound() {
    data->bound_by_icmsg = true;
    send_work__ep_bound_process();
}

void get_tx_buffer() {
    if (too_big) return -ENOMEM;
    while (1) {
        buf = bitmap_alloc(total_size / buf_size);
        if (buf == -ENOMEM) {
            sem_take();
            continue;
        }
    }
    if (sem_taken) {
        sem_give();
    }
    buf[0] = total_size;
    return buf[1];
}

void drop_tx_buffer() {
    total_size = buf[0];
    bitmap_free(buf, total_size / buf_size);
    sem_give();
}

void send_no_copy() {
    flush_cache(buf);
    icmsg_send([ep->id, buf_index]);
}

void send() {
    buf = alloc_buffer();
    memcpy(buf, ep->name);
    send_no_copy();
}

void on_received() {
    if (packet->id == 255) {
        drop_tx_buffer();
    } else if (packet->id & 128) {
        name = buffer;
        ep = find_ep_by_name(name);
        if (ep == NULL) {
            ep = add_new_ep(endpoint);
            ep->name = buffer;
        }
        ep->remote = packet->id & 127;
        data->remote_to_local[packet->id & 127] = ep->id;
    } else {
        ep = data->endpoints[data->remote_to_local[packet->id & 127]];
        read_cache();
        ep->callbacks->received(buffer->data, buffer->size);
        if (!(data->buffers_flags[buffer_index] & HOLD_BUFFER)) {
            icmsg_send([255, buf_index]);
        }
    }
}

void hold_rx_buffer() {
    data->buffers_flags[buffer_index] |= HOLD_BUFFER;
}

void free_rx_buffer() {
    icmsg_send([255, buf_index]);
}


#endif

