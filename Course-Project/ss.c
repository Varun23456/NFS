#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/time.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <signal.h>
#include <netdb.h>
#include "errors.h"
#define BUFFER_SIZE 8192
#define MAX_BUFFER_SIZE 16384
#define MAXI_BUFFER_SIZE 500000
#define NS_PORT 8080
#define DT_DIr 4
int serverno;
int client_sock, nm_sock, ns_sock;
struct sockaddr_in nm_addr, client_addr, ns_addr;
pthread_mutex_t ns_lock = PTHREAD_MUTEX_INITIALIZER;

#define CLIENT_PORT 9004

#define NM_PORT 9003


typedef struct
{
    char path[4096];
    int wcnt;
    int rcnt;
    int dflag;
    pthread_mutex_t file_lock;
    pthread_cond_t file_cond;
} files_path;

files_path files[100];
int file_cnt = 0;

int send_update_to_ns(const char *path, const char *op)
{
    char buffer[BUFFER_SIZE];
    snprintf(buffer, BUFFER_SIZE, "UPDATE_SS%d|Path:%s|Operation:%s", serverno, path, op);
    send(nm_sock, buffer, strlen(buffer), 0);
    return 0;
}

void read_path(const char *path, int new_client_sock)
{
    int flag = 0;
    int ind = -1;
    for (int i = 0; i < file_cnt; i++)
    {
        if (strcmp(path, files[i].path) == 0)
        {
            ind = i;
            if (files[i].dflag == 1)
            {
                return;
            }

            if (files[i].wcnt >= 1)
            {
                flag = 1;
            }
            else
            {
                files[i].rcnt++;
            }
            break;
        }
    }

    pthread_mutex_lock(&files[ind].file_lock);
    if (flag)
    {
        while (files[ind].wcnt >= 1)
        {
            pthread_cond_wait(&files[ind].file_cond, &files[ind].file_lock);
        }
        files[ind].rcnt++;
    }

    pthread_mutex_unlock(&files[ind].file_lock);
    printf("Reading from %s \n", path);
    int fp = open(path, O_RDONLY);
    if (fp == -1)
    {
        printf("error opening file.error code %d\n",E_FOPEN);
        char response[MAX_BUFFER_SIZE];
        snprintf(response, MAX_BUFFER_SIZE, "ERR|Unable to read file: %s", strerror(errno));
        send(client_sock, response, strlen(response), 0);
        return;
    }

    // fseek(fp, 0, SEEK_END);
    // long file_size = ftell(fp);
    // fseek(fp, 0, SEEK_SET);
    // printf("File size: %ld\n", file_size);

    char response[MAX_BUFFER_SIZE];

    char buffer[MAX_BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = read(fp, buffer, sizeof(buffer))) > 0)
    {
        send(new_client_sock, buffer, bytes_read, 0);
    }
    close(fp);
    printf("File closed\n");
    // if (send(new_client_sock, "STOP", 4, 0) == -1)
    // {
    //     printf("send failed. error code %d\n",E_SEND);
    // }

    pthread_mutex_lock(&files[ind].file_lock);
    files[ind].rcnt--;

    pthread_cond_signal(&files[ind].file_cond);
    pthread_mutex_unlock(&files[ind].file_lock);
    return;
}
////////////////////////////////////////////////////
typedef struct pending_write
{
    char *path;
    char *data;
    size_t data_length;
    int client_sock;
    int is_sync;
    struct pending_write *next;
} pending_write_t;

void write_path(const char *path, const char *data, int new_client_sock, int is_sync)
{
    int flag = 0;
    int ind = -1;

    for (int i = 0; i < file_cnt; i++)
    {
        if (strcmp(path, files[i].path) == 0)
        {
            ind = i;
            if (files[i].dflag == 1)
            {
                return;
            }

            if (files[i].wcnt >= 1 || files[i].rcnt >= 1)
            {
                flag = 1;
            }
            else
            {
                files[i].wcnt++;
            }
            break;
        }
    }

    pthread_mutex_lock(&files[ind].file_lock);
    if (flag)
    {
        while (files[ind].wcnt >= 1 || files[ind].rcnt >= 1)
        {
            pthread_cond_wait(&files[ind].file_cond, &files[ind].file_lock);
        }
        files[ind].wcnt++;
    }
    
    size_t data_length = strlen(data);
    // If synchronous write or small data size (e.g., < 1MB), write immediately
    // if (is_sync || data_length > (1024 * 1024))
    if (is_sync || data_length < (1024 * 1024))
    {
        printf("Writting instantely\n");
        FILE *fp = fopen(path, "wb");
        if (fp == NULL)
        {
            char response[MAX_BUFFER_SIZE];
            snprintf(response, MAX_BUFFER_SIZE, "ERR|Unable to write file: %s", strerror(errno));
            send(new_client_sock, response, strlen(response), 0);
            if (send(new_client_sock, "STOP", 4, 0) == -1)
            {
                printf("send failed. error code %d\n",E_SEND);
            }
            files[ind].wcnt--;
            pthread_mutex_unlock(&files[ind].file_lock);
            pthread_cond_signal(&files[ind].file_cond);
            return;
        }

        size_t bytes_written = fwrite(data, 1, data_length, fp);
        fclose(fp);

        char response[MAX_BUFFER_SIZE];
        if (bytes_written != data_length)
        {
            snprintf(response, MAX_BUFFER_SIZE, "ERR|Unable to write all data to file: %s", strerror(errno));
        }
        else
        {
            snprintf(response, MAX_BUFFER_SIZE, "ACK|Data written successfully");
        }
        send(new_client_sock, response, strlen(response), 0);
        if (send(new_client_sock, "STOP", 4, 0) == -1)
        {
            printf("send failed. error code %d\n",E_SEND);
        }
    }
    else
    {
        printf("writting asynchrounsaly\n");
        pending_write_t *write = malloc(sizeof(pending_write_t));
        write->path = strdup(path);
        write->data = malloc(data_length);
        memcpy(write->data, data, data_length);
        write->data_length = data_length;
        write->client_sock = new_client_sock;
        // write->is_sync = is_sync;
        // write->next = NULL;

        char response[MAX_BUFFER_SIZE] = "ACK|Write request accepted for processing";
        send(new_client_sock, response, strlen(response), 0);
        if (send(new_client_sock, "STOP", 4, 0) == -1)
        {
            printf("send failed. error code %d\n",E_SEND);
        }
        ///////////////////////
        close(new_client_sock);
        ///////////////////////
        FILE *fp = fopen(write->path, "wb");
        if (fp == NULL)
        {
            printf("failed to open file. error code %d\n",E_FOPEN);
            free(write->data);
            free(write->path);
            free(write);
        }

        if (fp != NULL)
        {
            // size_t chunk_size = 1024 * 1024; // 1MB chunks
            size_t chunk_size = 5;
            size_t bytes_remaining = write->data_length;
            char *current_data = write->data;

            while (bytes_remaining > 0)
            {
                size_t to_write = (bytes_remaining > chunk_size) ? chunk_size : bytes_remaining;
                sleep(10);
                fwrite(current_data, 1, to_write, fp);
                fflush(fp);
                sleep(2);
                current_data += to_write;
                bytes_remaining -= to_write;
                printf("bytes remaining %ld\n",bytes_remaining);
            }

            fclose(fp);
        }
    }
    files[ind].wcnt--;
    pthread_cond_signal(&files[ind].file_cond);
    pthread_mutex_unlock(&files[ind].file_lock);
}

