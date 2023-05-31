/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * ICmsg with buffers backend.
 *
 * This is an IPC service backend that dynamically allocates buffers for data transmission
 * and uses ICmsg as a signaling transport.
 *
 * Shared memory organization
 * --------------------------
 *
 * Single channel (RX or TX) of the shared memory is divided into two areas: ICmsg area
 * followed by Blocks area. ICmsg is used to send and receive short 2-byte messages.
 * Blocks area is evenly divided into aligned blocks. Blocks are used to allocate
 * buffers containing actual data. Data buffers can span multiple blocks. The first block
 * starts with the size of the following data.
 *
 *  +------------+-------------+
 *  | ICmsg area | Blocks area |
 *  +------------+-------------+
 *       _______/               \_________________________________________
 *      /                                                                 \
 *      +-----------+-----------+-----------+-----------+-   -+-----------+
 *      |  Block 0  |  Block 1  |  Block 2  |  Block 3  | ... | Block N-1 |
 *      +-----------+-----------+-----------+-----------+-   -+-----------+
 *            _____/                                     \_____
 *           /                                                 \
 *           +------+--------------------------------+---------+
 *           | size | data_buffer[size] ...          | padding |
 *           +------+--------------------------------+---------+
 *
 * The sender holds information about reserved blocks using bitarray and it is always
 * responsible for allocating and releasing the blocks. The receiver just tells the sender
 * that it does not need a specific buffer anymore.
 *
 * ICmsg messages
 * --------------
 *
 * ICmsg is used to send and receive small 2-byte messages. The first byte is an endpoint
 * address or a message type and the second is the block index where the relevant buffer
 * starts.
 *
 *  - Send data
 *    | receiver endpoint address | block index |
 *    This message is used to send data buffer to specific endpoint. The same endpoint
 *    may have different addresses on different sides, so the message contains "receiver"
 *    address.
 *
 *  - Release data
 *    | MSG_RELEASE_DATA | block index |
 *    This message is a response to the "Send data" message and it is used to inform that
 *    specific buffer is not used anymore and can be released.
 *
 *  - Bound endpoint
 *    | MSG_BOUND | block index |
 *    This message starts the bounding of the endpoint. The buffer contains the sender
 *    endpoint address in the first byte followed by the null-terminated endpoint name.
 *
 *  - Release bound endpoint
 *    | MSG_RELEASE_BOUND | block index |
 *    This message is a response to the "Bound endpoint" message and it is used to inform
 *    that a specific buffer is not used anymore and a specific endpoint can now receive
 *    data.
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ipc_svc_icmsg_w_buf,
		    CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_LOG_LEVEL);

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/sys/bitarray.h>
#include <zephyr/ipc/icmsg.h>
#include <zephyr/ipc/ipc_service_backend.h>
#include <zephyr/cache.h>

#define DT_DRV_COMPAT zephyr_ipc_icmsg_with_buf

/** Special endpoint address indicating invalid (or empty) entry. */
#define EPT_ADDR_INVALID 0xFF

/** Message type for releasing data buffers. */
#define MSG_RELEASE_DATA 0xFE

/** Message type for endpoint bounding message. */
#define MSG_BOUND 0xFD

/** Message type for releasing endpoint bound message buffer. */
#define MSG_RELEASE_BOUND 0xFC

/** Maximum value of endpoint address. */
#define EPT_ADDR_MAX 0xFB

/** Special value for empty entry in bound message waiting table. */
#define WAITING_BOUND_MSG_EMPTY 0xFFFF

/** Alignment of a block */
#define BLOCK_ALIGNMENT sizeof(size_t)

/** Number of bytes per each ICmsg message. It is used to calculate size of ICmsg area. */
#define BYTES_PER_ICMSG_MESSAGE 8

/** Maximum ICmsg overhead. It is used to calculate size of ICmsg area. */
#define ICMSG_BUFFER_OVERHEAD (2 * (sizeof(struct spsc_pbuf) + BYTES_PER_ICMSG_MESSAGE))

/** Size of the header (size field) of the block. */
#define BLOCK_HEADER_SIZE (offsetof(struct block_header, data))

