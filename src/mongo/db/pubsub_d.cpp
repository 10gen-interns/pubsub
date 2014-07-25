/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/pch.h"

#include <boost/thread.hpp>
#include <zmq.hpp>

#include "mongo/base/init.h"
#include "mongo/db/pubsub.h"
#include "mongo/db/server_options.h"

namespace mongo {
    namespace {

        MONGO_INITIALIZER(SetupPubSubSockets)
                                            (InitializerContext* context) {

            // TODO: allow users to set pubsub ports on startup
            const int port = serverGlobalParams.port;

            // is publish socket regardless of if config or mongod
            PubSub::extSendSocket = PubSub::initSendSocket();

            // is pull socket if config, sub socket if mongod
            PubSub::extRecvSocket = PubSub::initRecvSocket();

            // config servers must be started with --configsvr to
            // use pubsub within a sharded cluster
            if (serverGlobalParams.configsvr) {
                // each mongos pushes its messages to a queue shared between the config servers
                // (shared queue handled automatically by zmq's push/pull sockets);
                // each config server atomically pulls messages from the queue and
                // broadcasts them to all mongoses through a zmq.publish socket.

                try {
                    // listen (pull) from each mongos in the cluster
                    const std::string EXT_PULL_ENDPOINT = str::stream() << "tcp://*:"
                                                                        << (port + 1234);
                    PubSub::extRecvSocket->bind(EXT_PULL_ENDPOINT.c_str());

                    // publish to all mongoses in the cluster
                    const std::string EXT_PUB_ENDPOINT = str::stream() << "tcp://*:"
                                                                       << (port + 2345);
                    PubSub::extSendSocket->bind(EXT_PUB_ENDPOINT.c_str());
                } catch (zmq::error_t& e) {
                    log() << "Error initializing pubsub sockets." << causedBy(e) << endl;
                }

                // automatically proxy messages from PULL endpoint to PUB endpoint
                boost::thread internal_proxy(PubSub::proxy,
                                             PubSub::extRecvSocket,
                                             PubSub::extSendSocket);
            } else {
                // each mongod in a replica set publishes its messages
                // to all other mongods in its replica set

                try {
                    // listen (subscribe) to all mongods in the replset
                    const std::string EXT_SUB_ENDPOINT = str::stream() << "tcp://*:"
                                                                       << (port + 1234);
                    PubSub::extRecvSocket->bind(EXT_SUB_ENDPOINT.c_str());

                    // connect to own sub socket to publish messages to self
                    // (other mongods in replset connect to our sub socket when they join the set)
                    const std::string EXT_PUB_ENDPOINT = str::stream() << "tcp://localhost:"
                                                                       << (port + 1234);
                    PubSub::extSendSocket->connect(EXT_PUB_ENDPOINT.c_str());

                    // automatically proxy messages from SUB endpoint to client sub sockets
                    PubSub::intPubSocket.bind(PubSub::INT_PUBSUB_ENDPOINT);
                } catch (zmq::error_t& e) {
                    log() << "Could not initialize pubsub sockets: " << causedBy(e) << endl;
                }

                // proxy incoming messages to internal publisher to be received by clients
                boost::thread internal_proxy(PubSub::proxy,
                                             PubSub::extRecvSocket,
                                             &PubSub::intPubSocket);
            }

            // clean up subscriptions that have been inactive for at least 10 minutes
            boost::thread subscription_cleanup(PubSub::subscription_cleanup);

            return Status::OK();
        }
    }
}
