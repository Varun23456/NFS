#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/select.h>

#define BUFFER_SIZE 1024
#define NS_PORT 8080
#define TIMEOUT 5

struct StorageServer
{
    char ip[16];
    int port;
};
int request_file_from_ss(struct StorageServer *ss_info, char *filename);

int send_command_to_naming_server(const char *command, char ip[INET_ADDRSTRLEN])
{
    int ns_sock;
    struct sockaddr_in ns_addr;
    char buffer[BUFFER_SIZE];
    char message[BUFFER_SIZE];
    fd_set read_fds;
    struct timeval timeout;

    if ((ns_sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("Naming Server socket creation failed");
        return -1;
    }

    ns_addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &ns_addr.sin_addr);
    ns_addr.sin_port = htons(NS_PORT);

    if (connect(ns_sock, (struct sockaddr *)&ns_addr, sizeof(ns_addr)) == -1)
    {
        perror("Connection to Naming Server failed");
        close(ns_sock);
        return -1;
    }

    snprintf(message, sizeof(message), "%s", command);

    if (send(ns_sock, message, strlen(message), 0) == -1)
    {
        perror("Failed to send command to Naming Server");
        close(ns_sock);
        return -1;
    }

    FD_ZERO(&read_fds);
    FD_SET(ns_sock, &read_fds);
    timeout.tv_sec = TIMEOUT;
    timeout.tv_usec = 0;

    int activity = select(ns_sock + 1, &read_fds, NULL, NULL, &timeout);
    if (activity == 0)
    {
        printf("Timeout: No response from Naming Server\n");
        close(ns_sock);
        return -1;
    }
    else if (activity < 0)
    {
        perror("Select failed");
        close(ns_sock);
        return -1;
    }

    int read_size = recv(ns_sock, buffer, sizeof(buffer) - 1, 0);
    if (read_size <= 0)
    {
        perror("Failed to receive acknowledgment from Naming Server");
        close(ns_sock);
        return -1;
    }

    buffer[read_size] = '\0';
    if (strstr(command, "LIST_ALL_ACCESSIBLE_PATHS"))
    {
        printf("Accessible paths:\n%s", buffer);
    }
    else if (strstr(buffer, "error code"))
    {
        printf("%s\n", buffer);
        return 1;
    }
    else
    {
        printf("Acknowledgment from Naming Server: %s\n", buffer);
    }

    close(ns_sock);
    return 0;
}

int ns_sock;
int get_storage_server_details(char *filename, struct StorageServer *ss_info, char ip[INET_ADDRSTRLEN])
{
    // int ns_sock;
    struct sockaddr_in ns_addr;
    char buffer[BUFFER_SIZE];
    pthread_t handle_async_ack;

    if ((ns_sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("Naming Server socket creation failed");
        return -1;
    }

    ns_addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &ns_addr.sin_addr);
    ns_addr.sin_port = htons(NS_PORT);

    if (connect(ns_sock, (struct sockaddr *)&ns_addr, sizeof(ns_addr)) == -1)
    {
        perror("Connection to Naming Server failed");
        close(ns_sock);
        return -1;
    }

    snprintf(buffer, sizeof(buffer), "%s", filename);

    if (send(ns_sock, buffer, strlen(buffer), 0) == -1)
    {
        perror("Failed to send request to NS");
        close(ns_sock);
        return -1;
    }

    int read_size = recv(ns_sock, buffer, sizeof(buffer) - 1, 0);
    if (read_size <= 0)
    {
        perror("Failed to receive data from Naming Server");
        close(ns_sock);
        return -1;
    }

    buffer[read_size] = '\0';
    sscanf(buffer, "%15s %d", ss_info->ip, &ss_info->port);

    printf("%s\n", buffer);
    if (strstr(buffer, "Server is not in online"))
    {
        printf("Server is not available\n");
        return 0;
    }
    if (strstr(buffer, "error code") )
    {
        printf("%s\n", buffer);
        return -1;
    }
    if (strstr(buffer, "READ"))
    {
        char buff1[1024];
        sscanf(buffer, "%s %d READ %s", ss_info->ip, &ss_info->port, buff1);
        // strcpy(filename, buff1);
        strcpy(filename, "READ ");
        strcat(filename, buff1);
    }
    else
    {
        sscanf(buffer, "%15s %d", ss_info->ip, &ss_info->port);
    }
    printf("%s\n", buffer);
    printf("%s\n", filename);
    printf("Storage Server found at IP: %s, Port: %d\n", ss_info->ip, ss_info->port);

    close(ns_sock);
    request_file_from_ss(ss_info, filename);
    return 0;
}

