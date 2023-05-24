
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/sys/bitarray.h>
#include <zephyr/ipc/icmsg.h>
#include <zephyr/ipc/ipc_service_backend.h>

#define DT_DRV_COMPAT	zephyr_ipc_icmsg_with_buf

#define ID_INVALID 0x7F
#define ID_REGISTER_EP_FLAG 0x80

#define CONFIG_IPC_ICMSG_WITH_BUF_ENDPOINT_COUNT 4 // TODO: move to kconfig

/*
 * Shared memory layout:
 *
 *     | block 0 | block 1 | ... | block N-1 | Icmsg queue |
 *
 *     Icmsg queue contains enough space to hold at least the same number of
 *     messages as number of blocks on both sides, so it will never overflow.
 *
 * Block layout:
 *
 *     | size_t data_size | data ... |
 *
 *     If buffer spawns more than one block, only the first block contains
 *     'data_size'. 'data_size' value does not include the 'data_size' field.
 *
 * Icmsg messages:
 *
 *     - Endpoint registration (2 bytes):
 *       uint8_t index of sender block containing null-terminated endpoint name
 *       uint8_t endpoint id with highest bit set
 *
 *     - Data (2 bytes):
 *       uint8_t sender block index
 *       uint8_t endpoint id
 *
 *     - Buffer deallocation request (1 byte)
 *       uint8_t receiver block index
 *
 *     Endpoint ids are different on each side. Each side has an array that
 *     maps remote and local ids. It is created when 'endpoint registration' messages
 *     is received, because it is the first message at given endpoint and it has
 *     an endpoint name.
*/

#define BLOCK_ALIGNMENT sizeof(size_t)
#define BYTES_PER_ICMSG_DATA_MESSAGE 4 // TODO: check it
#define BYTES_PER_ICMSG_FREE_MESSAGE 3
#define ICMSG_BUFFER_OVERHEAD 32 // TODO: check it

#define GET_ICMSG_MIN_SIZE(data_blocks, free_blocks) \
	(ICMSG_BUFFER_OVERHEAD + \
	BYTES_PER_ICMSG_DATA_MESSAGE * (data_blocks) + \
	BYTES_PER_ICMSG_FREE_MESSAGE * (free_blocks))

#define GET_BLOCK_SIZE(total_size, data_blocks, free_blocks) ROUND_DOWN(   \
	((total_size) - GET_ICMSG_MIN_SIZE(data_blocks, free_blocks)) / \
	(data_blocks), BLOCK_ALIGNMENT)

#define GET_ICMSG_OFFSET(total_size, data_blocks, free_blocks) \
	(GET_BLOCK_SIZE(total_size, data_blocks, free_blocks) * (data_blocks))

#define GET_ICMSG_SIZE(total_size, data_blocks, free_blocks) \
	((total_size) - GET_ICMSG_OFFSET(total_size, data_blocks, free_blocks))

struct icmsg_with_buf_config {
	struct icmsg_config_t icmsg_config;
	uint8_t *tx_blocks_ptr;
	uint8_t *rx_blocks_ptr;
	uint16_t tx_block_count;
	uint16_t rx_block_count;
	size_t tx_block_size;
	size_t rx_block_size;
	sys_bitarray_t *tx_usage_bitmap;
	sys_bitarray_t *rx_hold_bitmap;
};

struct endpoint_data {
	const struct ipc_ept_cfg *cfg;
	const uint8_t *reg_ept_buffer;
	uint8_t local_id;
	bool bounded;
};

struct backend_data {
	const struct icmsg_with_buf_config* conf;
	struct icmsg_data_t icmsg_data;
	struct k_mutex epts_mutex;
	struct k_work ep_bound_work;
	struct k_sem block_wait_sem;
	struct endpoint_data ep[CONFIG_IPC_ICMSG_WITH_BUF_ENDPOINT_COUNT];
	uint8_t remote_to_local_id[CONFIG_IPC_ICMSG_WITH_BUF_ENDPOINT_COUNT];
	uint8_t ep_count;
	bool icmsg_bounded;
};

struct block_header {
	size_t size;
	uint8_t data[];
};

