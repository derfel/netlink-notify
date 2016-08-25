/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Federico Stella <code@derfel.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <iostream>
#include <chrono>
#include <thread>
#include <locale>
#include <clocale>
#include <iomanip>
#include <ctime>
#include <tuple>
#include <deque>

#include <unistd.h>
#include <net/if.h>
#include <linux/if.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>

#include <uv.h>
#include <libmnl/libmnl.h>

#include "json.hpp"
#include "streaming-worker.h"


using json = nlohmann::json;

#define		my_attr_for_each(attr, nlh, offset)	for ((attr) = (const nlattr *) mnl_nlmsg_get_payload_offset((nlh), (offset)); \
	     mnl_attr_ok((attr), (char *)mnl_nlmsg_get_payload_tail(nlh) - (char *)(attr)); \
         (attr) = mnl_attr_next(attr))


/*
 * Missing constant in glibc
 */
const unsigned int IFF_LOWER_UP = 1 << 16;
const unsigned int IFF_DORMANT = 1 << 17;
const unsigned int IFF_ECHO = 1 << 18;

std::string get_iso8601_timestamp()
{
	std::ostringstream buffer;

	auto now = std::chrono::system_clock::now();

	auto milli = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
	auto now_c = std::chrono::system_clock::to_time_t(now);

	buffer << std::put_time(std::gmtime(&now_c), "%FT%T") << '.' << std::setfill('0') << std::setw(3) << milli << 'Z';

	return buffer.str();
}

using namespace std;

class Netlink;

struct cbdata {
	Netlink * self;
	const AsyncProgressWorker::ExecutionProgress& progress;
};

class Netlink: public StreamingWorker
{
	public:
		typedef std::tuple<int, json> callback_return_json_type;

		typedef AsyncProgressWorker::ExecutionProgress progress_type;

		Netlink(Callback *data, Callback *complete, Callback *error_callback, v8::Local<v8::Object> & options)
			: StreamingWorker(data, complete, error_callback), seq_(0) {

				if (!(nl_ = mnl_socket_open(NETLINK_ROUTE))) {
					SetErrorMessage("Cannot create NETLINK_ROUTE socket");
					return;
				}

				portid_ = MNL_SOCKET_AUTOPID;

				if (mnl_socket_bind(nl_, RTMGRP_LINK | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR, portid_) < 0) {
					SetErrorMessage("Cannot bind libmnl socket");
					return;
				}
			}

		void Execute(const progress_type& progress) {
			loop_ = uv_loop_new();

			uv_timer_init(loop_, &timer_req_);
			uv_poll_init_socket(loop_, &poll_handle_, mnl_socket_get_fd(nl_));

			// XXX: we need to pass this and progress through the callback
			struct cbdata cb = { this, progress };
			poll_handle_.data = &cb;

			uv_poll_start(&poll_handle_, UV_READABLE /*| UV_DISCONNECT*/, [](uv_poll_t * h, int s, int e) {
				auto self = ((struct cbdata *) h->data)->self;
				self->pollcb(((struct cbdata *) h->data)->progress, h, s, e);
			});

			timer_req_.data = this;
			uv_timer_start(&timer_req_, [](uv_timer_t * t) {
				auto self = reinterpret_cast<Netlink *>(t->data);
				self->drain_queue(t);
			}, 10, 10);

			uv_run(loop_, UV_RUN_DEFAULT);
		}

		/*
		 * hack to receive message from javascript because the loop is watching netlink fd.
		 */
		void drain_queue(uv_timer_t *)
		{
			std::deque<Message> queue;

			fromNode.readAll(queue);

			for (auto m: queue) {
				char buf[MNL_SOCKET_BUFFER_SIZE];
				struct nlmsghdr * nlh;

				if (m.name == "get_route") {
					nlh = mnl_nlmsg_put_header(buf);
					nlh->nlmsg_type = RTM_GETROUTE;
					nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
					nlh->nlmsg_seq = ++seq_;
					struct rtmsg * rtm = reinterpret_cast<struct rtmsg *>(mnl_nlmsg_put_extra_header(nlh, sizeof(struct rtmsg)));

					if (m.data == "ipv4")
						rtm->rtm_family = AF_INET;
					else
						rtm->rtm_family = AF_INET6;

					mnl_socket_sendto(nl_, nlh, nlh->nlmsg_len);
				} else if (m.name == "get_addr") {
					nlh = mnl_nlmsg_put_header(buf);
					nlh->nlmsg_type	= RTM_GETADDR;
					nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
					nlh->nlmsg_seq = ++seq_;
					struct rtgenmsg* rt = reinterpret_cast<struct rtgenmsg*>(mnl_nlmsg_put_extra_header(nlh, sizeof(struct rtgenmsg)));

					if (m.data == "ipv4")
						rt->rtgen_family = AF_INET;
					else
						rt->rtgen_family = AF_INET6;

					mnl_socket_sendto(nl_, nlh, nlh->nlmsg_len);
				} else if (m.name == "get_link") {
					nlh = mnl_nlmsg_put_header(buf);
					nlh->nlmsg_type	= RTM_GETLINK;
					nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
					nlh->nlmsg_seq = ++seq_;
					struct rtgenmsg* rt = reinterpret_cast<struct rtgenmsg*>(mnl_nlmsg_put_extra_header(nlh, sizeof(struct rtgenmsg)));
					rt->rtgen_family = AF_PACKET;

					mnl_socket_sendto(nl_, nlh, nlh->nlmsg_len);
				} else if (m.name == "stop_loop") {
					uv_stop(loop_);
				}
			}
		}

