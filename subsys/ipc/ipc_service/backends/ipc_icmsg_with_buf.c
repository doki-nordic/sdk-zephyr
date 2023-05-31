/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ipc_svc_icmsg_w_buf, CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_LOG_LEVEL);

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/sys/bitarray.h>
#include <zephyr/ipc/icmsg.h>
#include <zephyr/ipc/ipc_service_backend.h>
#include <zephyr/cache.h>

#define DT_DRV_COMPAT zephyr_ipc_icmsg_with_buf

/** Special endpoint address indicating invalid (or empty) entry. */
#define EPT_ADDR_INVALID 0xFF

/** Message type for deallocating data buffers. */
#define MSG_FREE_DATA 0xFE

/** Message type for deallocating endpoint registration buffers. */
#define MSG_FREE_BOUND_EPT 0xFD

/** Message type for endpoint bounding message. */
#define MSG_BOUND_EPT 0xFC

/** Maximum value of endpoint address. */
#define EPT_ADDR_MAX 0xFB

/** Special value for empty entry in bound message waiting table. */
#define WAITING_BOUND_MSG_EMPTY 0xFFFF

#define BLOCK_ALIGNMENT sizeof(size_t)
#define BYTES_PER_ICMSG_MESSAGE 8
#define ICMSG_BUFFER_OVERHEAD (2 * (sizeof(struct spsc_pbuf) + BYTES_PER_ICMSG_MESSAGE))

#if 1

#define PUSH do {} while (0)
#define POP do {} while (0)
#define SHOW(...) do {} while (0)

#else

const char *call_stack[256];
int call_stack_size = 0;

#define PUSH call_push(__FUNCTION__);
#define POP call_pop();
#define SHOW(...) do { printk(__VA_ARGS__); call_show(); } while (0)

static void call_push(const char *name) {
	call_stack[call_stack_size++] = name;
}

static void call_pop() {
	call_stack_size--;
}

static void call_show() {
	int i;
	for (i = 0; i < call_stack_size; i++) {
		printk("TR: %s\n", call_stack[i]);
	}
}

#endif

#define printk1(...)
#define printk2(...)