enum ept_bounding_state {
	EPT_UNCONFIGURED = 0,	/* Endpoint in not configured (initial state). */
	EPT_CONFIGURED,		/* Endpoint is configured, waiting for work queue to send
				 * bound message. */
	EPT_BOUNDING,		/* Bound message was send, waiting for bound release
				 * message which works as bounding ACK. */
	EPT_BOUNDED,		/* Release bound message was received, waiting for
				 * incoming bound message or (if already received)
				 * for work queue to call the user bound callback. */
	EPT_READY,		/* Endpoint is bounded, ready to exchange data. */
};

struct channel_config {
	uint8_t *blocks_ptr;	/**< Address where the blocks start. */
	size_t block_size;	/**< Size of one block. */
	size_t block_count;	/**< Number of blocks. */
};

struct icmsg_with_buf_config {
	struct icmsg_config_t icmsg_config;	/**< Configuration of the ICmsg. */
	struct channel_config rx;		/**< RX channel config. */
	struct channel_config tx;		/**< RX channel config. */
	sys_bitarray_t *tx_usage_bitmap;	/**< Bit is set when TX block is in use */
	sys_bitarray_t *rx_hold_bitmap;		/**< Bit is set is buffer starting at this
						     block should be kept after exit from
						     receive handler. */
};

struct ept_data {
	const struct ipc_ept_cfg *cfg;	/**< Endpoint configuration. */
	uint8_t local_addr;		/**< Local endpoint address. Equals index in
					 *   endpoints array. */
	uint8_t remote_addr;		/**< Remote endpoint address. Obtained during the
					 *   bounding process. */
	enum ept_bounding_state state;	/**< Bounding state. */
};

struct backend_data {
	const struct icmsg_with_buf_config* conf; /**< Backend instance config. */
	struct icmsg_data_t icmsg_data;	/**< ICmsg data. */
	struct k_mutex mutex;		/**< Mutex to protect common data and resources.*/
	struct k_work ep_bound_work;	/**< Work item for bounding processing. */
	struct k_sem block_wait_sem;	/**< Semaphore for waiting for free blocks. */
	struct ept_data ept[CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP];
					/**< Array of registered endpoints. */
	uint16_t waiting_bound_msg[CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP];
					/**< Array of "bound" messages waiting to be
					 *   registered locally. */
	uint8_t ept_count;		/**< Number of registered endpoints. */
	bool icmsg_bounded;		/**< ISmsg was bounded already. */
};

struct block_header {
	size_t size;	/**< Size of the data field. */
	uint8_t data[];	/**< Buffer data. */
};

struct ept_bound_msg {
	uint8_t ept_addr;	/**< Sender address of the endpoint. */
	char name[];		/**< Name of the endpoint. */
};

BUILD_ASSERT(CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP <= EPT_ADDR_MAX + 1,
	     "Too many endpoints");

/**
 * Calculate pointer to block from its index and channel configuration (RX or TX).
 * No validation is performed.
 */
static struct block_header *block_from_index(const struct channel_config *ch_conf,
					     size_t block_index)
{
	return (struct block_header *)(ch_conf->blocks_ptr + block_index *
				       ch_conf->block_size);
}

/**
 * Calculate pointer to data buffer from block index and channel configuration (RX or TX).
 * Also validate the index and optionally the buffer size allocated on the this block.
 *
 * @param[in]  ch_conf		The channel
 * @param[in]  block_index	Block index
 * @param[out] size		Size of the buffer allocated on the block if not NULL.
 *				The size is also checked if it fits in the blocks area.
 *				If it is NULL, no size validation is performed.
 * @param[in]  invalidate_cache	If size is not NULL, invalidates cache for entire buffer
 *				(all blocks). Otherwise, it is ignored.
 * @return	Pointer to data buffer or NULL if validation failed.
 */
