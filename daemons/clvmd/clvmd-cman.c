/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * CMAN communication layer for clvmd.
 */

#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>

#include "clvmd-comms.h"
#include "clvm.h"
#include "libdlm.h"
#include "log.h"
#include "clvmd.h"
#include "lvm-functions.h"

#define LOCKSPACE_NAME "clvmd"

static int num_nodes;
static struct cman_node *nodes = NULL;
static struct cman_node this_node;
static int count_nodes; /* size of allocated nodes array */
static int max_updown_nodes = 50;	/* Current size of the allocated array */
/* Node up/down status, indexed by nodeid */
static int *node_updown = NULL;
static dlm_lshandle_t *lockspace;
static cman_handle_t c_handle;

static void count_clvmds_running(void);
static void get_members(void);
static int nodeid_from_csid(char *csid);
static int name_from_nodeid(int nodeid, char *name);
static void event_callback(cman_handle_t handle, void *private, int reason, int arg);
static void data_callback(cman_handle_t handle, void *private,
			  char *buf, int len, uint8_t port, int nodeid);

struct lock_wait {
	pthread_cond_t cond;
	pthread_mutex_t mutex;
	struct dlm_lksb lksb;
};

static int _init_cluster(void)
{
	/* Open the cluster communication socket */
	c_handle = cman_init(NULL);
	if (!c_handle) {
		syslog(LOG_ERR, "Can't open cluster manager socket: %m");
		return -1;
	}

	if (cman_start_recv_data(c_handle, data_callback, CLUSTER_PORT_CLVMD)) {
		syslog(LOG_ERR, "Can't bind cluster socket: %m");
		return -1;
	}

	if (cman_start_notification(c_handle, event_callback)) {
		syslog(LOG_ERR, "Can't start cluster event listening");
		return -1;
	}

	/* Get the cluster members list */
	get_members();
	count_clvmds_running();

	/* Create a lockspace for LV & VG locks to live in */
	lockspace = dlm_create_lockspace(LOCKSPACE_NAME, 0600);
	if (!lockspace) {
		syslog(LOG_ERR, "Unable to create lockspace for CLVM: %m");
		return -1;
	}
	dlm_ls_pthread_init(lockspace);

	return 0;
}

static void _cluster_init_completed(void)
{
	clvmd_cluster_init_completed();
}

static int _get_main_cluster_fd()
{
	return cman_get_fd(c_handle);
}

static int _get_num_nodes()
{
	int i;
	int nnodes = 0;

	/* return number of ACTIVE nodes */
	for (i=0; i<num_nodes; i++) {
		if (nodes[i].cn_member && nodes[i].cn_nodeid)
			nnodes++;
	}
	return nnodes;
}

/* send_message with the fd check removed */
static int _cluster_send_message(void *buf, int msglen, char *csid, const char *errtext)
{
	int nodeid = 0;

	if (csid)
		memcpy(&nodeid, csid, CMAN_MAX_CSID_LEN);

	if (cman_send_data(c_handle, buf, msglen, 0, CLUSTER_PORT_CLVMD, nodeid) <= 0)
	{
		log_error(errtext);
	}
	return msglen;
}

static void _get_our_csid(char *csid)
{
	if (this_node.cn_nodeid == 0) {
		cman_get_node(c_handle, 0, &this_node);
	}
	memcpy(csid, &this_node.cn_nodeid, CMAN_MAX_CSID_LEN);
}

/* Call a callback routine for each node is that known (down means not running a clvmd) */
static int _cluster_do_node_callback(struct local_client *client,
				     void (*callback) (struct local_client *, char *,
						       int))
{
	int i;
	int somedown = 0;

	for (i = 0; i < _get_num_nodes(); i++) {
		if (nodes[i].cn_member && nodes[i].cn_nodeid) {
			callback(client, (char *)&nodes[i].cn_nodeid, node_updown[nodes[i].cn_nodeid]);
			if (!node_updown[nodes[i].cn_nodeid])
				somedown = -1;
		}
	}
	return somedown;
}

