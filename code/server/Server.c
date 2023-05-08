#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sqlite3.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <libgen.h>
#include <errno.h>
#include <errno.h>
#include <sys/sendfile.h>
#include <stdint.h>
#include <fcntl.h>

#define MAX_CLIENTS 1000
#define BUF_SIZE 8192

#define PWD_PERM 0
#define CWD_PERM 1

#pragma region prototypes

sqlite3 *db;
char *zErrMsg = 0;
int rc;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
int server;

typedef enum{ 
    ASCII,
    BIN
} TransferType;

typedef struct{
    char root[BUF_SIZE/2];
    char dir[BUF_SIZE/2];
}FileExplorer;

typedef struct{
    bool isActive;
    TransferType type;
    FileExplorer *fe;
    int data_sock;
    int cmd_sock;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    char command[BUF_SIZE];
} DataThread;

typedef struct
{
    int sock;
    int data_sock;
    int perm;
    bool isAuth;
    bool isActive;
    DataThread *dt;
    char homedir[BUF_SIZE];
    pthread_t data_thread;
    char file_from[BUF_SIZE];
}client;

typedef struct{
    int client_index;
    client* clients;
    char* root;
}ThreadArgs;

typedef struct{
    char command[BUF_SIZE];
    char response[BUF_SIZE];
}Request;

client clients[MAX_CLIENTS];

//Wrapper
int Socket(int domain, int type, int protocol);
void Bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
void Listen(int sockfd, int backlog);
int Accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

//FileExplorer
void get_file_permissions(mode_t mode, char* permissions);
void get_directory_listing(char* directory_path, char* output, int show_hidden, int show_details);
void init_explorer(FileExplorer* fe, char* root_dir);
void rename_from(client* cl, FileExplorer* fe, char* filename);
void rename_to(client* cl, FileExplorer* fe, char* filename);
bool make_dir(char* path, FileExplorer* fe);
bool rm(FileExplorer* fe, const char* filename);
bool cd(char* next_dir, FileExplorer* fe); 
char* cd_up(FileExplorer* fe);
uint64_t file_size(FileExplorer* fe, const char* filename);
void send_file(DataThread* dt, char* file_from, char* file_name);
int rmdir_recursive(const char* path);
void recv_data(DataThread *dt,const char* to_file);

//Sql
int callback(void *count_ptr, int argc, char **argv, char **azColName);
int auth_db(char *name, char* pass, client *cl);
void addDefaultUsers();

//init
void handle_sigint(int sig);
void initialize_ftp_server(int port);

//FtpServer
void auth(client* cl);
void send_message(int client_sock, char *message);
void parse_command(char* command, char* operation, char* argument);
void *handle_client_thread(void *arg);

//DataThread
char* start_active(char* address, client *cl, FileExplorer* fe, TransferType type);
void* handle_data_thread(void* args);
void start_pasv(client *cl, FileExplorer* fe, TransferType type);
bool data_thread_cancel(client* cl);

#pragma endregion

#pragma region Wrapper

int Socket(int domain, int type, int protocol)
{
    int res = socket(domain, type, protocol);
    if(res == -1)
    {
        perror("socket failed\n");
        exit(EXIT_FAILURE);
    }
    return res;
}

void Bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    if(bind(sockfd, addr, addrlen) < 0)
    {
        perror("bind failed\n");
        exit(EXIT_FAILURE);
    }
}

void Listen(int sockfd, int backlog)
{
    if(listen(sockfd, backlog) < 0)
    {
        perror("listen failed\n");
        exit(EXIT_FAILURE);
    }
}

int Accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    int res = accept(sockfd, addr, addrlen);
    if(res < 0)
    {
        perror("accept failed\n");
        exit(EXIT_FAILURE);
    }

    return res;
}
#pragma endregion

#pragma region Dir

