/*
 * The Shadow Simulator
 *
 * Copyright (c) 2010-2011 Rob Jansen <jansen@cs.umn.edu>
 *
 * This file is part of Shadow.
 *
 * Shadow is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Shadow is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Shadow.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "shadow.h"

struct _Node {
	/* asynchronous event priority queue. other nodes may push to this queue. */
	GAsyncQueue* event_mailbox;

	/* the network this node belongs to */
	Network* network;

	/* general node lock. nothing that belongs to the node should be touched
	 * unless holding this lock. everything following this falls under the lock.
	 */
	GMutex* lock;

	/* a simple priority queue holding events currently being executed.
	 * events are place in this queue before handing the node off to a
	 * worker and should not be modified by other nodes. */
	GQueue* event_priority_queue;

	GQuark id;
	gchar* name;
	GHashTable* interfaces;
	NetworkInterface* defaultInterface;
	CPU* cpu;

	Application* application;

	/* all file, socket, and epoll descriptors we know about and track */
	GHashTable* descriptors;
	gint descriptorHandleCounter;
	in_port_t randomPortCounter;

	MAGIC_DECLARE;
};

Node* node_new(GQuark id, Network* network, Software* software, guint32 ip, GString* hostname, guint32 bwDownKiBps, guint32 bwUpKiBps, guint64 cpuBps) {
	Node* node = g_new0(Node, 1);
	MAGIC_INIT(node);

	node->id = id;
	node->name = g_strdup(hostname->str);
	node->lock = g_mutex_new();

	/* thread-level event communication with other nodes */
	node->event_mailbox = g_async_queue_new_full(shadowevent_free);
	node->event_priority_queue = g_queue_new();

	/* where we are in the network topology */
	node->network = network;

	/* virtual interfaces for managing network I/O */
	node->interfaces = g_hash_table_new_full(g_direct_hash, g_direct_equal,
			NULL, (GDestroyNotify) networkinterface_free);
	NetworkInterface* ethernet = networkinterface_new(network, id, hostname->str, bwDownKiBps, bwUpKiBps);
	g_hash_table_replace(node->interfaces, GUINT_TO_POINTER((guint)id), ethernet);
	NetworkInterface* loopback = networkinterface_new(NULL, (GQuark)htonl(INADDR_LOOPBACK), "loopback", G_MAXUINT32, G_MAXUINT32);
	g_hash_table_replace(node->interfaces, GUINT_TO_POINTER((guint)id), loopback);
	node->defaultInterface = ethernet;

	/* virtual descriptor management */
	node->descriptors = g_hash_table_new_full(g_int_hash, g_int_equal, NULL, descriptor_unref);
	node->descriptorHandleCounter = MIN_DESCRIPTOR;
	node->randomPortCounter = MIN_RANDOM_PORT;

	/* applications this node will run */
	node->application = application_new(software);
	node->cpu = cpu_new(cpuBps);

	info("Created Node '%s', ip %s, %u bwUpKiBps, %u bwDownKiBps, %lu cpuBps",
			g_quark_to_string(node->id), networkinterface_getIPName(node->defaultInterface),
			bwUpKiBps, bwDownKiBps, cpuBps);

	return node;
}

void node_free(gpointer data) {
	Node* node = data;
	MAGIC_ASSERT(node);

	/* this was hopefully freed in node_stopApplication */
	if(node->application) {
		node_stopApplication(NULL, node, NULL);
	}

	g_hash_table_destroy(node->interfaces);

	g_async_queue_unref(node->event_mailbox);
	g_queue_free(node->event_priority_queue);

	g_free(node->name);

	g_mutex_free(node->lock);

	MAGIC_CLEAR(node);
	g_free(node);
}

void node_lock(Node* node) {
	MAGIC_ASSERT(node);
	g_mutex_lock(node->lock);
}

void node_unlock(Node* node) {
	MAGIC_ASSERT(node);
	g_mutex_unlock(node->lock);
}

void node_pushMail(Node* node, Event* event) {
	MAGIC_ASSERT(node);
	MAGIC_ASSERT(event);

	g_async_queue_push_sorted(node->event_mailbox, event, shadowevent_compare, NULL);
}

Event* node_popMail(Node* node) {
	MAGIC_ASSERT(node);
	return g_async_queue_try_pop(node->event_mailbox);
}