/**
 * Allocate buffer for transmission
 *
 * @param[in] dev_data	Device data
 * @param[in] size	Required size of the buffer. If zero, first free block is
 * 			allocated and all subsequent free blocks.
 * @param[out] block	First allocated block. Block header contains actually allocated
 * 			bytes, so it have to be adjusted before sending.
 * @param[in] timeout	Timeout
 *
 * @return		Positive index of the first allocated block or negative error
 * @retval -EINVAL	If requested size is bigger than entire allocable space
 * @retval -ENOSPC	If timeout was K_NO_WAIT and there was not enough space
 * @retval -EAGAIN	If timeout occurred
 */
static int alloc_tx_buffer(struct backend_data *dev_data, size_t size,
			   struct block_header **block, k_timeout_t timeout)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	size_t total_size = size + offsetof(struct block_header, data);
	size_t num_blocks = (total_size + conf->tx_block_size - 1) / conf->tx_block_size;
	bool sem_taken = false;
	size_t block_index;
	size_t next_bit;
	int prev_bit_val;
	int r;

	do {
		/* Try to allocate specified number of blocks */
		r = sys_bitarray_alloc(conf->tx_usage_bitmap, num_blocks, &block_index);
		if (r == -ENOSPC && !K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
			/* Wait for deallocation if there is no enough space and exit loop
			 * on timeout */
			r = k_sem_take(&dev_data->block_wait_sem, timeout);
			if (r < 0) {
				break;
			}
			sem_taken = true;
		} else {
			/* Exit loop if space was allocated or other error occurred */
			break;
		}
	} while (true);

	/* If semaphore was taken, give it back because this thread does not
	 * necessary took all available space, so other thread may need it. */
	if (sem_taken) {
		k_sem_give(&dev_data->block_wait_sem);
	}

	if (r < 0) {
		return r;
	}

	/* If size is 0 try to allocate more blocks after already allocated. */
	if (size == 0) {
		prev_bit_val = 0;
		for (next_bit = block_index + 1; next_bit < conf->tx_block_count;
		     next_bit++) {
			r = sys_bitarray_test_and_set_bit(conf->tx_usage_bitmap, next_bit,
							  &prev_bit_val);
			__ASSERT_NO_MSG(r == 0);
			if (prev_bit_val) {
				break;
			}
		}
		num_blocks = next_bit - block_index;
	}

	/* Calculate block pointer and adjust size to actually allocated space (which is
	 * not less that requested). */
	*block = (struct block_header *)(conf->tx_blocks_ptr +
					 conf->tx_block_size * block_index);
	(*block)->size = conf->tx_block_size * num_blocks - offsetof(struct block_header,
								     data);

	return block_index;
}

/**
 * Free all or part of the blocks occupied by the buffer.
 *
 * @param[in] dev_data	Device data
 * @param[in] buffer	Buffer to free
 * @param[in] new_size	If less than zero, free all blocks, otherwise reduce size to this
 *			value and update size in block header.
 *
 * @returns		Positive block index where the buffer starts or negative error
 * @retval -EINVAL	If invalid buffer was provided or size is greater than already
 *			allocated size.
 */
static int free_tx_buffer(struct backend_data *dev_data, const uint8_t *buffer,
			  int new_size)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	struct block_header *block;
	size_t size;
	size_t num_blocks;
	size_t total_size;
	size_t new_total_size;
	size_t new_num_blocks;
	size_t block_free_index;
	size_t block_index;
	int r;

	/* Calculate and validate block pointer and index */
	block_index = (buffer - conf->tx_blocks_ptr) / conf->tx_block_size;
	block = (struct block_header *)(conf->tx_blocks_ptr +
					conf->tx_block_size * block_index);
	if (block_index >= conf->tx_block_count || buffer != block->data) {
		return -EINVAL;
	}

	/* Calculate and validate number of blocks. Size is taken from shared
	 * memory, so we cannot fully trust it. */
	size = block->size;
	total_size = size + offsetof(struct block_header, data);
	num_blocks = (total_size + conf->tx_block_size - 1) / conf->tx_block_size;
	if (num_blocks >= conf->tx_block_count ||
	    block_index + num_blocks > conf->tx_block_count) {
		return -EINVAL;
	}

	if (new_size >= 0) {
		/* Calculate and validate new values */
		new_total_size = new_size + offsetof(struct block_header, data);
		new_num_blocks = (new_total_size + conf->tx_block_size - 1) /
				 conf->tx_block_size;
		if (new_num_blocks > num_blocks) {
			return -EINVAL;
		}
		/* Update actual buffer size and blocks to deallocate */
		block->size = new_size;
		block_free_index = block_index + new_num_blocks;
		num_blocks = num_blocks - new_num_blocks;
	} else {
		/* If size is negative, free all blocks */
		block_free_index = block_index;
	}

	if (num_blocks > 0) {
		/* Free bits in the bitmap */
		r = sys_bitarray_free(conf->tx_usage_bitmap, num_blocks,
				      block_free_index);
		if (r < 0) {
			return r;
		}

		/* Wake up all waiting threads */
		k_sem_give(&dev_data->block_wait_sem);
	}

	return block_index;
}