/* Process OOB messages from the cluster socket */
static void event_callback(cman_handle_t handle, void *private, int reason, int arg)
{
	char namebuf[MAX_CLUSTER_MEMBER_NAME_LEN];

	switch (reason) {
        case CMAN_REASON_PORTCLOSED:
		name_from_nodeid(arg, namebuf);
		log_notice("clvmd on node %s has died\n", namebuf);
		DEBUGLOG("Got port closed message, removing node %s\n", namebuf);

		node_updown[arg] = 0;
		break;

	case CMAN_REASON_STATECHANGE:
		DEBUGLOG("Got state change message, re-reading members list\n");
		get_members();
		break;

#if defined(LIBCMAN_VERSION) && LIBCMAN_VERSION >= 2
	case CMAN_REASON_PORTOPENED:
		/* Ignore this, wait for startup message from clvmd itself */
		break;

	case CMAN_REASON_TRY_SHUTDOWN:
		DEBUGLOG("Got try shutdown, sending OK\n");
		cman_replyto_shutdown(c_handle, 1);
		break;
#endif
	default:
		/* ERROR */
		DEBUGLOG("Got unknown event callback message: %d\n", reason);
		break;
	}
}

static struct local_client *cman_client;
static int _cluster_fd_callback(struct local_client *fd, char *buf, int len, char *csid,
				struct local_client **new_client)
{

	/* Save this for data_callback */
	cman_client = fd;

	/* We never return a new client */
	*new_client = NULL;

	return cman_dispatch(c_handle, 0);
}


static void data_callback(cman_handle_t handle, void *private,
			  char *buf, int len, uint8_t port, int nodeid)
{
	/* Ignore looped back messages */
	if (nodeid == this_node.cn_nodeid)
		return;
	process_message(cman_client, buf, len, (char *)&nodeid);
}

static void _add_up_node(char *csid)
{
	/* It's up ! */
	int nodeid = nodeid_from_csid(csid);

	if (nodeid >= max_updown_nodes) {
	        int new_size = nodeid + 10;
		int *new_updown = realloc(node_updown, new_size);

		if (new_updown) {
			node_updown = new_updown;
			max_updown_nodes = new_size;
			DEBUGLOG("realloced more space for nodes. now %d\n",
				 max_updown_nodes);
		} else {
			log_error
				("Realloc failed. Node status for clvmd will be wrong. quitting\n");
			exit(999);
		}
	}
	node_updown[nodeid] = 1;
	DEBUGLOG("Added new node %d to updown list\n", nodeid);
}

static void _cluster_closedown()
{
	unlock_all();
	dlm_release_lockspace(LOCKSPACE_NAME, lockspace, 1);
	cman_finish(c_handle);
}

static int is_listening(int nodeid)
{
	int status;

	do {
		status = cman_is_listening(c_handle, nodeid, CLUSTER_PORT_CLVMD);
		if (status < 0 && errno == EBUSY) {	/* Don't busywait */
			sleep(1);
			errno = EBUSY;	/* In case sleep trashes it */
		}
	}
	while (status < 0 && errno == EBUSY);

	return status;
}

/* Populate the list of CLVMDs running.
   called only at startup time */
static void count_clvmds_running(void)
{
	int i;

	for (i = 0; i < num_nodes; i++) {
		node_updown[nodes[i].cn_nodeid] = is_listening(nodes[i].cn_nodeid);
	}
}

/* Get a list of active cluster members */
static void get_members()
{
	int retnodes;
	int status;

	num_nodes = cman_get_node_count(c_handle);
	if (num_nodes == -1) {
		log_error("Unable to get node count");
		return;
	}

	/* Not enough room for new nodes list ? */
	if (num_nodes > count_nodes && nodes) {
		free(nodes);
		nodes = NULL;
	}

	if (nodes == NULL) {
		count_nodes = num_nodes + 10; /* Overallocate a little */
		nodes = malloc(count_nodes * sizeof(struct cman_node));
		if (!nodes) {
			log_error("Unable to allocate nodes array\n");
			exit(5);
		}
	}

	status = cman_get_nodes(c_handle, count_nodes, &retnodes, nodes);
	if (status < 0) {
		log_error("Unable to get node details");
		exit(6);
	}

	if (node_updown == NULL) {
		node_updown =
			(int *) malloc(sizeof(int) *
				       max(num_nodes, max_updown_nodes));
		memset(node_updown, 0,
		       sizeof(int) * max(num_nodes, max_updown_nodes));
	}
}


/* Convert a node name to a CSID */
static int _csid_from_name(char *csid, char *name)
{
	int i;

	for (i = 0; i < num_nodes; i++) {
		if (strcmp(name, nodes[i].cn_name) == 0) {
			memcpy(csid, &nodes[i].cn_nodeid, CMAN_MAX_CSID_LEN);
			return 0;
		}
	}
	return -1;
}

/* Convert a CSID to a node name */
static int _name_from_csid(char *csid, char *name)
{
	int i;

	for (i = 0; i < num_nodes; i++) {
		if (memcmp(csid, &nodes[i].cn_nodeid, CMAN_MAX_CSID_LEN) == 0) {
			strcpy(name, nodes[i].cn_name);
			return 0;
		}
	}
	/* Who?? */
	strcpy(name, "Unknown");
	return -1;
}