void permission_path(const char *path, int new_client_sock)
{
    struct stat file_stat;
    if (stat(path, &file_stat) != 0)
    {
        perror("Error retrieving file stats");
        char response[MAX_BUFFER_SIZE];
        snprintf(response, MAX_BUFFER_SIZE, "ERR|Unable to retrieve file stats for: %s", strerror(errno));
        send(new_client_sock, response, strlen(response), 0);
        if (send(new_client_sock, "STOP", 4, 0) == -1)
        {
            printf("send failed. error code %d\n",E_SEND);
        }
        return;
    }

    char permissions[10];
    snprintf(permissions, sizeof(permissions), "%c%c%c%c%c%c%c%c%c",
             (file_stat.st_mode & S_IRUSR) ? 'r' : '-',
             (file_stat.st_mode & S_IWUSR) ? 'w' : '-',
             (file_stat.st_mode & S_IXUSR) ? 'x' : '-',
             (file_stat.st_mode & S_IRGRP) ? 'r' : '-',
             (file_stat.st_mode & S_IWGRP) ? 'w' : '-',
             (file_stat.st_mode & S_IXGRP) ? 'x' : '-',
             (file_stat.st_mode & S_IROTH) ? 'r' : '-',
             (file_stat.st_mode & S_IWOTH) ? 'w' : '-',
             (file_stat.st_mode & S_IXOTH) ? 'x' : '-');

    off_t size = file_stat.st_size;

    char access_time[30], modify_time[30], change_time[30];
    strftime(access_time, sizeof(access_time), "%Y-%m-%d %H:%M:%S", localtime(&file_stat.st_atime));
    strftime(modify_time, sizeof(modify_time), "%Y-%m-%d %H:%M:%S", localtime(&file_stat.st_mtime));
    strftime(change_time, sizeof(change_time), "%Y-%m-%d %H:%M:%S", localtime(&file_stat.st_ctime));

    char *file_type;
    if (S_ISREG(file_stat.st_mode))
        file_type = "Regular file";
    else if (S_ISDIR(file_stat.st_mode))
        file_type = "Directory";
    else if (S_ISLNK(file_stat.st_mode))
        file_type = "Symbolic link";
    else if (S_ISCHR(file_stat.st_mode))
        file_type = "Character device";
    else if (S_ISBLK(file_stat.st_mode))
        file_type = "Block device";
    else if (S_ISFIFO(file_stat.st_mode))
        file_type = "FIFO/pipe";
    else if (S_ISSOCK(file_stat.st_mode))
        file_type = "Socket";
    else
        file_type = "Unknown";

    // printf("Metadata for path: %s\n", path);
    // printf("File Type: %s\n", file_type);
    // printf("File Permissions: %s\n", permissions);
    // printf("File Size: %ld bytes\n", size);
    // printf("Access Time: %s\n", access_time);
    // printf("Modify Time: %s\n", modify_time);
    // printf("Change Time: %s\n", change_time);

    char response[MAX_BUFFER_SIZE];
    snprintf(response, MAX_BUFFER_SIZE,
             "FileType|%s\nFile_Permissions|%s\nSize|%ld bytes\nAccessTime|%s\nModifyTime|%s\nChangeTime|%s",
             file_type, permissions, size, access_time, modify_time, change_time);

    send(new_client_sock, response, strlen(response), 0);
    if (send(new_client_sock, "STOP", 4, 0) == -1)
    {
        printf("send failed. error code %d\n",E_SEND);
    }
}

void file_size_path(const char *path, int new_client_sock)
{
    struct stat file_stat;
    if (stat(path, &file_stat) != 0)
    {
        perror("Error retrieving file stats");
        char response[MAX_BUFFER_SIZE];
        snprintf(response, MAX_BUFFER_SIZE, "ERR|Unable to retrieve file size: %s", strerror(errno));
        send(new_client_sock, response, strlen(response), 0);
        if (send(new_client_sock, "STOP", 4, 0) == -1)
        {
            printf("send failed. error code %d\n",E_SEND);
        }
        return;
    }

    if (S_ISREG(file_stat.st_mode))
    {
        char response[MAX_BUFFER_SIZE];
        snprintf(response, MAX_BUFFER_SIZE, "SIZE|%ld", file_stat.st_size);
        send(new_client_sock, response, strlen(response), 0);
        printf("Sent file size: %ld bytes\n", file_stat.st_size);
    }
    else
    {
        char response[MAX_BUFFER_SIZE] = "ERR|Not a regular file";
        send(new_client_sock, response, strlen(response), 0);
        printf("Path is not a regular file\n");
    }
    if (send(new_client_sock, "STOP", 4, 0) == -1)
    {
        printf("send failed. error code %d\n",E_SEND);
    }
}