/*
 * Shared memory layout:
 *
 *     | ICMsg queue | block 0 | block 1 | ... | block N-1 |
 *
 *     This shows layout of one channel. There are two channels RX and TX per instance.
 *     ICMsg queue contains enough space to hold at least the same number of messages
 *     as number of blocks on both sides, so it will never overflow.
 *
 * Block layout:
 *
 *     | size_t data_size | data ... |
 *
 *     If buffer spawns more than one block, only the first block contains
 *     'data_size'. 'data_size' value does not include the 'data_size' field.
 *     The 'data_size' field is located in the shared memory, so extra care must
 *     be taken to prevent buffer overflow (or similar) attacks if we don't trust
 *     the other side.
 *
 * ICMsg messages:
 *
 *     | uint8_t message_type_or_ept_addr | uint8_t block_index |
 *
 *     There are four types of messages:
 *
 *     - Endpoint bound (MSG_BOUND_EPT) - send from each side to start bounding process.
 *       Buffer contains:
 *       uint8_t sender endpoint address
 *       char[] null-terminated endpoint name
 *
 *     - Endpoint bound free (MSG_FREE_BOUND_EPT) - send in response to MSG_BOUND_EPT to
 *       deallocate the buffer and inform that bounding request has completed.
 *
 *     - Data (receiver endpoint address) - send the actual payload over specified endpoint.
 *
 *     - Data free (MSG_FREE_DATA) - send in response to "data" message to deallocate
 *       the buffer.
 *
 * Endpoint bounding process:
 *
 *     The bounding process is symmetric - both sides are the same. Endpoint addresses may
 *     be different on each side, so each side must know both local and remote address.
 *     Bounding processes is done in two parallel jobs:
 *     1) Informing remote site about local address and name.
 *        * Local side sends "Endpoint bound" message with local address and name.
 *        * Local side waits for deallocation of above message to confirm that it was
 *          successfully processed by the remote side.
 *     2) Getting remote address of the endpoint.
 *        * Local side waits for the "Endpoint bound" message.
 *        * It associates remote address with local endpoint
 *        * It sends "Endpoint bound free" to deallocate the above message and inform
 *          that it was successfully processed.
 *     When those two jobs complete, the endpoint is bounded, callback is called and
 *     the endpoint is ready to transfer data.
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
	uint8_t local_addr;
	uint8_t remote_addr;
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
	uint8_t ept_addr;
	char name[];
};

BUILD_ASSERT(CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP <= EPT_ADDR_MAX + 1,
	     "Too many endpoints");

static struct block_header *block_from_index(const struct channel_config *ch_conf, size_t block_index)
{
	return (struct block_header *)(ch_conf->blocks_ptr + block_index * ch_conf->block_size);
}

static uint8_t *buffer_from_index_validate(const struct channel_config *ch_conf, size_t block_index, size_t *size, bool invalidate_cache)
{
	PUSH;
	size_t allocable_size;
	size_t buffer_size;
	uint8_t *end_ptr;
	struct block_header *block = block_from_index(ch_conf, block_index);

	if (block_index >= ch_conf->block_count) {
		LOG_ERR("Pointer invalid");
		SHOW("Pointer invalid\n");
		POP;
		return NULL;
	}

	if (size != NULL) {
		allocable_size = ch_conf->block_count * ch_conf->block_size;
		end_ptr = ch_conf->blocks_ptr + allocable_size;
		if (invalidate_cache) {
			sys_cache_data_invd_range(block, ch_conf->block_size);
			__sync_synchronize();
		}
		buffer_size = block->size;
		if (buffer_size > allocable_size - offsetof(struct block_header, data) || &block->data[buffer_size] > end_ptr) {
			LOG_ERR("Block corrupted");
			POP;
			return NULL;
		}
		*size = buffer_size;
		if (invalidate_cache && buffer_size > ch_conf->block_size - offsetof(struct block_header, data)) {
			sys_cache_data_invd_range(block, buffer_size + offsetof(struct block_header, data));
			__sync_synchronize();
		}
	}

	POP;
	return block->data;
}

static int buffer_to_index_validate(const struct channel_config *ch_conf, const uint8_t *buffer, size_t *size)
{PUSH;
	size_t block_index;
	uint8_t *expected;

	block_index = (buffer - ch_conf->blocks_ptr) / ch_conf->block_size;

	expected = buffer_from_index_validate(ch_conf, block_index, size, false);

	if (expected == NULL || expected != buffer) {
		LOG_ERR("Pointer invalid");
		SHOW("Pointer invalid\n");
		POP;
		return -EINVAL;
	}

	POP;
	return block_index;
}

/**
 * Send data with ICmsg with mutex locked. Mutex must be locked because ICmsg may return
 * error on concurrent invocations even when there is enough space in queue.
 */
