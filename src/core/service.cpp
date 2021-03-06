/*
 * This file is part of the trojan project.
 * Trojan is an unidentifiable mechanism that helps you bypass GFW.
 * Copyright (C) 2017-2020  The Trojan Authors.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "service.h"
#include <cstring>
#include <cerrno>
#include <stdexcept>
#include <fstream>
#ifdef _WIN32
#include <wincrypt.h>
#include <tchar.h>
#endif // _WIN32
#ifdef __APPLE__
#include <Security/Security.h>
#endif // __APPLE__
#include <openssl/opensslv.h>
#include "session/serversession.h"
#include "session/clientsession.h"
#include "session/forwardsession.h"
#include "session/natsession.h"
#include "ssl/ssldefaults.h"
#include "ssl/sslsession.h"
using namespace std;
using namespace boost::asio::ip;
using namespace boost::asio::ssl;

#ifdef ENABLE_REUSE_PORT
typedef boost::asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT> reuse_port;
#endif // ENABLE_REUSE_PORT

// copied from shadowsocks-libe udprelay.h
#ifndef IP_TRANSPARENT
#define IP_TRANSPARENT       19
#endif

#ifndef IP_RECVORIGDSTADDR
#ifdef  IP_ORIGDSTADDR
#define IP_RECVORIGDSTADDR   IP_ORIGDSTADDR
#else
#define IP_RECVORIGDSTADDR   20
#endif
#endif

#ifndef IPV6_RECVORIGDSTADDR
#ifdef  IPV6_ORIGDSTADDR
#define IPV6_RECVORIGDSTADDR   IPV6_ORIGDSTADDR
#else
#define IPV6_RECVORIGDSTADDR   74
#endif
#endif

#ifndef SOL_IP
#define SOL_IP  IPPROTO_IP
#endif

#ifndef SOL_IPV6
#define SOL_IPV6  IPPROTO_IPV6
#endif

#define PACKET_HEADER_SIZE (1 + 28 + 2 + 64)
#define DEFAULT_PACKET_SIZE 1397 // 1492 - PACKET_HEADER_SIZE = 1397, the default MTU for UDP relay

#ifndef _WIN32  // nat mode does not support in windows platform
// copied from shadowsocks-libev udpreplay.c
static int get_dstaddr(struct msghdr *msg, struct sockaddr_storage *dstaddr)
{
    struct cmsghdr *cmsg;

    for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVORIGDSTADDR) {
            memcpy(dstaddr, CMSG_DATA(cmsg), sizeof(struct sockaddr_in));
            dstaddr->ss_family = AF_INET;
            return 0;
        } else if (cmsg->cmsg_level == SOL_IPV6 && cmsg->cmsg_type == IPV6_RECVORIGDSTADDR) {
            memcpy(dstaddr, CMSG_DATA(cmsg), sizeof(struct sockaddr_in6));
            dstaddr->ss_family = AF_INET6;
            return 0;
        }
    }

    return 1;
}

static pair<string, uint16_t> get_addr(struct sockaddr_storage addr){
    
    const int buf_size = 256;
    char buf[256];

    if(addr.ss_family == AF_INET){
        sockaddr_in *sa = (sockaddr_in*) &addr;
        if(inet_ntop(AF_INET, &(sa->sin_addr), buf, buf_size)){
            return make_pair(buf, ntohs(sa->sin_port));
        }             
    }else{
        sockaddr_in6 *sa = (sockaddr_in6*) &addr;
        if(inet_ntop(AF_INET6, &(sa->sin6_addr), buf, buf_size)){
            return make_pair(buf, ntohs(sa->sin6_port));
        }                
    }

    return make_pair("", 0);
}

// copied from shadowsocks-libev udpreplay.c
// it works if in NAT mode
static pair<string, uint16_t> recv_tproxy_udp_msg(int fd, boost::asio::ip::udp::endpoint& recv_endpoint, char* buf, int& buf_len){
    struct sockaddr_storage src_addr;
    memset(&src_addr, 0, sizeof(struct sockaddr_storage));

    char control_buffer[64] = { 0 };
    struct msghdr msg;
    memset(&msg, 0, sizeof(struct msghdr));
    struct iovec iov[1];
    struct sockaddr_storage dst_addr;
    memset(&dst_addr, 0, sizeof(struct sockaddr_storage));

    msg.msg_name       = &src_addr;
    msg.msg_namelen    = sizeof(struct sockaddr_storage);;
    msg.msg_control    = control_buffer;
    msg.msg_controllen = sizeof(control_buffer);

    const int packet_size = DEFAULT_PACKET_SIZE;
    const int buf_size = DEFAULT_PACKET_SIZE * 2;

    iov[0].iov_base = buf;
    iov[0].iov_len  = buf_size;
    msg.msg_iov     = iov;
    msg.msg_iovlen  = 1;

    buf_len = recvmsg(fd, &msg, 0);
    if (buf_len == -1) {
        Log::log("[udp] server_recvmsg failed!", Log::FATAL);
    } else{
        if (buf_len > packet_size) {
            Log::log(string("[udp] UDP server_recv_recvmsg fragmentation, MTU at least be: ") + to_string(buf_len + PACKET_HEADER_SIZE), Log::INFO);
        }

        if (get_dstaddr(&msg, &dst_addr)) {
            Log::log("[udp] unable to get dest addr!", Log::FATAL);
        }else{
            auto target_dst = get_addr(dst_addr);       
            auto src_dst = get_addr(src_addr);
            recv_endpoint.address(boost::asio::ip::make_address(src_dst.first));
            recv_endpoint.port(src_dst.second); 
            return target_dst;           
        }
    }    

    return make_pair("", 0);
}
#endif

Service::Service(Config &config, bool test) :
    config(config),
    socket_acceptor(io_context),
    ssl_context(context::sslv23),
    auth(nullptr),
    udp_socket(io_context) {
#ifndef ENABLE_NAT
    if (config.run_type == Config::NAT) {
        throw runtime_error("NAT is not supported");
    }
#endif // ENABLE_NAT
    if (!test) {
        tcp::resolver resolver(io_context);
        tcp::endpoint listen_endpoint = *resolver.resolve(config.local_addr, to_string(config.local_port)).begin();
        socket_acceptor.open(listen_endpoint.protocol());
        socket_acceptor.set_option(tcp::acceptor::reuse_address(true));

        if (config.tcp.reuse_port) {
#ifdef ENABLE_REUSE_PORT
            socket_acceptor.set_option(reuse_port(true));
#else  // ENABLE_REUSE_PORT
            Log::log_with_date_time("SO_REUSEPORT is not supported", Log::WARN);
#endif // ENABLE_REUSE_PORT
        }

        socket_acceptor.bind(listen_endpoint);
        socket_acceptor.listen();
        if (config.run_type == Config::FORWARD || config.run_type == Config::NAT) {
            auto udp_bind_endpoint = udp::endpoint(listen_endpoint.address(), listen_endpoint.port());
            auto udp_protocol = udp_bind_endpoint.protocol();
            udp_socket.open(udp_protocol);
            
            if(config.run_type == Config::NAT){
#ifdef _WIN32
                throw runtime_error("NAT is not supported in Windows");
#else
                // copy from shadowsocks-libev
                int opt = 1;
                int fd = udp_socket.native_handle();
                int sol;
                int ip_recv;

                if(udp_protocol.family() == boost::asio::ip::tcp::v4().family()){
                    sol = SOL_IP;
                    ip_recv = IP_RECVORIGDSTADDR;
                }else if (udp_protocol.family() == boost::asio::ip::tcp::v6().family()) {
                    sol = SOL_IPV6;
                    ip_recv = IPV6_RECVORIGDSTADDR;
                }else{
                    Log::log("[udp] protocol can't be recognized", Log::FATAL);
                    stop();
                    return;
                }

                if (setsockopt(fd, sol, IP_TRANSPARENT, &opt, sizeof(opt))) {
                    Log::log("[udp] setsockopt IP_TRANSPARENT failed!", Log::FATAL);
                    stop();
                    return;
                }

                if (setsockopt(fd, sol, ip_recv, &opt, sizeof(opt))) {
                    Log::log("[udp] setsockopt IP_RECVORIGDSTADDR failed!", Log::FATAL);
                    stop();
                    return;
                }

                if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
                    Log::log( "[udp] setsockopt SO_REUSEADDR failed!", Log::FATAL);
                    stop();
                    return;
                }
#endif // _WIN32

            }

            udp_socket.bind(udp_bind_endpoint);
        }
    }
    Log::level = config.log_level;
    auto native_context = ssl_context.native_handle();
    ssl_context.set_options(context::default_workarounds | context::no_sslv2 | context::no_sslv3 | context::single_dh_use);
    if (!config.ssl.curves.empty()) {
        SSL_CTX_set1_curves_list(native_context, config.ssl.curves.c_str());
    }
    if (config.run_type == Config::SERVER) {
        ssl_context.use_certificate_chain_file(config.ssl.cert);
        ssl_context.set_password_callback([this](size_t, context_base::password_purpose) {
            return this->config.ssl.key_password;
        });
        ssl_context.use_private_key_file(config.ssl.key, context::pem);
        if (config.ssl.prefer_server_cipher) {
            SSL_CTX_set_options(native_context, SSL_OP_CIPHER_SERVER_PREFERENCE);
        }
        if (!config.ssl.alpn.empty()) {
            SSL_CTX_set_alpn_select_cb(native_context, [](SSL*, const unsigned char **out, unsigned char *outlen, const unsigned char *in, unsigned int inlen, void *config) -> int {
                if (SSL_select_next_proto((unsigned char**)out, outlen, (unsigned char*)(((Config*)config)->ssl.alpn.c_str()), ((Config*)config)->ssl.alpn.length(), in, inlen) != OPENSSL_NPN_NEGOTIATED) {
                    return SSL_TLSEXT_ERR_NOACK;
                }
                return SSL_TLSEXT_ERR_OK;
            }, &config);
        }
        if (config.ssl.reuse_session) {
            SSL_CTX_set_timeout(native_context, config.ssl.session_timeout);
            if (!config.ssl.session_ticket) {
                SSL_CTX_set_options(native_context, SSL_OP_NO_TICKET);
            }
        } else {
            SSL_CTX_set_session_cache_mode(native_context, SSL_SESS_CACHE_OFF);
            SSL_CTX_set_options(native_context, SSL_OP_NO_TICKET);
        }
        if (!config.ssl.plain_http_response.empty()) {
            ifstream ifs(config.ssl.plain_http_response, ios::binary);
            if (!ifs.is_open()) {
                throw runtime_error(config.ssl.plain_http_response + ": " + strerror(errno));
            }
            plain_http_response = string(istreambuf_iterator<char>(ifs), istreambuf_iterator<char>());
        }
        if (config.ssl.dhparam.empty()) {
            ssl_context.use_tmp_dh(boost::asio::const_buffer(SSLDefaults::g_dh2048_sz, SSLDefaults::g_dh2048_sz_size));
        } else {
            ssl_context.use_tmp_dh_file(config.ssl.dhparam);
        }
        if (config.mysql.enabled) {
#ifdef ENABLE_MYSQL
            auth = new Authenticator(config);
#else // ENABLE_MYSQL
            Log::log_with_date_time("MySQL is not supported", Log::WARN);
#endif // ENABLE_MYSQL
        }
    } else {
        if (config.ssl.sni.empty()) {
            config.ssl.sni = config.remote_addr;
        }
        if (config.ssl.verify) {
            ssl_context.set_verify_mode(verify_peer);
            if (config.ssl.cert.empty()) {
                ssl_context.set_default_verify_paths();
#ifdef _WIN32
                HCERTSTORE h_store = CertOpenSystemStore(0, _T("ROOT"));
                if (h_store) {
                    X509_STORE *store = SSL_CTX_get_cert_store(native_context);
                    PCCERT_CONTEXT p_context = NULL;
                    while ((p_context = CertEnumCertificatesInStore(h_store, p_context))) {
                        const unsigned char *encoded_cert = p_context->pbCertEncoded;
                        X509 *x509 = d2i_X509(NULL, &encoded_cert, p_context->cbCertEncoded);
                        if (x509) {
                            X509_STORE_add_cert(store, x509);
                            X509_free(x509);
                        }
                    }
                    CertCloseStore(h_store, 0);
                }
#endif // _WIN32
#ifdef __APPLE__
                SecKeychainSearchRef pSecKeychainSearch = NULL;
                SecKeychainRef pSecKeychain;
                OSStatus status = noErr;
                X509 *cert = NULL;

                // Leopard and above store location
                status = SecKeychainOpen ("/System/Library/Keychains/SystemRootCertificates.keychain", &pSecKeychain);
                if (status == noErr) {
                    X509_STORE *store = SSL_CTX_get_cert_store(native_context);
                    status = SecKeychainSearchCreateFromAttributes (pSecKeychain, kSecCertificateItemClass, NULL, &pSecKeychainSearch);
                     for (;;) {
                        SecKeychainItemRef pSecKeychainItem = nil;

                        status = SecKeychainSearchCopyNext (pSecKeychainSearch, &pSecKeychainItem);
                        if (status == errSecItemNotFound) {
                            break;
                        }

                        if (status == noErr) {
                            void *_pCertData;
                            UInt32 _pCertLength;
                            status = SecKeychainItemCopyAttributesAndData (pSecKeychainItem, NULL, NULL, NULL, &_pCertLength, &_pCertData);

                            if (status == noErr && _pCertData != NULL) {
                                unsigned char *ptr;

                                ptr = (unsigned char *)_pCertData;       /*required because d2i_X509 is modifying pointer */
                                cert = d2i_X509 (NULL, (const unsigned char **) &ptr, _pCertLength);
                                if (cert == NULL) {
                                    continue;
                                }

                                if (!X509_STORE_add_cert (store, cert)) {
                                    X509_free (cert);
                                    continue;
                                }
                                X509_free (cert);

                                status = SecKeychainItemFreeAttributesAndData (NULL, _pCertData);
                            }
                        }
                        if (pSecKeychainItem != NULL) {
                            CFRelease (pSecKeychainItem);
                        }
                    }
                    CFRelease (pSecKeychainSearch);
                    CFRelease (pSecKeychain);
                }
