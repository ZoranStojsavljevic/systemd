/* SPDX-License-Identifier: LGPL-2.1+ */

#include "errno-util.h"
#include "fd-util.h"
#include "missing_network.h"
#include "resolved-dns-stub.h"
#include "socket-netlink.h"
#include "socket-util.h"

/* The MTU of the loopback device is 64K on Linux, advertise that as maximum datagram size, but subtract the Ethernet,
 * IP and UDP header sizes */
#define ADVERTISE_DATAGRAM_SIZE_MAX (65536U-14U-20U-8U)

static int manager_dns_stub_udp_fd(Manager *m);
static int manager_dns_stub_tcp_fd(Manager *m);

int dns_stub_extra_new(DNSStubListenerExtra **ret) {
        DNSStubListenerExtra *l;

        l = new(DNSStubListenerExtra, 1);
        if (!l)
                return -ENOMEM;

        *l = (DNSStubListenerExtra) {
                .fd = -1,
        };

        *ret = TAKE_PTR(l);

        return 0;
}

static int dns_stub_make_reply_packet(
                DnsPacket **p,
                size_t max_size,
                DnsQuestion *q,
                DnsAnswer *answer,
                bool *ret_truncated) {

        bool truncated = false;
        DnsResourceRecord *rr;
        unsigned c = 0;
        int r;

        assert(p);

        /* Note that we don't bother with any additional RRs, as this is stub is for local lookups only, and hence
         * roundtrips aren't expensive. */

        if (!*p) {
                r = dns_packet_new(p, DNS_PROTOCOL_DNS, 0, max_size);
                if (r < 0)
                        return r;

                r = dns_packet_append_question(*p, q);
                if (r < 0)
                        return r;

                DNS_PACKET_HEADER(*p)->qdcount = htobe16(dns_question_size(q));
        }

        DNS_ANSWER_FOREACH(rr, answer) {

                r = dns_question_matches_rr(q, rr, NULL);
                if (r < 0)
                        return r;
                if (r > 0)
                        goto add;

                r = dns_question_matches_cname_or_dname(q, rr, NULL);
                if (r < 0)
                        return r;
                if (r > 0)
                        goto add;

                continue;
        add:
                r = dns_packet_append_rr(*p, rr, 0, NULL, NULL);
                if (r == -EMSGSIZE) {
                        truncated = true;
                        break;
                }
                if (r < 0)
                        return r;

                c++;
        }

        if (ret_truncated)
                *ret_truncated = truncated;
        else if (truncated)
                return -EMSGSIZE;

        DNS_PACKET_HEADER(*p)->ancount = htobe16(be16toh(DNS_PACKET_HEADER(*p)->ancount) + c);

        return 0;
}

static int dns_stub_finish_reply_packet(
                DnsPacket *p,
                uint16_t id,
                int rcode,
                bool tc,        /* set the Truncated bit? */
                bool add_opt,   /* add an OPT RR to this packet? */
                bool edns0_do,  /* set the EDNS0 DNSSEC OK bit? */
                bool ad) {      /* set the DNSSEC authenticated data bit? */

        int r;

        assert(p);

        if (add_opt) {
                r = dns_packet_append_opt(p, ADVERTISE_DATAGRAM_SIZE_MAX, edns0_do, rcode, NULL);
                if (r == -EMSGSIZE) /* Hit the size limit? then indicate truncation */
                        tc = true;
                else if (r < 0)
                        return r;

        } else {
                /* If the client can't to EDNS0, don't do DO either */
                edns0_do = false;

                /* If the client didn't do EDNS, clamp the rcode to 4 bit */
                if (rcode > 0xF)
                        rcode = DNS_RCODE_SERVFAIL;
        }

        /* Don't set the AD bit unless DO is on, too */
        if (!edns0_do)
                ad = false;

        DNS_PACKET_HEADER(p)->id = id;

        DNS_PACKET_HEADER(p)->flags = htobe16(DNS_PACKET_MAKE_FLAGS(
                                                              1  /* qr */,
                                                              0  /* opcode */,
                                                              0  /* aa */,
                                                              tc /* tc */,
                                                              1  /* rd */,
                                                              1  /* ra */,
                                                              ad /* ad */,
                                                              0  /* cd */,
                                                              rcode));

        return 0;
}

