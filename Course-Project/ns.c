#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <sys/epoll.h>
#include "errors.h"
#define NI_MAXHOSt 1024
#define NI_NUMERICHOSt 1
#define BUFFER_SIZE 1024
#define MAX_SS 10
#define PORT 8080
char wifi_ip[NI_MAXHOSt] = "";
#define MAX_CHILDREN 256
#define CACHE_CAPACITY 3
#define SS_STATUS_ACTIVE 1
#define SS_STATUS_INACTIVE 0
int isbacking = 0;
#define MAX_EVENTS 10
#define HEALTH_CHECK_INTERVAL 5
#define MONITOR_TIMEOUT_MS 50 // 1 second timeout for epoll
#define MAX_HEALTH_RETRIES 2
char list[8196];
pthread_mutex_t ss_count_mutex = PTHREAD_MUTEX_INITIALIZER;
typedef struct
{
    char ip[INET_ADDRSTRLEN];
    int ns_port;
    int client_port;
    int sock_fd;
    int status;
    int back_up;
    int back_up1;
    int back_up2;
    // char base_paths[]
    char paths[BUFFER_SIZE];
    char base_path[BUFFER_SIZE];
    char back_up_path[BUFFER_SIZE];
    time_t last_seen;
    pthread_t monitor_thread;
    int monitor_fd; // epoll fd for this server
    volatile int is_running;
    pthread_mutex_t lock;
} StorageServerInfo;

int storage_server_count = 0;
StorageServerInfo storage_servers[MAX_SS];
int flags[MAX_SS];
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for thread safety

typedef struct TrieNode
{
    struct TrieNode *children[MAX_CHILDREN];
    char *part;
    int file_flag;
    int isEndOfPath;
} TrieNode;

TrieNode *tries[MAX_SS];

typedef struct QueueNode
{
    TrieNode *trieNode;
    char path[1024];
    struct QueueNode *next;
} QueueNode;

pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
void log_message(const char *message)
{
    time_t now;
    struct tm *timeinfo;
    char timestamp[26];

    pthread_mutex_lock(&log_mutex);

    time(&now);
    timeinfo = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);

    FILE *log_file = fopen("./history.txt", "a");
    if (log_file == NULL)
    { // file open error
        perror("error opening file.");
    }
    if (log_file != NULL)
    {
        fprintf(log_file, "[%s] %s\n", timestamp, message);
        fclose(log_file);
    }

    pthread_mutex_unlock(&log_mutex);
}

TrieNode *createNode(const char *part)
{
    TrieNode *node = (TrieNode *)malloc(sizeof(TrieNode));
    node->part = part ? strdup(part) : NULL;
    node->isEndOfPath = 0;
    if (node->part != NULL && strchr(node->part, '.'))
    {
        node->file_flag = 1;
    }
    else
    {
        node->file_flag = 0;
    }
    for (int i = 0; i < MAX_CHILDREN; i++)
    {
        node->children[i] = NULL;
    }
    return node;
}

void insert(TrieNode *root, char path[])
{
    TrieNode *node = root;
    char part[256];
    const char *delimiter = "/";
    char *token;
    char *pathCopy = strdup(path);

    token = strtok(pathCopy, delimiter);
    while (token != NULL)
    {
        snprintf(part, sizeof(part), "%s", token);

        int found = 0;
        for (int i = 0; i < MAX_CHILDREN; i++)
        {
            if (node->children[i] != NULL && strcmp(node->children[i]->part, part) == 0)
            {
                node = node->children[i];
                found = 1;
                break;
            }
        }

        if (!found)
        {
            int i;
            for (i = 0; i < MAX_CHILDREN; i++)
            {
                if (node->children[i] == NULL)
                {
                    node->children[i] = createNode(part);
                    node = node->children[i];
                    break;
                }
            }
        }

        token = strtok(NULL, delimiter);
    }

    node->isEndOfPath = 1;
    free(pathCopy);
}

int searchPartial(TrieNode *root, char *path, TrieNode **head)
{
    TrieNode *node = root;
    char part[256];
    const char *delimiter = "/";
    char *token;
    // printf("given %s %ld\n", path, strlen(path));
    // char *pathCopy = (char*)malloc(1024 * sizeof(char));
    char pathCopy[1024];
    if (pathCopy == NULL)
    {
        printf("malloc failed. error code %d\n", E_NOMEM);
        exit(1);
    }
    strcpy(pathCopy, path);
    // char *pathCopy = strdup(path);

    token = strtok(pathCopy, delimiter);
    while (token != NULL)
    {
        snprintf(part, sizeof(part), "%s", token);
        int found = 0;
        for (int i = 0; i < MAX_CHILDREN; i++)
        {
            if (node->children[i] != NULL && strcmp(node->children[i]->part, part) == 0)
            {
                node = node->children[i];
                found = 1;
                break;
            }
        }

        if (!found)
        {
            *(head) = NULL;
            // free(pathCopy);
            return 0;
        }

        token = strtok(NULL, delimiter);
    }

    *(head) = node;
    // free(pathCopy);
    return 1;
}

typedef struct CacheNode
{
    char path[1024];
    int i;
    struct CacheNode *next;
} CacheNode;


typedef struct
{
    CacheNode *head;
    int size;
} LRUCache;
LRUCache *cache;

CacheNode *createcachenode(const char *path, int i)
{
    CacheNode *newNode = (CacheNode *)malloc(sizeof(CacheNode));
    if (!newNode)
    {
        printf("malloc failed. error code %d\n", E_NOMEM);
        exit(EXIT_FAILURE);
    }
    strncpy(newNode->path, path, sizeof(newNode->path));
    newNode->i = i;
    newNode->next = NULL;
    return newNode;
}


LRUCache *createCache()
{
    LRUCache *cache = (LRUCache *)malloc(sizeof(LRUCache));
    if (!cache)
    {
        printf("malloc failed. error code %d\n", E_NOMEM);
        exit(EXIT_FAILURE);
    }
    cache->head = NULL;
    cache->size = 0;
    return cache;
}

void moveToHead(LRUCache *cache, CacheNode *prev, CacheNode *node)
{
    if (!prev)
        return; 
    prev->next = node->next;
    node->next = cache->head;
    cache->head = node;
}

int get(LRUCache *cache, const char *path)
{
    CacheNode *prev = NULL, *curr = cache->head;

    while (curr)
    {
        if (strcmp(curr->path, path) == 0)
        {
            moveToHead(cache, prev, curr);
            return curr->i;
        }
        prev = curr;
        curr = curr->next;
    }

    return -1;
}

void removeTail(LRUCache *cache)
{
    if (!cache->head)
        return;

    CacheNode *prev = NULL, *curr = cache->head;
    while (curr->next)
    {
        prev = curr;
        curr = curr->next;
    }

    if (prev)
        prev->next = NULL;
    else
        cache->head = NULL;

    free(curr);
    cache->size--;
}

