/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** \file
 * Set of workarounds for rte_vhost to make it work with device types
 * other than vhost-net.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/barrier.h"
#include "spdk/vhost.h"
#include "vhost_internal.h"

#include "spdk_internal/vhost_user.h"

const struct vhost_device_ops g_dpdk_vhost_ops;

void
unblock_dpdk_thread(int rc)
{
	g_vhost_sock_info.response = rc;
	sem_post(&g_vhost_sock_info.sem);
}

static void
block_dpdk_thread(void)
{
	struct timespec timeout;
	int rc;

	clock_gettime(CLOCK_REALTIME, &timeout);
	timeout.tv_sec += 3;
	rc = sem_timedwait(&g_vhost_sock_info.sem, &timeout);
	if (rc != 0) {
		SPDK_ERRLOG("timeout\n");
		sem_wait(&g_vhost_sock_info.sem);
	}
}

#define SHIFT_2MB	21
#define SIZE_2MB	(1ULL << SHIFT_2MB)
#define FLOOR_2MB(x)	(((uintptr_t)x) / SIZE_2MB) << SHIFT_2MB
#define CEIL_2MB(x)	((((uintptr_t)x) + SIZE_2MB - 1) / SIZE_2MB) << SHIFT_2MB

static void
register_vhost_mem(struct rte_vhost_memory *mem)
{
	struct rte_vhost_mem_region *region;
	uint32_t i;
	uint64_t previous_start = UINT64_MAX;

	for (i = 0; i < mem->nregions; i++) {
		uint64_t start, end, len;
		region = &mem->regions[i];
		start = FLOOR_2MB(region->mmap_addr);
		end = CEIL_2MB(region->mmap_addr + region->mmap_size);
		if (start == previous_start) {
			start += (size_t) SIZE_2MB;
		}
		previous_start = start;
		len = end - start;
		SPDK_INFOLOG(SPDK_LOG_VHOST, "Registering VM memory for vtophys translation - 0x%jx len:0x%jx\n",
			     start, len);

		if (spdk_mem_register((void *)start, len) != 0) {
			SPDK_WARNLOG("Failed to register memory region %"PRIu32". Future vtophys translation might fail.\n",
				     i);
			continue;
		}
	}
}

static void
unregister_vhost_mem(struct rte_vhost_memory *mem)
{
	struct rte_vhost_mem_region *region;
	uint32_t i;
	uint64_t previous_start = UINT64_MAX;

	for (i = 0; i < mem->nregions; i++) {
		uint64_t start, end, len;
		region = &mem->regions[i];
		start = FLOOR_2MB(region->mmap_addr);
		end = CEIL_2MB(region->mmap_addr + region->mmap_size);
		if (start == previous_start) {
			start += (size_t) SIZE_2MB;
		}
		previous_start = start;
		len = end - start;

		if (spdk_vtophys((void *) start, NULL) == SPDK_VTOPHYS_ERROR) {
			continue; /* region has not been registered */
		}

		if (spdk_mem_unregister((void *)start, len) != 0) {
			assert(false);
		}
	}

}

#ifndef SPDK_CONFIG_VHOST_INTERNAL_LIB
static struct rte_vhost_user_extern_ops g_extern_vhost_ops;
#endif

static int
new_connection(int vid)
{
	g_vhost_sock_info.vid = vid;
	if (rte_vhost_get_ifname(vid, g_vhost_sock_info.u.connect.path, PATH_MAX) < 0) {
		SPDK_ERRLOG("rte_vhost_get_ifname() failed for vid = %d\n", vid);
		return -EFAULT;
	}

#ifndef SPDK_CONFIG_VHOST_INTERNAL_LIB
	if (rte_vhost_extern_callback_register(vid, &g_extern_vhost_ops, NULL)) {
		SPDK_ERRLOG("rte_vhost_extern_callback_register() failed for vid = %d\n",
			    vid);
		return -EFAULT;
	}
#endif

	spdk_thread_send_msg(g_vhost_init_thread,
			     g_vhost_sock_ops.new_session, NULL);
	block_dpdk_thread();
	return g_vhost_sock_info.response;
}