static int dns_stub_send(Manager *m, DnsStream *s, DnsPacket *p, DnsPacket *reply) {
        int r;

        assert(m);
        assert(p);
        assert(reply);

        if (s)
                r = dns_stream_write_packet(s, reply);
        else {
                int fd;

                fd = manager_dns_stub_udp_fd(m);
                if (fd < 0)
                        return log_debug_errno(fd, "Failed to get reply socket: %m");

                /* Note that it is essential here that we explicitly choose the source IP address for this packet. This
                 * is because otherwise the kernel will choose it automatically based on the routing table and will
                 * thus pick 127.0.0.1 rather than 127.0.0.53. */

                r = manager_send(m, fd, LOOPBACK_IFINDEX, p->family, &p->sender, p->sender_port, &p->destination, reply);
        }
        if (r < 0)
                return log_debug_errno(r, "Failed to send reply packet: %m");

        return 0;
}

static int dns_stub_send_failure(Manager *m, DnsStream *s, DnsPacket *p, int rcode, bool authenticated) {
        _cleanup_(dns_packet_unrefp) DnsPacket *reply = NULL;
        int r;

        assert(m);
        assert(p);

        r = dns_stub_make_reply_packet(&reply, DNS_PACKET_PAYLOAD_SIZE_MAX(p), p->question, NULL, NULL);
        if (r < 0)
                return log_debug_errno(r, "Failed to make failure packet: %m");

        r = dns_stub_finish_reply_packet(reply, DNS_PACKET_ID(p), rcode, false, !!p->opt, DNS_PACKET_DO(p), authenticated);
        if (r < 0)
                return log_debug_errno(r, "Failed to build failure packet: %m");

        return dns_stub_send(m, s, p, reply);
}

static void dns_stub_query_complete(DnsQuery *q) {
        int r;

        assert(q);
        assert(q->request_dns_packet);

        switch (q->state) {

        case DNS_TRANSACTION_SUCCESS: {
                bool truncated;

                r = dns_stub_make_reply_packet(&q->reply_dns_packet, DNS_PACKET_PAYLOAD_SIZE_MAX(q->request_dns_packet), q->question_idna, q->answer, &truncated);
                if (r < 0) {
                        log_debug_errno(r, "Failed to build reply packet: %m");
                        break;
                }

                if (!truncated) {
                        r = dns_query_process_cname(q);
                        if (r == -ELOOP) {
                                (void) dns_stub_send_failure(q->manager, q->request_dns_stream, q->request_dns_packet, DNS_RCODE_SERVFAIL, false);
                                break;
                        }
                        if (r < 0) {
                                log_debug_errno(r, "Failed to process CNAME: %m");
                                break;
                        }
                        if (r == DNS_QUERY_RESTARTED)
                                return;
                }

                r = dns_stub_finish_reply_packet(
                                q->reply_dns_packet,
                                DNS_PACKET_ID(q->request_dns_packet),
                                q->answer_rcode,
                                truncated,
                                !!q->request_dns_packet->opt,
                                DNS_PACKET_DO(q->request_dns_packet),
                                dns_query_fully_authenticated(q));
                if (r < 0) {
                        log_debug_errno(r, "Failed to finish reply packet: %m");
                        break;
                }

                (void) dns_stub_send(q->manager, q->request_dns_stream, q->request_dns_packet, q->reply_dns_packet);
                break;
        }

        case DNS_TRANSACTION_RCODE_FAILURE:
                (void) dns_stub_send_failure(q->manager, q->request_dns_stream, q->request_dns_packet, q->answer_rcode, dns_query_fully_authenticated(q));
                break;

        case DNS_TRANSACTION_NOT_FOUND:
                (void) dns_stub_send_failure(q->manager, q->request_dns_stream, q->request_dns_packet, DNS_RCODE_NXDOMAIN, dns_query_fully_authenticated(q));
                break;

        case DNS_TRANSACTION_TIMEOUT:
        case DNS_TRANSACTION_ATTEMPTS_MAX_REACHED:
                /* Propagate a timeout as a no packet, i.e. that the client also gets a timeout */
                break;

        case DNS_TRANSACTION_NO_SERVERS:
        case DNS_TRANSACTION_INVALID_REPLY:
        case DNS_TRANSACTION_ERRNO:
        case DNS_TRANSACTION_ABORTED:
        case DNS_TRANSACTION_DNSSEC_FAILED:
        case DNS_TRANSACTION_NO_TRUST_ANCHOR:
        case DNS_TRANSACTION_RR_TYPE_UNSUPPORTED:
        case DNS_TRANSACTION_NETWORK_DOWN:
                (void) dns_stub_send_failure(q->manager, q->request_dns_stream, q->request_dns_packet, DNS_RCODE_SERVFAIL, false);
                break;

        case DNS_TRANSACTION_NULL:
        case DNS_TRANSACTION_PENDING:
        case DNS_TRANSACTION_VALIDATING:
        default:
                assert_not_reached("Impossible state");
        }

        dns_query_free(q);
}