void stream_path(const char *path, int new_client_sock)
{
    printf("Streaming the audio from the path %s\n", path);
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
    {
        char response[MAXI_BUFFER_SIZE];
        snprintf(response, MAXI_BUFFER_SIZE, "ERR|Unable to stream file: %s", strerror(errno));
        send(new_client_sock, response, strlen(response), 0);
        if (send(new_client_sock, "STOP", 4, 0) == -1)
        {
            printf("send failed. error code %d\n",E_SEND);
        }
        return;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char response[MAXI_BUFFER_SIZE];
    // snprintf(response, MAXI_BUFFER_SIZE, "DATA|%ld|", file_size);
    // if (send(new_client_sock, response, strlen(response), 0) == -1) {
    //     perror("Failed to send file size to client");
    //     fclose(fp);
    //     return;
    // }

    char buffer[MAXI_BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, MAXI_BUFFER_SIZE, fp)) > 0)
    {
        if (send(new_client_sock, buffer, bytes_read, 0) == -1)
        {
            printf("send failed. error code %d\n",E_SEND);
            fclose(fp);
            return;
        }
    }

    // if (send(new_client_sock, "STOP", 4, 0) == -1)
    // {
    //     perror("Failed to send end-of-stream signal");
    // }

    fclose(fp);
    printf("Streaming complete for %s\n", path);
}

void *real_handle_cl(void *arg)
{
    int new_client_sock = *(int *)arg;
    free(arg);
    const char *base_directory = "";
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE);
    int is_sync;
    int read_size = read(new_client_sock, buffer, BUFFER_SIZE);
    if (read_size > 0)
    {
        printf("Client request: %s\n", buffer);
        char command[BUFFER_SIZE];
        command[sizeof(command) - 1] = '\0';
        if (strstr(buffer, "READ"))
        {
            char path[BUFFER_SIZE];
            if (sscanf(buffer, "READ %s", path) == 1)
            {
                printf("READING from  %s \n", path);

                char full_path[BUFFER_SIZE];
                snprintf(full_path, BUFFER_SIZE, "%s%s", base_directory, path);
                read_path(full_path, new_client_sock);
                // send_update_to_client(full_path, "READ");
                // send_update_to_ns(path, "ADD"); // This could also include the name depending on your logic
            }
        }
        else if (strstr(buffer, "WRITE"))
        {
            char path[BUFFER_SIZE];
            char data[BUFFER_SIZE];
            // int is_sync;
            if (sscanf(buffer, "WRITE %s %[^\n]", path, data) == 2)
            {
                char *sync = data + (strlen(data) - 1);
                is_sync = *sync - '0';
                *sync = '\0';

                size_t data_length = strlen(data);
                if (data_length >= 1024 * 1024)
                {
                    is_sync = 0;
                }
                char full_path[BUFFER_SIZE];
                snprintf(full_path, BUFFER_SIZE, "%s%s", base_directory, path);
                printf("writing %s to %s", data, path);
                write_path(path, data, new_client_sock, is_sync);
                // delete_path(path); // Call to delete the specified file/directory
                // send_update_to_ns(path, "DELETE");
                // send_update_to_client(path, "WRITE");
            }
        }
        else if (strstr(buffer, "GET"))
        {
            char path[BUFFER_SIZE];
            sscanf(buffer, "GET %s", path);
            char full_path[BUFFER_SIZE];
            snprintf(full_path, BUFFER_SIZE, "%s%s", base_directory, path);
            printf("Getting the permisssion %s \n", path);
            // copy_file(source_path, dest_path); // Call to copy file
            permission_path(full_path, new_client_sock);
            // send_update_to_client(path, "GET");
            // send_update_to_ns(dest_path, "ADD");
        }
        else if (strstr(buffer, "FILE_SIZE"))
        {
            char path[BUFFER_SIZE];
            sscanf(buffer, "FILE_SIZE %s", path);
            char full_path[BUFFER_SIZE];
            snprintf(full_path, BUFFER_SIZE, "%s%s", base_directory, path);
            printf("Getting the size %s \n", path);
            file_size_path(full_path, new_client_sock);
        }
        else if (strstr(buffer, "STREAM"))
        {
            char path[BUFFER_SIZE];
            sscanf(buffer, "STREAM %s", path);
            char full_path[BUFFER_SIZE];
            snprintf(full_path, BUFFER_SIZE, "%s%s", base_directory, path);
            printf("streaming the %s", path);
            stream_path(full_path, new_client_sock);
            // send_update_to_client(path, "GET");
        }
    }
    close(new_client_sock);
    return NULL;
}

void *handle_client_connections(void *arg)
{
    while (1)
    {
        pthread_t tid;
        struct sockaddr_in cl_addr;
        socklen_t addr_len = sizeof(cl_addr);
        addr_len = sizeof(cl_addr);
        int *new_client_sock = malloc(sizeof(int));
        *new_client_sock = accept(client_sock, (struct sockaddr *)&cl_addr, &addr_len);

        if (*new_client_sock == -1)
        {
            printf("accept failed. error code %d\n",E_ACPT);
            free(new_client_sock);
            continue;
        }

        if (pthread_create(&tid, NULL, real_handle_cl, new_client_sock) != 0)
        {
            printf("thread allocation failed. error code %d\n",E_NOMEM);
            free(new_client_sock);
        }
        pthread_detach(tid);
    }
    return NULL;
}