void get_file_permissions(mode_t mode, char* permissions) {
    permissions[0] = (S_ISDIR(mode)) ? 'd' : '-';
    permissions[1] = (mode & S_IRUSR) ? 'r' : '-';
    permissions[2] = (mode & S_IWUSR) ? 'w' : '-';
    permissions[3] = (mode & S_IXUSR) ? 'x' : '-';
    permissions[4] = (mode & S_IRGRP) ? 'r' : '-';
    permissions[5] = (mode & S_IWGRP) ? 'w' : '-';
    permissions[6] = (mode & S_IXGRP) ? 'x' : '-';
    permissions[7] = (mode & S_IROTH) ? 'r' : '-';
    permissions[8] = (mode & S_IWOTH) ? 'w' : '-';
    permissions[9] = (mode & S_IXOTH) ? 'x' : '-';
    permissions[10] = '\0';
}

void get_directory_listing(char* directory_path, char* output, int show_hidden, int show_details) {

    puts(directory_path);

    strcpy(output, "150 List of catalogs: \n");


    DIR* dir = opendir(directory_path);
    if (dir == NULL) {
        strcpy(output, "550 Failed to open directory.\n");
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!show_hidden && entry->d_name[0] == '.') {
            continue;
        }

        char path[BUF_SIZE];
        sprintf(path, "%s/%s", directory_path, entry->d_name);

        struct stat file_stat;
        if (lstat(path, &file_stat) == -1) {
            continue;
        }

        char file_permissions[11] = {'\0'};
        if (show_details) {
            get_file_permissions(file_stat.st_mode, file_permissions);
            sprintf(output + strlen(output), "%s %3ld %-8s %-8s %8lld %.12s %s\n", 
                    file_permissions, file_stat.st_nlink, 
                    getpwuid(file_stat.st_uid)->pw_name, 
                    getgrgid(file_stat.st_gid)->gr_name,
                    (long long)file_stat.st_size, 
                    ctime(&file_stat.st_mtime) + 4, 
                    entry->d_name);
        } else {
            sprintf(output + strlen(output), "%s\n", entry->d_name);
        }
    }

    closedir(dir);
    if (strlen(output) == 0) {
        strcpy(output, "Directory is empty.\n");
    }
}

void rename_from(client* cl, FileExplorer* fe, char* filename)
{
    int file_from_fd;
    char buf[BUF_SIZE*2];
    sprintf(buf, "%s%s%s", fe->root, fe->dir, filename);

    if (access(buf, F_OK) != -1) {
        strncpy(cl->file_from, buf, sizeof(cl->file_from));
    
        file_from_fd = open(buf, O_RDONLY);
        if (file_from_fd == -1) {
            perror("open");
            send_message(cl->sock, "550 Error opening file\r\n");
        } else {
            send_message(cl->sock, "350 Ready for RNTO\r\n");
        }
    } 
    else {
        send_message(cl->sock, "550 File not found\r\n");
    }
}

void rename_to(client* cl, FileExplorer* fe, char* filename)
{
    char buf[BUF_SIZE*2];
    sprintf(buf, "%s%s%s", fe->root, fe->dir, filename);
    if (strlen(cl->file_from) == 0) {
        send_message(cl->sock, "503 Bad sequence of commands\r\n");
        return;
    }
    if (rename(cl->file_from, buf) != 0) {
        perror("Error renaming file");
        send_message(cl->sock, "550 Requested action not taken. File unavailable (e.g., file not found, no access).\r\n");
        return;
    }
    memset(cl->file_from, 0, sizeof(cl->file_from));
    send_message(cl->sock, "250 File renamed successfully\r\n");
}

bool make_dir(char* path, FileExplorer* fe)
{
    if(path == NULL || strlen(path)==0)
        return false;

    while (path[0] == '/' || path[0] == '.') 
    {
        path++; 
        if(strlen(path)==0)
            return false;
    }

    size_t len = strlen(path);
    while (len > 0 && path[len - 1] == '/') {
        --len;
    }
    if (len == 0) {
        return -1;
    }

    char *folder = (char*) malloc(strlen(fe->root) + strlen(fe->dir) + len + 2);
    if (folder == NULL) {
        return -1;
    }

    sprintf(folder, "%s%s/%.*s", fe->root, fe->dir, (int)len, path);

    int res = mkdir(folder, S_IRWXU | S_IRWXG | S_IRGRP);
    if (res != 0) {
        fprintf(stderr, "E: Error mkdir %s\n", folder);
        return false;
    }
    
    free(folder);
    return true;
}