#endif // __APPLE__
            } else {
                ssl_context.load_verify_file(config.ssl.cert);
            }
            if (config.ssl.verify_hostname) {
                ssl_context.set_verify_callback(rfc2818_verification(config.ssl.sni));
            }
            X509_VERIFY_PARAM *param = X509_VERIFY_PARAM_new();
            X509_VERIFY_PARAM_set_flags(param, X509_V_FLAG_PARTIAL_CHAIN);
            SSL_CTX_set1_param(native_context, param);
            X509_VERIFY_PARAM_free(param);
        } else {
            ssl_context.set_verify_mode(verify_none);
        }
        if (!config.ssl.alpn.empty()) {
            SSL_CTX_set_alpn_protos(native_context, (unsigned char*)(config.ssl.alpn.c_str()), config.ssl.alpn.length());
        }
        if (config.ssl.reuse_session) {
            SSL_CTX_set_session_cache_mode(native_context, SSL_SESS_CACHE_CLIENT);
            SSLSession::set_callback(native_context);
            if (!config.ssl.session_ticket) {
                SSL_CTX_set_options(native_context, SSL_OP_NO_TICKET);
            }
        } else {
            SSL_CTX_set_options(native_context, SSL_OP_NO_TICKET);
        }
    }
    if (!config.ssl.cipher.empty()) {
        SSL_CTX_set_cipher_list(native_context, config.ssl.cipher.c_str());
    }
    if (!config.ssl.cipher_tls13.empty()) {
#ifdef ENABLE_TLS13_CIPHERSUITES
        SSL_CTX_set_ciphersuites(native_context, config.ssl.cipher_tls13.c_str());
#else  // ENABLE_TLS13_CIPHERSUITES
        Log::log_with_date_time("TLS1.3 ciphersuites are not supported", Log::WARN);
#endif // ENABLE_TLS13_CIPHERSUITES
    }

    if (!test) {
        if (config.tcp.no_delay) {
            socket_acceptor.set_option(tcp::no_delay(true));
        }
        if (config.tcp.keep_alive) {
            socket_acceptor.set_option(boost::asio::socket_base::keep_alive(true));
        }
        if (config.tcp.fast_open) {
#ifdef TCP_FASTOPEN
            using fastopen = boost::asio::detail::socket_option::integer<IPPROTO_TCP, TCP_FASTOPEN>;
            boost::system::error_code ec;
            socket_acceptor.set_option(fastopen(config.tcp.fast_open_qlen), ec);
#else // TCP_FASTOPEN
            Log::log_with_date_time("TCP_FASTOPEN is not supported", Log::WARN);
#endif // TCP_FASTOPEN
#ifndef TCP_FASTOPEN_CONNECT
            Log::log_with_date_time("TCP_FASTOPEN_CONNECT is not supported", Log::WARN);
#endif // TCP_FASTOPEN_CONNECT
        }
    }
    if (Log::keylog) {
#ifdef ENABLE_SSL_KEYLOG
        SSL_CTX_set_keylog_callback(native_context, [](const SSL*, const char *line) {
            fprintf(Log::keylog, "%s\n", line);
            fflush(Log::keylog);
        });
#else // ENABLE_SSL_KEYLOG
        Log::log_with_date_time("SSL KeyLog is not supported", Log::WARN);
#endif // ENABLE_SSL_KEYLOG
    }
}

