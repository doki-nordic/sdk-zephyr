
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/sys/bitarray.h>
#include <zephyr/ipc/icmsg.h>
#include <zephyr/ipc/ipc_service_backend.h>
#include <zephyr/cache.h>

#define DT_DRV_COMPAT zephyr_ipc_icmsg_with_buf

#define ID_INVALID 0x7F
#define ID_REGISTER_EP_FLAG 0x80

#define BLOCK_ALIGNMENT sizeof(size_t)
#define BYTES_PER_ICMSG_DATA_MESSAGE 8
#define BYTES_PER_ICMSG_FREE_MESSAGE 8
#define ICMSG_BUFFER_OVERHEAD (sizeof(struct spsc_pbuf) + 2 * BYTES_PER_ICMSG_DATA_MESSAGE)

/*
 * Shared memory layout:
 *
 *     | ICMsg queue | block 0 | block 1 | ... | block N-1 |
 *
 *     ICMsg queue contains enough space to hold at least the same number of
 *     messages as number of blocks on both sides, so it will never overflow.
 *
 * Block layout:
 *
 *     | size_t data_size | data ... |
 *
 *     If buffer spawns more than one block, only the first block contains
 *     'data_size'. 'data_size' value does not include the 'data_size' field.
 *
 * ICMsg messages:
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

struct ept_data {
	const struct ipc_ept_cfg *cfg;
	const uint8_t *reg_ept_buffer;
	uint8_t local_id;
	bool bounded;
};

struct backend_data {
	const struct icmsg_with_buf_config* conf;
	struct icmsg_data_t icmsg_data;
	struct k_mutex mutex;
	struct k_work ep_bound_work;
	struct k_sem block_wait_sem;
	struct ept_data ept[CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP];
	uint8_t remote_to_local_id[CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP];
	uint8_t ept_count;
	bool icmsg_bounded;
};

struct block_header {
	size_t size;
	uint8_t data[];
};

/**
 * Special value for 'icmsg_send_wrapper' function indicating that second byte is empty
 * which means that message is one-byte long.
*/
#define ICMSG_SEND_WRAPPER_EMPTY 0x100

/**
 * Send data with ICmsg with mutex locked. Mutex must be locked because ICmsg may return
 * error on concurrent invocations even when there is enough space in queue.
 */
static int icmsg_send_wrapper(struct backend_data *dev_data, uint8_t b0, uint16_t b1)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	uint8_t message[2] = { b0, (uint8_t)b1 };
	int r;

	k_mutex_lock(&dev_data->mutex, K_FOREVER);
	r = icmsg_send(&conf->icmsg_config, &dev_data->icmsg_data, message,
		       b1 == ICMSG_SEND_WRAPPER_EMPTY ? 1 : 2);
	k_mutex_unlock(&dev_data->mutex);
	return r;
}

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
static void schedule_ept_bound_process(struct backend_data *dev_data)
{
	k_work_submit(&dev_data->ep_bound_work);
}

/**
 * Find endpoint that was registered only locally with name that matches name
 * contained in the endpoint registration buffer received from remote. This function
 * must be called when mutex is locked.
 * 
 * @param[in] dev_data		Device data
 * @param[in] reg_ept_buffer	Endpoint registration buffer received from the remote.
 * 
 * @return	Found endpoint data or NULL if not found
 */
static struct ept_data *find_ept_registered_locally(struct backend_data *dev_data,
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

	for (i = 0; i < dev_data->ept_count; i++) {
		if (dev_data->ept[i].cfg != NULL &&
		    strncmp(dev_data->ept[i].cfg->name, reg_ept_buffer, name_size) == 0) {
			return &dev_data->ept[i];
		}
	}
	return NULL;
}

/**
 * Find endpoint that was registered only by the remote. This function
 * must be called when mutex is locked.
 * 
 * @param[in] dev_data	Device data
 * @param[in] name	Name of the endpoint
 * 
 * @return	Found endpoint data or NULL if not found
 */
