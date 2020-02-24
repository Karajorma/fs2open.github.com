/*
 * Copyright (C) Volition, Inc. 1999.  All rights reserved.
 *
 * All source code herein is the property of Volition, Inc. You may not sell
 * or otherwise commercially exploit the source or things you created based on the
 * source.
 *
*/

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#endif

#include "pcp.h"

#include "cmdline/cmdline.h"
#include "io/timer.h"
#include "network/multi_log.h"
#include "network/multi_portfwd.h"
#include "network/psnet2.h"

// from "default_config.h" in libpcp
#ifndef PCP_SERVER_PORT
#define PCP_SERVER_PORT 5351
#endif

#ifndef PCP_MAX_SUPPORTED_VERSION
#define PCP_MAX_SUPPORTED_VERSION 2
#endif


static bool PF_initted = false;

static pcp_flow_t *PF_pcp_flow = nullptr;
static pcp_ctx_t *PF_pcp_ctx = nullptr;

static int PF_wait_timestamp = 0;

static const uint32_t PF_lifetime = 7200;	// 2 hrs

static bool PF_get_addr(const char *host, const uint16_t port, struct sockaddr *addr);
static void PF_log_init();


void multi_port_forward_init()
{
	struct sockaddr_storage gateway_addr;
	struct sockaddr_storage local_addr;
	int wait_ms;
	bool auto_discover = true;

	if (PF_initted) {
		return;
	}

	if (Cmdline_gateway_ip) {
		if ( PF_get_addr(Cmdline_gateway_ip, PCP_SERVER_PORT,
						 reinterpret_cast<struct sockaddr *>(&gateway_addr)) )
		{
			auto_discover = false;
		}
	}

	PF_log_init();	// must call before pcp_init() to fix logging

	PF_pcp_ctx = pcp_init(auto_discover ? ENABLE_AUTODISCOVERY : DISABLE_AUTODISCOVERY, nullptr);

	if (PF_pcp_ctx == nullptr) {
		ml_string("Port forward => Initialization failed!");
		return;
	}

	// this needs to be after successful pcp_init() so we'll know to shutdown
	PF_initted = true;

	if ( !auto_discover ) {
		pcp_add_server(PF_pcp_ctx, reinterpret_cast<struct sockaddr *>(&gateway_addr), PCP_MAX_SUPPORTED_VERSION);
	}

	PF_get_addr(nullptr, Psnet_default_port, reinterpret_cast<struct sockaddr *>(&local_addr));

	PF_pcp_flow = pcp_new_flow(PF_pcp_ctx, reinterpret_cast<struct sockaddr *>(&local_addr),
							   nullptr, nullptr, IPPROTO_UDP, PF_lifetime, nullptr);

	if (PF_pcp_flow == nullptr) {
		ml_string("Port forward => Failed to init mapping!");
		multi_port_forward_close();
		return;
	}

	ml_string("Port forward => Initialized successfully");

	// let's start this ball rolling...
	wait_ms = pcp_pulse(PF_pcp_ctx, nullptr);
	PF_wait_timestamp = timer_get_milliseconds() + wait_ms;
}

void multi_port_forward_do()
{
	pcp_fstate_e state;
	size_t info_count = 0;
	int wait_ms;

	if ( !PF_initted ) {
		return;
	}

	if ( PF_wait_timestamp > timer_get_milliseconds() ) {
		return;
	}

	// this ultimately does everything to maintain the connection
	wait_ms = pcp_pulse(PF_pcp_ctx, nullptr);
	PF_wait_timestamp = timer_get_milliseconds() + wait_ms;

	// check progress and log if needed
	if ( pcp_eval_flow_state(PF_pcp_flow, &state) ) {
		if (state == pcp_state_failed) {
			ml_string("Port forward => Mapping failed!");
		} else if (state == pcp_state_succeeded) {
			pcp_flow_info_t *flow_info = pcp_flow_get_info(PF_pcp_flow, &info_count);

			for (size_t idx = 0; idx < info_count; ++idx) {
				pcp_flow_info_t *info = &flow_info[idx];

				char time_str[16];
				char int_ip[INET6_ADDRSTRLEN];
				char ext_ip[INET6_ADDRSTRLEN];

				memset(&int_ip, 0, sizeof(int_ip));
				memset(&ext_ip, 0, sizeof(ext_ip));

				inet_ntop(AF_INET6, &info->int_ip, int_ip, sizeof(int_ip));
				inet_ntop(AF_INET6, &info->ext_ip, ext_ip, sizeof(ext_ip));

				ml_printf("Port forward => Mapping successful  [%s]:%u <-> [%s]:%u",
						  int_ip, ntohs(info->int_port),
						  ext_ip, ntohs(info->ext_port));

				// formatted to match what multi_log uses
				strftime(time_str, sizeof(time_str), "%m/%d %H:%M:%S", localtime(&info->recv_lifetime_end));
				ml_printf("Port forward => Mapping valid until %s", time_str);
			}

			if (flow_info) {
				free(flow_info);	// system call, allocated in lib  ** don't change! **
			}
		}
	}
}

void multi_port_forward_close()
{
	if ( !PF_initted ) {
		return;
	}

	pcp_terminate(PF_pcp_ctx, 1);

	if (PF_pcp_flow) {
		ml_string("Port forward => Mapping removed");
	} else {
		ml_string("Port forward => Shutdown");
	}

	PF_pcp_ctx = nullptr;
	PF_pcp_flow = nullptr;

	PF_initted = false;

	PF_wait_timestamp = 0;
}


static bool PF_get_addr(const char *host, const uint16_t port, struct sockaddr *addr)
{
	struct addrinfo hints, *srvinfo;
	bool success = false;

	Assert(addr != nullptr);

	memset(&hints, 0, sizeof(hints));

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_V4MAPPED;

	if (host == nullptr) {
		hints.ai_flags |= AI_PASSIVE;
	}

	SCP_string port_str = std::to_string(port);

	int rval = getaddrinfo(host, port_str.c_str(), &hints, &srvinfo);

	if (rval) {
		ml_printf("PF_get_addr : getaddrinfo() => %s", gai_strerror(rval));
		return false;
	}

	for (auto *srv = srvinfo; srv != nullptr; srv = srv->ai_next) {
		if ( (srv->ai_family == AF_INET) || (srv->ai_family == AF_INET6) ) {
			size_t ssize = sizeof(struct sockaddr_in);

			if (srv->ai_family == AF_INET6) {
				ssize = sizeof(struct sockaddr_in6);
			}

			memcpy(addr, srv->ai_addr, ssize);

			success = true;

			break;
		}
	}

	freeaddrinfo(srvinfo);

	return success;
}

#ifndef NDEBUG
static void PF_logger_fn(pcp_loglvl_e /* lvl */, const char *msg)
{
	ml_printf("Port forward => %s", msg);
}
#endif

static void PF_log_init()
{
#ifdef NDEBUG
	// log spew for libpcp is set with this
	pcp_log_level = PCP_LOGLVL_NONE;
#else
	pcp_log_level = PCP_LOGLVL_INFO;

	pcp_set_loggerfn(PF_logger_fn);
#endif
}