static int icmsg_send_wrapper(struct backend_data *dev_data, uint8_t addr_or_msg_type, uint8_t block_index)
{PUSH;
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	uint8_t message[2] = { addr_or_msg_type, block_index };
	uint8_t *buffer;
	size_t size;
	int r;

	switch (addr_or_msg_type) {
	case MSG_FREE_DATA:
		buffer = buffer_from_index_validate(&dev_data->conf->rx, block_index, &size, false);
		printk1(">>> Free Data @%d, size=%d\n", block_index, size);
		break;
	case MSG_FREE_BOUND_EPT:
		buffer = buffer_from_index_validate(&dev_data->conf->rx, block_index, NULL, false);
		printk1(">>> Free Bound Msg @%d, rem_addr=%d, name=%s\n", block_index, buffer ? buffer[0] : -1, buffer ? (char*)&buffer[1] : "?");
		break;
	case MSG_BOUND_EPT:
		buffer = buffer_from_index_validate(&dev_data->conf->tx, block_index, NULL, false);
		printk1(">>> Bound Msg @%d, loc_addr=%d, name=%s\n", block_index, buffer ? buffer[0] : -1, buffer ? (char*)&buffer[1] : "?");
		break;
	default:
		buffer = buffer_from_index_validate(&dev_data->conf->tx, block_index, &size, false);
		printk1(">>> Data @%d, rem_addr=%d, size=%d\n", block_index, addr_or_msg_type, size);
		break;
	}

	k_mutex_lock(&dev_data->mutex, K_FOREVER);
	r = icmsg_send(&conf->icmsg_config, &dev_data->icmsg_data, message,
		       sizeof(message));
	k_mutex_unlock(&dev_data->mutex);
	if (r < 0) {
		LOG_ERR("Cannot send over ICmsg, error: %d", r);
	}
	POP;
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
			   uint8_t **buffer, k_timeout_t timeout)
{PUSH;
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	size_t total_size = size + offsetof(struct block_header, data);
	size_t num_blocks = (total_size + conf->tx.block_size - 1) / conf->tx.block_size;
	struct block_header *block;
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
		if (r != -ENOSPC && r != EAGAIN) {
			LOG_ERR("Failed to allocate buffer, error: %d", r);
		}
		POP;
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
	block = block_from_index(&conf->tx, tx_block_index);
	block->size = conf->tx.block_size * num_blocks - offsetof(struct block_header,
								  data);

	if (buffer != NULL) {
		*buffer = block->data;
	}

	POP;
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
{PUSH;
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
		POP;
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
			LOG_ERR("Requested size bigger than allocated");
			__ASSERT_NO_MSG(false);
			POP;
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
			LOG_ERR("Cannot free bits");
			__ASSERT_NO_MSG(false);
			POP;
			return r;
		}

		/* Wake up all waiting threads */
		k_sem_give(&dev_data->block_wait_sem);
	}

	POP;
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
{PUSH;
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
			POP;
			return &dev_data->ept[i];
		}
	}
	POP;
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
static int free_rx_buffer(struct backend_data *dev_data, const uint8_t *buffer, uint8_t msg_type)
{PUSH;
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	int block_index;

	block_index = buffer_to_index_validate(&conf->rx, buffer, NULL);
	if (block_index < 0) {
		POP;
		return block_index;
	}

	SHOW("free_rx_buffer msg 0x%0X bl %d\n", msg_type, block_index);

	POP;
	return icmsg_send_wrapper(dev_data, msg_type, block_index);
}

static int received_free_reg_ept(struct backend_data *dev_data, size_t tx_block_index)
{PUSH;
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	uint8_t *buffer;
	struct ept_bound_msg *msg;
	size_t local_addr;
	int r = 0;

	buffer = buffer_from_index_validate(&conf->tx, tx_block_index, NULL, false);
	if (buffer == NULL) {
		POP;
		return -EINVAL;
	}

	msg = (struct ept_bound_msg *)buffer;
	local_addr = msg->ept_addr;

	r = free_tx_buffer(dev_data, buffer, -1);

	if (local_addr >= CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP) {
		LOG_ERR("Cannot free bits");
		__ASSERT_NO_MSG(false);
		POP;
		return -EINVAL;
	}

	k_mutex_lock(&dev_data->mutex, K_FOREVER);
	dev_data->ept[local_addr].state = EPT_BOUNDED;
	k_mutex_unlock(&dev_data->mutex);

	SHOW("received_free_reg_ept - schedule_ept_bound_process %d ep %d bl %d\n", r, local_addr, tx_block_index);

	while (r < 0);

	schedule_ept_bound_process(dev_data);

	POP;
	return r;
}

static int received_free_data(struct backend_data *dev_data, size_t tx_block_index)
{PUSH;
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	uint8_t *buffer;

	buffer = buffer_from_index_validate(&conf->tx, tx_block_index, NULL, false);
	if (buffer == NULL) {
		POP;
		return -EINVAL;
	}

	POP;
	return free_tx_buffer(dev_data, buffer, -1);
}