static struct ept_data *find_ept_registered_remotely(struct backend_data *dev_data,
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
	for (i = 0; i < dev_data->ept_count; i++) {
		ept_name = dev_data->ept[i].reg_ept_buffer;
		if (ept_name == NULL) {
			continue;
		}
		max_chars = buffer_end - ept_name;
		max_chars = MIN(max_chars, name_size);
		if (dev_data->ept[i].cfg == NULL &&
		    strncmp(ept_name, name, max_chars) == 0) {
			return &dev_data->ept[i];
		}
	}
	return NULL;
}

/**
 * Request deallocation of receive buffer. This function will just sends buffer
 * deallocation message over ICMsg.
 * 
 * @param[in] dev_data	Device data
 * @param[in] buffer	Buffer to deallocate
 * 
 * @return	zero or ICMsg send error
 */
static int free_rx_buffer(struct backend_data *dev_data, const uint8_t *buffer)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	size_t block_index;

	block_index = (buffer - conf->rx_blocks_ptr) / conf->rx_block_size;

	return icmsg_send_wrapper(dev_data, block_index, ICMSG_SEND_WRAPPER_EMPTY);
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

	if (dev_data->ept_count < CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP) {
		r = dev_data->ept_count;
		dev_data->ept[dev_data->ept_count].cfg = cfg;
		dev_data->ept[dev_data->ept_count].local_id = dev_data->ept_count;
		dev_data->ept[dev_data->ept_count].reg_ept_buffer = reg_ept_buffer;
		dev_data->ept[dev_data->ept_count].bounded = false;
		dev_data->ept_count++;
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
	struct ept_data *ept;
	int r = 0;
	bool hold_buf = false;

	k_mutex_lock(&dev_data->mutex, K_FOREVER);

	/* If endpoint was not registered locally, add it without configuration and
	 * hold RX buffer. It will be used to find right endpoint during local
	 * registration. */
	ept = find_ept_registered_locally(dev_data, reg_ept_buffer);
	if (ept == NULL) {
		r = add_endpoint(dev_data, NULL, reg_ept_buffer);
		if (r < 0) {
			goto exit;
		}
		ept = &dev_data->ept[r];
		hold_buf = true;
	}

	/* Save information needed to map local and remote endpoint id. */
	dev_data->remote_to_local_id[remote_ept_id] = ept->local_id;

exit:
	k_mutex_unlock(&dev_data->mutex);
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
	struct ept_data *ept;
	int bit_val = 0;

	/* Map remote id to local id and validate */
	local_ept_id = dev_data->remote_to_local_id[remote_ept_id];
	if (local_ept_id >= CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP) {
		return -EINVAL;
	}

	/* Clear bit. If cleared, specific block will not be hold after the callback. */
	sys_bitarray_clear_bit(conf->rx_hold_bitmap, block_index);

	/* Call the endpoint callback. It can set the hold bit. */
	ept = &dev_data->ept[local_ept_id];
	ept->cfg->cb.received(block->data, block->size, ept->cfg->priv);

	/* If the bit is still cleared, request deallocation of the buffer. */
	sys_bitarray_test_bit(conf->rx_hold_bitmap, block_index, &bit_val);
	if (!bit_val) {
		free_rx_buffer(dev_data, block->data);
	}

	return 0;
}

/**
 * Callback called by ICMsg that handles message (data or ept registration) received
 * from the remote.
 *
 * @param[in] data	Message received from the ICMsg
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
		if (remote_ept_id >= CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP) {
			r = -EINVAL;
			goto exit;
		}

		/* Make sure that cache is up to date. */
		sys_cache_data_invd_range(block, block->size + offsetof(struct block_header, data)); // TODO: consider checking size before using it
		__sync_synchronize();

		/* Handle the message */
		if (message[1] & ID_REGISTER_EP_FLAG) {
			r = register_remote_ept(dev_data, remote_ept_id, block->data);
		} else {
			r = received_ept_data(dev_data, remote_ept_id, block_index,
					      block);
		}

	} else {
		r = -EINVAL;
	}