static uint8_t *buffer_from_index_validate(const struct channel_config *ch_conf,
					   size_t block_index, size_t *size,
					   bool invalidate_cache)
{

	size_t allocable_size;
	size_t buffer_size;
	uint8_t *end_ptr;
	struct block_header *block;

	if (block_index >= ch_conf->block_count) {
		LOG_ERR("Block index invalid");
		return NULL;
	}

	block = block_from_index(ch_conf, block_index);

	if (size != NULL) {
		if (invalidate_cache) {
			sys_cache_data_invd_range(block, BLOCK_HEADER_SIZE);
			__sync_synchronize();
		}
		allocable_size = ch_conf->block_count * ch_conf->block_size;
		end_ptr = ch_conf->blocks_ptr + allocable_size;
		buffer_size = block->size;
		if (buffer_size > allocable_size - BLOCK_HEADER_SIZE ||
		    &block->data[buffer_size] > end_ptr) {
			LOG_ERR("Block corrupted");
			return NULL;
		}
		*size = buffer_size;
		if (invalidate_cache) {
			sys_cache_data_invd_range(block->data, buffer_size);
			__sync_synchronize();
		}
	}

	return block->data;
}

/**
 * Calculate block index based on data buffer pointer and validate it.
 *
 * @param[in]  ch_conf		The channel
 * @param[in]  buffer		Pointer to data buffer
 * @param[out] size		Size of the allocated buffer if not NULL.
 *				The size is also checked if it fits in the blocks area.
 *				If it is NULL, no size validation is performed.
 * @return		Block index or negative error code
 * @retval -EINVAL	The buffer is not correct
 */
static int buffer_to_index_validate(const struct channel_config *ch_conf,
				    const uint8_t *buffer, size_t *size)
{
	size_t block_index;
	uint8_t *expected;

	block_index = (buffer - ch_conf->blocks_ptr) / ch_conf->block_size;

	expected = buffer_from_index_validate(ch_conf, block_index, size, false);

	if (expected == NULL || expected != buffer) {
		LOG_ERR("Pointer invalid");
		return -EINVAL;
	}

	return block_index;
}

/**
 * Allocate buffer for transmission
 *
 * @param[in,out] size	Required size of the buffer. If zero, first available block is
 *			allocated and all subsequent available blocks. Size actually
 *			allocated which is not less than requested.
 * @param[out] block	First allocated block. Block header contains actually allocated
 * 			bytes, so it have to be adjusted before sending.
 * @param[in] timeout	Timeout
 *
 * @return		Positive index of the first allocated block or negative error
 * @retval -EINVAL	If requested size is bigger than entire allocable space
 * @retval -ENOSPC	If timeout was K_NO_WAIT and there was not enough space
 * @retval -EAGAIN	If timeout occurred
 */
static int alloc_tx_buffer(struct backend_data *dev_data, uint32_t *size,
			   uint8_t **buffer, k_timeout_t timeout)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	size_t total_size = *size + BLOCK_HEADER_SIZE;
	size_t num_blocks = (total_size + conf->tx.block_size - 1) / conf->tx.block_size;
	struct block_header *block;
	bool sem_taken = false;
	size_t tx_block_index;
	size_t next_bit;
	int prev_bit_val;
	int r;

	do {
		/* Try to allocate specified number of blocks */
		r = sys_bitarray_alloc(conf->tx_usage_bitmap, num_blocks,
				       &tx_block_index);
		if (r == -ENOSPC && !K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
			/* Wait for releasing if there is no enough space and exit loop
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
		if (r != -ENOSPC && r != -EAGAIN) {
			LOG_ERR("Failed to allocate buffer, err: %d", r);
			/* Only -EINVAL is allowed in this place. Any other code
			 * indicates something wrong with the logic. */
			__ASSERT_NO_MSG(r == -EINVAL);
		}
		return r;
	}

	/* If size is 0 try to allocate more blocks after already allocated. */
	if (*size == 0) {
		prev_bit_val = 0;
		for (next_bit = tx_block_index + 1; next_bit < conf->tx.block_count;
		     next_bit++) {
			r = sys_bitarray_test_and_set_bit(conf->tx_usage_bitmap, next_bit,
							  &prev_bit_val);
			/** Setting bit should always success. */
			__ASSERT_NO_MSG(r == 0);
			if (prev_bit_val) {
				break;
			}
		}
		num_blocks = next_bit - tx_block_index;
	}

	/* Get block pointer and adjust size to actually allocated space. */
	*size = conf->tx.block_size * num_blocks - BLOCK_HEADER_SIZE;
	block = block_from_index(&conf->tx, tx_block_index);
	block->size = *size;
	*buffer = block->data;
	return tx_block_index;
}