static int received_register_ept(struct backend_data *dev_data, size_t rx_block_index)
{PUSH;
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	size_t size;
	uint8_t *buffer;
	bool is_bounded;
	int i;
	int r = -ENOMEM;

	buffer = buffer_from_index_validate(&conf->rx, rx_block_index, &size, true);
	if (buffer == NULL) {
		LOG_ERR("Received invalid block index: %d", rx_block_index);
		__ASSERT_NO_MSG(false);
		POP;
		return -EINVAL;
	}

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
		printk2("received_register_ept - schedule_ept_bound_process %d\n", r);
		schedule_ept_bound_process(dev_data);
	}

	if (r < 0) {
		LOG_ERR("Remote requires more endpoints than available");
		__ASSERT_NO_MSG(false);
	}

	POP;
	return r;
}

static int received_data(struct backend_data *dev_data, size_t rx_block_index, int local_addr)
{PUSH;
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	uint8_t *buffer;
	struct ept_data *ept;
	size_t size;
	int bit_val;
	int r = 0;

	/* Validate */
	buffer = buffer_from_index_validate(&conf->rx, rx_block_index, &size, true);
	if (buffer == NULL ||
	    local_addr >= CONFIG_IPC_SERVICE_BACKEND_ICMSG_WITH_BUF_NUM_EP) {
		LOG_ERR("Received invalid block index: %d", rx_block_index);
		__ASSERT_NO_MSG(false);
		POP;
		return -EINVAL;
	}

	/* Clear bit. If cleared, specific block will not be hold after the callback. */
	sys_bitarray_clear_bit(conf->rx_hold_bitmap, rx_block_index);

	/* Call the endpoint callback. It can set the hold bit. */
	ept = &dev_data->ept[local_addr];
	ept->cfg->cb.received(buffer, size, ept->cfg->priv);

	/* If the bit is still cleared, request deallocation of the buffer. */
	sys_bitarray_test_bit(conf->rx_hold_bitmap, rx_block_index, &bit_val);
	if (!bit_val) {
		free_rx_buffer(dev_data, buffer, MSG_FREE_DATA);
	}

	POP;
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
{PUSH;
	const struct device *instance = priv;
	struct backend_data *dev_data = instance->data;
	const uint8_t *message = (const uint8_t *)data;
	size_t addr_or_msg_type;
	size_t block_index = 0;
	size_t size;
	uint8_t *buffer;
	int r;

	if (len != 2) {
		r = -EINVAL;
		goto exit;
	}

	addr_or_msg_type = message[0];
	block_index = message[1];

	switch (addr_or_msg_type) {
	case MSG_FREE_DATA:
		buffer = buffer_from_index_validate(&dev_data->conf->tx, block_index, &size, false);
		printk1("<<< Free Data @%d, size=%d\n", block_index, size);
		break;
	case MSG_FREE_BOUND_EPT:
		buffer = buffer_from_index_validate(&dev_data->conf->tx, block_index, NULL, false);
		printk1("<<< Free Bound Msg @%d, loc_addr=%d, name=%s\n", block_index, buffer ? buffer[0] : -1, buffer ? (char*)&buffer[1] : "?");
		break;
	case MSG_BOUND_EPT:
		buffer = buffer_from_index_validate(&dev_data->conf->rx, block_index, &size, true);
		printk1("<<< Bound Msg @%d, rem_addr=%d, name=%s\n", block_index, buffer ? buffer[0] : -1, buffer ? (char*)&buffer[1] : "?");
		break;
	default:
		buffer = buffer_from_index_validate(&dev_data->conf->rx, block_index, &size, true);
		printk1("<<< Data @%d, loc_addr=%d, size=%d\n", block_index, addr_or_msg_type, size);
		break;
	}

	switch (addr_or_msg_type) {
	case MSG_FREE_BOUND_EPT:
		r = received_free_reg_ept(dev_data, block_index);
		break;
	case MSG_FREE_DATA:
		r = received_free_data(dev_data, block_index);
		break;
	case MSG_BOUND_EPT:
		r = received_register_ept(dev_data, block_index);
		break;
	default:
		r = received_data(dev_data, block_index, addr_or_msg_type);
		break;
	}

exit:
	if (r < 0) {
		LOG_ERR("Failed to receive, error %d", r);
		__ASSERT_NO_MSG(false);
	}
	POP;
}

/**
 * Callback called when ICMsg is bound.
 */
static void bound(void *priv)
{PUSH;
	const struct device *instance = priv;
	struct backend_data *dev_data = instance->data;

	printk2("ICmsg bounded\n");

	/* Set flag that ICMsg is bounded and now, endpoint bounding may start. */
	k_mutex_lock(&dev_data->mutex, K_FOREVER);
	dev_data->icmsg_bounded = true;
	k_mutex_unlock(&dev_data->mutex);
	printk2("bound - schedule_ept_bound_process\n");
	schedule_ept_bound_process(dev_data);
	POP;
}

static int send_block(struct backend_data *dev_data, size_t tx_block_index, size_t size, uint8_t remote_addr)
{PUSH;
	struct block_header *block = block_from_index(&dev_data->conf->tx, tx_block_index);

	block->size = size;
	__sync_synchronize();
	sys_cache_data_flush_range(block, size + offsetof(struct block_header, data));
	POP;
	return icmsg_send_wrapper(dev_data, remote_addr, tx_block_index);
}

static int send_bound_message(struct backend_data *dev_data, struct ept_data *ept)
{PUSH;
	size_t msg_len;
	uint8_t *buffer;
	struct ept_bound_msg *msg;
	int r;

	printk2("bound message send\n");

	msg_len = offsetof(struct ept_bound_msg, name) + strlen(ept->cfg->name) + 1;
	r = alloc_tx_buffer(dev_data, msg_len, &buffer, K_FOREVER);
	if (r >= 0) {
		msg = (struct ept_bound_msg *)buffer;
		msg->ept_addr = ept->local_addr;
		strcpy(msg->name, ept->cfg->name);
		r = send_block(dev_data, r, msg_len, MSG_BOUND_EPT);
		/* ICMsg queue should have enough space for all blocks. */
		__ASSERT_NO_MSG(r == 0);
	} else {
		/* EP name cannot be bigger then entire allocable space. */
		__ASSERT_NO_MSG(false);
	}

	POP;
	return r;
}

static int match_bound_msg(struct backend_data *dev_data, size_t rx_block_index) // TODO: reconsider adding rx/tx prefix in other paces
{PUSH;
	const struct icmsg_with_buf_config *conf = dev_data->conf;
	uint8_t *buffer;
	uint8_t remote_addr;
	struct ept_bound_msg *msg;
	struct ept_data *ept;
	int r = 0;

	buffer = block_from_index(&conf->rx, rx_block_index)->data;
	msg = (struct ept_bound_msg *)buffer;
	remote_addr = msg->ept_addr;

	ept = find_ept_by_name(dev_data, msg->name);

	if (ept == NULL) {
		POP;
		return 0;
	}

	ept->remote_addr = remote_addr;

	printk2("Remote %d bounded with local %d\n", remote_addr, ept->local_addr);

	k_mutex_unlock(&dev_data->mutex);
	r = free_rx_buffer(dev_data, buffer, MSG_FREE_BOUND_EPT);
	k_mutex_lock(&dev_data->mutex, K_FOREVER);

	if (r < 0) {
		POP;
		return r;
	}

	POP;
	return 1;
}

/**
 * Work handler that is responsible for bounding the endpoints. It sends endpoint
 * registration requests to the remote and calls local endpoint bound callback.
 */
static void ep_bound_process(struct k_work *item)
{PUSH;
	struct backend_data *dev_data = CONTAINER_OF(item, struct backend_data, ep_bound_work);
	struct ept_data *ept = NULL;
	int i;
	int r = 0;

	k_mutex_lock(&dev_data->mutex, K_FOREVER);

	/* Skip processing if ICMsg was not bounded yet. */
	if (!dev_data->icmsg_bounded) {
		goto exit;
	}

	printk2("ep_bound_process\n");

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
		if (ept->state == EPT_CONFIGURED) {
			ept->state = EPT_BOUNDING;
			k_mutex_unlock(&dev_data->mutex);
			r = send_bound_message(dev_data, ept);
			k_mutex_lock(&dev_data->mutex, K_FOREVER);
			if (r < 0) {
				ept->state = EPT_UNCONFIGURED;
				goto exit;
			}
		} else if (ept->state == EPT_BOUNDED && ept->remote_addr != EPT_ADDR_INVALID) {
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
		printk2("schedule_ept_bound_process %d\n", r);
		schedule_ept_bound_process(dev_data);
		LOG_ERR("Failed to process bounding, error %d", r);
		__ASSERT_NO_MSG(false);
	}
	POP;
}

/**
 * Backend device initialization.
 */
static int backend_init(const struct device *instance)
{PUSH;
	const struct icmsg_with_buf_config *conf = instance->config;
	struct backend_data *dev_data = instance->data;

	printk2("Backend initialization\n");

	dev_data->conf = conf;
	k_mutex_init(&dev_data->mutex);
	k_work_init(&dev_data->ep_bound_work, ep_bound_process);
	k_sem_init(&dev_data->block_wait_sem, 0, 1);
	memset(&dev_data->waiting_bound_msg, 0xFF, sizeof(dev_data->waiting_bound_msg));
	POP;
	return 0;
}

/**
 * Open the backend instance callback.
 */
static int open(const struct device *instance)
{PUSH;
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
			   offsetof(struct block_header, data)));
	LOG_DBG("  RX %d blocks of %d bytes at 0x%08X, max allocable %d bytes",
		(uint32_t)conf->rx.block_count,
		(uint32_t)conf->rx.block_size,
		(uint32_t)conf->rx.blocks_ptr,
		(uint32_t)(conf->rx.block_size * conf->tx.block_count -
			   offsetof(struct block_header, data)));
	POP;
	return icmsg_open(&conf->icmsg_config, &dev_data->icmsg_data, &cb, (void *)instance);
}