		void pollcb(const progress_type & progress, uv_poll_t * handle, int status, int events)
		{
			char buf[MNL_SOCKET_BUFFER_SIZE];

			//printf("Pollcb Status %d events: %d\n", status, events);

			if (events == UV_READABLE) {
				int ret = mnl_socket_recvfrom(nl_, buf, sizeof(buf));

				struct cbdata cb = { this, progress };

				ret = mnl_cb_run(buf, ret, 0, portid_, [](const struct nlmsghdr *nlh, void * data) -> int {
						auto self = ((struct cbdata *) data)->self;
						return self->data_cb(nlh, ((struct cbdata *) data)->progress);
					}, &cb);
			//} else if (events == UV_DISCONNECT) {
			}
		}

		int data_cb(const struct nlmsghdr *nlh, const progress_type & progress)
		{
			int retval = MNL_CB_OK;
			json j;

			//printf("Datacb: %d\n", nlh->nlmsg_type);

			switch (nlh->nlmsg_type) {
				case RTM_NEWROUTE:
				case RTM_DELROUTE:
				case RTM_GETROUTE: {
					std::tie(retval, j) = data_route_cb(nlh);
					Message tosend("route", j.dump());
					writeToNode(progress, tosend);
					break;
				}
				case RTM_NEWLINK:
				case RTM_DELLINK:
				case RTM_GETLINK: {
					std::tie(retval, j) = data_link_cb(nlh);
					Message tosend("link", j.dump());
					writeToNode(progress, tosend);
					break;
				}
				case RTM_NEWADDR:
				case RTM_DELADDR:
				case RTM_GETADDR: {
					std::tie(retval, j) = data_addr_cb(nlh);
					Message tosend("addr", j.dump());
					writeToNode(progress, tosend);
					break;
				}
			}
			return retval;
		}

		int data_addr_attr_cb(const struct nlattr *attr, void * data)
		{
			const struct nlattr **tb = reinterpret_cast<const struct nlattr **>(data);
			int type = mnl_attr_get_type(attr);

			/* skip unsupported attribute in user-space */
			if (mnl_attr_type_valid(attr, IFA_MAX) < 0)
				return MNL_CB_OK;

			switch (type) {
				case IFA_LABEL:
					if (mnl_attr_validate(attr, MNL_TYPE_STRING) < 0) {
						//perror("mnl_addr_validate");
						return MNL_CB_ERROR;
					}
					break;
			}

			tb[type] = attr;

			return MNL_CB_OK;
		}

		int data_attr_cb(const struct nlattr *attr, void * data)
		{
			const struct nlattr **tb = reinterpret_cast<const struct nlattr **>(data);
			int type = mnl_attr_get_type(attr);

			/* skip unsupported attribute in user-space */
			if (mnl_attr_type_valid(attr, IFLA_MAX) < 0)
				return MNL_CB_OK;

			switch(type) {
				case IFLA_MTU:
					if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0) {
						//perror("mnl_attr_validate1");
						return MNL_CB_ERROR;
					}
					break;
				case IFLA_IFNAME:
					if (mnl_attr_validate(attr, MNL_TYPE_STRING) < 0) {
						//perror("mnl_attr_validate2");
						return MNL_CB_ERROR;
					}
					break;
			}
			tb[type] = attr;

