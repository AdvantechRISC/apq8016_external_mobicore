/*
 * Copyright (c) 2013-2014 TRUSTONIC LIMITED
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the TRUSTONIC LIMITED nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * Connection server.
 *
 * Handles incoming socket connections from clients using the MobiCore driver.
 */
#include "public/Server.h"
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

//#define LOG_VERBOSE
#include "log.h"
#include "FSD.h"
// Local headers
#include "Queue.h"
#include "Client.h"

struct Server::Private : public CThread {
    ConnectionHandler *connectionHandler;
    std::list<Client*> clients;
    pthread_mutex_t clients_mutex_;
    Queue<Client*> queue;
    Private(ConnectionHandler *ch): connectionHandler(ch) {
        pthread_mutex_init(&clients_mutex_, NULL);
    }
    ~Private() {
        pthread_mutex_destroy(&clients_mutex_);
    }
    void run() {
        Client* client;
        for (;;) {
            client = queue.pop();
            if (!client) {
                break;
            }
            if (client->isDead()) {
                if (!client->isDetached()) {
                    client->dropConnection();
                }
                client->setExiting();
            } else {
                connectionHandler->handleCommand(client->connection(), client->commandId());
            }
            client->unlock();
        }
    }
};

//------------------------------------------------------------------------------
Server::Server(ConnectionHandler *handler, const char *localAddr):
    serverSock(-1), socketAddr(localAddr), connectionHandler(handler),
    priv_(new Private(connectionHandler)) {}


//------------------------------------------------------------------------------
void Server::run() {
    bool isFSDStarted=false;
    FSD *FileStorageDaemon=NULL;

    do {
        LOG_I("Server: start listening on socket %s", socketAddr.c_str());

        // Open a socket (a UNIX domain stream socket)
        serverSock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (serverSock < 0) {
            LOG_ERRNO("Can't open stream socket, because socket");
            break;
        }

        // Fill in address structure and bind to socket
        struct sockaddr_un  serverAddr;
        serverAddr.sun_family = AF_UNIX;
        strncpy(serverAddr.sun_path, socketAddr.c_str(), sizeof(serverAddr.sun_path) - 1);

        uint32_t len = strlen(serverAddr.sun_path) + sizeof(serverAddr.sun_family);
        // Make the socket in the Abstract Domain(no path but everyone can connect)
        serverAddr.sun_path[0] = 0;
        if (bind(serverSock, (struct sockaddr *) &serverAddr, len) < 0) {
            LOG_ERRNO("Binding to server socket failed, because bind");
        }

        // Start listening on the socket
        if (listen(serverSock, LISTEN_QUEUE_LEN) < 0) {
            LOG_ERRNO("listen");
            break;
        }

        LOG_I("\n********* successfully initialized Daemon *********\n");
        priv_->start();

        for (;;) {
            fd_set fdReadSockets;

            // Clear FD for select()
            FD_ZERO(&fdReadSockets);

            // Select server socket descriptor
            FD_SET(serverSock, &fdReadSockets);

            if (!isFSDStarted) {
                // Create the <t-base File Storage Daemon
                FileStorageDaemon = new FSD();
                // Start File Storage Daemon
                FileStorageDaemon->start("McDaemon.FSD");

                isFSDStarted=true;
            }

            // Wait for activities, select() returns the number of sockets
            // which require processing
            LOG_V(" Server: waiting on server socket");
            int numSockets = select(serverSock + 1, &fdReadSockets, NULL, NULL, NULL);

            // Check if select failed
            if (numSockets < 0) {
                LOG_ERRNO("select");
                break;
            }

            // actually, this should not happen.
            if (0 == numSockets) {
                LOG_W(" Server: select() returned 0, spurious event?.");
                continue;
            }

            LOG_V(" Server: event on socket.");

            // Check if a new client connected to the server socket
            if (FD_ISSET(serverSock, &fdReadSockets)) {
                do {
                    LOG_V(" Server: new connection attempt.");
                    numSockets--;

                    struct sockaddr_un clientAddr;
                    socklen_t clientSockLen = sizeof(clientAddr);
                    int clientSock = accept(
                                         serverSock,
                                         (struct sockaddr *) &clientAddr,
                                         &clientSockLen);

                    if (clientSock <= 0) {
                        LOG_ERRNO("accept");
                        break;
                    }

                    Client *client = new Client(connectionHandler, clientSock, &clientAddr, priv_->queue);
                    pthread_mutex_lock(&priv_->clients_mutex_);
                    priv_->clients.push_back(client);
                    pthread_mutex_unlock(&priv_->clients_mutex_);
                    client->start();
                    LOG_I(" Server: new socket client %p created and start listening.", client);
                } while (false);

                // we can ignore any errors from accepting a new connection.
                // If this fail, the client has to deal with it, we are done
                // and nothing has changed.
            }

            // Take this opportunity to reap any dead clients
            pthread_mutex_lock(&priv_->clients_mutex_);
            std::list<Client*>::iterator it = priv_->clients.begin();
            while (it != priv_->clients.end()) {
                Client* client = *it;
                if (client->isExiting()) {
                    it = priv_->clients.erase(it);
                    client->join();
                    delete client;
                    LOG_I(" Server: client %p destroyed.", client);
                } else {
                    it++;
                }
            }
            pthread_mutex_unlock(&priv_->clients_mutex_);
        }

        // Exit worker thread
        priv_->queue.push(NULL);
        priv_->join();
    } while (false);

    // Change state to STOPPED, releases start() on failure
    pthread_mutex_lock(&startup_mutex);
    server_state = STOPPED;
    pthread_cond_broadcast(&startup_cond);
    pthread_mutex_unlock(&startup_mutex);



    //Wait for File Storage Daemon to exit
    if (FileStorageDaemon) {
        FileStorageDaemon->join();
        delete FileStorageDaemon;
    }

    LOG_ERRNO("Exiting Server, because");
    kill(getpid(), SIGTERM);

}


//------------------------------------------------------------------------------
void Server::detachConnection(Connection* connection) {
    LOG_V(" Stopping to listen on notification socket.");

    pthread_mutex_lock(&priv_->clients_mutex_);
    for (std::list<Client*>::iterator it = priv_->clients.begin(); it != priv_->clients.end(); it++) {
        Client* client = *it;
        if (client->connection() == connection) {
            client->detachConnection();
            LOG_I(" Stopped listening on notification socket.");
            break;
        }
    }
    pthread_mutex_unlock(&priv_->clients_mutex_);
}


//------------------------------------------------------------------------------
Server::~Server() {
    // Shut down the server socket
    if (serverSock != -1) {
        close(serverSock);
    }
    // Destroy all client connections
    for (std::list<Client*>::iterator it = priv_->clients.begin(); it != priv_->clients.end(); it++) {
        Client* client = *it;
        delete client;
    }
    delete priv_;
}

//------------------------------------------------------------------------------

void Server::start(const char *name) {
    server_state = STARTING;
    CThread::start(name);
    // Hang on till thread has started or stopped
    pthread_mutex_lock(&startup_mutex);
    while (server_state == STARTING) {
        pthread_cond_wait(&startup_cond, &startup_mutex);
    }
    pthread_mutex_unlock(&startup_mutex);
}

