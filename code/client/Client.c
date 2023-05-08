#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio_ext.h>
#include <sys/sendfile.h>

#define BUF_SIZE 8192

bool close_fd = false;
char operation[BUF_SIZE];
char argument[BUF_SIZE];
char code[BUF_SIZE];
pthread_t data_thread;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
bool start_act = false;

typedef struct
{
    char file_to[BUF_SIZE];
    int port;
    int cmd_sock;
    int data_sock;
    bool isActive;
    char address[BUF_SIZE];
    char command[BUF_SIZE];
    pthread_cond_t cond;
    pthread_mutex_t mutex;
}DataThread;

DataThread *dt;

void Send(int sock, char* buf, int len, int fl);
void parse_command(char* command, char* operation, char* argument);
void command(int sock, char* buf);
void wait_response(int sock, char* buf);
void parse_response(int sock,char* buf, char* code,char* argument);
void wait_command(DataThread* dt);

void wait_command(DataThread* dt)
{
    char buf[BUF_SIZE];

    pthread_mutex_lock(&dt->mutex);

    char command[BUF_SIZE];
    while (strlen(dt->command) == 0) {
        pthread_cond_wait(&dt->cond, &dt->mutex);
    }

    strcpy(command, dt->command);
    memset(dt->command, 0, sizeof(dt->command));

    pthread_mutex_unlock(&dt->mutex);

    if(strcmp(command, "RETR") == 0)
    {
        int fd;
        ssize_t nread;
        bool opened = false;
        while (1) {
            if(nread = recv(dt->data_sock, buf, BUF_SIZE, 0)>0){
                if(!opened){
                    fd = open(dt->file_to, O_WRONLY | O_CREAT, 0644);
                    if (fd < 0) {
                        perror("Error opening file for writing");
                        dt->isActive = false;
                        pthread_exit(NULL);
                    }
                    opened = true;
                }
                if (write(fd, buf, nread) != nread) {
                    perror("Error writing to file");
                    dt->isActive = false;
                    pthread_exit(NULL);
                }
            }
            else
            {
                break;
            }
        }

        if (nread < 0) {
            perror("Error reading from socket");
            dt->isActive = false;
            pthread_exit(NULL);
        }
        close(fd);
    }
    else if (strcmp(command, "STOR") == 0)
    {
        char bufff[BUF_SIZE];
        int fd;
        struct stat stat_buf;
        if (stat(dt->file_to, &stat_buf) == -1) {
            perror("stat failed");
        }

        if (S_ISDIR(stat_buf.st_mode)) {
            printf("Is directory! \n");
            close(dt->data_sock);     
        }
        if ((fd = open(dt->file_to, O_RDONLY)) == -1) {
            perror("open failed");
            printf("Error file not found \n");
            close(dt->data_sock);
            dt->isActive = false;
            pthread_exit(NULL);  
        }

        off_t offset = 0;
        struct stat file_stat;
        fstat(fd, &file_stat);
        ssize_t size = file_stat.st_size;
        ssize_t sent = sendfile(dt->data_sock, fd, &offset, size);
        printf("sent = %zd Bytes\n", sent);
        close(fd);
    }
}

void* start_pasv_conn(void* arg) 
{
    DataThread *dt = (DataThread*)arg;

    pthread_cond_init(&dt->cond, NULL);
    pthread_mutex_init(&dt->mutex, NULL);

    unsigned int ip1, ip2, ip3, ip4, port1, port2;
    sscanf(dt->address, "%u,%u,%u,%u,%u,%u", &ip1, &ip2, &ip3, &ip4, &port1, &port2);
    unsigned int port = port1 * 256 + port2;

    int data_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (data_sockfd < 0) {
        perror("socket");
        dt->isActive = false;
        pthread_exit(NULL);
    }

    struct sockaddr_in data_servaddr = {0};
    data_servaddr.sin_family = AF_INET;
    data_servaddr.sin_port = htons(port);
    data_servaddr.sin_addr.s_addr = htonl((ip1 << 24) | (ip2 << 16) | (ip3 << 8) | ip4);
    if (connect(data_sockfd, (struct sockaddr *)&data_servaddr, sizeof(data_servaddr)) < 0) {
        perror("connect");
        dt->isActive = false;
        pthread_exit(NULL);
    }
    dt->data_sock = data_sockfd;

    wait_command(dt);

    close(data_sockfd);
    dt->isActive = false;
    pthread_exit(NULL);
}