/**
 * Release all or part of the blocks occupied by the buffer.
 *
 * @param[in] tx_block_index	First block index to release, no validation is performed,
 *				so caller is responsible for passing valid index.
 * @param[in] size		Size of data buffer, no validation is performed,
 *				so caller is responsible for passing valid size.
 * @param[in] new_size		If less than zero, release all blocks, otherwise reduce
 *				size to this value and update size in block header.
 *
 * @returns		Positive block index where the buffer starts or negative error
 * @retval -EINVAL	If invalid buffer was provided or size is greater than already
 *			allocated size.
 */
static int release_tx_blocks(struct backend_data *dev_data, size_t tx_block_index,
			     size_t size, int new_size)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	struct block_header *block;
	size_t num_blocks;
	size_t total_size;
	size_t new_total_size;
	size_t new_num_blocks;
	size_t release_index;
	int r;

	/* Calculate number of blocks. */
	total_size = size + BLOCK_HEADER_SIZE;
	num_blocks = (total_size + conf->tx.block_size - 1) / conf->tx.block_size;

	if (new_size >= 0) {
		/* Calculate and validate new values. */
		new_total_size = new_size + BLOCK_HEADER_SIZE;
		new_num_blocks = (new_total_size + conf->tx.block_size - 1) /
				 conf->tx.block_size;
		if (new_num_blocks > num_blocks) {
			LOG_ERR("Requested %d blocks, allocated %d", new_num_blocks,
				num_blocks);
			return -EINVAL;
		}
		/* Update actual buffer size and number of block to release. */
		block = block_from_index(&conf->tx, tx_block_index);
		block->size = new_size;
		release_index = tx_block_index + new_num_blocks;
		num_blocks = num_blocks - new_num_blocks;
	} else {
		/* If size is negative, release all blocks. */
		release_index = tx_block_index;
	}

	if (num_blocks > 0) {
		/* Free bits in the bitmap */
		r = sys_bitarray_free(conf->tx_usage_bitmap, num_blocks,
				      release_index);
		if (r < 0) {
			LOG_ERR("Cannot free bits, err %d", r);
			return r;
		}

		/* Wake up all waiting threads */
		k_sem_give(&dev_data->block_wait_sem);
	}

	return tx_block_index;
}

/**
 * Release all or part of the blocks occupied by the buffer.
 *
 * @param[in] buffer	Buffer to release
 * @param[in] new_size	If less than zero, release all blocks, otherwise reduce size to
 *			this value and update size in block header.
 *
 * @returns		Positive block index where the buffer starts or negative error
 * @retval -EINVAL	If invalid buffer was provided or size is greater than already
 *			allocated size.
 */
static int release_tx_buffer(struct backend_data *dev_data, const uint8_t *buffer,
			     int new_size)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	size_t size;
	int tx_block_index;

	tx_block_index = buffer_to_index_validate(&conf->tx, buffer, &size);
	if (tx_block_index < 0) {
		return tx_block_index;
	}

	return release_tx_blocks(dev_data, tx_block_index, size, new_size);
}

/**
 * Send data with ICmsg with mutex locked. Mutex must be locked because ICmsg may return
 * error on concurrent invocations even when there is enough space in queue.
 */
static int icmsg_send_wrapper(struct backend_data *dev_data, uint8_t addr_or_msg_type,
			      uint8_t tx_block_index)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	uint8_t message[2] = { addr_or_msg_type, tx_block_index };
	int r;

	k_mutex_lock(&dev_data->mutex, K_FOREVER);
	r = icmsg_send(&conf->icmsg_config, &dev_data->icmsg_data, message,
		       sizeof(message));
	k_mutex_unlock(&dev_data->mutex);
	if (r < 0) {
		LOG_ERR("Cannot send over ICmsg, err %d", r);
	}
	return r;
}

/**
 * Release received buffer. This function will just sends release message over ICMsg.
 *
 * @param[in] buffer	Buffer to release
 * @param[in] msg_type	Message type: MSG_RELEASE_BOUND or MSG_RELEASE_DATA
 *
 * @return	zero or ICMsg send error
 */