bool rm(FileExplorer* fe, const char* filename) 
{
    char path[BUF_SIZE*2];
    snprintf(path, sizeof(path), "%s%s%s", fe->root, fe->dir, filename);
    if (remove(path) == -1) {
        fprintf(stderr, "E: removing file %s\n", filename);
        return false;
    }
    return true;
}

bool cd(char* next_dir, FileExplorer* fe) 
{
    if (next_dir == NULL || strlen(next_dir) == 0)
        return false;
    while (*next_dir == '/' || *next_dir == '.') {
        next_dir++;
        if (*next_dir == '\0')
            return false;
    }
    int size = strlen(next_dir);
    while (next_dir[size - 1] == '/') {
        size--;
        if (size == 0)
            return false;
        next_dir[size] = '\0';
    }
    if (size == 0)
        return false;
    char* result_dir = malloc(strlen(fe->root) + strlen(fe->dir) + size + 2);
    sprintf(result_dir, "%s%s%s/", fe->root, fe->dir, next_dir);

    DIR *d = opendir(result_dir);
    if (d) {
        strcat(fe->dir, next_dir);
        strcat(fe->dir, "/");
        closedir(d);
        free(result_dir);
        return true;
    } else {
        free(result_dir);
        return false;
    }
}

char* cd_up(FileExplorer* fe)
{
    if (strcmp(fe->dir, "/") == 0)
        return fe->root;

    char buffer[BUF_SIZE];
    strcpy(buffer, fe->dir);

    char* parent = dirname(buffer);
    if (parent[strlen(parent) - 1] != '/')
        strcat(parent, "/");

    strcpy(fe->dir, parent);
    char* res = (char*)malloc(BUF_SIZE*sizeof(char));
    sprintf(res, "%s%s", fe->root, fe->dir);
    return res;
}

uint64_t file_size(FileExplorer* fe, const char* filename)
{
    char path[BUF_SIZE*2];
    sprintf(path, "%s%s%s", fe->root, fe->dir, filename);
    
    struct stat file_stat;
    if (stat(path, &file_stat) == -1) {
        perror("stat failed");
        return -1;
    }
    return file_stat.st_size;
}

void send_file(DataThread* dt, char* file_from, char* file_name)
{
    char res[BUF_SIZE] = "226 Data transferred \n";
    char bufff[BUF_SIZE];
    int fd;
    struct stat stat_buf;
    if (stat(file_from, &stat_buf) == -1) {
        perror("stat failed");
        return;
    }

    if (S_ISDIR(stat_buf.st_mode)) {
        printf("Is directory!\n");
        char *buff="tar cvfz %s.tar.gz %s\n";
        sprintf(bufff, buff, file_from , file_from);
        printf("%s",bufff);
        system(bufff); 
        close(fd);
        strcat(file_from,".tar.gz");
        strcat(file_name,".tar.gz");
    }
    if ((fd = open(file_from, O_RDONLY)) == -1) {
        perror("open failed");
        strncpy(res, "500 Error file not found\r\n", sizeof(res));
        printf("< %s", res);
        send_message(dt->cmd_sock, res);
        return;
    }
    sprintf(bufff, "200 %s \n", file_name);
    send_message(dt->cmd_sock, bufff);

    off_t offset = 0;
    struct stat file_stat;
    fstat(fd, &file_stat);
    ssize_t size = file_stat.st_size;
    ssize_t sent = sendfile(dt->data_sock, fd, &offset, size);
    printf("sent = %zd Bytes\n", sent);
    close(fd);
    send_message(dt->cmd_sock, res);
}

int rmdir_recursive(const char* path)
{
    struct dirent* entry;
    DIR* dir = opendir(path);
    int result = 0;
    if (dir == NULL) {
        perror("Error opendir");
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char* full_path = malloc(strlen(path) + strlen(entry->d_name) + 2);
        sprintf(full_path, "%s/%s", path, entry->d_name);

        if (entry->d_type == DT_DIR) {
            if (rmdir_recursive(full_path) == -1) {
                result = -1;
            }
        } else {
            if (remove(full_path) == -1) {
                perror("Error removing file");
                result = -1;
            }
        }
        free(full_path);
    }

    if (rmdir(path) == -1) {
        perror("Error removing directory");
        result = -1;
    }

    closedir(dir);
    return result;
}