void put(LRUCache *cache, const char *path, int i)
{
    CacheNode *prev = NULL, *curr = cache->head;

    while (curr)
    {
        if (strcmp(curr->path, path) == 0)
        {
            curr->i = i;
            moveToHead(cache, prev, curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }

    if (cache->size == CACHE_CAPACITY)
    {
        removeTail(cache);
    }

    CacheNode *newNode = createcachenode(path, i);
    newNode->next = cache->head;
    cache->head = newNode;
    cache->size++;
}

int isEmpty(LRUCache *cache)
{
    return cache->head == NULL;
}

void printCache(LRUCache *cache)
{
    CacheNode *curr = cache->head;
    while (curr)
    {
        printf("Path: %s, Value: %d\n", curr->path, curr->i);
        curr = curr->next;
    }
    printf("----\n");
}

void removeFile(LRUCache *cache, const char *path)
{
    if (isEmpty(cache))
    {
        printf("Cache is empty, nothing to remove.\n");
        return;
    }

    CacheNode *prev = NULL, *curr = cache->head;

    while (curr)
    {
        if (strcmp(curr->path, path) == 0)
        {
            if (prev)
                prev->next = curr->next; 
            else
                cache->head = curr->next; 

            free(curr);
            cache->size--;
            printf("File '%s' removed from cache.\n", path);
            return;
        }

        prev = curr;
        curr = curr->next;
    }

    printf("File '%s' not found in cache.\n", path);
}

void getpaths(TrieNode **root, char *command)
{
    if (root == NULL)
        return;

    QueueNode *queue = NULL;
    QueueNode *rear = NULL;

    QueueNode *newNode = (QueueNode *)malloc(sizeof(QueueNode));
    newNode->trieNode = *(root);
    strcpy(newNode->path, newNode->trieNode->part);
    newNode->next = NULL;
    queue = rear = newNode;

    while (queue != NULL)
    {
        QueueNode *currentQueueNode = queue;
        queue = queue->next;

        TrieNode *currentTrieNode = currentQueueNode->trieNode;
        // char *currentPath = (char *)malloc(1024 * sizeof(char));
        char currentPath[1024];
        strcpy(currentPath, currentQueueNode->path);

        if (currentPath)
        {
            if (!strstr(currentPath, "backup"))
            {
                if (command[0] == '\0')
                {
                    strcpy(command, currentPath);
                }
                else
                {
                    strcat(command, currentPath);
                }
                strcat(command, ",");
            }
        }

        for (int i = 0; i < MAX_CHILDREN; i++)
        {
            if (currentTrieNode->children[i] != NULL)
            {
                QueueNode *childNode = (QueueNode *)malloc(sizeof(QueueNode));
                childNode->trieNode = currentTrieNode->children[i];
                strcpy(childNode->path, currentPath);
                strcat(childNode->path, "/");
                strcat(childNode->path, currentTrieNode->children[i]->part);
                childNode->next = NULL;

                if (queue == NULL)
                {
                    queue = rear = childNode;
                }
                else
                {
                    rear->next = childNode;
                    rear = childNode;
                }
            }
        }

        // free(currentPath);
        free(currentQueueNode);
    }
}

void freeTrie(TrieNode *root)
{
    if (!root)
        return;
    for (int i = 0; i < MAX_CHILDREN; i++)
    {
        if (root->children[i] != NULL)
        {
            freeTrie(root->children[i]);
        }
    }
    free(root->part);
    free(root);
}

int deletePath(TrieNode *root, const char *path)
{
    char part[256];
    const char *delimiter = "/";
    char *token;
    char *pathCopy = strdup(path);

    TrieNode *node = root;
    TrieNode *parent = NULL;
    TrieNode *nodesToDelete[MAX_CHILDREN];
    int deleteIndices[MAX_CHILDREN];
    int depth = 0;

    token = strtok(pathCopy, delimiter);
    while (token != NULL)
    {
        snprintf(part, sizeof(part), "%s", token);
        int found = 0;
        for (int i = 0; i < MAX_CHILDREN; i++)
        {
            if (node->children[i] != NULL && strcmp(node->children[i]->part, part) == 0)
            {
                parent = node;
                node = node->children[i];
                nodesToDelete[depth] = parent;
                deleteIndices[depth] = i;
                found = 1;
                depth++;
                break;
            }
        }

        if (!found)
        {
            free(pathCopy);
            printf("Path '%s' not found in the Trie.\n", path);
            return 0;
        }

        token = strtok(NULL, delimiter);
    }

    freeTrie(node);
    if (parent)
    {
        parent->children[deleteIndices[depth - 1]] = NULL;
    }
    printf("Path '%s' and all its descendants deleted.\n", path);
    free(pathCopy);
    return 1;
}

void *monitor_server(void *arg)
{
    StorageServerInfo *server = (StorageServerInfo *)arg;
    int retry_count = 0;

    while (server->is_running)
    {
        int health_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (health_sock == -1)
        {
            printf("socket creation failed. error code %d\n", E_SOCK);
            retry_count++;
            if (retry_count >= MAX_HEALTH_RETRIES)
            {
                pthread_mutex_lock(&server->lock);
                if (server->status == SS_STATUS_ACTIVE)
                {
                    server->status = SS_STATUS_INACTIVE;
                    printf("Storage Server %s:%d is down\n", server->ip, server->ns_port);
                }
                pthread_mutex_unlock(&server->lock);
                retry_count = 0;
                server->is_running = 0; 
            }
            sleep(HEALTH_CHECK_INTERVAL);
            break;
        }

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server->ns_port);
        inet_pton(AF_INET, server->ip, &server_addr.sin_addr);

        int connect_result = connect(health_sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
        if (connect_result == -1)
        {

            pthread_mutex_lock(&server->lock);
            if (server->status == SS_STATUS_ACTIVE)
            {
                server->status = SS_STATUS_INACTIVE;
                printf("Storage Server %s:%d is down\n", server->ip, server->ns_port);
            }
            pthread_mutex_unlock(&server->lock);
            retry_count = 0;
            server->is_running = 0; 
            break;
            close(health_sock);
        }

        char health_msg[] = "HEALTH_CHECK";
        if (send(health_sock, health_msg, strlen(health_msg), 0) == -1)
        {
            printf("send failed. error code %d\n", E_SEND);
            retry_count++;
            close(health_sock);
            sleep(HEALTH_CHECK_INTERVAL);
            continue;
        }

        char buffer[128] = {0};
        int received = recv(health_sock, buffer, sizeof(buffer) - 1, 0);
        pthread_mutex_lock(&server->lock);
        if (received > 0)
        {
            server->status = SS_STATUS_ACTIVE;
            server->last_seen = time(NULL);
            retry_count = 0;
        }
        else
        {
            retry_count++;
            if (retry_count >= MAX_HEALTH_RETRIES)
            {
                if (server->status == SS_STATUS_ACTIVE)
                {
                    server->status = SS_STATUS_INACTIVE;
                    printf("Storage Server %s:%d is down (no response)\n", server->ip, server->ns_port);
                }
                retry_count = 0;
                server->is_running = 0;
            }
        }
        pthread_mutex_unlock(&server->lock);

        // Cleanup
        close(health_sock);
        sleep(HEALTH_CHECK_INTERVAL);
    }

    return NULL;
}

#define MAX_SERVERS 100 // Maximum number of unique server numbers

// Structure to store a port pair and its corresponding server number
typedef struct
{
    int port1;
    int port2;
    int serverno;
} PortPair;

// Global array to track used server numbers and pairs
PortPair server_map[MAX_SERVERS];
int server_count = 0;                  // Number of unique server pairs stored
int used_servernos[MAX_SERVERS] = {0}; // 0 indicates server number is available
int used_server_list[MAX_SERVERS];     // List of used server numbers
int used_server_count = 0;

/**
 * Function to find the smallest available server number
 */
int get_available_serverno()
{
    for (int i = 0; i < MAX_SERVERS; i++)
    {
        if (used_servernos[i] == 0)
        {
            used_servernos[i]++; // Mark as used
            used_server_list[used_server_count++] = i;
            return i; // Return server number
        }
    }
    return -1; // No available server numbers
}

/**
 * Function to release a server number (optional)
 */
void release_serverno(int serverno)
{
    if (serverno >= 0 && serverno < MAX_SERVERS)
    {
        used_servernos[serverno] = 0; // Mark as available
    }
}
int is_serveno_used(int serverno)
{
    if (serverno >= 0 && serverno < MAX_SERVERS)
    {
        return used_servernos[serverno]; // Return 1 if used, 0 if not
    }
    return 0; // Invalid server number, treated as not used
}
/**
 * Function to get the server number for a given pair of ports
 */
int get_serverno(int port1, int port2)
{
    // Check if the pair already exists
    for (int i = 0; i < server_count; i++)
    {
        if (server_map[i].port1 == port1 && server_map[i].port2 == port2)
        {
            int n = server_map[i].serverno;
            used_servernos[n]++;
            return server_map[i].serverno; // Return existing server number
        }
    }

    // Assign a new server number
    int new_serveno = get_available_serverno();
    if (new_serveno == -1)
    {
        fprintf(stderr, "Error: No available server numbers.\n");
        return -1;
    }

    // Store the new pair and its server number
    server_map[server_count].port1 = port1;
    server_map[server_count].port2 = port2;
    server_map[server_count].serverno = new_serveno;
    server_count++;

    return new_serveno;
}

int select_backup_servers(int serverno, int *backup_servers)
{
    // Check if we have enough servers in the used_server_list excluding serverno
    if (used_server_count < 3) // At least two backups + the given serverno
    {
        return 0; // Not enough servers to select backups
    }

    // Seed random number generator
    srand(time(NULL));

    // Array to track selected indices
    int *selected_indices = calloc(MAX_SERVERS, sizeof(int));
    int selected_count = 0;

    // Find two unique backup servers excluding serverno
    while (selected_count < 2)
    {
        // Select a random index from used_server_list
        int random_index = rand() % used_server_count;
        int server_index = used_server_list[random_index]; // Get the server index from used_server_list

        // Ensure the selected server is distinct, alive, running, and not serverno
        if (selected_indices[server_index] == 0 &&
            server_index != serverno &&
            storage_servers[server_index].status == 1 &&   // Check if alive
            storage_servers[server_index].is_running == 1) // Check if running
        {
            backup_servers[selected_count] = server_index;
            selected_indices[server_index] = 1; // Mark server as selected
            selected_count++;
        }
    }

    free(selected_indices);
    return 0; // Success
}

// Function to start monitoring a new storage server
void start_server_monitoring(int server_index)
{
    StorageServerInfo *server = &storage_servers[server_index];
    server->is_running = 1;

    if (pthread_create(&server->monitor_thread, NULL, monitor_server, server) != 0)
    {
        printf("thread creation failed. error code %d\n", E_NOMEM);
        return;
    }
    pthread_detach(server->monitor_thread);
}

// Mod
void update_storage_server(char *ip, int port1, int port2, char *paths, int serverno)
{
    pthread_mutex_lock(&mutex);
    if (paths[strlen(paths) - 1] == ',')
    {
        paths[strlen(paths) - 1] = '\0';
    }

    char *savePtr;
    char *token = strtok_r(paths, ",", &savePtr);
    while (token != NULL)
    {
        char temp[1024];

        // Check if "/backup" is in the token
        if (strstr(token, "/backup") != NULL)
        {
            strcpy(storage_servers[serverno].back_up_path, token);
            storage_servers[serverno].back_up_path[sizeof(storage_servers[serverno].back_up_path) - 1] = '\0'; // Handle backup logic here, if needed
                                                                                                               // Find the position of "/backup" in the token
            char *backup_pos = strstr(token, "/backup");

            // Calculate the length of the base path (excluding "/backup")
            size_t base_path_length = backup_pos - token;

            // Copy the base path (excluding "/backup") into base_path
            if (base_path_length < BUFFER_SIZE)
            {
                strncpy(storage_servers[serverno].base_path, token, base_path_length);
                storage_servers[serverno].base_path[base_path_length] = '\0';
                // storage_servers->back_up_path[base_path_length] = '\0'; // Null-terminate the base path
            }
            int l = strlen(storage_servers[serverno].base_path) - 1;
            while (storage_servers[serverno].base_path[l] != '/')
            {
                --l;
            }
            storage_servers[serverno].base_path[l] = '\0';
        }

        strcpy(temp, token);
        insert(tries[serverno], temp);
        token = strtok_r(NULL, ",", &savePtr);
    }

    strcpy(storage_servers[serverno].ip, ip);
    storage_servers[serverno].ns_port = port1;
    storage_servers[serverno].client_port = port2;
    storage_servers[serverno].status = SS_STATUS_ACTIVE;
    storage_servers[serverno].last_seen = time(NULL);
    storage_servers[serverno].back_up = 0;
    storage_servers[serverno].back_up1 = 0;
    storage_servers[serverno].back_up2 = 0;
    start_server_monitoring(serverno);
    storage_server_count++;
    pthread_mutex_unlock(&mutex);
}

int find_storage_server_with_path(int num_servers, const char *target_path, TrieNode **head)
{
    printf("given %s\n", target_path);
    for (int i = 0; i < num_servers; i++)
    {
        if (searchPartial(tries[i], target_path, head))
        {
            return i;
        }
    }
    return -1;
}

// Cleanup function for a single server
void cleanup_server_monitoring(StorageServerInfo *server)
{
    server->is_running = 0;
    // Thread will clean itself up since we used pthread_detach
}

// Initialize the monitoring system
void init_storage_server_monitoring()
{
    for (int i = 0; i < MAX_SS; i++)
    {
        pthread_mutex_init(&storage_servers[i].lock, NULL);
        storage_servers[i].is_running = 0;
        storage_servers[i].status = SS_STATUS_INACTIVE;
    }
}

// Cleanup all servers
void cleanup_all_monitoring()
{
    for (int i = 0; i < storage_server_count; i++)
    {
        cleanup_server_monitoring(&storage_servers[i]);
    }

    for (int i = 0; i < MAX_SS; i++)
    {
        pthread_mutex_destroy(&storage_servers[i].lock);
    }
    pthread_mutex_destroy(&ss_count_mutex);
}

void printAccessiblePaths(TrieNode *node, char *currentPath, int depth)
{
    if (!node)
        return;

    if (node->part != NULL)
    {
        if (depth > 0)
            strcat(currentPath, "/");
        strcat(currentPath, node->part);
        // printf("%s\n", currentPath);
        if (!strstr(currentPath, "backup"))
        {
            strcat(list, currentPath);
            strcat(list, "\n");
        }
    }

    for (int i = 0; i < MAX_CHILDREN; i++)
    {
        if (node->children[i] != NULL)
        {
            char tempPath[1024];
            strcpy(tempPath, currentPath);
            printAccessiblePaths(node->children[i], tempPath, depth + 1);
        }
    }
}

void printAllPaths(TrieNode *root)
{
    for (int i = 0; i < MAX_CHILDREN; i++)
    {
        if (root->children[i] != NULL)
        {
            char currentPath[1024] = "";
            printAccessiblePaths(root->children[i], currentPath, 0);
        }
    }
}

void sendcommand1(char message[BUFFER_SIZE])
{
    struct sockaddr_in serv_addr;
    int sock = 0;
    printf("%s", message);
    char buffer[1024] = {0};
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        printf("socket creation failed. error code %d\n", E_SEND);
        // return EXIT_FAILURE;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, wifi_ip, &serv_addr.sin_addr) <= 0)
    {
        printf("invalid address. error code %d\n", E_NOADDR);
        // return EXIT_FAILURE;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        printf("connect failed. error code %d\n", E_CONN);
        // return EXIT_FAILURE;
    }

    printf("Connected to server %s:%d\n", wifi_ip, PORT);

    if (send(sock, message, strlen(message), 0) < 0)
    {
        printf("send failed. error code %d\n", E_SEND);
        close(sock);
        // return EXIT_FAILURE;
    }
    printf("Message sent: %s\n", message);

    close(sock);
}

void sendcommand2(char message[BUFFER_SIZE])
{
    struct sockaddr_in serv_addr;
    int sock = 0;
    char buffer[1024] = {0};
    // Create socket
    printf("%s", message);
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        printf("socket creation failed. error code %d\n", E_SOCK);
        // return EXIT_FAILURE;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, wifi_ip, &serv_addr.sin_addr) <= 0)
    {
        printf("invalid address. error code %d\n", E_NOADDR);
        // return EXIT_FAILURE;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        printf("connect failed. error code %d\n", E_CONN);
        // return EXIT_FAILURE;
    }

    printf("Connected to server %s:%d\n", wifi_ip, PORT);
    if (send(sock, message, strlen(message), 0) < 0)
    {
        printf("send failed. error code %d\n", E_SEND);
        close(sock);
        // return EXIT_FAILURE;
    }
    printf("Message sent: %s\n", message);

    close(sock);
}

void *thread_sendcommand1(void *arg)
{
    char *message = (char *)arg;
    printf("%s\n", message);
    sendcommand1(message);
    return NULL;
}

void *thread_sendcommand2(void *arg)
{
    char *message = (char *)arg;
    printf("%s\n", message);
    sendcommand2(message);
    return NULL;
}

int can_find_servers(int serverno)
{
    int count = 0;
    for (int i = 0; i < used_server_count; i++)
    {
        /* code */
        int n = used_server_list[i];
        if (storage_servers[n].status == 1 && (n != serverno))
        {
            count++;
        }
        // if(used_server_list[i])
    }
    if (count < 2)
    {
        return 0;
    }
    return count;
}

void backup(int serverno, char paths[BUFFER_SIZE])
{
    int backup_server_nums[2];
    if (can_find_servers(serverno) == 0)
    {
        printf("Can't find 2 running servers for backup\n");
        return;
    }
    int num_backups = select_backup_servers(serverno, backup_server_nums);
    storage_servers[serverno].back_up = 1;
    storage_servers[serverno].back_up1 = backup_server_nums[0];
    storage_servers[serverno].back_up2 = backup_server_nums[1];
    strcpy(storage_servers[serverno].paths, paths);
    char message1[8196];
    char message2[8196];
    char path1[BUFFER_SIZE];
    strcpy(path1, storage_servers[serverno].back_up_path);
    int l = strlen(path1) - 1;
    while (path1[l] != '/')
    {
        --l;
    }
    path1[l] = '\0';
    char path3[BUFFER_SIZE];
    strcpy(path3, path1);
    int l1 = strlen(path3) - 1;
    while (path3[l1] != '/')
    {
        --l1;
    }
    path3[l] = '\0';
    printf("%s\n", path1);
    char result[20];
    snprintf(result, 20, "ss%d", serverno);
    char cre1[1024];
    char cre2[1024];
    snprintf(cre1, sizeof(cre1), "CREATE %s %s", storage_servers[backup_server_nums[0]].back_up_path, result);
    snprintf(cre2, sizeof(cre2), "CREATE %s %s", storage_servers[backup_server_nums[1]].back_up_path, result);
    pthread_t thread1, thread2, thread3, thread4;
    if (pthread_create(&thread1, NULL, thread_sendcommand1, (void *)cre1) != 0)
    {
        printf("thread allocation failed. error code %d\n", E_NOMEM);
        exit(1);
    }
    if (pthread_create(&thread2, NULL, thread_sendcommand2, (void *)cre2) != 0)
    {
        printf("thread allocation failed. error code %d\n", E_NOMEM);
        exit(1);
    }

    snprintf(message1, sizeof(message1), "COPY %s %s/%s", path1, storage_servers[backup_server_nums[0]].back_up_path, result);
    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);

    // usleep(500000);
    sleep(1);
    char path2[BUFFER_SIZE];
    strcpy(path2, storage_servers[backup_server_nums[1]].back_up_path);
    printf("%s\n", path2);
    snprintf(message2, sizeof(message2), "COPY %s %s/%s", path1, storage_servers[backup_server_nums[1]].back_up_path, result);
    // Thread identifiers
    // sendcommand1(message1);
    // sendcommand1(message2);

    if (pthread_create(&thread3, NULL, thread_sendcommand1, (void *)message1) != 0)
    {
        printf("thread allocation failed. error code %d\n", E_NOMEM);
        exit(1);
    }
    if (pthread_create(&thread4, NULL, thread_sendcommand2, (void *)message2) != 0)
    {
        printf("thread allocation failed. error code %d\n", E_NOMEM);
        exit(1);
    }

    pthread_join(thread3, NULL);
    pthread_join(thread4, NULL);
}

char *remove_substring(char path[BUFFER_SIZE], char base[BUFFER_SIZE])
{
    if (!path || !base)
    {
        return NULL; // Handle null inputs
    }

    const char *sub_pos = strstr(path, base); // Find the position of base in path
    if (!sub_pos)
    {
        // Base not found, return a copy of the original path
        char *result = malloc(strlen(path) + 1);
        if (result)
        {
            strcpy(result, path);
        }
        return result;
    }

    // Calculate the new string size
    size_t new_length = strlen(path) - strlen(base);
    char *result = malloc(new_length + 1); // Allocate memory for the new string
    if (!result)
    {
        return NULL; // Memory allocation failed
    }

    // Copy the part before the substring
    size_t prefix_length = sub_pos - path;
    strncpy(result, path, prefix_length);

    // Copy the part after the substring
    strcpy(result + prefix_length, sub_pos + strlen(base));
    // printf("%s\n",result);
    return result;
}

int send_to_backup(char message1[], char message2[])
{
    pthread_t thread1, thread2, thread3, thread4;
    if (pthread_create(&thread1, NULL, thread_sendcommand1, (void *)message1) != 0)
    {
        printf("thread allocation failed. error code %d\n", E_NOMEM);
        exit(1);
    }
    if (pthread_create(&thread2, NULL, thread_sendcommand2, (void *)message2) != 0)
    {
        printf("thread allocation failed. error code %d\n", E_NOMEM);
        exit(1);
    }

    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);
    return 0;
}

