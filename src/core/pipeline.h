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

#ifndef _PIPELINE_H_
#define _PIPELINE_H_

#include <set>
#include <memory>
#include <functional>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>
#include "proto/pipelinerequest.h"
#include "core/config.h"


class Pipeline : public std::enable_shared_from_this<Pipeline> {
public:
    typedef std::function<void(boost::system::error_code ec, uint32_t session_id, const std::string& data)> RecvHandler;
    typedef std::function<void()> DestroyHandler;
private:
    enum {
        MAX_LENGTH = 8192
    };

    bool destroyed;
    const Config& config;
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket>out_socket;
    RecvHandler recv_handler;
    bool connected;
    uint64_t sent_data_length;
    char out_read_buf[MAX_LENGTH];
    std::string out_read_data;
    std::string cache_out_send_data;
    std::function<void(boost::system::error_code ec)> cache_out_sent_handler;
    boost::asio::ip::tcp::resolver resolver; 
    std::vector<std::weak_ptr<Session>> sessions;

    void out_async_recv();
    void async_send_data(const std::string& data, std::function<void(boost::system::error_code ec)> sent_handler);
    void async_send_cmd(PipelineRequest::Command cmd,uint32_t session_id, const std::string& send_data, std::function<void(boost::system::error_code ec)> sent_handler);
public:
    Pipeline(const Config& config, boost::asio::io_context& io_context, boost::asio::ssl::context &ssl_context);
    void start();
    void destroy();

    void set_recv_handler(RecvHandler handler){ recv_handler = handler;}
    uint64_t get_sent_data_length()const{ return sent_data_length; }

    void session_start(uint32_t session_id,  std::function<void(boost::system::error_code ec)> started_handler);
    void session_async_send(uint32_t session_id, const std::string& send_data, std::function<void(boost::system::error_code ec)> sent_handler);
    void session_destroyed(uint32_t session_id);

    inline bool is_connected()const { return connected; }
    inline bool is_in_pipeline(Session& session)const;
    
};

#endif // _PIPELINE_H_