void recv_data(DataThread *dt,const char* to_file)
{
    char buffer[BUF_SIZE + 1];
    int out_fd = open(to_file, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP);
    if (out_fd == -1) {
        perror("Error opening file");
        strcpy(buffer, "500 error opening file \n");
        send_message(dt->cmd_sock, buffer);
        return;
    }

    ssize_t all = 0;
    int wrote = 0;
    int readed = 0;
    do {
        readed = recv(dt->data_sock, buffer, BUF_SIZE, 0);
        if (readed == -1) {
            perror("Error receiving data");
            strcpy(buffer, "500 Error in the server \n");
            send_message(dt->cmd_sock, buffer);
            break;
        }
        if (readed == 0) {
            strcpy(buffer, "226 Data transferred \n");
            send_message(dt->cmd_sock, buffer);
            break;
        }
        all += readed;
        buffer[readed] = '\0';
        wrote = write(out_fd, buffer, readed);
        if (wrote == -1) {
            perror("Error writing data to file");
            strcpy(buffer, "500 Error in the server \n");
            send_message(dt->cmd_sock, buffer);
            break;
        }
    } while (true);
    printf("received = %zd Bytes\n", all);
    close(out_fd);
    return;
}

void init_explorer(FileExplorer* fe, char* root_dir)
    {
        if(root_dir[strlen(root_dir)-1]== '/')
        {
            root_dir[strlen(root_dir)-1] = '\0';
        }
        strcpy(fe->root, root_dir);
        strcpy(fe->dir, "/");
    }

#pragma endregion

#pragma region sql

int callback(void *count_ptr, int argc, char **argv, char **azColName) 
{
    client *cl = (client*)count_ptr;
    cl->isAuth = 1;
    cl->perm = atoi(argv[4]);
    strcpy(cl->homedir, argv[3]);
    return 0;
}