void* start_active_conn(void* arg) {
    DataThread *dt = (DataThread*)arg;

    pthread_cond_init(&dt->cond, NULL);
    pthread_mutex_init(&dt->mutex, NULL);

    struct sockaddr_in cliadr = {0};
    cliadr.sin_family = AF_INET;
    cliadr.sin_port = htons(dt->port);
    cliadr.sin_addr.s_addr = htonl(INADDR_ANY);

    char buf[BUF_SIZE];

    int conn_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (conn_sockfd < 0) {
        perror("socket");
        dt->isActive = false;
        start_act = true;
        pthread_cond_signal(&cond);
        pthread_exit(NULL);
    }

    if (bind(conn_sockfd, (struct sockaddr *)&cliadr, sizeof(cliadr)) < 0) {
        perror("bind");
        dt->isActive = false;
        start_act = true;
        pthread_cond_signal(&cond);
        pthread_exit(NULL);
    }

    if (listen(conn_sockfd, 1) < 0) {
        perror("listen");
        dt->isActive = false;
        start_act = true;
        pthread_cond_signal(&cond);
        pthread_exit(NULL);
    }

    printf("Data socket created and listening...\n");

    start_act = true;
    pthread_cond_signal(&cond);

    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int data_sockfd = accept(conn_sockfd, (struct sockaddr *)&client_addr, &client_addr_len);
    if (conn_sockfd < 0) {
        perror("accept");
        dt->isActive = false;
        pthread_exit(NULL);
    }
    dt->data_sock = data_sockfd;

    wait_command(dt);

    close(data_sockfd);
    dt->isActive = false;
    pthread_exit(NULL);

}

void Send(int sock, char* buf, int len, int fl)
{
    if (send(sock, buf, len, fl) == -1) {
        perror("Error sending message to server");
        exit(EXIT_FAILURE);
    }
}

void parse_command(char* command, char* operation, char* argument) {
    char* token = strtok(command, " \r\n"); 
    if (token != NULL) {
        strcpy(operation, token);
    }
    token = strtok(NULL, " \r\n");
    if (token != NULL) {
        strcpy(argument, token); 
    }
}