void node_pushTask(Node* node, Event* event) {
	MAGIC_ASSERT(node);
	MAGIC_ASSERT(event);

	g_queue_insert_sorted(node->event_priority_queue, event, shadowevent_compare, NULL);
}

Event* node_popTask(Node* node) {
	MAGIC_ASSERT(node);
	return g_queue_pop_head(node->event_priority_queue);
}

guint node_getNumTasks(Node* node) {
	MAGIC_ASSERT(node);
	return g_queue_get_length(node->event_priority_queue);
}

void node_startApplication(Node* node) {
	MAGIC_ASSERT(node);
	application_boot(node->application);
}

void node_stopApplication(gpointer key, gpointer value, gpointer user_data) {
	Node* node = value;
	MAGIC_ASSERT(node);

	Worker* worker = worker_getPrivate();
	worker->cached_node = node;

	application_free(node->application);
	node->application = NULL;

	worker->cached_node = NULL;
}

gint node_compare(gconstpointer a, gconstpointer b, gpointer user_data) {
	const Node* na = a;
	const Node* nb = b;
	MAGIC_ASSERT(na);
	MAGIC_ASSERT(nb);
	return na->id > nb->id ? +1 : na->id == nb->id ? 0 : -1;
}

gboolean node_isEqual(Node* a, Node* b) {
	if(a == NULL && b == NULL) {
		return TRUE;
	} else if(a == NULL || b == NULL) {
		return FALSE;
	} else {
		return node_compare(a, b, NULL) == 0;
	}
}

CPU* node_getCPU(Node* node) {
	MAGIC_ASSERT(node);
	return node->cpu;
}

Network* node_getNetwork(Node* node) {
	MAGIC_ASSERT(node);
	return node->network;
}

gchar* node_getName(Node* node) {
	MAGIC_ASSERT(node);
	return node->name;
}

in_addr_t node_getDefaultIP(Node* node) {
	MAGIC_ASSERT(node);
	return networkinterface_getIPAddress(node->defaultInterface);
}

Application* node_getApplication(Node* node) {
	MAGIC_ASSERT(node);
	return node->application;
}

Descriptor* node_lookupDescriptor(Node* node, gint handle) {
	MAGIC_ASSERT(node);
	return g_hash_table_lookup(node->descriptors, (gconstpointer) &handle);
}

NetworkInterface* node_lookupInterface(Node* node, in_addr_t handle) {
	MAGIC_ASSERT(node);
	return g_hash_table_lookup(node->interfaces, GUINT_TO_POINTER(handle));
}

static gint _node_monitorDescriptor(Node* node, Descriptor* descriptor) {
	MAGIC_ASSERT(node);

	/* make sure there are no collisions before inserting */
	gint* handle = descriptor_getHandleReference(descriptor);
	g_assert(handle && node_lookupDescriptor(node, *handle));
	g_hash_table_replace(node->descriptors, handle, descriptor);

	return *handle;
}

gint node_createDescriptor(Node* node, enum DescriptorType type) {
	MAGIC_ASSERT(node);

	/* get a unique descriptor that can be "closed" later */
	Descriptor* descriptor;

	switch(type) {
		case DT_EPOLL: {
			descriptor = (Descriptor*) epoll_new((node->descriptorHandleCounter)++);
			break;
		}

		case DT_TCPSOCKET: {
			descriptor = (Descriptor*) tcp_new((node->descriptorHandleCounter)++);
			break;
		}

		case DT_UDPSOCKET: {
			descriptor = (Descriptor*) udp_new((node->descriptorHandleCounter)++);
			break;
		}

		default: {
			warning("unknown descriptor type: %i", (int)type);
			return EINVAL;
		}
	}

	return _node_monitorDescriptor(node, descriptor);
}

gint node_epollControl(Node* node, gint epollDescriptor, gint operation,
		gint fileDescriptor, struct epoll_event* event) {
	MAGIC_ASSERT(node);

	/* EBADF  epfd is not a valid file descriptor. */
	Descriptor* descriptor = node_lookupDescriptor(node, epollDescriptor);
	if(descriptor == NULL) {
		return EBADF;
	}

	/* EINVAL epfd is not an epoll file descriptor */
	if(descriptor_getType(descriptor) != DT_EPOLL) {
		return EINVAL;
	}

	/* now we know its an epoll */
	Epoll* epoll = (Epoll*) descriptor;

	/* EBADF  fd is not a valid file descriptor. */
	descriptor = node_lookupDescriptor(node, fileDescriptor);
	if(descriptor == NULL) {
		return EBADF;
	}

	return epoll_control(epoll, operation, descriptor, event);

}