int random_between_two(int num1, int num2)
{
    return (rand() % 2) == 0 ? num1 : num2; // Randomly return either num1 or num2
}

void *handle_connection(void *arg)
{
    int sock = *(int *)arg;
    free(arg);

    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE);
    int bytes_received = read(sock, buffer, BUFFER_SIZE - 1);
    if (bytes_received > 0)
    {
        buffer[bytes_received] = '\0'; // Null-terminate the received data
        // printf("Received: %s\n", buffer);
    }

    if (strncmp(buffer, "SS|", 3) == 0)
    {
        char ip[INET_ADDRSTRLEN] = {0};
        char paths[BUFFER_SIZE] = {0};
        int port1, port2 = 0;
        sscanf(buffer, "SS|IP:%15[^|]|Port1:%d|Port2:%d|Paths:%4096[^\n]", ip, &port1, &port2, paths);
        printf("Parsed: IP=%s, Port1=%d, Port2=%d, Paths=%s\n", ip, port1, port2, paths);
        char log_msg[BUFFER_SIZE];
        snprintf(log_msg, sizeof(log_msg),
                 "Registered SS IP: %s, NS-port: %d, Storage Server Client-port: %d",
                 ip, port1, port2);
        log_msg[sizeof(log_msg) - 1] = '\0';
        log_message(log_msg);
        int server_num = get_serverno(port1, port2);
        printf("%d\n", server_num);
        int n = is_serveno_used(server_num);
        if (n == 1)
        {
            update_storage_server(ip, port1, port2, paths, server_num);
        }
        else
        {
            storage_servers[server_num].status = SS_STATUS_ACTIVE;
            storage_servers[server_num].is_running = 1;
        }
        if (storage_server_count > 2 && (n == 1))
        {
            backup(server_num, paths);
            printf("%d %d %d %s %s %s\n", storage_servers[server_num - 1].back_up, storage_servers[server_num - 1].back_up1, storage_servers[server_num - 1].back_up2, storage_servers[server_num - 1].paths, storage_servers[storage_servers[server_num].back_up1].back_up_path, storage_servers[storage_servers[server_num].back_up2].back_up_path);
        }
        memset(log_msg, 0, sizeof(log_msg));
        snprintf(log_msg, sizeof(log_msg),
                 "Registered SS IP: %s, NS-port: %d, Storage Server Client-port: %d, backup: %d",
                 ip, port1, port2, storage_servers[storage_server_count - 1].back_up);

        // Ensure the command string is null-terminated
        log_msg[sizeof(log_msg) - 1] = '\0';
        log_message(log_msg);
        printf("Registered SS IP: %s, Port1: %d, Port2: %d, Paths: %s,base_path : %s\n", ip, port1, port2, paths, storage_servers[storage_server_count - 1].base_path);
        char command[BUFFER_SIZE];
        if (n > 1)
        {
            snprintf(command, sizeof(command), "Welcome back Storage Server %d", server_num);
        }
        else
        {
            snprintf(command, sizeof(command), "Registration Successful.You are Storage Server %d", server_num);
        }
        command[sizeof(command) - 1] = '\0';
        write(sock, command, strlen(command));
        close(sock);
    }
    else if (strstr(buffer, "CREATE") || (strstr(buffer, "DELETE")) || (strstr(buffer, "COPY")) || (strstr(buffer, "LIST_ALL_ACCESSIBLE_PATHS")))
    {
        log_message(buffer);
        char path[BUFFER_SIZE];
        char name[BUFFER_SIZE];
        int x = -1;
        int x2 = -1;
        int ss_sock;
        struct sockaddr_in ss_addr;
        if ((ss_sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        {
            perror("Socket creation failed");
            // free(buffer);
            exit(1);
        }

        ss_addr.sin_family = AF_INET;

        if (strstr(buffer, "CREATE"))
        {
            if (sscanf(buffer, "CREATE %s %s", path, name) == 2)
            {
                printf("Creating: %s with name: %s\n", path, name);
                TrieNode *head = NULL;
                if (!isEmpty(cache))
                {
                    printf("hi");
                    int c = get(cache, path);
                    if (c != -1)
                    {
                        x = cache->head->i;
                        printf("from cache\n");
                    }
                }
                if (x == -1)
                {
                    x = find_storage_server_with_path(storage_server_count, path, &head);
                    put(cache, path, x);
                }
                if (x == -1)
                {
                    printf("Invalid path. error code %d\n", E_INVAL);
                    char error_msg[] = "Invalid Path";
                    write(sock, error_msg, strlen(error_msg));
                    close(sock);
                    close(ss_sock);
                    return NULL;
                }
                else
                {
                    printf("%s path found in storage server %d\n",path, x);
                }
                if (!storage_servers[x].status)
                {
                    printf("Server is not  in online \n");
                    char error_msg[] = "Server is not inonline ";
                    write(sock, error_msg, strlen(error_msg));
                    close(sock);
                    close(ss_sock);
                    return NULL;
                }
                inet_pton(AF_INET, storage_servers[x].ip, &ss_addr.sin_addr);
                ss_addr.sin_port = htons(storage_servers[x].ns_port);
                printf("Attempting to connect to Storage Server at %s:%d\n", storage_servers[x].ip, storage_servers[x].ns_port);

                if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) == -1)
                {
                    printf("connect failed. error code %d\n", E_CONN);
                    char error_msg[] = "connect failed";
                    write(sock, error_msg, strlen(error_msg));
                    close(sock);
                    close(ss_sock);
                    // printf("Error Code: %d, Error Message: %s\n", errno, strerror(errno));
                    return NULL;
                    // continue;
                }
                // Ensure that snprintf does not overflow the buffer buffer
                int remaining_space = BUFFER_SIZE - strlen("CREATE ");
                if (remaining_space > 0)
                {
                    snprintf(buffer, remaining_space, "CREATE %s %s", path, name);
                }
                else
                {
                    printf("Path too large. error code %d\n", E_OVERFLOW);
                    char error_msg[] = "Path is too large";
                    write(sock, error_msg, strlen(error_msg));
                    close(sock);
                    close(ss_sock);
                    return NULL;
                }

                if (send(ss_sock, buffer, strlen(buffer), 0) == -1)
                {
                    printf("send failed. error code %d\n", E_SEND);
                    char error_msg[] = "Send failed";
                    write(sock, error_msg, strlen(error_msg));
                    close(sock);
                    close(ss_sock);
                    return NULL;
                }
                char buffer[BUFFER_SIZE];
                // int read_size = read(ss_sock, buffer, BUFFER_SIZE);
                while (1)
                {
                    int read_size = read(ss_sock, buffer, BUFFER_SIZE);
                    if (read_size > 0)
                    {
                        int serverno;
                        char path[BUFFER_SIZE];
                        char op[BUFFER_SIZE];
                        if (sscanf(buffer, "UPDATE_SS%d|Path:%[^|]|Operation:%s", &serverno, path, op) == 3)
                        {
                            log_message(buffer);
                            printf("inserted %s\n", path);
                            insert(tries[x], path);
                            break;
                            // update_storage_server_no(serverno, path, op);
                        }
                        else
                        {
                            printf("Failed to parse buffer %s. error code %d\n", buffer, E_SYNTAX);
                            break;
                        }
                    }
                }

                if (storage_servers[x].back_up == 1 && (!strstr(path, "backup")))
                {
                    char base[BUFFER_SIZE];
                    printf("%s\n", path);
                    strcpy(base, storage_servers[x].base_path);
                    printf("%s\n", base);
                    char *s = remove_substring(path, base);
                    char message1[BUFFER_SIZE];
                    char message2[BUFFER_SIZE];
                    int b1 = storage_servers[x].back_up1;
                    int b2 = storage_servers[x].back_up2;
                    printf("%d %d\n", b1, b2);
                    snprintf(message1, sizeof(message1), "CREATE %s/ss%d%s  %s", storage_servers[b1].back_up_path, x, s, name);
                    snprintf(message2, sizeof(message2), "CREATE %s/ss%d%s  %s", storage_servers[b2].back_up_path, x, s, name);
                    int c = send_to_backup(message1, message2);
                }
            }
        }
        else if (strstr(buffer, "DELETE"))
        {
            char path[BUFFER_SIZE];
            sscanf(buffer, "DELETE %s", path);
            printf("Deleting: %s\n", path);
            TrieNode *head = NULL;
            if (!isEmpty(cache))
            {
                printf("hi");
                int c = get(cache, path);
                if (c != -1)
                {
                    x = cache->head->i;
                    printf("from cache\n");
                }
            }
            if (x == -1)
            {
                x = find_storage_server_with_path(storage_server_count, path, &head);
                put(cache, path, x);
            }
            if (x == -1)
            {
                printf("Invalid path.\n");
                char error_msg[] = "Invalid Path";
                write(sock, error_msg, strlen(error_msg));
                close(sock);
                close(ss_sock);
                return NULL;
            }
            else
            {
                printf("%s path found in storage server %d\n",path, x);
            }
            if (!storage_servers[x].status)
            {
                printf("Server is not  in online \n");
                char error_msg[] = "Server is not inonline ";
                write(sock, error_msg, strlen(error_msg));
                close(sock);
                close(ss_sock);
                return NULL;
            }
            inet_pton(AF_INET, storage_servers[x].ip, &ss_addr.sin_addr);
            ss_addr.sin_port = htons(storage_servers[x].ns_port);
            printf("Attempting to connect to Storage Server at %s:%d\n", storage_servers[x].ip, storage_servers[x].ns_port);

            if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) == -1)
            {
                printf("connect failed. error code %d\n", E_CONN);
                // printf("Error Code: %d, Error Message: %s\n", errno, strerror(errno));
                char error_msg[] = "connect failed";
                write(sock, error_msg, strlen(error_msg));
                close(sock);
                close(ss_sock);
                return NULL;
                // continue;
            }

            int remaining_space = BUFFER_SIZE - strlen("DELETE ");
            if (remaining_space > 0)
            {
                snprintf(buffer, remaining_space, "DELETE %s", path);
            }
            else
            {
                printf("path too large. error code %d\n", E_OVERFLOW);
                char error_msg[] = "Path is too large";
                write(sock, error_msg, strlen(error_msg));
                close(sock);
                close(ss_sock);
                return NULL;
            }

            if (send(ss_sock, buffer, strlen(buffer), 0) == -1)
            {
                printf("send failed. error code %d\n", E_SEND);
                char error_msg[] = "Send failed";
                write(sock, error_msg, strlen(error_msg));
                close(sock);
                close(ss_sock);
                return NULL;
            }
            char buffer[BUFFER_SIZE];
            while (1)
            {
                int read_size = read(ss_sock, buffer, BUFFER_SIZE);
                if (read_size > 0)
                {
                    int serverno;
                    char path[BUFFER_SIZE];
                    char op[BUFFER_SIZE];
                    if (sscanf(buffer, "UPDATE_SS%d|Path:%[^|]|Operation:%s", &serverno, path, op) == 3)
                    {
                        log_message(buffer);
                        printf("%d %s %s\n", serverno, path, op);
                        printf("%s\n", buffer);
                        deletePath(tries[x], path);
                        removeFile(cache, path);
                        break;
                        // update_storage_server_no(serverno, path, op);
                    }
                    else
                    {
                        printf("Failed to parse buffer %s. error code %d\n", buffer, E_SYNTAX);
                        char error_msg[] = "Parsing error";
                        write(sock, error_msg, strlen(error_msg));
                        close(sock);
                        close(ss_sock);
                        return NULL;
                    }
                }
            }

            if (storage_servers[x].back_up == 1 && (!strstr(path, "backup")))
            {
                char base[BUFFER_SIZE];
                printf("%s\n", path);
                strcpy(base, storage_servers[x].base_path);
                printf("%s\n", base);
                char *s = remove_substring(path, base);
                char message1[BUFFER_SIZE];
                char message2[BUFFER_SIZE];
                int b1 = storage_servers[x].back_up1;
                int b2 = storage_servers[x].back_up2;

                snprintf(message1, sizeof(message1), "DELETE %s/ss%d%s", storage_servers[b1].back_up_path, x, s);
                snprintf(message2, sizeof(message2), "DELETE %s/ss%d%s", storage_servers[b2].back_up_path, x, s);
                int c = send_to_backup(message1, message2);
            }
        }
        else if (strstr(buffer, "COPY"))
        {
            char final_comm[8192];
            char *src = (char *)malloc(sizeof(char) * (100));
            char *dest = (char *)malloc(sizeof(char) * (100));
            sscanf(buffer, "COPY %s %s", src, dest);
            printf("%s %s\n", src, dest);
            char dup[1024];
            strcpy(dup, src);
            char dup2[1024];
            strcpy(dup2, dest);
            TrieNode *head1 = NULL;
            TrieNode *head2 = NULL;
            if (x == -1)
            {
                x = find_storage_server_with_path(storage_server_count, dest, &head1);
                // put(cache, dest, x);
            }

            if (x2 == -1)
            {
                x2 = find_storage_server_with_path(storage_server_count, src, &head2);
                // put(cache, src, x2);
            }
            if (x == -1)
            {
                printf("dest path not found. error code %d\n", E_INVAL);
                char error_msg[] = "Invalid Path";
                write(sock, error_msg, strlen(error_msg));
                close(sock);
                close(ss_sock);
                return NULL;
            }
            else
            {
                printf("dest path found in storage server %d", x);
            }
            if (x2 == -1)
            {
                printf("src path not found. error code %d\n", E_INVAL);
                char error_msg[] = "Invalid Path";
                write(sock, error_msg, strlen(error_msg));
                close(sock);
                close(ss_sock);
                return NULL;
            }
            else
            {
                printf("src path found in storage server %d", x2);
            }
            if (!storage_servers[x].status || !storage_servers[x].status)
            {
                printf("Server %d is not in online \n", x);
                char error_msg[BUFFER_SIZE];
                snprintf(error_msg, sizeof(error_msg), "Server %d is not  in online", x);
                write(sock, error_msg, strlen(error_msg));
                close(sock);
                close(ss_sock);
                return NULL;
            }
            printf("%d %d\n", head1->file_flag, head2->file_flag);
            if (!head1->file_flag && !head2->file_flag)
            {
                char *command = (char *)malloc(4096 * sizeof(char));
                command[0] = '\0';
                getpaths(&head2, command);
                strcat(command, " ");
                strcat(command, dup);
                strcat(command, " ");
                strcat(command, dup2);
                snprintf(final_comm, sizeof(final_comm), "COPY %s %s %d %d", command, storage_servers[x2].ip, storage_servers[x2].client_port, head1->file_flag * head2->file_flag);
                printf("%s\n", final_comm);
            }
            else if (head1->file_flag && head2->file_flag)
            {
                char final_comm[8192];
                snprintf(final_comm, sizeof(final_comm), "COPY %s %s %s %d %d", src, dest, storage_servers[x2].ip, storage_servers[x2].client_port, head1->file_flag * head2->file_flag);
            }
            else
            {
                printf("invalid combination. error code %d\n", E_SYNTAX);
                char error_msg[] = "Invalid combination for COPY";
                write(sock, error_msg, strlen(error_msg));
                close(sock);
                close(ss_sock);
                return NULL;
            }
            inet_pton(AF_INET, storage_servers[x].ip, &ss_addr.sin_addr);
            ss_addr.sin_port = htons(storage_servers[x].ns_port);
            printf("Attempting to connect to Storage Server at %s:%d\n", storage_servers[x].ip, storage_servers[x].ns_port);

            if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) == -1)
            {
                printf("connect failed. error code %d\n", E_CONN);
                char error_msg[] = "connect failed";
                write(sock, error_msg, strlen(error_msg));
                close(sock);
                // printf("Error Code: %d, Error Message: %s\n", errno, strerror(errno));
                close(ss_sock);
                return NULL;
                // continue;
            }

            if (send(ss_sock, final_comm, strlen(final_comm), 0) == -1)
            {
                printf("send failed. error code %d\n", E_SEND);
                char error_msg[] = "Send failed";
                write(sock, error_msg, strlen(error_msg));
                close(sock);
                close(ss_sock);
                return NULL;
            }

            while (1)
            {
                memset(buffer, 0, BUFFER_SIZE);
                int read_size = read(ss_sock, buffer, BUFFER_SIZE);
                if (read_size > 0)
                {
                    printf("%s\n", buffer);
                    int serverno;
                    char path[BUFFER_SIZE];
                    char op[6];
                    if (sscanf(buffer, "UPDATE_SS%d|Path:%[^|]|Operation:%s", &serverno, path, op) == 3)
                    {
                        log_message(buffer);
                        printf("%d %s %s\n", serverno, path, op);
                        // update_storage_server_no(serverno, path, op);
                    }
                    else
                    {
                        printf("Failed to parse buffer %s. error code %d\n", buffer, E_SYNTAX);
                        char error_msg[] = "Parsing error";
                        write(sock, error_msg, strlen(error_msg));
                        close(sock);
                        close(ss_sock);
                        return NULL;
                    }

                    if (strcmp(op, "CREATE") == 0)
                    {
                        insert(tries[x], path);
                        printf("Inserted %s into trie\n", path);
                    }

                    if (strcmp(op, "COPIED") == 0)
                    {
                        // printf("Copied Succesfully.\n");
                        break;
                    }
                }
            }

            printf("%s is copied to %s\n", src, dest);
            if (storage_servers[x].back_up == 1 && (!strstr(dest, "backup")))
            {
                char base[BUFFER_SIZE];
                printf("%s\n", dest);
                strcpy(base, storage_servers[x].base_path);
                printf("%s\n", base);
                char *s = remove_substring(dest, base);
                char message1[BUFFER_SIZE];
                char message2[BUFFER_SIZE];
                int b1 = storage_servers[x].back_up1;
                int b2 = storage_servers[x].back_up2;

                snprintf(message1, sizeof(message1), "COPY %s %s/ss%d%s", src, storage_servers[b1].back_up_path, x, s);
                snprintf(message2, sizeof(message2), "COPY %s %s/ss%d%s", src, storage_servers[b2].back_up_path, x, s);
                int c = send_to_backup(message1, message2);
            }
        }
        else if (strstr(buffer, "LIST_ALL_ACCESSIBLE_PATHS"))
        {
            memset(list, 0, sizeof(list));
            for (int i = 0; i < storage_server_count; i++)
            {
                if (storage_servers[i].is_running)
                {
                    printAllPaths(tries[i]);
                }
            }

            write(sock, list, strlen(list));
            close(ss_sock);
            close(sock);
            return NULL;
        }
        else
        {
            printf("Unknown buffer %s. error code %d\n", buffer, E_INVAL);
            char error_msg[] = "Unknown buffer";
            write(sock, error_msg, strlen(error_msg));
            close(sock);
            close(ss_sock);
            return NULL;
        }
        close(ss_sock); //
        char command[BUFFER_SIZE];
        // snprintf(command, sizeof(command), "%s %d", storage_servers[0].ip, storage_servers[0].client_port);
        snprintf(command, sizeof(command), "done");
        command[sizeof(command) - 1] = '\0';
        write(sock, command, strlen(command));
        close(sock);
    }
    else if (strstr(buffer, "is disconnected"))
    {
        log_message(buffer);
        printf("%s\n", buffer);
        int y = -1;
        if (sscanf(buffer, "Server no %d is disconnected", &y) == 1)
        {
            printf("%d\n", y);
            storage_servers[y].status = SS_STATUS_INACTIVE;
            storage_servers[y].is_running = 0;
        }
        close(sock);
    }
    else
    {
        printf("Client Command: %s\n", buffer);
        int x = -1;
        TrieNode *head = NULL;
        int issync;
        if (strstr(buffer, "READ") || strstr(buffer, "STREAM") || strstr(buffer, "GET") || strstr(buffer, "WRITE"))
        {
            log_message(buffer);
            char command[10]; // Enough space for the
            char path[256];
            char data[256];
            // int issync;
            if (strstr(buffer, "WRITE"))
            {

                if (sscanf(buffer, "%s %s %[^\n]", command, path, data) == 3)
                {
                    printf("Command: %s\n", command);
                    printf("Path: %s\n", path);
                    /////////////////////////////////
                    char *sync = data + (strlen(data) - 1);
                    issync = *sync - '0';
                    *sync = '\0';
                    printf("%d\n", issync);

                    if (!isEmpty(cache))
                    {
                        int c = get(cache, path);
                        if (c != -1)
                        {
                            x = cache->head->i;
                            printf("from cache\n");
                        }
                    }

                    if (x == -1)
                    {
                        x = find_storage_server_with_path(storage_server_count, path, &head);
                        put(cache, path, x);
                    }
                    if (x == -1)
                    {
                        printf("path not found. error code %d\n", E_INVAL);
                        char error_msg[] = "Invalid Path";
                        write(sock, error_msg, strlen(error_msg));
                        close(sock);
                        return NULL;
                    }
                    else
                    {
                        printf("%s path found in storage server %d\n",path, x);
                    }
                    if (!storage_servers[x].status)
                    {
                        printf("Server is not  in online \n");
                        char error_msg[] = "Server is not in online";
                        write(sock, error_msg, strlen(error_msg));
                        close(sock);
                        // close(ss_sock);
                        return NULL;
                    }
                }
                else
                {
                    printf("Invalid format!.error code %d\n", E_SYNTAX);
                    char error_msg[] = "Invalid Path";
                    write(sock, error_msg, strlen(error_msg));
                    close(sock);
                    return NULL;
                }

                if (storage_servers[x].back_up == 1 && (!strstr(path, "backup")))
                {
                    char base[BUFFER_SIZE];
                    printf("%s\n", path);
                    strcpy(base, storage_servers[x].base_path);
                    printf("%s\n", base);
                    char *s = remove_substring(path, base);
                    char message1[BUFFER_SIZE];
                    char message2[BUFFER_SIZE];
                    int b1 = storage_servers[x].back_up1;
                    int b2 = storage_servers[x].back_up2;

                    snprintf(message1, sizeof(message1), "WRITE %s/ss%d%s %s %d", storage_servers[b1].back_up_path, x, s, data, issync);
                    snprintf(message2, sizeof(message2), "WRITE %s/ss%d%s %s %d", storage_servers[b2].back_up_path, x, s, data, issync);
                    int c = send_to_backup(message1, message2);
                }
            }
            else if (sscanf(buffer, "%s %s", command, path) == 2)
            {
                printf("Command: %s\n", command);
                printf("Path: %s\n", path);
                if (!isEmpty(cache))
                {
                    int c = get(cache, path);
                    if (c != -1)
                    {
                        x = cache->head->i;
                        printf("from cache\n");
                    }
                }
                if (x == -1)
                {
                    x = find_storage_server_with_path(storage_server_count, path, &head);
                    put(cache, path, x);
                }
                if (x == -1)
                {
                    printf("path not found. error code %d\n", E_INVAL);
                    char error_msg[] = "Invalid Path";
                    write(sock, error_msg, strlen(error_msg));
                    close(sock);
                    return NULL;
                }
                else
                {
                    printf("%s path found in storage server %d\n", path, x);
                }
                if (!storage_servers[x].status)
                {
                    if (strstr(command, "READ"))
                    {
                        int r = random_between_two(storage_servers[x].back_up1, storage_servers[x].back_up2);
                        char base[BUFFER_SIZE];
                        printf("%s\n", path);
                        strcpy(base, storage_servers[x].base_path);
                        printf("%s\n", base);
                        char *s = remove_substring(path, base);
                        char message1[BUFFER_SIZE];
                        char message2[BUFFER_SIZE];
                        int b1 = storage_servers[x].back_up1;
                        int b2 = storage_servers[x].back_up2;
                        if (!storage_servers[r].status)
                        {
                            if (r == b1)
                            {
                                r = b2;
                            }
                            else
                            {
                                r = b1;
                            }
                        }
                        printf("hi");
                        snprintf(message1, sizeof(message1), "%s %d READ %s/ss%d%s", storage_servers[r].ip, storage_servers[r].client_port, storage_servers[r].back_up_path, x, s);
                        // snprintf(message2, sizeof(message2), "WRITE %s/ss%d%s %s %d", storage_servers[b2].back_up_path, x, s, data, issync);
                        // int c = send_to_backup(message1, message2);
                        printf("%s", message1);
                        write(sock, message1, strlen(message1));
                        close(sock);
                    }
                    else
                    {
                        printf("Server is not  in online \n");
                        char error_msg[] = "Server is not in online";
                        write(sock, error_msg, strlen(error_msg));
                        close(sock);
                        return NULL;
                    }
                }
            }
            else
            {
                printf("Invalid format! error code %d\n", E_SYNTAX);
                char error_msg[] = "Invalid format";
                write(sock, error_msg, strlen(error_msg));
                close(sock);
                return NULL;
            }
        }
        else
        {
            log_message(buffer);
            printf("Invalid format! error code %d\n", E_SYNTAX);
            char error_msg[] = "Invalid format";
            write(sock, error_msg, strlen(error_msg));
            close(sock);
            return NULL;
        }
        char command[BUFFER_SIZE];
        printf("%s\n", storage_servers[x].ip);
        snprintf(command, sizeof(command), "%s %d", storage_servers[x].ip, storage_servers[x].client_port);
        command[sizeof(command) - 1] = '\0';
        write(sock, command, strlen(command));
        close(sock);
    }
    return NULL;
}