/**
 * Backend endpoint registration callback.
 */
static int register_ept(const struct device *instance, void **token,
			const struct ipc_ept_cfg *cfg)
{PUSH;
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
		printk1("EP %d: %s\n", ept->local_addr, ept->cfg->name);
	} else {
		r = -ENOMEM;
		LOG_ERR("Too many endpoints registered");
		__ASSERT_NO_MSG(false);
	}

	k_mutex_unlock(&dev_data->mutex);

	/* Keep endpoint address in token. */
	*token = ept;

	printk2("Register EP %d\n", r);

	/* Rest of the bounding will be done in the system workqueue. */
	printk2("register_ept - schedule_ept_bound_process %d\n", r);
	schedule_ept_bound_process(dev_data);

	printk2("return %d\n", r);

	POP;
	return r;
}

/**
 * Endpoint send callback function (with copy).
 */
static int send(const struct device *instance, void *token,
		const void *msg, size_t len)
{PUSH;
	struct backend_data *dev_data = instance->data;
	struct ept_data *ept = token;
	uint8_t *buffer;
	int r; // TODO: reconsider naming return values that holds actual data

	/* Allocate the buffer. */
	r = alloc_tx_buffer(dev_data, len, &buffer, K_FOREVER);
	if (r < 0) {
		POP;
		return r;
	}

	/* Copy data to allocated buffer. */
	memcpy(buffer, msg, len);

	/* Send data message. */
	r = send_block(dev_data, r, len, ept->remote_addr);

	/* Remove buffer if something gone wrong - buffer was not send */
	if (r < 0) {
		free_tx_buffer(dev_data, buffer, -1);
	}

	POP;
	return r;
}