void Service::run() {
    async_accept();
    if (config.run_type == Config::FORWARD || config.run_type == Config::NAT) {
        udp_async_read();
    }
    tcp::endpoint local_endpoint = socket_acceptor.local_endpoint();
    string rt;
    if (config.run_type == Config::SERVER) {
        rt = "server";
    } else if (config.run_type == Config::FORWARD) {
        rt = "forward";
    } else if (config.run_type == Config::NAT) {
        rt = "nat";
    } else {
        rt = "client";
    }
    Log::log_with_date_time(string("trojan service (") + rt + ") started at " + local_endpoint.address().to_string() + ':' + to_string(local_endpoint.port()), Log::WARN);
    io_context.run();
    Log::log_with_date_time("trojan service stopped", Log::WARN);
}

void Service::stop() {
    boost::system::error_code ec;
    socket_acceptor.cancel(ec);
    if (udp_socket.is_open()) {
        udp_socket.cancel(ec);
        udp_socket.close(ec);
    }
    io_context.stop();
}

void Service::async_accept() {
    shared_ptr<Session>session(nullptr);
    if (config.run_type == Config::SERVER) {
        session = make_shared<ServerSession>(config, io_context, ssl_context, auth, plain_http_response);
    } else if (config.run_type == Config::FORWARD) {
        session = make_shared<ForwardSession>(config, io_context, ssl_context);
    } else if (config.run_type == Config::NAT) {
        session = make_shared<NATSession>(config, io_context, ssl_context);
    } else {
        session = make_shared<ClientSession>(config, io_context, ssl_context);
    }
    socket_acceptor.async_accept(session->accept_socket(), [this, session](const boost::system::error_code error) {
        if (error == boost::asio::error::operation_aborted) {
            // got cancel signal, stop calling myself
            return;
        }
        if (!error) {
            boost::system::error_code ec;
            auto endpoint = session->accept_socket().remote_endpoint(ec);
            if (!ec) {
                Log::log_with_endpoint(endpoint, "incoming connection");
                session->start();
            }
        }
        async_accept();
    });
}