int create_file_or_folder(const char *path, const char *type, int nm_sock)
{
    if (strcmp(type, "FILE") == 0)
    {
        // Create an empty file
        int file = open(path, O_CREAT | O_WRONLY, 0644);
        if (file == -1)
        {
            printf("file creation failed. error code %d\n",E_FCREATE);
            send(nm_sock, "ERR|Failed to create file", 25, 0);
            if (send(nm_sock, "STOP", 4, 0) == -1)
            {
                perror("Failed to send ack to Naming Server");
            }
            return -1;
        }
        close(file);
        ///////////

        strcpy(files[file_cnt].path, path);
        files[file_cnt].rcnt = 0;
        files[file_cnt].wcnt = 0;
        files[file_cnt].dflag = 0;
        pthread_mutex_init(&files[file_cnt].file_lock, NULL);
        pthread_cond_init(&files[file_cnt].file_cond, NULL);
        file_cnt++;
        ///////////
        char buffer[BUFFER_SIZE];
        snprintf(buffer, BUFFER_SIZE, "UPDATE_SS%d|Path:%s|Operation:CREATE", serverno, path);
        // printf("%s\n", buffer);
        send(nm_sock, buffer, strlen(buffer), 0);
        // if (send(nm_sock, "STOP", 4, 0) == -1)
        // {
        //     perror("Failed to send ack to Naming Server");
        // }
        // send(nm_sock, "OK|File created successfully", strlen("OK|File created successfully"), 0);
        // int s = send_update_to_ns(path, "CREATE");
    }
    else if (strcmp(type, "DIR") == 0)
    {
        if (mkdir(path, 0755) == -1)
        {
            printf("failed to create dir, error code %d\n", E_DIRCREATE);
            send(nm_sock, "ERR|Failed to create directory", 30, 0);
            if (send(nm_sock, "STOP", 4, 0) == -1)
            {
                printf("send failed. error code %d\n", E_SEND);
            }
            return -1;
        }
        char buffer[BUFFER_SIZE];
        snprintf(buffer, BUFFER_SIZE, "UPDATE_SS%d|Path:%s|Operation:CREATE", serverno, path);
        printf("%s\n", buffer);
        send(nm_sock, buffer, strlen(buffer), 0);
        // if (send(nm_sock, "STOP", 4, 0) == -1)
        // {
        //     perror("Failed to send ack to Naming Server");
        // }
    }
    else
    {

        send(nm_sock, "ERR|Unknown type", 16, 0);
        return -1;
    }
    return 0;
}

int delete_directory(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    struct dirent *entry;
    char path[MAX_BUFFER_SIZE];

    if (dir == NULL)
    {
        printf("error opening dir. error code %d\n", E_DOPEN);
        return -1;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

        if (entry->d_type == DT_DIr)
        {
            if (delete_directory(path) == -1)
            {
                closedir(dir);
                return -1;
            }
        }
        else
        {
            //////////////////////////////////////////////
            int flag = 0;
            int ind = -1;

            for (int i = 0; i < file_cnt; i++)
            {
                if (strcmp(path, files[i].path) == 0)
                {
                    ind = i;
                    if (files[i].dflag == 1)
                    {
                        return 0;
                    }

                    if (files[i].wcnt >= 1 || files[i].rcnt >= 1)
                    {
                        flag = 1;
                    }
                    else
                    {
                        files[ind].dflag = 1;
                    }
                    break;
                }
            }

            pthread_mutex_lock(&files[ind].file_lock);

            if (flag)
            {
                while (files[ind].wcnt >= 1 || files[ind].rcnt >= 1)
                {
                    pthread_cond_wait(&files[ind].file_cond, &files[ind].file_lock);
                }
                files[ind].dflag = 1;
            }

            if (remove(path) == -1)
            {
                perror("Failed to delete file");
                closedir(dir);
                return -1;
            }

            pthread_cond_signal(&files[ind].file_cond);
            pthread_mutex_unlock(&files[ind].file_lock);
        }
    }

    closedir(dir);
    if (rmdir(dir_path) == -1)
    {
        perror("Failed to delete directory");
        return -1;
    }

    return 0;
}


int delete_file_or_folder(const char *path, int naming_sock)
{
    struct stat path_stat;

    if (stat(path, &path_stat) == -1)
    {
        printf("invalid path.error code %d\n",E_EPATH);
        send(naming_sock, "ERR|File/Folder not found", 24, 0);
        return -1;
    }

    if (S_ISREG(path_stat.st_mode))
    {
        int flag = 0;
        int ind = -1;

        for (int i = 0; i < file_cnt; i++)
        {
            if (strcmp(path, files[i].path) == 0)
            {
                ind = i;
                if (files[i].dflag == 1)
                {
                    return 0;
                }

                if (files[i].wcnt >= 1 || files[i].rcnt >= 1)
                {
                    flag = 1;
                }
                else
                {
                    files[ind].dflag = 1;
                }
                break;
            }
        }

        pthread_mutex_lock(&files[ind].file_lock);

        if (flag)
        {
            while (files[ind].wcnt >= 1 || files[ind].rcnt >= 1)
            {
                pthread_cond_wait(&files[ind].file_cond, &files[ind].file_lock);
            }
            files[ind].dflag = 1;
        }

        if (remove(path) == -1)
        {
            send(naming_sock, "ERR|Failed to delete file", 24, 0);
            if (send(nm_sock, "STOP", 4, 0) == -1)
            {
                printf("send failed.error code %d\n",E_SEND);
            }
            return -1;
        }

        pthread_cond_signal(&files[ind].file_cond);
        pthread_mutex_unlock(&files[ind].file_lock);

        // send(naming_sock, "OK|File deleted successfully", 26, 0);
        char buffer[BUFFER_SIZE];
        snprintf(buffer, BUFFER_SIZE, "UPDATE_SS%d|Path:%s|Operation:DELETE", serverno, path);
        send(naming_sock, buffer, strlen(buffer), 0);
        // if (send(nm_sock, "STOP", 4, 0) == -1)
        // {
        //     perror("Failed to send ack to Naming Server");
        // }
        return 0;
    }
    else if (S_ISDIR(path_stat.st_mode))
    {
        if (delete_directory(path) == -1)
        {
            send(naming_sock, "ERR|Failed to delete directory", 30, 0);
            if (send(nm_sock, "STOP", 4, 0) == -1)
            {
                printf("send failed. error code %d\n",E_SEND);
            }
            return -1;
        }
        char buffer[BUFFER_SIZE];
        snprintf(buffer, BUFFER_SIZE, "UPDATE_SS%d|Path:%s|Operation:DELETE", serverno, path);
        send(naming_sock, buffer, strlen(buffer), 0);
        // if (send(nm_sock, "STOP", 4, 0) == -1)
        // {
        //     perror("Failed to send ack to Naming Server");
        // }
        return 0;
    }
    else
    {
        send(naming_sock, "ERR|Unknown path type", 21, 0);
        if (send(naming_sock, "STOP", 4, 0) == -1)
        {
            printf("send failed.error code %d\n",E_SEND);
        }
        return -1;
    }
}