void addDefaultUsers()
{

    char *sql = "INSERT INTO user (name, password, path, perm)" 
                    "VALUES ('anonymus','','anon/', 0),"
                    "('user', 'UserPassword', 'user/', 1),"
                    "('admin', 'PasswordAdmin', 'admin/', 2)";

    rc = sqlite3_exec(db, sql, NULL, 0, &zErrMsg);
    if (rc != SQLITE_OK) {
        printf("SQL error: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }

}

int auth_db(char *name, char* pass, client *cl)
{
    int count = 0; 
    char *sql;
    sql = malloc(BUF_SIZE * sizeof(char));
    snprintf(sql, BUF_SIZE, "SELECT * FROM user WHERE name='%s'AND password='%s';", name, pass);

    pthread_mutex_lock(&mutex);
    rc = sqlite3_exec(db, sql, callback, cl, &zErrMsg);
    if (rc != SQLITE_OK) {
        printf("SQL error: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }
    pthread_mutex_unlock(&mutex);

    if(cl->isAuth == 1)
        return 0;
    else 
        return -1;
}

#pragma endregion

#pragma region init

void handle_sigint(int sig)
{
    printf("\nShutting down server...\n");
    sqlite3_close(db);
    exit(EXIT_SUCCESS);
}

void initialize_ftp_server(int port)
{
    rc = sqlite3_open("users.db", &db);
    if (rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        exit(EXIT_FAILURE);
    }

    char *sql = "DROP TABLE IF EXISTS user;";
    rc = sqlite3_exec(db, sql, NULL, 0, &zErrMsg);
    if(rc != SQLITE_OK)
    {
        perror("Error with teble user: DROP\n");
        exit(EXIT_FAILURE);
    }

    sql = "CREATE TABLE IF NOT EXISTS user (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, password TEXT, path TEXT, perm INT);";
    rc = sqlite3_exec(db, sql, NULL, 0, &zErrMsg);
    if (rc != SQLITE_OK) 
    {
        perror("Can't create table: user\n");
        exit(EXIT_FAILURE);
    }

    addDefaultUsers();

    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].sock = -1;
        clients[i].data_sock = -1;
        clients[i].isActive = 0;
        clients[i].isAuth = 0;
        clients[i].perm = -1;
        DataThread dt;
        dt.type = 1;
        dt.data_sock = -1;
        dt.isActive = 0;
    }

    signal(SIGINT, handle_sigint);

    server = Socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in adr = {0};
    adr.sin_family = AF_INET;
    adr.sin_port = htons(port);

    Bind(server, (struct sockaddr *) &adr, sizeof adr);
    Listen(server, MAX_CLIENTS);

    printf("Ftp server is listening on port %d\n", port);
}

#pragma endregion

#pragma region DataThread

void start_pasv(client *cl, FileExplorer* fe, TransferType type)
{
    printf("Pasv started\n");

    char *err_response = "550 Error on the server\n";
    char addres[BUF_SIZE];

    if(cl->dt != NULL){
        if(cl->dt->isActive)
        {
            printf("Another data transfering is active \n");
            send_message(cl->sock, err_response);
            return;
        }
    }    

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        send_message(cl->sock, err_response);
        return;
    }

    struct sockaddr_in data_addr = {0};
    data_addr.sin_family = AF_INET;
    data_addr.sin_port = htons(0);
    data_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(sockfd, (struct sockaddr *)&data_addr, sizeof(data_addr)) < 0) {
        perror("bind");
        send_message(cl->sock, err_response);
        return;
    }

    if (listen(sockfd, 1) < 0) {
        perror("listen");
        send_message(cl->sock, err_response);
        return;
    }

    socklen_t len = sizeof(data_addr);
    if (getsockname(sockfd, (struct sockaddr *)&data_addr, &len) < 0) {
        perror("getsockname");
        send_message(cl->sock, err_response);
        return;
    }

    int ip[4];
    unsigned short int port = ntohs(data_addr.sin_port);
    char* host = inet_ntoa(data_addr.sin_addr);
    sscanf(host,"%d.%d.%d.%d",&ip[0],&ip[1],&ip[2],&ip[3]);

    char *addr = (char *) malloc(30 * sizeof(char));
    sprintf(addr, "%d,%d,%d,%d,%d,%d", ip[0], ip[1],
            ip[2], ip[3], (port >> 8) & 0xff, port & 0xff);

    sprintf(addres, "200 %s \n", addr);

    send_message(cl->sock, addres);

    int data_sock = accept(sockfd, (struct sockaddr *)&data_addr, &len);
    if (data_sock < 0) {
        perror("accept");
        send_message(cl->sock, err_response);
        return;
    }

    printf("Accept success \n");

    cl->dt = (DataThread*)malloc(sizeof(DataThread));
    cl->dt->cmd_sock = cl->sock;
    cl->data_sock = data_sock;
    cl->dt->data_sock = data_sock;
    cl->dt->fe = fe;
    cl->dt->type = type;
    cl->dt->isActive = 1;
    
    pthread_create(&cl->data_thread, NULL, handle_data_thread, cl->dt);

    char buf[BUF_SIZE];
    memset(buf, 0, sizeof(buf)); 
    sprintf(buf, "227 Entering Passive Mode (%s)\n", addr);

    send_message(cl->sock, buf);
}

char* start_active(char* address, client *cl, FileExplorer* fe, TransferType type)
{
    
    char* response = "550 Error on the server\n";
    if(cl->dt != NULL){
        if(cl->dt->isActive)
        {
            printf("Another data transfering is active \n");
            return response;
        }
    }

    unsigned int ip1, ip2, ip3, ip4, port1, port2;
    sscanf(address, "%u,%u,%u,%u,%u,%u", &ip1, &ip2, &ip3, &ip4, &port1, &port2);
    unsigned int port = port1 * 256 + port2;

    int data_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (data_sockfd < 0) {
    perror("socket");
    return response;
    }

    struct sockaddr_in data_servaddr = {0};
    data_servaddr.sin_family = AF_INET;
    data_servaddr.sin_port = htons(port);
    printf("%u \n", port);
    data_servaddr.sin_addr.s_addr = htonl((ip1 << 24) | (ip2 << 16) | (ip3 << 8) | ip4);
    if (connect(data_sockfd, (struct sockaddr *)&data_servaddr, sizeof(data_servaddr)) < 0) {
        perror("connect");
        return response;
    }

    socklen_t len = sizeof(data_servaddr);
    if (getsockname(data_sockfd, (struct sockaddr *)&data_servaddr, &len) < 0) {
        perror("getsockname");
        return response;
    }

    int port_sv = ntohs(data_servaddr.sin_port);

    cl->dt = (DataThread*)malloc(sizeof(DataThread));
    cl->dt->cmd_sock = cl->sock;
    cl->data_sock = data_sockfd;
    cl->dt->data_sock = data_sockfd;
    cl->dt->fe = fe;
    cl->dt->type = type;
    cl->dt->isActive = 1;
    
    pthread_create(&cl->data_thread, NULL, handle_data_thread, cl->dt);
 
    return "200 OK\n";
}

