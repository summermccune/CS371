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
#include <stdint.h>

// --- Macros and Global Variables ---

#define MAX_EVENTS         64
#define MESSAGE_SIZE       16      // Total packet size (header + payload)
#define EPOLL_TIMEOUT_MS   100     // Used by the server's epoll loop
#define DEFAULT_CLIENT_THREADS 4

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
long num_requests = 1000000;  // Total number of requests per client thread

// --- Packet Header Structure ---
// We include a client identifier and a sequence number in each packet.
typedef struct {
    uint32_t client_id;   // Unique per client thread.
    uint32_t seq_num;     // Sequence number for each packet.
} packet_header_t;

// --- Client Thread Data ---
typedef struct {
    int epoll_fd;         /* File descriptor for the epoll instance */
    int socket_fd;        /* UDP socket file descriptor */
    long long total_rtt;  /* Accumulated round-trip time (RTT) in microseconds */
    long total_messages;  /* Total number of successfully acknowledged packets */
    float request_rate;   /* Calculated request rate (messages per second) */
    long tx_cnt;          /* Count of packets transmitted (only counting first transmissions) */
    long rx_cnt;          /* Count of packets received (matching acknowledgments) */
} client_thread_data_t;

// --- Client Thread Function ---
void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];

    // Add the UDP socket to the epoll instance for reading.
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) < 0) {
        perror("epoll_ctl add failed");
        pthread_exit(NULL);
    }

    // ARQ parameters.
    const int ARQ_TIMEOUT_MS  = 100;    // Timeout per ARQ attempt.
    const int GLOBAL_TIMEOUT_MS = 3000;   // Maximum cumulative waiting time per packet.
    // Use pthread_self() as a (simple) unique client identifier. (Alternatively, assign a
    // unique client id as you create each thread.)
    uint32_t client_id = (uint32_t)pthread_self();

    for (unsigned long seq = 0; seq < num_requests; seq++) {
        int acked = 0;
        struct timeval global_start, curr_time;
        if (gettimeofday(&global_start, NULL) < 0) {
            perror("gettimeofday failed");
            continue;
        }
        int firstAttempt = 1;  // Only the very first send is counted.

        // Continue retransmitting until an acknowledgement is received, or a global timeout is reached.
        while (!acked) {
            // --- Build the Packet ---
            char packet[MESSAGE_SIZE];
            packet_header_t header;
            header.client_id = client_id;
            header.seq_num = (uint32_t)seq;
            memcpy(packet, &header, sizeof(header));
            // Fill the remaining payload with arbitrary data (here, the character 'A')
            int payload_size = MESSAGE_SIZE - sizeof(packet_header_t);
            memset(packet + sizeof(packet_header_t), 'A', payload_size);

            // --- Transmit the Packet ---
            ssize_t sent = send(data->socket_fd, packet, MESSAGE_SIZE, 0);
            if (sent != MESSAGE_SIZE) {
                perror("send failed");
                // Even if send fails, continue with ARQ attempts.
            } else if (firstAttempt) {
                data->tx_cnt++;  // Only count the very first transmission.
                firstAttempt = 0;
            }
        
            // --- Start ARQ Attempt Timer ---
            struct timeval attempt_start;
            if (gettimeofday(&attempt_start, NULL) < 0) {
                perror("gettimeofday failed");
                break;
            }
            int elapsed_attempt_ms = 0;
            int got_ack = 0;

            // Wait (up to ARQ_TIMEOUT_MS) for an incoming packet.
            while (elapsed_attempt_ms < ARQ_TIMEOUT_MS) {
                int wait_time = ARQ_TIMEOUT_MS - elapsed_attempt_ms;
                int nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, wait_time);
                if (nfds < 0) {
                    perror("epoll_wait failed");
                    break;
                }
                if (nfds == 0) {  // No events occurred in the wait_time.
                    break;
                }
                // Process any received events.
                for (int j = 0; j < nfds; j++) {
                    if (events[j].data.fd == data->socket_fd && (events[j].events & EPOLLIN)) {
                        char recv_buf[MESSAGE_SIZE];
                        ssize_t recvd = recv(data->socket_fd, recv_buf, MESSAGE_SIZE, 0);
                        if (recvd < 0) {
                            perror("recv failed");
                            continue;
                        }
                        if (recvd != MESSAGE_SIZE) {
                            fprintf(stderr, "Incomplete packet received\n");
                            continue;
                        }
                        // Parse the header from the reply.
                        packet_header_t resp_header;
                        memcpy(&resp_header, recv_buf, sizeof(resp_header));
                        // Check for matching client ID and sequence number.
                        if (resp_header.client_id == client_id &&
                            resp_header.seq_num == (uint32_t)seq) {
                            data->rx_cnt++; // Correct acknowledgment.
                            got_ack = 1;
                            break;
                        }
                    }
                }
                if (got_ack)
                    break;

                // Update the elapsed time for this ARQ attempt.
                struct timeval now;
                if (gettimeofday(&now, NULL) < 0) {
                    perror("gettimeofday failed");
                    break;
                }
                elapsed_attempt_ms = (int)((now.tv_sec - attempt_start.tv_sec) * 1000 +
                                             (now.tv_usec - attempt_start.tv_usec) / 1000);
            }  // End inner while (one ARQ attempt).

            if (got_ack) {
                acked = 1;
                // Measure round-trip time from the very first send.
                if (gettimeofday(&curr_time, NULL) < 0) {
                    perror("gettimeofday failed");
                    continue;
                }
                long rtt = (curr_time.tv_sec - global_start.tv_sec) * 1000000L +
                           (curr_time.tv_usec - global_start.tv_usec);
                data->total_rtt += rtt;
                data->total_messages++;
            } else {
                // Check the global timeout.
                if (gettimeofday(&curr_time, NULL) < 0) {
                    perror("gettimeofday failed");
                    break;
                }
                int elapsed_global_ms = (int)((curr_time.tv_sec - global_start.tv_sec) * 1000 +
                                                (curr_time.tv_usec - global_start.tv_usec) / 1000);
                if (elapsed_global_ms >= GLOBAL_TIMEOUT_MS) {
                    fprintf(stderr, "Packet (seq %lu) not acknowledged after %d ms\n", seq, GLOBAL_TIMEOUT_MS);
                    // For robustness, we break out of the ARQ loop to continue with the next packet.
                    break;
                }
                // Otherwise, the ARQ loop repeats (retransmitting the same seq).
            }
        }   // End ARQ while for one packet.
    }  // End for each request.

    // Calculate the request rate for this thread.
    float total_time_sec = data->total_rtt / 1000000.0f;
    if (total_time_sec > 0)
        data->request_rate = data->total_messages / total_time_sec;
    else
        data->request_rate = 0.0f;

    pthread_exit(NULL);
}

