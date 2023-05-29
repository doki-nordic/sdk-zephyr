
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/sys/bitarray.h>
#include <zephyr/ipc/icmsg.h>
#include <zephyr/ipc/ipc_service_backend.h>
#include <zephyr/cache.h>

#define DT_DRV_COMPAT zephyr_ipc_icmsg_with_buf

/** Special endpoint id indicating invalid (or empty) entry. */
#define ID_INVALID 0xFF

/** Message id for deallocating data buffers. */
#define ID_FREE_DATA 0xFE

/** Message id for deallocating endpoint registration buffers. */
#define ID_FREE_BOUND_EPT 0xFD

/** Message id for endpoint bounding message. */
#define ID_BOUND_EPT 0xFC

/** Special value for empty entry in bound message waiting table. */
#define WAITING_BOUND_MSG_EMPTY 0xFFFF

#define BLOCK_ALIGNMENT sizeof(size_t)
#define BYTES_PER_ICMSG_MESSAGE 8
#define ICMSG_BUFFER_OVERHEAD (2 * (sizeof(struct spsc_pbuf) + BYTES_PER_ICMSG_MESSAGE))

/* TODO: update this comment
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
 *       uint8_t index of sender block containing endpoint id and null-terminated endpoint name
 *       uint8_t ID_BOUND_EPT (0xFE)
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

enum ept_bounding_state {
	EPT_UNCONFIGURED = 0,	/* Endpoint in not configured (initial state). */
	EPT_CONFIGURED,		/* Endpoint is configured, waiting for work queue to send
				 * bound message. */
	EPT_BOUNDING,		/* Bound message was send, waiting for bound deallocation
				 * message which works as bounding ACK. */
	EPT_BOUNDED,		/* Bound message deallocation was received, waiting for
				 * incoming bound message or (if already received)
				 * for work queue to call the user bound callback. */
	EPT_READY,		/* Endpoint is bounded, ready to exchange messages. */
};

struct channel_config {
	uint8_t *blocks_ptr;
	size_t block_size;
	size_t block_count;
};

struct icmsg_with_buf_config {
	struct icmsg_config_t icmsg_config;
	struct channel_config rx;
	struct channel_config tx;
	sys_bitarray_t *tx_usage_bitmap;
	sys_bitarray_t *rx_hold_bitmap;
};

struct ept_data {
	const struct ipc_ept_cfg *cfg;
	uint8_t local_id;
	uint8_t remote_id;
	enum ept_bounding_state state;
};

struct backend_data {
	const struct icmsg_with_buf_config* conf;
	struct icmsg_data_t icmsg_data;
	struct k_mutex mutex;
	struct k_work ep_bound_work;
	struct k_sem block_wait_sem;
	struct ept_data ept[CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP];
	uint16_t waiting_bound_msg[CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP];
	uint8_t ept_count;
	bool icmsg_bounded;
};

struct block_header {
	size_t size;
	uint8_t data[];
};

struct ept_bound_msg {
	uint8_t ept_id;
	char name[];
};

static struct block_header *block_from_index(const struct channel_config *ch_conf, size_t block_index)
{
	return (struct block_header *)(ch_conf->blocks_ptr + block_index * ch_conf->block_size);
}

static uint8_t *buffer_from_index_validate(const struct channel_config *ch_conf, size_t block_index, size_t *size)
{
	size_t allocable_size;
	size_t block_size;
	uint8_t *end_ptr;
	struct block_header *block = block_from_index(ch_conf, block_index);

	if (block_index >= ch_conf->block_count) {
		return NULL;
	}

	if (size != NULL) {
		allocable_size = ch_conf->block_count * ch_conf->block_size;
		end_ptr = ch_conf->blocks_ptr + allocable_size;
		block_size = block->size;
		if (block_size > allocable_size - offsetof(struct block_header, data) || &block->data[block_size] > end_ptr) {
			return NULL;
		}
		*size = block_size;
	}

	return block->data;
}