void copy_file(const char *source, const char *dest)
{
    printf("Copied file from %s to %s\n", source, dest);
}

void *real_handle_ns(void *arg)
{
    int new_ss_sock = *(int *)arg;
    free(arg);
    char buffer[BUFFER_SIZE];
    const char *base_directory = "";
    memset(buffer, 0, BUFFER_SIZE);
    int read_size = read(new_ss_sock, buffer, BUFFER_SIZE);
    if (read_size > 0)
    {
        printf("Command from NS: %s\n", buffer);

        if (strstr(buffer, "CREATE"))
        {
            char path[BUFFER_SIZE], type[BUFFER_SIZE], name_of[BUFFER_SIZE];
            sscanf(buffer, "CREATE %s %s", path, name_of);
            printf("%s %s\n", path, name_of);
            if (strchr(name_of, '.') != NULL)
            {
                strcpy(type, "FILE");
            }
            else
            {
                strcpy(type, "DIR");
            }
            char full_path[BUFFER_SIZE];
            snprintf(full_path, MAX_BUFFER_SIZE, "%s/%s", path, name_of);
            printf("Creating: %s with type: %s\n", full_path, type);
            create_file_or_folder(full_path, type, new_ss_sock);
        }
        else if (strstr(buffer, "DELETE"))
        {
            char path[BUFFER_SIZE];
            sscanf(buffer, "DELETE %s", path);
            char full_path[BUFFER_SIZE];
            snprintf(full_path, BUFFER_SIZE, "%s%s", base_directory, path);
            printf("Deleting %s", path);
            delete_file_or_folder(full_path, new_ss_sock);
        }
        else if (strstr(buffer, "COPY"))
        {
            printf("%s\n", buffer);
            char *path = (char *)malloc(1024 * sizeof(char));
            char command[BUFFER_SIZE];
            char src_ip[256];
            int src_port;
            if (buffer[strlen(buffer) - 1] == '0')
            {
                int a = 0;
                char *token;
                char *copies = (char *)malloc(sizeof(char) * BUFFER_SIZE);
                char *s = (char *)malloc(sizeof(char) * BUFFER_SIZE);
                char *src = (char *)malloc(sizeof(char) * BUFFER_SIZE);
                char *dest = (char *)malloc(sizeof(char) * BUFFER_SIZE);
                strcpy(s, buffer);
                s = s + 5;
                token = strtok(s, " ");
                while (token)
                {
                    if (!a)
                    {
                        strcpy(copies, token);
                    }
                    else if (a == 1)
                    {
                        strcpy(src, token);
                    }
                    else if (a == 2)
                    {
                        strcpy(dest, token);
                    }
                    else if (a == 3)
                    {
                        strcpy(src_ip, token);
                    }
                    else if (a == 4)
                    {
                        int port = atoi(token);
                        src_port = port;
                    }
                    token = strtok(NULL, " ");
                    ++a;
                }
                // printf("%s %s %s %s %d\n",command,src,dest,src_ip,src_port);
                strcpy(path, dest);
                copies[strlen(copies) - 1] = '\0';
                char *dum = (char *)malloc(sizeof(char) * BUFFER_SIZE);
                strcpy(dum, copies);
                char *token2 = strtok(dum, ",");
                while (token2)
                {
                    char *new_path = (char *)malloc(sizeof(char) * BUFFER_SIZE);
                    strcpy(new_path, dest);
                    strcat(new_path, "/");
                    strcat(new_path, token2);
                    char *type = "DIR";
                    // printf("%s\n",new_path);
                    if (strstr(token2, "."))
                    {
                        type = strdup("FILE");
                    }
                    // printf("%s\n",type);
                    create_file_or_folder(new_path, type, new_ss_sock);
                    usleep(5000);
                    token2 = strtok(NULL, ",");
                    if (new_path)
                        free(new_path);
                }
                if (dum)
                {
                    free(dum);
                }
                char *src2 = (char *)malloc(sizeof(char) * BUFFER_SIZE);
                strcpy(src2, src);
                int l = strlen(src2) - 1;
                while (src2[l] != '/')
                    --l;
                src2[l + 1] = '\0';
                char *dum2 = (char *)malloc(sizeof(char) * BUFFER_SIZE);
                strcpy(dum2, copies);
                char *token3 = strtok(dum2, ",");
                while (token3)
                {
                    char *new_path = (char *)malloc(sizeof(char) * BUFFER_SIZE);
                    strcpy(new_path, src2);
                    strcat(new_path, token3);
                    // printf("%s\n",new_path);
                    if (strstr(new_path, "."))
                    {
                        char buffer1[8192];
                        // strcpy(buffer1,new_path);
                        int ss_sock;
                        ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                        if (ss_sock < 0)
                        {
                            printf("socket creation failed. error code %d\n",E_SOCK);
                            exit(EXIT_FAILURE);
                        }
                        struct sockaddr_in ss_addr;
                        ss_addr.sin_family = AF_INET;
                        inet_pton(AF_INET, src_ip, &ss_addr.sin_addr);
                        ss_addr.sin_port = htons(src_port);
                        if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) == -1)
                        {
                            printf("connect failed.error code %d\n",E_CONN);
                            close(ss_sock);
                            exit(EXIT_FAILURE);
                        }
                        snprintf(buffer1, sizeof(buffer1), "READ %s", new_path);
                        if (send(ss_sock, buffer1, strlen(buffer1), 0) == -1)
                        {
                            printf("send failed. error code %d\n",E_SEND);
                            close(ss_sock);
                            exit(EXIT_FAILURE);
                        }

                        char *new_path2 = (char *)malloc(sizeof(char) * BUFFER_SIZE);
                        strcpy(new_path2, dest);
                        strcat(new_path2, "/");
                        strcat(new_path2, token3);
                        memset(buffer1, 0, sizeof(buffer1));
                        int is_first_chunk = 1;

                        // while (1)
                        // {
                            int read_size = recv(ss_sock, buffer1, sizeof(buffer1) - 1, 0);
                            if (read_size < 0)
                            {
                                // printf("recv failed.error code %d\n",E_RECV);
                                close(ss_sock);
                                return NULL;
                            }

                            // if (strstr(buffer1, "STOP") != NULL)
                            // {
                            //     break;
                            // }

                            FILE *fp;
                            if (is_first_chunk)
                            {
                                fp = fopen(new_path2, "wb");
                                is_first_chunk = 0;
                            }
                            else
                            {
                                fp = fopen(new_path2, "ab");
                            }

                            if (fp == NULL)
                            {
                                char response[MAX_BUFFER_SIZE];
                                snprintf(response, MAX_BUFFER_SIZE, "ERR|Unable to open file for writing: %s", strerror(errno));
                                send(new_ss_sock, response, strlen(response), 0);
                                close(ss_sock);
                                return NULL;
                            }

                            size_t bytes_written = fwrite(buffer1, 1, read_size, fp);
                            fclose(fp);

                            if (bytes_written != (size_t)read_size)
                            {
                                char response[MAX_BUFFER_SIZE];
                                snprintf(response, MAX_BUFFER_SIZE, "ERR|Unable to write all data to file: %s", strerror(errno));
                                send(new_ss_sock, response, strlen(response), 0);
                                // break;
                            }
                        // }

                        close(ss_sock);
                        free(new_path2);
                    }
                    token3 = strtok(NULL, ",");
                    usleep(5000);
                    free(new_path);
                }
                if (dum2)
                {
                    free(dum2);
                }
            }
            else
            {
                int a = 0;
                char *token;
                char *s = (char *)malloc(sizeof(char) * BUFFER_SIZE);
                char *src = (char *)malloc(sizeof(char) * BUFFER_SIZE);
                char *dest = (char *)malloc(sizeof(char) * BUFFER_SIZE);
                strcpy(s, buffer);
                s = s + 5;
                token = strtok(s, " ");
                while (token)
                {
                    if (!a)
                    {
                        strcpy(src, token);
                    }
                    else if (a == 1)
                    {
                        strcpy(dest, token);
                    }
                    else if (a == 2)
                    {
                        strcpy(src_ip, token);
                    }
                    else if (a == 3)
                    {
                        int port = atoi(token);
                        src_port = port;
                    }
                    token = strtok(NULL, " ");
                    ++a;
                }
                strcpy(path, dest);
                char *type = "DIR";
                if (strstr(dest, "."))
                {
                    type = strdup("FILE");
                }
                char buffer1[8192];
                // strcpy(buffer1,new_path);
                int ss_sock;
                ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                if (ss_sock < 0)
                {
                    printf("socket creation failed. error code %d\n",E_SOCK);
                    exit(EXIT_FAILURE);
                }
                struct sockaddr_in ss_addr;
                ss_addr.sin_family = AF_INET;
                inet_pton(AF_INET, src_ip, &ss_addr.sin_addr);
                ss_addr.sin_port = htons(src_port);
                if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) == -1)
                {
                    printf("connect failed. error code %d\n",E_CONN);
                    close(ss_sock);
                    exit(EXIT_FAILURE);
                }
                snprintf(buffer1, sizeof(buffer1), "READ %s", src);
                if (send(ss_sock, buffer1, strlen(buffer1), 0) == -1)
                {
                    printf("send failed. error code %d\n",E_SEND);
                    close(ss_sock);
                    exit(EXIT_FAILURE);
                }

                memset(buffer1, 0, sizeof(buffer1));
                int is_first_chunk = 1;

                // while (1)
                // {
                    int read_size = recv(ss_sock, buffer1, sizeof(buffer1), 0);
                    if (read_size < 0)
                    {
                        printf("recv failed.error code %d\n",E_RECV);
                        close(ss_sock);
                        return NULL;
                    }

                    // if (strstr(buffer1, "STOP") != NULL)
                    // {
                    //     break;
                    // }

                    FILE *fp;
                    if (is_first_chunk)
                    {
                        fp = fopen(dest, "wb");
                        is_first_chunk = 0;
                    }
                    else
                    {
                        fp = fopen(dest, "ab");
                    }

                    if (fp == NULL)
                    {
                        char response[MAX_BUFFER_SIZE];
                        snprintf(response, MAX_BUFFER_SIZE, "ERR|Unable to open file for writing: %s", strerror(errno));
                        send(new_ss_sock, response, strlen(response), 0);
                        close(ss_sock);
                        return NULL;
                    }

                    size_t bytes_written = fwrite(buffer1, 1, read_size, fp);
                    fclose(fp);

                    if (bytes_written != (size_t)read_size)
                    {
                        char response[MAX_BUFFER_SIZE];
                        snprintf(response, MAX_BUFFER_SIZE, "ERR|Unable to write all data to file: %s", strerror(errno));
                        send(new_ss_sock, response, strlen(response), 0);
                        // break;
                    }
                // }

                close(ss_sock);
                // printf("%s %s %s %d\n",src,dest,src_ip,src_port);
            }
            char p[BUFFER_SIZE];
            snprintf(p, sizeof(p), "UPDATE_SS%d|Path:%s|Operation:COPIED", serverno, path);
            printf("%s\n", p);
            usleep(5000);
            send(new_ss_sock, p, strlen(p), 0);
        }
        else if (strstr(buffer, "HEALTH_CHECK"))
        {
            char p[BUFFER_SIZE];
            snprintf(p, sizeof(p), "Alive");
            // printf("%s\n", p);
            usleep(5000);
            send(new_ss_sock, p, strlen(p), 0);
        }
    }
    close(new_ss_sock);
    return NULL;
}

