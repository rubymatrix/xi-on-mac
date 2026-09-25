/* WS2_32 on host sockets (ws2.c). */
#pragma once

#include <stdint.h>

/* Starts host networking and registers the shims (before the images are mapped). */
void ws2_init(void);
/* Where PlayOnline's hosts are: gethostbyname answers "pol.com" and every "*.pol.com" name with
 * this IPv4 address (host byte order) instead of asking DNS, whose answer is Square Enix's. */
void ws2_set_pol_server(uint32_t ipv4_host_order);
/* A LandSandBoat sign-in (host/lsb_login.c): every lobby command FFXiMain sends to the login
 * server's data or view port carries this session hash at +12, where xiloader's send detour puts
 * it. */
void ws2_set_lobby_session(const uint8_t hash[16], uint16_t data_port, uint16_t view_port);