int request_file_from_ss(struct StorageServer *ss_info, char *filename)
{
    int ss_sock;
    struct sockaddr_in ss_addr;
    char buffer[BUFFER_SIZE];

    if ((ss_sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("Storage Server socket creation failed");
        return -1;
    }

    ss_addr.sin_family = AF_INET;
    inet_pton(AF_INET, ss_info->ip, &ss_addr.sin_addr);
    ss_addr.sin_port = htons(ss_info->port);

    if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) == -1)
    {
        perror("Connection to Storage Server failed");
        close(ss_sock);
        return -1;
    }

    snprintf(buffer, sizeof(buffer), "%s", filename);

    if (send(ss_sock, buffer, strlen(buffer), 0) == -1)
    {
        perror("Failed to send request to Storage Server");
        close(ss_sock);
        return -1;
    }

    if (strstr(buffer, "STREAM"))
    {
        int read_size = recv(ss_sock, buffer, sizeof(buffer) - 1, 0);
        if (read_size > 0)
        {
            buffer[read_size] = '\0';
            printf("Received: %s\n", buffer);
        }
        if (memcmp(buffer, "ID3", 3) == 0 || (buffer[0] == (char)0xFF && (buffer[1] & (char)0xE0) == (char)0xE0))
        {
            FILE *player = popen("mpg123 -", "w");
            if (player == NULL)
            {
                perror("Failed to open music player");
                close(ss_sock);
                return -1;
            }

            do
            {
                fwrite(buffer, 1, read_size, player);
                read_size = recv(ss_sock, buffer, sizeof(buffer), 0);
            } while (read_size > 0);

            if (read_size < 0)
            {
                perror("Error receiving audio data from Storage Server");
            }
            pclose(player);
            printf("Streaming complete.\n");
        }
    }
    else if (strstr(buffer, "READ"))
    {
        int read_size = recv(ss_sock, buffer, sizeof(buffer) - 1, 0);
        if (read_size > 0)
        {
            buffer[read_size] = '\0';
            printf("Received: %s\n", buffer);
        }
    }
    else
    {
        while (1)
        {
            int read_size = recv(ss_sock, buffer, sizeof(buffer) - 1, 0);
            if (read_size > 0)
            {
                buffer[read_size] = '\0';
                printf("Received: %s\n", buffer);
            }
            if (strstr(buffer, "STOP"))
                break;
        }
    }

    close(ss_sock);
    return 0;
}

int main()
{
    char input[BUFFER_SIZE];
    struct StorageServer ss_info;
    struct sockaddr_in ns_addr;
    char ip_address[INET_ADDRSTRLEN];
    printf("Enter the IP address of the Naming Server: ");
    if (scanf("%s", ip_address) != 1)
    {
        perror("Invalid IP address input");
        exit(1);
    }

    if ((ns_sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("Naming Server socket creation failed");
        exit(1);
    }

    ns_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip_address, &ns_addr.sin_addr) <= 0)
    {
        perror("Invalid IP address");
        close(ns_sock);
        exit(1);
    }
    ns_addr.sin_port = htons(NS_PORT);

    if (connect(ns_sock, (struct sockaddr *)&ns_addr, sizeof(ns_addr)) == -1)
    {
        perror("Connection to Naming Server failed");
        close(ns_sock);
        exit(1);
    }

    printf("Connected to Naming Server at %s:%d\n", ip_address, NS_PORT);
    int c;
    while ((c = getchar()) != '\n' && c != EOF)
        ;
    while (1)
    {
        printf("Enter the file name to request: ");
        fgets(input, sizeof(input), stdin);
        input[strcspn(input, "\n")] = '\0';
        if (strncmp(input, "CREATE ", 7) == 0 ||
            strncmp(input, "DELETE ", 7) == 0 ||
            strncmp(input, "COPY ", 5) == 0 ||
            strncmp(input, "LIST_ALL_ACCESSIBLE_PATHS", 25) == 0)
        {
            if (send_command_to_naming_server(input, ip_address) != 0)
            {
                printf("Failed to send command to Naming Server\n");
            }
        }
        else
        {
            if (get_storage_server_details(input, &ss_info, ip_address) != 0)
            {
                printf("Failed to get Storage Server details\n");
                return 1;
            }

            // if (request_file_from_ss(&ss_info, input) != 0)
            // {
            //     printf("Failed to request file from Storage Server\n");
            //     return 1;
            // }
        }
    }

    return 0;
}