static int send_release(struct backend_data *dev_data, const uint8_t *buffer,
			uint8_t msg_type)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	int rx_block_index;

	rx_block_index = buffer_to_index_validate(&conf->rx, buffer, NULL);
	if (rx_block_index < 0) {
		return rx_block_index;
	}

	return icmsg_send_wrapper(dev_data, msg_type, rx_block_index);
}

/**
 * Send data contained in specified block. It will adjust data size and flush cache
 * if necessary. If sending failed, allocated blocks will be released.
 *
 * @param[in] tx_block_index	Index of first block containing data, it is not validated,
 *				so caller is responsible for passing only valid index.
 * @param[in] size		Actual size of the data, can be smaller than allocated,
 *				but it cannot change number of required blocks.
 * @param[in] remote_addr	Remote endpoints address
 *
 * @return			O or negative error code
 */
static int send_block(struct backend_data *dev_data, size_t tx_block_index, size_t size,
		      uint8_t remote_addr)
{
	struct block_header *block;
	int r;

	block = block_from_index(&dev_data->conf->tx, tx_block_index);

	block->size = size;
	__sync_synchronize();
	sys_cache_data_flush_range(block, size + BLOCK_HEADER_SIZE);

	r = icmsg_send_wrapper(dev_data, remote_addr, tx_block_index);

	if (r < 0) {
		release_tx_blocks(dev_data, tx_block_index, size, -1);
	}

	return r;
}

/**
 * Find endpoint that was registered with name that matches name
 * contained in the endpoint bound message received from remote. This function
 * must be called when mutex is locked.
 *
 * @param[in] name	Endpoint name, it must be in a received block.
 *
 * @return	Found endpoint data or NULL if not found
 */
static struct ept_data *find_ept_by_name(struct backend_data *dev_data, const char *name)
{
	const struct channel_config *rx_conf = &dev_data->conf->rx;
	const char *buffer_end = (const char *)rx_conf->blocks_ptr +
				 rx_conf->block_count * rx_conf->block_size;
	size_t name_size;
	size_t i;

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
 * Find registered endpoint that matches given "bound endpoint" message. When found,
 * remote endpoint address is saved for future use and "release bound endpoint" message
 * is send. This function must be called when mutex is locked.
 *
 * @param[in] rx_block_index	Block containing the "bound endpoint" message.
 *
 * @retval 0	match not found
 * @retval 1	match found and processing was successful
 * @retval < 0	error occurred during processing
 */
static int match_bound_msg(struct backend_data *dev_data, size_t rx_block_index)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	uint8_t *buffer;
	struct ept_bound_msg *msg;
	struct ept_data *ept;
	struct block_header *block;
	int r = 0;

	block = block_from_index(&conf->rx, rx_block_index);
	buffer = block->data;
	msg = (struct ept_bound_msg *)buffer;

	ept = find_ept_by_name(dev_data, msg->name);

	if (ept == NULL) {
		return 0;
	}

	ept->remote_addr = msg->ept_addr;

	k_mutex_unlock(&dev_data->mutex);
	r = send_release(dev_data, buffer, MSG_RELEASE_BOUND);
	k_mutex_lock(&dev_data->mutex, K_FOREVER);

	if (r < 0) {
		return r;
	}

	return 1;
}

/**
 * Send bound message on specified endpoint.
 *
 * @param[in] ept	Endpoint to use
 *
 * @return		O or negative error code
 */
static int send_bound_message(struct backend_data *dev_data, struct ept_data *ept)
{
	size_t msg_len;
	uint32_t alloc_size;
	uint8_t *buffer;
	struct ept_bound_msg *msg;
	int r;

	msg_len = offsetof(struct ept_bound_msg, name) + strlen(ept->cfg->name) + 1;
	alloc_size = msg_len;
	r = alloc_tx_buffer(dev_data, &alloc_size, &buffer, K_FOREVER);
	if (r >= 0) {
		msg = (struct ept_bound_msg *)buffer;
		msg->ept_addr = ept->local_addr;
		strcpy(msg->name, ept->cfg->name);
		r = send_block(dev_data, r, msg_len, MSG_BOUND);
	}

	return r;
}

/**
 * Put endpoint bound processing into system workqueue.
 */
static void schedule_ept_bound_process(struct backend_data *dev_data)
{
	k_work_submit(&dev_data->ep_bound_work);
}