/**
 * Endpoint TX buffer allocation callback for nocopy sending.
 */
static int get_tx_buffer(const struct device *instance, void *token,
			 void **data, uint32_t *user_len, k_timeout_t wait)
{
	struct backend_data *dev_data = instance->data;
	struct block_header *block;
	int r;

	r = alloc_tx_buffer(dev_data, *user_len, (uint8_t **)data, wait);
	if (r >= 0) {
		block = CONTAINER_OF(*data, struct block_header, data);
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
{PUSH;
	struct backend_data *dev_data = instance->data;

	POP;
	return free_tx_buffer(dev_data, data, -1);
}

/**
 * Endpoint nocopy sending.
 */
static int send_nocopy(const struct device *instance, void *token,
			const void *data, size_t len)
{PUSH;
	struct backend_data *dev_data = instance->data;
	struct ept_data *ept = token;
	int r;

	/* Actual size may be smaller than requested, so shrink if possible. */
	r = free_tx_buffer(dev_data, data, len);
	if (r < 0) {
		free_tx_buffer(dev_data, data, -1);
		POP;
		return r;
	}

	r = send_block(dev_data, r, len, ept->remote_addr);

	/* Remove buffer if something gone wrong - buffer was not send */
	if (r < 0) {
		free_tx_buffer(dev_data, data, -1);
	}

	POP;
	return r;
}

/**
 * Holding RX buffer for nocopy receiving.
 */
static int hold_rx_buffer(const struct device *instance, void *token, void *data)
{PUSH;
	const struct icmsg_with_buf_config *conf = instance->config;
	int block_index;
	uint8_t *buffer = data;

	/* Calculate block index and set associated bit. */
	block_index = buffer_to_index_validate(&conf->rx, buffer, NULL);
	if (block_index < 0) {
		POP;
		return block_index;
	}
	POP;
	return sys_bitarray_set_bit(conf->rx_hold_bitmap, block_index);
}

/**
 * Release RX buffer that was previously held.
 */
static int release_rx_buffer(const struct device *instance, void *token, void *data)
{PUSH;
	struct backend_data *dev_data = instance->data;

	POP;
	return free_rx_buffer(dev_data, (uint8_t *)data, MSG_FREE_DATA);
}

/**
 * Returns maximum TX buffer size.
 */
static int get_tx_buffer_size(const struct device *instance, void *token)
{PUSH;
	const struct icmsg_with_buf_config *conf = instance->config;

	POP;
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

/*

Idea for reduced variant (around 50% size)
------------------------------------------

Device tree entry has optional field "max-epts". By default, it is EPT_ADDR_MAX,
so it is actually taken from the Kconfig. If it is provided and it is "1", then
simpler protocol can be used, without bounding and endpoint addressing.
There should be 3 variants that can be determined on build time:
- all instances have >1 endpoints - use standard protocol always
- one instance has 1 endpoint other have more - use standard protocol, but do not
  wait for the remote bound message on instance that has 1 endpoint. Assume
  remote_addr=0 during endpoint configuration, this will prevent waiting.
  Skip "bound message" and jump immediately to "bounded" state.
  Also, ICmsg open must be moved to the endpoint registration.
- all instances have 1 endpoint - open ICmsg on endpoint registration and call
  bounded callback on ICmsg bounded callback. Remove unnecessary structures and
  fields, e.g. ept_bounding_state, ept_data, ep_bound_work, waiting_bound_msg,
  ept_count, icmsg_bounded, ept_bound_msg, Remove unnecessary functions, e.g.
  received_free_reg_ept, ep_bound_process

Detection of variant:
#define BACKEND_DEVICE_EPT_CNT_AND(i) && (DT_INST_PROP(i, max_epts) - 1)
#define BACKEND_DEVICE_EPT_CNT_OR(i) || (DT_INST_PROP(i, max_epts) - 1)
#define SINGLE_EPT (!(1 DT_INST_FOREACH_STATUS_OKAY(BACKEND_DEVICE_EPT_CNT_AND)))
#define MULTI_EPT (0 DT_INST_FOREACH_STATUS_OKAY(BACKEND_DEVICE_EPT_CNT_OR))

MULTI_EPT && !SINGLE_EPT  - standard protocol
MULTI_EPT && SINGLE_EPT   - standard protocol with reduced support
!MULTI_EPT && SINGLE_EPT  - reduced protocol
!MULTI_EPT && !SINGLE_EPT - N/A


Create optional statistics
--------------------------

Statistics may include:
- number of messages/bytes send/received
- minimum, maximum, and average packet size
- maximum and average allocation wait time
- maximum blocks usage
- histogram of log2(packet size)

They will be printed periodically.


Add optional data sniffing over J-Link RTT
------------------------------------------

All send and received messages will go to the RTT data channel (including timestamps).
On PC side, RTT log will be taken and some python script will convert it to readable
format, e.g. HTML

*/