static int dns_stub_stream_complete(DnsStream *s, int error) {
        assert(s);

        log_debug_errno(error, "DNS TCP connection terminated, destroying queries: %m");

        for (;;) {
                DnsQuery *q;

                q = set_first(s->queries);
                if (!q)
                        break;

                dns_query_free(q);
        }

        /* This drops the implicit ref we keep around since it was allocated, as incoming stub connections
         * should be kept as long as the client wants to. */
        dns_stream_unref(s);
        return 0;
}

static void dns_stub_process_query(Manager *m, DnsStream *s, DnsPacket *p) {
        _cleanup_(dns_query_freep) DnsQuery *q = NULL;
        int r;

        assert(m);
        assert(p);
        assert(p->protocol == DNS_PROTOCOL_DNS);

        if (in_addr_is_localhost(p->family, &p->sender) <= 0 ||
            in_addr_is_localhost(p->family, &p->destination) <= 0) {
                log_error("Got packet on unexpected IP range, refusing.");
                dns_stub_send_failure(m, s, p, DNS_RCODE_SERVFAIL, false);
                return;
        }

        r = dns_packet_extract(p);
        if (r < 0) {
                log_debug_errno(r, "Failed to extract resources from incoming packet, ignoring packet: %m");
                dns_stub_send_failure(m, s, p, DNS_RCODE_FORMERR, false);
                return;
        }

        if (!DNS_PACKET_VERSION_SUPPORTED(p)) {
                log_debug("Got EDNS OPT field with unsupported version number.");
                dns_stub_send_failure(m, s, p, DNS_RCODE_BADVERS, false);
                return;
        }

        if (dns_type_is_obsolete(p->question->keys[0]->type)) {
                log_debug("Got message with obsolete key type, refusing.");
                dns_stub_send_failure(m, s, p, DNS_RCODE_NOTIMP, false);
                return;
        }

        if (dns_type_is_zone_transer(p->question->keys[0]->type)) {
                log_debug("Got request for zone transfer, refusing.");
                dns_stub_send_failure(m, s, p, DNS_RCODE_NOTIMP, false);
                return;
        }

        if (!DNS_PACKET_RD(p))  {
                /* If the "rd" bit is off (i.e. recursion was not requested), then refuse operation */
                log_debug("Got request with recursion disabled, refusing.");
                dns_stub_send_failure(m, s, p, DNS_RCODE_REFUSED, false);
                return;
        }

        if (DNS_PACKET_DO(p) && DNS_PACKET_CD(p)) {
                log_debug("Got request with DNSSEC CD bit set, refusing.");
                dns_stub_send_failure(m, s, p, DNS_RCODE_NOTIMP, false);
                return;
        }

        r = dns_query_new(m, &q, p->question, p->question, 0, SD_RESOLVED_PROTOCOLS_ALL|SD_RESOLVED_NO_SEARCH);
        if (r < 0) {
                log_error_errno(r, "Failed to generate query object: %m");
                dns_stub_send_failure(m, s, p, DNS_RCODE_SERVFAIL, false);
                return;
        }

        /* Request that the TTL is corrected by the cached time for this lookup, so that we return vaguely useful TTLs */
        q->clamp_ttl = true;

        q->request_dns_packet = dns_packet_ref(p);
        q->request_dns_stream = dns_stream_ref(s); /* make sure the stream stays around until we can send a reply through it */
        q->complete = dns_stub_query_complete;

        if (s) {
                /* Remember which queries belong to this stream, so that we can cancel them when the stream
                 * is disconnected early */

                r = set_ensure_put(&s->queries, NULL, q);
                if (r < 0) {
                        log_oom();
                        return;
                }
                assert(r > 0);
        }

        r = dns_query_go(q);
        if (r < 0) {
                log_error_errno(r, "Failed to start query: %m");
                dns_stub_send_failure(m, s, p, DNS_RCODE_SERVFAIL, false);
                return;
        }

        log_debug("Processing query...");
        TAKE_PTR(q);
}

