/**
 * @file    udp.h
 * @brief   Platform-agnostic UDP socket wrapper.
 *
 * On Windows, uses Winsock2 (ws2_32). On POSIX, uses BSD sockets.
 * Provides simple create / recv / sendto / destroy operations
 * with non-blocking receive + timeout support.
 */

#ifndef UDP_H
#define UDP_H

#include <stdint.h>

/* Opaque socket handle */
typedef struct UDPSocket UDPSocket;

/**
 * @brief Create a UDP socket bound to the given port.
 * @param local_port  Port to bind on this machine (0.0.0.0).
 * @return Socket handle, or NULL on failure.
 */
UDPSocket* udp_socket_create(int local_port);

/**
 * @brief Close and free the socket.
 */
void udp_socket_destroy(UDPSocket* sock);

/**
 * @brief Receive a datagram (non-blocking with timeout).
 * @param sock        Socket handle.
 * @param buf         Receive buffer.
 * @param buf_size    Buffer capacity.
 * @param timeout_ms  Max wait time in milliseconds (0 = poll, -1 = block).
 * @return Number of bytes received, 0 on timeout, -1 on error.
 */
int udp_socket_recv(UDPSocket* sock, uint8_t* buf, int buf_size, int timeout_ms);

/**
 * @brief Send a datagram to the specified address.
 * @param sock       Socket handle.
 * @param data       Data to send.
 * @param len        Data length.
 * @param dest_ip    Destination IP string (e.g. "127.0.0.1").
 * @param dest_port  Destination port.
 * @return Number of bytes sent, -1 on error.
 */
int udp_socket_sendto(UDPSocket* sock, const uint8_t* data, int len,
                      const char* dest_ip, int dest_port);

#endif /* UDP_H */
