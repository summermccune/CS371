/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

/* 
Please specify the group members here
# Student #1: Summer McCune
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4

// Global configuration variables.
char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
long num_requests = 1000000;  // use long for large number of requests

/*
 * This structure is used to store per-thread data in the client
 */
typedef struct {
    int epoll_fd;        /* File descriptor for the epoll instance, used for monitoring events on the socket. */
    int socket_fd;       /* File descriptor for the client socket connected to the server. */
    long long total_rtt; /* Accumulated Round-Trip Time (RTT) for all messages sent and received (in microseconds). */
    long total_messages; /* Total number of messages sent and received. */
    float request_rate;  /* Computed request rate (requests per second) based on RTT and total messages. */
} client_thread_data_t;

/*
 * This function runs in a separate client thread to handle communication with the server.
 * It sends MESSAGE_SIZE bytes to the server, waits for an echo response using epoll,
 * and times the round-trip duration for each request
 */
void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    /* Prepare a 16-byte message */
    char send_buf[MESSAGE_SIZE] = "ABCDEFGHIJKMLNOP"; 
    char recv_buf[MESSAGE_SIZE];
    struct timeval start, end;

    // Hint 1: register the connected client socket with its epoll instance.
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) < 0) {
        perror("epoll_ctl add failed");
        pthread_exit(NULL);
    }

    // Loop for sending and receiving messages.
    for (long i = 0; i < num_requests; i++) {
        // Record timestamp before sending the message.
        if(gettimeofday(&start, NULL) < 0) {
            perror("gettimeofday failed");
            continue;
        }
        
        // Send the message.
        ssize_t sent = send(data->socket_fd, send_buf, MESSAGE_SIZE, 0);
        if (sent != MESSAGE_SIZE) {
            perror("send failed");
            continue;
        }

        // Wait for the response using epoll.
        int nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, -1);
        if(nfds < 0){
            perror("epoll_wait failed");
            continue;
        }
        // We expect only one event (the echo response).
        for (int j = 0; j < nfds; j++) {
            if (events[j].data.fd == data->socket_fd && (events[j].events & EPOLLIN)) {
                ssize_t received = recv(data->socket_fd, recv_buf, MESSAGE_SIZE, 0);
                if(received <= 0) {
                    if (received < 0)
                        perror("recv failed");
                    else
                        fprintf(stderr, "Server closed connection unexpectedly\n");
                    pthread_exit(NULL);
                }
            }
        }

        // Record timestamp after receiving the echo.
        if(gettimeofday(&end, NULL) < 0) {
            perror("gettimeofday failed");
            continue;
        }

        // Compute RTT in microseconds.
        long rtt = (end.tv_sec - start.tv_sec) * 1000000L + (end.tv_usec - start.tv_usec);
        data->total_rtt += rtt;
        data->total_messages++;
    }

    // Calculate request rate for this thread.
    float total_time_sec = data->total_rtt / 1000000.0f;
    if (total_time_sec > 0) {
        data->request_rate = data->total_messages / total_time_sec;
    } else {
        data->request_rate = 0.0f;
    }
    pthread_exit(NULL);
}

/*
 * This function orchestrates multiple client threads to send requests to a server,
 * collect performance data of each thread, and compute aggregated metrics.
 */
void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    // Set up the server address.
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid server IP address");
        exit(EXIT_FAILURE);
    }
    
    // Create a socket and epoll instance for each client thread and connect to the server.
    for (int i = 0; i < num_client_threads; i++) {
        // Create socket.
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("socket creation failed");
            exit(EXIT_FAILURE);
        }
        
        // Connect to the server.
        if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            perror("connect failed");
            close(sock);
            exit(EXIT_FAILURE);
        }
        
        // Create epoll instance.
        int epoll_fd = epoll_create1(0);
        if (epoll_fd < 0) {
            perror("epoll_create1 failed");
            close(sock);
            exit(EXIT_FAILURE);
        }
        
        // Initialize thread data.
        thread_data[i].socket_fd = sock;
        thread_data[i].epoll_fd = epoll_fd;
        thread_data[i].total_rtt = 0;
        thread_data[i].total_messages = 0;
        thread_data[i].request_rate = 0.0;
        
        // Create the client thread.
        if (pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]) != 0) {
            perror("pthread_create failed");
            exit(EXIT_FAILURE);
        }
    }
    
    // Wait for client threads to complete and aggregate metrics.
    long long total_rtt = 0;
    long total_messages = 0;
    float total_request_rate = 0.0f;
    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;
        
        // Clean up descriptors.
        close(thread_data[i].socket_fd);
        close(thread_data[i].epoll_fd);
    }
    
    // Compute overall average RTT.
    long long average_rtt = (total_messages > 0) ? (total_rtt / total_messages) : 0;
    printf("Average RTT: %lld us\n", average_rtt);
    printf("Total Request Rate: %f messages/s\n", total_request_rate);
}

/*
 * The server creates a listening socket, adds it to an epoll instance,
 * and then enters a continuous event loop. It handles:
 *    - connection requests (accepting new clients, and registration with epoll)
 *    - read events (receiving messages from a client and echoing them back).
 */
void run_server() {
    int listen_fd, epoll_fd;
    struct sockaddr_in server_addr;
    struct epoll_event ev, events[MAX_EVENTS];

    // Create the listening socket.
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Set socket options (reuse address).
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
    
    // Initialize server address.
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    // Bind to provided IP, or if 127.0.0.1 is specified, then listen on that interface.
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0)
        server_addr.sin_addr.s_addr = INADDR_ANY;
    
    // Bind the socket.
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
    
    // Listen for incoming connections.
    if (listen(listen_fd, 64) < 0) {
        perror("listen failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
    
    // Create the epoll instance.
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1 failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
    
    // Register the listening socket with epoll.
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        perror("epoll_ctl failed");
        close(listen_fd);
        close(epoll_fd);
        exit(EXIT_FAILURE);
    }
    
    printf("Server listening on %s:%d\n", server_ip, server_port);
    
    // Server's run-to-completion event loop.
    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if(errno == EINTR)
                continue;
            perror("epoll_wait failed");
            break;
        }
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == listen_fd) {
                // New connection request; accept connection.
                struct sockaddr_in client_addr;
                socklen_t client_addr_len = sizeof(client_addr);
                int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_addr_len);
                if (client_fd < 0) {
                    perror("accept failed");
                    continue;
                }
                // Add the new client socket to the epoll instance.
                ev.events = EPOLLIN;
                ev.data.fd = client_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                    perror("epoll_ctl client_fd failed");
                    close(client_fd);
                    continue;
                }
            } else {
                // A client socket is ready; read data from it.
                int client_fd = events[i].data.fd;
                char buffer[MESSAGE_SIZE];
                ssize_t count = recv(client_fd, buffer, MESSAGE_SIZE, 0);
                if (count <= 0) {
                    if (count < 0)
                        perror("recv failed");
                    close(client_fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                } else {
                    // Echo the message back to the client.
                    ssize_t sent = send(client_fd, buffer, count, 0);
                    if (sent != count) {
                        perror("send failed");
                    }
                }
            }
        }
    }
    
    close(listen_fd);
    close(epoll_fd);
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        if (argc > 4) num_client_threads = atoi(argv[4]);
        if (argc > 5) num_requests = atol(argv[5]);
        run_client();
    } else {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }

    return 0;
}