exit:
	// TODO: handle errors on
}

/**
 * Callback called when ICMsg is bound.
 */
static void bound(void *priv)
{
	const struct device *instance = priv;
	struct backend_data *dev_data = instance->data;

	/* Set flag that ICMsg is bounded and now, endpoint bounding may start. */
	dev_data->icmsg_bounded = true;
	schedule_ept_bound_process(dev_data);
}

/**
 * Work handler that is responsible for bounding the endpoints. It sends endpoint
 * registration requests to the remote and calls local endpoint bound callback.
 */
static void ep_bound_process(struct k_work *item)
{
	struct backend_data *dev_data = CONTAINER_OF(item, struct backend_data, ep_bound_work);
	struct ept_data *ept = NULL;
	int i;
	int r;
	size_t name_len;
	struct block_header *block;

	/* Skip processing if ICMsg was not bounded yet. */
	if (!dev_data->icmsg_bounded) {
		return;
	}

	/* Find first unprocessed endpoint that was already registered locally. */
	for (i = 0; i < dev_data->ept_count; i++) {
		if (!dev_data->ept[i].bounded && dev_data->ept[i].cfg != NULL) {
			ept = &dev_data->ept[i];
			break;
		}
	}

	/* End processing if not found. */
	if (ept == NULL) {
		return;
	}

	ept->bounded = true;

	/* Register EP on remote */
	name_len = strlen(ept->cfg->name) + 1;
	r = alloc_tx_buffer(dev_data, name_len, &block, K_FOREVER);
	if (r >= 0) {
		memcpy(block->data, ept->cfg->name, name_len);
		sys_cache_data_invd_range(block, name_len + offsetof(struct block_header, data));
		__sync_synchronize();
		r = icmsg_send_wrapper(dev_data, r, ept->local_id | ID_REGISTER_EP_FLAG);
		/* ICMsg queue should have enough space for all blocks. */
		__ASSERT_NO_MSG(r == 0);
	} else {
		/* EP name cannot be bigger then entire allocable space. */
		__ASSERT_NO_MSG(false);
	}

	/* After the registration was send, EP is ready to send more data */
	if (ept->cfg->cb.bound != NULL) {
		ept->cfg->cb.bound(ept->cfg->priv);
	}

	/* Do next processing later */
	schedule_ept_bound_process(dev_data);
}

/**
 * Backend device initialization.
 */
static int backend_init(const struct device *instance)
{
	const struct icmsg_with_buf_config *conf = instance->config;
	struct backend_data *dev_data = instance->data;

	dev_data->conf = conf;
	k_mutex_init(&dev_data->mutex);
	k_work_init(&dev_data->ep_bound_work, ep_bound_process);
	k_sem_init(&dev_data->block_wait_sem, 0, 1);
	memset(&dev_data->remote_to_local_id, ID_INVALID,
	       CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP);
	return 0;
}

/**
 * Open the backend instance callback.
 */
static int open(const struct device *instance)
{
	const struct icmsg_with_buf_config *conf = instance->config;
	struct backend_data *dev_data = instance->data;

	static const struct ipc_service_cb cb = {
		.bound = bound,
		.received = received,
		.error = NULL,
	};

	return icmsg_open(&conf->icmsg_config, &dev_data->icmsg_data, &cb, (void *)instance);
}

/**
 * Backend endpoint registration callback.
 */