static int buffer_to_index_validate(const struct channel_config *ch_conf, const uint8_t *buffer, size_t *size)
{
	size_t block_index;
	uint8_t *expected;

	block_index = (buffer - ch_conf->blocks_ptr) / ch_conf->block_size;

	expected = buffer_from_index_validate(ch_conf, block_index, size);

	if (expected == NULL || expected != buffer) {
		return -EINVAL;
	}

	return block_index;
}

/**
 * Send data with ICmsg with mutex locked. Mutex must be locked because ICmsg may return
 * error on concurrent invocations even when there is enough space in queue.
 */
static int icmsg_send_wrapper(struct backend_data *dev_data, uint8_t b0, uint8_t b1)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	uint8_t message[2] = { b0, b1 };
	int r;

	k_mutex_lock(&dev_data->mutex, K_FOREVER);
	r = icmsg_send(&conf->icmsg_config, &dev_data->icmsg_data, message,
		       sizeof(message));
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
	size_t num_blocks = (total_size + conf->tx.block_size - 1) / conf->tx.block_size;
	bool sem_taken = false;
	size_t tx_block_index;
	size_t next_bit;
	int prev_bit_val;
	int r;

	do {
		/* Try to allocate specified number of blocks */
		r = sys_bitarray_alloc(conf->tx_usage_bitmap, num_blocks, &tx_block_index);
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
		for (next_bit = tx_block_index + 1; next_bit < conf->tx.block_count;
		     next_bit++) {
			r = sys_bitarray_test_and_set_bit(conf->tx_usage_bitmap, next_bit,
							  &prev_bit_val);
			__ASSERT_NO_MSG(r == 0);
			if (prev_bit_val) {
				break;
			}
		}
		num_blocks = next_bit - tx_block_index;
	}

	/* Calculate block pointer and adjust size to actually allocated space (which is
	 * not less that requested). */
	*block = block_from_index(&conf->tx, tx_block_index);
	(*block)->size = conf->tx.block_size * num_blocks - offsetof(struct block_header,
								     data);

	return tx_block_index;
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

	r = buffer_to_index_validate(&conf->tx, buffer, &size);
	if (r < 0) {
		return r;
	}

	block_index = r;

	/* Calculate number of blocks. */
	total_size = size + offsetof(struct block_header, data);
	num_blocks = (total_size + conf->tx.block_size - 1) / conf->tx.block_size;

	if (new_size >= 0) {
		/* Calculate and validate new values */
		new_total_size = new_size + offsetof(struct block_header, data);
		new_num_blocks = (new_total_size + conf->tx.block_size - 1) /
				 conf->tx.block_size;
		if (new_num_blocks > num_blocks) {
			return -EINVAL;
		}
		/* Update actual buffer size and blocks to deallocate */
		block = block_from_index(&conf->tx, block_index);
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
 * Find endpoint that was registered with name that matches name
 * contained in the endpoint registration buffer received from remote. This function
 * must be called when mutex is locked.
 * 
 * @param[in] dev_data	Device data.
 * @param[in] name	Endpoint name, it must be in a received block.
 * 
 * @return	Found endpoint data or NULL if not found
 */
static struct ept_data *find_ept_by_name(struct backend_data *dev_data, const char *name)
{
	const struct channel_config *rx_conf = &dev_data->conf->rx;
	int i;
	const char *buffer_end = (const char *)rx_conf->blocks_ptr +
				 rx_conf->block_count * rx_conf->block_size;
	size_t name_size;

	/* Requested name is in shared memory, so we have to assume that it
	 * can be corrupted. Extra care must be taken to avoid out of
	 * bounds reads. */
	name_size = strnlen(name, buffer_end - name - 1) + 1;

	for (i = 0; i < dev_data->ept_count; i++) {
		if (strncmp(dev_data->ept[i].cfg->name, name, name_size) == 0) {
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
static int free_rx_buffer(struct backend_data *dev_data, const uint8_t *buffer, uint8_t id)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	int block_index;

	block_index = buffer_to_index_validate(&conf->rx, buffer, NULL);
	if (block_index < 0) {
		return block_index;
	}

	return icmsg_send_wrapper(dev_data, block_index, id);
}

static int received_free_reg_ept(struct backend_data *dev_data, size_t tx_block_index)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	uint8_t *buffer;
	struct ept_bound_msg *msg;
	size_t local_ept_id;
	int r = 0;

	buffer = buffer_from_index_validate(&conf->tx, tx_block_index, NULL);
	if (buffer == NULL) {
		return -EINVAL;
	}

	msg = (struct ept_bound_msg *)buffer;
	local_ept_id = msg->ept_id;

	r = free_tx_buffer(dev_data, buffer, -1);

	if (local_ept_id >= CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP) {
		return -EINVAL;
	}

	k_mutex_lock(&dev_data->mutex, K_FOREVER);
	dev_data->ept[local_ept_id].state = EPT_BOUNDED;
	k_mutex_unlock(&dev_data->mutex);

	schedule_ept_bound_process(dev_data);

	return r;
}

static int received_free_data(struct backend_data *dev_data, size_t tx_block_index)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	uint8_t *buffer;

	buffer = buffer_from_index_validate(&conf->tx, tx_block_index, NULL);
	if (buffer == NULL) {
		return -EINVAL;
	}

	return free_tx_buffer(dev_data, buffer, -1);
}

static int received_register_ept(struct backend_data *dev_data, size_t rx_block_index)
{
	bool is_bounded;
	int i;
	int r = -ENOMEM;

	k_mutex_lock(&dev_data->mutex, K_FOREVER);

	is_bounded = dev_data->icmsg_bounded;

	/* Find empty entry in waiting list and put this block to it. */
	for (i = 0; i < CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP; i++) {
		if (dev_data->waiting_bound_msg[i] == WAITING_BOUND_MSG_EMPTY) {
			dev_data->waiting_bound_msg[i] = rx_block_index;
			r = 0;
			break;
		}
	}

	k_mutex_unlock(&dev_data->mutex);

	/* If ICmsg is already bounded, schedule processing the message */
	if (is_bounded) {
		schedule_ept_bound_process(dev_data);
	}

	return r;
}

static int received_data(struct backend_data *dev_data, size_t rx_block_index, int local_ept_id)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	uint8_t *buffer;
	struct ept_data *ept;
	size_t size;
	int bit_val;
	int r = 0;

	/* Validate */
	buffer = buffer_from_index_validate(&conf->rx, rx_block_index, &size);
	if (buffer == NULL ||
	    local_ept_id >= CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP) {
		return -EINVAL;
	}

	/* Clear bit. If cleared, specific block will not be hold after the callback. */
	sys_bitarray_clear_bit(conf->rx_hold_bitmap, rx_block_index);

	/* Call the endpoint callback. It can set the hold bit. */
	ept = &dev_data->ept[local_ept_id];
	ept->cfg->cb.received(buffer, size, ept->cfg->priv);

	/* If the bit is still cleared, request deallocation of the buffer. */
	sys_bitarray_test_bit(conf->rx_hold_bitmap, rx_block_index, &bit_val);
	if (!bit_val) {
		free_rx_buffer(dev_data, buffer, ID_FREE_DATA);
	}

	return r;
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
	const uint8_t *message = (const uint8_t *)data;
	size_t ept_id;
	size_t block_index = 0;
	int r;

	if (len != 2) {
		r = -EINVAL;
		goto exit;
	}

	block_index = message[0];
	ept_id = message[1];

	switch (ept_id) {
	case ID_FREE_BOUND_EPT:
		r = received_free_reg_ept(dev_data, block_index);
		break;
	case ID_FREE_DATA:
		r = received_free_data(dev_data, block_index);
		break;
	case ID_BOUND_EPT:
		r = received_register_ept(dev_data, block_index);
		break;
	default:
		r = received_data(dev_data, block_index, ept_id);
		break;
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
	k_mutex_lock(&dev_data->mutex, K_FOREVER);
	dev_data->icmsg_bounded = true;
	k_mutex_unlock(&dev_data->mutex);
	schedule_ept_bound_process(dev_data);
}

static int send_bound_message(struct backend_data *dev_data, struct ept_data *ept)
{
	size_t msg_len;
	struct block_header *block;
	struct ept_bound_msg *msg;
	int r;

	msg_len = offsetof(struct ept_bound_msg, name) + strlen(ept->cfg->name) + 1;
	r = alloc_tx_buffer(dev_data, msg_len, &block, K_FOREVER);
	if (r >= 0) {
		msg = (struct ept_bound_msg *)block->data;
		msg->ept_id = ept->local_id;
		strcpy(msg->name, ept->cfg->name);
		__sync_synchronize();
		sys_cache_data_flush_range(block, msg_len + offsetof(struct block_header, data));
		r = icmsg_send_wrapper(dev_data, r, ID_BOUND_EPT);
		/* ICMsg queue should have enough space for all blocks. */
		__ASSERT_NO_MSG(r == 0);
	} else {
		/* EP name cannot be bigger then entire allocable space. */
		__ASSERT_NO_MSG(false);
	}

	return r;
}

static int match_bound_msg(struct backend_data *dev_data, size_t rx_block_index) // TODO: reconsider adding rx/tx prefix in other paces
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	uint8_t *buffer;
	uint8_t remote_ept_id;
	struct ept_bound_msg *msg;
	struct ept_data *ept;
	int r = 0;

	buffer = buffer_from_index_validate(&conf->rx, rx_block_index, NULL);
	if (buffer == NULL) {
		return -EINVAL;
	}

	msg = (struct ept_bound_msg *)buffer;
	remote_ept_id = msg->ept_id;

	if (remote_ept_id >= CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP) {
		return -EINVAL;
	}

	ept = find_ept_by_name(dev_data, msg->name);

	if (ept == NULL) {
		return 0;
	}

	ept->remote_id = remote_ept_id;

	k_mutex_unlock(&dev_data->mutex);
	r = free_rx_buffer(dev_data, buffer, ID_FREE_BOUND_EPT);
	k_mutex_lock(&dev_data->mutex, K_FOREVER);

	if (r < 0) {
		return r;
	}

	return 1;
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
	int r = 0;

	k_mutex_lock(&dev_data->mutex, K_FOREVER);

	/* Skip processing if ICMsg was not bounded yet. */
	if (!dev_data->icmsg_bounded) {
		schedule_ept_bound_process(dev_data);
		goto exit;
	}

	/* Walk over all waiting incoming bound messages and match to local endpoints. */
	for (i = 0; i < CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP; i++) {
		if (dev_data->waiting_bound_msg[i] != WAITING_BOUND_MSG_EMPTY) {
			r = match_bound_msg(dev_data, dev_data->waiting_bound_msg[i]);
			if (r != 1) {
				dev_data->waiting_bound_msg[i] = WAITING_BOUND_MSG_EMPTY;
				if (r < 0) {
					goto exit;
				}
			}
		}
	}

	for (i = 0; i < dev_data->ept_count; i++) {
		ept = &dev_data->ept[i];
		if (ept->state == EPT_CONFIGURED) {
			ept->state = EPT_BOUNDING;
			k_mutex_unlock(&dev_data->mutex);
			r = send_bound_message(dev_data, ept);
			k_mutex_lock(&dev_data->mutex, K_FOREVER);
			if (r < 0) {
				ept->state = EPT_UNCONFIGURED;
				goto exit;
			}
		} else if (ept->state == EPT_BOUNDED && ept->remote_id != ID_INVALID) {
			/* Call bound callback if endpoint is bound. */
			ept->state = EPT_READY;
			k_mutex_unlock(&dev_data->mutex);
			if (ept->cfg->cb.bound != NULL) {
				ept->cfg->cb.bound(ept->cfg->priv);
			}
			k_mutex_lock(&dev_data->mutex, K_FOREVER);
		}
	}

exit:
	k_mutex_unlock(&dev_data->mutex);
	if (r < 0) {
		schedule_ept_bound_process(dev_data);
		// TODO: process error code
	}
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
	memset(&dev_data->waiting_bound_msg, 0xFF, sizeof(dev_data->waiting_bound_msg));
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
static int add_endpoint(struct backend_data *dev_data, const struct ipc_ept_cfg *cfg) // TODO: it is used once, reconsider merging this function into caller function
{
	struct ept_data *ept;
	int r = 0;

	k_mutex_lock(&dev_data->mutex, K_FOREVER);

	if (dev_data->ept_count < CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP) {
		ept = &dev_data->ept[dev_data->ept_count];
		r = dev_data->ept_count;
		dev_data->ept_count++;
		ept->cfg = cfg;
		ept->local_id = r;
		ept->remote_id = ID_INVALID;
		ept->state = EPT_CONFIGURED;
	} else {
		r = -ENOMEM;
	}

	k_mutex_unlock(&dev_data->mutex);

	return r;
}

/**
 * Backend endpoint registration callback.
 */
static int register_ept(const struct device *instance, void **token,
			const struct ipc_ept_cfg *cfg)
{
	struct backend_data *dev_data = instance->data;
	int r = 0;

	/* Add new endpoint. */
	r = add_endpoint(dev_data, cfg);
	if (r < 0) {
		return r;
	}

	/* Keep endpoint address in token. */
	*token = (void *)&dev_data->ept[r];

	/* Rest of the bounding will be done in the system workqueue. */
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
	r = icmsg_send_wrapper(dev_data, r, ept->remote_id);

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
	r = icmsg_send_wrapper(dev_data, r, ept->remote_id);

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
	int block_index;
	uint8_t *buffer = data;

	/* Calculate block index and set associated bit. */
	block_index = buffer_to_index_validate(&conf->tx, buffer, NULL);
	if (block_index < 0) {
		return block_index;
	}
	return sys_bitarray_set_bit(conf->rx_hold_bitmap, block_index);
}

/**
 * Release RX buffer that was previously held.
 */
static int release_rx_buffer(const struct device *instance, void *token, void *data)
{
	struct backend_data *dev_data = instance->data;

	return free_rx_buffer(dev_data, (uint8_t *)data, ID_FREE_DATA);
}

/**
 * Returns maximum TX buffer size.
 */
static int get_tx_buffer_size(const struct device *instance, void *token)
{
	const struct icmsg_with_buf_config *conf = instance->config;

	return (conf->tx.block_size * conf->tx.block_count -
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
	(ICMSG_BUFFER_OVERHEAD + BYTES_PER_ICMSG_MESSAGE * (local_blocks + remote_blocks))

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
		.tx = {									\
			.blocks_ptr = (uint8_t *)GET_BLOCKS_ADDR_INST(i, tx, rx),	\
			.block_count = DT_INST_PROP(i, tx_blocks),			\
			.block_size = GET_BLOCK_SIZE_INST(i, tx, rx),			\
		},									\
		.rx = {									\
			.blocks_ptr = (uint8_t *)GET_BLOCKS_ADDR_INST(i, rx, tx),	\
			.block_count = DT_INST_PROP(i, rx_blocks),			\
			.block_size = GET_BLOCK_SIZE_INST(i, rx, tx),			\
		},									\
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

// TODO: Assert block count <= 256

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