gint node_epollGetEvents(Node* node, gint handle,
		struct epoll_event* eventArray, gint eventArrayLength, gint* nEvents) {
	MAGIC_ASSERT(node);

	/* EBADF  epfd is not a valid file descriptor. */
	Descriptor* descriptor = node_lookupDescriptor(node, handle);
	if(descriptor == NULL) {
		return EBADF;
	}

	/* EINVAL epfd is not an epoll file descriptor */
	if(descriptor_getType(descriptor) != DT_EPOLL) {
		return EINVAL;
	}

	Epoll* epoll = (Epoll*) descriptor;
	return epoll_getEvents(epoll, eventArray, eventArrayLength, nEvents);
}

static gboolean _node_doesInterfaceExist(Node* node, in_addr_t interfaceIP) {
	MAGIC_ASSERT(node);

	if(interfaceIP == htonl(INADDR_ANY) && node->defaultInterface) {
		return TRUE;
	}

	NetworkInterface* interface = node_lookupInterface(node, interfaceIP);
	if(interface) {
		return TRUE;
	}

	return FALSE;
}

static gboolean _node_isInterfaceAvailable(Node* node, in_addr_t interfaceIP,
		enum DescriptorType type, in_port_t port) {
	MAGIC_ASSERT(node);

	enum ProtocolType protocol = type == DT_TCPSOCKET ? PTCP : type == DT_UDPSOCKET ? PUDP : PLOCAL;
	gint associationKey = PROTOCOL_DEMUX_KEY(protocol, port);
	gboolean isAvailable = FALSE;

	if(interfaceIP == htonl(INADDR_ANY)) {
		/* need to check that all interfaces are free */
		GHashTableIter iter;
		gpointer key, value;
		g_hash_table_iter_init(&iter, node->interfaces);

		while(g_hash_table_iter_next(&iter, &key, &value)) {
			NetworkInterface* interface = value;
			isAvailable = networkinterface_isAssociated(interface, associationKey);

			/* as soon as one is taken, break out to return FALSE */
			if(!isAvailable) {
				break;
			}
		}
	} else {
		NetworkInterface* interface = node_lookupInterface(node, interfaceIP);
		isAvailable = networkinterface_isAssociated(interface, associationKey);
	}

	return isAvailable;
}


static in_port_t _node_getRandomFreePort(Node* node, in_addr_t interfaceIP,
		enum DescriptorType type) {
	MAGIC_ASSERT(node);

	in_port_t randomPort = 0;
	gboolean available = FALSE;

	while(!available) {
		randomPort = (node->randomPortCounter)++;
		g_assert(randomPort >= MIN_RANDOM_PORT);
		available = _node_isInterfaceAvailable(node, interfaceIP, type, randomPort);
	}

	return randomPort;
}

static void _node_associateInterface(Node* node, Transport* transport,
		in_addr_t bindAddress, in_port_t bindPort) {
	MAGIC_ASSERT(node);

	/* connect up transport layer */
	transport_setBinding(transport, bindAddress, bindPort);

	/* now associate the interfaces corresponding to bindAddress with transport */
	if(bindAddress == htonl(INADDR_ANY)) {
		/* need to associate all interfaces */
		GHashTableIter iter;
		gpointer key, value;
		g_hash_table_iter_init(&iter, node->interfaces);

		while(g_hash_table_iter_next(&iter, &key, &value)) {
			NetworkInterface* interface = value;
			networkinterface_associate(interface, transport);
		}
	} else {
		NetworkInterface* interface = node_lookupInterface(node, bindAddress);
		networkinterface_associate(interface, transport);
	}
}