void Service::udp_async_read() {
    auto cb = [this](const boost::system::error_code error, size_t length) {
        if (error == boost::asio::error::operation_aborted) {
            // got cancel signal, stop calling myself
            return;
        }
        if (error) {
            stop();
            throw runtime_error(error.message());
        }
        
        pair<string,uint16_t> targetdst;
        
        if(config.run_type == Config::NAT){
#ifdef _WIN32  // windows cannot support nat mode
            targetdst = make_pair("", 0);
#else
            int read_length = (int)length;
            targetdst = recv_tproxy_udp_msg(udp_socket.native_handle(), udp_recv_endpoint, (char*)udp_read_buf, read_length);
            length = read_length < 0 ? 0 : read_length;
#endif
        }else{
            targetdst = make_pair(config.target_addr, config.target_port);
        }

        if(targetdst.second != 0){                
            string data((const char *)udp_read_buf, length);
            for (auto it = udp_sessions.begin(); it != udp_sessions.end();) {
                auto next = ++it;
                --it;
                if (it->expired()) {
                    udp_sessions.erase(it);
                } else if (it->lock()->process(udp_recv_endpoint, data)) {
                    udp_async_read();
                    return;
                }
                it = next;
            }
            
            Log::log_with_endpoint(udp_recv_endpoint, "new UDP session");
            auto session = make_shared<UDPForwardSession>(config, io_context, ssl_context, udp_recv_endpoint, targetdst, 
             [this](const udp::endpoint &endpoint, const pair<string, uint16_t>& target, const string &data) {
                boost::system::error_code ec;

                if(config.run_type == Config::NAT){
#ifdef _WIN32
                    throw runtime_error("NAT is not supported!" + target.first);
#else
                    auto target_endpoint = udp::endpoint(boost::asio::ip::make_address(target.first), target.second);
                    auto new_udp_socket = udp::socket(io_context);
                    new_udp_socket.open(target_endpoint.protocol());

                    bool succ = true;
                    int opt = 1;
                    int fd = new_udp_socket.native_handle();
                    int sol = endpoint.protocol().family() == boost::asio::ip::tcp::v6().family() ? SOL_IPV6 : SOL_IP;
                    if (setsockopt(fd, sol, IP_TRANSPARENT, &opt, sizeof(opt))) {
                        Log::log_with_endpoint(target_endpoint, "[udp] setsockopt IP_TRANSPARENT failed!", Log::FATAL);
                        succ = false;
                    }
                    
                    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
                        Log::log_with_endpoint(target_endpoint, "[udp] setsockopt SO_REUSEADDR failed!", Log::FATAL);
                        succ = false;
                    }
                    
                    if(succ){
                        new_udp_socket.bind(target_endpoint);
                        new_udp_socket.send_to(boost::asio::buffer(data), endpoint,0,ec);
                    }                   
                    
                    new_udp_socket.close(ec);
#endif // _WIN32
                }else{
                    udp_socket.send_to(boost::asio::buffer(data), endpoint, 0, ec);
                }

                if (ec == boost::asio::error::no_permission) {
                    Log::log_with_endpoint(udp_recv_endpoint, "dropped a UDP packet due to firewall policy or rate limit");
                } else if (ec) {
                    throw runtime_error(ec.message());
                }             
            });
            udp_sessions.emplace_back(session);
            session->start();
            session->process(udp_recv_endpoint, data);    
        }else{
            Log::Log::log_with_endpoint(udp_recv_endpoint, "cannot read original destination address!");
        }

        udp_async_read();        
    };

    if(config.run_type == Config::NAT){
        udp_socket.async_receive_from(boost::asio::null_buffers(), udp_recv_endpoint, cb);
    }else{
        udp_socket.async_receive_from(boost::asio::buffer(udp_read_buf, MAX_LENGTH), udp_recv_endpoint, cb);
    }    
}

boost::asio::io_context &Service::service() {
    return io_context;
}

void Service::reload_cert() {
    if (config.run_type == Config::SERVER) {
        Log::log_with_date_time("reloading certificate and private key. . . ", Log::WARN);
        ssl_context.use_certificate_chain_file(config.ssl.cert);
        ssl_context.use_private_key_file(config.ssl.key, context::pem);
        boost::system::error_code ec;
        socket_acceptor.cancel(ec);
        async_accept();
        Log::log_with_date_time("certificate and private key reloaded", Log::WARN);
    } else {
        Log::log_with_date_time("cannot reload certificate and private key: wrong run_type", Log::ERROR);
    }
}

Service::~Service() {
    if (auth) {
        delete auth;
        auth = nullptr;
    }
}