void *handle_ns_commands(void *arg)
{
    while (1)
    {
        pthread_t tid;
        struct sockaddr_in ns_addr;
        socklen_t addr_len = sizeof(ns_addr);
        addr_len = sizeof(ns_addr);
        int *new_ss_sock = malloc(sizeof(int));
        *new_ss_sock = accept(nm_sock, (struct sockaddr *)&ns_addr, &addr_len);

        if (*new_ss_sock == -1)
        {
            perror("Accept failed");
            free(new_ss_sock);
            continue;
        }

        if (pthread_create(&tid, NULL, real_handle_ns, new_ss_sock) != 0)
        {
            perror("Thread creation failed");
            free(new_ss_sock);
        }
        pthread_detach(tid);
    }

    return NULL;
}


void *handle_terminal_input(void *arg)
{
    char buffer[BUFFER_SIZE];

    while (1)
    {
        memset(buffer, 0, BUFFER_SIZE);
        fgets(buffer, BUFFER_SIZE, stdin);
        if (strstr(buffer, "CREATE") || strstr(buffer, "DELETE"))
        {
            // send_update_to_ns(buffer, strstr(buffer, "CREATE") ? "ADD" : "DELETE");
        }
    }
    return NULL;
}

void handle_signal(int sig)
{
    int sock_fd;
    int status;

    printf("\nReceived interrupt signal. Connecting to server to send disconnect...\n");

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1)
    {
        perror("Socket creation failed in signal handler");
        exit(1);
    }

    status = connect(sock_fd, (struct sockaddr *)&ns_addr, sizeof(ns_addr));
    if (status == -1)
    {
        perror("Connection failed in signal handler");
        close(sock_fd);
        exit(1);
    }
    char message[29];
    snprintf(message, sizeof(message), "Server no %d is disconnected", serverno);

    printf("%s\n", message);
    send(sock_fd, message, strlen(message), 0);

    close(sock_fd);
    usleep(10);
    // printf("Disconnect message sent. Exiting...\n");
    exit(0);
}