gint node_bindToInterface(Node* node, gint handle, in_addr_t bindAddress, in_port_t bindPort) {
	MAGIC_ASSERT(node);

	Descriptor* descriptor = node_lookupDescriptor(node, handle);
	if(descriptor == NULL) {
		warning("descriptor handle '%i' not found", handle);
		return EBADF;
	}

	enum DescriptorType type = descriptor_getType(descriptor);
	if(type != DT_TCPSOCKET && type != DT_UDPSOCKET) {
		warning("wrong type for descriptor handle '%i'", handle);
		return ENOTSOCK;
	}

	/* make sure we have an interface at that address */
	if(!_node_doesInterfaceExist(node, bindAddress)) {
		return EADDRNOTAVAIL;
	}

	Transport* transport = (Transport*) descriptor;

	/* make sure socket is not bound */
	if(transport_isBound(transport)) {
		warning("socket already bound to requested address");
		return EINVAL;
	}

	/* make sure we have a proper port */
	if(bindPort == 0) {
		/* we know it will be available */
		bindPort = _node_getRandomFreePort(node, bindAddress, type);
	} else {
		/* make sure their port is available at that address for this protocol. */
		if(!_node_isInterfaceAvailable(node, bindAddress, type, bindPort)) {
			return EADDRINUSE;
		}
	}

	/* bind port and set associations */
	_node_associateInterface(node, transport, bindAddress, bindPort);

	return 0;
}

gint node_connectToPeer(Node* node, gint handle, in_addr_t peerAddress,
		in_port_t peerPort, sa_family_t family) {
	MAGIC_ASSERT(node);

	Descriptor* descriptor = node_lookupDescriptor(node, handle);
	if(descriptor == NULL) {
		warning("descriptor handle '%i' not found", handle);
		return EBADF;
	}

	enum DescriptorType type = descriptor_getType(descriptor);
	if(type != DT_TCPSOCKET && type != DT_UDPSOCKET) {
		warning("wrong type for descriptor handle '%i'", handle);
		return ENOTSOCK;
	}

	Transport* transport = (Transport*) descriptor;
	Socket* socket = (Socket*) descriptor;

	if(!socket_isFamilySupported(socket, family)) {
		return EAFNOSUPPORT;
	}

	if(type == DT_TCPSOCKET) {
		gint error = tcp_getConnectError((TCP*)socket);
		if(error) {
			return error;
		}
	}

	if(!transport_isBound(transport)) {
		/* do an implicit bind to a random port.
		 * use default interface unless the remote peer is on loopback */
		in_addr_t loIP = htonl(INADDR_LOOPBACK);
		in_addr_t defaultIP = networkinterface_getIPAddress(node->defaultInterface);

		in_addr_t bindAddress = loIP == peerAddress ? loIP : defaultIP;
		in_port_t bindPort = _node_getRandomFreePort(node, bindAddress, type);

		_node_associateInterface(node, transport, bindAddress, bindPort);
	}

	return socket_connectToPeer(socket, peerAddress, peerPort, family);
}

gint node_listenForPeer(Node* node, gint handle, gint backlog) {
	MAGIC_ASSERT(node);

	Descriptor* descriptor = node_lookupDescriptor(node, handle);
	if(descriptor == NULL) {
		warning("descriptor handle '%i' not found", handle);
		return EBADF;
	}

	enum DescriptorType type = descriptor_getType(descriptor);
	if(type != DT_TCPSOCKET) {
		warning("wrong type for descriptor handle '%i'", handle);
		return EOPNOTSUPP;
	}

	Transport* transport = (Transport*) descriptor;
	TCP* tcp = (TCP*) descriptor;

	if(!transport_isBound(transport)) {
		/* implicit bind */
		in_addr_t bindAddress = htonl(INADDR_ANY);
		in_port_t bindPort = _node_getRandomFreePort(node, bindAddress, type);

		_node_associateInterface(node, transport, bindAddress, bindPort);
	}

	tcp_enterServerMode(tcp, backlog);
	return 0;
}

gint node_acceptNewPeer(Node* node, gint handle, in_addr_t* ip, in_port_t* port) {
	MAGIC_ASSERT(node);

	Descriptor* descriptor = node_lookupDescriptor(node, handle);
	if(descriptor == NULL) {
		warning("descriptor handle '%i' not found", handle);
		return EBADF;
	}

	enum DescriptorType type = descriptor_getType(descriptor);
	if(type != DT_TCPSOCKET) {
		return EOPNOTSUPP;
	}

	return tcp_acceptServerPeer((TCP*)descriptor, ip, port);
}

gint node_getPeerName(Node* node, gint handle, in_addr_t* ip, in_port_t* port) {
	MAGIC_ASSERT(node);

	Descriptor* descriptor = node_lookupDescriptor(node, handle);
	if(descriptor == NULL) {
		warning("descriptor handle '%i' not found", handle);
		return EBADF;
	}

	enum DescriptorType type = descriptor_getType(descriptor);
	if(type != DT_TCPSOCKET) {
		return ENOTCONN;
	}

	return socket_getPeerName((Socket*)descriptor, ip, port);
}

