#include <arpa/inet.h> //close
#include <stdio.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h> //strlen
#include <sys/socket.h>
#include <sys/time.h> //FD_SET, FD_ISSET, FD_ZERO, FD_SETSIZE macros
#include <sys/types.h>
#include <unistd.h> //close
#include "socket.c"
#include "parse.c"

#define MAXCLIENTS 30
#define HEADERLEN 10000
#define CONTENT 100000

int open_listen_socket(unsigned short port) {
    int listen_fd;
    int opt_value = 1;
    struct sockaddr_in server_address;

    listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const void*)&opt_value, sizeof(int));

    memset((char*)&server_address, 0, sizeof(server_address));

    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(port);
    bind(listen_fd, (struct sockaddr*)&server_address, sizeof(server_address));

    listen(listen_fd, MAXCLIENTS);

    return listen_fd;
}

int client(const char* hostname, int port)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    struct hostent* host = gethostbyname(hostname);
    memcpy(&(addr.sin_addr), host->h_addr, host->h_length);
    addr.sin_port = htons((unsigned short int)port);

    if (connect(sockfd, (sockaddr*)&addr, sizeof(addr)) == -1)
    {
        perror("Error connecting");
        exit(0);
    }

    // (4) Return
    printf("Client made successfully, socket: %d\n", sockfd);
    return sockfd;
}

void bitrate_reorder(double* throughputs, int tp_length) {
    int i, j;
    for (i = 0; i < tp_length; i ++ ) {
        for (j = i+1; j < tp_length; j++) {
            if (throughputs[i] > throughputs[j]) {
                double temp = throughputs[j];
                throughputs[j] = throughputs[i];
                throughputs[i] = temp;
            }
        }
    }
}

//choose a proper bitrate
double get_bitrate(double T_cur, double* throughputs, int len)
{
    int i = 0;
    for (; i < len; ++i)
    {
        if (T_cur < 1.5 * throughputs[i])
        {
            break;
        }
    }
    --i;
    if (i < 0) i = 0;
    return throughputs[i];
}

int readLine(char* from, int offset, int length, char* to) {
    int n;
    char* tmp = from + offset;
    for (n = 0; n < length; n++)
    {
        to[n] = tmp[n];
        if (tmp[n] == '\n' || tmp[n] == '\0')
        {
            break;
        }
    }
    return n;
}