static int on_dns_stub_packet(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        _cleanup_(dns_packet_unrefp) DnsPacket *p = NULL;
        Manager *m = userdata;
        int r;

        r = manager_recv(m, fd, DNS_PROTOCOL_DNS, &p);
        if (r <= 0)
                return r;

        if (dns_packet_validate_query(p) > 0) {
                log_debug("Got DNS stub UDP query packet for id %u", DNS_PACKET_ID(p));

                dns_stub_process_query(m, NULL, p);
        } else
                log_debug("Invalid DNS stub UDP packet, ignoring.");

        return 0;
}

static int set_dns_stub_common_socket_options(int fd) {
        int r;

        assert(fd >= 0);

        r = setsockopt_int(fd, SOL_SOCKET, SO_REUSEADDR, true);
        if (r < 0)
                return r;

        r = setsockopt_int(fd, IPPROTO_IP, IP_PKTINFO, true);
        if (r < 0)
                return r;

        return setsockopt_int(fd, IPPROTO_IP, IP_RECVTTL, true);
}

static int manager_dns_stub_udp_fd(Manager *m) {
        union sockaddr_union sa = {
                .in.sin_family = AF_INET,
                .in.sin_port = htobe16(53),
                .in.sin_addr.s_addr = htobe32(INADDR_DNS_STUB),
        };
        _cleanup_close_ int fd = -1;
        int r;

        if (m->dns_stub_udp_fd >= 0)
                return m->dns_stub_udp_fd;

        fd = socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
        if (fd < 0)
                return -errno;

        r = set_dns_stub_common_socket_options(fd);
        if (r < 0)
                return r;

        /* Make sure no traffic from outside the local host can leak to onto this socket */
        r = socket_bind_to_ifindex(fd, LOOPBACK_IFINDEX);
        if (r < 0)
                return r;

        if (bind(fd, &sa.sa, sizeof(sa.in)) < 0)
                return -errno;

        r = sd_event_add_io(m->event, &m->dns_stub_udp_event_source, fd, EPOLLIN, on_dns_stub_packet, m);
        if (r < 0)
                return r;

        (void) sd_event_source_set_description(m->dns_stub_udp_event_source, "dns-stub-udp");

        return m->dns_stub_udp_fd = TAKE_FD(fd);
}

static int manager_dns_stub_udp_fd_extra(Manager *m, DNSStubListenerExtra *l) {
        _cleanup_free_ char *pretty = NULL;
        _cleanup_close_ int fd = -1;
        int r;

        if (l->fd >= 0)
                return 0;

        fd = socket(socket_address_family(&l->address), SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
        if (fd < 0) {
                r = -errno;
                goto fail;
        }

        r = setsockopt_int(fd, IPPROTO_IP, IP_FREEBIND, true);
        if (r < 0)
                goto fail;

        r = set_dns_stub_common_socket_options(fd);
        if (r < 0)
                goto fail;

        if (bind(fd, &l->address.sockaddr.sa, l->address.size) < 0) {
                r = -errno;
                goto fail;
        }

        r = sd_event_add_io(m->event, &l->dns_stub_extra_event_source, fd, EPOLLIN, on_dns_stub_packet, m);
        if (r < 0)
                goto fail;

        (void) sd_event_source_set_description(l->dns_stub_extra_event_source, "dns-stub-udp-extra");

        l->fd = TAKE_FD(fd);

        if (DEBUG_LOGGING) {
                (void) sockaddr_pretty(&l->address.sockaddr.sa, FAMILY_ADDRESS_SIZE(l->address.sockaddr.sa.sa_family), true, true, &pretty);
                log_debug("Listening on UDP socket %s.", strnull(pretty));
        }

        return 0;

 fail:
       (void) sockaddr_pretty(&l->address.sockaddr.sa, FAMILY_ADDRESS_SIZE(l->address.sockaddr.sa.sa_family), true, true, &pretty);
       if (r == -EADDRINUSE)
               return log_warning_errno(r,
                                        "Another process is already listening on UDP socket %s.\n"
                                        "Turning off local DNS stub extra support.", strnull(pretty));
       if (r == -EPERM)
               return log_warning_errno(r,
                                        "Failed to listen on UDP socket %s: %m.\n"
                                        "Turning off local DNS stub extra support.", strnull(pretty));

       assert(r < 0);

       return log_warning_errno(r, "Failed to listen on UDP socket %s, ignoring: %m", strnull(pretty));
}

static int on_dns_stub_stream_packet(DnsStream *s) {
        _cleanup_(dns_packet_unrefp) DnsPacket *p = NULL;

        assert(s);

        p = dns_stream_take_read_packet(s);
        assert(p);

        if (dns_packet_validate_query(p) > 0) {
                log_debug("Got DNS stub TCP query packet for id %u", DNS_PACKET_ID(p));

                dns_stub_process_query(s->manager, s, p);
        } else
                log_debug("Invalid DNS stub TCP packet, ignoring.");

        return 0;
}

static int on_dns_stub_stream(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        DnsStream *stream;
        Manager *m = userdata;
        int cfd, r;

        cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK|SOCK_CLOEXEC);
        if (cfd < 0) {
                if (ERRNO_IS_ACCEPT_AGAIN(errno))
                        return 0;

                return -errno;
        }

        r = dns_stream_new(m, &stream, DNS_STREAM_STUB, DNS_PROTOCOL_DNS, cfd, NULL);
        if (r < 0) {
                safe_close(cfd);
                return r;
        }

        stream->on_packet = on_dns_stub_stream_packet;
        stream->complete = dns_stub_stream_complete;

        /* We let the reference to the stream dangle here, it will be dropped later by the complete callback. */

        return 0;
}