			return MNL_CB_OK;
		}

		static int data_attr_cb2(const struct nlattr *attr, void *data)
		{
			const struct nlattr **tb = reinterpret_cast<const struct nlattr **>(data);

			/* skip unsupported attribute in user-space */
			if (mnl_attr_type_valid(attr, RTAX_MAX) < 0)
				return MNL_CB_OK;

			if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0) {
				//perror("mnl_attr_validate");
				return MNL_CB_ERROR;
			}

			tb[mnl_attr_get_type(attr)] = attr;
			return MNL_CB_OK;
		}

		callback_return_json_type data_addr_cb(const struct nlmsghdr *nlh)
		{
			struct nlattr *tb[IFA_MAX+1] = {};
			struct ifaddrmsg *ifa = reinterpret_cast<struct ifaddrmsg *>(mnl_nlmsg_get_payload(nlh));

			json ret;

			ret["type"] = "addr";
			ret["event"] = nlh->nlmsg_type == RTM_NEWADDR ? "new" : "delete";
			ret["timestamp"] = get_iso8601_timestamp();

			ret["data"]["family"] = ifa->ifa_family == AF_INET ? "ipv4" : "ipv6";
			ret["data"]["prefixlen"] = ifa->ifa_prefixlen;

			ret["data"]["flags"] = [&]() -> std::vector<std::string> {
				std::vector<std::string> flags;

				if (ifa->ifa_flags & IFA_F_SECONDARY)
					flags.push_back("secondary");

				if (ifa->ifa_flags & IFA_F_NODAD)
					flags.push_back("nodad");

				if (ifa->ifa_flags & IFA_F_OPTIMISTIC)
					flags.push_back("optimistic");

				if (ifa->ifa_flags & IFA_F_DADFAILED)
					flags.push_back("dadfailed");

				if (ifa->ifa_flags & IFA_F_HOMEADDRESS)
					flags.push_back("homeaddress");

				if (ifa->ifa_flags & IFA_F_DEPRECATED)
					flags.push_back("deprecated");

				if (ifa->ifa_flags & IFA_F_TENTATIVE)
					flags.push_back("tentative");

				if (ifa->ifa_flags & IFA_F_PERMANENT)
					flags.push_back("permanent");

				if (ifa->ifa_flags & IFA_F_MANAGETEMPADDR)
					flags.push_back("managetempaddr");

				if (ifa->ifa_flags & IFA_F_NOPREFIXROUTE)
					flags.push_back("noprefixroute");

				if (ifa->ifa_flags & IFA_F_MCAUTOJOIN )
					flags.push_back("mcautojoin");

				if (ifa->ifa_flags & IFA_F_STABLE_PRIVACY)
					flags.push_back("stableprivacy");

				return flags;
			}();

			ret["data"]["scope"] = [&]() -> std::string {
				switch (ifa->ifa_scope) {
					case RT_SCOPE_UNIVERSE: return "universe";
					case RT_SCOPE_SITE: return "site";
					case RT_SCOPE_LINK: return "link";
					case RT_SCOPE_HOST: return "host";
					case RT_SCOPE_NOWHERE: return "nowhere";
					default: return std::string("user:") + std::to_string(ifa->ifa_scope);
				}
			}();

			char buf[IF_NAMESIZE + 1];
			ret["data"]["if"] = std::string(if_indextoname(ifa->ifa_index, &buf[0]));

			// XXX: create do_addr_parse
			do_addr_parse(nlh, sizeof(*ifa), tb);

			if (tb[IFA_LABEL]) {
				ret["data"]["label"] = std::string(mnl_attr_get_str(tb[IFA_LABEL]));
			}
			if (tb[IFA_ADDRESS]) {
				if (ifa->ifa_family == AF_INET) {
					char buf2[INET_ADDRSTRLEN];
					struct in_addr * addr = reinterpret_cast<struct in_addr *>(mnl_attr_get_payload(tb[IFA_ADDRESS]));
					inet_ntop(AF_INET, addr, buf2, INET_ADDRSTRLEN);

					ret["data"]["addr"] = std::string(buf2);
				} else {
					char buf2[INET6_ADDRSTRLEN];
					struct in6_addr * addr = reinterpret_cast<struct in6_addr *>(mnl_attr_get_payload(tb[IFA_ADDRESS]));
					inet_ntop(AF_INET6, addr, buf2, INET6_ADDRSTRLEN);

					ret["data"]["addr"] = std::string(buf2);
				}
			}
			if (tb[IFA_BROADCAST]) {
				if (ifa->ifa_family == AF_INET) {
					char buf2[INET_ADDRSTRLEN];
					struct in_addr * addr = reinterpret_cast<struct in_addr *>(mnl_attr_get_payload(tb[IFA_BROADCAST]));
					inet_ntop(AF_INET, addr, buf2, INET_ADDRSTRLEN);

					ret["data"]["broadcast"] = std::string(buf2);
				} else {
					char buf2[INET6_ADDRSTRLEN];
					struct in6_addr * addr = reinterpret_cast<struct in6_addr *>(mnl_attr_get_payload(tb[IFA_BROADCAST]));
					inet_ntop(AF_INET6, addr, buf2, INET6_ADDRSTRLEN);

					ret["data"]["broadcast"] = std::string(buf2);
				}
			}

			return std::make_tuple(MNL_CB_OK, ret);
		}

		callback_return_json_type data_link_cb(const struct nlmsghdr *nlh)
		{
			struct nlattr *tb[IFLA_MAX+1] = {};
			struct ifinfomsg *ifm = reinterpret_cast<struct ifinfomsg *>(mnl_nlmsg_get_payload(nlh));

			json ret;

			/*
			printf("index=%d type=%d flags=%d family=%d ",
					ifm->ifi_index, ifm->ifi_type,
					ifm->ifi_flags, ifm->ifi_family);
					*/

			ret["data"]["index"] = ifm->ifi_index;
			ret["data"]["type"] = ifm->ifi_type;

			ret["data"]["flags"] = [&]() -> std::vector<std::string> {
				std::vector<std::string> flags;

				if (ifm->ifi_flags & IFF_UP)
					flags.push_back("up");

				if (ifm->ifi_flags & IFF_BROADCAST)
					flags.push_back("broadcast");

				if (ifm->ifi_flags & IFF_DEBUG)
					flags.push_back("debug");

				if (ifm->ifi_flags & IFF_LOOPBACK)
					flags.push_back("loopback");

				if (ifm->ifi_flags & IFF_POINTOPOINT)
					flags.push_back("pointtopoint");

				if (ifm->ifi_flags & IFF_NOTRAILERS)
					flags.push_back("notrailers");

				if (ifm->ifi_flags & IFF_RUNNING)
					flags.push_back("running");

				if (ifm->ifi_flags & IFF_NOARP)
					flags.push_back("noarp");

				if (ifm->ifi_flags & IFF_PROMISC)
					flags.push_back("promisc");

				if (ifm->ifi_flags & IFF_ALLMULTI)
					flags.push_back("allmulti");

				if (ifm->ifi_flags & IFF_MASTER)
					flags.push_back("master");

				if (ifm->ifi_flags & IFF_SLAVE)
					flags.push_back("slave");

				if (ifm->ifi_flags & IFF_MULTICAST)
					flags.push_back("multicast");

				if (ifm->ifi_flags & IFF_PORTSEL)
					flags.push_back("portsel");

				if (ifm->ifi_flags & IFF_AUTOMEDIA)
					flags.push_back("automedia");

				if (ifm->ifi_flags & IFF_DYNAMIC)
					flags.push_back("dynamic");

				if (ifm->ifi_flags & IFF_LOWER_UP)
					flags.push_back("lowerup");

				if (ifm->ifi_flags & IFF_DORMANT)
					flags.push_back("dormant");

				if (ifm->ifi_flags & IFF_ECHO)
					flags.push_back("echo");

				return flags;
			}();

			ret["data"]["family"] = ifm->ifi_family;

			if (ifm->ifi_flags & IFF_RUNNING) {
				ret["data"]["running"] = true;
			} else {
				ret["data"]["running"] = false;
			}
			if (ifm->ifi_flags & IFF_UP) {
				ret["data"]["up"] = true;
			} else {
				ret["data"]["up"] = false;
			}

			/*
			do_attr_parse(nlh, sizeof(*ifm), [&](const struct nlattr *attr, void * data) -> int {
					ret["data"]urn data_attr_cb(attr, data);
				}, tb
			);
			*/

			do_attr_parse(nlh, sizeof(*ifm), tb);

			if (tb[IFLA_MTU]) {
				ret["data"]["mtu"] = mnl_attr_get_u32(tb[IFLA_MTU]);
			}
			if (tb[IFLA_IFNAME]) {
				ret["data"]["name"] = std::string(mnl_attr_get_str(tb[IFLA_IFNAME]));
			}

			ret["type"] = "link";
			ret["event"] = nlh->nlmsg_type == RTM_NEWLINK ? "new" : "delete";
			ret["timestamp"] = get_iso8601_timestamp();

			return std::make_tuple(MNL_CB_OK, ret);
		}

		callback_return_json_type data_route_cb(const struct nlmsghdr *nlh)
		{
			struct nlattr *tb[RTA_MAX+1] = {};
			struct rtmsg *rm = reinterpret_cast<struct rtmsg *>(mnl_nlmsg_get_payload(nlh));

			json reply;

			reply["type"] = "route";
			reply["timestamp"] = get_iso8601_timestamp();

			switch(nlh->nlmsg_type) {
				case RTM_NEWROUTE:
					reply["event"] = "new";
					break;
				case RTM_DELROUTE:
					reply["event"] = "delete";
					break;
			}

			/* protocol family = AF_INET | AF_INET6 */
			reply["data"]["family"] = rm->rtm_family == AF_INET ? "ipv4" : "ipv6";

			/* destination CIDR, eg. 24 or 32 for IPv4 */
			reply["data"]["dst_len"] = rm->rtm_dst_len;

			/* source CIDR */
			reply["data"]["src_len"] = rm->rtm_src_len;

			/* type of service (TOS), eg. 0 */
			reply["data"]["tos"] = rm->rtm_tos;

			/* table id:
			 *	RT_TABLE_UNSPEC		= 0
			 *
			 *	... user defined values ...
			 *
			 *	RT_TABLE_COMPAT		= 252
			 *	RT_TABLE_DEFAULT	= 253
			 *	RT_TABLE_MAIN		= 254
			 *	RT_TABLE_LOCAL		= 255
			 *	RT_TABLE_MAX		= 0xFFFFFFFF
			 *
			 * Synonimous attribute: RTA_TABLE.
			 */
			reply["data"]["table"] = [&]() -> std::string {
				switch (rm->rtm_table) {
					case RT_TABLE_UNSPEC: return "unspec";
					case RT_TABLE_COMPAT: return "compat";
					case RT_TABLE_DEFAULT: return "default";
					case RT_TABLE_MAIN: return "main";
					case RT_TABLE_LOCAL: return "local";
					default: return std::string("user:") + std::to_string(rm->rtm_table);
				}
			}();

			/* type:
			 *	RTN_UNSPEC	= 0
			 *	RTN_UNICAST	= 1
			 *	RTN_LOCAL	= 2
			 *	RTN_BROADCAST	= 3
			 *	RTN_ANYCAST	= 4
			 *	RTN_MULTICAST	= 5
			 *	RTN_BLACKHOLE	= 6
			 *	RTN_UNREACHABLE	= 7
			 *	RTN_PROHIBIT	= 8
			 *	RTN_THROW	= 9
			 *	RTN_NAT		= 10
			 *	RTN_XRESOLVE	= 11
			 *	__RTN_MAX	= 12
			 */
			reply["data"]["type"] = [&]() {
				switch(rm->rtm_type) {
					case RTN_UNSPEC: return "unspec";
					case RTN_UNICAST: return "unicast";
					case RTN_LOCAL: return "local";
					case RTN_BROADCAST: return "broadcast";
					case RTN_ANYCAST: return "anycast";
					case RTN_MULTICAST: return "multicast";
					case RTN_BLACKHOLE: return "blackhole";
					case RTN_UNREACHABLE: return "unreacheable";
					case RTN_PROHIBIT: return "prohibit";
					case RTN_THROW: return "throw";
					case RTN_NAT: return "nat";
					case RTN_XRESOLVE: return "xresolve";
					default: return "unspec";
				}
			}();

			/* scope:
			 *	RT_SCOPE_UNIVERSE	= 0   : everywhere in the universe
			 *
			 *      ... user defined values ...
			 *
			 *	RT_SCOPE_SITE		= 200
			 *	RT_SCOPE_LINK		= 253 : destination attached to link
			 *	RT_SCOPE_HOST		= 254 : local address
			 *	RT_SCOPE_NOWHERE	= 255 : not existing destination
			 */
			reply["data"]["scope"] = [&]() -> std::string {
				switch (rm->rtm_scope) {
					case RT_SCOPE_UNIVERSE: return "universe";
					case RT_SCOPE_SITE: return "site";
					case RT_SCOPE_LINK: return "link";
					case RT_SCOPE_HOST: return "host";
					case RT_SCOPE_NOWHERE: return "nowhere";
					default: return std::string("user:") + std::to_string(rm->rtm_scope);
				}
			}();

			/* protocol:
			 *	RTPROT_UNSPEC	= 0
			 *	RTPROT_REDIRECT = 1
			 *	RTPROT_KERNEL	= 2 : route installed by kernel
			 *	RTPROT_BOOT	= 3 : route installed during boot
			 *	RTPROT_STATIC	= 4 : route installed by administrator
			 *
			 * Values >= RTPROT_STATIC are not interpreted by kernel, they are
			 * just user-defined.
			 */
			reply["data"]["proto"] = [&]() -> std::string {
				switch (rm->rtm_protocol) {
					case RTPROT_UNSPEC: return "unspec";
					case RTPROT_REDIRECT: return "redirect";
					case RTPROT_KERNEL: return "kernel";
					case RTPROT_BOOT: return "boot";
					case RTPROT_STATIC: return "static";
					default: return std::string("user:") + std::to_string(rm->rtm_protocol);
				}
			}();

			/* flags:
			 *	RTM_F_NOTIFY	= 0x100: notify user of route change
			 *	RTM_F_CLONED	= 0x200: this route is cloned
			 *	RTM_F_EQUALIZE	= 0x400: Multipath equalizer: NI
			 *	RTM_F_PREFIX	= 0x800: Prefix addresses
			 */
			reply["data"]["flags"] = [&]() -> std::vector<std::string> {
				std::vector<std::string> flags;

				if (rm->rtm_flags & RTM_F_NOTIFY)
					flags.push_back("notify");

				if (rm->rtm_flags & RTM_F_CLONED)
					flags.push_back("cloned");

				if (rm->rtm_flags & RTM_F_EQUALIZE)
					flags.push_back("equalize");

				if (rm->rtm_flags & RTM_F_PREFIX)
					flags.push_back("prefix");

				if (rm->rtm_flags & RTM_F_LOOKUP_TABLE)
					flags.push_back("lookup");

				return flags;
			}();

			switch(rm->rtm_family) {
				case AF_INET:
					do_attr_ipv4_parse(nlh, sizeof(*rm), tb);
					reply["data"]["addr"] = attributes_show_ipv4(tb);
					break;
				case AF_INET6:
					do_attr_ipv6_parse(nlh, sizeof(*rm), tb);
					reply["data"]["addr"] = attributes_show_ipv6(tb);
					break;
			}

			return std::make_tuple(MNL_CB_OK, reply);
		}

		int data_ipv4_attr_cb(const struct nlattr *attr, void *data)
		{
			const struct nlattr **tb = reinterpret_cast<const struct nlattr **>(data);
			int type = mnl_attr_get_type(attr);

			/* skip unsupported attribute in user-space */
			if (mnl_attr_type_valid(attr, RTA_MAX) < 0)
				return MNL_CB_OK;

			switch(type) {
				case RTA_TABLE:
				case RTA_DST:
				case RTA_SRC:
				case RTA_OIF:
				case RTA_FLOW:
				case RTA_PREFSRC:
				case RTA_GATEWAY:
				case RTA_PRIORITY:
					if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0) {
						//perror("mnl_attr_validate3");
						return MNL_CB_ERROR;
					}
					break;
				case RTA_METRICS:
					if (mnl_attr_validate(attr, MNL_TYPE_NESTED) < 0) {
						//perror("mnl_attr_validate4");
						return MNL_CB_ERROR;
					}
					break;
			}
			tb[type] = attr;
			return MNL_CB_OK;
		}

		int data_ipv6_attr_cb(const struct nlattr *attr, void *data)
		{
			const struct nlattr **tb = reinterpret_cast<const struct nlattr **>(data);
			int type = mnl_attr_get_type(attr);

			/* skip unsupported attribute in user-space */
			if (mnl_attr_type_valid(attr, RTA_MAX) < 0)
				return MNL_CB_OK;

			switch(type) {
				case RTA_TABLE:
				case RTA_OIF:
				case RTA_FLOW:
				case RTA_PRIORITY:
					if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0) {
						//perror("mnl_attr_validate5");
						return MNL_CB_ERROR;
					}
					break;
				case RTA_DST:
				case RTA_SRC:
				case RTA_PREFSRC:
				case RTA_GATEWAY:
					if (mnl_attr_validate2(attr, MNL_TYPE_BINARY,
								sizeof(struct in6_addr)) < 0) {
						//perror("mnl_attr_validate6");
						return MNL_CB_ERROR;
					}
					break;
				case RTA_METRICS:
					if (mnl_attr_validate(attr, MNL_TYPE_NESTED) < 0) {
						//perror("mnl_attr_validate7");
						return MNL_CB_ERROR;
					}
					break;
			}
			tb[type] = attr;
			return MNL_CB_OK;
		}

		json attributes_show_ipv4(struct nlattr *tb[])
		{
			char buf[INET_ADDRSTRLEN];
			json ret;

			if (tb[RTA_TABLE]) {
				ret["table"] = [&]() -> std::string {
					int table = mnl_attr_get_u32(tb[RTA_TABLE]);
					switch (table) {
						case RT_TABLE_UNSPEC: return "unspec";
						case RT_TABLE_COMPAT: return "compat";
						case RT_TABLE_DEFAULT: return "default";
						case RT_TABLE_MAIN: return "main";
						case RT_TABLE_LOCAL: return "local";
						default: return std::string("user:") + std::to_string(table);
					}
				}();
			}
			if (tb[RTA_DST]) {
				struct in_addr *addr = reinterpret_cast<struct in_addr *>(mnl_attr_get_payload(tb[RTA_DST]));
				inet_ntop(AF_INET, addr, buf, INET_ADDRSTRLEN);

				ret["dst"] = std::string(buf);
			}
			if (tb[RTA_SRC]) {
				struct in_addr *addr = reinterpret_cast<struct in_addr *>(mnl_attr_get_payload(tb[RTA_SRC]));
				inet_ntop(AF_INET, addr, buf, INET_ADDRSTRLEN);

				ret["src"] = std::string(buf);
			}
			if (tb[RTA_OIF]) {
				char buf[IF_NAMESIZE + 1];
				ret["oif"] = std::string(if_indextoname(mnl_attr_get_u32(tb[RTA_OIF]), &buf[0]));
			}
			if (tb[RTA_FLOW]) {
				ret["flow"] = mnl_attr_get_u32(tb[RTA_FLOW]);
			}
			if (tb[RTA_PREFSRC]) {
				struct in_addr *addr = reinterpret_cast<struct in_addr *>(mnl_attr_get_payload(tb[RTA_PREFSRC]));
				inet_ntop(AF_INET, addr, buf, INET_ADDRSTRLEN);

				ret["prefsrc"] = std::string(buf);
			}
			if (tb[RTA_GATEWAY]) {
				struct in_addr *addr = reinterpret_cast<struct in_addr *>(mnl_attr_get_payload(tb[RTA_GATEWAY]));
				inet_ntop(AF_INET, addr, buf, INET_ADDRSTRLEN);

				ret["gw"] = std::string(buf);
			}
			if (tb[RTA_PRIORITY]) {
				ret["prio"] = mnl_attr_get_u32(tb[RTA_PRIORITY]);
			}
			/*
			if (tb[RTA_METRICS]) {
				struct nlattr * tbx[RTAX_MAX + 1] = {};

				mnl_attr_parse_nested(tb[RTA_METRICS], data_attr_cb2, tbx);

				if (tbx[RTAX_MTU])
					ret["metrics"]["mtu"] = mnl_attr_get_u32(tbx[RTAX_MTU]);
				if (tbx[RTAX_WINDOW])
					ret["metrics"]["window"] = mnl_attr_get_u32(tbx[RTAX_WINDOW]);
				if (tbx[RTAX_RTT])
					ret["metrics"]["rtt"] = mnl_attr_get_u32(tbx[RTAX_RTT]);
				if (tbx[RTAX_CWND])
					ret["metrics"]["cwnd"] = mnl_attr_get_u32(tbx[RTAX_CWND]);
				if (tbx[RTAX_HOPLIMIT])
					ret["metrics"]["hoplimit"] = mnl_attr_get_u32(tbx[RTAX_HOPLIMIT]);

				for (int i = 0; i < RTAX_MAX; i++) {
					if (tbx[i]) {
						printf("----------->>>>>>>>>> metrics[%d]=%u ", i, mnl_attr_get_u32(tbx[i]));
					}
				}
			}
			*/

			return ret;
		}

		json attributes_show_ipv6(struct nlattr *tb[])
		{
			char buf[INET6_ADDRSTRLEN];
			json ret;

			if (tb[RTA_TABLE]) {
				//printf("table=%u ", mnl_attr_get_u32(tb[RTA_TABLE]));

				ret["table"] = [&]() -> std::string {
					int table = mnl_attr_get_u32(tb[RTA_TABLE]);
					switch (table) {
						case RT_TABLE_UNSPEC: return "unspec";
						case RT_TABLE_COMPAT: return "compat";
						case RT_TABLE_DEFAULT: return "default";
						case RT_TABLE_MAIN: return "main";
						case RT_TABLE_LOCAL: return "local";
						default: return std::string("user:") + std::to_string(table);
					}
				}();
			}
			if (tb[RTA_DST]) {
				struct in6_addr *addr = reinterpret_cast<struct in6_addr *>(mnl_attr_get_payload(tb[RTA_DST]));
				inet_ntop(AF_INET6, addr, buf, INET6_ADDRSTRLEN);

				ret["dst"] = std::string(buf);
			}
			if (tb[RTA_SRC]) {
				struct in6_addr *addr = reinterpret_cast<struct in6_addr *>(mnl_attr_get_payload(tb[RTA_SRC]));
				inet_ntop(AF_INET6, addr, buf, INET6_ADDRSTRLEN);

				ret["src"] = std::string(buf);
			}
			if (tb[RTA_OIF]) {
				char buf[IF_NAMESIZE + 1];
				ret["oif"] = std::string(if_indextoname(mnl_attr_get_u32(tb[RTA_OIF]), &buf[0]));
			}
			if (tb[RTA_FLOW]) {
				ret["flow"] = mnl_attr_get_u32(tb[RTA_FLOW]);
			}
			if (tb[RTA_PREFSRC]) {
				struct in6_addr *addr = reinterpret_cast<struct in6_addr *>(mnl_attr_get_payload(tb[RTA_PREFSRC]));
				inet_ntop(AF_INET6, addr, buf, INET6_ADDRSTRLEN);

				ret["prefsrc"] = std::string(buf);
			}
			if (tb[RTA_GATEWAY]) {
				struct in6_addr *addr = reinterpret_cast<struct in6_addr *>(mnl_attr_get_payload(tb[RTA_GATEWAY]));
				inet_ntop(AF_INET6, addr, buf, INET6_ADDRSTRLEN);

				ret["gw"] = std::string(buf);
			}
			if (tb[RTA_PRIORITY]) {
				ret["prio"] = mnl_attr_get_u32(tb[RTA_PRIORITY]);
			}
			/*
			if (tb[RTA_METRICS]) {
				int i;
				struct nlattr *tbx[RTAX_MAX+1] = {};

				do_attr_parse_nested(tb[RTA_METRICS], data_attr_cb2, tbx);

				for (i=0; i<RTAX_MAX; i++) {
					if (tbx[i]) {
						//printf("metrics[%d]=%u ",
								i, mnl_attr_get_u32(tbx[i]));
					}
				}
			}
			*/

			return ret;
		}
		int do_attr_ipv4_parse(const struct nlmsghdr *nlh, unsigned int offset, void * data)
		{
			int ret = MNL_CB_OK;
			const struct nlattr *attr;

			my_attr_for_each(attr, nlh, offset) {
				if ((ret = data_ipv4_attr_cb(attr, data)) <= MNL_CB_STOP)
					return ret;
			}

			return ret;
		}

		int do_attr_ipv6_parse(const struct nlmsghdr *nlh, unsigned int offset, void * data)
		{
			int ret = MNL_CB_OK;
			const struct nlattr *attr;

			my_attr_for_each(attr, nlh, offset) {
				if ((ret = data_ipv6_attr_cb(attr, data)) <= MNL_CB_STOP)
					return ret;
			}

			return ret;
		}

		int do_addr_parse(const struct nlmsghdr *nlh, unsigned int offset, void * data)
		{
			int ret = MNL_CB_OK;
			const struct nlattr *attr;

			my_attr_for_each(attr, nlh, offset) {
				if ((ret = data_addr_attr_cb(attr, data)) <= MNL_CB_STOP)
					return ret;
			}

			return ret;
		}
		int do_attr_parse(const struct nlmsghdr *nlh, unsigned int offset, void * data)
		{
			int ret = MNL_CB_OK;
			const struct nlattr *attr;

			my_attr_for_each(attr, nlh, offset) {
				if ((ret = data_attr_cb(attr, data)) <= MNL_CB_STOP)
					return ret;
			}

			return ret;
		}

	private:
		struct mnl_socket * nl_;
		uv_loop_t * loop_;
		uv_poll_t poll_handle_;
		uv_timer_t timer_req_;
		unsigned int seq_;
		unsigned int portid_;
};

// Important:  You MUST include this function, and you cannot alter
//             the signature at all.  The base wrapper class calls this
//             to build your particular worker.  The prototype for this
//             function is defined in addon-streams.h
StreamingWorker * create_worker(Callback *data
		, Callback *complete
		, Callback *error_callback, v8::Local<v8::Object> & options) {
	return new Netlink(data, complete, error_callback, options);
}

// Don't forget this!  You can change the name of your module,
// but the second parameter should always be as shown.
NODE_MODULE(netlink_worker, StreamWorkerWrapper::Init)