void get_dynamic_ip(char *ip_buffer, size_t buffer_size)
{
    struct ifaddrs *ifaddr, *ifa;

    if (getifaddrs(&ifaddr) == -1)
    {
        perror("getifaddrs");
        exit(EXIT_FAILURE);
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == NULL)
            continue;

        if (ifa->ifa_addr->sa_family == AF_INET)
        {
            if (strcmp(ifa->ifa_name, "lo") != 0)
            {
                struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;

                if (inet_ntop(AF_INET, &addr->sin_addr, ip_buffer, buffer_size) == NULL)
                {
                    perror("inet_ntop");
                    exit(EXIT_FAILURE);
                }
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
}

void send_server_details(struct sockaddr_in client_addr, int ns_port, int client_port, char *paths[], int num_paths)
{
    char details[BUFFER_SIZE] = {0};
    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    // char *server_ip = (char *)malloc(sizeof(char) * INET_ADDRSTRLEN);
    char server_ip[INET_ADDRSTRLEN];
    get_dynamic_ip(server_ip, sizeof(server_ip));

    printf("%s", server_ip);
    int offset = snprintf(details, sizeof(details), "SS|IP:%s|Port1:%d|Port2:%d|Paths:", server_ip, ns_port, client_port);
    if (offset < 0 || offset >= sizeof(details))
    {
        fprintf(stderr, "Error formatting server details\n");
        return;
    }

    for (int i = 0; i < num_paths; i++)
    {
        int path_len = snprintf(details + offset, sizeof(details) - offset, "%s%s", paths[i], (i < num_paths - 1) ? "," : "");
        if (path_len < 0 || offset + path_len >= sizeof(details))
        {
            fprintf(stderr, "Error appending paths to server details\n");
            return;
        }
        offset += path_len;
    }
    if (send(ns_sock, details, strlen(details), 0) == -1)
    {
        perror("Failed to send server details");
    }
    else
    {
        printf("Server details sent to Naming Server\n");
    }
}


int main()
{
    memset(&nm_addr, 0, sizeof(nm_addr));
    memset(&client_addr, 0, sizeof(client_addr));
    ////////////////////////////////////////////////////////////////////////////////////////
    struct sigaction sa;
    sa.sa_handler = handle_signal; 
    sa.sa_flags = 0;               
    sigemptyset(&sa.sa_mask);     

    if (sigaction(SIGINT, &sa, NULL) == -1)
    {
        perror("Error setting up SIGINT handler");
        exit(1);
    }

    if (sigaction(SIGTERM, &sa, NULL) == -1)
    {
        perror("Error setting up SIGTERM handler");
        exit(1);
    }

    if (sigaction(SIGQUIT, &sa, NULL) == -1)
    {
        perror("Error setting up SIGQUIT handler");
        exit(1);
    }
    //////////////////////////////////////////////////////////////////////////////////
    // struct sockaddr_in nm_addr, client_addr, ns_addr;
    socklen_t nm_addr_len = sizeof(nm_addr);
    socklen_t client_len = sizeof(client_addr);
    char *paths[] = {"/home/home/Desktop/OSN/Course-Project/server","/home/home/Desktop/OSN/Course-Project/server/backup","/home/home/Desktop/OSN/Course-Project/server/test","/home/home/Desktop/OSN/Course-Project/server/test/b.txt"};
    // char *paths[] = {"/home/sigullapalliakash/Documents/check/course-project-team_17/test", "/home/sigullapalliakash/Documents/check/course-project-team_17/test/a.txt", "/home/sigullapalliakash/Documents/check/course-project-team_17/test/test1", "/home/sigullapalliakash/Documents/check/course-project-team_17/test/test1/b.txt", "/home/sigullapalliakash/Documents/check/course-project-team_17/test/a.txt", "/home/sigullapalliakash/Documents/check/course-project-team_17/test/test1", "/home/sigullapalliakash/Documents/check/course-project-team_17/b.txt","/home/sigullapalliakash/Documents/check/course-project-team_17/yanji.mp3"};
    // char *paths[] = {"/home/nikhilesht/Desktop/OSN/course-project-team_17", "/home/nikhilesht/Desktop/OSN/course-project-team_17/backup", "/home/nikhilesht/Desktop/OSN/course-project-team_17/test", "/home/nikhilesht/Desktop/OSN/course-project-team_17/test/c.txt"};
    int num_paths = sizeof(paths) / sizeof(paths[0]);

    for (int i = 0; i < num_paths; i++)
    {
        if (strstr(paths[i], "."))
        {
            printf("%s\n", paths[i]);
            strcpy(files[file_cnt].path, paths[i]);
            // printf("%s\n",files[i].path);
            files[file_cnt].rcnt = 0;
            files[file_cnt].wcnt = 0;
            files[file_cnt].dflag = 0;
            pthread_mutex_init(&files[file_cnt].file_lock, NULL);
            pthread_cond_init(&files[file_cnt].file_cond, NULL);
            file_cnt++;
        }
    }
    char ip_address[INET_ADDRSTRLEN];
    printf("Enter the IP address of the Naming Server: ");
    if (scanf("%s", ip_address) != 1)
    {
        printf("invalid input. error code %d\n",E_INVAL);
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

    if ((nm_sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("Naming Server socket creation failed");
        exit(1);
    }
    int opt = 1;
    if (setsockopt(nm_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("setsockopt(SO_REUSEADDR) failed");
        // close(client_sock);
        close(nm_sock);
        exit(1);
    }

    nm_addr.sin_family = AF_INET;
    nm_addr.sin_addr.s_addr = INADDR_ANY;
    nm_addr.sin_port = htons(NM_PORT);

    if (bind(nm_sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr)) == -1)
    {
        perror("Naming Server socket bind failed");
        close(nm_sock);
        exit(1);
    }

    if (getsockname(nm_sock, (struct sockaddr *)&nm_addr, &nm_addr_len) == -1)
    {
        perror("Getting Naming Server socket name failed");
        close(nm_sock);
        exit(1);
    }

    printf("Naming Server Port: %d\n", ntohs(nm_addr.sin_port));

    if (listen(nm_sock, 5) == -1)
    {
        perror("Listening on nm socket failed");
        close(client_sock);
        close(nm_sock);
        exit(1);
    }

    if ((client_sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("Client socket creation failed");
        close(nm_sock);
        exit(1);
    }
    int opt1 = 1;
    if (setsockopt(client_sock, SOL_SOCKET, SO_REUSEADDR, &opt1, sizeof(opt1)) < 0)
    {
        perror("setsockopt(SO_REUSEADDR) failed");
        close(client_sock);
        close(nm_sock);
        exit(1);
    }

    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = INADDR_ANY;
    client_addr.sin_port = htons(CLIENT_PORT);

    if (bind(client_sock, (struct sockaddr *)&client_addr, sizeof(client_addr)) == -1)
    {
        perror("Client socket bind failed");
        close(client_sock);
        close(nm_sock);
        exit(1);
    }

    if (getsockname(client_sock, (struct sockaddr *)&client_addr, &client_len) == -1)
    {
        perror("Getting Client socket name failed");
        close(client_sock);
        close(nm_sock);
        exit(1);
    }

    printf("Storage Server Client Port: %d\n", ntohs(client_addr.sin_port));

    if (listen(client_sock, 5) == -1)
    {
        perror("Listening on client socket failed");
        close(client_sock);
        close(nm_sock);
        exit(1);
    }

    send_server_details(client_addr, ntohs(nm_addr.sin_port), ntohs(client_addr.sin_port), paths, num_paths);
    char buffer[BUFFER_SIZE];
    int read_size = read(ns_sock, buffer, BUFFER_SIZE);
    if (read_size > 0)
    {
        buffer[read_size] = '\0'; 

        int server_id;
        if (sscanf(buffer, "Registration Successful.You are Storage Server %d", &server_id) == 1)
        {
            serverno = server_id;
            printf("%s\n", buffer);
            // printf("Extracted Server ID: %d\n", server_id);
        }
        else if (sscanf(buffer, "Welcome back Storage Server %d", &server_id) == 1)
        {
            serverno = server_id;
            printf("%s\n", buffer);
        }
        else
        {
            printf("Failed to extract server ID from buffer: %s\n", buffer);
        }
    }
    else
    {
        printf("Error reading data or empty message\n");
    }

    close(ns_sock);
    pthread_t client_thread, ns_thread, terminal_thread;
    pthread_create(&client_thread, NULL, handle_client_connections, NULL);
    pthread_create(&ns_thread, NULL, handle_ns_commands, NULL);
    pthread_create(&terminal_thread, NULL, handle_terminal_input, NULL);

    pthread_join(client_thread, NULL);
    pthread_join(ns_thread, NULL);
    pthread_join(terminal_thread, NULL);

    close(nm_sock);
    close(client_sock);
    return 0;
}