static int register_ept(const struct device *instance, void **token,
			const struct ipc_ept_cfg *cfg)
{
	struct backend_data *dev_data = instance->data;
	struct ept_data *ept;
	int r = 0;

	k_mutex_lock(&dev_data->mutex, K_FOREVER);

	/* First, find if endpoint with specific endpoint was registered by the remote. */
	ept = find_ept_registered_remotely(dev_data, cfg->name);

	if (ept == NULL) {
		/* This is a new endpoint, so just add it. */
		r = add_endpoint(dev_data, cfg, NULL);
		k_mutex_unlock(&dev_data->mutex);
		if (r < 0) {
			return r;
		}
		ept = &dev_data->ept[r];
	} else {
		/* The endpoint was already registered by the remote, so just add
		 * configuration to it. */
		ept->cfg = cfg;
		k_mutex_unlock(&dev_data->mutex);
		free_rx_buffer(dev_data, ept->reg_ept_buffer);
	}

	*token = (void *)ept;

	/* Actual bounding will be done in the work running from system workqueue. */
	schedule_ept_bound_process(dev_data);

	return r;
}

/**
 * Endpoint send callback function (with copy).
 */
static int send(const struct device *instance, void *token,
		const void *msg, size_t len)
{
	struct backend_data *dev_data = instance->data;
	struct ept_data *ept = token;
	struct block_header* block;
	int r; // TODO: reconsider naming return values that holds actual data

	/* Allocate the buffer. */
	r = alloc_tx_buffer(dev_data, len, &block, K_FOREVER);
	if (r < 0) {
		return r;
	}

	/* Copy data to allocated buffer. */
	block->size = len;
	memcpy(block->data, msg, len);

	/* Make sure that data is not cached. */
	__sync_synchronize();
	sys_cache_data_flush_range(block, len + offsetof(struct block_header, data));

	/* Send data message. */
	r = icmsg_send_wrapper(dev_data, r, ept->local_id);

	/* Remove buffer if something gone wrong - buffer was not send */
	if (r < 0) {
		free_tx_buffer(dev_data, block->data, -1);
	}

	return r;
}

/**
 * Endpoint TX buffer allocation callback for nocopy sending.
 */
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

/**
 * Endpoint TX buffer deallocation callback for nocopy sending.
 */
static int drop_tx_buffer(const struct device *instance, void *token,
			  const void *data)
{
	struct backend_data *dev_data = instance->data;

	return free_tx_buffer(dev_data, data, -1);
}

/**
 * Endpoint nocopy sending.
 */
static int send_nocopy(const struct device *instance, void *token,
			const void *data, size_t len)
{
	struct backend_data *dev_data = instance->data;
	struct ept_data *ept = token;
	int r;

	/* Actual size may be smaller than requested, so shrink if possible. */
	r = free_tx_buffer(dev_data, data, len);
	if (r < 0) {
		free_tx_buffer(dev_data, data, -1);
		return r;
	}

	/* Make sure that data is not cached. */
	__sync_synchronize();
	sys_cache_data_flush_range((uint8_t *)data - offsetof(struct block_header, data),
				   len + offsetof(struct block_header, data));

	/* Send data message. */
	r = icmsg_send_wrapper(dev_data, r, ept->local_id);

	/* Remove buffer if something gone wrong - buffer was not send */
	if (r < 0) {
		free_tx_buffer(dev_data, data, -1);
	}

	return r;
}

/**
 * Holding RX buffer for nocopy receiving.
 */
static int hold_rx_buffer(const struct device *instance, void *token, void *data)
{
	const struct icmsg_with_buf_config *conf = instance->config;
	size_t block_index;
	uint8_t *buffer = data;

	/* Calculate block index and set associated bit. Called function will check if
	 * block index is valid. */
	block_index = (buffer - conf->tx_blocks_ptr) / conf->tx_block_size;
	return sys_bitarray_set_bit(conf->rx_hold_bitmap, block_index);
}

/**
 * Release RX buffer that was previously held.
 */
static int release_rx_buffer(const struct device *instance, void *token, void *data)
{
	struct backend_data *dev_data = instance->data;

	return free_rx_buffer(dev_data, (uint8_t *)data);
}