/**
 * Put endpoint bound processing into system workqueue.
 *
 * @param[in] dev_data	Device data
 */
static void schedule_ept_bound_process(struct backend_data *dev_data) {
	k_work_submit(&dev_data->ep_bound_work);
}

/**
 * Find endpoint that was registered only locally with name that matches name
 * contained in the endpoint registration buffer received from remote. This function
 * must be called when epts_mutex is locked.
 * 
 * @param[in] dev_data		Device data
 * @param[in] reg_ept_buffer	Endpoint registration buffer received from the remote.
 * 
 * @return	Found endpoint data or NULL if not found
 */
static struct endpoint_data *find_ept_registered_locally(struct backend_data *dev_data,
							 const char *reg_ept_buffer)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	int i;
	const char *buffer_end = (const char *)conf->icmsg_config.rx_shm_addr;
	size_t name_size;

	/* Requested name is in shared memory, so we have to assume that it
	 * can be corrupted. Extra care must be taken to avoid out of
	 * bounds reads. */
	name_size = strnlen(reg_ept_buffer, buffer_end - reg_ept_buffer - 1) + 1;

	for (i = 0; i < dev_data->ep_count; i++) {
		if (dev_data->ep[i].cfg != NULL &&
		    strncmp(dev_data->ep[i].cfg->name, reg_ept_buffer, name_size) == 0) {
			return &dev_data->ep[i];
		}
	}
	return NULL;
}

/**
 * Find endpoint that was registered only by the remote. This function
 * must be called when epts_mutex is locked.
 * 
 * @param[in] dev_data	Device data
 * @param[in] name	Name of the endpoint
 * 
 * @return	Found endpoint data or NULL if not found
 */
static struct endpoint_data *find_ept_registered_remotely(struct backend_data *dev_data,
							  const char *name)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	int i;
	size_t name_size = strlen(name) + 1;
	size_t max_chars;
	const char *buffer_end = (const char *)conf->icmsg_config.rx_shm_addr;
	const char *ept_name;

	/* Name of an endpoint registered only remotely is in shared memory,
	 * so we have to assume that it can be corrupted. Extra care must be
	 * taken to avoid out of bounds reads. */
	for (i = 0; i < dev_data->ep_count; i++) {
		ept_name = dev_data->ep[i].reg_ept_buffer;
		if (ept_name == NULL) {
			continue;
		}
		max_chars = buffer_end - ept_name;
		max_chars = MIN(max_chars, name_size);
		if (dev_data->ep[i].cfg == NULL &&
		    strncmp(ept_name, name, max_chars) == 0) {
			return &dev_data->ep[i];
		}
	}
	return NULL;
}

/**
 * Request deallocation of receive buffer. This function will just sends buffer
 * deallocation message over Icmsg.
 * 
 * @param[in] dev_data	Device data
 * @param[in] buffer	Buffer to deallocate
 * 
 * @return	zero or Icmsg send error
 */
static int free_rx_buffer(struct backend_data *dev_data, const uint8_t *buffer)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	uint8_t message[1];
	size_t block_index;

	block_index = (buffer - conf->tx_blocks_ptr) / conf->tx_block_size;

	message[0] = block_index;

	return icmsg_send(&conf->icmsg_config, &dev_data->icmsg_data, message,
			  sizeof(message));
}

