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
#define EPOLL_TIMEOUT_MS 100  // Timeout in milliseconds for epoll_wait
#define DEFAULT_CLIENT_THREADS 4

// Global configuration variables.
char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
long num_requests = 1000000;  // Total number of requests per client thread

/*
 * Extended structure to track per-thread data and packet counts.
 */
typedef struct {
    int epoll_fd;         /* File descriptor for the epoll instance */
    int socket_fd;        /* UDP socket file descriptor */
    long long total_rtt;  /* Accumulated round-trip time (RTT) in microseconds */
    long total_messages;  /* Total number of successful round trips */
    float request_rate;   /* Calculated request rate (requests per second) */
    long tx_cnt;          /* Count of packets transmitted */
    long rx_cnt;          /* Count of packets received (echoed) */
} client_thread_data_t;

/*
 * UDP-based Stop-and-Wait client thread function.
 * Each thread sends MESSAGE_SIZE bytes to the server, waits for the echo response using epoll,
 * and tracks the round-trip time along with packet transmission and loss.
 */
void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    char send_buf[MESSAGE_SIZE] = "ABCDEFGHIJKMLNOP";
    char recv_buf[MESSAGE_SIZE];
    struct timeval start, end;

    // Register the UDP socket with the epoll instance for reading.
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) < 0) {
        perror("epoll_ctl add failed");
        pthread_exit(NULL);
    }

    // Loop to send UDP packets and wait for the echo reply.
    for (long i = 0; i < num_requests; i++) {
        // Record timestamp before sending.
        if (gettimeofday(&start, NULL) < 0) {
            perror("gettimeofday failed");
            continue;
        }

        // Send the UDP packet.
        ssize_t sent = send(data->socket_fd, send_buf, MESSAGE_SIZE, 0);
        if (sent != MESSAGE_SIZE) {
            perror("send failed");
            continue;
        }
        data->tx_cnt++;  // Increment transmitted counter.

        // Wait for the echo response with a timeout.
        int nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, EPOLL_TIMEOUT_MS);
        if (nfds < 0) {
            perror("epoll_wait failed");
            continue;
        }

        if (nfds == 0) {
            // Timeout indicates no response; count this as a lost packet.
            continue;
        }

        // Process the events (we expect just one event on our UDP socket).
        for (int j = 0; j < nfds; j++) {
            if (events[j].data.fd == data->socket_fd && (events[j].events & EPOLLIN)) {
                ssize_t received = recv(data->socket_fd, recv_buf, MESSAGE_SIZE, 0);
                if (received <= 0) {
                    if (received < 0)
                        perror("recv failed");
                    else
                        fprintf(stderr, "Server closed connection unexpectedly\n");
                    pthread_exit(NULL);
                }
                data->rx_cnt++;  // Increment received counter.
            }
        }

        // Record timestamp after receiving the echo.
        if (gettimeofday(&end, NULL) < 0) {
            perror("gettimeofday failed");
            continue;
        }

        // Compute RTT in microseconds.
        long rtt = (end.tv_sec - start.tv_sec) * 1000000L + (end.tv_usec - start.tv_usec);
        data->total_rtt += rtt;
        data->total_messages++;
    }

    // Calculate the request rate (messages per second) for this thread.
    float total_time_sec = data->total_rtt / 1000000.0f;
    if (total_time_sec > 0) {
        data->request_rate = data->total_messages / total_time_sec;
    } else {
        data->request_rate = 0.0f;
    }
    pthread_exit(NULL);
}

