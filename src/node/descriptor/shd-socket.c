/**
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

gboolean socket_pushInPacket(Socket* socket, Packet* packet) {
	MAGIC_ASSERT(socket);
	MAGIC_ASSERT(socket->vtable);
	return socket->vtable->push((Transport*)socket, packet);
}

Packet* socket_pullOutPacket(Socket* socket) {
	MAGIC_ASSERT(socket);
	MAGIC_ASSERT(socket->vtable);
	return socket->vtable->pull((Transport*)socket);
}

gssize socket_sendUserData(Socket* socket, gconstpointer buffer, gsize nBytes,
		in_addr_t ip, in_port_t port) {
	MAGIC_ASSERT(socket);
	MAGIC_ASSERT(socket->vtable);
	return socket->vtable->send((Transport*)socket, buffer, nBytes, ip, port);
}

gssize socket_receiveUserData(Socket* socket, gpointer buffer, gsize nBytes,
		in_addr_t* ip, in_port_t* port) {
	MAGIC_ASSERT(socket);
	MAGIC_ASSERT(socket->vtable);
	return socket->vtable->receive((Transport*)socket, buffer, nBytes, ip, port);
}

void socket_free(gpointer data) {
	Socket* socket = data;
	MAGIC_ASSERT(socket);
	MAGIC_ASSERT(socket->vtable);

	MAGIC_CLEAR(socket);
	socket->vtable->free((Descriptor*)socket);
}

TransportFunctionTable socket_functions = {
	(DescriptorFreeFunc) socket_free,
	(TransportSendFunc) socket_sendUserData,
	(TransportReceiveFunc) socket_receiveUserData,
	(TransportPushFunc) socket_pushInPacket,
	(TransportPullFunc) socket_pullOutPacket,
	MAGIC_VALUE
};

void socket_init(Socket* socket, SocketFunctionTable* vtable, enum DescriptorType type, gint handle) {
	g_assert(socket && vtable);

	transport_init(&(socket->super), &socket_functions, type, handle);

	MAGIC_INIT(socket);
	MAGIC_INIT(vtable);

	socket->vtable = vtable;
}

/* interface functions, implemented by subtypes */

gboolean socket_isFamilySupported(Socket* socket, sa_family_t family) {
	MAGIC_ASSERT(socket);
	MAGIC_ASSERT(socket->vtable);
	return socket->vtable->isFamilySupported(socket, family);
}

gint socket_connectToPeer(Socket* socket, in_addr_t ip, in_port_t port, sa_family_t family) {
	MAGIC_ASSERT(socket);
	MAGIC_ASSERT(socket->vtable);
	return socket->vtable->connectToPeer(socket, ip, port, family);
}

/* functions implemented by socket */

gint socket_getPeerName(Socket* socket, in_addr_t* ip, in_port_t* port) {
	MAGIC_ASSERT(socket);
	g_assert(ip && port);

	if(socket->peerIP == 0 || socket->peerPort == 0) {
		return ENOTCONN;
	}

	*ip = socket->peerIP;
	*port = socket->peerPort;

	return 0;
}

gint socket_getSocketName(Socket* socket, in_addr_t* ip, in_port_t* port) {
	MAGIC_ASSERT(socket);
	g_assert(ip && port);

	if(socket->super.boundAddress == 0 || socket->super.boundPort == 0) {
		return ENOTCONN;
	}

	*ip = socket->super.boundAddress;
	*port = socket->super.boundPort;

	return 0;
}