void* handle_data_thread(void* arg)
{
    DataThread* dt = (DataThread*) arg;
    char command[BUF_SIZE];
    char response[BUF_SIZE];
    char operation[BUF_SIZE/2];
    char argument[BUF_SIZE/2];

    pthread_mutex_init(&dt->mutex, NULL);
    pthread_cond_init(&dt->cond, NULL);

    memset(command, 0, sizeof(command));
    memset(response, 0, sizeof(response));
    memset(operation, 0, sizeof(response));
    memset(argument, 0, sizeof(response));


    pthread_mutex_lock(&dt->mutex);

    while (strlen(dt->command) == 0) {
        pthread_cond_wait(&dt->cond, &dt->mutex);
    }
    strcpy(command, dt->command);
    memset(dt->command, 0, sizeof(dt->command));

    pthread_mutex_unlock(&dt->mutex);

    parse_command(command, operation, argument);

    if(strcmp(operation, "LIST") == 0)
    {
        char buf[BUF_SIZE*2];
        if(strlen(argument)==0)
            strcpy(argument, ".");

        sprintf(buf, "%s%s%s", dt->fe->root, dt->fe->dir, argument);
        get_directory_listing(buf, response, 1, 1);
        send_message(dt->cmd_sock, response);
    }
    else if(strcmp(operation, "SEND") == 0)
    {
        char buf[BUF_SIZE*2];
        sprintf(buf, "%s%s%s", dt->fe->root, dt->fe->dir, argument);
        send_file(dt, buf, argument);
    }
    else if(strcmp(operation, "STOR") == 0)
    {   
        char buf[BUF_SIZE*2];
        sprintf(buf, "%s%s%s", dt->fe->root, dt->fe->dir, argument);
        recv_data(dt, buf);
    }

    dt->isActive = false;
    close(dt->data_sock);
    pthread_exit(NULL);
}

bool data_thread_cancel(client* cl)
{
    if(pthread_cancel(cl->data_thread) != 0)
    {
        fprintf(stderr, "pthread_cancel failed");
        return false;
    }
    cl->dt->isActive = false;
    close(cl->dt->data_sock);
    return true;
}

#pragma endregion

#pragma region FtpServer

void auth(client* cl)
{
    Request request;
    int sockfd = cl->sock;
    char operation[BUF_SIZE];
    char argument[BUF_SIZE];
    
    if (recv(sockfd, request.command, BUF_SIZE, 0) < 0) {
            fprintf(stderr, "Failed to receive command from client.\n");
            return;
        }

    parse_command(request.command, operation, argument);

    while(strcmp(operation, "USER") != 0)
    {
        if (strcmp(operation, "QUIT") == 0 || strlen(operation)==0) {
            return;
        }

        send_message(sockfd, "<130 Sign in first\n");

        memset(operation, 0, sizeof(operation));
        memset(argument, 0, sizeof(argument));

        if (recv(sockfd, request.command, BUF_SIZE, 0) < 0) {
            fprintf(stderr, "Failed to receive command from client.\n");
            return;
        }

        parse_command(request.command, operation, argument);
    }

    char name[BUF_SIZE];
    strcpy(name, argument);

    if (strcmp(name, "anonymus") != 0){
        strcpy(request.response, "331 Password required\n");
    }
    else
    {
        if(auth_db(name, "", cl)==0)
        {
            send_message(sockfd, "230 Log in success\n");
            return;
        }
        else
        {
            send_message(sockfd, "530 Authentication failed\n");
            return;
        }
    }

    send_message(sockfd, request.response);


    if(strcmp(name, "anonymus") != 0)
    {
        if (recv(sockfd, request.command, BUF_SIZE, 0) < 0) {
            fprintf(stderr, "Failed to receive command from client.\n");
            return;
        }
        memset(operation, 0, sizeof(operation));
        memset(argument, 0, sizeof(argument));

        parse_command(request.command, operation, argument);

        if(strcmp(operation, "PASS") != 0)
        {
            send_message(sockfd, "530 Authentication failed\n");
            return;
        }

        if(auth_db(name, argument, cl)==0)
        {
            send_message(sockfd, "230 Log in success\n");
            return;
        }
        else
        {
            send_message(sockfd, "530 Authentication failed\n");
            return;
        }
    }
}