gint node_getSocketName(Node* node, gint handle, in_addr_t* ip, in_port_t* port) {
	MAGIC_ASSERT(node);

	Descriptor* descriptor = node_lookupDescriptor(node, handle);
	if(descriptor == NULL) {
		warning("descriptor handle '%i' not found", handle);
		return EBADF;
	}

	enum DescriptorType type = descriptor_getType(descriptor);
	if(type != DT_TCPSOCKET && type != DT_UDPSOCKET) {
		warning("wrong type for descriptor handle '%i'", handle);
		return ENOTSOCK;
	}

	return socket_getSocketName((Socket*)descriptor, ip, port);
}

gint node_sendUserData(Node* node, gint handle, gconstpointer buffer, gsize nBytes,
		in_addr_t ip, in_addr_t port, gsize* bytesCopied) {
	MAGIC_ASSERT(node);
	g_assert(bytesCopied);

	Descriptor* descriptor = node_lookupDescriptor(node, handle);
	if(descriptor == NULL) {
		warning("descriptor handle '%i' not found", handle);
		return EBADF;
	}

	enum DescriptorType type = descriptor_getType(descriptor);
	if(type != DT_TCPSOCKET && type != DT_UDPSOCKET && type != DT_PIPE) {
		return EBADF;
	}

	Transport* transport = (Transport*) descriptor;

	/* we should block if our cpu has been too busy lately */
	if(cpu_isBlocked(node->cpu)) {
		debug("blocked on CPU when trying to send %lu bytes from socket %i", nBytes, handle);

		/*
		 * immediately schedule an event to tell the socket it can write. it will
		 * pop out when the CPU delay is absorbed. otherwise we could miss writes.
		 */
		descriptor_adjustStatus(descriptor, TRUE, DS_WRITABLE);

		return EAGAIN;
	}

	gssize n = transport_sendUserData(transport, buffer, nBytes, ip, port);
	if(n > 0) {
		/* user is writing some bytes. lets assume some cpu processing delay
		 * here since they will need to copy these and process them. */
		*bytesCopied = (gsize)n;
		cpu_add_load_write(node->cpu, (guint32)n);
	} else if(n < 0) {
		return EWOULDBLOCK;
	}

	return 0;
}

gint node_receiveUserData(Node* node, gint handle, gpointer buffer, gsize nBytes,
		in_addr_t* ip, in_port_t* port, gsize* bytesCopied) {
	MAGIC_ASSERT(node);
	g_assert(ip && port && bytesCopied);

	Descriptor* descriptor = node_lookupDescriptor(node, handle);
	if(descriptor == NULL) {
		warning("descriptor handle '%i' not found", handle);
		return EBADF;
	}

	enum DescriptorType type = descriptor_getType(descriptor);
	if(type != DT_TCPSOCKET && type != DT_UDPSOCKET && type != DT_PIPE) {
		return EBADF;
	}

	Transport* transport = (Transport*) descriptor;

	/* we should block if our cpu has been too busy lately */
	if(cpu_isBlocked(node->cpu)) {
		debug("blocked on CPU when trying to send %lu bytes from socket %i", nBytes, handle);

		/*
		 * immediately schedule an event to tell the socket it can read. it will
		 * pop out when the CPU delay is absorbed. otherwise we could miss reads.
		 */
		descriptor_adjustStatus(descriptor, TRUE, DS_READABLE);

		return EAGAIN;
	}

	gssize n = transport_receiveUserData(transport, buffer, nBytes, ip, port);
	if(n > 0) {
		/* user is reading some bytes. lets assume some cpu processing delay
		 * here since they will need to copy these and process them. */
		*bytesCopied = (gsize)n;
		cpu_add_load_read(node->cpu, (guint32)n);
	} else if(n < 0) {
		return EWOULDBLOCK;
	}

	return 0;
}

gint node_closeDescriptor(Node* node, gint handle) {
	MAGIC_ASSERT(node);

	Descriptor* descriptor = node_lookupDescriptor(node, handle);
	if(descriptor == NULL) {
		warning("descriptor handle '%i' not found", handle);
		return EBADF;
	}

	return -1;
}