/**
 * Add endpoint data to dev_data.
 *
 * @param[in] dev_data		Device data
 * @param[in] cfg		Endpoint configuration, can be NULL if endpoint was not
 *				configured locally yet.
 * @param[in] reg_ept_buffer	Remote endpoint registration buffer containing endpoint
 *				name. Can be NULL if endpoint was not configured
 *				remotely yet.
 * @retval 0		Success
 * @retval -ENOMEM	No more space available in endpoints array
 */
static int add_endpoint(struct backend_data *dev_data, const struct ipc_ept_cfg *cfg,
			const uint8_t* reg_ept_buffer)
{
	int r = 0;

	if (dev_data->ep_count < CONFIG_IPC_ICMSG_WITH_BUF_ENDPOINT_COUNT) {
		r = dev_data->ep_count;
		dev_data->ep[dev_data->ep_count].cfg = cfg;
		dev_data->ep[dev_data->ep_count].local_id = dev_data->ep_count;
		dev_data->ep[dev_data->ep_count].reg_ept_buffer = reg_ept_buffer;
		dev_data->ep[dev_data->ep_count].bounded = false;
		dev_data->ep_count++;
	} else {
		r = -ENOMEM;
	}

	return r;
}

/**
 * Handles endpoint registration received from the remote.
 *
 * @param[in] dev_data		Device data
 * @param[in] remote_ept_id	Remote endpoint id
 * @param[in] reg_ept_buffer	RX buffer containing the name
 *
 * @retval 0		Success
 * @retval -ENOMEM	No more space available in endpoints array
 */
static int register_remote_ept(struct backend_data *dev_data, size_t remote_ept_id,
			       const char *reg_ept_buffer)
{
	struct endpoint_data *ept;
	int r = 0;
	bool hold_buf = false;

	k_mutex_lock(&dev_data->epts_mutex, K_FOREVER);

	/* If endpoint was not registered locally, add it without configuration and
	 * hold RX buffer. It will be used to find right endpoint during local
	 * registration. */
	ept = find_ept_registered_locally(dev_data, reg_ept_buffer);
	if (ept == NULL) {
		r = add_endpoint(dev_data, NULL, reg_ept_buffer);
		if (r < 0) {
			goto exit;
		}
		ept = &dev_data->ep[r];
		hold_buf = true;
	}

	/* Save information needed to map local and remote endpoint id. */
	dev_data->remote_to_local_id[remote_ept_id] = ept->local_id;

exit:
	k_mutex_unlock(&dev_data->epts_mutex);
	if (!hold_buf) {
		free_rx_buffer(dev_data, reg_ept_buffer);
	}
	return r;
}

/**
 * Handles data received from the remote.
 *
 * @param[in] dev_data		Device data
 * @param[in] remote_ept_id	Remote endpoint id
 * @param[in] block_index	First RX block index
 * @param[in] block		First RX block pointer
 *
 * @retval 0		Success
 * @retval -EINVAL	Cannot map remote id to local id, for example when invalid remote
 *			endpoint registration occurred.
 */
static int received_ept_data(struct backend_data *dev_data, size_t remote_ept_id,
			     size_t block_index, const struct block_header *block)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	size_t local_ept_id;
	struct endpoint_data *ept;
	int bit_val = 0;

	/* Map remote id to local id and validate */
	local_ept_id = dev_data->remote_to_local_id[remote_ept_id]; // TODO: set mapping to ID_INVALID
	if (local_ept_id >= CONFIG_IPC_ICMSG_WITH_BUF_ENDPOINT_COUNT) {
		return -EINVAL;
	}

	/* Clear bit. If cleared, specific block will not be hold after the callback. */
	sys_bitarray_clear_bit(conf->rx_hold_bitmap, block_index);

	/* Call the endpoint callback. It can set the hold bit. */
	ept = &dev_data->ep[local_ept_id];
	ept->cfg->cb.received(block->data, block->size, ept->cfg->priv);

	/* If the bit is still cleared, request deallocation of the buffer. */
	sys_bitarray_test_bit(conf->rx_hold_bitmap, block_index, &bit_val);
	if (!bit_val) {
		free_rx_buffer(dev_data, block->data);
	}

	return 0;
}