int run_miProxy(unsigned short port, char* wwwip, double alpha, char* log) {
    int listen_socket, addrlen, activity, valread;
    int client_sockets[MAXCLIENTS] = { 0 };
    double throughputs[MAXCLIENTS] = { 0 };
    int tp_length = 0;
    int bitrate;
    int proxy_server_socket,client_sock, proxy_client_sock;
    //Open log file;
    
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    proxy_server_socket = open_listen_socket(port);

    char buffer[1025];

    addrlen = sizeof(address);
    puts("Waiting for connections ...");
    // set of socket descriptors
    fd_set readfds;

    double T_cur = 0.0001;
    double T_new = 0.0001;

    //listen
    while (1) {
        // clear the socket set
        FD_ZERO(&readfds);

        // add master socket to set
        FD_SET(listen_socket, &readfds);
        for (int i = 0; i < MAXCLIENTS; i++) {
            client_sock = client_sockets[i];
            if (client_sock != 0) {
                FD_SET(client_sock, &readfds);
            }
        }
        // wait for an activity on one of the sockets , timeout is NULL ,
        // so wait indefinitely
        activity = select(FD_SETSIZE, &readfds, NULL, NULL, NULL);


        // If something happened on the master socket ,
        // then its an incoming connection, call accept()
        if (FD_ISSET(listen_socket, &readfds)) {
            int new_socket = accept(listen_socket, (struct sockaddr*)&address, (socklen_t*)&addrlen);

            // inform user of socket number - used in send and receive commands
            printf("\n---New host connection---\n");
            printf("socket fd is %d , ip is : %s , port : %d \n", new_socket, inet_ntoa(address.sin_addr), ntohs(address.sin_port));

            // add new socket to the array of sockets
            for (int i = 0; i < MAXCLIENTS; i++) {
                // if position is empty
                if (client_sockets[i] == 0) {
                    client_sockets[i] = new_socket;
                    break;
                }
            }
        }
        // else it's some IO operation on a client socket
        for (int i = 0; i < MAXCLIENTS; i++) {
            client_sock = client_sockets[i];
            // Note: sd == 0 is our default here by fd 0 is actually stdin
            if (client_sock != 0 && FD_ISSET(client_sock, &readfds)) {
                // Check if it was for closing , and also read the incoming message
                getpeername(client_sock, (struct sockaddr*)&address, (socklen_t*)&addrlen);
                valread = read(client_sock, buffer, 1024);
                if (valread == 0) {
                    // Somebody disconnected , get their details and print
                    printf("\n---Host disconnected---\n");
                    printf("Host disconnected , ip %s , port %d \n", inet_ntoa(address.sin_addr), ntohs(address.sin_port));
                    // Close the socket and mark as 0 in list for reuse
                    close(client_sock);
                    client_sockets[i] = 0;
                }
                else {
                    // Parse http request

                    proxy_client_sock = client(wwwip, port);

                    int nbytes = 0;
                    char buf[CONTENT];
                    
                    //Revised request(both nolist f4m and video chunk)
                    char request[HEADERLEN];  memset(request, 0, HEADERLEN * sizeof(char));
                    //Request Line buffer
                    char line[HEADERLEN];  memset(line, 0, HEADERLEN * sizeof(char));
                    int offset = 0;
                    // Method / URI / Version
                    char method[HEADERLEN];   memset(method, 0, HEADERLEN * sizeof(char));
                    char url[HEADERLEN];   memset(url, 0, HEADERLEN * sizeof(char));
                    char version[HEADERLEN];   memset(version, 0, HEADERLEN * sizeof(char));

                    //store rest of header;
                    char rest[HEADERLEN];   memset(rest, 0, HEADERLEN * sizeof(char));

                    char path[1000];   memset(path, 0, 1000 * sizeof(char));
                    char chunk[1000];   memset(chunk, 0, 1000 * sizeof(char));

                    //receive request
                    nbytes = (int)recv(client_sock, buf, CONTENT, MSG_NOSIGNAL);

                    //delete all IF if there is something wrong here.
                    if (nbytes < 1) { 
                        perror("Error in receving");
                        close(client_sock);
                        client_sockets[i] = 0;
                        close(proxy_client_sock);
                        continue; 
                    }

                    //parse first line

                    nbytes = readLine(buf, offset, line, sizeof(buf));
                    offset = offset + nbytes + 1;
                    sscanf(line, "%s %s %s\r\n", method, url, version);

                    //store rest of them
                    while (nbytes)
                    {
                        memset(line, 0, HEADERLEN * sizeof(char));
                        nbytes = readLine(buf, offset , line, sizeof(buf));
                        strcat(rest, line);

                        offset += nbytes + 1;
                    }
                    nbytes = readLine(buf, offset, line, sizeof(buf));
                    strcat(rest, line);
                    
                    printf("Finish parse header: \nmethod: %s, url: %s, version: %s\n %s", method, url, version,rest);
                    
                    //IF it is a f4m request
                    if (strstr(url, ".f4m")){
                        char* newTail = "_nolist.f4m";
                        char* newUrl = strcat(strtok(url, ".f4m"), newTail);
                        sprintf(request, "%s %s %s", method, newUrl, version);
                        strcat(request, rest);

                        //send revised request first
                        nbytes = (int)send(proxy_client_sock, request, sizeof(request), 0);
                        char bb[CONTENT];
                        memset(bb, 0, CONTENT * sizeof(char));

                        //get reponse forr nolist.f4m
                        nbytes = (int)recv(proxy_client_sock, bb, HEADERLEN * sizeof(char), 0);
                        if (nbytes == -1)
                        {
                            // perror("Error receiving response");
                            close(proxy_client_sock);
                            break;
                        }
                        int read = nbytes;
                        int remain;
                        int content_length = 0;

                        // Parse content length
                        offset = 0;
                        // printf("Parse content length\n");
                        while (strcmp(line, "\r\n"))
                        {
                            memset(line, 0, HEADERLEN * sizeof(char));
                            nbytes = readLine(bb, offset,sizeof(line), line);
                            char* cl = strstr(line, "Content-Length: ");
                            if (cl)
                                sscanf(cl, "Content-Length: %d", &content_length);
                            offset += nbytes + 1;
                        }
                        remain = content_length - (read - offset);
                        char* bb_ptr = bb + read;
                        while (remain > 0) {
                            nbytes = (int)recv(proxy_client_sock, bb_ptr, remain, 0);
                            if (nbytes == -1)
                            {
                                perror("Error receiving response");
                                close(proxy_client_sock);
                                break;
                            }
                            remain -= nbytes;
                            bb_ptr += nbytes;
                        }
                        //send back to client
                        nbytes = (int)send(client_sock, bb, remain + read, 0);
                        //send original .f4m request
                        nbytes = (int)send(proxy_client_sock, buf, sizeof(buffer), 0);

                    }
                    //IF it is a video request
                    else if (strstr(url, "Seg") && strstr(url, "Frag")) {

                        //get a bitrate, not done yet
                        bitrate = get_bitrate(T_cur,throughputs,tp_length);

                        //pathBitrateChunk of url
                        sscanf(url, "%[^0-9]%*d%s", path, chunk);
                        sprintf(request, "%s %s%d%s %s", method, path, bitrate, chunk, version);
                        strcat(request, rest);
                        //send revised request
                        nbytes = (int)send(proxy_client_sock, request, sizeof(request), 0);
                    }
                    else {
                        strcpy(request, buf);
                        //send revised request
                        nbytes = (int)send(proxy_client_sock, request, sizeof(request), 0);
                    }

                    //Recv
                    memset(buf, 0, CONTENT * sizeof(char));
                    time_t start;
                    time_t end;
                    time(&start);

                    nbytes = (int)recv(proxy_client_sock, buf, HEADERLEN * sizeof(char), 0);
                    
                    if (nbytes == -1)
                    {
                        // perror("Error receiving response");
                        close(proxy_client_sock);
                        break;
                    }
                    int read = nbytes;
                    int remain;
                    int content_length = 0;

                    // Parse content length
                    offset = 0;
                    // printf("Parse content length\n");
                    while (strcmp(line, "\r\n"))
                    {
                        memset(line, 0, HEADERLEN * sizeof(char));
                        nbytes = readLine(buf, offset, sizeof(line), line);
                        char* cl = strstr(line, "Content-Length: ");
                        if (cl)
                            sscanf(cl, "Content-Length: %d", &content_length);
                        offset += nbytes + 1;
                    }
                    remain = content_length - (read - offset);
                    char* buf_ptr = buf + read;
                    int total = remain + read;
                    while (remain > 0) {
                        nbytes = (int)recv(proxy_client_sock, buf_ptr, remain, 0);
                        if (nbytes == -1)
                        {
                            perror("Error receiving response");
                            close(proxy_client_sock);
                            break;
                        }
                        remain -= nbytes;
                        buf_ptr += nbytes;
                    }
                    // XML, get bitrate
                    if (strstr(url, ".f4m")) {
                        offset = 0;
                        while (!strstr(line, "</manifest>"))
                        {
                            memset(line, 0, HEADERLEN * sizeof(char));
                            nbytes = readLine(buf, offset, sizeof(line), line);

                            offset += nbytes + 1;
                            // Find bitrate address
                            char* br_addr = strstr(line, "bitrate");
                            char* brate[30];
                            if (br_addr)
                            {
                                sscanf(br_addr, "bitrate=\"%d\"", brate);
                                // printf("Add bitrate: %d\n", br_list[br_len]);
                                char err[30];
                                throughputs[tp_length] = strtod(brate,err);
                                tp_length++;
                            }
                        }
                        bitrate_reorder(throughputs, tp_length);

                        T_cur = throughputs[0];
                    }
                    // Video Chunk
                    else if (strstr(url, "Seg") && strstr(url, "Frag")) {
                        time(&end);
                        double period = (double)end - start;

                        T_new = ((total * 8) / 1000) / period;
                        T_cur = (1 - alpha) * T_cur + alpha * T_new;

                        FILE* logFile;
                        logFile = fopen(log, "w");

                        fprintf(logFile, "%s %s %s %.3f %.3f %.3f %d\n", inet_ntoa(address.sin_addr),chunk, wwwip, period, T_new, T_cur, bitrate);

                        fclose(logFile);

                        nbytes = (int)send(client_sock, buf, sizeof(buf), 0);
                        if (nbytes == -1)
                        {
                            perror("Error sending response");
                            close(proxy_client_sock);
                            continue;
                        }
                    }
                    //Else, do nothing
                    else {
                        nbytes = (int)send(client_sock, buf, sizeof(buf), 0);
                        if (nbytes == -1)
                        {
                            perror("Error sending response");
                            close(proxy_client_sock);
                            continue;
                        }
                    }
                }
            }
        }
        //not finished yet
    }
    return 0;
}


int main(int argc, const char **argv){
    if (argc == 6 && strcmp(argv[1],"-nodns")==0) {
        unsigned short port = (unsigned short)atoi(argv[2]);
        char* wwwip = argv[3];
        char* errptr;
        double alpha = strtod(argv[4], errptr);
        char* log = argv[5];

        run_miProxy(port, wwwip, alpha, log);

        return 0;

       }
    else {

        printf("Error! Please Run with proper argument");

        return -1;
    }
}