/* Convert a node ID to a node name */
static int name_from_nodeid(int nodeid, char *name)
{
	int i;

	for (i = 0; i < num_nodes; i++) {
		if (nodeid == nodes[i].cn_nodeid) {
			strcpy(name, nodes[i].cn_name);
			return 0;
		}
	}
	/* Who?? */
	strcpy(name, "Unknown");
	return -1;
}

/* Convert a CSID to a node ID */
static int nodeid_from_csid(char *csid)
{
        int nodeid;

	memcpy(&nodeid, csid, CMAN_MAX_CSID_LEN);

	return nodeid;
}

static int _is_quorate()
{
	return cman_is_quorate(c_handle);
}

static void sync_ast_routine(void *arg)
{
	struct lock_wait *lwait = arg;

	pthread_mutex_lock(&lwait->mutex);
	pthread_cond_signal(&lwait->cond);
	pthread_mutex_unlock(&lwait->mutex);
}

static int _sync_lock(const char *resource, int mode, int flags, int *lockid)
{
	int status;
	struct lock_wait lwait;

	if (!lockid) {
		errno = EINVAL;
		return -1;
	}

	DEBUGLOG("sync_lock: '%s' mode:%d flags=%d\n", resource,mode,flags);
	/* Conversions need the lockid in the LKSB */
	if (flags & LKF_CONVERT)
		lwait.lksb.sb_lkid = *lockid;

	pthread_cond_init(&lwait.cond, NULL);
	pthread_mutex_init(&lwait.mutex, NULL);
	pthread_mutex_lock(&lwait.mutex);

	status = dlm_ls_lock(lockspace,
			     mode,
			     &lwait.lksb,
			     flags,
			     resource,
			     strlen(resource),
			     0, sync_ast_routine, &lwait, NULL, NULL);
	if (status)
		return status;

	/* Wait for it to complete */
	pthread_cond_wait(&lwait.cond, &lwait.mutex);
	pthread_mutex_unlock(&lwait.mutex);

	*lockid = lwait.lksb.sb_lkid;

	errno = lwait.lksb.sb_status;
	DEBUGLOG("sync_lock: returning lkid %x\n", *lockid);
	if (lwait.lksb.sb_status)
		return -1;
	else
		return 0;
}

static int _sync_unlock(const char *resource /* UNUSED */, int lockid)
{
	int status;
	struct lock_wait lwait;

	DEBUGLOG("sync_unlock: '%s' lkid:%x\n", resource, lockid);

	pthread_cond_init(&lwait.cond, NULL);
	pthread_mutex_init(&lwait.mutex, NULL);
	pthread_mutex_lock(&lwait.mutex);

	status = dlm_ls_unlock(lockspace, lockid, 0, &lwait.lksb, &lwait);

	if (status)
		return status;

	/* Wait for it to complete */
	pthread_cond_wait(&lwait.cond, &lwait.mutex);
	pthread_mutex_unlock(&lwait.mutex);

	errno = lwait.lksb.sb_status;
	if (lwait.lksb.sb_status != EUNLOCK)
		return -1;
	else
		return 0;

}

static int _get_cluster_name(char *buf, int buflen)
{
	cman_cluster_t cluster_info;
	int status;

	status = cman_get_cluster(c_handle, &cluster_info);
	if (!status) {
		strncpy(buf, cluster_info.ci_name, buflen);
	}
	return status;
}

static struct cluster_ops _cluster_cman_ops = {
	.cluster_init_completed   = _cluster_init_completed,
	.cluster_send_message     = _cluster_send_message,
	.name_from_csid           = _name_from_csid,
	.csid_from_name           = _csid_from_name,
	.get_num_nodes            = _get_num_nodes,
	.cluster_fd_callback      = _cluster_fd_callback,
	.get_main_cluster_fd      = _get_main_cluster_fd,
	.cluster_do_node_callback = _cluster_do_node_callback,
	.is_quorate               = _is_quorate,
	.get_our_csid             = _get_our_csid,
	.add_up_node              = _add_up_node,
	.cluster_closedown        = _cluster_closedown,
	.get_cluster_name         = _get_cluster_name,
	.sync_lock                = _sync_lock,
	.sync_unlock              = _sync_unlock,
};

struct cluster_ops *init_cman_cluster(void)
{
	if (!_init_cluster())
		return &_cluster_cman_ops;
	else
		return NULL;
}