static int manager_dns_stub_tcp_fd(Manager *m) {
        union sockaddr_union sa = {
                .in.sin_family = AF_INET,
                .in.sin_addr.s_addr = htobe32(INADDR_DNS_STUB),
                .in.sin_port = htobe16(53),
        };
        _cleanup_close_ int fd = -1;
        int r;

        if (m->dns_stub_tcp_fd >= 0)
                return m->dns_stub_tcp_fd;

        fd = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
        if (fd < 0)
                return -errno;

        r = set_dns_stub_common_socket_options(fd);
        if (r < 0)
                return r;

        r = setsockopt_int(fd, IPPROTO_IP, IP_TTL, 1);
        if (r < 0)
                return r;

        /* Make sure no traffic from outside the local host can leak to onto this socket */
        r = socket_bind_to_ifindex(fd, LOOPBACK_IFINDEX);
        if (r < 0)
                return r;

        if (bind(fd, &sa.sa, sizeof(sa.in)) < 0)
                return -errno;

        if (listen(fd, SOMAXCONN) < 0)
                return -errno;

        r = sd_event_add_io(m->event, &m->dns_stub_tcp_event_source, fd, EPOLLIN, on_dns_stub_stream, m);
        if (r < 0)
                return r;

        (void) sd_event_source_set_description(m->dns_stub_tcp_event_source, "dns-stub-tcp");

        return m->dns_stub_tcp_fd = TAKE_FD(fd);
}