/**
 * Returns maximum TX buffer size.
 */
static int get_tx_buffer_size(const struct device *instance, void *token)
{
	const struct icmsg_with_buf_config *conf = instance->config;

	return (conf->tx_block_size * conf->tx_block_count -
		offsetof(struct block_header, data));
}

/**
 * IPC service backend callbacks.
 */
const static struct ipc_service_backend backend_ops = {
	.open_instance = open,
	.close_instance = NULL, /* not implemented */
	.send = send,
	.register_endpoint = register_ept,
	.deregister_endpoint = NULL, /* not implemented */
	.get_tx_buffer_size = get_tx_buffer_size,
	.get_tx_buffer = get_tx_buffer,
	.drop_tx_buffer = drop_tx_buffer,
	.send_nocopy = send_nocopy,
	.hold_rx_buffer = hold_rx_buffer,
	.release_rx_buffer = release_rx_buffer,
};

/**
 * Calculates minimum size required for ICmsg region for specific number of local
 * and remote blocks. The minimum size ensures that ICmsg queue is will never overflow
 * because it can hold data message for each local block and deallocation message
 * for each remote block.
 */
#define GET_ICMSG_MIN_SIZE(local_blocks, remote_blocks) \
	(ICMSG_BUFFER_OVERHEAD + \
	BYTES_PER_ICMSG_DATA_MESSAGE * (local_blocks) + \
	BYTES_PER_ICMSG_FREE_MESSAGE * (remote_blocks))

/**
 * Calculate aligned block size by evenly dividing remaining space after removing
 * the space for ICmsg.
 */
#define GET_BLOCK_SIZE(total_size, local_blocks, remote_blocks) ROUND_DOWN(		\
	((total_size) - GET_ICMSG_MIN_SIZE((local_blocks), (remote_blocks))) /		\
	(local_blocks), BLOCK_ALIGNMENT)

/**
 * Calculate offset where area for blocks starts which is just after the ICmsg.
 */
#define GET_BLOCKS_OFFSET(total_size, local_blocks, remote_blocks)			\
	((total_size) - GET_BLOCK_SIZE((total_size), (local_blocks), (remote_blocks)) *	\
	 (local_blocks))

/**
 * Returns GET_ICMSG_SIZE, but for specific instance and direction.
 * 'loc' and 'rem' parameters tells the direction. They can be either "tx, rx"
 *  or "rx, tx".
 */
