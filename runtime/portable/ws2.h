/* WS2_32 on host sockets (ws2.c). */
#pragma once

/* Starts host networking and registers the shims (before the images are mapped). */
void ws2_init(void);