void command(int sock, char* buf)
{
    memset(operation, 0, BUF_SIZE);
    memset(argument, 0, BUF_SIZE);
    memset(buf, 0, BUF_SIZE);

    printf("> ");
    __fpurge(stdin);
    scanf("%[^\n]", buf);
    getchar();

    buf[strcspn(buf, "\n")] = '\0'; 

    char buf2[BUF_SIZE];

    strcpy(buf2, buf);

    parse_command(buf2, operation, argument);

    if(strcmp(operation, "RETR")==0 && dt->isActive)
    {
       Send(sock, buf, strlen(buf), 0);
       parse_response(sock, buf, code, argument); 
       strcpy(dt->file_to, argument);
       strcpy(dt->command, "RETR");
       if(dt->isActive){
            pthread_cond_signal(&dt->cond);
        }
       return;
    }
    else if(strcmp(operation, "STOR")==0 && dt->isActive)
    {
        strcpy(dt->command, "STOR");
        strcpy(dt->file_to, argument);
        if(dt->isActive){
            pthread_cond_signal(&dt->cond);
        }
    }
    else if(strcmp(operation, "PASV")==0)
    {
        Send(sock, buf, strlen(buf), 0);  
        parse_response(sock, buf, code, argument); 
        if(dt->isActive == false){
            strcpy(dt->address, argument);
            dt->cmd_sock = sock;
            dt->isActive = true;
            pthread_create(&data_thread, NULL, start_pasv_conn, dt);
        }
        return;
    }
    else if(strcmp(operation, "LIST")==0)
    {
        if(dt->isActive == true)
        {
            if(pthread_cancel(data_thread))
            {
                fprintf(stderr, "error canceling in client \n");
            }
            else
            {
                close(dt->data_sock);
                dt->isActive = false;
            }
        }
    }
    else if(strcmp(operation, "ABOR")==0)
    {
        if(dt->isActive == true)
        {
            if(pthread_cancel(data_thread))
            {
                fprintf(stderr, "error canceling in client \n");
            }
            else
            {   
                close(dt->data_sock);
                dt->isActive = false;
            }
        }
    } 
    else if(strcmp(operation, "PORT")==0)
    {
        dt = (DataThread*)malloc(sizeof(DataThread));
        unsigned int ip1, ip2, ip3, ip4, port1, port2;
        sscanf(argument, "%u,%u,%u,%u,%u,%u", &ip1, &ip2, &ip3, &ip4, &port1, &port2);
        unsigned int port = port1 * 256 + port2;

        if(dt->isActive == false){
            dt->port = port;
            dt->cmd_sock = sock;
            dt->isActive = true;
            pthread_create(&data_thread, NULL, start_active_conn, dt);

            pthread_mutex_lock(&mutex);

            while(start_act == false)
            {
                pthread_cond_wait(&cond, &mutex);
            }
            start_act = false;

            pthread_mutex_unlock(&mutex);
        }
        else{   
        printf("Now is transfering \n");
        return;
        }
    }

    Send(sock, buf, strlen(buf), 0);
};

void parse_response(int sock,char* buf, char* code,char* argument)
{
    int total_bytes_received = 0;
    int bytes_received;
    while ((bytes_received = read(sock, buf + total_bytes_received, BUF_SIZE - total_bytes_received)) > 0) {
        total_bytes_received += bytes_received;
        if (buf[total_bytes_received - 1] == '\n') {
            break;
        }
    }
    if (bytes_received == -1) {
        perror("Error receiving message from server");
        exit(EXIT_FAILURE);
    } else if (bytes_received == 0) {
        printf("Connection closed by server\n");
        close_fd = true;
    } else {
        char* token = strtok(buf, " \r\n"); 
        if (token != NULL) {
            strcpy(code, token);
        }
        token = strtok(NULL, " \r\n");
        if (token != NULL) {
            strcpy(argument, token); 
        }
    }
}

void wait_response(int sock, char* buf)
{
    int total_bytes_received = 0;
    int bytes_received;
    while ((bytes_received = read(sock, buf + total_bytes_received, BUF_SIZE - total_bytes_received)) > 0) {
        total_bytes_received += bytes_received;
        if (buf[total_bytes_received - 1] == '\n') {
            break;
        }
    }
    if (bytes_received == -1) {
        perror("Error receiving message from server");
        exit(EXIT_FAILURE);
    } else if (bytes_received == 0) {
        printf("Connection closed by server\n");
        close_fd = true;
    } else {
        printf("%s\n", buf? buf: "NULL");
    }
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    dt = (DataThread*)malloc(sizeof(DataThread));

    dt->isActive = false;
    const char *server_ip = argv[1];
    const int server_port = atoi(argv[2]);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("Error creating socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(server_ip);
    server_addr.sin_port = htons(server_port);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("Error connecting to server");
        exit(EXIT_FAILURE);
    }

    char buf[BUF_SIZE];
    while (!close_fd) {
        memset(buf, 0, BUF_SIZE);
        wait_response(sock, buf);
        command(sock, buf);       
    }

    close(sock);
    return 0;
}