/*
 * run_client() creates multiple client threads, where each thread sets up its UDP socket and epoll instance.
 * It then aggregates performance metrics including RTT, request rate, and counts for transmitted and received packets.
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
    
    // Create a UDP socket and epoll instance for each client thread.
    for (int i = 0; i < num_client_threads; i++) {
        // Create a UDP socket.
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            perror("socket creation failed");
            exit(EXIT_FAILURE);
        }
        
        // Optional: use connect() on the UDP socket to set a default server address.
        if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            perror("connect failed");
            close(sock);
            exit(EXIT_FAILURE);
        }
        
        // Create an epoll instance.
        int epoll_fd = epoll_create1(0);
        if (epoll_fd < 0) {
            perror("epoll_create1 failed");
            close(sock);
            exit(EXIT_FAILURE);
        }
        
        // Initialize the thread data.
        thread_data[i].socket_fd = sock;
        thread_data[i].epoll_fd = epoll_fd;
        thread_data[i].total_rtt = 0;
        thread_data[i].total_messages = 0;
        thread_data[i].request_rate = 0.0f;
        thread_data[i].tx_cnt = 0;
        thread_data[i].rx_cnt = 0;
        
        // Create the client thread.
        if (pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]) != 0) {
            perror("pthread_create failed");
            exit(EXIT_FAILURE);
        }
    }
    
    // Aggregate metrics across all client threads.
    long long total_rtt = 0;
    long total_messages = 0;
    float total_request_rate = 0.0f;
    long total_tx = 0, total_rx = 0;
    
    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;
        total_tx += thread_data[i].tx_cnt;
        total_rx += thread_data[i].rx_cnt;

        close(thread_data[i].socket_fd);
        close(thread_data[i].epoll_fd);
    }
    
    long long average_rtt = (total_messages > 0) ? (total_rtt / total_messages) : 0;
    printf("Average RTT: %lld us\n", average_rtt);
    printf("Total Request Rate: %f messages/s\n", total_request_rate);
    printf("Total Packets Sent: %ld\n", total_tx);
    printf("Total Packets Received: %ld\n", total_rx);
    printf("Total Packets Lost: %ld\n", total_tx - total_rx);
}

/*
 * UDP-based server function.
 * The server binds to the given IP address and port, then enters a continuous event loop using epoll.
 * It uses recvfrom() to receive packets from clients, and sends the exact same payload back using sendto().
 */
void run_server() {
    int server_sock;
    int epoll_fd;
    struct sockaddr_in server_addr, client_addr;
    struct epoll_event ev, events[MAX_EVENTS];
    socklen_t client_addr_len = sizeof(client_addr);

    // Create a UDP socket.
    server_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_sock < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Set socket option to reuse the address.
    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }
    
    // Set up the server address.
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0)
        server_addr.sin_addr.s_addr = INADDR_ANY;
    
    // Bind the UDP socket.
    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }
    
    // Create an epoll instance.
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1 failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }
    
    // Register the UDP socket with epoll for incoming data.
    ev.events = EPOLLIN;
    ev.data.fd = server_sock;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock, &ev) < 0) {
        perror("epoll_ctl failed");
        close(server_sock);
        close(epoll_fd);
        exit(EXIT_FAILURE);
    }
    
    printf("UDP Server listening on %s:%d\n", server_ip, server_port);
    
    // Server event loop.
    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait failed");
            break;
        }
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_sock && (events[i].events & EPOLLIN)) {
                char buffer[MESSAGE_SIZE];
                ssize_t count = recvfrom(server_sock, buffer, MESSAGE_SIZE, 0,
                                         (struct sockaddr *)&client_addr, &client_addr_len);
                if (count < 0) {
                    perror("recvfrom failed");
                    continue;
                }
                // Echo the received data back to the sender.
                ssize_t sent = sendto(server_sock, buffer, count, 0,
                                      (struct sockaddr *)&client_addr, client_addr_len);
                if (sent != count) {
                    perror("sendto failed");
                }
            }
        }
    }
    
    close(server_sock);
    close(epoll_fd);
}

/*
 * Main entry point.
 * Usage:
 *   ./udp_app server [server_ip server_port]
 *   ./udp_app client [server_ip server_port num_client_threads num_requests]
 */
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