/**
 * Callback called by Icmsg that handles message (data or ept registration) received
 * from the remote.
 *
 * @param[in] data	Message received from the Icmsg
 * @param[in] len	Number of bytes of data
 * @param[in] priv	Opaque pointer to device instance
 */
static void received(const void *data, size_t len, void *priv)
{
	const struct device *instance = priv;
	struct backend_data *dev_data = instance->data;
	const struct icmsg_with_buf_config *conf = instance->config;
	const uint8_t *message = data;
	size_t remote_ept_id;
	size_t block_index = 0;
	struct block_header *block;
	int r;

	if (len == 1) {

		/* Calculate and validate first block index and pointer. */
		block_index = message[0];
		if (block_index >= conf->tx_block_count) {
			r = -EINVAL;
			goto exit;
		}
		block = (struct block_header *)(conf->tx_blocks_ptr +
						conf->tx_block_size * block_index); // TODO: check if calculating and validating blocks can be in one place

		/* Just deallocate the buffer */
		r = free_tx_buffer(dev_data, block->data, -1);

	} else if (len == 2) {

		/* Calculate and validate block index and pointer. */
		block_index = message[0];
		if (block_index >= conf->rx_block_count) {
			r = -EINVAL;
			goto exit;
		}
		block = (struct block_header *)(conf->rx_blocks_ptr +
						conf->rx_block_size * block_index);

		/* Get and validate endpoint id. */
		remote_ept_id = message[1] & ~ID_REGISTER_EP_FLAG;
		if (remote_ept_id >= CONFIG_IPC_ICMSG_WITH_BUF_ENDPOINT_COUNT) {
			r = -EINVAL;
			goto exit;
		}

		/* Make sure that cache is up to date. */
		// TODO: read cache
		// TODO: Check that cache is updated on write

		/* Handle the message */
		if (message[1] & ID_REGISTER_EP_FLAG) {
			r = register_remote_ept(dev_data, remote_ept_id, block->data);
		} else {
			r = received_ept_data(dev_data, remote_ept_id, block_index, block);
		}

	} else {
		r = -EINVAL;
	}

exit:
	// TODO: handle errors on
}

/**
 * Callback called when Icmsg is bound.
 *
 * @param[in] priv	Opaque pointer to device instance
 */
static void bound(void *priv)
{
	const struct device *instance = priv;
	struct backend_data *dev_data = instance->data;

	dev_data->icmsg_bounded = true;
	schedule_ept_bound_process(dev_data);
}

static const struct ipc_service_cb cb = {
	.bound = bound,
	.received = received,
	.error = NULL,
};

static void ep_bound_process(struct k_work *item)
{
	uint8_t message[2];
	struct backend_data *dev_data = CONTAINER_OF(item, struct backend_data, ep_bound_work);
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	struct endpoint_data *ep = NULL;
	int i;
	int r;
	size_t name_len;
	struct block_header *block;

	/* Skip processing if Icmsg was not bounded yet. */
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
	name_len = strlen(ep->cfg->name) + 1;
	r = alloc_tx_buffer(dev_data, name_len, &block, K_FOREVER);
	if (r >= 0) {
		memcpy(block->data, ep->cfg->name, name_len);
		message[0] = r;
		message[1] = ep->local_id | ID_REGISTER_EP_FLAG;
		r = icmsg_send(&conf->icmsg_config, &dev_data->icmsg_data, message, sizeof(message));
		/* Icmsg queue should have enough space for all blocks. */
		__ASSERT_NO_MSG(r == 0);
	} else {
		/* EP name cannot be bigger then entire allocable space. */
		__ASSERT_NO_MSG(false);
	}

	/* After the registration was send, EP is ready to send more data */
	if (ep->cfg->cb.bound != NULL) {
		ep->cfg->cb.bound(ep->cfg->priv);
	}

	/* Do next processing later */
	schedule_ept_bound_process(dev_data);
}

static int backend_init(const struct device *instance)
{
	const struct icmsg_with_buf_config *conf = instance->config;
	struct backend_data *dev_data = instance->data;

	dev_data->conf = conf;
	k_mutex_init(&dev_data->epts_mutex);
	k_work_init(&dev_data->ep_bound_work, ep_bound_process);
	k_sem_init(&dev_data->block_wait_sem, 0, 1);
	return 0;
}