// --- Function to Run the Client --- 
void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid server IP address");
        exit(EXIT_FAILURE);
    }
    
    for (int i = 0; i < num_client_threads; i++) {
        // Create a UDP socket.
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            perror("socket creation failed");
            exit(EXIT_FAILURE);
        }
        
        // Connect the UDP socket to the server.
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
        
        // Initialize thread data.
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
    
    // Wait for all threads to finish and aggregate metrics.
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

// --- UDP-based Server Function ---
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
    
    // Enable address reuse.
    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    // Create an epoll instance for event-driven packet processing.
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1 failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    ev.events = EPOLLIN;
    ev.data.fd = server_sock;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock, &ev) < 0) {
        perror("epoll_ctl add failed");
        close(server_sock);
        close(epoll_fd);
        exit(EXIT_FAILURE);
    }

    // Main server loop: Receive and echo packets.
    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            perror("epoll_wait failed");
            continue;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_sock && (events[i].events & EPOLLIN)) {
                char buf[MESSAGE_SIZE];
                ssize_t recv_len = recvfrom(server_sock, buf, MESSAGE_SIZE, 0,
                                            (struct sockaddr *)&client_addr, &client_addr_len);
                if (recv_len < 0) {
                    perror("recvfrom failed");
                    continue;
                }

                // Echo the received packet back to the sender.
                ssize_t sent_len = sendto(server_sock, buf, recv_len, 0,
                                          (struct sockaddr *)&client_addr, client_addr_len);
                if (sent_len != recv_len) {
                    perror("sendto failed");
                }
            }
        }
    }

    close(server_sock);
    close(epoll_fd);
}

// --- main() ---
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <server|client> <server_ip> <server_port> [num_client_threads] [num_requests]\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "server") == 0) {
        server_ip = argv[2];
        server_port = atoi(argv[3]);
        run_server();
    } else if (strcmp(argv[1], "client") == 0) {
        if (argc > 3)
            server_ip = argv[2];
        if (argc > 4)
            server_port = atoi(argv[3]);
        if (argc > 5)
            num_client_threads = atoi(argv[4]);
        if (argc > 6)
            num_requests = atol(argv[5]);
        run_client();
    } else {
        fprintf(stderr, "Invalid argument: %s. Use 'server' or 'client'.\n", argv[1]);
        return 1;
    }

    return 0;
}