/**
 * Work handler that is responsible for bounding the endpoints. It sends endpoint
 * bound message to the remote and calls local endpoint bound callback.
 */
static void ept_bound_process(struct k_work *item)
{
	struct backend_data *dev_data = CONTAINER_OF(item, struct backend_data,
						     ep_bound_work);
	struct ept_data *ept = NULL;
	size_t i;
	int r = 0;

	k_mutex_lock(&dev_data->mutex, K_FOREVER);

	/* Skip processing if ICMsg was not bounded yet. */
	if (!dev_data->icmsg_bounded) {
		goto exit;
	}

	/* Walk over all waiting incoming bound messages and match to local endpoints. */
	for (i = 0; i < CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP; i++) {
		if (dev_data->waiting_bound_msg[i] != WAITING_BOUND_MSG_EMPTY) {
			r = match_bound_msg(dev_data, dev_data->waiting_bound_msg[i]);
			if (r != 0) {
				dev_data->waiting_bound_msg[i] = WAITING_BOUND_MSG_EMPTY;
				if (r < 0) {
					goto exit;
				}
			}
		}
	}

	for (i = 0; i < dev_data->ept_count; i++) {
		ept = &dev_data->ept[i];
		/* If any endpoint is configured, start bounding. */
		if (ept->state == EPT_CONFIGURED) {
			ept->state = EPT_BOUNDING;
			k_mutex_unlock(&dev_data->mutex);
			r = send_bound_message(dev_data, ept);
			k_mutex_lock(&dev_data->mutex, K_FOREVER);
			if (r < 0) {
				ept->state = EPT_UNCONFIGURED;
				goto exit;
			}
		/* If any endpoint is bounded and remote address is known, call the
		 * bound callback. */
		} else if (ept->state == EPT_BOUNDED
			   && ept->remote_addr != EPT_ADDR_INVALID) {
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
		LOG_ERR("Failed to process bounding, err %d", r);
	}
}

/**
 * Data message received.
 */
static int received_data(struct backend_data *dev_data, size_t rx_block_index,
			 int local_addr)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	uint8_t *buffer;
	struct ept_data *ept;
	size_t size;
	int bit_val;
	int r = 0;

	/* Validate */
	buffer = buffer_from_index_validate(&conf->rx, rx_block_index, &size, true);
	if (buffer == NULL || local_addr >= dev_data->ept_count) {
		LOG_ERR("Received invalid block index %d or addr %d", rx_block_index,
			local_addr);
		return -EINVAL;
	}

	/* Clear bit. If cleared, specific block will not be hold after the callback. */
	sys_bitarray_clear_bit(conf->rx_hold_bitmap, rx_block_index);

	/* Call the endpoint callback. It can set the hold bit. */
	ept = &dev_data->ept[local_addr];
	ept->cfg->cb.received(buffer, size, ept->cfg->priv);

	/* If the bit is still cleared, request release of the buffer. */
	sys_bitarray_test_bit(conf->rx_hold_bitmap, rx_block_index, &bit_val);
	if (!bit_val) {
		send_release(dev_data, buffer, MSG_RELEASE_DATA);
	}

	return r;
}

/**
 * Release data message received.
 */
static int received_release_data(struct backend_data *dev_data, size_t tx_block_index)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	uint8_t *buffer;
	size_t size;

	buffer = buffer_from_index_validate(&conf->tx, tx_block_index, &size, false);
	if (buffer == NULL) {
		return -EINVAL;
	}

	return release_tx_blocks(dev_data, tx_block_index, size, -1);
}

/**
 * Bound endpoint message received.
 */
static int received_bound(struct backend_data *dev_data, size_t rx_block_index)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	size_t size;
	uint8_t *buffer;
	size_t i;
	int r = -ENOMEM;

	buffer = buffer_from_index_validate(&conf->rx, rx_block_index, &size, true);
	if (buffer == NULL) {
		LOG_ERR("Received invalid block index %d", rx_block_index);
		return -EINVAL;
	}

	k_mutex_lock(&dev_data->mutex, K_FOREVER);

	/* Find empty entry in waiting list and put this block to it. */
	for (i = 0; i < CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP; i++) {
		if (dev_data->waiting_bound_msg[i] == WAITING_BOUND_MSG_EMPTY) {
			dev_data->waiting_bound_msg[i] = rx_block_index;
			r = 0;
			break;
		}
	}

	k_mutex_unlock(&dev_data->mutex);

	/* Schedule processing the message */
	schedule_ept_bound_process(dev_data);

	if (r < 0) {
		LOG_ERR("Too many remote endpoints");
	}

	return r;
}