static int open(const struct device *instance)
{
	const struct icmsg_with_buf_config *conf = instance->config;
	struct backend_data *dev_data = instance->data;

	return icmsg_open(&conf->icmsg_config, &dev_data->icmsg_data, &cb, (void *)instance);
}

static int register_ept(const struct device *instance, void **token,
			const struct ipc_ept_cfg *cfg)
{
	struct backend_data *dev_data = instance->data;
	struct endpoint_data *ep;
	int r = 0;

	k_mutex_lock(&dev_data->epts_mutex, K_FOREVER);

	ep = find_ept_registered_remotely(dev_data, cfg->name); // TODO: consistent name: ep or ept?

	if (ep == NULL) {
		r = add_endpoint(dev_data, cfg, NULL);
		k_mutex_unlock(&dev_data->epts_mutex);
		if (r < 0) {
			return r;
		}
		ep = &dev_data->ep[r];
	} else {
		ep->cfg = cfg;
		k_mutex_unlock(&dev_data->epts_mutex);
		free_rx_buffer(dev_data, ep->reg_ept_buffer);
	}
	*token = (void *)ep;
	schedule_ept_bound_process(dev_data);
	return r;
}

static int send(const struct device *instance, void *token,
		const void *msg, size_t len)
{
	const struct icmsg_with_buf_config *conf = instance->config;
	struct backend_data *dev_data = instance->data;
	struct endpoint_data *ep = token;
	struct block_header* block;
	uint8_t message[2];
	int r;

	r = alloc_tx_buffer(dev_data, len, &block, K_FOREVER);
	if (r < 0) {
		return r;
	}

	block->size = len;
	memcpy(block->data, msg, len);

	message[0] = r;
	message[1] = ep->local_id;

	r = icmsg_send(&conf->icmsg_config, &dev_data->icmsg_data, message, sizeof(message));

	if (r < 0) {
		free_tx_buffer(dev_data, block->data, -1);
	}

	return r;
}

static int get_tx_buffer(const struct device *instance, void *token,
			 void **data, uint32_t *user_len, k_timeout_t wait)
{
	struct backend_data *dev_data = instance->data;
	struct block_header* block;
	int r;

	r = alloc_tx_buffer(dev_data, *user_len, &block, wait);
	if (r >= 0) {
		*data = block->data;
		*user_len = block->size;
		r = 0;
	}

	return r;
}

static int drop_tx_buffer(const struct device *instance, void *token,
			  const void *data)
{
	struct backend_data *dev_data = instance->data;

	return free_tx_buffer(dev_data, data, -1);
}

static int send_nocopy(const struct device *instance, void *token,
			const void *data, size_t len)
{
	const struct icmsg_with_buf_config *conf = instance->config;
	struct backend_data *dev_data = instance->data;
	struct endpoint_data *ep = token;
	uint8_t message[2];
	int r;

	/* Actual size may be smaller than requested, so shrink if possible. */
	r = free_tx_buffer(dev_data, data, len);
	if (r < 0) {
		free_tx_buffer(dev_data, data, -1);
		return r;
	}

	/* Prepare data message [local_ept_id, starting_block_index]. */
	message[0] = r;
	message[1] = ep->local_id;

	r = icmsg_send(&conf->icmsg_config, &dev_data->icmsg_data, message, sizeof(message));

	if (r < 0) {
		free_tx_buffer(dev_data, data, -1);
	}

	return r;
}

static int hold_rx_buffer(const struct device *instance, void *token, void *data)
{
	const struct icmsg_with_buf_config *conf = instance->config;
	size_t block_index;
	uint8_t *buffer = data;

	block_index = (buffer - conf->tx_blocks_ptr) / conf->tx_block_size;

	return sys_bitarray_set_bit(conf->rx_hold_bitmap, block_index);
}

static int release_rx_buffer(const struct device *instance, void *token, void *data)
{
	struct backend_data *dev_data = instance->data;

	return free_rx_buffer(dev_data, (uint8_t *)data);
}

