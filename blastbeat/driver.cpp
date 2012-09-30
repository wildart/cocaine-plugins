/*
    Copyright (c) 2011-2012 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2012 Other contributors as noted in the AUTHORS file.

    This file is part of Cocaine.

    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>. 
*/

#include <boost/format.hpp>

#include <cocaine/context.hpp>
#include <cocaine/engine.hpp>
#include <cocaine/logging.hpp>

#include "driver.hpp"
#include "job.hpp"

using namespace cocaine;
using namespace cocaine::driver;

blastbeat_t::blastbeat_t(context_t& context, engine::engine_t& engine, const std::string& name, const Json::Value& args):
    category_type(context, engine, name, args),
    m_context(context),
    m_log(context.log(
        (boost::format("app/%1%")
            % name
        ).str()
    )),
    m_event(args.get("emit", "").asString()),
    m_identity(args.get("identity", "").asString()),
    m_endpoint(args.get("endpoint", "").asString()),
    m_watcher(engine.loop()),
    m_processor(engine.loop()),
    m_check(engine.loop()),
    m_socket(context, ZMQ_DEALER)
{
    int linger = 0;
    m_socket.setsockopt(ZMQ_LINGER, &linger, sizeof(linger));
    
    try {
        m_socket.setsockopt(
            ZMQ_IDENTITY,
            m_identity.data(),
            m_identity.size()
        );
    } catch(const zmq::error_t& e) {
        boost::format message("invalid driver identity - %s");
        throw configuration_error_t((message % e.what()).str());
    }
    
    try {
        m_socket.connect(m_endpoint);
    } catch(const zmq::error_t& e) {
        boost::format message("invalid driver endpoint - %s");
        throw configuration_error_t((message % e.what()).str());
    }

    m_watcher.set<blastbeat_t, &blastbeat_t::event>(this);
    m_watcher.start(m_socket.fd(), ev::READ);
    m_processor.set<blastbeat_t, &blastbeat_t::process>(this);
    m_check.set<blastbeat_t, &blastbeat_t::check>(this);
    m_check.start();
}

blastbeat_t::~blastbeat_t() {
    m_watcher.stop();
    m_processor.stop();
    m_check.stop();
}

Json::Value
blastbeat_t::info() const {
    Json::Value result;

    result["identity"] = m_identity;
    result["endpoint"] = m_endpoint;
    result["type"] = "blastbeat";

    return result;
}

namespace {
    typedef boost::tuple<
        io::raw<std::string>,
        io::raw<std::string>,
        zmq::message_t*
    > request_proxy_t;
        
    struct uwsgi_header_t {
        uint8_t  modifier1;
        uint16_t datasize;
        uint8_t  modifier2;
    } __attribute__((__packed__));
}

void
blastbeat_t::process(ev::idle&,
                     int)
{
    int counter = defaults::io_bulk_size;
    
    std::string sid,
                type;

    zmq::message_t message;
    
    do {
        if(!m_socket.pending()) {
            m_processor.stop();
            return;
        }   
     
        request_proxy_t proxy(
            io::protect(sid),
            io::protect(type),
            &message
        );

        try {
            m_socket.recv_multi(proxy);
        } catch(const std::runtime_error& e) {
            m_log->error(
                "received a corrupted blastbeat request, %s",
                e.what()
            );

            m_socket.drop();
            
            return;
        }

        m_log->debug(
            "received a blastbeat request, type: '%s', body size: %d bytes",
            type.c_str(),
            message.size()
        );

        if(type == "ping") {
            on_ping();
        } else if(type == "spawn") {
            on_spawn();
        } else if(type == "uwsgi") {
            on_uwsgi(sid, message);
        } else if(type == "body") {
            on_body(sid, message);
        } else if(type == "end") {
            on_end(sid);
        } else {
            m_log->warning(
                "received an unknown message type '%s'",
                type.c_str()
            );
        }
    } while(--counter);
}

void
blastbeat_t::event(ev::io&, 
                   int)
{
    if(m_socket.pending() && !m_processor.is_active()) {
        m_processor.start();
    }
}

void
blastbeat_t::check(ev::prepare&, 
                   int)
{
    event(m_watcher, ev::READ);
}

void
blastbeat_t::on_ping() {
    std::string empty,
                response("pong");

    m_socket.send(io::protect(empty), ZMQ_SNDMORE);
    m_socket.send(io::protect(response), ZMQ_SNDMORE);
    m_socket.send(io::protect(empty));
}

void
blastbeat_t::on_spawn() {
    m_log->info("connected to the blastbeat server");
}

void
blastbeat_t::on_uwsgi(const std::string& sid,
                      zmq::message_t& message)
{
    std::map<
        std::string,
        std::string
    > env;

    const char * ptr = static_cast<const char*>(message.data()),
               * const end = ptr + message.size();

    // Skip the header.
    ptr += sizeof(uwsgi_header_t);

    while(ptr < end) {
        const uint16_t * ksz,
                       * vsz;

        ksz = reinterpret_cast<const uint16_t*>(ptr);
        ptr += sizeof(*ksz);

        std::string key(ptr, *ksz);
        ptr += *ksz;

        vsz = reinterpret_cast<const uint16_t*>(ptr);
        ptr += sizeof(*vsz);

        std::string value(ptr, *vsz);
        ptr += *vsz;

        env[key] = value;
    }

    if(env["REQUEST_METHOD"] == "POST") {
        // Defer till body arrives.
    } else {
        msgpack::sbuffer buffer;
        msgpack::packer<msgpack::sbuffer> packer(buffer);

        packer.pack_map(2);

        packer << std::string("meta")    << env;
        packer << std::string("request") << std::string();

        engine().enqueue(
            boost::make_shared<blastbeat_job_t>(
                m_event,
                std::string(
                    buffer.data(),
                    buffer.size()
                ),
                engine::policy_t(),
                sid,
                boost::ref(m_socket)
            )
        );
    }
}

void
blastbeat_t::on_body(const std::string& sid, 
                     zmq::message_t& body)
{
}

void
blastbeat_t::on_end(const std::string& sid) {
    m_log->debug("session ended");
}