void send_message(int client_sock, char *message)
{
    if (write(client_sock, message, strlen(message)) < 0) {
        perror("Error sending message");
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

void *handle_client_thread(void *arg)
{
    ThreadArgs* thread_args = (ThreadArgs*)arg;
    int client_index = thread_args->client_index;
    client* clients = thread_args->clients;
    char* root = thread_args->root;
    free(arg);

    TransferType type = 1;
    
    int sockfd = clients[client_index].sock;
    send_message(sockfd, "220 Welcom to the FTP from Maketfay\n");

    char command[BUF_SIZE];
    char response[BUF_SIZE];
    char operation[BUF_SIZE/2];
    char argument[BUF_SIZE/2];

    client *cl = &clients[client_index];
    auth(cl);
    
    if(cl->isAuth == 0)
    {
        printf("Close connections with unknown user\n");
        cl->isActive = false;
        close(sockfd);
        pthread_exit(NULL);
    }

    FileExplorer fe;
    sprintf(command, "%s%s", root, cl->homedir);
    init_explorer(&fe, command);

    while (1) {
        memset(command, 0, sizeof(command));
        memset(response, 0, sizeof(response));
        memset(operation, 0, sizeof(response));
        memset(argument, 0, sizeof(response));


        if (recv(sockfd, command, BUF_SIZE, 0) < 0) {
            fprintf(stderr, "Failed to receive command from client.\n");
            break;
        }
        
        parse_command(command, operation, argument);

        if (strcmp(operation, "QUIT") == 0) {
            pthread_mutex_lock(&mutex);

            clients[client_index].isActive = 0;
            send_message(sockfd, "221 Goodbye.\n");
            
            pthread_mutex_unlock(&mutex);
            break;
        }
        else if(strcmp(operation, "MKD") == 0)
        {
            if(make_dir(argument, &fe))
            {
                sprintf(response, "257 Directory %s was created \n", argument);
                send_message(sockfd, response);
            }
            else
            {
                sprintf(response, "530 Error creating dir \n");
                send_message(sockfd, response);
            }
        } 
        else if(strcmp(operation, "LIST") == 0&&cl->dt != NULL && cl->dt->isActive)
        {
            sprintf(command, "LIST %s\n", argument);
            strcpy(cl->dt->command, command); 
            pthread_cond_signal(&cl->dt->cond); 
        }   
        else if(strcmp(operation, "RMD") == 0)
        {
            char path[BUF_SIZE*2];
            sprintf(path, "%s%s%s", fe.root, fe.dir, argument);
            if(rmdir_recursive(path) == 0)
            {
                sprintf(response, "250 Directory removed \n");
            }
            else
            {
                sprintf(response, "530 Error removing folder \n");
            }
            send_message(sockfd, response);
        } 
        else if(strcmp(operation, "DELE") == 0)
        {
            if(rm(&fe, argument))
            {
                sprintf(response, "250 File %s deleted \n", argument);
            }
            else
            {
                sprintf(response, "530 Error deleting file \n");
            }
            send_message(sockfd, response);
        }
        else if(strcmp(operation, "CWD") == 0)
        {
            if (strncmp(argument, "..", 2) == 0)
            {
                char *res = cd_up(&fe);
                sprintf(response, "200 Changed directory to %s \n", res);

                send_message(sockfd, response);
            }
            else if(cd(argument, &fe))
            {
                sprintf(response, "200 Changed directory to %s \n", fe.dir);

                send_message(sockfd, response);
            }
            else 
            {
                sprintf(response, "400 Unknown directory %s \n", argument);

                send_message(sockfd, response);
            }
        }
        else if(strcmp(operation, "PWD") == 0)
        {
            sprintf(response, "257 \"%s\" is working directory\n", fe.dir);
            send_message(sockfd, response);
        }
        else if(strcmp(operation, "PASV") == 0)
        {
            start_pasv(cl, &fe, type);
        }
        else if(strcmp(operation, "TYPE") == 0)
        {
            if(strcmp(argument, "a") == 0)
            {
                type = ASCII;
                sprintf(response, "200 Type ASCII \n");
            }
            else
            {
                type = BIN;
                sprintf(response, "200 Type BIN \n");
            }
            send_message(sockfd, response);
        }
        else if(strcmp(operation, "PORT") == 0)
        {
            char* res = start_active(argument, cl, &fe, type);
            send_message(sockfd, res);
        }   
        else if(strcmp(operation, "RETR") == 0 && cl->dt != NULL && cl->dt->isActive)
        {
            sprintf(command, "SEND %s\n", argument);
            strcpy(cl->dt->command, command); 
            pthread_cond_signal(&cl->dt->cond);
        }
        else if(strcmp(operation, "STOR") == 0 && cl->dt != NULL && cl->dt->isActive)
        {
            sprintf(command, "STOR %s\n", argument);
            strcpy(cl->dt->command, command); 
            pthread_cond_signal(&cl->dt->cond);    
        }
        else if(strcmp(operation, "ABOR") == 0 && cl->dt != NULL && cl->dt->isActive)
        {
            if(data_thread_cancel(cl))
            {
                sprintf(response, "226 data transfer aborted \n");
            }
            else
            {
                sprintf(response, "500 data transfer abort failed \n");
            }
            send_message(sockfd, response);
        }  
        else if(strcmp(operation, "RNFR") == 0)
        {
            rename_from(cl, &fe, argument);
        }
        else if(strcmp(operation, "RNTO") == 0)
        {
            rename_to(cl, &fe, argument);
        }
        else if(strcmp(operation, "SIZE")==0)
        {
            uint64_t size = file_size(&fe, argument);
            if(size != -1)
            {
                sprintf(response, "230 Size = %lu \n", size);
                send_message(sockfd, response);
            }
            else
            {
                sprintf(response, "530 Error getting size of file %s \n", argument);
                send_message(sockfd, response);
            }
        }
        else if(strcmp(operation, "HELP")==0)
        {
            sprintf(response, "215 UNIX Type: L8. Remote system transfer type is UNIX.\n");
            send_message(sockfd, response);
        }
        else if(strcmp(operation, "NOOP")==0)
        {
            sprintf(response, "200 Command OK\n");
            send_message(sockfd, response);
        }
        else if(strlen(operation)==0)
        {
            send_message(sockfd, "404 Wrong command\n");
        }     
        else
        {
            send_message(sockfd, "404 Wrong command\n");
        }                 
    }
    close(sockfd);
    pthread_exit(NULL);
}

#pragma endregion

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    int port = atoi(argv[1]);
    if (port == 0) {
        fprintf(stderr, "Invalid port number.\n");
        exit(1);
    }

    initialize_ftp_server(port);

    pthread_t threads[MAX_CLIENTS];
    int thread_count = 0;

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_sock = Accept(server, (struct sockaddr *)&client_addr, &client_addr_len);

        int client_index = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].isActive) {
                client_index = i;
                break;
            }
        }

        if (client_index == -1) {
            fprintf(stderr, "Maximum number of clients reached.\n");
            close(client_sock);
            continue;
        }

        clients[client_index].sock = client_sock;
        clients[client_index].isActive = 1;

        ThreadArgs* args = (ThreadArgs*)malloc(sizeof(ThreadArgs));
        args->client_index = client_index;
        args->clients = clients;
        args->root = "./";

        printf("Accepted connection from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        if (thread_count < MAX_CLIENTS) {
            pthread_create(&threads[thread_count], NULL, handle_client_thread, args);
            thread_count++;
        } else {
            write(client_sock, "510 Server is busy, try again later.\n", strlen("510 Server is busy, try again later.\n"));
            close(client_sock);
        }
    }
    return 0;
}