const static struct ipc_service_backend backend_ops = {
	.open_instance = open,
	.register_endpoint = register_ept,
	.send = send,
	.get_tx_buffer = get_tx_buffer,
	.drop_tx_buffer = drop_tx_buffer,
	.send_nocopy = send_nocopy,
	.hold_rx_buffer = hold_rx_buffer,
	.release_rx_buffer = release_rx_buffer,
};


#define BACKEND_CONFIG_POPULATE(i)						\
	{									\
		.icmsg_config = {				\
			.tx_shm_size = GET_ICMSG_SIZE(DT_REG_SIZE(DT_INST_PHANDLE(i, tx_region)), DT_INST_PROP(i, tx_blocks), DT_INST_PROP(i, rx_blocks)),	\
			.tx_shm_addr = DT_REG_ADDR(DT_INST_PHANDLE(i, tx_region)) + GET_ICMSG_OFFSET(DT_REG_SIZE(DT_INST_PHANDLE(i, tx_region)), DT_INST_PROP(i, tx_blocks), DT_INST_PROP(i, rx_blocks)),	\
			.rx_shm_size = GET_ICMSG_SIZE(DT_REG_SIZE(DT_INST_PHANDLE(i, rx_region)), DT_INST_PROP(i, rx_blocks), DT_INST_PROP(i, tx_blocks)),	\
			.rx_shm_addr = DT_REG_ADDR(DT_INST_PHANDLE(i, rx_region)) + GET_ICMSG_OFFSET(DT_REG_SIZE(DT_INST_PHANDLE(i, rx_region)), DT_INST_PROP(i, rx_blocks), DT_INST_PROP(i, tx_blocks)),	\
			.mbox_tx = MBOX_DT_CHANNEL_GET(DT_DRV_INST(i), tx),		\
			.mbox_rx = MBOX_DT_CHANNEL_GET(DT_DRV_INST(i), rx),		\
		},															\
		.tx_blocks_ptr = (uint8_t *)DT_REG_ADDR(DT_INST_PHANDLE(i, tx_region)),	\
		.rx_blocks_ptr = (uint8_t *)DT_REG_ADDR(DT_INST_PHANDLE(i, rx_region)),	\
		.tx_block_count = DT_INST_PROP(i, tx_blocks),				\
		.rx_block_count = DT_INST_PROP(i, rx_blocks),				\
		.tx_block_size = GET_BLOCK_SIZE(DT_REG_SIZE(DT_INST_PHANDLE(i, tx_region)), DT_INST_PROP(i, tx_blocks), DT_INST_PROP(i, rx_blocks)),	\
		.rx_block_size = GET_BLOCK_SIZE(DT_REG_SIZE(DT_INST_PHANDLE(i, rx_region)), DT_INST_PROP(i, rx_blocks), DT_INST_PROP(i, tx_blocks)),	\
		.tx_usage_bitmap = &tx_usage_bitmap_##i,	\
		.rx_hold_bitmap = &rx_hold_bitmap_##i,	\
	}

#define DEFINE_BACKEND_DEVICE(i)						\
	SYS_BITARRAY_DEFINE_STATIC(tx_usage_bitmap_##i, DT_INST_PROP(i, tx_blocks)); \
	SYS_BITARRAY_DEFINE_STATIC(rx_hold_bitmap_##i, DT_INST_PROP(i, rx_blocks)); \
	static const struct icmsg_with_buf_config backend_config_##i =			\
		BACKEND_CONFIG_POPULATE(i);					\
	BUILD_ASSERT(GET_BLOCK_SIZE(DT_REG_SIZE(DT_INST_PHANDLE(i, tx_region)), DT_INST_PROP(i, tx_blocks), DT_INST_PROP(i, rx_blocks)) > BLOCK_ALIGNMENT, "TX region is too small for provided number of blocks"); \
	BUILD_ASSERT(GET_BLOCK_SIZE(DT_REG_SIZE(DT_INST_PHANDLE(i, rx_region)), DT_INST_PROP(i, rx_blocks), DT_INST_PROP(i, tx_blocks)) > BLOCK_ALIGNMENT, "RX region is too small for provided number of blocks"); \
	static struct backend_data backend_data_##i;				\
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