char *get_local_ip()
{
    static char ip[INET_ADDRSTRLEN];
    struct sockaddr_in sa;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    // Check if the socket is created
    if (sock < 0)
    {
        printf("socket creation failed. error code %d\n", E_SOCK);
        exit(1);
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(80);
    // Try connecting to an external address to determine local IP
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == 0)
    {
        socklen_t len = sizeof(sa);
        if (getsockname(sock, (struct sockaddr *)&sa, &len) == -1)
        {
            printf("getsockname failed. error code %d\n", E_SOCKNAME);
            exit(1);
        }
        inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof(ip));
    }
    close(sock);
    return ip;
}

void print_ipv4_addresses()
{
    struct ifaddrs *ifaddr, *ifa;
    int family;
    char host[NI_MAXHOSt];
    printf("Server IPv4 Addresses:\n");
    // Get the list of network interfaces
    if (getifaddrs(&ifaddr) == -1)
    {
        printf("getifaddrs failed. error code %d\n", E_GETIF);
        exit(EXIT_FAILURE);
    }
    // Iterate through the linked list of network interfaces
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == NULL)
        {
            continue; // Skip if address is NULL
        }
        family = ifa->ifa_addr->sa_family;
        // Only consider IPv4 addresses (AF_INET)
        if (family == AF_INET)
        {
            // Get the address in string format
            int s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                                host, NI_MAXHOSt,
                                NULL, 0, NI_NUMERICHOSt);
            if (s != 0)
            {
                fprintf(stderr, "getnameinfo() failed: %s\n", gai_strerror(s));
                continue; // Skip if getnameinfo fails
            }
            printf("  %s: %s\n", ifa->ifa_name, host);
        }
    }
    // Free the linked list of network interfaces
    freeifaddrs(ifaddr);
}
void get_dynamic_ip(char *ip_buffer, size_t buffer_size)
{
    struct ifaddrs *ifaddr, *ifa;

    if (getifaddrs(&ifaddr) == -1)
    {
        printf("getifaddrs error. error code %d\n", E_GETIF);
        exit(EXIT_FAILURE);
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == NULL)
            continue;

        // Check for IPv4 addresses
        if (ifa->ifa_addr->sa_family == AF_INET)
        {
            // Ignore loopback interface
            if (strcmp(ifa->ifa_name, "lo") != 0)
            {
                struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;

                // Use inet_ntop to convert address to string
                if (inet_ntop(AF_INET, &addr->sin_addr, ip_buffer, buffer_size) == NULL)
                {
                    printf("inet_ntop error. error code %d\n", E_NTOP);
                    exit(EXIT_FAILURE);
                }
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
}

int main()
{
    get_dynamic_ip(wifi_ip, sizeof(wifi_ip));
    cache = createCache();

    for (int i = 0; i < MAX_SS; i++)
    {
        tries[i] = createNode(NULL);
    }

    int sockfd, new_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len;
    pthread_t tid, term_tid;

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        printf("socket failed. error code %d", E_SOCK);
        exit(1);
    }
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        printf("setsockopt error. error code %d\n", E_SOCKOPT);
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        printf("bind failed. error code %d\n", E_BIND);
        exit(1);
    }

    listen(sockfd, 10);
    print_ipv4_addresses();
    // printf("%s\n",wifi_ip);
    printf("Naming Server running on Port: %d\n", ntohs(server_addr.sin_port));
    init_storage_server_monitoring();
    // Main loop to accept connections
    while (1)
    {
        addr_len = sizeof(client_addr);
        int *client_socket = malloc(sizeof(int));
        *client_socket = accept(sockfd, (struct sockaddr *)&client_addr, &addr_len);

        if (*client_socket == -1)
        {
            printf("accept failed. error code %d\n", E_ACPT);
            free(client_socket);
            continue;
        }

        // Create a thread to handle each connection
        if (pthread_create(&tid, NULL, handle_connection, client_socket) != 0)
        {
            printf("thread allocation failed. error code %d\n", E_NOMEM);
            free(client_socket);
        }
        pthread_detach(tid); // Detach to avoid memory leaks
    }
    cleanup_all_monitoring();
    // Close the socket
    close(sockfd);
    return 0;
}