/**
 * Release bound endpoint message received.
 */
static int received_release_bound(struct backend_data *dev_data, size_t tx_block_index)
{
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	uint8_t *buffer;
	struct ept_bound_msg *msg;
	size_t local_addr;
	size_t size;
	int r = 0;

	buffer = buffer_from_index_validate(&conf->tx, tx_block_index, &size, false);
	if (buffer == NULL) {
		return -EINVAL;
	}

	msg = (struct ept_bound_msg *)buffer;
	local_addr = msg->ept_addr;

	r = release_tx_blocks(dev_data, tx_block_index, size, -1);

	if (local_addr >= dev_data->ept_count) {
		LOG_ERR("Invalid address %d", local_addr);
		return -EINVAL;
	}

	k_mutex_lock(&dev_data->mutex, K_FOREVER);
	dev_data->ept[local_addr].state = EPT_BOUNDED;
	k_mutex_unlock(&dev_data->mutex);

	schedule_ept_bound_process(dev_data);

	return r;
}

/**
 * Callback called by ICMsg that handles message (data or endpoint bound) received
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
	size_t addr_or_msg_type;
	size_t block_index = 0;
	int r;

	if (len != 2) {
		r = -EINVAL;
		goto exit;
	}

	addr_or_msg_type = message[0];
	block_index = message[1];

	switch (addr_or_msg_type) {
	case MSG_RELEASE_BOUND:
		r = received_release_bound(dev_data, block_index);
		break;
	case MSG_RELEASE_DATA:
		r = received_release_data(dev_data, block_index);
		break;
	case MSG_BOUND:
		r = received_bound(dev_data, block_index);
		break;
	default:
		r = received_data(dev_data, block_index, addr_or_msg_type);
		break;
	}

exit:
	if (r < 0) {
		LOG_ERR("Failed to receive, err %d", r);
	}
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

	LOG_DBG("Open instance 0x%08X", (uint32_t)instance);
	LOG_DBG("  ICmsg, TX %d at 0x%08X, RX %d at 0x%08X",
		(uint32_t)conf->icmsg_config.tx_shm_size,
		(uint32_t)conf->icmsg_config.tx_shm_addr,
		(uint32_t)conf->icmsg_config.tx_shm_size,
		(uint32_t)conf->icmsg_config.tx_shm_addr);
	LOG_DBG("  TX %d blocks of %d bytes at 0x%08X, max allocable %d bytes",
		(uint32_t)conf->tx.block_count,
		(uint32_t)conf->tx.block_size,
		(uint32_t)conf->tx.blocks_ptr,
		(uint32_t)(conf->tx.block_size * conf->tx.block_count -
			   BLOCK_HEADER_SIZE));
	LOG_DBG("  RX %d blocks of %d bytes at 0x%08X, max allocable %d bytes",
		(uint32_t)conf->rx.block_count,
		(uint32_t)conf->rx.block_size,
		(uint32_t)conf->rx.blocks_ptr,
		(uint32_t)(conf->rx.block_size * conf->tx.block_count -
			   BLOCK_HEADER_SIZE));

	return icmsg_open(&conf->icmsg_config, &dev_data->icmsg_data, &cb,
			  (void *)instance);
}

/**
 * Endpoint send callback function (with copy).
 */
static int send(const struct device *instance, void *token, const void *msg, size_t len)
{
	struct backend_data *dev_data = instance->data;
	struct ept_data *ept = token;
	uint32_t alloc_size;
	uint8_t *buffer;
	int r;

	/* Allocate the buffer. */
	alloc_size = len;
	r = alloc_tx_buffer(dev_data, &alloc_size, &buffer, K_FOREVER);
	if (r < 0) {
		return r;
	}

	/* Copy data to allocated buffer. */
	memcpy(buffer, msg, len);

	/* Send data message. */
	return send_block(dev_data, r, len, ept->remote_addr);
}