static int
start_device(int vid)
{
	uint16_t i;

	g_vhost_sock_info.vid = vid;
	for (i = 0; i < SPDK_VHOST_MAX_VQUEUES; i++) {
		struct vhost_sock_queue_info *q_info = &g_vhost_sock_info.u.start.queue[i];

		if (rte_vhost_get_vhost_vring(vid, i, &q_info->vring)) {
			continue;
		}

		if (rte_vhost_get_vring_base(vid, i,
					     &q_info->last_avail_idx,
					     &q_info->last_used_idx)) {
			q_info->vring.desc = NULL;
		}
	}

	if (rte_vhost_get_negotiated_features(vid, &g_vhost_sock_info.u.start.negotiated_features) != 0) {
		SPDK_ERRLOG("vhost device %d: Failed to get negotiated driver features\n", vid);
		return -1;
	}

	if (rte_vhost_get_mem_table(vid, &g_vhost_sock_info.u.start.mem) != 0) {
		SPDK_ERRLOG("vhost device %d: Failed to get guest memory table\n", vid);
		return -1;
	}

	/* This can be an extremely long, synchronous operation, so do it on this
	 * DPDK thread rather than the SPDK's init thread which could be already
	 * busy with handling other I/Os.
	 */
	register_vhost_mem(g_vhost_sock_info.u.start.mem);

	spdk_thread_send_msg(g_vhost_init_thread,
			     g_vhost_sock_ops.start_session, NULL);
	block_dpdk_thread();

	if (g_vhost_sock_info.response == -EALREADY) {
		return 0;
	} else if (g_vhost_sock_info.response != 0) {
		unregister_vhost_mem(g_vhost_sock_info.u.start.mem);
		free(g_vhost_sock_info.u.start.mem);
	}

	return g_vhost_sock_info.response;
}

static void
stop_device(int vid)
{
	uint16_t i;

	g_vhost_sock_info.vid = vid;
	spdk_thread_send_msg(g_vhost_init_thread,
			     g_vhost_sock_ops.stop_session, NULL);
	block_dpdk_thread();

	if (g_vhost_sock_info.response == -EALREADY) {
		return;
	} else if (g_vhost_sock_info.response != 0) {
		SPDK_ERRLOG("Couldn't stop device with vid %d.\n", vid);
		return;
	}

	for (i = 0; i < SPDK_VHOST_MAX_VQUEUES; i++) {
		struct vhost_sock_queue_info *q_info = &g_vhost_sock_info.u.stop.queue[i];

		if (q_info->vring.desc == NULL) {
			continue;
		}

		rte_vhost_set_vring_base(vid, i,
					 q_info->last_avail_idx,
					 q_info->last_used_idx);
	}

	unregister_vhost_mem(g_vhost_sock_info.u.stop.mem);
	free(g_vhost_sock_info.u.stop.mem);
}

static void
destroy_connection(int vid)
{
	/* we might have forcefully started the session without DPDK knowing
	 * about it, so forcefully stop it now. It'll simply return if the
	 * session is not started.
	 */
	stop_device(vid);

	g_vhost_sock_info.vid = vid;
	spdk_thread_send_msg(g_vhost_init_thread,
			     g_vhost_sock_ops.delete_session, NULL);
	block_dpdk_thread();
}

static int
get_config(int vid, uint8_t *config, uint32_t size)
{
	g_vhost_sock_info.u.config.buf = config;
	g_vhost_sock_info.u.config.size = size;

	spdk_thread_send_msg(g_vhost_init_thread,
			     g_vhost_sock_ops.get_config, NULL);
	block_dpdk_thread();

	return g_vhost_sock_info.response;
}

static int
set_config(int vid, uint8_t *config, uint32_t offset, uint32_t size, uint32_t flags)
{
	g_vhost_sock_info.u.config.buf = config;
	g_vhost_sock_info.u.config.offset = offset;
	g_vhost_sock_info.u.config.size = size;
	g_vhost_sock_info.u.config.flags = flags;

	spdk_thread_send_msg(g_vhost_init_thread,
			     g_vhost_sock_ops.set_config, NULL);
	block_dpdk_thread();

	return g_vhost_sock_info.response;
}