#define GET_ICMSG_SIZE_INST(i, loc, rem)					\
	GET_BLOCKS_OFFSET(							\
		DT_REG_SIZE(DT_INST_PHANDLE(i, loc##_region)),			\
		DT_INST_PROP(i, loc##_blocks),					\
		DT_INST_PROP(i, rem##_blocks))

/**
 * Returns address where area for blocks starts for specific instance and direction.
 * 'loc' and 'rem' parameters tells the direction. They can be either "tx, rx"
 *  or "rx, tx".
 */
#define GET_BLOCKS_ADDR_INST(i, loc, rem)					\
	DT_REG_ADDR(DT_INST_PHANDLE(i, loc##_region)) +				\
	GET_BLOCKS_OFFSET(							\
		DT_REG_SIZE(DT_INST_PHANDLE(i, loc##_region)),			\
		DT_INST_PROP(i, loc##_blocks),					\
		DT_INST_PROP(i, rem##_blocks))

/**
 * Returns block size for specific instance and direction.
 * 'loc' and 'rem' parameters tells the direction. They can be either "tx, rx"
 *  or "rx, tx".
 */
#define GET_BLOCK_SIZE_INST(i, loc, rem)					\
	GET_BLOCK_SIZE(								\
		DT_REG_SIZE(DT_INST_PHANDLE(i, loc##_region)),			\
		DT_INST_PROP(i, loc##_blocks),					\
		DT_INST_PROP(i, rem##_blocks))

#define DEFINE_BACKEND_DEVICE(i)							\
	SYS_BITARRAY_DEFINE_STATIC(tx_usage_bitmap_##i, DT_INST_PROP(i, tx_blocks));	\
	SYS_BITARRAY_DEFINE_STATIC(rx_hold_bitmap_##i, DT_INST_PROP(i, rx_blocks));	\
	static struct backend_data backend_data_##i;					\
	static const struct icmsg_with_buf_config backend_config_##i =			\
	{										\
		.icmsg_config = {							\
			.tx_shm_size = GET_ICMSG_SIZE_INST(i, tx, rx),			\
			.tx_shm_addr = DT_REG_ADDR(DT_INST_PHANDLE(i, tx_region)),	\
			.rx_shm_size = GET_ICMSG_SIZE_INST(i, rx, tx),			\
			.rx_shm_addr = DT_REG_ADDR(DT_INST_PHANDLE(i, rx_region)),	\
			.mbox_tx = MBOX_DT_CHANNEL_GET(DT_DRV_INST(i), tx),		\
			.mbox_rx = MBOX_DT_CHANNEL_GET(DT_DRV_INST(i), rx),		\
		},									\
		.tx_blocks_ptr = (uint8_t *)GET_BLOCKS_ADDR_INST(i, tx, rx),		\
		.rx_blocks_ptr = (uint8_t *)GET_BLOCKS_ADDR_INST(i, rx, tx),		\
		.tx_block_count = DT_INST_PROP(i, tx_blocks),				\
		.rx_block_count = DT_INST_PROP(i, rx_blocks),				\
		.tx_block_size = GET_BLOCK_SIZE_INST(i, tx, rx),			\
		.rx_block_size = GET_BLOCK_SIZE_INST(i, rx, tx),			\
		.tx_usage_bitmap = &tx_usage_bitmap_##i,				\
		.rx_hold_bitmap = &rx_hold_bitmap_##i,					\
	};										\
	BUILD_ASSERT((GET_BLOCK_SIZE_INST(i, tx, rx) > BLOCK_ALIGNMENT) &&		\
		     (GET_BLOCK_SIZE_INST(i, tx, rx) <					\
		      DT_REG_SIZE(DT_INST_PHANDLE(i, tx_region))),			\
		     "TX region is too small for provided number of blocks");		\
	BUILD_ASSERT((GET_BLOCK_SIZE_INST(i, rx, tx) > BLOCK_ALIGNMENT) &&		\
		     (GET_BLOCK_SIZE_INST(i, rx, tx) <					\
		      DT_REG_SIZE(DT_INST_PHANDLE(i, rx_region))),			\
		     "RX region is too small for provided number of blocks");		\
	DEVICE_DT_INST_DEFINE(i,							\
			      &backend_init,						\
			      NULL,							\
			      &backend_data_##i,					\
			      &backend_config_##i,					\
			      POST_KERNEL,						\
			      CONFIG_IPC_SERVICE_REG_BACKEND_PRIORITY,			\
			      &backend_ops);

DT_INST_FOREACH_STATUS_OKAY(DEFINE_BACKEND_DEVICE)

#if defined(CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_SHMEM_RESET)

#define BACKEND_ICMSG_CONFIG_PTR(i) &backend_config_##i.icmsg_config,

static int shared_memory_prepare(void)
{
	int i;
	const struct icmsg_config_t *backend_configs[] = {
		DT_INST_FOREACH_STATUS_OKAY(BACKEND_ICMSG_CONFIG_PTR)
	};

	for (i = 0; i < ARRAY_SIZE(backend_configs); i++) {
		icmsg_clear_tx_memory(backend_configs[i]);
		icmsg_clear_rx_memory(backend_configs[i]);
	}

	return 0;
}

SYS_INIT(shared_memory_prepare, PRE_KERNEL_1, 1);

#endif /* CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_SHMEM_RESET */