/**
 * Backend endpoint registration callback.
 */
static int register_ept(const struct device *instance, void **token,
			const struct ipc_ept_cfg *cfg)
{
	struct backend_data *dev_data = instance->data;
	struct ept_data *ept = NULL;
	int r = 0;

	k_mutex_lock(&dev_data->mutex, K_FOREVER);

	/* Add new endpoint. */
	if (dev_data->ept_count < CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP) {
		ept = &dev_data->ept[dev_data->ept_count];
		ept->cfg = cfg;
		ept->local_addr = dev_data->ept_count;
		ept->remote_addr = EPT_ADDR_INVALID;
		ept->state = EPT_CONFIGURED;
		dev_data->ept_count++;
	} else {
		r = -ENOMEM;
		LOG_ERR("Too many endpoints");
		__ASSERT_NO_MSG(false);
	}

	k_mutex_unlock(&dev_data->mutex);

	/* Keep endpoint address in token. */
	*token = ept;

	/* Rest of the bounding will be done in the system workqueue. */
	schedule_ept_bound_process(dev_data);

	return r;
}

/**
 * Returns maximum TX buffer size.
 */
static int get_tx_buffer_size(const struct device *instance, void *token)
{
	const struct icmsg_with_buf_config *conf = instance->config;

	return conf->tx.block_size * conf->tx.block_count - BLOCK_HEADER_SIZE;
}

/**
 * Endpoint TX buffer allocation callback for nocopy sending.
 */
static int get_tx_buffer(const struct device *instance, void *token, void **data,
			 uint32_t *user_len, k_timeout_t wait)
{
	struct backend_data *dev_data = instance->data;
	int r;

	r = alloc_tx_buffer(dev_data, user_len, (uint8_t **)data, wait);
	if (r < 0) {
		return r;
	}
	return 0;
}

/**
 * Endpoint TX buffer release callback for nocopy sending.
 */
static int drop_tx_buffer(const struct device *instance, void *token, const void *data)
{
	struct backend_data *dev_data = instance->data;

	return release_tx_buffer(dev_data, data, -1);
}

/**
 * Endpoint nocopy sending.
 */
static int send_nocopy(const struct device *instance, void *token, const void *data,
		       size_t len)
{
	struct backend_data *dev_data = instance->data;
	struct ept_data *ept = token;
	int r;

	/* Actual size may be smaller than requested, so shrink if possible. */
	r = release_tx_buffer(dev_data, data, len);
	if (r < 0) {
		release_tx_buffer(dev_data, data, -1);
		return r;
	}

	return send_block(dev_data, r, len, ept->remote_addr);
}

/**
 * Holding RX buffer for nocopy receiving.
 */
static int hold_rx_buffer(const struct device *instance, void *token, void *data)
{
	const struct icmsg_with_buf_config *conf = instance->config;
	int rx_block_index;
	uint8_t *buffer = data;

	/* Calculate block index and set associated bit. */
	rx_block_index = buffer_to_index_validate(&conf->rx, buffer, NULL);
	__ASSERT_NO_MSG(rx_block_index >= 0);
	return sys_bitarray_set_bit(conf->rx_hold_bitmap, rx_block_index);
}

/**
 * Release RX buffer that was previously held.
 */
static int release_rx_buffer(const struct device *instance, void *token, void *data)
{
	struct backend_data *dev_data = instance->data;

	return send_release(dev_data, (uint8_t *)data, MSG_RELEASE_DATA);
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
	k_work_init(&dev_data->ep_bound_work, ept_bound_process);
	k_sem_init(&dev_data->block_wait_sem, 0, 1);
	memset(&dev_data->waiting_bound_msg, 0xFF, sizeof(dev_data->waiting_bound_msg));
	return 0;
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
 * because it can hold data message for each local block and release message
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
	BUILD_ASSERT(DT_INST_PROP(i, rx_blocks) <= 256,					\
		     "Too many RX blocks");						\
	BUILD_ASSERT(DT_INST_PROP(i, tx_blocks) <= 256,					\
		     "Too many TX blocks");						\
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
	size_t i;
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