const struct vhost_device_ops g_dpdk_vhost_ops = {
	.new_device =  start_device,
	.destroy_device = stop_device,
	.new_connection = new_connection,
	.destroy_connection = destroy_connection,
#ifdef SPDK_CONFIG_VHOST_INTERNAL_LIB
	.get_config = get_config,
	.set_config = set_config,
	.vhost_nvme_admin_passthrough = vhost_nvme_admin_passthrough,
	.vhost_nvme_set_cq_call = vhost_nvme_set_cq_call,
	.vhost_nvme_get_cap = vhost_nvme_get_cap,
	.vhost_nvme_set_bar_mr = vhost_nvme_set_bar_mr,
#endif
};

#ifndef SPDK_CONFIG_VHOST_INTERNAL_LIB
static enum rte_vhost_msg_result
spdk_extern_vhost_pre_msg_handler(int vid, void *_msg)
{
	struct vhost_user_msg *msg = _msg;
	struct spdk_vhost_session *vsession;

	vsession = vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Received a message to unitialized session (vid %d).\n", vid);
		assert(false);
		return RTE_VHOST_MSG_RESULT_ERR;
	}

	switch (msg->request) {
	case VHOST_USER_GET_VRING_BASE:
		if (vsession->forced_polling && vsession->started) {
			/* Our queue is stopped for whatever reason, but we may still
			 * need to poll it after it's initialized again.
			 */
			g_dpdk_vhost_ops.destroy_device(vid);
		}
		break;
	case VHOST_USER_SET_VRING_BASE:
	case VHOST_USER_SET_VRING_ADDR:
	case VHOST_USER_SET_VRING_NUM:
	case VHOST_USER_SET_VRING_KICK:
		if (vsession->forced_polling && vsession->started) {
			/* Additional queues are being initialized, so we either processed
			 * enough I/Os and are switching from SeaBIOS to the OS now, or
			 * we were never in SeaBIOS in the first place. Either way, we
			 * don't need our workaround anymore.
			 */
			g_dpdk_vhost_ops.destroy_device(vid);
			vsession->forced_polling = false;
		}
		break;
	case VHOST_USER_SET_VRING_CALL:
		/* rte_vhost will close the previous callfd and won't notify
		 * us about any change. This will effectively make SPDK fail
		 * to deliver any subsequent interrupts until a session is
		 * restarted. We stop the session here before closing the previous
		 * fd (so that all interrupts must have been delivered by the
		 * time the descriptor is closed) and start right after (which
		 * will make SPDK retrieve the latest, up-to-date callfd from
		 * rte_vhost.
		 */
	case VHOST_USER_SET_MEM_TABLE:
		/* rte_vhost will unmap previous memory that SPDK may still
		 * have pending DMA operations on. We can't let that happen,
		 * so stop the device before letting rte_vhost unmap anything.
		 * This will block until all pending I/Os are finished.
		 * We will start the device again from the post-processing
		 * message handler.
		 */
		if (vsession->started) {
			g_dpdk_vhost_ops.destroy_device(vid);
			vsession->needs_restart = true;
		}
		break;
	case VHOST_USER_GET_CONFIG: {
		int rc = 0;

		rc = get_config(vid, msg->payload.cfg.region, msg->payload.cfg.size);
		if (rc != 0) {
			msg->size = 0;
		}

		return RTE_VHOST_MSG_RESULT_REPLY;
	}
	case VHOST_USER_SET_CONFIG: {
		int rc = 0;

		rc = set_config(vid, msg->payload.cfg.region,
				msg->payload.cfg.offset,
				msg->payload.cfg.size,
				msg->payload.cfg.flags);
		return rc == 0 ? RTE_VHOST_MSG_RESULT_OK : RTE_VHOST_MSG_RESULT_ERR;
	}
	default:
		break;
	}

	return RTE_VHOST_MSG_RESULT_NOT_HANDLED;
}