static int manager_dns_stub_tcp_fd_extra(Manager *m, DNSStubListenerExtra *l) {
        _cleanup_free_ char *pretty = NULL;
        _cleanup_close_ int fd = -1;
        int r;

        if (l->fd >= 0)
                return 0;

        fd = socket(socket_address_family(&l->address), SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
        if (fd < 0) {
                r = -errno;
                goto fail;
        }

        r = set_dns_stub_common_socket_options(fd);
        if (r < 0)
                goto fail;

        r = setsockopt_int(fd, IPPROTO_IP, IP_TTL, 1);
        if (r < 0)
                goto fail;

        r = setsockopt_int(fd, IPPROTO_IP, IP_FREEBIND, true);
        if (r < 0)
                goto fail;

        if (bind(fd, &l->address.sockaddr.sa, l->address.size) < 0) {
                r = -errno;
                goto fail;
        }

        if (listen(fd, SOMAXCONN) < 0) {
                r = -errno;
                goto fail;
        }

        r = sd_event_add_io(m->event, &l->dns_stub_extra_event_source, fd, EPOLLIN, on_dns_stub_packet, m);
        if (r < 0)
                goto fail;

        (void) sd_event_source_set_description(l->dns_stub_extra_event_source, "dns-stub-tcp-extra");

        l->fd = TAKE_FD(fd);

        if (DEBUG_LOGGING) {
                (void) sockaddr_pretty(&l->address.sockaddr.sa, FAMILY_ADDRESS_SIZE(l->address.sockaddr.sa.sa_family), true, true, &pretty);
                log_debug("Listening on TCP socket %s.", strnull(pretty));
        }

        return 0;

 fail:
       (void) sockaddr_pretty(&l->address.sockaddr.sa, FAMILY_ADDRESS_SIZE(l->address.sockaddr.sa.sa_family), true, true, &pretty);
       if (r == -EADDRINUSE)
               return log_warning_errno(r,
                                        "Another process is already listening on TCP socket %s.\n"
                                        "Turning off local DNS stub extra support.", strnull(pretty));
       if (r == -EPERM)
               return log_warning_errno(r,
                                        "Failed to listen on TCP socket %s: %m.\n"
                                        "Turning off local DNS stub extra support.", strnull(pretty));

       assert(r < 0);

       return log_warning_errno(r, "Failed to listen on TCP socket %s, ignoring: %m", strnull(pretty));
}

int manager_dns_stub_start(Manager *m) {
        const char *t = "UDP";
        int r = 0;

        assert(m);

        if (m->dns_stub_listener_mode == DNS_STUB_LISTENER_NO)
                log_debug("Not creating stub listener.");
        else
                log_debug("Creating stub listener using %s.",
                          m->dns_stub_listener_mode == DNS_STUB_LISTENER_UDP ? "UDP" :
                          m->dns_stub_listener_mode == DNS_STUB_LISTENER_TCP ? "TCP" :
                          "UDP/TCP");

        if (IN_SET(m->dns_stub_listener_mode, DNS_STUB_LISTENER_YES, DNS_STUB_LISTENER_UDP))
                r = manager_dns_stub_udp_fd(m);

        if (r >= 0 &&
            IN_SET(m->dns_stub_listener_mode, DNS_STUB_LISTENER_YES, DNS_STUB_LISTENER_TCP)) {
                t = "TCP";
                r = manager_dns_stub_tcp_fd(m);
        }

        if (IN_SET(r, -EADDRINUSE, -EPERM)) {
                if (r == -EADDRINUSE)
                        log_warning_errno(r,
                                          "Another process is already listening on %s socket 127.0.0.53:53.\n"
                                          "Turning off local DNS stub support.", t);
                else
                        log_warning_errno(r,
                                          "Failed to listen on %s socket 127.0.0.53:53: %m.\n"
                                          "Turning off local DNS stub support.", t);
                manager_dns_stub_stop(m);
        } else if (r < 0)
                return log_error_errno(r, "Failed to listen on %s socket 127.0.0.53:53: %m", t);

        if (!ordered_set_isempty(m->dns_extra_stub_listeners)) {
                DNSStubListenerExtra *l;
                Iterator i;

                log_debug("Creating stub listener extra using %s.",
                          m->dns_stub_listener_mode == DNS_STUB_LISTENER_UDP ? "UDP" :
                          m->dns_stub_listener_mode == DNS_STUB_LISTENER_TCP ? "TCP" :
                          "UDP/TCP");

                ORDERED_SET_FOREACH(l, m->dns_extra_stub_listeners, i)
                        if (l->mode == DNS_STUB_LISTENER_UDP)
                                (void) manager_dns_stub_udp_fd_extra(m, l);
                        else
                                (void) manager_dns_stub_tcp_fd_extra(m, l);
        }

        return 0;
}

void manager_dns_stub_stop(Manager *m) {
        assert(m);

        m->dns_stub_udp_event_source = sd_event_source_unref(m->dns_stub_udp_event_source);
        m->dns_stub_tcp_event_source = sd_event_source_unref(m->dns_stub_tcp_event_source);

        m->dns_stub_udp_fd = safe_close(m->dns_stub_udp_fd);
        m->dns_stub_tcp_fd = safe_close(m->dns_stub_tcp_fd);
}

void manager_dns_stub_stop_extra(Manager *m) {
        DNSStubListenerExtra *l;
        Iterator i;

        assert(m);

        ORDERED_SET_FOREACH(l, m->dns_extra_stub_listeners, i) {
                l->dns_stub_extra_event_source = sd_event_source_unref(l->dns_stub_extra_event_source);
                l->fd = safe_close(l->fd);
        }
}