static enum rte_vhost_msg_result
spdk_extern_vhost_post_msg_handler(int vid, void *_msg)
{
	struct vhost_user_msg *msg = _msg;
	struct spdk_vhost_session *vsession;

	vsession = vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Received a message to unitialized session (vid %d).\n", vid);
		assert(false);
		return RTE_VHOST_MSG_RESULT_ERR;
	}

	if (vsession->needs_restart) {
		g_dpdk_vhost_ops.new_device(vid);
		vsession->needs_restart = false;
		return RTE_VHOST_MSG_RESULT_NOT_HANDLED;
	}

	switch (msg->request) {
	case VHOST_USER_SET_FEATURES:
		/* rte_vhost requires all queues to be fully initialized in order
		 * to start I/O processing. This behavior is not compliant with the
		 * vhost-user specification and doesn't work with QEMU 2.12+, which
		 * will only initialize 1 I/O queue for the SeaBIOS boot.
		 * Theoretically, we should start polling each virtqueue individually
		 * after receiving its SET_VRING_KICK message, but rte_vhost is not
		 * designed to poll individual queues. So here we use a workaround
		 * to detect when the vhost session could be potentially at that SeaBIOS
		 * stage and we mark it to start polling as soon as its first virtqueue
		 * gets initialized. This doesn't hurt any non-QEMU vhost slaves
		 * and allows QEMU 2.12+ to boot correctly. SET_FEATURES could be sent
		 * at any time, but QEMU will send it at least once on SeaBIOS
		 * initialization - whenever powered-up or rebooted.
		 */
		vsession->forced_polling = true;
		break;
	case VHOST_USER_SET_VRING_KICK:
		/* vhost-user spec tells us to start polling a queue after receiving
		 * its SET_VRING_KICK message. Let's do it!
		 */
		if (vsession->forced_polling && !vsession->started) {
			g_dpdk_vhost_ops.new_device(vid);
		}
		break;
	default:
		break;
	}

	return RTE_VHOST_MSG_RESULT_NOT_HANDLED;
}

static struct rte_vhost_user_extern_ops g_extern_vhost_ops = {
	.pre_msg_handle = spdk_extern_vhost_pre_msg_handler,
	.post_msg_handle = spdk_extern_vhost_post_msg_handler,
};

#endif

int
vhost_register_unix_socket(const char *path, const char *ctrl_name,
			   uint64_t virtio_features, uint64_t disabled_features)
{
	struct stat file_stat;
#ifndef SPDK_CONFIG_VHOST_INTERNAL_LIB
	uint64_t protocol_features = 0;
#endif

	/* Register vhost driver to handle vhost messages. */
	if (stat(path, &file_stat) != -1) {
		if (!S_ISSOCK(file_stat.st_mode)) {
			SPDK_ERRLOG("Cannot create a domain socket at path \"%s\": "
				    "The file already exists and is not a socket.\n",
				    path);
			return -EIO;
		} else if (unlink(path) != 0) {
			SPDK_ERRLOG("Cannot create a domain socket at path \"%s\": "
				    "The socket already exists and failed to unlink.\n",
				    path);
			return -EIO;
		}
	}


	if (rte_vhost_driver_register(path, 0) != 0) {
		SPDK_ERRLOG("Could not register controller %s with vhost library\n", ctrl_name);
		SPDK_ERRLOG("Check if domain socket %s already exists\n", path);
		return -EIO;
	}

	if (rte_vhost_driver_set_features(path, virtio_features) ||
	    rte_vhost_driver_disable_features(path, disabled_features)) {
		SPDK_ERRLOG("Couldn't set vhost features for controller %s\n", ctrl_name);
		rte_vhost_driver_unregister(path);
		return -EIO;
	}

	if (rte_vhost_driver_callback_register(path, &g_dpdk_vhost_ops) != 0) {
		rte_vhost_driver_unregister(path);
		SPDK_ERRLOG("Couldn't register callbacks for controller %s\n", ctrl_name);
		return -EIO;
	}

#ifndef SPDK_CONFIG_VHOST_INTERNAL_LIB
	rte_vhost_driver_get_protocol_features(path, &protocol_features);
	protocol_features |= (1ULL << VHOST_USER_PROTOCOL_F_CONFIG);
	rte_vhost_driver_set_protocol_features(path, protocol_features);
#endif

	if (rte_vhost_driver_start(path) != 0) {
		SPDK_ERRLOG("Failed to start vhost driver for controller %s (%d): %s\n",
			    ctrl_name, errno, spdk_strerror(errno));
		rte_vhost_driver_unregister(path);
		return -EIO;
	}

	return 0;
}
